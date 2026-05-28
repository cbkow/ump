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
//   - Intra-only hwaccel gate per platform: D3D11VA on Windows
//     (subject to performance/hardwareDecodeEnabled), VAAPI on Linux.
//     macOS forces SOFTWARE — scrub reads every frame back to CPU
//     anyway, so VideoToolbox buys it nothing, and a per-side scrub VT
//     session would contend with the two streaming DualVideoDecoder VT
//     sessions and stall the B-side scrub at startup. The Cpu-only
//     publish also keeps scrub frames out of the dual Vulkan
//     compositor's compute pipeline (sidesteps the NVIDIA device-lost
//     crash on Win/Linux).
//
// Lifecycle is owned by DualPlaybackController: scrub decoders open
// alongside their paired DualVideoDecoder and close BEFORE the
// streaming source is destroyed (the worker holds a raw pointer to
// the streaming decoder for its pts façade calls).

#pragma once

#include "i_dual_source.h"   // DualFrame

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

class DualScrubDecoder : public QObject
{
    Q_OBJECT
public:
    explicit DualScrubDecoder(DualVideoDecoder *streaming,
                               QObject *parent = nullptr);
    ~DualScrubDecoder() override;

    // Open / close are sequenced by DualPlaybackController against
    // the paired DualVideoDecoder. The streaming decoder MUST be
    // open before this scrub decoder opens (we read its pts façades).
    bool open(const QString &path);
    void close();

    // QML-callable indirectly via the controller. Latest target wins;
    // older requests are abandoned mid-decode loop on the next
    // worker iteration. Cheap to call at slider-drag rate (60+ Hz).
    Q_INVOKABLE void requestFrame(int frameNo);

private:
    void workerLoop();
    bool initFFmpeg(const QString &path);
    void teardownFFmpeg();
    bool initSwsContext(AVFrame *frame);
    bool decodeAndPublish(int target, AVPacket *pkt,
                          AVFrame *frame, AVFrame *swFrame);

    DualVideoDecoder *m_streaming = nullptr;   // not owned

    AVFormatContext *m_fmt = nullptr;
    AVCodecContext  *m_cctx = nullptr;
    AVBufferRef     *m_hwDeviceCtx = nullptr;
    int              m_videoStreamIdx = -1;

    SwsContext      *m_sws = nullptr;
    int              m_swsSrcWidth = 0;
    int              m_swsSrcHeight = 0;
    int              m_swsSrcFormat = -1;

    std::atomic<int> m_pendingTarget{-1};
    int              m_lastDecodedFrame = -1;

    std::thread             m_thread;
    std::atomic<bool>       m_stopRequested{false};
    std::mutex              m_condMutex;
    std::condition_variable m_cond;
};

} // namespace qcv::dual
