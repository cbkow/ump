#include "dual_video_decoder.h"

#include <QDebug>
#include <QFileInfo>
#include <QSettings>
#include <QtLogging>
#include <algorithm>
#include <cmath>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#if defined(Q_OS_MACOS)
// CFRetain / CFRelease shims for CVPixelBufferRef. Defined in
// dual_cv_helpers.mm so this .cpp stays Obj-C-free.
extern "C" void dualCvPixelBufferRetain(void *cvPix);
extern "C" void dualCvPixelBufferRelease(void *cvPix);
#endif

#if defined(Q_OS_WIN)
// F.2.12.c — Vulkan-decode handoff. Same helper single-flow uses;
// promoted to a public header in F.2.12.a so dual reuses it
// verbatim instead of duplicating the AVVulkanDeviceContext setup.
#include "decode/vulkan_hw_device_ctx.h"
// Cut A — used in teardownFFmpeg to flush in-flight Vulkan work
// before freeing the hwdevice context that owns the AVVkFrame pool.
// Without the flush, an intermittent VK_ERROR_DEVICE_LOST cascades on
// the next dual init when the GPU is still sampling the just-freed
// frames (Intel Arc — fragile re cross-pool VkImage reuse).
//
// NOMINMAX must be defined before vulkan.h drags in windows.h
// (transitively via the device-manager header) so the Win32 min/max
// macros don't clobber std::min / std::max used elsewhere in this TU.
#define NOMINMAX
#include "decode/vulkan/vulkan_device_manager.h"
#endif

namespace qcv::dual {

namespace {

QString avErrToString(int err)
{
    char buf[256];
    av_strerror(err, buf, sizeof(buf));
    return QString::fromUtf8(buf);
}

// FFmpeg pixfmt-selection callback. Picks our preferred hwaccel pixel
// format when the codec offers it; otherwise falls through to the
// first software format. Mirrors VideoDecoder's hwaccelGetFormat —
// duplicated here per qcv_dual's "isolation > duplication" design.
AVPixelFormat hwaccelGetFormat(AVCodecContext *ctx,
                                const AVPixelFormat *fmts)
{
    // Mirrors single-flow VideoDecoder::hwaccelGetFormat. Windows
    // routes per-codec — ProRes via Vulkan (libplacebo compute is
    // the only viable hardware path), everything else via D3D11VA
    // (well-trodden Microsoft path, sidesteps a libplacebo Vulkan
    // H.264 heap-corruption bug — see
    // [[intel-arc-vulkan-bridge-crash]] memory entry).
#if defined(Q_OS_MACOS)
    constexpr AVPixelFormat kPreferred[] = { AV_PIX_FMT_VIDEOTOOLBOX,
                                              AV_PIX_FMT_NONE };
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
    constexpr AVPixelFormat kPreferred[] = { AV_PIX_FMT_VULKAN,
                                              AV_PIX_FMT_VAAPI,
                                              AV_PIX_FMT_NONE };
#else
    constexpr AVPixelFormat kPreferred[] = { AV_PIX_FMT_NONE };
#endif
    for (const AVPixelFormat *p = kPreferred; *p != AV_PIX_FMT_NONE; ++p) {
        const AVPixelFormat want = *p;
        for (int i = 0; fmts[i] != AV_PIX_FMT_NONE; ++i) {
            if (fmts[i] == want) return want;
        }
    }
    return fmts[0];
}

// Adaptive timeout for the decode CV — matches old QCView's pattern
// (~1/fps clamped to 15-30 ms). When the decoder idles full, this is
// how long it sleeps before re-checking NeedsMoreFrames in case the
// playhead moved without an explicit notify.
int adaptiveTimeoutMs(double fps)
{
    if (fps <= 0.0) return 30;
    const int ms = static_cast<int>(std::lround(1000.0 / fps));
    return std::clamp(ms, 15, 30);
}

} // namespace

DualVideoDecoder::DualVideoDecoder() = default;

DualVideoDecoder::~DualVideoDecoder()
{
    close();
}

bool DualVideoDecoder::open(const QString &path)
{
    close();

    if (!QFileInfo(path).isFile()) {
        qWarning("DualVideoDecoder::open: file not found: %s", qPrintable(path));
        return false;
    }

    m_path = path;

    if (!initFFmpeg(path)) {
        teardownFFmpeg();
        m_path.clear();
        return false;
    }

    m_stopRequested.store(false, std::memory_order_release);
    m_pendingSeekTarget.store(-1, std::memory_order_release);
    m_decodeTarget.store(0, std::memory_order_release);

    {
        std::lock_guard<std::mutex> lk(m_bufferMutex);
        for (auto &slot : m_ring) slot.reset();
        m_frameMap.clear();
        m_ringHead  = 0;
        m_ringCount = 0;
    }

    m_open.store(true, std::memory_order_release);
    m_thread = std::thread([this] { decodeThreadFunc(); });

    qInfo("DualVideoDecoder: opened %s (%dx%d, %.2f fps, %d frames)",
          qPrintable(QFileInfo(path).fileName()),
          m_width, m_height, m_fps, m_frameCount);
    return true;
}

void DualVideoDecoder::close()
{
    if (!m_open.exchange(false, std::memory_order_acq_rel) && !m_thread.joinable()) {
        return;
    }
    m_stopRequested.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lk(m_decodeCvMutex);
    }
    m_decodeCv.notify_all();
    if (m_thread.joinable()) {
        m_thread.join();
    }

    teardownFFmpeg();

    {
        std::lock_guard<std::mutex> lk(m_bufferMutex);
        for (auto &slot : m_ring) slot.reset();
        m_frameMap.clear();
        m_ringHead  = 0;
        m_ringCount = 0;
    }

    m_path.clear();
    m_width = m_height = m_frameCount = 0;
    m_fps = 0.0;
}

void DualVideoDecoder::setDecodeTarget(int frameNumber)
{
    if (!m_open.load(std::memory_order_acquire)) return;
    if (frameNumber < 0) frameNumber = 0;
    if (m_frameCount > 0 && frameNumber >= m_frameCount) {
        frameNumber = m_frameCount - 1;
    }
    const int prev = m_decodeTarget.exchange(frameNumber, std::memory_order_acq_rel);
    if (prev == frameNumber) return;

    // Phase 7.8 — when the new target is FAR from currently-buffered
    // frames, trigger a real av_seek_frame instead of letting the
    // decode loop catch up by decoding every intermediate frame in
    // stream order. Without this, an edit that shifts sourceIn by
    // 100 frames takes ~100 decode-and-discards to catch up — the
    // user sees a "flash of pre-edit content then rapid catch-up"
    // because the decoder is producing all the intermediate frames
    // before reaching the requested one.
    //
    // Threshold = kRingSize. Within the ring, normal decode-ahead
    // catches up cheaply. Beyond it, we seek.
    bool needsSeek = false;
    {
        std::lock_guard<std::mutex> lk(m_bufferMutex);
        if (m_ringCount > 0) {
            int bufMin = std::numeric_limits<int>::max();
            int bufMax = std::numeric_limits<int>::min();
            for (int i = 0; i < m_ringCount; ++i) {
                const int idx = (m_ringHead + i) % kRingSize;
                if (!m_ring[idx].valid) continue;
                bufMin = std::min(bufMin, m_ring[idx].frameNumber);
                bufMax = std::max(bufMax, m_ring[idx].frameNumber);
            }
            if (bufMin <= bufMax) {
                // ANY backward jump out of the buffered range needs a
                // seek — the decoder can't go backwards by decoding.
                // Forward jumps within ring + readahead headroom are
                // handled by needsMoreFrames; beyond it, seek to skip
                // decoding-and-discarding intermediate frames.
                if (frameNumber < bufMin) {
                    needsSeek = true;
                } else if (frameNumber > bufMax + kRingSize) {
                    needsSeek = true;
                }
            }
        }
    }
    if (needsSeek) {
        m_pendingSeekTarget.store(frameNumber, std::memory_order_release);
    }

    std::lock_guard<std::mutex> lk(m_decodeCvMutex);
    m_decodeCv.notify_one();
}

void DualVideoDecoder::setRangeOverride(int v)
{
    if (v < 0) v = 0;
    if (v > 2) v = 0;
    const int prev = m_rangeOverride.exchange(v, std::memory_order_acq_rel);
    if (prev == v) return;

    // Drop the ring on change. CPU-converted frames have the OLD
    // sws_setColorspaceDetails range baked into their RGBA bytes;
    // leaving them in the ring would display stale-range pixels for
    // the next ~16 frames. Metal frames hold raw CVPixelBuffers so
    // re-running the YUV→RGB pass with the new override at compositor
    // time would suffice — but for one shared invalidation path it's
    // simpler (and cheaper, given how rare range changes are) to drop
    // both and let the decode thread refill from the current target.
    {
        std::lock_guard<std::mutex> lk(m_bufferMutex);
        for (auto &slot : m_ring) slot.reset();
        m_frameMap.clear();
        m_ringHead  = 0;
        m_ringCount = 0;
    }
    {
        std::lock_guard<std::mutex> lk(m_decodeCvMutex);
    }
    m_decodeCv.notify_one();
}

void DualVideoDecoder::seekTo(int frameNumber)
{
    if (!m_open.load(std::memory_order_acquire)) return;
    if (frameNumber < 0) frameNumber = 0;
    if (m_frameCount > 0 && frameNumber >= m_frameCount) {
        frameNumber = m_frameCount - 1;
    }
    m_pendingSeekTarget.store(frameNumber, std::memory_order_release);
    m_decodeTarget.store(frameNumber, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lk(m_decodeCvMutex);
    }
    m_decodeCv.notify_one();
}

std::shared_ptr<DualFrame> DualVideoDecoder::getBufferedFrame(int frameNumber) const
{
    std::lock_guard<std::mutex> lk(m_bufferMutex);
    auto it = m_frameMap.find(frameNumber);
    if (it == m_frameMap.end() || !m_ring[it->second].valid) {
        // G.1.c — log ring snapshot on Empty pulls during active
        // playback so we can see whether the decoder is behind, ahead,
        // or just out-of-range. One log every ~30 misses to avoid
        // flooding.
        static std::atomic<int> missCount{0};
        const int n = missCount.fetch_add(1) + 1;
        if (n % 30 == 1) {
            int bufMin = INT_MAX, bufMax = INT_MIN, valid = 0;
            for (int i = 0; i < m_ringCount; ++i) {
                const int idx = (m_ringHead + i) % kRingSize;
                if (!m_ring[idx].valid) continue;
                ++valid;
                bufMin = std::min(bufMin, m_ring[idx].frameNumber);
                bufMax = std::max(bufMax, m_ring[idx].frameNumber);
            }
            const int target = m_decodeTarget.load(std::memory_order_acquire);
            qInfo("DualVideoDecoder: getBufferedFrame MISS #%d req=%d "
                  "target=%d ringCount=%d valid=%d range=[%d,%d]",
                  n, frameNumber, target, m_ringCount, valid,
                  valid > 0 ? bufMin : -1,
                  valid > 0 ? bufMax : -1);
        }
        return nullptr;
    }
    const BufferedFrame &slot = m_ring[it->second];
    return slot.frame;
}

bool DualVideoDecoder::hasFrame(int frameNumber) const
{
    std::lock_guard<std::mutex> lk(m_bufferMutex);
    auto it = m_frameMap.find(frameNumber);
    return it != m_frameMap.end() && m_ring[it->second].valid;
}

int DualVideoDecoder::bufferedAhead() const
{
    const int target = m_decodeTarget.load(std::memory_order_acquire);
    std::lock_guard<std::mutex> lk(m_bufferMutex);
    int count = 0;
    for (int i = 0; i < m_ringCount; ++i) {
        const int idx = (m_ringHead + i) % kRingSize;
        if (m_ring[idx].valid && m_ring[idx].frameNumber >= target) {
            ++count;
        }
    }
    return count;
}

int DualVideoDecoder::bufferedBehind() const
{
    const int target = m_decodeTarget.load(std::memory_order_acquire);
    std::lock_guard<std::mutex> lk(m_bufferMutex);
    int count = 0;
    for (int i = 0; i < m_ringCount; ++i) {
        const int idx = (m_ringHead + i) % kRingSize;
        if (m_ring[idx].valid && m_ring[idx].frameNumber < target) {
            ++count;
        }
    }
    return count;
}

int DualVideoDecoder::bufferCount() const
{
    std::lock_guard<std::mutex> lk(m_bufferMutex);
    return m_ringCount;
}

void DualVideoDecoder::getBufferedRange(int &startFrame, int &endFrame) const
{
    std::lock_guard<std::mutex> lk(m_bufferMutex);
    if (m_ringCount == 0) {
        startFrame = endFrame = -1;
        return;
    }
    int lo = std::numeric_limits<int>::max();
    int hi = std::numeric_limits<int>::min();
    for (int i = 0; i < m_ringCount; ++i) {
        const int idx = (m_ringHead + i) % kRingSize;
        if (m_ring[idx].valid) {
            lo = std::min(lo, m_ring[idx].frameNumber);
            hi = std::max(hi, m_ring[idx].frameNumber);
        }
    }
    startFrame = (lo == std::numeric_limits<int>::max()) ? -1 : lo;
    endFrame   = (hi == std::numeric_limits<int>::min()) ? -1 : hi;
}

bool DualVideoDecoder::initFFmpeg(const QString &path)
{
    const QByteArray pathUtf8 = path.toUtf8();

    if (int err = avformat_open_input(&m_fmt, pathUtf8.constData(), nullptr, nullptr); err < 0) {
        qWarning("DualVideoDecoder: avformat_open_input failed: %s",
                 qPrintable(avErrToString(err)));
        return false;
    }
    if (int err = avformat_find_stream_info(m_fmt, nullptr); err < 0) {
        qWarning("DualVideoDecoder: avformat_find_stream_info failed: %s",
                 qPrintable(avErrToString(err)));
        return false;
    }

    for (unsigned i = 0; i < m_fmt->nb_streams; ++i) {
        if (m_fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            m_streamIdx = static_cast<int>(i);
            break;
        }
    }
    if (m_streamIdx < 0) {
        qWarning("DualVideoDecoder: no video stream");
        return false;
    }

    AVStream *st = m_fmt->streams[m_streamIdx];
    AVCodecParameters *codecpar = st->codecpar;
    const AVCodec *codec = avcodec_find_decoder(codecpar->codec_id);
    if (!codec) {
        qWarning("DualVideoDecoder: no decoder for codec id %d", codecpar->codec_id);
        return false;
    }

    m_cctx = avcodec_alloc_context3(codec);
    if (!m_cctx) {
        qWarning("DualVideoDecoder: avcodec_alloc_context3 failed");
        return false;
    }
    avcodec_parameters_to_context(m_cctx, codecpar);

    // Deliberately NOT reading the user's `performance/ffmpegThreads`
    // QSetting here — see VideoDecoder::initFFmpeg for the per-decoder
    // count. Dual SBS / Wipe runs two AVCodecContexts concurrently, so
    // applying the user's "16 threads" knob to both would request 32
    // threads for the same workload and just have them contend.
    // FFmpeg's per-context auto (thread_count = 0, default) picks a
    // sensible per-codec value that doesn't fight its sibling. The
    // settings panel help text reflects this.

    // Attach platform hwaccel. Tier ladder per platform mirrors
    // single-flow VideoDecoder (video_decoder.cpp:497-589):
    //   macOS   — VideoToolbox (zero-copy CVPixelBuffer)
    //   Windows — Vulkan with a feasibility probe + shared
    //             VulkanDeviceManager handoff; falls back to software
    //             when the driver rejects the stream's sw_format
    //             (Intel Arc + 4-plane YUVA is the known case).
    //   Linux   — VAAPI (Vulkan-on-Linux gated behind QCV_BUILD_LINUX_VULKAN)
    // When the probe / device-create fails the get_format callback
    // falls through to a software pixfmt and decode continues on CPU.
#if defined(Q_OS_MACOS)
    {
        constexpr auto kHwType = AV_HWDEVICE_TYPE_VIDEOTOOLBOX;
        const char *kHwName = "videotoolbox";
        if (av_hwdevice_ctx_create(&m_hwDeviceCtx, kHwType,
                                     nullptr, nullptr, 0) >= 0) {
            m_cctx->hw_device_ctx = av_buffer_ref(m_hwDeviceCtx);
            m_cctx->get_format    = hwaccelGetFormat;
            m_hwAttached          = true;
            m_hwBackend           = QStringLiteral("videotoolbox");
            qInfo("DualVideoDecoder: %s hwaccel attached", kHwName);
        } else {
            qInfo("DualVideoDecoder: %s hwaccel unavailable; software decode",
                  kHwName);
        }
    }
#elif defined(Q_OS_WIN)
    // Per-codec hwaccel routing — only ProRes routes to Vulkan
    // (libplacebo compute decoder); everything else routes to
    // D3D11VA. Mirrors single-flow VideoDecoder. Sidesteps a
    // libplacebo Vulkan H.264 heap-corruption bug — see
    // [[intel-arc-vulkan-bridge-crash]] memory.
    //
    // performance/hardwareDecodeEnabled — Windows user-facing
    // toggle (defaults ON). When the user turns it OFF, skip every
    // hwaccel attach (Vulkan and D3D11VA) and go straight to
    // software decode.
    const bool kHwDecodeEnabled = QSettings().value(
        QStringLiteral("performance/hardwareDecodeEnabled"), true).toBool();
    const bool kIsProRes =
        codecpar && codecpar->codec_id == AV_CODEC_ID_PRORES;
    // Per-instance ProRes-only force-SW (set by DualPlaybackController
    // for B in dual mode) joins the global hardwareDecodeEnabled toggle
    // — both routes take ProRes through software, sidestepping the
    // ProRes-over-Vulkan instability on NVIDIA dual paths.
    const bool kForceSoftwareDecode =
        !kHwDecodeEnabled || (kIsProRes && m_forceSwForProRes);
    bool skipVulkan = !kIsProRes || kForceSoftwareDecode;
    bool skipAllHw  = kForceSoftwareDecode;

    // Vulkan pre-probe — only when we'd actually use Vulkan for
    // this codec. Allocates a small hwframes pool to test driver
    // feasibility; if the probe rejects the sw_format, skip Vulkan
    // and let D3D11VA take over below.
    AVPixelFormat probedSwFmt =
        (codecpar && !skipVulkan)
            ? static_cast<AVPixelFormat>(codecpar->format)
            : AV_PIX_FMT_NONE;

    if (probedSwFmt != AV_PIX_FMT_NONE) {
        AVBufferRef *deviceProbe = qcv::createSharedVulkanHwDeviceCtx();
        if (deviceProbe) {
            AVBufferRef *framesRef = av_hwframe_ctx_alloc(deviceProbe);
            if (framesRef) {
                auto *fctx = reinterpret_cast<AVHWFramesContext *>(framesRef->data);
                fctx->format    = AV_PIX_FMT_VULKAN;
                fctx->sw_format = probedSwFmt;
                fctx->width     = codecpar->width;
                fctx->height    = codecpar->height;
                fctx->initial_pool_size = 4;
                const int initResult = av_hwframe_ctx_init(framesRef);
                av_buffer_unref(&framesRef);
                if (initResult == 0) {
                    m_hwDeviceCtx = deviceProbe;
                    qInfo("DualVideoDecoder: Vulkan hwframes probe OK for %s "
                          "%dx%d — will use Vulkan with FFmpeg-managed pool",
                          av_get_pix_fmt_name(probedSwFmt),
                          codecpar->width, codecpar->height);
                } else {
                    av_buffer_unref(&deviceProbe);
                    skipVulkan = true;
                    qInfo("DualVideoDecoder: Vulkan hwframes probe REJECTED for "
                          "%s %dx%d — driver can't allocate this format; "
                          "falling back to D3D11VA / CPU",
                          av_get_pix_fmt_name(probedSwFmt),
                          codecpar->width, codecpar->height);
                }
            } else {
                av_buffer_unref(&deviceProbe);
                skipVulkan = true;
            }
        } else {
            skipVulkan = true;
        }
    }

    if (m_hwDeviceCtx && !skipVulkan) {
        m_cctx->hw_device_ctx = av_buffer_ref(m_hwDeviceCtx);
        m_cctx->get_format    = hwaccelGetFormat;
        m_hwAttached          = true;
        m_hwBackend           = QStringLiteral("vulkan");
        qInfo("DualVideoDecoder: vulkan hwaccel attached (FFmpeg-managed pool)");
    } else if (!skipAllHw
               && av_hwdevice_ctx_create(&m_hwDeviceCtx,
                                          AV_HWDEVICE_TYPE_D3D11VA,
                                          nullptr, nullptr, 0) >= 0) {
        m_cctx->hw_device_ctx = av_buffer_ref(m_hwDeviceCtx);
        m_cctx->get_format    = hwaccelGetFormat;
        m_hwAttached          = true;
        m_hwBackend           = QStringLiteral("d3d11va");
        qInfo("DualVideoDecoder: d3d11va hwaccel attached%s",
              skipVulkan ? " (codec-routed)"
                         : " (Vulkan shared-device unavailable)");
    } else {
        qInfo("DualVideoDecoder: software decode%s",
              kForceSoftwareDecode
                  ? (m_forceSwForProRes && kIsProRes
                     ? " (ProRes B-side dual-mode force-SW)"
                     : " (performance/hardwareDecodeEnabled is off)")
                  : "");
    }
#elif defined(Q_OS_LINUX)
    {
        constexpr auto kHwType = AV_HWDEVICE_TYPE_VAAPI;
        const char *kHwName = "vaapi";
        if (av_hwdevice_ctx_create(&m_hwDeviceCtx, kHwType,
                                     nullptr, nullptr, 0) >= 0) {
            m_cctx->hw_device_ctx = av_buffer_ref(m_hwDeviceCtx);
            m_cctx->get_format    = hwaccelGetFormat;
            m_hwAttached          = true;
            m_hwBackend           = QStringLiteral("vaapi");
            qInfo("DualVideoDecoder: %s hwaccel attached", kHwName);
        } else {
            qInfo("DualVideoDecoder: %s hwaccel unavailable; software decode",
                  kHwName);
        }
    }
#endif

    if (int err = avcodec_open2(m_cctx, codec, nullptr); err < 0) {
        qWarning("DualVideoDecoder: avcodec_open2 failed: %s",
                 qPrintable(avErrToString(err)));
        return false;
    }

    // Allocate the HW→SW transfer landing pad up front (cheap; freed
    // in teardownFFmpeg). Used in convertFrameToRgba when frame is
    // backed by a hwaccel surface.
    m_swFrame = av_frame_alloc();
    if (!m_swFrame) {
        qWarning("DualVideoDecoder: av_frame_alloc(swFrame) failed");
        return false;
    }

    m_width  = codecpar->width;
    m_height = codecpar->height;

    m_streamTimeBase = st->time_base;
    m_streamFrameRate = av_guess_frame_rate(m_fmt, st, nullptr);
    if (m_streamFrameRate.num > 0 && m_streamFrameRate.den > 0) {
        m_fps = av_q2d(m_streamFrameRate);
    } else {
        m_fps = 24.0;   // safe default; rare for streams to omit fps entirely
    }

    int totalFrames = static_cast<int>(st->nb_frames);
    if (totalFrames <= 0 && m_streamFrameRate.num > 0 && m_streamFrameRate.den > 0
        && m_fmt->duration > 0) {
        totalFrames = static_cast<int>(av_rescale_q(
            m_fmt->duration, { 1, AV_TIME_BASE },
            { m_streamFrameRate.den, m_streamFrameRate.num }));
    }
    m_frameCount = totalFrames;

    m_streamStartPts = (st->start_time != AV_NOPTS_VALUE) ? st->start_time : 0;

    return true;
}

void DualVideoDecoder::teardownFFmpeg()
{
#if defined(Q_OS_WIN)
    // Flush any in-flight GPU work BEFORE freeing the hwdevice context
    // that backs our AVVkFrame pool. The compositor / decode bridge may
    // still be reading frames from the previous session; tearing the
    // context out from under them on Intel Arc trips a device-lost
    // cascade on the *next* dual init that recovers but is user-visible.
    // No-op when the singleton wasn't initialized (CPU-only fallback).
    VulkanDeviceManager::instance().waitForGpu();
#endif
    if (m_sws) {
        sws_freeContext(m_sws);
        m_sws = nullptr;
    }
    if (m_swFrame) {
        av_frame_free(&m_swFrame);
    }
    if (m_cctx) {
        avcodec_free_context(&m_cctx);
    }
    if (m_hwDeviceCtx) {
        av_buffer_unref(&m_hwDeviceCtx);
    }
    m_hwAttached = false;
    if (m_fmt) {
        avformat_close_input(&m_fmt);
    }
    m_streamIdx = -1;
    m_swsSrcW = m_swsSrcH = 0;
    m_swsSrcFmt = -1;
}

bool DualVideoDecoder::initSwsContext(AVFrame *frame)
{
    const bool fresh = !(m_sws
        && m_swsSrcW   == frame->width
        && m_swsSrcH   == frame->height
        && m_swsSrcFmt == frame->format);

    if (fresh) {
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
            qWarning("DualVideoDecoder: sws_getContext failed");
            return false;
        }
        m_swsSrcW   = frame->width;
        m_swsSrcH   = frame->height;
        m_swsSrcFmt = frame->format;
    }

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
            srcCsp = (frame->width >= 1280 || frame->height >= 720)
                     ? SWS_CS_ITU709 : SWS_CS_SMPTE170M;
            break;
    }
    // videoRangeOverride: 0 = Auto (use detected), 1 = Full, 2 = Limited.
    // Mirrors single-flow's metal_player_renderer.mm:1224-1226 and
    // d3d11_vulkan_decode_bridge.cpp:568-572. Apply per-frame so a
    // mid-stream override change (Inspector pill click) takes effect
    // without waiting for a format change to re-init the context.
    const int detectedFullRange =
        (frame->color_range == AVCOL_RANGE_JPEG) ? 1 : 0;
    const int ov = m_rangeOverride.load(std::memory_order_acquire);
    int srcFullRange = detectedFullRange;
    if (ov == 1) srcFullRange = 1;
    else if (ov == 2) srcFullRange = 0;

    sws_setColorspaceDetails(
        m_sws,
        sws_getCoefficients(srcCsp), srcFullRange,
        sws_getCoefficients(SWS_CS_ITU709), /*dstFullRange=*/1,
        /*brightness=*/0, /*contrast=*/1 << 16, /*saturation=*/1 << 16);

    return true;
}

int DualVideoDecoder::frameNumberForPts(int64_t pts) const
{
    if (pts == AV_NOPTS_VALUE) return -1;
    if (m_streamFrameRate.num <= 0 || m_streamFrameRate.den <= 0) return -1;
    if (m_streamTimeBase.num <= 0 || m_streamTimeBase.den <= 0) return -1;

    // frame = round((pts - start_pts) * time_base * fps)
    const double secs = (pts - m_streamStartPts)
                        * static_cast<double>(m_streamTimeBase.num)
                        / static_cast<double>(m_streamTimeBase.den);
    return static_cast<int>(std::lround(secs * m_fps));
}

int64_t DualVideoDecoder::ptsForFrameNumber(int frameNumber) const
{
    if (m_streamFrameRate.num <= 0 || m_streamFrameRate.den <= 0) return 0;
    if (m_streamTimeBase.num <= 0 || m_streamTimeBase.den <= 0) return 0;
    const double secs = static_cast<double>(frameNumber) / m_fps;
    return m_streamStartPts + static_cast<int64_t>(
        secs * m_streamTimeBase.den / m_streamTimeBase.num);
}

bool DualVideoDecoder::needsMoreFrames() const
{
    // G.1.d (2026-05-14): removed the "target buffered + ring full →
    // idle" shortcut that used to live here. It short-circuited the
    // readahead check below: the decoder would idle as soon as the
    // target frame was in the ring, regardless of how many frames
    // were buffered AHEAD of target. The result was the ring sitting
    // at [target-16, target-1] in steady state — zero readahead.
    //
    // macOS hid this with continuous vsync polling: a missed frame
    // at master-frame-advance time would be picked up by the next
    // vsync 16 ms later, once the decoder caught up. Windows is
    // render-on-demand and exposes the miss directly as an Empty
    // payload (= visible stutter post-cache).
    //
    // The readahead check below maintains the proper steady state
    // (ring covers [target-7, target+8]) so pullFrameA(target) hits
    // on every render-thread tick.
    const int target = m_decodeTarget.load(std::memory_order_acquire);
    std::lock_guard<std::mutex> lk(m_bufferMutex);

    int ahead = 0;
    for (int i = 0; i < m_ringCount; ++i) {
        const int idx = (m_ringHead + i) % kRingSize;
        if (m_ring[idx].valid && m_ring[idx].frameNumber >= target) {
            ++ahead;
        }
    }
    return ahead < kRingSize / 2;
}

#if defined(Q_OS_MACOS)
std::shared_ptr<DualFrame>
DualVideoDecoder::makeMetalScrubFrame(AVFrame *frame, int frameNumber)
{
    if (!frame || frame->format != AV_PIX_FMT_VIDEOTOOLBOX || !frame->data[3]) {
        return nullptr;
    }
    // Retain the VideoToolbox CVPixelBuffer and hand back a Metal-kind
    // DualFrame; the compositor's IDualPixbufConverter does YUV→RGB on the
    // GPU (no CPU transfer, no sws_scale). Mirrors convertFrameToRgba's
    // macOS branch — kept separate so the streaming hot path is untouched.
    void *cvPix = static_cast<void *>(frame->data[3]);
    dualCvPixelBufferRetain(cvPix);

    auto out = std::make_shared<DualFrame>();
    out->frameNumber   = frameNumber;
    out->width         = frame->width;
    out->height        = frame->height;
    out->kind          = DualFrame::Kind::Metal;
    out->cvPixelBuffer = std::shared_ptr<void>(cvPix, dualCvPixelBufferRelease);
    out->rangeOverride = m_rangeOverride.load(std::memory_order_acquire);
    return out;
}
#endif

std::shared_ptr<DualFrame>
DualVideoDecoder::convertFrameToRgba(AVFrame *frame, int frameNumber)
{
#if defined(Q_OS_MACOS)
    // Zero-copy fast path: VideoToolbox places the decoded surface
    // in frame->data[3] as a CVPixelBufferRef. Retain it and hand
    // back a Metal-kind DualFrame; the compositor's
    // IDualPixbufConverter does the YUV→RGB on GPU. No CPU transfer,
    // no sws_scale.
    if (frame->format == AV_PIX_FMT_VIDEOTOOLBOX && frame->data[3]) {
        void *cvPix = static_cast<void *>(frame->data[3]);
        dualCvPixelBufferRetain(cvPix);

        auto out = std::make_shared<DualFrame>();
        out->frameNumber = frameNumber;
        out->width       = frame->width;
        out->height      = frame->height;
        out->kind        = DualFrame::Kind::Metal;
        out->cvPixelBuffer = std::shared_ptr<void>(cvPix,
                                                    dualCvPixelBufferRelease);
        // Snapshot the current override so the compositor's YUV→RGB
        // pass picks the right matrix without having to call back into
        // the source. setRangeOverride invalidates the ring on change,
        // so cached frames always reflect the override they were
        // built with.
        out->rangeOverride = m_rangeOverride.load(std::memory_order_acquire);
        return out;
    }
#endif

#if defined(Q_OS_WIN)
    // F.2.12.c — Vulkan zero-copy fast path. FFmpeg's Vulkan hwaccel
    // hands us an AVFrame with AVVkFrame in frame->data[0] living on
    // qcv::VulkanDeviceManager's shared VkDevice. Clone the AVFrame
    // (bumps the AVVkFrame ref) and publish as Kind::Vulkan; the
    // shared_ptr deleter calls av_frame_free when the consumer drops
    // the frame, which drops the AVVkFrame ref and returns the
    // VkImage to FFmpeg's decode pool. Mirrors single-flow's
    // VideoDecoder::publishVulkanFrame + FrameHandle::vulkan exactly.
    if (frame->format == AV_PIX_FMT_VULKAN) {
        AVFrame *cloned = av_frame_clone(frame);
        if (!cloned) {
            qWarning("DualVideoDecoder: av_frame_clone(Vulkan) failed");
            return nullptr;
        }
        auto out = std::make_shared<DualFrame>();
        out->frameNumber = frameNumber;
        out->width       = frame->width;
        out->height      = frame->height;
        out->kind        = DualFrame::Kind::Vulkan;
        out->avFrame     = std::shared_ptr<void>(
            static_cast<void *>(cloned),
            [](void *p) {
                AVFrame *f = static_cast<AVFrame *>(p);
                av_frame_free(&f);
            });
        return out;
    }
#endif

    // Software / HW-fallback path. Pull HW frames to system memory if
    // the platform path didn't catch them above (D3D11VA / VAAPI on
    // non-macOS today; macOS only hits this for non-VT software
    // codecs like ProRes / DNxHD).
    AVFrame *src = frame;
    if (frame->hw_frames_ctx) {
        if (!m_swFrame) {
            qWarning("DualVideoDecoder: HW frame received but swFrame "
                     "not allocated");
            return nullptr;
        }
        av_frame_unref(m_swFrame);
        if (int err = av_hwframe_transfer_data(m_swFrame, frame, 0); err < 0) {
            qWarning("DualVideoDecoder: av_hwframe_transfer_data failed: %s",
                     qPrintable(avErrToString(err)));
            return nullptr;
        }
        m_swFrame->colorspace      = frame->colorspace;
        m_swFrame->color_range     = frame->color_range;
        m_swFrame->color_primaries = frame->color_primaries;
        m_swFrame->color_trc       = frame->color_trc;
        src = m_swFrame;
    }

    if (!initSwsContext(src)) return nullptr;

    auto out = std::make_shared<DualFrame>();
    out->frameNumber = frameNumber;
    out->width       = src->width;
    out->height      = src->height;
    out->kind        = DualFrame::Kind::Cpu;
    out->rgba        = std::make_shared<QImage>(src->width, src->height,
                                                  QImage::Format_RGBA8888);

    uint8_t *dst[4] = { out->rgba->bits(), nullptr, nullptr, nullptr };
    int dstStride[4] = { static_cast<int>(out->rgba->bytesPerLine()), 0, 0, 0 };

    sws_scale(m_sws,
              src->data, src->linesize, 0, src->height,
              dst, dstStride);

    return out;
}

void DualVideoDecoder::setFrameAvailableCallback(FrameAvailableCallback cb)
{
    std::lock_guard<std::mutex> lk(m_callbackMutex);
    m_onFrameAvailable = std::move(cb);
}

void DualVideoDecoder::addCurrentFrameToBuffer(AVFrame *frame, int frameNumber)
{
    auto dualFrame = convertFrameToRgba(frame, frameNumber);
    if (!dualFrame) return;

    // Ring insert in its own scope so the buffer mutex is released
    // before the wake callback runs (the callback calls into the
    // renderer; don't hold a decode-path lock across it).
    {
        std::lock_guard<std::mutex> lk(m_bufferMutex);

        // If frame already buffered (e.g., re-decode after seek),
        // update in place.
        auto existing = m_frameMap.find(frameNumber);
        if (existing != m_frameMap.end()) {
            BufferedFrame &slot = m_ring[existing->second];
            slot.frame = std::move(dualFrame);
            slot.frameNumber = frameNumber;
            slot.valid = true;
        } else {
            int slotIdx;
            if (m_ringCount < kRingSize) {
                slotIdx = (m_ringHead + m_ringCount) % kRingSize;
                ++m_ringCount;
            } else {
                // FIFO eviction — evict the head (oldest) frame.
                slotIdx = m_ringHead;
                m_ringHead = (m_ringHead + 1) % kRingSize;
                BufferedFrame &evicted = m_ring[slotIdx];
                if (evicted.valid) {
                    m_frameMap.erase(evicted.frameNumber);
                }
                evicted.reset();
            }

            BufferedFrame &slot = m_ring[slotIdx];
            slot.frameNumber = frameNumber;
            slot.frame       = std::move(dualFrame);
            slot.valid       = true;
            m_frameMap[frameNumber] = slotIdx;
        }
    }

    // Wake the renderer — a freshly decoded frame is now pullable.
    // Fired on the decode thread; the installed callback's
    // requestUpdate() is thread-safe. This is what redraws the
    // viewport after a timeline seek while paused, when
    // currentFrameChanged already fired before the decode finished.
    FrameAvailableCallback cb;
    {
        std::lock_guard<std::mutex> lk(m_callbackMutex);
        cb = m_onFrameAvailable;
    }
    if (cb) cb();
}

bool DualVideoDecoder::decodeOneFrame(AVFrame *frame, AVPacket *packet)
{
    while (true) {
        const int recvErr = avcodec_receive_frame(m_cctx, frame);
        if (recvErr == AVERROR(EAGAIN)) {
            // Need more packets.
            const int rc = av_read_frame(m_fmt, packet);
            if (rc == AVERROR_EOF) {
                avcodec_send_packet(m_cctx, nullptr);
                continue;
            }
            if (rc < 0) {
                qWarning("DualVideoDecoder: av_read_frame error: %s",
                         qPrintable(avErrToString(rc)));
                return false;
            }
            if (packet->stream_index == m_streamIdx) {
                if (int sendErr = avcodec_send_packet(m_cctx, packet);
                    sendErr < 0 && sendErr != AVERROR(EAGAIN)) {
                    qWarning("DualVideoDecoder: send_packet: %s",
                             qPrintable(avErrToString(sendErr)));
                }
            }
            av_packet_unref(packet);
            continue;
        }
        if (recvErr == AVERROR_EOF) return false;
        if (recvErr < 0) {
            qWarning("DualVideoDecoder: receive_frame: %s",
                     qPrintable(avErrToString(recvErr)));
            return false;
        }

        const int64_t pts = (frame->best_effort_timestamp != AV_NOPTS_VALUE)
                            ? frame->best_effort_timestamp
                            : frame->pts;
        int frameNo = frameNumberForPts(pts);
        if (frameNo < 0) frameNo = 0;

        addCurrentFrameToBuffer(frame, frameNo);
        av_frame_unref(frame);
        return true;
    }
}

void DualVideoDecoder::performSeek(int targetFrame, AVPacket *pkt, AVFrame *frame)
{
    avcodec_flush_buffers(m_cctx);

    {
        std::lock_guard<std::mutex> lk(m_bufferMutex);
        for (auto &slot : m_ring) slot.reset();
        m_frameMap.clear();
        m_ringHead  = 0;
        m_ringCount = 0;
    }

    // Seek to the keyframe at-or-before the target. For intra-only
    // codecs every frame is a keyframe; for inter-frame codecs FFmpeg
    // walks back to the nearest IDR/keyframe.
    const int64_t targetPts = ptsForFrameNumber(targetFrame);
    if (av_seek_frame(m_fmt, m_streamIdx, targetPts, AVSEEK_FLAG_BACKWARD) < 0) {
        qWarning("DualVideoDecoder: av_seek_frame failed for frame %d", targetFrame);
        return;
    }
    avcodec_flush_buffers(m_cctx);

    // Burst-decode forward to the target. For intra codecs this is
    // ~1 frame; for B-frame H.264 it can be up to GOP_size.
    constexpr int kBurstMax = 256;
    int decoded = 0;
    while (decoded < kBurstMax && !m_stopRequested.load(std::memory_order_acquire)) {
        if (!decodeOneFrame(frame, pkt)) break;
        ++decoded;

        // Stop bursting once target is buffered.
        std::lock_guard<std::mutex> lk(m_bufferMutex);
        if (m_frameMap.find(targetFrame) != m_frameMap.end()) break;
    }
}

void DualVideoDecoder::decodeThreadFunc()
{
    AVPacket *pkt   = av_packet_alloc();
    AVFrame  *frame = av_frame_alloc();
    if (!pkt || !frame) {
        qWarning("DualVideoDecoder: av_packet_alloc / av_frame_alloc failed");
        if (pkt) av_packet_free(&pkt);
        if (frame) av_frame_free(&frame);
        return;
    }

    const int timeoutMs = adaptiveTimeoutMs(m_fps);

    while (!m_stopRequested.load(std::memory_order_acquire)) {
        // ---- Pending seek wins over all other work.
        const int seekTarget = m_pendingSeekTarget.exchange(-1, std::memory_order_acq_rel);
        if (seekTarget >= 0) {
            performSeek(seekTarget, pkt, frame);
            continue;
        }

        // ---- Wait until buffer needs more frames (or shutdown / seek /
        // scrub-active transition). The scrub-active gate parks the
        // streaming decode thread while the user drags the timeline —
        // DualScrubDecoder owns the per-side scrub publish slot during
        // that window. Codec contexts stay hot (no flush) so the
        // release-time seekToFrame is a warm seek. Stop and pending-
        // seek still wake regardless so endScrub's seek path takes effect.
        {
            std::unique_lock<std::mutex> lk(m_decodeCvMutex);
            m_decodeCv.wait_for(lk, std::chrono::milliseconds(timeoutMs), [this] {
                return m_stopRequested.load(std::memory_order_acquire) ||
                       m_pendingSeekTarget.load(std::memory_order_acquire) >= 0 ||
                       (!m_scrubActive.load(std::memory_order_acquire)
                        && needsMoreFrames());
            });
        }
        if (m_stopRequested.load(std::memory_order_acquire)) break;
        if (m_pendingSeekTarget.load(std::memory_order_acquire) >= 0) continue;
        if (m_scrubActive.load(std::memory_order_acquire)) continue;
        if (!needsMoreFrames()) continue;

        // ---- Decode one frame (no wall-clock pacing; the master timer
        // controls when the renderer pulls).
        if (!decodeOneFrame(frame, pkt)) {
            // EOF or error — idle on the CV until shutdown / seek.
            std::unique_lock<std::mutex> lk(m_decodeCvMutex);
            m_decodeCv.wait_for(lk, std::chrono::milliseconds(50), [this] {
                return m_stopRequested.load(std::memory_order_acquire) ||
                       m_pendingSeekTarget.load(std::memory_order_acquire) >= 0;
            });
            continue;
        }
    }

    av_packet_free(&pkt);
    av_frame_free(&frame);
}

// ---------------------------------------------------------------------------
// Scrub publish slot (cross-thread). Independent of the ring buffer.
// DualScrubDecoder calls publishExternalFrame from its worker thread;
// the render thread (via DualPlaybackController::pullFrameA/B) calls
// getScrubFrame. Mutex separate from m_bufferMutex.
// ---------------------------------------------------------------------------

void DualVideoDecoder::publishExternalFrame(int frameNumber,
                                              std::shared_ptr<DualFrame> frame)
{
    // frameNumber arg is currently unused — the slot is "latest scrub
    // frame for this side." Kept in the signature for symmetry with
    // single-flow's VideoDecoder::publishExternalFrame and so future
    // logging / drift detection can use it without an API change.
    (void)frameNumber;
    std::lock_guard<std::mutex> lk(m_scrubMutex);
    m_scrubFrame = std::move(frame);
}

std::shared_ptr<DualFrame> DualVideoDecoder::getScrubFrame() const
{
    std::lock_guard<std::mutex> lk(m_scrubMutex);
    return m_scrubFrame;   // copy under lock — caller's lifetime extended
}

void DualVideoDecoder::clearScrubFrame()
{
    std::lock_guard<std::mutex> lk(m_scrubMutex);
    m_scrubFrame.reset();
}

} // namespace qcv::dual
