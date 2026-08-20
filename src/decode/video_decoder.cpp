#include "video_decoder.h"

#include "decode/rgb_range.h"

#include "decoder_cleanup_queue.h"

#include <QDebug>
#include <QFileInfo>
#include <QSettings>
#include <QtLogging>
#include <algorithm>
#include <array>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
#if defined(Q_OS_WIN)
// Phase F.2.4.2 — Vulkan hwaccel on Windows. The renderer is D3D11;
// the only reason this decode unit reaches into Vulkan is the
// AVVulkanDeviceContext shared-device handoff (we hand FFmpeg the
// VkDevice owned by qcv::VulkanDeviceManager so decoded VkImages
// land on the same device the F.2.4.3 NT-handle bridge consumes).
#include <libavutil/hwcontext_vulkan.h>
#endif
}

#if defined(Q_OS_WIN)
#  include <vulkan/vulkan.h>
#  include "vulkan/vulkan_device_manager.h"
#  include "vulkan_hw_device_ctx.h"
#endif

// Defined in frame_handle.mm; lets video_decoder.cpp retain the
// CVPixelBuffer carried in frame->data[3] without including
// CoreVideo here.
extern "C" void cvPixelBufferRetainRaw(void *cvPix);
// Returns true if the CVPixelBuffer's format type matches one of
// the layouts the render-thread bridge can sample as plane MTL
// textures. Anything else needs CPU readback.
extern "C" bool cvPixelBufferIsZeroCopySupportedRaw(void *cvPix);
// Returns the OSType pixel format ('2vuy', 'v210', 'v408', etc.).
// Diagnostic-only — used by the one-shot logs in publishMetalFrame
// and the VT→CPU-fallback branch to surface what VideoToolbox is
// actually producing for an unsupported codec/format combo.
extern "C" unsigned int cvPixelBufferFormatTypeRaw(void *cvPix);

namespace qcv {

namespace {

QString avErrToString(int err)
{
    char buf[256];
    av_strerror(err, buf, sizeof(buf));
    return QString::fromUtf8(buf);
}

// Phase F.2.4.2 diagnostic — hook FFmpeg's av_log so its internal
// errors (especially from av_hwdevice_ctx_init and hwaccel discovery
// inside avcodec_open2) surface in our log. Without this, FFmpeg
// writes to stderr which Windows GUI apps discard. Lifted from
// phase-e5-end's ffmpegLogCallback.
//
// Phase I.B — also acts as the DEVICE_LOST detection point. FFmpeg
// reports Vulkan errors here long before any avcodec return value
// reaches our decode loop (the failing call may be deep inside
// libavfilter / hwaccel). Substring-match on "DEVICE_LOST" (the
// VK_ERROR_DEVICE_LOST string is always uppercase in FFmpeg's log
// output) and flip the manager's sticky flag — subsequent decoder
// opens then refuse to wrap the poisoned device.
void ffmpegLogCallback(void *avcl, int level, const char *fmt, va_list vl)
{
    if (level > AV_LOG_INFO) return;   // skip DEBUG/TRACE noise
    char buf[1024];
    int prefix = 0;
    av_log_format_line(avcl, level, fmt, vl, buf, sizeof(buf), &prefix);
    int len = static_cast<int>(std::strlen(buf));
    while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r')) buf[--len] = 0;
    if (len == 0) return;
#if defined(Q_OS_WIN)
    // Cheap substring check — runs once per FFmpeg log line, only
    // on Windows where Vulkan decode exists. macOS path uses
    // VideoToolbox and never emits this string.
    if (level <= AV_LOG_ERROR && std::strstr(buf, "DEVICE_LOST")) {
        qcv::VulkanDeviceManager::instance().markDeviceLost();
    }
#endif
    if (level <= AV_LOG_ERROR)        qCritical("ffmpeg: %s", buf);
    else if (level <= AV_LOG_WARNING) qWarning("ffmpeg: %s", buf);
    else                              qInfo("ffmpeg: %s", buf);
}

struct FfmpegLogInstaller {
    FfmpegLogInstaller() {
        av_log_set_callback(ffmpegLogCallback);
        av_log_set_level(AV_LOG_INFO);
    }
};
FfmpegLogInstaller g_ffmpegLogInstaller;

// FFmpeg pixfmt-selection callback. Invoked once per stream after
// avcodec_open2 to pick which pixel format to output. We prefer the
// hwaccel format (VideoToolbox on macOS) when available; otherwise we
// fall through to the first software format the codec offers.
//
// Set as cctx->get_format only when m_hwDeviceCtx was created
// successfully — otherwise FFmpeg uses its default selection.
AVPixelFormat hwaccelGetFormat(AVCodecContext *ctx, const AVPixelFormat *fmts)
{
    // Per-platform candidate list, tried in priority order. On Windows
    // we route per-codec: ProRes gets Vulkan (libplacebo compute is
    // the only viable hardware path), everything else gets D3D11VA
    // (well-trodden Microsoft path, sidesteps libplacebo Vulkan H.264
    // heap-corruption — see [[intel-arc-vulkan-bridge-crash]] memory
    // entry). The Vulkan pre-probe in initFFmpeg makes the
    // corresponding skipVulkan decision so the attach side agrees.
#if defined(Q_OS_MACOS)
    constexpr AVPixelFormat kPreferred[] = { AV_PIX_FMT_VIDEOTOOLBOX };
#elif defined(Q_OS_WIN)
    static constexpr AVPixelFormat kProResPreferred[] = {
        AV_PIX_FMT_VULKAN, AV_PIX_FMT_D3D11, AV_PIX_FMT_NONE
    };
    static constexpr AVPixelFormat kOtherPreferred[] = {
        AV_PIX_FMT_D3D11, AV_PIX_FMT_VULKAN, AV_PIX_FMT_NONE
    };
    const bool isProRes =
        ctx && ctx->codec && ctx->codec->id == AV_CODEC_ID_PRORES;
    const AVPixelFormat *kPreferred =
        isProRes ? kProResPreferred : kOtherPreferred;
#elif defined(Q_OS_LINUX)
    constexpr AVPixelFormat kPreferred[] = { AV_PIX_FMT_VULKAN, AV_PIX_FMT_VAAPI };
#else
    constexpr AVPixelFormat kPreferred[] = { AV_PIX_FMT_NONE };
#endif
    // F.2.4.2 diagnostic: dump the formats FFmpeg offered + which we
    // picked. One-shot per AVCodecContext (cheap heuristic via a
    // static set keyed by ctx pointer would be overkill; the codec
    // calls get_format on every reconfigure, which is rare in
    // practice, so this is fine to log on every call).
    QString offered;
    for (int i = 0; fmts[i] != AV_PIX_FMT_NONE; ++i) {
        const char *n = av_get_pix_fmt_name(fmts[i]);
        if (i > 0) offered += QLatin1Char(',');
        offered += QString::fromUtf8(n ? n : "?");
    }

    // Sentinel-terminated iteration so the loop works whether
    // kPreferred is a fixed-size array (Mac / Linux / fallback)
    // or a pointer to one of two static arrays (Windows
    // per-codec routing). Either way we stop at the first
    // AV_PIX_FMT_NONE.
    for (const AVPixelFormat *p = kPreferred; *p != AV_PIX_FMT_NONE; ++p) {
        const AVPixelFormat want = *p;
        for (int i = 0; fmts[i] != AV_PIX_FMT_NONE; ++i) {
            if (fmts[i] == want) {
                qInfo("VideoDecoder: get_format codec=%s offered=[%s] picked=%s",
                      ctx && ctx->codec ? ctx->codec->name : "?",
                      qPrintable(offered),
                      av_get_pix_fmt_name(want));
                // Pool sizing + alpha feasibility is decided upfront
                // in initFFmpeg via the Vulkan hwframes probe — see
                // the "Unified pre-probe" block there. Nothing else
                // to do here: return the picked hw pix_fmt.
                return want;
            }
        }
    }
    qInfo("VideoDecoder: get_format codec=%s offered=[%s] picked=%s (SW fallback)",
          ctx && ctx->codec ? ctx->codec->name : "?",
          qPrintable(offered),
          av_get_pix_fmt_name(fmts[0]));
    // Codec doesn't support our hwaccel; let FFmpeg use the first SW fmt.
    return fmts[0];
}

// Phase F.2.12.a — createSharedVulkanHwDeviceCtx moved to
// src/decode/vulkan_hw_device_ctx.{h,cpp} so DualVideoDecoder can
// reuse the same helper without duplicating Vulkan-handoff logic.

} // namespace

VideoDecoder::VideoDecoder(QObject *parent)
    : QObject(parent)
{
}

VideoDecoder::~VideoDecoder()
{
    close();
}

bool VideoDecoder::open(const QString &path)
{
    // Suppress close()'s sourcePathChanged("") emit for the duration
    // of the internal close. Without this gate every playlist
    // boundary cross fires the downstream lambda twice — once on
    // the empty-path emit (triggers audio close + probe + decoder
    // teardown), then again on the new-path emit (triggers audio
    // open + new decoder construction). Doubles per-click cost on
    // multi-stream broadcast files. The true close() path (called
    // directly by an owner, with no follow-up open()) still emits
    // since m_reopening stays false there.
    m_reopening = true;
    close();
    m_reopening = false;

    if (!QFileInfo(path).isFile()) {
        setError(tr("File not found: %1").arg(path));
        setState(Errored);
        // close() suppressed its empty-path emit because we promised
        // we'd reopen; if the new path can't even be reached we have
        // to fire it now so audio + scrub tear down. Without this,
        // downstream subsystems stay bound to the previous file.
        emit sourcePathChanged();
        return false;
    }

    m_sourcePath = path;
    emit sourcePathChanged();
    setState(Opening);

    if (!initFFmpeg(path)) {
        teardownFFmpeg();
        setState(Errored);
        return false;
    }

    emit metadataChanged();

    m_stopRequested.store(false, std::memory_order_release);
    m_publishedSeq.store(0, std::memory_order_release);
    m_lastFetchedSeq = 0;
    m_paceBaselineSet = false;
    m_loggedMetalFormat     = false;
    m_loggedCpuFormat       = false;
    m_loggedHwToCpuFallback = false;
    m_loggedVulkanFormat    = false;
    // Open paused. Autoplay-on-open was disorienting in QC review
    // workflows where the user wants to scrub a freshly-loaded clip
    // before pressing Space. Callers (load, drop, project click)
    // explicitly call play() if they want playback to start.
    if (m_isPlaying.exchange(false, std::memory_order_acq_rel)) {
        emit isPlayingChanged();
    }

    m_thread = std::thread([this] { decodeLoop(); });
    setState(Decoding);
    return true;
}

void VideoDecoder::seekToFrame(int frameNo)
{
    if (!m_frameIndex.isValid()) return;
    const int total = m_frameIndex.totalFrames();
    if (frameNo < 0) frameNo = 0;
    if (total > 0 && frameNo >= total) frameNo = total - 1;

    m_pendingSeekTarget.store(frameNo, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lk(m_seekCondMutex);
    }
    m_seekCond.notify_one();
}

void VideoDecoder::play()
{
    if (m_isPlaying.exchange(true, std::memory_order_acq_rel)) return;
    m_paceBaselineSet = false;
    {
        std::lock_guard<std::mutex> lk(m_seekCondMutex);
    }
    m_seekCond.notify_one();
    emit isPlayingChanged();
}

void VideoDecoder::pause()
{
    if (!m_isPlaying.exchange(false, std::memory_order_acq_rel)) return;
    emit isPlayingChanged();
}

void VideoDecoder::togglePlayback()
{
    if (isPlaying()) pause();
    else play();
}

void VideoDecoder::setPlaybackSpeed(double speed)
{
    // Same clamp range as PlaybackTimer::setPlaybackSpeed.
    if (speed < 0.0625) speed = 0.0625;
    if (speed > 8.0)    speed = 8.0;
    if (m_playbackSpeed.exchange(speed, std::memory_order_acq_rel) == speed) {
        return;
    }
    m_paceSpeedDirty.store(true, std::memory_order_release);
}

void VideoDecoder::clearErrorState()
{
    if (state() == Errored) {
        setState(Idle);
    }
}

void VideoDecoder::close()
{
    m_stopRequested.store(true, std::memory_order_release);
    m_isPlaying.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lk(m_seekCondMutex);
    }
    m_seekCond.notify_one();

    // Synchronous join — the decode loop reads m_cctx / m_fmt as
    // members each iteration, so we MUST be sure it has exited
    // before we touch those pointers. The thread responds to the
    // stop flag fast (it only completes whatever FFmpeg call is
    // already in flight), so this typically takes 1–10 ms.
    if (m_thread.joinable()) {
        m_thread.join();
    }

#if defined(Q_OS_WIN)
    // Flush in-flight Vulkan work before handing the hwdevice context
    // to the async cleanup queue. Without this, the renderer-held
    // FrameHandle's AVVkFrame may still be GPU-active when the cleanup
    // thread later av_buffer_unref's the context, causing an
    // intermittent device-lost on the next decoder open (Intel Arc).
    // No-op when the singleton wasn't initialized (CPU-only fallback).
    VulkanDeviceManager::instance().waitForGpu();
#endif

    // Async-free the FFmpeg contexts. The slow part —
    // av_buffer_unref on the VideoToolbox hwframes_ctx — runs on
    // the cleanup queue thread, so the UI doesn't pay the ~50 ms
    // session-teardown cost.
    //
    // Phase I.A — on Windows, when this close() is the internal one
    // inside open() (m_reopening=true), KEEP m_hwDeviceCtx alive so
    // the immediately-following initFFmpeg can reuse it rather than
    // allocate a fresh AVHWDeviceContext against the same VkDevice.
    // Per-clip transitions in a playlist used to stack 4+ pending
    // hwdevice teardowns in the cleanup queue while new ones were
    // allocated — Intel Arc lost the VkDevice under that contention
    // (memory: vulkan-cleanup-queue-saturation). Cached reuse cuts
    // the per-boundary work to ~just the codec + format contexts.
    // True closes (m_reopening=false: ~VideoDecoder, app shutdown,
    // direct external close()) still post to the cleanup queue.
    AVFormatContext *fmt    = m_fmt;          m_fmt = nullptr;
    AVCodecContext  *cctx   = m_cctx;         m_cctx = nullptr;
    SwsContext      *sws    = m_sws;          m_sws = nullptr;
    AVBufferRef     *hwDev  = nullptr;
#if defined(Q_OS_WIN)
    if (!m_reopening) {
        hwDev = m_hwDeviceCtx;
        m_hwDeviceCtx = nullptr;
    }
    // else: keep m_hwDeviceCtx — initFFmpeg will reuse it.
#else
    hwDev = m_hwDeviceCtx;
    m_hwDeviceCtx = nullptr;
#endif
    postFFmpegCleanup(fmt, cctx, sws, hwDev);

    m_videoStreamIdx = -1;
    m_swsSrcWidth = m_swsSrcHeight = 0;
    m_swsSrcFormat = -1;
    m_hwAccelType.clear();

    {
        std::lock_guard<std::mutex> lk(m_publishMutex);
        m_publishedFrame.reset();
    }
    m_publishedSeq.store(0, std::memory_order_release);
    m_lastFetchedSeq = 0;
    m_pendingSeekTarget.store(-1, std::memory_order_release);
    m_currentFrame.store(-1, std::memory_order_release);
    m_frameIndex = FrameIndex();

    if (m_decodeErrorCount.exchange(0, std::memory_order_acq_rel) != 0) {
        std::lock_guard<std::mutex> lk(m_lastErrorMutex);
        m_lastDecodeError.clear();
        emit decodeHealthChanged();
    }

    // Cancel any in-flight tier-2 packet scan and join its thread.
    // The scan owns its own AVFormatContext (independent of the
    // decoder's), so this is just thread join + free.
    m_frameIndex.cancelScan();

    // Always clear member state + emit so downstream subsystems
    // (AudioPlayer, ScrubDecoder, timeline) tear down cleanly. The
    // Errored→Idle transition is intentionally suppressed so the
    // error stays visible for diagnostics; data clearing has to
    // happen regardless or close() becomes a no-op when the prior
    // open() left us in Errored (audio-only files fail initFFmpeg
    // → Errored → close was silently leaving sourcePath set + the
    // sourcePathChanged signal AudioPlayer needs never fired).
    //
    // sourcePathChanged is suppressed when m_reopening is set —
    // open() will fire its own emit shortly with the new path, so
    // downstream subsystems see one close+open cycle instead of two
    // (the empty-path emit + the new-path emit). Cuts the per-
    // playlist-boundary audio teardown cost in half on multi-stream
    // files. metadataChanged + startTimecodeChanged still fire so
    // QML readouts blank between clips even if briefly.
    if (!m_sourcePath.isEmpty()) {
        m_sourcePath.clear();
        m_width = m_height = m_frameCount = 0;
        m_codecName.clear();
        m_pixelFormat.clear();
        m_sourceTimecode.clear();
        m_isDropFrame = false;
        m_sourceStartFrame = -1;
        m_tcFormatter = TimecodeFormatter();
        m_startTimecodeString.clear();
        m_currentStartFrame.store(-1, std::memory_order_release);
        if (!m_reopening) emit sourcePathChanged();
        emit metadataChanged();
        emit startTimecodeChanged();
    }
    if (state() != Errored) {
        setState(Idle);
    }
}

bool VideoDecoder::fetchLatest(FrameHandle *out)
{
    const uint64_t latest = m_publishedSeq.load(std::memory_order_acquire);
    if (latest == m_lastFetchedSeq) {
        return false;
    }

    std::lock_guard<std::mutex> lk(m_publishMutex);
    if (!m_publishedFrame.isValid()) {
        m_lastFetchedSeq = latest;
        return false;
    }
    if (out) {
        *out = std::move(m_publishedFrame);   // moves ownership to caller
    } else {
        m_publishedFrame.reset();
    }
    m_lastFetchedSeq = latest;
    return true;
}

void VideoDecoder::setState(State s)
{
    if (m_state.exchange(s, std::memory_order_acq_rel) != s) {
        emit stateChanged();
    }
}

void VideoDecoder::setError(const QString &msg)
{
    m_lastError = msg;
    qWarning("VideoDecoder error: %s", qPrintable(msg));
    emit errorOccurred(msg);
}

QString VideoDecoder::lastDecodeError() const
{
    std::lock_guard<std::mutex> lk(m_lastErrorMutex);
    return m_lastDecodeError;
}

QString VideoDecoder::formatTimecode(int frameNo) const
{
    if (!m_tcFormatter.isValid()) return QString();
    // Internal frame numbers start at 0 ("first decoded frame");
    // SMPTE display adds the user-picked start offset (Inspector's
    // Timecodes source picker) — defaults to the FFmpeg-detected
    // source TC, so a tmcd "01:00:00:00" shows that on frame 0.
    // -1 ⇒ "From start" (no offset, origin = 00:00:00:00).
    const int startFrame =
        m_currentStartFrame.load(std::memory_order_acquire);
    const int absFrame = (startFrame >= 0) ? frameNo + startFrame : frameNo;
    return m_tcFormatter.format(absFrame);
}

int VideoDecoder::parseTimecode(const QString &tc) const
{
    if (!m_tcFormatter.isValid()) return -1;
    const int absFrame = m_tcFormatter.parse(tc);
    if (absFrame < 0) return -1;
    const int startFrame =
        m_currentStartFrame.load(std::memory_order_acquire);
    const int frame = (startFrame >= 0) ? absFrame - startFrame : absFrame;
    return frame >= 0 ? frame : 0;
}

void VideoDecoder::setStartTimecode(const QString &tc)
{
    if (tc == m_startTimecodeString) return;
    m_startTimecodeString = tc;
    if (tc.isEmpty()) {
        m_currentStartFrame.store(-1, std::memory_order_release);
    } else if (m_tcFormatter.isValid()) {
        const int frame = m_tcFormatter.parse(tc);
        m_currentStartFrame.store(frame >= 0 ? frame : -1,
                                   std::memory_order_release);
    } else {
        m_currentStartFrame.store(-1, std::memory_order_release);
    }
    emit startTimecodeChanged();
}

void VideoDecoder::setRangeOverride(int range)
{
    const int clamped = (range == 1 || range == 2) ? range : 0;
    const int prev = m_rangeOverride.exchange(clamped,
                                              std::memory_order_acq_rel);
    if (prev == clamped) return;

    // Force the next initSwsContext() pass to rebuild the colorspace
    // details with the new srcFullRange. The cached-context check
    // keys off width / height / format only, so we invalidate by
    // zeroing the width — the comparison fails and the rebuild path
    // calls sws_setColorspaceDetails again. Touched only here +
    // initSwsContext (decode thread); the brief window where the
    // decode thread might race is harmless because both writers
    // converge on the same final value within one frame.
    m_swsSrcWidth = 0;

    // Re-decode the current frame so the visible image updates
    // immediately, even when paused. The seek path goes through
    // performSeek() on the decode thread; we reuse the existing
    // pendingSeekTarget channel.
    const int cur = m_currentFrame.load(std::memory_order_acquire);
    if (cur >= 0) {
        m_pendingSeekTarget.store(cur, std::memory_order_release);
        std::lock_guard<std::mutex> lk(m_seekCondMutex);
        m_seekCond.notify_all();
    }
}

void VideoDecoder::recordDecodeError(const QString &msg)
{
    m_decodeErrorCount.fetch_add(1, std::memory_order_acq_rel);
    {
        std::lock_guard<std::mutex> lk(m_lastErrorMutex);
        m_lastDecodeError = msg;
    }
    qWarning("VideoDecoder: %s", qPrintable(msg));
    emit decodeHealthChanged();
}

bool VideoDecoder::initFFmpeg(const QString &path)
{
    const QByteArray pathUtf8 = path.toUtf8();
    // TRACE_VIDEO_OPEN — start of the playback decoder's
    // avformat_open_input. Brackets the find_stream_info that may
    // race with the metadata extractor + audio probe on the same
    // file in cold File > Open.
    const QString trimmedName = QFileInfo(path).fileName();
    qInfo("VideoDecoder: initFFmpeg begin '%s'", qPrintable(trimmedName));

    if (int err = avformat_open_input(&m_fmt, pathUtf8.constData(), nullptr, nullptr); err < 0) {
        setError(tr("avformat_open_input failed: %1").arg(avErrToString(err)));
        return false;
    }
    qInfo("VideoDecoder: avformat_open_input done '%s'", qPrintable(trimmedName));
    if (int err = avformat_find_stream_info(m_fmt, nullptr); err < 0) {
        setError(tr("avformat_find_stream_info failed: %1").arg(avErrToString(err)));
        return false;
    }
    qInfo("VideoDecoder: find_stream_info done '%s'", qPrintable(trimmedName));

    for (unsigned i = 0; i < m_fmt->nb_streams; ++i) {
        if (m_fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            m_videoStreamIdx = static_cast<int>(i);
            break;
        }
    }
    if (m_videoStreamIdx < 0) {
        setError(tr("No video stream found"));
        return false;
    }

    AVStream *st = m_fmt->streams[m_videoStreamIdx];
    AVCodecParameters *codecpar = st->codecpar;
    const AVCodec *codec = avcodec_find_decoder(codecpar->codec_id);
    if (!codec) {
        setError(tr("No decoder available for codec id %1").arg(codecpar->codec_id));
        return false;
    }

    m_cctx = avcodec_alloc_context3(codec);
    if (!m_cctx) {
        setError(tr("avcodec_alloc_context3 failed"));
        return false;
    }
    avcodec_parameters_to_context(m_cctx, codecpar);

    // F.2.4.2 diagnostic — dump the hwaccels FFmpeg has registered
    // for this codec. If a codec has NO hwaccel hw_configs (e.g.,
    // vcpkg's FFmpeg without prores_vulkan compiled in), it goes
    // straight to software decode and never calls our get_format
    // callback — and that's silent without this probe.
    {
        QString hwList;
        for (int i = 0;; ++i) {
            const AVCodecHWConfig *hwc = avcodec_get_hw_config(codec, i);
            if (!hwc) break;
            if (!hwList.isEmpty()) hwList += QLatin1Char(' ');
            const char *pixName = av_get_pix_fmt_name(hwc->pix_fmt);
            const char *devName = av_hwdevice_get_type_name(hwc->device_type);
            hwList += QStringLiteral("%1/%2:0x%3")
                          .arg(QString::fromUtf8(devName ? devName : "?"))
                          .arg(QString::fromUtf8(pixName ? pixName : "?"))
                          .arg(hwc->methods, 0, 16);
        }
        qInfo("VideoDecoder: codec=%s hw_configs=[%s]",
              codec->name,
              hwList.isEmpty() ? "<none>" : qPrintable(hwList));
    }
    // Phase 1.8.2a: try to attach the platform hwaccel device. If the
    // codec doesn't actually support hwaccel for this stream, FFmpeg's
    // get_format callback will fall through to a software pixfmt and
    // we still get a working software decode. So this is essentially
    // a free win when it works and silent when it doesn't.
#if defined(Q_OS_MACOS)
    constexpr auto kHwType = AV_HWDEVICE_TYPE_VIDEOTOOLBOX;
    const char *kHwName = "videotoolbox";
#elif defined(Q_OS_WIN)
    // Phase F.2.4.2 — try Vulkan via OUR shared VkDevice first; fall
    // back to D3D11VA (CPU readback path) if shared-Vulkan init fails.
    constexpr auto kHwType = AV_HWDEVICE_TYPE_D3D11VA;
    const char *kHwName = "d3d11va";
#elif defined(Q_OS_LINUX)
    constexpr auto kHwType = AV_HWDEVICE_TYPE_VAAPI;
    const char *kHwName = "vaapi";
#else
    constexpr auto kHwType = AV_HWDEVICE_TYPE_NONE;
    const char *kHwName = "";
#endif

#if defined(Q_OS_WIN)
    // Per-codec hwaccel routing on Windows: only ProRes routes to
    // Vulkan (libplacebo's compute decoder is the only viable
    // hardware path for ProRes). H.264 / H.265 / VP9 / AV1 / etc.
    // route to D3D11VA — the well-trodden Microsoft path that's
    // been stable for years, and that sidesteps a libplacebo
    // Vulkan-H.264 heap-corruption bug we hit on certain 2008-era
    // QuickTime files (see [[intel-arc-vulkan-bridge-crash]] memory
    // entry — same crash appears in latest BtbN n8.1.1 daily, so
    // it's not just our local FFmpeg build).
    //
    // We do this by setting skipVulkan=true upfront for non-ProRes,
    // which skips the Vulkan pre-probe + attach below, and the
    // existing fallback `av_hwdevice_ctx_create(D3D11VA)` branch
    // takes over.
    //
    // performance/hardwareDecodeEnabled user setting (Windows-only
    // toggle in Settings panel, defaults ON). When the user turns
    // it OFF — escape hatch for mini-PCs with broken GPU drivers —
    // we force software decode for everything. macOS is unaffected
    // (VideoToolbox is rock-solid and always used). Implemented by
    // setting BOTH skipVulkan + a skipAllHw flag that gates the
    // D3D11VA branch below.
    const bool kHwDecodeEnabled = QSettings().value(
        QStringLiteral("performance/hardwareDecodeEnabled"), true).toBool();
    const bool kForceSoftwareDecode = !kHwDecodeEnabled;
    const bool kIsProRes =
        codecpar && codecpar->codec_id == AV_CODEC_ID_PRORES;
    bool skipVulkan = !kIsProRes || kForceSoftwareDecode;
    bool skipAllHw  = kForceSoftwareDecode;
    if (kForceSoftwareDecode) {
        qInfo("VideoDecoder: software decode forced — "
              "performance/hardwareDecodeEnabled is off");
    } else if (skipVulkan) {
        qInfo("VideoDecoder: codec=%s → routing to D3D11VA (Vulkan "
              "reserved for ProRes on Windows)",
              avcodec_get_name(codecpar ? codecpar->codec_id : AV_CODEC_ID_NONE));
    }

    // Unified pre-probe for the Vulkan hwaccel path: attempt to
    // allocate a small hwframes pool upfront for the SW pixfmt the
    // codec will emit. The probe is purely a feasibility test —
    // we DON'T attach the probed context to the codec; we discard
    // it and let FFmpeg manage its own pool sizing (libplacebo's
    // Vulkan decoder has internal expectations about pool size that
    // it sets via its own avcodec_get_hw_frames_parameters path,
    // and pre-empting that hangs the decoder).
    //
    // If the probe succeeds, the driver can allocate the format —
    // FFmpeg's later default-pool allocation should too.
    // If the probe fails (e.g., Intel Arc + 4-plane YUVA),
    // FFmpeg's default would also fail mid-stream and assert; we
    // skip Vulkan cleanly upfront, CPU path takes over.
    AVPixelFormat probedSwFmt =
        (codecpar && !skipVulkan)
            ? static_cast<AVPixelFormat>(codecpar->format)
            : AV_PIX_FMT_NONE;

    // Phase I.B — drop a cached device that survived a DEVICE_LOST
    // event in a prior decode iteration but wasn't released yet.
    // (Could happen if open() runs before the decode loop's next
    // poll iteration sees the flag.) Without this we'd try to
    // attach a poisoned device → first decode submission would
    // immediately fail. Cleaner to refuse here.
    if (m_hwDeviceCtx && VulkanDeviceManager::instance().isDeviceLost()) {
        releaseCachedHwDevice();
    }
    if (probedSwFmt != AV_PIX_FMT_NONE) {
        // Phase I.A — reuse the cached hwdevice if we're in the
        // close-and-reopen cycle (close() left it alive). For first
        // open or after a true close, allocate a fresh one. The
        // probe below validates THIS clip's format/dim against the
        // device — codec change between clips is fine because the
        // device context is codec-agnostic.
        const bool reuseCachedDevice = (m_hwDeviceCtx != nullptr);
        AVBufferRef *deviceProbe = reuseCachedDevice
            ? av_buffer_ref(m_hwDeviceCtx)
            : createSharedVulkanHwDeviceCtx();
        if (deviceProbe) {
            AVBufferRef *framesRef = av_hwframe_ctx_alloc(deviceProbe);
            if (framesRef) {
                auto *fctx = reinterpret_cast<AVHWFramesContext *>(framesRef->data);
                fctx->format    = AV_PIX_FMT_VULKAN;
                fctx->sw_format = probedSwFmt;
                fctx->width     = codecpar->width;
                fctx->height    = codecpar->height;
                fctx->initial_pool_size = 4;   // small — just a probe
                const int initResult = av_hwframe_ctx_init(framesRef);
                av_buffer_unref(&framesRef);   // discard either way
                if (initResult == 0) {
                    if (reuseCachedDevice) {
                        // m_hwDeviceCtx already holds a ref; drop
                        // the extra ref the av_buffer_ref above made.
                        av_buffer_unref(&deviceProbe);
                    } else {
                        m_hwDeviceCtx = deviceProbe;
                    }
                    qInfo("VideoDecoder: Vulkan hwframes probe OK for %s "
                          "%dx%d — will use Vulkan with FFmpeg-managed pool%s",
                          av_get_pix_fmt_name(probedSwFmt),
                          codecpar->width, codecpar->height,
                          reuseCachedDevice ? " (cached device)" : "");
                } else {
                    // Probe failed. Drop the local ref; keep cached
                    // m_hwDeviceCtx alive — next clip may accept this
                    // format. Mark skipVulkan so this clip falls
                    // back to CPU but doesn't poison the cache.
                    av_buffer_unref(&deviceProbe);
                    skipVulkan = true;
                    qInfo("VideoDecoder: Vulkan hwframes probe REJECTED for %s "
                          "%dx%d — driver can't allocate this format; "
                          "falling back to CPU decode",
                          av_get_pix_fmt_name(probedSwFmt),
                          codecpar->width, codecpar->height);
                }
            } else {
                av_buffer_unref(&deviceProbe);
                skipVulkan = true;
            }
        } else {
            skipVulkan = true;  // Vulkan device init failed entirely
        }
    }

    // Attach the right hwaccel to the codec context. Three-way
    // priority:
    //   1. Vulkan via our shared device — when we WANT Vulkan for
    //      this codec (ProRes; libplacebo compute path) AND the
    //      shared device + hwframes pre-probe succeeded.
    //   2. D3D11VA via FFmpeg's own device — when we don't want
    //      Vulkan for this codec (non-ProRes per-codec routing) OR
    //      Vulkan device init failed. This is the well-trodden
    //      Microsoft path; works on every modern Windows + GPU.
    //   3. Software decode — when D3D11VA also can't open (rare;
    //      typically only on mini-PCs with broken GPU drivers).
    if (m_hwDeviceCtx && !skipVulkan) {
        m_cctx->hw_device_ctx = av_buffer_ref(m_hwDeviceCtx);
        m_cctx->get_format    = hwaccelGetFormat;
        m_hwAccelType         = QStringLiteral("vulkan");
        qInfo("VideoDecoder: vulkan hwaccel attached (FFmpeg-managed pool)");
    } else if (!skipAllHw
               && av_hwdevice_ctx_create(&m_hwDeviceCtx, kHwType, nullptr, nullptr, 0) >= 0) {
        m_cctx->hw_device_ctx = av_buffer_ref(m_hwDeviceCtx);
        m_cctx->get_format = hwaccelGetFormat;
        m_hwAccelType = QString::fromUtf8(kHwName);
        qInfo("VideoDecoder: %s hwaccel attached%s", kHwName,
              skipVulkan ? " (codec-routed)"
                         : " (Vulkan shared-device unavailable)");
    } else {
        qInfo("VideoDecoder: no hwaccel available; software decode%s",
              kForceSoftwareDecode ? " (forced by user setting)" : "");
    }
#else
    if (kHwType != AV_HWDEVICE_TYPE_NONE) {
        if (av_hwdevice_ctx_create(&m_hwDeviceCtx, kHwType, nullptr, nullptr, 0) >= 0) {
            m_cctx->hw_device_ctx = av_buffer_ref(m_hwDeviceCtx);
            m_cctx->get_format = hwaccelGetFormat;
            m_hwAccelType = QString::fromUtf8(kHwName);
            qInfo("VideoDecoder: %s hwaccel attached", kHwName);
        } else {
            qInfo("VideoDecoder: %s hwaccel unavailable; software decode", kHwName);
        }
    }
#endif

    // Phase 7.6 — software-decode threading. FFmpeg's silent
    // default is thread_count=1 (single-threaded) when neither
    // thread_count nor thread_type is set; explicitly setting 0
    // = auto-detect plus FF_THREAD_FRAME|FF_THREAD_SLICE engages
    // proper multithreading on the software fallback path. Auto
    // is the default; user override comes from Settings via
    // QSettings("performance/ffmpegThreads").
    {
        QSettings s;
        m_cctx->thread_count =
            s.value(QStringLiteral("performance/ffmpegThreads"), 0).toInt();
        m_cctx->thread_type  = FF_THREAD_FRAME | FF_THREAD_SLICE;
    }
    if (int err = avcodec_open2(m_cctx, codec, nullptr); err < 0) {
        setError(tr("avcodec_open2 failed: %1").arg(avErrToString(err)));
        return false;
    }

    m_width      = codecpar->width;
    m_height     = codecpar->height;
    m_frameCount = static_cast<int>(st->nb_frames);
    m_codecName  = QString::fromUtf8(codec->long_name ? codec->long_name : codec->name);
    if (const char *pn = av_get_pix_fmt_name(static_cast<AVPixelFormat>(codecpar->format))) {
        m_pixelFormat = QString::fromUtf8(pn);
    }

    // Source timecode — read from FFmpeg's per-stream metadata, which
    // covers MOV `tmcd` tracks, MP4 timecode atoms, and MXF start
    // timecode (FFmpeg normalizes them all to a "timecode" tag).
    // Format: "HH:MM:SS:FF" non-drop, "HH:MM:SS;FF" for SMPTE
    // drop-frame. The `;` separator is the standard DF signaler;
    // FFmpeg honors the MOV tmcd flag when emitting it. Phase
    // 1.8.7 (TimecodeFormatter) computes the DF-aware start frame;
    // for now we just store the string + the DF flag.
    if (AVDictionaryEntry *tcEntry = av_dict_get(st->metadata, "timecode",
                                                  nullptr, 0)) {
        m_sourceTimecode = QString::fromUtf8(tcEntry->value);
        m_isDropFrame    = m_sourceTimecode.contains(QLatin1Char(';'));
    }

    // FrameIndex — formula-based for Phase 1.8.3. Uses the stream's
    // time_base + frame_rate as the source of truth. Total frames
    // falls back to duration*fps when the container doesn't store it.
    const AVRational tb = st->time_base;
    const AVRational fr = av_guess_frame_rate(m_fmt, st, nullptr);
    const AVCodecDescriptor *desc = avcodec_descriptor_get(codecpar->codec_id);
    const bool intraOnly = desc && (desc->props & AV_CODEC_PROP_INTRA_ONLY);
    int total = m_frameCount;
    if (total <= 0 && fr.num > 0 && fr.den > 0 && m_fmt->duration > 0) {
        total = static_cast<int>(av_rescale_q(m_fmt->duration,
                                              { 1, AV_TIME_BASE },
                                              { fr.den, fr.num }));
    }
    m_frameIndex = FrameIndex(tb.num, tb.den, fr.num, fr.den, total, intraOnly);
    if (total > 0) m_frameCount = total;

    // Build the SMPTE timecode formatter (drop-frame aware) and
    // resolve the source's start-frame offset if a timecode tag was
    // present. Done here, after the frame_rate is known.
    m_tcFormatter = TimecodeFormatter(fr.num, fr.den, m_isDropFrame);
    if (!m_sourceTimecode.isEmpty()) {
        const int startFrame = m_tcFormatter.parse(m_sourceTimecode);
        if (startFrame >= 0) m_sourceStartFrame = startFrame;
    }
    // Default the user-pickable origin to the FFmpeg-detected source TC.
    // Inspector's Timecodes section can switch to a different source
    // (QT Start, XMP Alt, or "From start") at any time.
    m_startTimecodeString = m_sourceTimecode;
    m_currentStartFrame.store(m_sourceStartFrame, std::memory_order_release);
    emit startTimecodeChanged();

    // Tier-2 packet scan — kick off a background thread that walks
    // the entire file to build an exact (pts, isKeyframe) table.
    // Runs in parallel with playback; lookups fall through to the
    // formula until the scan completes. Cancelled in close().
    m_frameIndex.startScan(path, m_videoStreamIdx);

    return true;
}

void VideoDecoder::teardownFFmpeg()
{
    if (m_sws) {
        sws_freeContext(m_sws);
        m_sws = nullptr;
    }
    if (m_cctx) {
        avcodec_free_context(&m_cctx);
    }
    if (m_fmt) {
        avformat_close_input(&m_fmt);
    }
    if (m_hwDeviceCtx) {
        av_buffer_unref(&m_hwDeviceCtx);
    }
    m_videoStreamIdx = -1;
    m_swsSrcWidth = m_swsSrcHeight = 0;
    m_swsSrcFormat = -1;
    m_hwAccelType.clear();
}

void VideoDecoder::releaseCachedHwDevice()
{
    // Synchronous unref of the cached hwdevice — drops our ref;
    // the underlying AVHWDeviceContext frees when the refcount
    // hits 0 (no other holder usually, since codec contexts that
    // had taken refs via av_buffer_ref were already freed via the
    // cleanup queue by the time we get here on the reopen path).
    // No-op when there's nothing cached.
    if (m_hwDeviceCtx) {
        av_buffer_unref(&m_hwDeviceCtx);
    }
}

bool VideoDecoder::initSwsContext(AVFrame *frame)
{
    // Re-create if first init or any source params changed.
    if (m_sws &&
        m_swsSrcWidth  == frame->width &&
        m_swsSrcHeight == frame->height &&
        m_swsSrcFormat == frame->format) {
        return true;
    }
    if (m_sws) {
        sws_freeContext(m_sws);
        m_sws = nullptr;
    }
    m_sws = sws_getContext(
        frame->width, frame->height,
        static_cast<AVPixelFormat>(frame->format),
        frame->width, frame->height,
        AV_PIX_FMT_RGBA,
        SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!m_sws) {
        setError(tr("sws_getContext failed"));
        return false;
    }

    // Pick the matrix that matches the source colorspace metadata, not
    // swscale's BT.601 default. Wrong matrix = "crunched" colors —
    // saturated greens shift toward cyan, oranges go pink, shadows
    // milky. (Phase 2 will replace this CPU path with proper OCIO
    // input transforms; for Phase 1.8.1 we just pick the right matrix
    // and let macOS handle BT.709 → sRGB-display approximately.)
    int srcCsp;
    switch (frame->colorspace) {
        case AVCOL_SPC_BT709:      srcCsp = SWS_CS_ITU709; break;
        case AVCOL_SPC_BT470BG:    srcCsp = SWS_CS_ITU601; break;
        case AVCOL_SPC_SMPTE170M:  srcCsp = SWS_CS_SMPTE170M; break;
        case AVCOL_SPC_SMPTE240M:  srcCsp = SWS_CS_SMPTE240M; break;
        case AVCOL_SPC_FCC:        srcCsp = SWS_CS_FCC; break;
        case AVCOL_SPC_BT2020_NCL: srcCsp = SWS_CS_BT2020; break;
        case AVCOL_SPC_BT2020_CL:  srcCsp = SWS_CS_BT2020; break;
        default:
            // Heuristic: HD content is BT.709, SD is BT.601. Matches
            // what most video tools do when metadata is missing.
            srcCsp = (frame->width >= 1280 || frame->height >= 720)
                     ? SWS_CS_ITU709 : SWS_CS_SMPTE170M;
            break;
    }
    // Phase 3.G — apply the user's per-clip range override when set.
    // The detected `frame->color_range` only feeds the conversion when
    // the override is Auto (0). Wrong-range mistagged sources are
    // common enough to need a manual escape hatch.
    int srcFullRange = (frame->color_range == AVCOL_RANGE_JPEG) ? 1 : 0;
    const int rangeOv = m_rangeOverride.load(std::memory_order_acquire);
    if (rangeOv == 1) srcFullRange = 1;        // Full
    else if (rangeOv == 2) srcFullRange = 0;   // Limited

    sws_setColorspaceDetails(
        m_sws,
        sws_getCoefficients(srcCsp), srcFullRange,
        sws_getCoefficients(SWS_CS_ITU709), /*dstFullRange=*/1,
        /*brightness=*/0, /*contrast=*/1 << 16, /*saturation=*/1 << 16);

    m_swsSrcWidth  = frame->width;
    m_swsSrcHeight = frame->height;
    m_swsSrcFormat = frame->format;
    return true;
}

int64_t VideoDecoder::ptsToMicroseconds(int64_t streamTickPts) const
{
    if (streamTickPts == AV_NOPTS_VALUE) return 0;
    if (!m_fmt || m_videoStreamIdx < 0) return streamTickPts;
    const AVStream *st = m_fmt->streams[m_videoStreamIdx];
    if (!st) return streamTickPts;
    return av_rescale_q(streamTickPts, st->time_base,
                        AVRational{1, 1000000});
}

void VideoDecoder::publishCpuFrame(AVFrame *frame)
{
    if (!initSwsContext(frame)) return;

    // One-shot diag — fires the first time we publish a CPU frame
    // for this source so we can see the format we're round-tripping
    // through swscale.
    if (!m_loggedCpuFormat) {
        const char *name = av_get_pix_fmt_name(static_cast<AVPixelFormat>(frame->format));
        qInfo("VideoDecoder[%s]: CPU publish path — pix_fmt=%s (%dx%d)",
              qPrintable(QFileInfo(m_sourcePath).fileName()),
              name ? name : "<unknown>",
              frame->width, frame->height);
        m_loggedCpuFormat = true;
    }

    QImage rgba(frame->width, frame->height, QImage::Format_RGBA8888);
    uint8_t *dst[4] = { rgba.bits(), nullptr, nullptr, nullptr };
    int dstStride[4] = { static_cast<int>(rgba.bytesPerLine()), 0, 0, 0 };

    sws_scale(m_sws,
              frame->data, frame->linesize, 0, frame->height,
              dst, dstStride);

    // RGB sources: swscale applies no range handling on an RGB→RGB
    // conversion, so the Range pill would be inert here — apply the
    // legal→full expansion under the shared rule (decode/rgb_range.h;
    // same helper the scrub / dual / thumbnail paths use).
    if (isRgbPixelFormat(frame->format)) {
        const int rangeOv = m_rangeOverride.load(std::memory_order_acquire);
        const bool limited = rgbFrameNeedsLegalExpansion(frame, rangeOv);
        if (limited != m_loggedRgbExpand || !m_loggedRgbExpandOnce) {
            qInfo("VideoDecoder[%s]: RGB source, legal-range expansion %s "
                  "(override=%d, frame tag=%s)",
                  qPrintable(QFileInfo(m_sourcePath).fileName()),
                  limited ? "ON" : "off", rangeOv,
                  frame->color_range == AVCOL_RANGE_MPEG ? "limited"
                  : frame->color_range == AVCOL_RANGE_JPEG ? "full" : "none");
            m_loggedRgbExpand = limited;
            m_loggedRgbExpandOnce = true;
        }
        if (limited) {
            expandRgba8LegalToFull(rgba.bits(), frame->width, frame->height,
                                   static_cast<int>(rgba.bytesPerLine()));
        }
    }

    const int64_t pts = (frame->best_effort_timestamp != AV_NOPTS_VALUE)
                        ? frame->best_effort_timestamp
                        : frame->pts;
    // Stamp the FrameHandle with PTS in microseconds — common
    // reference frame across decoders so the renderer's atomic-pair
    // gate can compare A vs B from sources with different
    // stream time_bases.
    const int64_t ptsUs = ptsToMicroseconds(pts);

    publishHandle(FrameHandle::cpu(std::move(rgba), ptsUs), pts, /*pace=*/true);
}

void VideoDecoder::publishMetalFrame(AVFrame *frame)
{
#if defined(Q_OS_MACOS)
    // For VideoToolbox, frame->data[3] holds CVPixelBufferRef. We
    // retain it so our FrameHandle owns a reference; av_frame_unref
    // on the AVFrame can then safely release the ffmpeg-owned ref.
    void *pix = frame->data[3];
    if (!pix) {
        qWarning("VideoDecoder: HW frame missing CVPixelBuffer at data[3]");
        return;
    }
    if (!m_loggedMetalFormat) {
        const unsigned int cvFmt = cvPixelBufferFormatTypeRaw(pix);
        char fcc[5] = {char((cvFmt>>24)&0xff), char((cvFmt>>16)&0xff),
                       char((cvFmt>>8)&0xff),  char(cvFmt&0xff), 0};
        qInfo("VideoDecoder[%s]: METAL zero-copy publish — "
              "CVPixelFormat=0x%08x ('%s') (%dx%d)",
              qPrintable(QFileInfo(m_sourcePath).fileName()),
              cvFmt, fcc, frame->width, frame->height);
        m_loggedMetalFormat = true;
    }
    cvPixelBufferRetainRaw(pix);
    const int64_t pts = (frame->best_effort_timestamp != AV_NOPTS_VALUE)
                        ? frame->best_effort_timestamp
                        : frame->pts;
    const int64_t ptsUs = ptsToMicroseconds(pts);
    publishHandle(FrameHandle::metal(pix, frame->width, frame->height, ptsUs), pts,
                  /*pace=*/true);
#else
    Q_UNUSED(frame);
#endif
}

void VideoDecoder::publishVulkanFrame(AVFrame *frame)
{
#if defined(Q_OS_WIN)
    // Phase F.2.4.2 — zero-copy publish for Vulkan-decoded frames.
    // FFmpeg gave us an AVFrame whose data[0] is an AVVkFrame
    // referencing a VkImage on OUR shared device. Cloning the AVFrame
    // bumps the ref count so the VkImage stays alive until the
    // FrameHandle is destroyed — that's the b-frame correctness lever
    // (renderer holds the ref while sampling, FFmpeg can't recycle
    // the pool slot out from under us).
    if (!m_loggedVulkanFormat) {
        const char *name = av_get_pix_fmt_name(static_cast<AVPixelFormat>(frame->format));
        const auto *swCfg = frame->hw_frames_ctx
            ? reinterpret_cast<const AVHWFramesContext *>(frame->hw_frames_ctx->data)
            : nullptr;
        const char *swName = (swCfg && swCfg->sw_format != AV_PIX_FMT_NONE)
            ? av_get_pix_fmt_name(swCfg->sw_format) : "<none>";
        qInfo("VideoDecoder[%s]: VULKAN zero-copy publish — "
              "format=%s sw_format=%s (%dx%d)",
              qPrintable(QFileInfo(m_sourcePath).fileName()),
              name ? name : "<unknown>", swName,
              frame->width, frame->height);
        m_loggedVulkanFormat = true;
    }

    AVFrame *cloned = av_frame_clone(frame);
    if (!cloned) {
        qWarning("VideoDecoder: av_frame_clone failed for Vulkan frame; dropping");
        return;
    }

    const int64_t pts = (frame->best_effort_timestamp != AV_NOPTS_VALUE)
                        ? frame->best_effort_timestamp
                        : frame->pts;
    const int64_t ptsUs = ptsToMicroseconds(pts);
    publishHandle(FrameHandle::vulkan(cloned, frame->width, frame->height, ptsUs),
                  pts, /*pace=*/true);
#else
    Q_UNUSED(frame);
#endif
}

void VideoDecoder::publishExternalFrame(FrameHandle handle, int64_t pts)
{
    publishHandle(std::move(handle), pts, /*pace=*/false);
    m_paceBaselineSet = false;   // streaming pacing has lost coherence with slot
}

void VideoDecoder::publishHandle(FrameHandle handle, int64_t pts, bool pace)
{
    // FPS pacing — only when actively playing AND the caller asked
    // for it. ScrubDecoder calls with pace=false so scrub frames
    // appear immediately. The first frame after a play()/seek/EOF
    // discontinuity sets the baseline; subsequent frames sleep until
    // baselineWall + (pts - baselinePts) * dt.
    if (pace && m_isPlaying.load(std::memory_order_acquire) && m_frameIndex.isValid()) {
        // Speed change since the last paced publish: re-baseline so
        // the new rate applies from here rather than re-pacing the
        // whole distance from the old baseline (which would fast-
        // forward or stall to "catch up" with the new divisor).
        if (m_paceSpeedDirty.exchange(false, std::memory_order_acq_rel)) {
            m_paceBaselineSet = false;
        }
        if (!m_paceBaselineSet) {
            m_paceBaselineWall = std::chrono::steady_clock::now();
            m_paceBaselinePts  = pts;
            m_paceBaselineSet  = true;
        } else {
            const double secsPerTick =
                static_cast<double>(m_frameIndex.timeBaseNum()) /
                static_cast<double>(m_frameIndex.timeBaseDen());
            const double speed =
                m_playbackSpeed.load(std::memory_order_acquire);
            const double targetSecs =
                (pts - m_paceBaselinePts) * secsPerTick
                / ((speed > 0.0) ? speed : 1.0);
            const auto target = m_paceBaselineWall +
                std::chrono::nanoseconds(static_cast<int64_t>(targetSecs * 1e9));
            const auto now = std::chrono::steady_clock::now();
            if (target > now) {
                std::this_thread::sleep_for(target - now);
            }
        }
    }

    {
        std::lock_guard<std::mutex> lk(m_publishMutex);
        m_publishedFrame = std::move(handle);
    }
    m_publishedSeq.fetch_add(1, std::memory_order_release);

    // Update currentFrame Q_PROPERTY (read by the UI's slider). Emit
    // on UI thread via queued connection.
    if (m_frameIndex.isValid()) {
        const int frameNo = m_frameIndex.frameForPts(pts);
        if (frameNo != m_currentFrame.exchange(frameNo, std::memory_order_acq_rel)) {
            emit currentFrameChanged();
        }
    }

    emit frameAvailable();
}

void VideoDecoder::decodeLoop()
{
    AVPacket *pkt     = av_packet_alloc();
    AVFrame  *frame   = av_frame_alloc();
    AVFrame  *swFrame = av_frame_alloc();   // landing pad for hw → cpu transfer
    if (!pkt || !frame || !swFrame) {
        setError(QStringLiteral("av_packet_alloc / av_frame_alloc failed"));
        setState(Errored);
        if (pkt) av_packet_free(&pkt);
        if (frame) av_frame_free(&frame);
        if (swFrame) av_frame_free(&swFrame);
        return;
    }

    bool eofPacketsSent = false;
    bool atEof = false;

    while (!m_stopRequested.load(std::memory_order_acquire)) {
#if defined(Q_OS_WIN)
        // Phase I.B — bail out cleanly when the shared VkDevice has
        // been marked lost (ffmpegLogCallback flipped the flag in
        // response to a VK_ERROR_DEVICE_LOST). Drop our cached
        // hwdevice ref so a subsequent reopen falls through to
        // createSharedVulkanHwDeviceCtx (which also refuses while
        // the flag is set) → CPU path. Without this poll the decode
        // loop keeps submitting to the poisoned device and spamming
        // the log; with it we error out once and the user gets a
        // clear "needs restart" state.
        if (m_hwDeviceCtx
            && VulkanDeviceManager::instance().isDeviceLost()) {
            releaseCachedHwDevice();
            setError(tr("Vulkan device lost — restart to recover "
                        "hardware decode"));
            setState(Errored);
            return;
        }
#endif
        // ---- Pending-seek check. Latest request wins; older targets
        // are silently abandoned via the atomic exchange.
        const int seekTarget = m_pendingSeekTarget.exchange(-1, std::memory_order_acq_rel);
        if (seekTarget >= 0) {
            m_paceBaselineSet = false;   // discontinuity; reset pacing
            performSeek(seekTarget, pkt, frame, swFrame);
            eofPacketsSent = false;
            atEof = false;
            setState(Decoding);
            continue;
        }

        // ---- Paused: block until play(), seek, or stop. Decode work
        // ceases; the last published frame stays visible.
        if (!m_isPlaying.load(std::memory_order_acquire)) {
            std::unique_lock<std::mutex> lk(m_seekCondMutex);
            m_seekCond.wait(lk, [this] {
                return m_stopRequested.load(std::memory_order_acquire) ||
                       m_pendingSeekTarget.load(std::memory_order_acquire) >= 0 ||
                       m_isPlaying.load(std::memory_order_acquire);
            });
            // Reset pacing baseline — wall time advanced while paused.
            m_paceBaselineSet = false;
            continue;
        }

        // ---- If we've drained the file, auto-pause and block until
        // a seek arrives (or stop). Keeps the slider responsive after
        // EOF and matches Premiere/Resolve "play stops at the end".
        if (atEof) {
            if (m_isPlaying.exchange(false, std::memory_order_acq_rel)) {
                emit isPlayingChanged();
            }
            std::unique_lock<std::mutex> lk(m_seekCondMutex);
            m_seekCond.wait(lk, [this] {
                return m_stopRequested.load(std::memory_order_acquire) ||
                       m_pendingSeekTarget.load(std::memory_order_acquire) >= 0;
            });
            continue;
        }

        // ---- Normal decode: read packets, send to codec.
        if (!eofPacketsSent) {
            const int rc = av_read_frame(m_fmt, pkt);
            if (rc == AVERROR_EOF) {
                avcodec_send_packet(m_cctx, nullptr);   // drain
                eofPacketsSent = true;
            } else if (rc < 0) {
                setError(tr("av_read_frame error: %1").arg(avErrToString(rc)));
                break;
            } else {
                if (pkt->stream_index == m_videoStreamIdx) {
                    if (int sendErr = avcodec_send_packet(m_cctx, pkt); sendErr < 0 &&
                        sendErr != AVERROR(EAGAIN)) {
                        recordDecodeError(tr("send_packet: %1").arg(avErrToString(sendErr)));
                    }
                }
                av_packet_unref(pkt);
            }
        }

        // ---- Drain whatever frames are ready.
        while (true) {
            const int recvErr = avcodec_receive_frame(m_cctx, frame);
            if (recvErr == AVERROR(EAGAIN)) break;
            if (recvErr == AVERROR_EOF) {
                setState(EndOfStream);
                atEof = true;
                break;
            }
            if (recvErr < 0) {
                setError(tr("avcodec_receive_frame: %1").arg(avErrToString(recvErr)));
                goto finished;
            }

            if (frame->format == AV_PIX_FMT_VIDEOTOOLBOX) {
                // VideoToolbox emits CVPixelBuffers in a few formats
                // depending on codec. Our render-thread bridge handles
                // biplanar YUV (NV12, P010, P210) zero-copy. Anything
                // else (notably ProRes 4444's 4-plane AYpCbCr16 layout)
                // falls back to av_hwframe_transfer_data + CPU upload
                // — still hwaccel-decoded on the Media Engine, just
                // one extra round trip to system memory.
                void *pix = frame->data[3];
                if (pix && cvPixelBufferIsZeroCopySupportedRaw(pix)) {
                    publishMetalFrame(frame);
                } else {
#if defined(Q_OS_MACOS)
                    if (!m_loggedHwToCpuFallback && pix) {
                        const unsigned int cvFmt = cvPixelBufferFormatTypeRaw(pix);
                        char fcc[5] = {char((cvFmt>>24)&0xff), char((cvFmt>>16)&0xff),
                                       char((cvFmt>>8)&0xff),  char(cvFmt&0xff), 0};
                        qInfo("VideoDecoder[%s]: VT decoded, but pixfmt 0x%08x "
                              "('%s') NOT in zero-copy list — falling back to "
                              "hwframe_transfer + CPU upload (sws_scale)",
                              qPrintable(QFileInfo(m_sourcePath).fileName()),
                              cvFmt, fcc);
                        m_loggedHwToCpuFallback = true;
                    }
#endif
                    if (av_hwframe_transfer_data(swFrame, frame, 0) >= 0) {
                        swFrame->pts                   = frame->pts;
                        swFrame->best_effort_timestamp = frame->best_effort_timestamp;
                        swFrame->pkt_dts               = frame->pkt_dts;
                        swFrame->colorspace            = frame->colorspace;
                        swFrame->color_range           = frame->color_range;
                        swFrame->color_primaries       = frame->color_primaries;
                        swFrame->color_trc             = frame->color_trc;
                        publishCpuFrame(swFrame);
                        av_frame_unref(swFrame);
                    } else {
                        recordDecodeError(tr("hwframe_transfer_data fallback failed"));
                    }
                }
            }
            else if (frame->format == AV_PIX_FMT_VULKAN) {
                // Phase F.2.4.2 — zero-copy Vulkan publish. NO
                // av_hwframe_transfer_data: the AVVkFrame stays on the
                // GPU; the F.2.4.3 NT-handle bridge exports it to
                // D3D11 on the renderer thread.
                publishVulkanFrame(frame);
            }
            else if (frame->format == AV_PIX_FMT_D3D11 ||
                     frame->format == AV_PIX_FMT_VAAPI) {
                if (int xferErr = av_hwframe_transfer_data(swFrame, frame, 0); xferErr < 0) {
                    recordDecodeError(tr("hwframe_transfer_data: %1").arg(avErrToString(xferErr)));
                    av_frame_unref(frame);
                    continue;
                }
                swFrame->pts                   = frame->pts;
                swFrame->best_effort_timestamp = frame->best_effort_timestamp;
                swFrame->pkt_dts               = frame->pkt_dts;
                swFrame->colorspace            = frame->colorspace;
                swFrame->color_range           = frame->color_range;
                swFrame->color_primaries       = frame->color_primaries;
                swFrame->color_trc             = frame->color_trc;
                publishCpuFrame(swFrame);
                av_frame_unref(swFrame);
            }
            else {
                publishCpuFrame(frame);
            }

            av_frame_unref(frame);
        }
    }

finished:
    av_packet_free(&pkt);
    av_frame_free(&frame);
    av_frame_free(&swFrame);
}

void VideoDecoder::performSeek(int targetFrame, AVPacket *pkt,
                               AVFrame *frame, AVFrame *swFrame)
{
    if (!m_frameIndex.isValid() || !m_fmt || !m_cctx) return;

    const int64_t targetPts = m_frameIndex.ptsForFrame(targetFrame);

    // av_seek_frame in stream's time_base; AVSEEK_FLAG_BACKWARD lands
    // on the keyframe at-or-before target. For intra codecs every
    // frame is a keyframe so this is exact; for inter codecs we
    // decode forward from the keyframe.
    if (av_seek_frame(m_fmt, m_videoStreamIdx, targetPts,
                      AVSEEK_FLAG_BACKWARD) < 0) {
        recordDecodeError(tr("av_seek_frame failed for frame %1").arg(targetFrame));
        return;
    }

    // Codec quirk: flush before AND we'll be flushing buffers post-
    // seek. Reference: Guide 13 Part II §13.
    avcodec_flush_buffers(m_cctx);

    bool eof = false;
    while (!m_stopRequested.load(std::memory_order_acquire)) {
        // Run this seek to completion. The decode-thread main loop
        // (line ~744) collapses queued seeks via the
        // pendingSeekTarget atomic — it always pulls the LATEST
        // target before re-entering performSeek, so older targets
        // are dropped naturally. Aborting mid-decode here breaks
        // fast-seek for B-frame media: every tick stomps the
        // pending target before this loop ever publishes a frame,
        // so the decoder never produces output during the gesture.
        // DualVideoDecoder doesn't abort mid-seek and it's why dual
        // fast-seek works smoothly even with two B-frame sources.

        if (!eof) {
            const int rc = av_read_frame(m_fmt, pkt);
            if (rc == AVERROR_EOF) {
                avcodec_send_packet(m_cctx, nullptr);
                eof = true;
            } else if (rc < 0) {
                recordDecodeError(tr("read_frame after seek: %1").arg(avErrToString(rc)));
                return;
            } else {
                if (pkt->stream_index == m_videoStreamIdx) {
                    avcodec_send_packet(m_cctx, pkt);
                }
                av_packet_unref(pkt);
            }
        }

        while (true) {
            const int recvErr = avcodec_receive_frame(m_cctx, frame);
            if (recvErr == AVERROR(EAGAIN)) break;
            if (recvErr < 0) return;   // EOF before reaching target

            const int64_t framePts = (frame->best_effort_timestamp != AV_NOPTS_VALUE)
                                     ? frame->best_effort_timestamp
                                     : frame->pts;

            if (framePts >= targetPts) {
                if (frame->format == AV_PIX_FMT_VIDEOTOOLBOX) {
                    void *pix = frame->data[3];
                    if (pix && cvPixelBufferIsZeroCopySupportedRaw(pix)) {
                        publishMetalFrame(frame);
                    } else if (av_hwframe_transfer_data(swFrame, frame, 0) >= 0) {
                        swFrame->pts                   = frame->pts;
                        swFrame->best_effort_timestamp = frame->best_effort_timestamp;
                        swFrame->pkt_dts               = frame->pkt_dts;
                        swFrame->colorspace            = frame->colorspace;
                        swFrame->color_range           = frame->color_range;
                        swFrame->color_primaries       = frame->color_primaries;
                        swFrame->color_trc             = frame->color_trc;
                        publishCpuFrame(swFrame);
                        av_frame_unref(swFrame);
                    }
                }
                else if (frame->format == AV_PIX_FMT_VULKAN) {
                    publishVulkanFrame(frame);
                }
                else if (frame->format == AV_PIX_FMT_D3D11 ||
                         frame->format == AV_PIX_FMT_VAAPI) {
                    if (av_hwframe_transfer_data(swFrame, frame, 0) >= 0) {
                        swFrame->pts                   = frame->pts;
                        swFrame->best_effort_timestamp = frame->best_effort_timestamp;
                        swFrame->colorspace            = frame->colorspace;
                        swFrame->color_range           = frame->color_range;
                        publishCpuFrame(swFrame);
                        av_frame_unref(swFrame);
                    }
                }
                else {
                    publishCpuFrame(frame);
                }
                av_frame_unref(frame);
                return;   // back to main loop; resume forward decode
            }
            // Pre-target frame in an inter-frame chain — drop silently.
            av_frame_unref(frame);
        }
    }
}

} // namespace qcv
