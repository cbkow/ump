// MacDualScrubDecoder — macOS dual-view scrub decoder (zero-copy).
//
// macOS-only counterpart to DualScrubDecoder (Windows/Linux). Same shape
// — own AVFormatContext + worker thread, latest-target-wins, PTS seek via
// the paired DualVideoDecoder's façade — but it publishes zero-copy:
// VideoToolbox hands back a CVPixelBuffer, which we wrap as a
// DualFrame::Kind::Metal (reusing DualVideoDecoder::makeMetalScrubFrame).
// The dual compositor's IDualPixbufConverter then does YUV→RGB on the GPU
// — no av_hwframe_transfer_data, no sws_scale, no re-upload. This mirrors
// the single-flow ScrubDecoder, which is the path that scrubs smoothly on
// macOS.
//
// CPU readback (sws_scale → QImage → DualFrame::Kind::Cpu) is kept only as
// a fallback for software-decoded frames or surfaces that can't be wrapped.
//
// Selected by the platform factory in src/dual/CMakeLists.txt: this file
// is compiled only on Apple; dual_scrub_decoder.cpp is compiled elsewhere.
// Both define makeDualScrubDecoder(); exactly one is linked per platform.

#include "i_dual_scrub_decoder.h"
#include "dual_video_decoder.h"
#include "dual_scrub_cache.h"       // DualScrubEntry
#include "decode/simple_lru.h"      // SimpleLRU
#include "decode/sws_rgba_image.h"  // swsFrameToRgbaImage

#include <QFileInfo>
#include <QImage>
#include <QSettings>
#include <QString>
#include <QtLogging>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

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
    for (int i = 0; fmts[i] != AV_PIX_FMT_NONE; ++i) {
        if (fmts[i] == AV_PIX_FMT_VIDEOTOOLBOX) return AV_PIX_FMT_VIDEOTOOLBOX;
    }
    return fmts[0];
}

// CPU-readback fallback frame (software decode / non-wrappable surface).
std::shared_ptr<DualFrame> makeCpuFrame(QImage rgba, int frameNumber)
{
    auto f = std::make_shared<DualFrame>();
    f->kind          = DualFrame::Kind::Cpu;
    f->width         = rgba.width();
    f->height        = rgba.height();
    f->frameNumber   = frameNumber;
    f->rangeOverride = 0;   // baked into the QImage by sws_scale
    f->rgba          = std::make_shared<QImage>(std::move(rgba));
    return f;
}

class MacDualScrubDecoder : public IDualScrubDecoder {
public:
    explicit MacDualScrubDecoder(DualVideoDecoder *streaming)
        : m_streaming(streaming) {}
    ~MacDualScrubDecoder() override { close(); }

    MacDualScrubDecoder(const MacDualScrubDecoder &)            = delete;
    MacDualScrubDecoder &operator=(const MacDualScrubDecoder &) = delete;

    bool open(const QString &path) override;
    void close() override;
    void requestFrame(int frameNo) override;

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

    AVFormatContext *m_fmt  = nullptr;
    AVCodecContext  *m_cctx = nullptr;
    AVBufferRef     *m_hwDeviceCtx = nullptr;
    int              m_videoStreamIdx = -1;

    SwsContext      *m_sws = nullptr;
    int              m_swsSrcWidth  = 0;
    int              m_swsSrcHeight = 0;
    int              m_swsSrcFormat = -1;
    int              m_swsAppliedRange = -1;  // last range override baked into m_sws

    std::atomic<int> m_pendingTarget{-1};

    // Decoded-GOP cache (mirrors single-flow ScrubDecoder). Worker-thread
    // only; keyed by frame number, byte-bounded. Holds zero-copy Metal
    // DualFrames (re-publish is free) plus a YUV fallback for software.
    SimpleLRU<int, std::shared_ptr<DualScrubEntry>> m_gopCache;
    int  m_lastShown = -1;
    int  m_decoderPos = -1;
    bool m_decoderPositioned = false;
    int  m_cachedRangeOv = -1;
    bool m_hwInterScrub = false;      // performance/hwScrubInterCodecs

    // Intra-only codec → legacy direct-seek scrub (no GOP cache, no
    // forward-fill). Intra (ProRes/DNxHD/MJPEG) is random-access — every
    // frame is a keyframe, so seek-exact + decode-1 is already optimal and
    // the GOP-cache rework (which targets inter/b-frame) only adds memory +
    // forward over-decode for no benefit. Set at open(); false for inter.
    bool m_intraDirectScrub = false;

    std::thread             m_thread;
    std::atomic<bool>       m_stopRequested{false};
    std::mutex              m_condMutex;
    std::condition_variable m_cond;
};

bool MacDualScrubDecoder::open(const QString &path)
{
    close();

    if (!QFileInfo(path).isFile()) {
        qWarning("MacDualScrubDecoder: file not found: %s", qPrintable(path));
        return false;
    }
    if (!initFFmpeg(path)) {
        teardownFFmpeg();
        return false;
    }

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

void MacDualScrubDecoder::close()
{
    m_stopRequested.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lk(m_condMutex);
    }
    m_cond.notify_one();

    if (m_thread.joinable()) m_thread.join();

    // Drop cached frames before freeing the contexts (worker is joined).
    m_gopCache.clear();

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

void MacDualScrubDecoder::requestFrame(int frameNo)
{
    if (frameNo < 0) frameNo = 0;
    m_pendingTarget.store(frameNo, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lk(m_condMutex);
    }
    m_cond.notify_one();
}

bool MacDualScrubDecoder::initFFmpeg(const QString &path)
{
    const QByteArray pathUtf8 = path.toUtf8();
    const QString trimmedName = QFileInfo(path).fileName();
    qInfo("MacDualScrubDecoder: initFFmpeg begin '%s'", qPrintable(trimmedName));

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

    // Attach VideoToolbox for intra-only codecs (ProRes et al. — the dual
    // scrub workload). Frames come back as AV_PIX_FMT_VIDEOTOOLBOX and are
    // published zero-copy. Non-intra falls to software decode + the CPU
    // fallback below. (Matches the intra gate the dual island already used;
    // the NVIDIA/Vulkan rationale behind it is moot on macOS but the gate
    // keeps the HW-session footprint small.)
    // performance/hwScrubInterCodecs lifts the intra-only gate for inter
    // (b-frame) codecs too — same flag and rationale as single-flow
    // ScrubDecoder. The GOP cache decodes each GOP once, so the per-step VT
    // session churn the gate guarded against no longer applies. Default OFF.
    m_hwInterScrub = QSettings().value(
        QStringLiteral("performance/hwScrubInterCodecs"), false).toBool();
    const AVCodecDescriptor *desc = avcodec_descriptor_get(codecpar->codec_id);
    const bool intraOnly = desc && (desc->props & AV_CODEC_PROP_INTRA_ONLY);
    // Intra → legacy direct-seek (the GOP cache targets inter/b-frame; intra
    // is already random-access). Still HW-decoded zero-copy, just not cached.
    m_intraDirectScrub = intraOnly;
    if ((intraOnly || m_hwInterScrub) &&
        av_hwdevice_ctx_create(&m_hwDeviceCtx, AV_HWDEVICE_TYPE_VIDEOTOOLBOX,
                               nullptr, nullptr, 0) >= 0) {
        m_cctx->hw_device_ctx = av_buffer_ref(m_hwDeviceCtx);
        m_cctx->get_format = hwaccelGetFormat;
    }

    if (avcodec_open2(m_cctx, codec, nullptr) < 0) return false;
    qInfo("MacDualScrubDecoder: opened '%s' (%s)", qPrintable(trimmedName),
          m_hwDeviceCtx ? "videotoolbox zero-copy" : "software");
    return true;
}

void MacDualScrubDecoder::teardownFFmpeg()
{
    if (m_sws) { sws_freeContext(m_sws); m_sws = nullptr; }
    if (m_cctx) avcodec_free_context(&m_cctx);
    if (m_fmt)  avformat_close_input(&m_fmt);
    if (m_hwDeviceCtx) av_buffer_unref(&m_hwDeviceCtx);
    m_videoStreamIdx = -1;
    m_swsSrcWidth = m_swsSrcHeight = 0;
    m_swsSrcFormat = -1;
}

bool MacDualScrubDecoder::initSwsContext(AVFrame *frame)
{
    // Range override is owned by the paired streaming decoder (the
    // controller keeps both sides in sync). Re-apply colorspace details
    // when it changes even if dims/format are unchanged, so the CPU
    // fallback's scrubbed levels match playback. (The Metal fast path
    // picks the override up via makeMetalScrubFrame and never reaches
    // here.)
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

void MacDualScrubDecoder::workerLoop()
{
    AVPacket *pkt     = av_packet_alloc();
    AVFrame  *frame   = av_frame_alloc();
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
            continue;   // already showing this frame
        }
        decodeAndPublish(target, pkt, frame, swFrame);
    }

    av_packet_free(&pkt);
    av_frame_free(&frame);
    av_frame_free(&swFrame);
}

bool MacDualScrubDecoder::decodeAndPublish(int target, AVPacket *pkt,
                                           AVFrame *frame, AVFrame *swFrame)
{
    if (!m_streaming) return false;

    // Range override changed → cached Metal frames carry a stale snapshot
    // (makeMetalScrubFrame stamps it); drop and refill.
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
    //    m_intraDirectScrub (intra codecs) forces seek+flush — intra seek
    //    lands exact, so forward-fill would decode intermediates for nothing.
    constexpr int kForwardReach = 90;
    const bool canForward =
        !m_intraDirectScrub &&
        m_decoderPositioned &&
        target > m_decoderPos &&
        (target - m_decoderPos) <= kForwardReach;

    if (!canForward) {
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

bool MacDualScrubDecoder::decodeForwardCaching(int target, int64_t targetPts,
                                               AVPacket *pkt, AVFrame *frame,
                                               AVFrame *swFrame)
{
    while (!m_stopRequested.load(std::memory_order_acquire)) {
        // No mid-decode preemption: the worker loop's exchange(-1) takes
        // the LATEST pending target on the next iteration, so fast drags
        // naturally skip intermediate values.
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
            const bool isTarget = (framePts >= targetPts);
            const int  frameNo  = isTarget
                ? target : m_streaming->frameNumberForPts(framePts);

            std::shared_ptr<DualScrubEntry> entry =
                makeEntry(frame, swFrame, frameNo);
            m_decoderPos = frameNo;
            m_decoderPositioned = true;

            if (entry) {
                // Intra-direct keeps nothing — legacy O(1) random-access path.
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
MacDualScrubDecoder::makeEntry(AVFrame *frame, AVFrame *swFrame, int frameNumber)
{
    if (frame->format == AV_PIX_FMT_VIDEOTOOLBOX) {
        // Zero-copy: wrap the CVPixelBuffer as a Metal DualFrame (the
        // compositor does YUV→RGB on the GPU). Holding it is cheap and
        // safe — CoreVideo pools grow on demand. makeMetalScrubFrame stamps
        // the current rangeOverride; range changes clear the cache above.
        if (auto metalFrame = m_streaming->makeMetalScrubFrame(frame, frameNumber)) {
            auto e = std::make_shared<DualScrubEntry>();
            e->frameNumber = frameNumber;
            e->bytes = static_cast<std::size_t>(frame->width) * frame->height * 2;
            e->ready = std::move(metalFrame);
            return e;
        }
        // Couldn't wrap the surface — read it back to a planar frame.
        if (av_hwframe_transfer_data(swFrame, frame, 0) < 0) return nullptr;
        swFrame->colorspace  = frame->colorspace;
        swFrame->color_range = frame->color_range;
        frame = swFrame;   // fall through to the YUV clone below
    }

    // Software (or VT-readback) planar frame — cheap clone, lazy convert.
    AVFrame *clone = av_frame_clone(frame);
    if (frame == swFrame) av_frame_unref(swFrame);
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

void MacDualScrubDecoder::publishEntry(const std::shared_ptr<DualScrubEntry> &entry)
{
    if (!entry || !m_streaming) return;

    if (entry->ready) {     // zero-copy Metal — re-publish directly
        m_streaming->publishExternalFrame(entry->frameNumber, entry->ready);
        return;
    }

    AVFrame *yf = entry->yuv.get();
    if (!yf || !initSwsContext(yf)) return;
    // Padded-destination helper — a bare QImage overflows on odd widths
    // (see decode/sws_rgba_image.h).
    QImage rgba = swsFrameToRgbaImage(
        m_sws, yf,
        rgbFrameNeedsLegalExpansion(
            yf, m_streaming ? m_streaming->rangeOverride() : 0));
    if (rgba.isNull()) return;
    m_streaming->publishExternalFrame(
        entry->frameNumber, makeCpuFrame(std::move(rgba), entry->frameNumber));
}

} // namespace

std::unique_ptr<IDualScrubDecoder>
makeDualScrubDecoder(DualVideoDecoder *streaming, QObject * /*parent*/)
{
    return std::make_unique<MacDualScrubDecoder>(streaming);
}

} // namespace qcv::dual
