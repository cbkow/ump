#include "dual_scrub_decoder.h"
#include "dual_video_decoder.h"

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
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

namespace qcv::dual {

namespace {

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

// Build a DualFrame::Kind::Cpu from a packed-RGBA QImage (the format
// our sws_scale produces). Mirrors the construction shape that
// DualImageSeqSource publishes.
std::shared_ptr<DualFrame> makeCpuFrame(QImage rgba, int frameNumber,
                                         int rangeOverride)
{
    auto f = std::make_shared<DualFrame>();
    f->kind        = DualFrame::Kind::Cpu;
    f->width       = rgba.width();
    f->height      = rgba.height();
    f->frameNumber = frameNumber;
    f->rangeOverride = rangeOverride;
    f->rgba        = std::make_shared<QImage>(std::move(rgba));
    return f;
}

} // namespace

DualScrubDecoder::DualScrubDecoder(DualVideoDecoder *streaming, QObject *parent)
    : QObject(parent), m_streaming(streaming)
{}

DualScrubDecoder::~DualScrubDecoder()
{
    close();
}

// Platform factory (Windows/Linux build). The macOS build supplies its
// own definition in dual_scrub_decoder_metal.mm; CMake compiles exactly
// one of the two per platform.
std::unique_ptr<IDualScrubDecoder>
makeDualScrubDecoder(DualVideoDecoder *streaming, QObject *parent)
{
    return std::make_unique<DualScrubDecoder>(streaming, parent);
}

bool DualScrubDecoder::open(const QString &path)
{
    close();

    if (!QFileInfo(path).isFile()) {
        qWarning("DualScrubDecoder: file not found: %s", qPrintable(path));
        return false;
    }

    if (!initFFmpeg(path)) {
        teardownFFmpeg();
        return false;
    }

    // Per-side GOP-cache byte budget (same knob as single-flow). Note each
    // dual side has its own cache, so A+B is 2x this value.
    const std::size_t budgetMB = static_cast<std::size_t>(
        QSettings().value(QStringLiteral("performance/scrubCacheMB"), 512).toInt());
    m_gopCache.clear();
    m_gopCache.setMaxBytes(budgetMB * 1024ull * 1024ull);

    m_stopRequested.store(false, std::memory_order_release);
    m_pendingTarget.store(-1, std::memory_order_release);
    m_lastShown = -1;
    m_decoderPos = -1;
    m_decoderPositioned = false;
    m_cachedRangeOv = m_streaming ? m_streaming->rangeOverride() : 0;
    m_thread = std::thread([this] { workerLoop(); });
    return true;
}

void DualScrubDecoder::close()
{
    m_stopRequested.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lk(m_condMutex);
    }
    m_cond.notify_one();

    if (m_thread.joinable()) m_thread.join();

    // Drop cached frames before freeing the contexts (worker is joined).
    m_gopCache.clear();

#if defined(Q_OS_WIN)
    // See VideoDecoder::close for rationale (mirrored from ScrubDecoder).
    VulkanDeviceManager::instance().waitForGpu();
#endif

    if (m_sws) { sws_freeContext(m_sws); m_sws = nullptr; }
    if (m_cctx) avcodec_free_context(&m_cctx);
    if (m_fmt)  avformat_close_input(&m_fmt);
    if (m_hwDeviceCtx) av_buffer_unref(&m_hwDeviceCtx);

    m_videoStreamIdx = -1;
    m_swsSrcWidth = m_swsSrcHeight = 0;
    m_swsSrcFormat = -1;
    m_pendingTarget.store(-1, std::memory_order_release);
    m_lastShown = -1;
    m_decoderPos = -1;
    m_decoderPositioned = false;
}

void DualScrubDecoder::requestFrame(int frameNo)
{
    if (frameNo < 0) frameNo = 0;
    m_pendingTarget.store(frameNo, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lk(m_condMutex);
    }
    m_cond.notify_one();
}

bool DualScrubDecoder::initFFmpeg(const QString &path)
{
    const QByteArray pathUtf8 = path.toUtf8();
    const QString trimmedName = QFileInfo(path).fileName();
    qInfo("DualScrubDecoder: initFFmpeg begin '%s'", qPrintable(trimmedName));

    if (avformat_open_input(&m_fmt, pathUtf8.constData(), nullptr, nullptr) < 0) return false;
    if (avformat_find_stream_info(m_fmt, nullptr) < 0) return false;

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
    // QSetting — see ScrubDecoder::initFFmpeg for the rationale.
    // Scrub coexists with the streaming DualVideoDecoder and we
    // don't want both reading the user's knob and doubling the
    // requested thread count. FFmpeg's per-context auto picks a
    // sensible value.

#if defined(Q_OS_MACOS)
    constexpr auto kHwType = AV_HWDEVICE_TYPE_VIDEOTOOLBOX;
#elif defined(Q_OS_WIN)
    constexpr auto kHwType = AV_HWDEVICE_TYPE_D3D11VA;
#elif defined(Q_OS_LINUX)
    constexpr auto kHwType = AV_HWDEVICE_TYPE_VAAPI;
#else
    constexpr auto kHwType = AV_HWDEVICE_TYPE_NONE;
#endif

    // Hwaccel is gated on intra-only codecs (same rationale as
    // ScrubDecoder::initFFmpeg). Output is ALWAYS Cpu-kind via
    // av_hwframe_transfer_data + sws_scale below — we never produce
    // VulkanShared payloads, so the dual Vulkan compositor's compute
    // pipeline (which crashes on NVIDIA with ProRes) is bypassed even
    // when HW decode is attached.
    const AVCodecDescriptor *desc = avcodec_descriptor_get(codecpar->codec_id);
    const bool intraOnly = desc && (desc->props & AV_CODEC_PROP_INTRA_ONLY);

    // Intra codecs are random-access — direct-seek + decode-1 is already
    // optimal; the GOP cache + forward-fill targets inter (b-frame) only. So
    // intra takes the legacy path on all platforms (this decoder is Win/Linux;
    // macOS has its own MacDualScrubDecoder with the same rule).
    m_intraDirectScrub = intraOnly;

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
        qInfo("DualScrubDecoder: software decode forced — "
              "performance/hardwareDecodeEnabled is off");
    }

    if (avcodec_open2(m_cctx, codec, nullptr) < 0) return false;
    qInfo("DualScrubDecoder: opened '%s'", qPrintable(trimmedName));
    return true;
}

void DualScrubDecoder::teardownFFmpeg()
{
    if (m_sws) { sws_freeContext(m_sws); m_sws = nullptr; }
    if (m_cctx) avcodec_free_context(&m_cctx);
    if (m_fmt)  avformat_close_input(&m_fmt);
    if (m_hwDeviceCtx) av_buffer_unref(&m_hwDeviceCtx);
    m_videoStreamIdx = -1;
    m_swsSrcWidth = m_swsSrcHeight = 0;
    m_swsSrcFormat = -1;
}

bool DualScrubDecoder::initSwsContext(AVFrame *frame)
{
    // Range override is owned by the paired streaming decoder (the
    // controller keeps both sides in sync). Re-apply colorspace details
    // when it changes even if dims/format are unchanged, so scrubbed
    // levels match playback. This decoder always publishes a CPU QImage,
    // so the override must be baked here via sws.
    const int rangeOv = m_streaming ? m_streaming->rangeOverride() : 0;
    const bool dimsSame =
        m_sws &&
        m_swsSrcWidth  == frame->width &&
        m_swsSrcHeight == frame->height &&
        m_swsSrcFormat == frame->format;
    if (dimsSame && rangeOv == m_swsAppliedRange) {
        return true;
    }
    if (!dimsSame) {
        if (m_sws) { sws_freeContext(m_sws); m_sws = nullptr; }
        m_sws = sws_getContext(
            frame->width, frame->height,
            static_cast<AVPixelFormat>(frame->format),
            frame->width, frame->height, AV_PIX_FMT_RGBA,
            SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!m_sws) return false;
    }

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
    int srcFullRange = (frame->color_range == AVCOL_RANGE_JPEG) ? 1 : 0;
    if (rangeOv == 1)      srcFullRange = 1;   // Full
    else if (rangeOv == 2) srcFullRange = 0;   // Limited
    sws_setColorspaceDetails(
        m_sws,
        sws_getCoefficients(srcCsp), srcFullRange,
        sws_getCoefficients(SWS_CS_ITU709), 1,
        0, 1 << 16, 1 << 16);

    m_swsSrcWidth     = frame->width;
    m_swsSrcHeight    = frame->height;
    m_swsSrcFormat    = frame->format;
    m_swsAppliedRange = rangeOv;
    return true;
}

void DualScrubDecoder::workerLoop()
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
        if (target == m_lastShown) {
            // Already showing this frame — skip (cache hit).
            continue;
        }
        decodeAndPublish(target, pkt, frame, swFrame);
    }

    av_packet_free(&pkt);
    av_frame_free(&frame);
    av_frame_free(&swFrame);
}

bool DualScrubDecoder::decodeAndPublish(int target, AVPacket *pkt,
                                        AVFrame *frame, AVFrame *swFrame)
{
    if (!m_streaming) return false;

    // Range override changed (controller pushed it onto the streaming
    // decoder) → cached CPU levels are stale; drop and refill.
    const int rangeOv = m_streaming->rangeOverride();
    if (rangeOv != m_cachedRangeOv) {
        m_gopCache.clear();
        m_decoderPositioned = false;
        m_decoderPos = -1;
        m_cachedRangeOv = rangeOv;
    }

    // 1. Cache hit — serve without touching the decoder.
    {
        std::shared_ptr<DualScrubEntry> hit;
        if (m_gopCache.get(target, hit) && hit) {
            publishEntry(hit);
            m_lastShown = target;
            return true;
        }
    }

    const int64_t targetPts = m_streaming->ptsForFrameNumber(target);

    // 2. Forward-no-reseek (see single-flow ScrubDecoder for the why).
    //    m_intraDirectScrub (Windows + intra) forces seek+flush — intra seek
    //    lands exact, so forward-fill would decode intermediates for nothing.
    constexpr int kForwardReach = 90;
    const bool canForward =
        !m_intraDirectScrub &&
        m_decoderPositioned &&
        target > m_decoderPos &&
        (target - m_decoderPos) <= kForwardReach;

    if (!canForward) {
        // 3. Cold / backward / long-jump: seek to the keyframe at-or-before.
        if (av_seek_frame(m_fmt, m_videoStreamIdx, targetPts,
                          AVSEEK_FLAG_BACKWARD) < 0) {
            return false;
        }
        avcodec_flush_buffers(m_cctx);
        m_decoderPositioned = false;
        m_decoderPos = -1;
    }

    return decodeForwardCaching(target, targetPts, pkt, frame, swFrame);
}

bool DualScrubDecoder::decodeForwardCaching(int target, int64_t targetPts,
                                            AVPacket *pkt, AVFrame *frame,
                                            AVFrame *swFrame)
{
    while (!m_stopRequested.load(std::memory_order_acquire)) {
        // No mid-decode preemption check — see ScrubDecoder::decodeAndPublish
        // for the rationale. The worker-loop's exchange(-1) at the top of
        // each iteration takes the LATEST pending target, naturally skipping
        // intermediate drag values during fast drags.

        const int rc = av_read_frame(m_fmt, pkt);
        if (rc == AVERROR_EOF) {
            avcodec_send_packet(m_cctx, nullptr);
        } else if (rc < 0) {
            m_decoderPositioned = false;
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
            if (recvErr < 0) { m_decoderPositioned = false; return false; }

            const int64_t framePts = (frame->best_effort_timestamp != AV_NOPTS_VALUE)
                                     ? frame->best_effort_timestamp
                                     : frame->pts;
            // Key the landed frame exactly by `target` (immune to PTS
            // rounding); prefix frames by their decoded PTS.
            const bool isTarget = (framePts >= targetPts);
            const int  frameNo  = isTarget
                ? target : m_streaming->frameNumberForPts(framePts);

            std::shared_ptr<DualScrubEntry> entry =
                makeEntry(frame, swFrame, frameNo);
            m_decoderPos = frameNo;
            m_decoderPositioned = true;

            if (entry) {
                // Intra-direct (Windows intra) publishes the target and keeps
                // nothing — the legacy O(1) random-access path.
                if (!m_intraDirectScrub)
                    m_gopCache.add(frameNo, entry, entry->bytes);
                if (isTarget) {
                    publishEntry(entry);
                    m_lastShown = target;
                    av_frame_unref(frame);
                    return true;
                }
            }
            av_frame_unref(frame);
        }

        if (rc == AVERROR_EOF) { m_decoderPositioned = false; return false; }
    }
    return false;
}

std::shared_ptr<DualScrubEntry>
DualScrubDecoder::makeEntry(AVFrame *frame, AVFrame *swFrame, int frameNumber)
{
    // Win/Linux dual always reads HW frames back to a planar CPU frame and
    // caches that lazily (no zero-copy GPU surface — deliberately stays off
    // the dual Vulkan compositor for NVIDIA stability). Clone is a cheap
    // refcount bump; the YUV->RGBA convert is deferred to publishEntry.
    AVFrame *src = frame;
    if (frame->format == AV_PIX_FMT_VIDEOTOOLBOX ||
        frame->format == AV_PIX_FMT_D3D11 ||
        frame->format == AV_PIX_FMT_VAAPI) {
        if (av_hwframe_transfer_data(swFrame, frame, 0) < 0) return nullptr;
        swFrame->colorspace  = frame->colorspace;
        swFrame->color_range = frame->color_range;
        src = swFrame;
    }

    AVFrame *clone = av_frame_clone(src);
    if (src == swFrame) av_frame_unref(swFrame);
    if (!clone) return nullptr;

    const int sz = av_image_get_buffer_size(
        static_cast<AVPixelFormat>(clone->format), clone->width, clone->height, 1);

    auto e = std::make_shared<DualScrubEntry>();
    e->frameNumber = frameNumber;
    e->bytes = sz > 0 ? static_cast<std::size_t>(sz)
                      : static_cast<std::size_t>(clone->width) * clone->height * 2;
    e->yuv = std::shared_ptr<AVFrame>(clone, [](AVFrame *f) { av_frame_free(&f); });
    return e;
}

void DualScrubDecoder::publishEntry(const std::shared_ptr<DualScrubEntry> &entry)
{
    if (!entry || !m_streaming) return;

    if (entry->ready) {     // pre-made DualFrame (unused on this arm)
        m_streaming->publishExternalFrame(entry->frameNumber, entry->ready);
        return;
    }

    AVFrame *yf = entry->yuv.get();
    if (!yf || !initSwsContext(yf)) return;
    QImage rgba(yf->width, yf->height, QImage::Format_RGBA8888);
    uint8_t *dst[4] = { rgba.bits(), nullptr, nullptr, nullptr };
    int dstStride[4] = { static_cast<int>(rgba.bytesPerLine()), 0, 0, 0 };
    sws_scale(m_sws, yf->data, yf->linesize, 0, yf->height, dst, dstStride);
    // rangeOverride baked into the QImage by sws (initSwsContext applied the
    // current override), so the DualFrame carries 0.
    m_streaming->publishExternalFrame(
        entry->frameNumber, makeCpuFrame(std::move(rgba), entry->frameNumber,
                                         /*rangeOverride=*/0));
}

} // namespace qcv::dual
