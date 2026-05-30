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
#include <functional>
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
        // Reports the backend that actually attached, not the one we
        // tried first. On Windows the dual decoder probes Vulkan first
        // and falls back to D3D11VA — m_hwBackend distinguishes them.
        // Empty string = software decode.
        if (!m_hwAttached) return QString();
        return m_hwBackend;
    }

    // Per-instance "force software decode for ProRes" flag. Set by
    // DualPlaybackController for B in dual mode to sidestep the
    // ProRes-over-Vulkan instability on NVIDIA. Harmless when B isn't
    // ProRes — the gate inside initFFmpeg is (kIsProRes && this flag),
    // so non-ProRes B still tries HW normally.
    void setForceSoftwareDecodeForProRes(bool v) { m_forceSwForProRes = v; }

    // Wake-on-frame hook (see IDualSource). Fired on the decode
    // thread once a freshly decoded frame lands in the ring buffer,
    // so the render-on-demand loop redraws after a seek-while-paused.
    void setFrameAvailableCallback(FrameAvailableCallback cb) override;

    // PTS ↔ frame conversion (formula based; old app's intra path).
    // Public so the paired DualScrubDecoder can map between the
    // FFmpeg PTS space and source-frame numbers without duplicating
    // the timebase math. Safe to call from any thread: the underlying
    // members (m_streamTimeBase, m_streamFrameRate, m_streamStartPts,
    // m_fps) are set in initFFmpeg BEFORE the decode thread spawns
    // and never mutate afterwards.
    int     frameNumberForPts(int64_t pts) const;
    int64_t ptsForFrameNumber(int frameNumber) const;

    // Scrub coordination — set by DualPlaybackController during a
    // scrub gesture. When true, the decode loop's main CV wait
    // predicate gates `needsMoreFrames()` so the streaming decode
    // thread parks. Codec contexts stay hot for warm-seek on
    // scrub release. Stop and pending-seek paths still wake.
    void setScrubActive(bool v) {
        m_scrubActive.store(v, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lk(m_decodeCvMutex);
        }
        m_decodeCv.notify_one();
    }

    // Per-side scrub publish slot — written by DualScrubDecoder, read
    // by DualPlaybackController::pullFrameA/B while scrubActive.
    // Independent of the ring buffer (the ring is the streaming-
    // cursor cache; scrub frames are NOT cursor predictions and would
    // poison the ring). Mutex separate from m_bufferMutex so the
    // ring and the scrub slot don't share a lock.
    void publishExternalFrame(int frameNumber,
                                std::shared_ptr<DualFrame> frame);
    std::shared_ptr<DualFrame> getScrubFrame() const;
    void clearScrubFrame();

#if defined(Q_OS_MACOS)
    // Zero-copy VideoToolbox → DualFrame::Kind::Metal for the macOS scrub
    // decoder: retains the CVPixelBuffer off a VT-decoded AVFrame and
    // stamps the current rangeOverride. Returns nullptr if `frame` isn't
    // a zero-copy-able VideoToolbox surface, so the caller falls back to
    // CPU readback. Mirrors the streaming path in convertFrameToRgba.
    std::shared_ptr<DualFrame> makeMetalScrubFrame(AVFrame *frame,
                                                   int frameNumber);
#endif

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
    // Deadlock backstop: true when the decode target is NOT buffered and
    // the entire ring sits AHEAD of it (bufMin > target) — the decoder
    // can't reach an earlier frame by decoding forward, and
    // needsMoreFrames() is satisfied by the ahead-window, so without a
    // forced backward seek the target frame is never produced (e.g. a
    // warm-up read-ahead that evicted frame 0 → permanent getBufferedFrame
    // miss → "readahead warm-up" spinner). The decode loop queues a seek
    // to the target when this holds.
    bool targetStrandedAhead() const;
    bool decodeOneFrame(AVFrame *frame, AVPacket *packet);
    void addCurrentFrameToBuffer(AVFrame *frame, int frameNumber);
    std::shared_ptr<DualFrame> convertFrameToRgba(AVFrame *frame, int frameNumber);

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
    QString          m_hwBackend;               // "vulkan" | "d3d11va" | "vaapi" | "videotoolbox"; empty on SW
    bool             m_forceSwForProRes = false; // per-instance: gate hwaccel for ProRes (B-side dual)
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

    // ---- Scrub coordination ----
    // m_scrubActive: when true, decode thread's primary CV wait
    // predicate skips needsMoreFrames(). The flag is set by
    // DualPlaybackController on beginScrub / endScrub. m_scrubFrame
    // is the per-side scrub publish slot (independent of m_ring);
    // DualScrubDecoder writes it via publishExternalFrame, the
    // controller reads it via getScrubFrame.
    std::atomic<bool>                     m_scrubActive{false};
    mutable std::mutex                    m_scrubMutex;
    std::shared_ptr<DualFrame>            m_scrubFrame;

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

    // ---- Wake-on-frame callback ----
    // Installed by WindowManager; invoked on the decode thread after
    // a frame lands in the ring buffer. Guarded by its own mutex so
    // install (GUI thread) doesn't race the decode-thread invoke.
    mutable std::mutex      m_callbackMutex;
    FrameAvailableCallback  m_onFrameAvailable;
};

} // namespace qcv::dual
