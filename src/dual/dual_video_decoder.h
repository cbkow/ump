// DualVideoDecoder — Phase 7.7 Stage 1.
//
// FFmpeg video source with a 16-frame ring buffer and a NeedsMoreFrames
// decode-ahead gate. Mirrors old QCView's metal_video_decoder.{h,mm}
// pattern (kFrameBufferSize=16, decode CV waits on demand-pull, no
// wall-clock sleep in the decoder). Pacing is DualPlaybackTimer's job.
//
// Why we don't use the existing qcv::VideoDecoder:
//   - Single-flow VideoDecoder paces every publish against wall clock
//     in publishHandle(), which makes decode-ahead structurally
//     impossible. Removing that and adding a ring buffer would touch
//     half the file. Per Phase 7.7 design, dual lives parallel to
//     single — code duplication accepted, isolation is the goal.
//   - Single-flow VideoDecoder is built around a single publish slot
//     + LRU history. Ring-buffer semantics are different enough that
//     a clean rewrite is simpler than a retrofit.
//
// Current scope:
//   - CPU / sws_scale RGBA8 path only. No HW accel, no pool, no MTL.
//   - Standalone verification: open, set decode_target, observe ring
//     fill 8 frames ahead before idling.
//
// KNOWN GAPS (tracked as real tasks in the project task list):
//   - "DualVideoDecoder — VideoToolbox HW decode": attach VT/D3D11VA/
//     VAAPI hwaccel so high-res sources don't saturate CPU cores in
//     dual mode (6K is currently unplayable).
//   - "DualPool — zero-copy MTLTexture handles": replace the
//     shared_ptr<QImage> ring with a frame-keyed MTLTexture pool;
//     pairs with the HW-decode work for a CVPixelBuffer→MTLTexture
//     bridge. The IDualSource API will need a return-shape change
//     (handle vs frame) at that point — explicit known seam.

#pragma once

#include "i_dual_source.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <unordered_map>

extern "C" {
#include <libavutil/rational.h>
}

struct AVFormatContext;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct AVBufferRef;
struct SwsContext;

namespace qcv::dual {

class DualVideoDecoder : public IDualSource {
public:
    DualVideoDecoder();
    ~DualVideoDecoder() override;

    DualVideoDecoder(const DualVideoDecoder &)            = delete;
    DualVideoDecoder &operator=(const DualVideoDecoder &) = delete;

    // ---- IDualSource ----
    bool open(const QString &path) override;
    void close() override;
    bool isOpen() const override { return m_open.load(std::memory_order_acquire); }

    void setDecodeTarget(int frameNumber) override;
    void seekTo(int frameNumber) override;

    std::shared_ptr<DualFrame> getBufferedFrame(int frameNumber) const override;
    bool hasFrame(int frameNumber) const override;

    int  bufferedAhead() const override;
    int  bufferedBehind() const override;
    int  bufferCount() const override;
    void getBufferedRange(int &startFrame, int &endFrame) const override;

    int    width()  const override { return m_width; }
    int    height() const override { return m_height; }
    double fps()    const override { return m_fps; }
    int    frameCount() const override { return m_frameCount; }
    QString path() const override { return m_path; }
    void setRangeOverride(int v) override;

    QString hwAccelName() const override {
        // m_hwAttached is set only after a successful HW probe +
        // FFmpeg hwaccel attach in initFFmpeg — VideoToolbox on macOS,
        // Vulkan on Windows, VAAPI on Linux. Anything else falls
        // through to sws_scale (empty string = software decode).
        if (!m_hwAttached) return QString();
#if defined(Q_OS_MACOS)
        return QStringLiteral("videotoolbox");
#elif defined(Q_OS_LINUX)
        return QStringLiteral("vaapi");
#else
        return QStringLiteral("vulkan");
#endif
    }

    // Ring depth (compile-time constant from old QCView pattern).
    static constexpr int kRingSize = 16;

private:
    struct BufferedFrame {
        int   frameNumber = -1;
        bool  valid       = false;
        std::shared_ptr<DualFrame> frame;
        void  reset() { frameNumber = -1; valid = false; frame.reset(); }
    };

    // FFmpeg lifecycle
    bool initFFmpeg(const QString &path);
    void teardownFFmpeg();
    bool initSwsContext(AVFrame *frame);

    // Decode thread loop
    void decodeThreadFunc();
    bool needsMoreFrames() const;            // decode-ahead gate
    bool decodeOneFrame(AVFrame *frame, AVPacket *packet);
    void addCurrentFrameToBuffer(AVFrame *frame, int frameNumber);
    std::shared_ptr<DualFrame> convertFrameToRgba(AVFrame *frame, int frameNumber);

    // PTS ↔ frame conversion (formula based; old app's intra path)
    int     frameNumberForPts(int64_t pts) const;
    int64_t ptsForFrameNumber(int frameNumber) const;

    // Seek handling — called from decode thread when m_seekPending is set
    void performSeek(int targetFrame, AVPacket *pkt, AVFrame *frame);

    // ---- Metadata (set in open(), const after) ----
    QString m_path;
    int     m_width      = 0;
    int     m_height     = 0;
    double  m_fps        = 0.0;
    int     m_frameCount = 0;

    // ---- FFmpeg state (decode thread only) ----
    AVFormatContext *m_fmt        = nullptr;
    AVCodecContext  *m_cctx       = nullptr;
    SwsContext      *m_sws        = nullptr;
    AVBufferRef     *m_hwDeviceCtx = nullptr;   // VT/D3D11/VAAPI; null on SW
    AVFrame         *m_swFrame     = nullptr;   // landing pad for HW→SW transfer
    bool             m_hwAttached  = false;     // true if hwaccel attached + active
    int              m_streamIdx  = -1;
    AVRational       m_streamTimeBase{0, 1};
    AVRational       m_streamFrameRate{0, 1};
    int64_t          m_streamStartPts = 0;
    int              m_swsSrcW   = 0;
    int              m_swsSrcH   = 0;
    int              m_swsSrcFmt = -1;

    // ---- Ring buffer ----
    mutable std::mutex                   m_bufferMutex;
    std::array<BufferedFrame, kRingSize> m_ring;
    std::unordered_map<int, int>         m_frameMap;   // frameNumber → ring index
    int m_ringHead  = 0;                                // index of oldest entry
    int m_ringCount = 0;                                // number of valid entries

    // ---- Decode-ahead state ----
    std::atomic<int>  m_decodeTarget{0};
    std::atomic<int>  m_pendingSeekTarget{-1};

    // Per-clip videoRangeOverride. Pushed by DualPlaybackController
    // when its own setRangeOverrideA/B fires (which fires from
    // ProjectManager::videoRangeOverrideChanged via WindowManager).
    // Read on the decode thread inside convertFrameToRgba: the CPU
    // path applies it to sws_setColorspaceDetails per-frame; the
    // Metal path stashes it into DualFrame.rangeOverride so the
    // compositor's YUV→RGB pass honors it.
    std::atomic<int>  m_rangeOverride{0};

    // ---- Threading ----
    std::atomic<bool>       m_open{false};
    std::atomic<bool>       m_stopRequested{false};
    std::condition_variable m_decodeCv;
    mutable std::mutex      m_decodeCvMutex;
    std::thread             m_thread;
};

} // namespace qcv::dual
