// DualScrubDecoder — port of qcv::ScrubDecoder for the dual-view
// island. Same shape as src/decode/scrub_decoder.h:
//
//   - separate FFmpeg context from the paired DualVideoDecoder,
//   - latest-target-wins worker (older requests abandoned),
//   - last-frame cache for repeat queries at the same frame,
//   - lazy lifetime: open() in lockstep with DualVideoDecoder::open.
//
// Deltas from single-flow ScrubDecoder:
//   - Takes a DualVideoDecoder* (not VideoDecoder*) for PTS↔frame
//     mapping via the public ptsForFrameNumber/frameNumberForPts
//     façades (DualVideoDecoder has no FrameIndex member).
//   - Always publishes DualFrame::Kind::Cpu (QImage RGBA8 via
//     av_hwframe_transfer_data + sws_scale). Single-flow's macOS
//     zero-copy Metal branch is intentionally deferred — keeps the
//     dual compositor's per-slot CvPixbufMetalBridge out of the
//     scrub path for Phase 1.
//   - Keeps the SAME platform-conditional intra-only hwaccel gate
//     (VT on macOS, D3D11VA on Windows, VAAPI on Linux). HW-decoded
//     frames are still readback to CPU; the Cpu-only publish means
//     scrub frames never enter the dual Vulkan compositor's compute
//     pipeline (sidesteps the NVIDIA device-lost crash on Win/Linux).
//
// Lifecycle is owned by DualPlaybackController: scrub decoders open
// alongside their paired DualVideoDecoder and close BEFORE the
// streaming source is destroyed (the worker holds a raw pointer to
// the streaming decoder for its pts façade calls).

#pragma once

#include "i_dual_source.h"          // DualFrame
#include "i_dual_scrub_decoder.h"   // IDualScrubDecoder
#include "dual_scrub_cache.h"       // DualScrubEntry
#include "decode/simple_lru.h"      // SimpleLRU

#include <QObject>
#include <QString>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

struct AVBufferRef;
struct AVCodecContext;
struct AVFormatContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;

namespace qcv::dual {

class DualVideoDecoder;

class DualScrubDecoder : public QObject, public IDualScrubDecoder
{
    Q_OBJECT
public:
    explicit DualScrubDecoder(DualVideoDecoder *streaming,
                               QObject *parent = nullptr);
    ~DualScrubDecoder() override;

    // Open / close are sequenced by DualPlaybackController against
    // the paired DualVideoDecoder. The streaming decoder MUST be
    // open before this scrub decoder opens (we read its pts façades).
    bool open(const QString &path) override;
    void close() override;

    // QML-callable indirectly via the controller. Latest target wins;
    // older requests are abandoned mid-decode loop on the next
    // worker iteration. Cheap to call at slider-drag rate (60+ Hz).
    Q_INVOKABLE void requestFrame(int frameNo) override;

private:
    void workerLoop();
    bool initFFmpeg(const QString &path);
    void teardownFFmpeg();
    bool initSwsContext(AVFrame *frame);
    bool decodeAndPublish(int target, AVPacket *pkt,
                          AVFrame *frame, AVFrame *swFrame);
    bool decodeForwardCaching(int target, int64_t targetPts, AVPacket *pkt,
                              AVFrame *frame, AVFrame *swFrame);
    std::shared_ptr<DualScrubEntry> makeEntry(AVFrame *frame, AVFrame *swFrame,
                                              int frameNumber);
    void publishEntry(const std::shared_ptr<DualScrubEntry> &entry);

    DualVideoDecoder *m_streaming = nullptr;   // not owned

    AVFormatContext *m_fmt = nullptr;
    AVCodecContext  *m_cctx = nullptr;
    AVBufferRef     *m_hwDeviceCtx = nullptr;
    int              m_videoStreamIdx = -1;

    SwsContext      *m_sws = nullptr;
    int              m_swsSrcWidth = 0;
    int              m_swsSrcHeight = 0;
    int              m_swsSrcFormat = -1;
    int              m_swsAppliedRange = -1;  // last range override baked into m_sws

    std::atomic<int> m_pendingTarget{-1};

    // Decoded-GOP cache (mirrors single-flow ScrubDecoder). Worker-thread
    // only; keyed by frame number, byte-bounded.
    SimpleLRU<int, std::shared_ptr<DualScrubEntry>> m_gopCache;
    int  m_lastShown = -1;            // published frame (exact-repeat skip)
    int  m_decoderPos = -1;           // last frame the decoder produced
    bool m_decoderPositioned = false; // can continue forward without re-seek
    int  m_cachedRangeOv = -1;        // range override the cache was filled at

    // Windows + intra-only codec → legacy direct-seek scrub (no GOP cache, no
    // forward-fill). Mirrors single-flow ScrubDecoder::m_intraDirectScrub:
    // intra (ProRes/DNxHD/MJPEG) is software-decoded + random-access on
    // Windows, so the GOP-cache rework only adds per-step decode cost there.
    // false on macOS/Linux and for inter codecs.
    bool m_intraDirectScrub = false;

    std::thread             m_thread;
    std::atomic<bool>       m_stopRequested{false};
    std::mutex              m_condMutex;
    std::condition_variable m_cond;
};

} // namespace qcv::dual
