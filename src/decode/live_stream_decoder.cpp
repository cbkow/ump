#include "live_stream_decoder.h"
#include "video_decoder.h"

#include <QImage>
#include <QSettings>
#include <QtLogging>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <chrono>

// CoreVideo raw helpers (defined in frame_handle.mm on Apple, no-op
// stubs in frame_handle_nonapple.cpp) — same pattern as VideoDecoder.
extern "C" void cvPixelBufferRetainRaw(void *cvPix);

namespace qcv {

namespace {

// Reconnect backoff: 250 ms doubling to a 3 s cap. Reset after any
// session that actually delivered frames, so a stable stream that
// blips once reconnects fast.
constexpr int kBackoffStartMs = 250;
constexpr int kBackoffCapMs   = 3000;

// Live-edge detection (work order 4c). SRT's TSBPD paces delivery at
// the receiver, so at the live edge av_read_frame blocks until the
// next packet's scheduled time (~one frame interval). A read that
// returns near-instantly is therefore serving backlog, not liveness.
constexpr int kLiveEdgeBlockMs   = 4;    // read ≥ this ⇒ at the edge
constexpr int kBurstFastReads    = 3;    // consecutive instant reads ⇒ draining
constexpr int kDrainKeepaliveMs  = 250;  // publish floor while draining

#if defined(Q_OS_MACOS)
AVPixelFormat liveGetFormat(AVCodecContext *, const AVPixelFormat *fmts)
{
    for (int i = 0; fmts[i] != AV_PIX_FMT_NONE; ++i) {
        if (fmts[i] == AV_PIX_FMT_VIDEOTOOLBOX) return fmts[i];
    }
    return fmts[0];
}
#elif defined(Q_OS_WIN)
AVPixelFormat liveGetFormat(AVCodecContext *, const AVPixelFormat *fmts)
{
    for (int i = 0; fmts[i] != AV_PIX_FMT_NONE; ++i) {
        if (fmts[i] == AV_PIX_FMT_D3D11) return fmts[i];
    }
    return fmts[0];
}
#endif

} // namespace

LiveStreamDecoder::LiveStreamDecoder(QObject *parent)
    : QObject(parent)
{
}

LiveStreamDecoder::~LiveStreamDecoder()
{
    close();
}

void LiveStreamDecoder::setFrameCallback(std::function<void()> cb)
{
    std::lock_guard<std::mutex> lk(m_frameCbMutex);
    m_frameCb = std::move(cb);
}

bool LiveStreamDecoder::open(const QString &url)
{
    if (m_thread.joinable()) {
        qWarning("LiveStreamDecoder: open('%s') while already open — "
                 "closing previous session first", qPrintable(url));
        close();
    }
    if (!m_sink) {
        qWarning("LiveStreamDecoder: no sink VideoDecoder set — refusing open");
        return false;
    }
    m_url = url;
    m_stopRequested.store(false, std::memory_order_release);
    m_reconnects.store(0, std::memory_order_release);
    m_framesReceived.store(0, std::memory_order_release);
    m_framesConflated.store(0, std::memory_order_release);
    setStatus(Connecting);
    m_thread = std::thread([this] { workerLoop(); });
    return true;
}

void LiveStreamDecoder::close()
{
    if (!m_thread.joinable()) return;
    m_stopRequested.store(true, std::memory_order_release);
    {
        // Wake a backoff sleep immediately; the AVIO interrupt
        // callback bounds every blocking FFmpeg call.
        std::lock_guard<std::mutex> lk(m_sleepMutex);
    }
    m_sleepCv.notify_all();
    m_thread.join();
    setStatus(Idle);
    qInfo("LiveStreamDecoder: closed ('%s')", qPrintable(m_url));
}

QString LiveStreamDecoder::codecName() const
{
    std::lock_guard<std::mutex> lk(m_metaMutex);
    return m_codecName;
}

QString LiveStreamDecoder::pixelFormatName() const
{
    std::lock_guard<std::mutex> lk(m_metaMutex);
    return m_pixelFormatName;
}

int LiveStreamDecoder::statLiveSeconds() const
{
    if (status() != Live) return 0;
    const qint64 since = m_liveSinceMs.load(std::memory_order_acquire);
    if (since <= 0) return 0;
    const qint64 now =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    return int((now - since) / 1000);
}

// ---- worker ------------------------------------------------------

int LiveStreamDecoder::interruptCb(void *opaque)
{
    auto *self = static_cast<LiveStreamDecoder *>(opaque);
    return self->m_stopRequested.load(std::memory_order_acquire) ? 1 : 0;
}

bool LiveStreamDecoder::interruptibleSleep(int ms)
{
    std::unique_lock<std::mutex> lk(m_sleepMutex);
    return !m_sleepCv.wait_for(lk, std::chrono::milliseconds(ms), [this] {
        return m_stopRequested.load(std::memory_order_acquire);
    });
}

void LiveStreamDecoder::workerLoop()
{
    int backoffMs = kBackoffStartMs;
    bool firstAttempt = true;

    while (!m_stopRequested.load(std::memory_order_acquire)) {
        if (!firstAttempt) {
            setStatus(Reconnecting);
            m_reconnects.fetch_add(1, std::memory_order_acq_rel);
            if (!interruptibleSleep(backoffMs)) break;
            backoffMs = std::min(backoffMs * 2, kBackoffCapMs);
        }
        firstAttempt = false;

        const qint64 framesBefore =
            m_framesReceived.load(std::memory_order_acquire);
        connectOnce();
        if (m_framesReceived.load(std::memory_order_acquire) > framesBefore) {
            // Session delivered pixels — treat the next drop as a
            // fresh blip, not a continuation of a failing spree.
            backoffMs = kBackoffStartMs;
        }
    }
}

void LiveStreamDecoder::teardownSession(AVFormatContext **fmt,
                                        AVCodecContext **cctx,
                                        SwsContext **sws)
{
    // Freed inline on the worker thread — the DecoderCleanupQueue
    // exists to unblock the GUI thread, which we are not.
    if (*sws)  { sws_freeContext(*sws);       *sws  = nullptr; }
    if (*cctx) { avcodec_free_context(cctx); }
    if (*fmt)  { avformat_close_input(fmt); }
}

bool LiveStreamDecoder::connectOnce()
{
    const QByteArray urlUtf8 = m_url.toUtf8();

    AVFormatContext *fmt = avformat_alloc_context();
    if (!fmt) return false;
    fmt->interrupt_callback.callback = &LiveStreamDecoder::interruptCb;
    fmt->interrupt_callback.opaque   = this;

    // Latency-lean demux: no packet buffering, bounded probe. The
    // keyframe gate below (not error suppression) is what keeps the
    // mid-GOP join clean, so the small probe costs nothing visible.
    AVDictionary *opts = nullptr;
    av_dict_set(&opts, "fflags", "nobuffer", 0);
    av_dict_set(&opts, "analyzeduration", "1000000", 0);   // 1 s
    av_dict_set(&opts, "probesize", "2000000", 0);         // 2 MB

    int rc = avformat_open_input(&fmt, urlUtf8.constData(), nullptr, &opts);
    av_dict_free(&opts);
    if (rc < 0) {
        char err[128] = {};
        av_strerror(rc, err, sizeof(err));
        // Routine while the sender is down — log at info, not warning.
        qInfo("LiveStreamDecoder: connect failed (%s): %s",
              qPrintable(m_url), err);
        avformat_free_context(fmt);
        return false;
    }

    AVCodecContext *cctx = nullptr;
    SwsContext     *sws  = nullptr;

    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        qInfo("LiveStreamDecoder: no stream info (%s)", qPrintable(m_url));
        teardownSession(&fmt, &cctx, &sws);
        return false;
    }

    const int videoIdx =
        av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (videoIdx < 0) {
        qWarning("LiveStreamDecoder: no video stream in '%s'",
                 qPrintable(m_url));
        teardownSession(&fmt, &cctx, &sws);
        return false;
    }
    AVStream *vs = fmt->streams[videoIdx];

    // v1 demuxes everything and discards non-video, but reports audio
    // presence truthfully so the UI can say "audio not yet supported"
    // instead of pretending silence is the stream.
    bool sawAudio = false;
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            sawAudio = true;
        }
        // v1 plays video only — let the demuxer drop the rest instead
        // of surfacing packets we'd unref anyway. (Mbps stat becomes
        // video-only; remove when the live audio engine lands.)
        if (int(i) != videoIdx) {
            fmt->streams[i]->discard = AVDISCARD_ALL;
        }
    }

    const AVCodec *codec = avcodec_find_decoder(vs->codecpar->codec_id);
    if (!codec) {
        qWarning("LiveStreamDecoder: no decoder for codec id %d",
                 int(vs->codecpar->codec_id));
        teardownSession(&fmt, &cctx, &sws);
        return false;
    }
    cctx = avcodec_alloc_context3(codec);
    if (!cctx || avcodec_parameters_to_context(cctx, vs->codecpar) < 0) {
        teardownSession(&fmt, &cctx, &sws);
        return false;
    }
    cctx->flags |= AV_CODEC_FLAG_LOW_DELAY;

#if defined(Q_OS_MACOS)
    // VideoToolbox zero-copy — same attach pattern as VideoDecoder:
    // free win when the codec supports it, silent SW fallback when
    // not. QCView owns the live latency budget (the ffplay sw-decode
    // stopgap was most of the ~1 s glass-to-glass).
    AVBufferRef *hwDev = nullptr;
    if (av_hwdevice_ctx_create(&hwDev, AV_HWDEVICE_TYPE_VIDEOTOOLBOX,
                               nullptr, nullptr, 0) == 0) {
        cctx->hw_device_ctx = hwDev;   // cctx takes the ref
        cctx->get_format    = liveGetFormat;
    }
#elif defined(Q_OS_WIN)
    // D3D11VA — same routing as VideoDecoder's H.264/HEVC file path
    // (Vulkan stays ProRes-only on Windows; live streams are inter
    // codecs). GPU decode + NV12 readback in the receive loop below;
    // silent SW fallback via get_format when the codec has no d3d11va
    // support. Respects the same hardwareDecodeEnabled escape hatch
    // as file playback.
    if (QSettings().value(
            QStringLiteral("performance/hardwareDecodeEnabled"),
            true).toBool()) {
        AVBufferRef *hwDev = nullptr;
        if (av_hwdevice_ctx_create(&hwDev, AV_HWDEVICE_TYPE_D3D11VA,
                                   nullptr, nullptr, 0) == 0) {
            cctx->hw_device_ctx = hwDev;   // cctx takes the ref
            cctx->get_format    = liveGetFormat;
        }
    }
#endif

    if (avcodec_open2(cctx, codec, nullptr) < 0) {
        qWarning("LiveStreamDecoder: avcodec_open2 failed for %s",
                 codec->name);
        teardownSession(&fmt, &cctx, &sws);
        return false;
    }

    const char *pixName = av_get_pix_fmt_name(
        static_cast<AVPixelFormat>(vs->codecpar->format));
    if (!pixName) pixName = "?";
    setSessionMetadata(vs->codecpar->width, vs->codecpar->height,
                       QString::fromUtf8(codec->name),
                       QString::fromUtf8(pixName),
                       sawAudio);
    qInfo("LiveStreamDecoder: connected — %s %dx%d pix_fmt=%s audio=%s (%s)",
          codec->name, vs->codecpar->width, vs->codecpar->height,
          pixName, sawAudio ? "yes" : "no", qPrintable(m_url));

    AVPacket *pkt     = av_packet_alloc();
    AVFrame  *frame   = av_frame_alloc();
    AVFrame  *swFrame = av_frame_alloc();  // landing pad for hw → cpu transfer

    // 4c receive discipline. Phases: (1) DrainToEdge — the backlog
    // that piled up behind avformat_find_stream_info reads without
    // blocking; discard it undecoded so it never becomes standing
    // latency. (2) WaitKeyframe — the existing mid-GOP join gate,
    // now anchored at the live edge. (3) Streaming.
    enum class Phase { DrainToEdge, WaitKeyframe, Streaming };
    Phase phase = Phase::DrainToEdge;
    int     fastReads       = 0;      // consecutive non-blocking reads
    bool    draining        = false;  // steady-state catch-up burst
    qint64  drainedPackets  = 0;      // discarded pre-join
    qint64  lastPublishMs   = 0;

    const auto nowMs = [] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    };

    while (!m_stopRequested.load(std::memory_order_acquire)) {
        const qint64 readStart = nowMs();
        rc = av_read_frame(fmt, pkt);
        if (rc < 0) {
            // EOF = sender ended the session (listener orphaned us);
            // other errors = transport drop. Either way: reconnect.
            if (!m_stopRequested.load(std::memory_order_acquire)) {
                char err[128] = {};
                av_strerror(rc, err, sizeof(err));
                qInfo("LiveStreamDecoder: stream ended/dropped (%s) — "
                      "will reconnect", err);
            }
            break;
        }
        const bool blockedRead = (nowMs() - readStart) >= kLiveEdgeBlockMs;
        m_bytesReceived.fetch_add(pkt->size, std::memory_order_acq_rel);
        if (pkt->stream_index != videoIdx) {
            av_packet_unref(pkt);
            continue;
        }

        if (phase == Phase::DrainToEdge) {
            if (!blockedRead) {
                ++drainedPackets;
                av_packet_unref(pkt);
                continue;
            }
            phase = Phase::WaitKeyframe;
            if (drainedPackets > 0) {
                qInfo("LiveStreamDecoder: drained %lld backlog packets "
                      "to the live edge", (long long)drainedPackets);
            }
        }
        if (phase == Phase::WaitKeyframe) {
            if (!(pkt->flags & AV_PKT_FLAG_KEY)) {
                av_packet_unref(pkt);
                continue;
            }
            phase = Phase::Streaming;
        }

        // Steady-state burst tracking: a run of non-blocking reads
        // means we fell behind (receiver stall). Drop non-reference
        // frames and skip the publish conversion until the reads
        // block again — that IS the live edge.
        if (blockedRead) {
            fastReads = 0;
            if (draining) {
                draining = false;
                cctx->skip_frame = AVDISCARD_DEFAULT;
            }
        } else if (++fastReads >= kBurstFastReads && !draining) {
            draining = true;
            cctx->skip_frame = AVDISCARD_NONREF;
        }

        rc = avcodec_send_packet(cctx, pkt);
        av_packet_unref(pkt);
        if (rc < 0 && rc != AVERROR(EAGAIN)) {
            continue;   // isolated bad packet; keep the session
        }
        while (avcodec_receive_frame(cctx, frame) == 0) {
            const bool publishThis =
                !draining || (nowMs() - lastPublishMs) >= kDrainKeepaliveMs;
            if (!publishThis) {
                m_framesConflated.fetch_add(1, std::memory_order_acq_rel);
                av_frame_unref(frame);
                continue;
            }
#if defined(Q_OS_WIN)
            if (frame->format == AV_PIX_FMT_D3D11 && swFrame) {
                // GPU-decoded frame: NV12 readback, then the shared
                // sws path — mirrors VideoDecoder's D3D11VA branch.
                if (av_hwframe_transfer_data(swFrame, frame, 0) >= 0) {
                    swFrame->pts                   = frame->pts;
                    swFrame->best_effort_timestamp = frame->best_effort_timestamp;
                    swFrame->colorspace            = frame->colorspace;
                    swFrame->color_range           = frame->color_range;
                    swFrame->color_primaries       = frame->color_primaries;
                    swFrame->color_trc             = frame->color_trc;
                    publishFrame(swFrame, cctx, &sws, vs->time_base,
                                 "d3d11va→RGBA");
                    lastPublishMs = nowMs();
                    av_frame_unref(swFrame);
                }
                av_frame_unref(frame);
                continue;
            }
#endif
            publishFrame(frame, cctx, &sws, vs->time_base);
            lastPublishMs = nowMs();
            av_frame_unref(frame);
        }
    }

    const qint64 conflated =
        m_framesConflated.load(std::memory_order_acquire);
    if (conflated > 0) {
        qInfo("LiveStreamDecoder: session end — %lld frames conflated "
              "while draining backlog", (long long)conflated);
    }

    av_frame_free(&swFrame);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    teardownSession(&fmt, &cctx, &sws);
    return true;
}

void LiveStreamDecoder::publishFrame(AVFrame *frame, AVCodecContext *cctx,
                                     SwsContext **sws, const AVRational &tb,
                                     const char *srcLabel)
{
    VideoDecoder *sink = m_sink;
    if (!sink) return;

    const int64_t pts = (frame->best_effort_timestamp != AV_NOPTS_VALUE)
                        ? frame->best_effort_timestamp : frame->pts;
    const int64_t ptsUs = (pts != AV_NOPTS_VALUE)
        ? av_rescale_q(pts, tb, AVRational{1, 1000000}) : 0;

#if defined(Q_OS_MACOS)
    if (frame->format == AV_PIX_FMT_VIDEOTOOLBOX) {
        void *pix = frame->data[3];
        if (pix) {
            cvPixelBufferRetainRaw(pix);
            sink->publishExternalFrame(
                FrameHandle::metal(pix, frame->width, frame->height, ptsUs),
                ptsUs);
        }
    } else
#endif
    {
        // Software fallback (and the v1 Windows/Linux path): convert
        // to RGBA8 — mirrors VideoDecoder's CPU sink.
        *sws = sws_getCachedContext(*sws,
                frame->width, frame->height,
                static_cast<AVPixelFormat>(frame->format),
                frame->width, frame->height, AV_PIX_FMT_RGBA,
                SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!*sws) return;
        QImage rgba(frame->width, frame->height, QImage::Format_RGBA8888);
        uint8_t *dst[4]      = { rgba.bits(), nullptr, nullptr, nullptr };
        int      dstStride[4] = { int(rgba.bytesPerLine()), 0, 0, 0 };
        sws_scale(*sws, frame->data, frame->linesize, 0, frame->height,
                  dst, dstStride);
        sink->publishExternalFrame(FrameHandle::cpu(std::move(rgba), ptsUs),
                                   ptsUs);
    }
    Q_UNUSED(cctx);

    if (m_framesReceived.fetch_add(1, std::memory_order_acq_rel) == 0
        || status() != Live) {
        qInfo("LiveStreamDecoder: LIVE — first frame published (%s, %dx%d)",
#if defined(Q_OS_MACOS)
              frame->format == AV_PIX_FMT_VIDEOTOOLBOX
                  ? "videotoolbox zero-copy" : srcLabel,
#else
              srcLabel,
#endif
              frame->width, frame->height);
        m_liveSinceMs.store(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count(),
            std::memory_order_release);
        setStatus(Live);
    }

    std::function<void()> cb;
    {
        std::lock_guard<std::mutex> lk(m_frameCbMutex);
        cb = m_frameCb;
    }
    if (cb) cb();
}

// ---- cross-thread state publication ------------------------------

void LiveStreamDecoder::setStatus(Status s)
{
    if (m_status.exchange(s, std::memory_order_acq_rel) == s) return;
    // Queue the emit onto the object's (GUI) thread — worker-side
    // direct emits would run QML handlers on the decode thread.
    QMetaObject::invokeMethod(this, [this] { emit statusChanged(); },
                              Qt::QueuedConnection);
}

void LiveStreamDecoder::setSessionMetadata(int w, int h, const QString &codec,
                                           const QString &pixFmt,
                                           bool hasAudio)
{
    m_width.store(w, std::memory_order_release);
    m_height.store(h, std::memory_order_release);
    m_hasAudio.store(hasAudio, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lk(m_metaMutex);
        m_codecName       = codec;
        m_pixelFormatName = pixFmt;
    }
    QMetaObject::invokeMethod(this, [this] { emit metadataChanged(); },
                              Qt::QueuedConnection);
}

} // namespace qcv
