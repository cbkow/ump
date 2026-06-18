#include "scrub_decoder.h"
#include "decode/sws_rgba_image.h"
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
#include <libavutil/imgutils.h>
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

    // GOP-cache byte budget. Default 512 MB — comfortably holds a typical
    // mp4 GOP at 4K whether the entries are cheap planar YUV refs or
    // zero-copy GPU surfaces. User-tunable (performance/scrubCacheMB) for
    // RAM/VRAM-constrained machines.
    const std::size_t budgetMB = static_cast<std::size_t>(
        QSettings().value(QStringLiteral("performance/scrubCacheMB"), 512).toInt());
    m_gopCache.clear();
    m_gopCache.setMaxBytes(budgetMB * 1024ull * 1024ull);

    m_stopRequested.store(false, std::memory_order_release);
    m_pendingTarget.store(-1, std::memory_order_release);
    m_cacheInvalidate.store(false, std::memory_order_release);
    m_lastShown = -1;
    m_decoderPos = -1;
    m_decoderPositioned = false;
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

    // Drop cached frames BEFORE freeing the codec/hwframes contexts.
    // Cached Vulkan entries hold AVFrame refs into hw_frames_ctx and
    // Metal entries hold CVPixelBuffer retains; clearing now (the worker
    // is already joined) runs their destructors while the contexts are
    // still alive, avoiding a dangling free order. The worker is the only
    // other toucher and it's gone.
    m_gopCache.clear();

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
    m_lastShown = -1;
    m_decoderPos = -1;
    m_decoderPositioned = false;
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
    //
    // The GOP cache changes that calculus: with decode-once-per-GOP +
    // forward-no-reseek, the flush is rare (only at GOP boundaries) and
    // is always immediately followed by feeding an IDR (the backward
    // seek lands on a keyframe), which is exactly what VT needs to
    // recover. So performance/hwScrubInterCodecs lifts the gate for
    // inter codecs too — behind a flag so it's trivially revertible if
    // VT async-reorder misbehaves. Default OFF.
    m_hwInterScrub = QSettings().value(
        QStringLiteral("performance/hwScrubInterCodecs"), false).toBool();

    const AVCodecDescriptor *desc = avcodec_descriptor_get(codecpar->codec_id);
    const bool intraOnly = desc && (desc->props & AV_CODEC_PROP_INTRA_ONLY);
    const bool hwEligible = intraOnly || m_hwInterScrub;

    // Intra codecs (ProRes/DNxHD/MJPEG/raw) are random-access — every frame
    // is a keyframe, so seek-exact + decode-1 is already optimal. The GOP
    // cache + forward-fill targets inter (b-frame) codecs; for intra it only
    // adds memory + forward over-decode for no real benefit (backward is
    // already cheap). So intra takes the legacy direct-seek path on ALL
    // platforms — the b-frame rework never touches the working ProRes flow.
    m_intraDirectScrub = intraOnly;

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

    if (hwEligible && kHwType != AV_HWDEVICE_TYPE_NONE
        && !kForceSoftwareDecode) {
        if (av_hwdevice_ctx_create(&m_hwDeviceCtx, kHwType, nullptr, nullptr, 0) >= 0) {
            m_cctx->hw_device_ctx = av_buffer_ref(m_hwDeviceCtx);
            m_cctx->get_format = hwaccelGetFormat;
            if (!intraOnly) {
                qInfo("ScrubDecoder: hardware scrub enabled for inter codec "
                      "(performance/hwScrubInterCodecs)");
            }
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

void ScrubDecoder::setRangeOverride(int range)
{
    const int clamped = (range < 0) ? 0 : (range > 2 ? 0 : range);
    const int prev = m_rangeOverride.exchange(clamped, std::memory_order_acq_rel);
    if (prev != clamped) {
        // Force initSwsContext to re-apply sws_setColorspaceDetails on the
        // next decode even if dims/format are unchanged. Also nudge a
        // re-decode of the held frame so a paused scrub updates live.
        m_swsColorspaceDirty.store(true, std::memory_order_release);
        // Invalidate the GOP cache: CPU entries baked the old range into
        // their pixels and GPU entries snapshotted it. The worker clears
        // the cache before its next decode (clearing here would race the
        // worker thread, which is the cache's only other toucher).
        m_cacheInvalidate.store(true, std::memory_order_release);
        const int held = m_lastShown;
        if (held >= 0) requestFrame(held);
    }
}

bool ScrubDecoder::initSwsContext(AVFrame *frame)
{
    const bool dimsSame =
        m_sws &&
        m_swsSrcWidth  == frame->width &&
        m_swsSrcHeight == frame->height &&
        m_swsSrcFormat == frame->format;
    // Fast path: nothing changed (dims/format AND range), reuse as-is.
    if (dimsSame &&
        !m_swsColorspaceDirty.exchange(false, std::memory_order_acq_rel)) {
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
    // Either we (re)created the context, or the range override changed —
    // (re)apply the colorspace details below in both cases.

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
    // Apply the user's per-clip range override (Phase 3.G parity with
    // VideoDecoder) so scrubbed levels match playback. Auto (0) uses the
    // stream's detected color_range.
    int srcFullRange = (frame->color_range == AVCOL_RANGE_JPEG) ? 1 : 0;
    const int rangeOv = m_rangeOverride.load(std::memory_order_acquire);
    if (rangeOv == 1)      srcFullRange = 1;   // Full
    else if (rangeOv == 2) srcFullRange = 0;   // Limited
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
        if (target == m_lastShown) {
            // Already showing this frame — skip (cache hit per Guide 13 §I.7).
            continue;
        }
        // Range override changed under us — drop stale cached frames before
        // the next decode. Doing it here (worker thread) avoids racing the
        // UI thread that set the flag.
        if (m_cacheInvalidate.exchange(false, std::memory_order_acq_rel)) {
            m_gopCache.clear();
            m_decoderPositioned = false;
            m_decoderPos = -1;
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

    // 1. Cache hit — the GOP this frame belongs to was already decoded.
    //    Serve it without touching the decoder. This is what makes
    //    backward scrub and forward revisits free (the QuickTime/MPV
    //    "keep the decoded GOP" trick). get() touches LRU so the frames
    //    around the playhead stay hot.
    {
        std::shared_ptr<ScrubCacheEntry> hit;
        if (m_gopCache.get(target, hit) && hit) {
            publishEntry(hit);
            m_lastShown = target;
            return true;
        }
    }

    const int64_t targetPts = fi.ptsForFrame(target);

    // 2. Forward-no-reseek: if the decoder is already positioned just
    //    behind the target (a small forward step within reach), keep
    //    decoding forward from where it is — no seek, no flush. This
    //    turns a forward drag from O(N^2) (re-decode the GOP prefix every
    //    step) into O(N) (decode each frame once). Inter video can only be
    //    decoded forward, so this is the only way to avoid the redundant
    //    prefix re-decode.
    constexpr int kForwardReach = 90;   // ~1–2 GOPs; past this, seeking to
                                        // the nearest keyframe is cheaper
                                        // than decoding the whole gap.
    // m_intraDirectScrub (Windows + intra) forces the seek+flush path: intra
    // seek lands on the exact frame, so forward-fill would decode every
    // intermediate frame for nothing. Inter codecs keep the fast path.
    const bool canForward =
        !m_intraDirectScrub &&
        m_decoderPositioned &&
        target > m_decoderPos &&
        (target - m_decoderPos) <= kForwardReach;

    if (!canForward) {
        // 3. Cold / backward / long-jump: seek to the keyframe at-or-before
        //    the target and flush. AVSEEK_FLAG_BACKWARD lands on a keyframe
        //    (an IDR for inter codecs — exactly what VideoToolbox needs to
        //    recover after a flush); for intra codecs it's the exact frame.
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

bool ScrubDecoder::decodeForwardCaching(int target, int64_t targetPts,
                                        AVPacket *pkt, AVFrame *frame,
                                        AVFrame *swFrame)
{
    const FrameIndex &fi = m_streaming->frameIndex();

    while (!m_stopRequested.load(std::memory_order_acquire)) {
        // Note: NO mid-decode preemption check here. A continuous drag at
        // 60 Hz fires a new requestFrame every ~16 ms; if we abandoned each
        // time pendingTarget was set, we'd never complete a decode mid-drag
        // and the player would freeze until release. Let each decode finish
        // and rely on the worker loop's exchange(-1) to take the LATEST
        // pending target next iteration. Lag during fast drag = "decoder
        // can't keep up" rather than "decoder gives up".

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
            // The frame we sought/decoded forward TO is `target` by
            // construction — key it exactly, immune to any ptsForFrame/
            // frameForPts rounding mismatch (the off-by-one the intra-seek
            // bug memory warns about; for intra codecs this is the ONLY
            // frame produced). Prefix frames (inter codecs only) are keyed
            // by their decoded PTS, which the frame index maps exactly once
            // its packet scan is ready.
            const bool isTarget = (framePts >= targetPts);
            const int  frameNo  = isTarget ? target : fi.frameForPts(framePts);

            // Cache EVERY decoded frame — the GOP prefix we used to drop is
            // exactly what makes backward scrub and revisits free. The
            // decoder is now positioned right after this frame.
            std::shared_ptr<ScrubCacheEntry> entry =
                makeCacheEntry(frame, swFrame, framePts);
            m_decoderPos = frameNo;
            m_decoderPositioned = true;

            if (entry) {
                // Intra-direct (Windows intra) publishes the target straight
                // away and keeps nothing — the legacy O(1) random-access path.
                if (!m_intraDirectScrub)
                    m_gopCache.add(frameNo, entry, entry->bytes());
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

std::shared_ptr<ScrubCacheEntry>
ScrubDecoder::makeCacheEntry(AVFrame *frame, AVFrame *swFrame, int64_t framePts)
{
    // Wrap a planar frame as a cheap Yuv entry: clone (a refcount bump, no
    // pixel copy, no convert) and record its decoded byte size. The
    // YUV->RGBA swscale is deferred to publishEntry so the cold GOP fill
    // stays as cheap as the old drop-everything path.
    auto yuvEntry = [&](AVFrame *src) -> std::shared_ptr<ScrubCacheEntry> {
        AVFrame *clone = av_frame_clone(src);
        if (!clone) return nullptr;
        const int sz = av_image_get_buffer_size(
            static_cast<AVPixelFormat>(src->format), src->width, src->height, 1);
        const std::size_t bytes = sz > 0
            ? static_cast<std::size_t>(sz)
            : static_cast<std::size_t>(src->width) * src->height * 2;
        return ScrubCacheEntry::yuv(clone, framePts, bytes);
    };

    // Read a HW surface back into swFrame (a planar SW frame), then cache
    // that. Required for the fixed-pool HW backends — see below.
    auto yuvFromHw = [&]() -> std::shared_ptr<ScrubCacheEntry> {
        if (av_hwframe_transfer_data(swFrame, frame, 0) < 0) return nullptr;
        swFrame->colorspace  = frame->colorspace;
        swFrame->color_range = frame->color_range;
        auto e = yuvEntry(swFrame);
        av_frame_unref(swFrame);
        return e;
    };

    if (frame->format == AV_PIX_FMT_VIDEOTOOLBOX) {
#if defined(Q_OS_MACOS)
        // Zero-copy: hold the CVPixelBuffer. CoreVideo pools grow on
        // demand, so holding a GOP's worth of surfaces is safe (unlike the
        // fixed-size Vulkan/VAAPI decode pools below). Bounded by the
        // cache's byte budget.
        void *pix = frame->data[3];
        if (pix && cvPixelBufferIsZeroCopySupportedRaw(pix)) {
            cvPixelBufferRetainRaw(pix);    // entry takes this retain
            return ScrubCacheEntry::metal(pix, frame->width, frame->height,
                                          framePts);
        }
        // Non-biplanar VT surface — read back to a planar frame.
        return yuvFromHw();
#else
        return nullptr;
#endif
    }

    if (frame->format == AV_PIX_FMT_VULKAN ||
        frame->format == AV_PIX_FMT_D3D11 ||
        frame->format == AV_PIX_FMT_VAAPI) {
        // Deliberately read back, NOT zero-copy-held. FFmpeg's Vulkan /
        // D3D11VA / VAAPI decoders draw output frames from a fixed-size
        // hw_frames_ctx pool; holding a GOP's worth of pool frames in the
        // cache would starve the decoder and deadlock. Reading back to a
        // planar CPU frame sidesteps that. (Metal above is exempt —
        // CoreVideo pools grow.)
        return yuvFromHw();
    }

    // Software frame — already planar, clone it directly.
    return yuvEntry(frame);
}

void ScrubDecoder::publishEntry(const std::shared_ptr<ScrubCacheEntry> &entry)
{
    if (!entry || !m_streaming) return;

    // Zero-copy Metal — re-retain and publish; the compositor does YUV->RGB.
    if (entry->kind() == ScrubCacheEntry::Kind::Metal) {
        m_streaming->publishExternalFrame(entry->makeMetalHandle(),
                                          entry->pts());
        return;
    }

    // Yuv — swscale to RGBA on demand (the convert we deferred at cache
    // fill). One frame's worth, only when actually shown. Padded-destination
    // helper — a bare QImage overflows on odd widths (see sws_rgba_image.h).
    AVFrame *yf = entry->yuvFrame();
    if (!yf || !initSwsContext(yf)) return;
    QImage rgba = swsFrameToRgbaImage(m_sws, yf);
    if (rgba.isNull()) return;
    m_streaming->publishExternalFrame(
        FrameHandle::cpu(std::move(rgba), entry->pts()), entry->pts());
}

} // namespace qcv
