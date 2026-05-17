#include "scrub_decoder.h"
#include "decoder_cleanup_queue.h"
#include "video_decoder.h"

#if defined(Q_OS_WIN)
// Cut A — see VideoDecoder::close for rationale. Same flush before
// async hwdevice unref so Intel Arc doesn't hit a device-lost on the
// next decoder open while a stale frame is still GPU-active.
#  include "decode/vulkan/vulkan_device_manager.h"
#endif

#include <QFileInfo>
#include <QImage>
#include <QSettings>
#include <QtLogging>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

extern "C" void cvPixelBufferRetainRaw(void *cvPix);
extern "C" bool cvPixelBufferIsZeroCopySupportedRaw(void *cvPix);

namespace qcv {

namespace {

QString avErrToString(int err)
{
    char buf[256];
    av_strerror(err, buf, sizeof(buf));
    return QString::fromUtf8(buf);
}

AVPixelFormat hwaccelGetFormat(AVCodecContext * /*ctx*/, const AVPixelFormat *fmts)
{
#if defined(Q_OS_MACOS)
    constexpr AVPixelFormat kPreferred = AV_PIX_FMT_VIDEOTOOLBOX;
#elif defined(Q_OS_WIN)
    constexpr AVPixelFormat kPreferred = AV_PIX_FMT_D3D11;
#elif defined(Q_OS_LINUX)
    constexpr AVPixelFormat kPreferred = AV_PIX_FMT_VAAPI;
#else
    constexpr AVPixelFormat kPreferred = AV_PIX_FMT_NONE;
#endif
    for (int i = 0; fmts[i] != AV_PIX_FMT_NONE; ++i) {
        if (fmts[i] == kPreferred) return kPreferred;
    }
    return fmts[0];
}

} // namespace

ScrubDecoder::ScrubDecoder(VideoDecoder *streaming, QObject *parent)
    : QObject(parent), m_streaming(streaming)
{}

ScrubDecoder::~ScrubDecoder()
{
    close();
}

bool ScrubDecoder::open(const QString &path)
{
    close();

    if (!QFileInfo(path).isFile()) {
        qWarning("ScrubDecoder: file not found: %s", qPrintable(path));
        return false;
    }

    if (!initFFmpeg(path)) {
        teardownFFmpeg();
        return false;
    }

    m_stopRequested.store(false, std::memory_order_release);
    m_pendingTarget.store(-1, std::memory_order_release);
    m_lastDecodedFrame = -1;
    m_thread = std::thread([this] { workerLoop(); });
    return true;
}

void ScrubDecoder::close()
{
    m_stopRequested.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lk(m_condMutex);
    }
    m_cond.notify_one();

    // Synchronous join — required for safety; the worker reads
    // m_cctx / m_fmt as members each iteration. Wakes fast on the
    // stop flag.
    if (m_thread.joinable()) m_thread.join();

#if defined(Q_OS_WIN)
    // See VideoDecoder::close for rationale.
    VulkanDeviceManager::instance().waitForGpu();
#endif

    // Async-free the FFmpeg contexts (slow av_buffer_unref of
    // hwframes_ctx runs off the UI thread). Same pattern as
    // VideoDecoder::close.
    AVFormatContext *fmt   = m_fmt;          m_fmt = nullptr;
    AVCodecContext  *cctx  = m_cctx;         m_cctx = nullptr;
    SwsContext      *sws   = m_sws;          m_sws = nullptr;
    AVBufferRef     *hwDev = m_hwDeviceCtx;  m_hwDeviceCtx = nullptr;
    postFFmpegCleanup(fmt, cctx, sws, hwDev);

    m_videoStreamIdx = -1;
    m_swsSrcWidth = m_swsSrcHeight = 0;
    m_swsSrcFormat = -1;
    m_pendingTarget.store(-1, std::memory_order_release);
    m_lastDecodedFrame = -1;
}

void ScrubDecoder::requestFrame(int frameNo)
{
    if (frameNo < 0) frameNo = 0;
    m_pendingTarget.store(frameNo, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lk(m_condMutex);
    }
    m_cond.notify_one();
}

bool ScrubDecoder::initFFmpeg(const QString &path)
{
    const QByteArray pathUtf8 = path.toUtf8();
    // TRACE_SCRUB_OPEN — start of the scrub-decoder's
    // avformat_open_input. Runs on the scrub thread, often
    // concurrent with the playback decoder's open + metadata
    // extractor on the same file.
    const QString trimmedName = QFileInfo(path).fileName();
    qInfo("ScrubDecoder: initFFmpeg begin '%s'", qPrintable(trimmedName));

    if (avformat_open_input(&m_fmt, pathUtf8.constData(), nullptr, nullptr) < 0) return false;
    qInfo("ScrubDecoder: avformat_open_input done '%s'",
          qPrintable(trimmedName));
    if (avformat_find_stream_info(m_fmt, nullptr) < 0) return false;
    qInfo("ScrubDecoder: find_stream_info done '%s'",
          qPrintable(trimmedName));

    for (unsigned i = 0; i < m_fmt->nb_streams; ++i) {
        if (m_fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            m_videoStreamIdx = static_cast<int>(i);
            break;
        }
    }
    if (m_videoStreamIdx < 0) return false;

    AVStream *st = m_fmt->streams[m_videoStreamIdx];
    AVCodecParameters *codecpar = st->codecpar;
    const AVCodec *codec = avcodec_find_decoder(codecpar->codec_id);
    if (!codec) return false;

    m_cctx = avcodec_alloc_context3(codec);
    if (!m_cctx) return false;
    avcodec_parameters_to_context(m_cctx, codecpar);

    // Deliberately NOT reading the user's `performance/ffmpegThreads`
    // QSetting here — see VideoDecoder::initFFmpeg for the per-decoder
    // count. Scrub runs alongside the main VideoDecoder during seek
    // bursts, so two AVCodecContexts compete; applying the user knob
    // to both would 2x the requested threads. FFmpeg's per-context
    // auto (thread_count = 0, default) picks a sensible per-codec
    // value that doesn't fight the main decoder's threads. The
    // settings panel help text reflects this.

#if defined(Q_OS_MACOS)
    constexpr auto kHwType = AV_HWDEVICE_TYPE_VIDEOTOOLBOX;
#elif defined(Q_OS_WIN)
    constexpr auto kHwType = AV_HWDEVICE_TYPE_D3D11VA;
#elif defined(Q_OS_LINUX)
    constexpr auto kHwType = AV_HWDEVICE_TYPE_VAAPI;
#else
    constexpr auto kHwType = AV_HWDEVICE_TYPE_NONE;
#endif

    // Hwaccel is gated on intra-only codecs. Old QCView (scrub_
    // decoder.cpp:146-167) does the same and the comment there is
    // load-bearing: VideoToolbox's async B-frame reorder pipeline
    // loses reference frames after avcodec_flush_buffers, which the
    // scrub path calls on every seek. For inter-frame codecs we'd
    // pay 50–200 ms per scrub step rebuilding VT session state —
    // software decode of one keyframe + a short P/B chain on Apple
    // Silicon is dramatically faster. Intra codecs (ProRes, DNxHD,
    // MJPEG, raw) have no reorder dependency so VT works cleanly.
    const AVCodecDescriptor *desc = avcodec_descriptor_get(codecpar->codec_id);
    const bool intraOnly = desc && (desc->props & AV_CODEC_PROP_INTRA_ONLY);

    // performance/hardwareDecodeEnabled — Windows user-facing toggle
    // (defaults ON). When the user turns it OFF — escape hatch for
    // mini-PCs with broken GPU drivers (see [[intel-arc-vulkan-
    // bridge-crash]] memory) — we skip every hwaccel attach across
    // single, dual, scrub, playlist. macOS is unaffected (no
    // checkbox there; VideoToolbox always used).
#if defined(Q_OS_WIN)
    const bool kHwDecodeEnabled = QSettings().value(
        QStringLiteral("performance/hardwareDecodeEnabled"), true).toBool();
    const bool kForceSoftwareDecode = !kHwDecodeEnabled;
#else
    const bool kForceSoftwareDecode = false;
#endif

    if (intraOnly && kHwType != AV_HWDEVICE_TYPE_NONE
        && !kForceSoftwareDecode) {
        if (av_hwdevice_ctx_create(&m_hwDeviceCtx, kHwType, nullptr, nullptr, 0) >= 0) {
            m_cctx->hw_device_ctx = av_buffer_ref(m_hwDeviceCtx);
            m_cctx->get_format = hwaccelGetFormat;
        }
    } else if (kForceSoftwareDecode) {
        qInfo("ScrubDecoder: software decode forced — "
              "performance/hardwareDecodeEnabled is off");
    }

    if (avcodec_open2(m_cctx, codec, nullptr) < 0) return false;
    return true;
}

void ScrubDecoder::teardownFFmpeg()
{
    if (m_sws) { sws_freeContext(m_sws); m_sws = nullptr; }
    if (m_cctx) avcodec_free_context(&m_cctx);
    if (m_fmt)  avformat_close_input(&m_fmt);
    if (m_hwDeviceCtx) av_buffer_unref(&m_hwDeviceCtx);
    m_videoStreamIdx = -1;
    m_swsSrcWidth = m_swsSrcHeight = 0;
    m_swsSrcFormat = -1;
}

bool ScrubDecoder::initSwsContext(AVFrame *frame)
{
    if (m_sws &&
        m_swsSrcWidth  == frame->width &&
        m_swsSrcHeight == frame->height &&
        m_swsSrcFormat == frame->format) {
        return true;
    }
    if (m_sws) { sws_freeContext(m_sws); m_sws = nullptr; }
    m_sws = sws_getContext(
        frame->width, frame->height, static_cast<AVPixelFormat>(frame->format),
        frame->width, frame->height, AV_PIX_FMT_RGBA,
        SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!m_sws) return false;

    int srcCsp;
    switch (frame->colorspace) {
        case AVCOL_SPC_BT709:      srcCsp = SWS_CS_ITU709; break;
        case AVCOL_SPC_BT470BG:    srcCsp = SWS_CS_ITU601; break;
        case AVCOL_SPC_SMPTE170M:  srcCsp = SWS_CS_SMPTE170M; break;
        case AVCOL_SPC_SMPTE240M:  srcCsp = SWS_CS_SMPTE240M; break;
        case AVCOL_SPC_FCC:        srcCsp = SWS_CS_FCC; break;
        case AVCOL_SPC_BT2020_NCL:
        case AVCOL_SPC_BT2020_CL:  srcCsp = SWS_CS_BT2020; break;
        default:
            srcCsp = (frame->width >= 1280 || frame->height >= 720)
                     ? SWS_CS_ITU709 : SWS_CS_SMPTE170M;
            break;
    }
    const int srcFullRange = (frame->color_range == AVCOL_RANGE_JPEG) ? 1 : 0;
    sws_setColorspaceDetails(
        m_sws,
        sws_getCoefficients(srcCsp), srcFullRange,
        sws_getCoefficients(SWS_CS_ITU709), 1,
        0, 1 << 16, 1 << 16);

    m_swsSrcWidth  = frame->width;
    m_swsSrcHeight = frame->height;
    m_swsSrcFormat = frame->format;
    return true;
}

void ScrubDecoder::workerLoop()
{
    AVPacket *pkt    = av_packet_alloc();
    AVFrame  *frame  = av_frame_alloc();
    AVFrame  *swFrame = av_frame_alloc();
    if (!pkt || !frame || !swFrame) {
        if (pkt) av_packet_free(&pkt);
        if (frame) av_frame_free(&frame);
        if (swFrame) av_frame_free(&swFrame);
        return;
    }

    while (!m_stopRequested.load(std::memory_order_acquire)) {
        int target = m_pendingTarget.exchange(-1, std::memory_order_acq_rel);
        if (target < 0) {
            std::unique_lock<std::mutex> lk(m_condMutex);
            m_cond.wait(lk, [this] {
                return m_stopRequested.load(std::memory_order_acquire) ||
                       m_pendingTarget.load(std::memory_order_acquire) >= 0;
            });
            continue;
        }
        if (target == m_lastDecodedFrame) {
            // Already showing this frame — skip (cache hit per Guide 13 §I.7).
            continue;
        }
        decodeAndPublish(target, pkt, frame, swFrame);
    }

    av_packet_free(&pkt);
    av_frame_free(&frame);
    av_frame_free(&swFrame);
}

bool ScrubDecoder::decodeAndPublish(int target, AVPacket *pkt,
                                    AVFrame *frame, AVFrame *swFrame)
{
    if (!m_streaming) return false;
    const FrameIndex &fi = m_streaming->frameIndex();
    if (!fi.isValid()) return false;

    const int64_t targetPts = fi.ptsForFrame(target);

    if (av_seek_frame(m_fmt, m_videoStreamIdx, targetPts,
                      AVSEEK_FLAG_BACKWARD) < 0) {
        return false;
    }
    avcodec_flush_buffers(m_cctx);

    while (!m_stopRequested.load(std::memory_order_acquire)) {
        // Note: NO mid-decode preemption check here. A continuous
        // drag at 60 Hz fires a new requestFrame every ~16 ms; if we
        // abandoned each time pendingTarget was set, we'd never
        // complete a decode mid-drag and the player would freeze
        // until release. Instead, let each decode finish and rely on
        // the worker loop's exchange(-1) to take the LATEST pending
        // target on next iteration, naturally skipping intermediate
        // drag values. Lag during fast drag = "decoder can't keep
        // up" rather than "decoder gives up".

        const int rc = av_read_frame(m_fmt, pkt);
        if (rc == AVERROR_EOF) {
            avcodec_send_packet(m_cctx, nullptr);
        } else if (rc < 0) {
            return false;
        } else {
            if (pkt->stream_index == m_videoStreamIdx) {
                avcodec_send_packet(m_cctx, pkt);
            }
            av_packet_unref(pkt);
        }

        while (true) {
            const int recvErr = avcodec_receive_frame(m_cctx, frame);
            if (recvErr == AVERROR(EAGAIN)) break;
            if (recvErr < 0) return false;

            const int64_t framePts = (frame->best_effort_timestamp != AV_NOPTS_VALUE)
                                     ? frame->best_effort_timestamp
                                     : frame->pts;

            if (framePts >= targetPts) {
                if (frame->format == AV_PIX_FMT_VIDEOTOOLBOX) {
#if defined(Q_OS_MACOS)
                    void *pix = frame->data[3];
                    if (pix && cvPixelBufferIsZeroCopySupportedRaw(pix)) {
                        cvPixelBufferRetainRaw(pix);
                        m_streaming->publishExternalFrame(
                            FrameHandle::metal(pix, frame->width, frame->height,
                                               framePts),
                            framePts);
                    } else if (av_hwframe_transfer_data(swFrame, frame, 0) >= 0) {
                        // Format isn't biplanar — fall back to CPU
                        // readback + swscale path below.
                        swFrame->pts                   = frame->pts;
                        swFrame->best_effort_timestamp = frame->best_effort_timestamp;
                        swFrame->colorspace            = frame->colorspace;
                        swFrame->color_range           = frame->color_range;
                        if (initSwsContext(swFrame)) {
                            QImage rgba(swFrame->width, swFrame->height,
                                        QImage::Format_RGBA8888);
                            uint8_t *dst[4] = { rgba.bits(), nullptr, nullptr, nullptr };
                            int dstStride[4] = { static_cast<int>(rgba.bytesPerLine()),
                                                 0, 0, 0 };
                            sws_scale(m_sws, swFrame->data, swFrame->linesize, 0,
                                      swFrame->height, dst, dstStride);
                            m_streaming->publishExternalFrame(
                                FrameHandle::cpu(std::move(rgba), framePts), framePts);
                        }
                        av_frame_unref(swFrame);
                    }
#endif
                } else if (frame->format == AV_PIX_FMT_D3D11 ||
                           frame->format == AV_PIX_FMT_VAAPI) {
                    if (av_hwframe_transfer_data(swFrame, frame, 0) >= 0) {
                        swFrame->pts                   = frame->pts;
                        swFrame->best_effort_timestamp = frame->best_effort_timestamp;
                        swFrame->colorspace            = frame->colorspace;
                        swFrame->color_range           = frame->color_range;
                        if (initSwsContext(swFrame)) {
                            QImage rgba(swFrame->width, swFrame->height,
                                        QImage::Format_RGBA8888);
                            uint8_t *dst[4] = { rgba.bits(), nullptr, nullptr, nullptr };
                            int dstStride[4] = { static_cast<int>(rgba.bytesPerLine()),
                                                 0, 0, 0 };
                            sws_scale(m_sws, swFrame->data, swFrame->linesize, 0,
                                      swFrame->height, dst, dstStride);
                            m_streaming->publishExternalFrame(
                                FrameHandle::cpu(std::move(rgba), framePts), framePts);
                        }
                        av_frame_unref(swFrame);
                    }
                } else {
                    if (initSwsContext(frame)) {
                        QImage rgba(frame->width, frame->height,
                                    QImage::Format_RGBA8888);
                        uint8_t *dst[4] = { rgba.bits(), nullptr, nullptr, nullptr };
                        int dstStride[4] = { static_cast<int>(rgba.bytesPerLine()),
                                             0, 0, 0 };
                        sws_scale(m_sws, frame->data, frame->linesize, 0,
                                  frame->height, dst, dstStride);
                        m_streaming->publishExternalFrame(
                            FrameHandle::cpu(std::move(rgba), framePts), framePts);
                    }
                }

                m_lastDecodedFrame = target;
                av_frame_unref(frame);
                return true;
            }
            // Pre-target frame in inter-frame chain — drop silently.
            av_frame_unref(frame);
        }

        if (rc == AVERROR_EOF) return false;
    }
    return false;
}

} // namespace qcv
