// MetalPlayerRenderer — see header for adaptation notes.

#include "metal_player_renderer.h"

#include "annotations/stroke_tessellator.h"
#include "annotations/viewport_annotator.h"
#include "color/ocio_config_manager.h"
#include "cv_pixbuf_metal_bridge.h"
#include "decode/frame_handle.h"
#include "decode/image_sequence_cache.h"
#include "decode/video_decoder.h"
// Phase 7.7 — DualCompositor delegate for dual flow.
#include "dual/dual_playback_controller.h"
#include "dual/metal/dual_compositor.h"
#include "dual_pixbuf_converter_impl.h"
#include "gpu_upload_thread.h"
#include "metal_annotation_renderer.h"
#include "metal_compositor.h"
#include "metal_device_manager.h"
#include "metal_hdr_swapchain.h"
#include "metal_ocio_renderer.h"
#include "metal_texture_pool.h"
#include "metal_yuv_renderer.h"
#include "render/player_window.h"
#include "render/safety_overlay.h"

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/CATransaction.h>

#include <QtLogging>
#include <QColorSpace>
#include <QImage>
#include <QWindow>

#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <utility>

namespace qcv {

namespace {

// Pixel-aspect-corrected width for a hover/corner thumbnail. renderCornerOverlay
// derives both the corner-box aspect and the fit-shader srcSize from thumbW, so
// scaling it by the clip's pixel aspect un-squeezes the preview to match the
// main image. 1:1 (or invalid) → unchanged.
inline int parThumbW(int w, int num, int den)
{
    if (w <= 0 || num <= 0 || den <= 0 || num == den) return w;
    return static_cast<int>(std::lround(double(w) * num / den));
}

// Upload a CPU-decoded RGBA8 QImage into a cached MTLTexture.
// Used when the decoder fell back to software (no zero-copy
// VideoToolbox path) — sws_scale produces an RGBA8888 QImage in
// publishCpuFrame which we then push through here onto the GPU.
// The cached texture is recreated only when the frame dims change;
// otherwise replaceRegion uploads new pixels in place. Returns the
// cached texture on success, nil if anything went wrong.
static id<MTLTexture> uploadCpuFrameRgba(
    id<MTLDevice> device,
    const QImage &img,
    __strong id<MTLTexture> &cachedTex,
    int &cachedW, int &cachedH)
{
    if (!device || img.isNull()) return nil;
    const int w = img.width();
    const int h = img.height();
    if (w <= 0 || h <= 0) return nil;

    // publishCpuFrame creates Format_RGBA8888 which matches
    // MTLPixelFormatRGBA8Unorm byte layout directly. Convert
    // defensively in case some other path sneaks in.
    QImage src = (img.format() == QImage::Format_RGBA8888)
                 ? img : img.convertToFormat(QImage::Format_RGBA8888);

    if (cachedTex == nil || cachedW != w || cachedH != h) {
        MTLTextureDescriptor *desc =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:
                MTLPixelFormatRGBA8Unorm
                width:w height:h mipmapped:NO];
        desc.storageMode = MTLStorageModeShared;
        desc.usage = MTLTextureUsageShaderRead;
        cachedTex = [device newTextureWithDescriptor:desc];
        cachedW = w;
        cachedH = h;
    }
    if (!cachedTex) return nil;

    [cachedTex replaceRegion:MTLRegionMake2D(0, 0, w, h)
                 mipmapLevel:0
                   withBytes:src.constBits()
                 bytesPerRow:src.bytesPerLine()];
    return cachedTex;
}

} // namespace

struct MetalPlayerRenderer::Impl {
    id<MTLDevice>       device       = nil;
    id<MTLCommandQueue> commandQueue = nil;
    CAMetalLayer       *layer        = nil;

    MetalHdrSwapchain    hdr;
    // Canvas-side compositor: writes srcA + srcB into compositeRaw
    // (RGBA16F target — color precision floor for the OCIO pass).
    // Per Guide 01 §5 this is the "atomic-pair gate" — both halves
    // come out of one render pass, downstream sees consistent A/B.
    MetalCompositor      compositor;
    // Drawable-side compositor: writes compositeCorrected onto the
    // swapchain texture, plus the background-fill pass + (later)
    // color bars. Baked at the swapchain pixel format which can
    // change with HDR mode flips, so we re-bake when the layer's
    // format changes.
    MetalCompositor      presentCompositor;
    MetalGpuUploadThread upload;

    // Phase A2 — composite intermediates.
    // compositeRaw holds the un-OCIO'd composited output; the OCIO
    // compute pass writes compositeCorrected from it. When OCIO is
    // disengaged we just alias compositeCorrected = compositeRaw
    // and skip the pass. RGBA16F maintains float precision through
    // the entire chain. Capped at 3840x2160 (Guide 01 §5 + user
    // preference); above that, the present pass aspect-fits up
    // into the drawable so display res isn't visually limited.
    id<MTLTexture>       compositeRaw       = nil;
    int                  compositeW         = 0;
    int                  compositeH         = 0;
    static constexpr int kMaxCompositeW     = 3840;
    static constexpr int kMaxCompositeH     = 2160;

    // Phase 7.5 B.6.3 — video YUV→RGBA chain.
    MetalYuvRenderer     yuv;
    CvPixbufMetalBridge  pixbufBridge;

    // Phase 7.5 B.6.4 — OCIO display transform.
    MetalOcioRenderer    ocio;

    // Phase 7.5 B.6.5 — annotation strokes.
    MetalAnnotationRenderer annotations;

    // Strong reference to the most recent video RGBA output. ARC
    // releases when overwritten next frame; Metal's encoder-side
    // retain keeps the texture alive while in flight on the GPU.
    id<MTLTexture>       videoFrameRgba = nil;
    int                  videoFrameW    = 0;
    int                  videoFrameH    = 0;

    // Source B mirror of the above — fed by setVideoDecoderB +
    // fetchLatest in drawFrame. Same lifetime + ARC rules; nil
    // when no B is loaded.
    id<MTLTexture>       videoFrameRgbaB = nil;
    int                  videoFrameWB    = 0;
    int                  videoFrameHB    = 0;
    FrameHandle          videoHandleB;

    // CPU-decoded frame upload — used when the decoder hits a
    // pixel format VideoToolbox can't zero-copy (e.g.
    // yuv444p12le, sw-decoded ProRes 4444 12-bit, anything that
    // takes the publishCpuFrame path in VideoDecoder). Cached
    // RGBA8Unorm textures sized to the frame; replaceRegion on
    // each fetch. One slot per source so A and B don't clobber
    // each other.
    id<MTLTexture>       cpuFrameTexA = nil;
    int                  cpuFrameAW   = 0;
    int                  cpuFrameAH   = 0;
    id<MTLTexture>       cpuFrameTexB = nil;
    int                  cpuFrameBW   = 0;
    int                  cpuFrameBH   = 0;

    // Atomic-pair gate (Guide 01 §5). Each draw, fetchLatest stores
    // freshly-published FrameHandles into these pendings (replacing
    // any previous unconsumed pending — fetchLatest semantics).
    // When in dual-source mode, the gate only commits both
    // textures when both pendings have the same PTS, otherwise it
    // holds the previously-shown pair. Solo case (only A) commits
    // on every A frame.
    FrameHandle          pendingHandleA;
    FrameHandle          pendingHandleB;

    // FrameHandle of the latest video frame. Holds the
    // CVPixelBufferRef alive while the bridge's CVMetalTextureRefs
    // are still wrapping it. Replaced on each fetchLatest hit.
    FrameHandle          videoHandle;

    // Last image-seq frame the renderer successfully showed. Used
    // as a hold-over when the upload thread hasn't produced a
    // texture for the current playhead yet (cache loaded the pixels
    // but GPU upload is one cycle behind, or stride > 1 means the
    // current frame is intentionally off-grid). Without this
    // fallback, the user sees color bars / black flicker between
    // frames at 24 fps content over a 60 Hz vsync render loop.
    //
    // We hold the id<MTLTexture> directly here (ARC retain) instead
    // of just the pool handle: the pool's processPendingDeletions
    // erases from its `m_textures` map *one frame* after queueDelete,
    // so handle-based lookups would return null immediately even
    // though the underlying GPU texture is graveyard-protected. By
    // keeping our own retain we survive any pool eviction; ARC
    // releases when we replace it next frame. Cleared only on
    // cache pointer change (sequence init/shutdown), matching old
    // app's last_good_texture_ policy.
    id<MTLTexture> lastGoodImageSeqTexture = nil;
    int            lastGoodImageSeqW       = 0;
    int            lastGoodImageSeqH       = 0;

    // Screenshot capture state — requestor (GUI thread) flips
    // capturePending; render thread reads it after present, blits
    // the drawable into a shared-mode RGBA8 texture, packs to a
    // QImage, and signals captureCv. Mutex guards all of these.
    std::mutex              captureMutex;
    std::condition_variable captureCv;
    bool                    capturePending = false;
    QImage                  captureImage;

    // Source pixel dimensions of the last successfully-drawn
    // frame. Used to crop the screenshot to the aspect-fit rect
    // so letterbox/pillarbox background isn't included.
    int                     lastDrawnSourceW = 0;
    int                     lastDrawnSourceH = 0;

    // Pointer to the post-OCIO display texture for the most
    // recent drawFrame. Owned by either ocio.output, the image-seq
    // pool, or videoFrameRgba — all alive across the present so
    // we can re-sample the texture in the capture pass below.
    void                   *lastDisplayTexture = nullptr;

    // Pointer to the *raw* source texture for the most recent
    // drawFrame, plus its native dimensions. Lets the capture
    // path render the source 1:1 at native resolution (skipping
    // the window-sized canvas + its letterbox bars) and then run
    // OCIO at source resolution.
    void                   *lastSourceTexture = nullptr;
    int                     lastSourceW       = 0;
    int                     lastSourceH       = 0;

    // Source-resolution intermediate for capture (RGBA16F). Holds
    // a 1:1 render of the source texture so OCIO can run on it
    // post-hoc; reused across captures, lazily resized.
    id<MTLTexture>          captureSourceRgba16f = nil;
    int                     captureSourceRgba16fW = 0;
    int                     captureSourceRgba16fH = 0;

    // Capture-side compositor — second MetalCompositor instance
    // baked at RGBA8Unorm_sRGB (the swapchain's compositor is
    // baked at the layer's pixel format, which for HDR is
    // RGBA16Float and rejects sRGB-encoding writes). Built lazily
    // on the first screenshot.
    MetalCompositor         captureCompositor;
    // Phase 3.H.6 Stage D — separate annotation renderer pinned
    // to RGBA8Unorm (the capture target). Lets the screenshot /
    // export thumbnail path bake strokes into the saved PNG; the
    // primary annotation renderer is bound to the swapchain's
    // pixel format and can't be reused.
    MetalAnnotationRenderer captureAnnotations;

    // Phase 7.7 — dual flow delegate. Initialized lazily when
    // drawFrame first sees rendererMode == DualFlow.
    dual::DualCompositor    dualCompositor;
    int                     dualLastPixelFmt = 0;

    // Loading-spinner timing — render-thread-owned. spinnerStart is set
    // when m_loadingActive is first observed true; the spinner reuses
    // dualCompositor's spin pipeline.
    bool                                  loadingSpinnerWasActive = false;
    std::chrono::steady_clock::time_point loadingSpinnerStart;

    // Task #107/#108 commit 2 — qcv_render-side impl of
    // IDualPixbufConverter; injected into dualCompositor on first
    // dual-mode entry, cleared on shutdown. Owns the per-slot
    // CvPixbufMetalBridge + MetalYuvRenderer so HW-decoded
    // CVPixelBuffers from DualVideoDecoder go YUV→RGB on the GPU
    // without a CPU bounce.
    DualPixbufConverterImpl dualPixbufConverter;

    // Dedicated dual-flow capture compositor — pinned at RGBA8Unorm
    // so the capture texture format matches the pipeline's render-
    // target format regardless of what the layer is currently using
    // (BGRA / RGBA16F-HDR / etc.). Lazily built on the first
    // dual-mode screenshot. Pairs with the single-flow
    // `captureCompositor` above.
    dual::DualCompositor    dualCaptureCompositor;

    // Stage 1 of dual OCIO/FP16 — RGBA16F canvas the dual compositor
    // draws into. A second pass blits it to the drawable via the
    // existing presentCompositor. Mirrors single flow's compositeRaw,
    // kept as a separate texture so the two flows don't fight over
    // sizing rules (single caps at kMaxCompositeW/H; dual matches
    // drawable). Stage 2 will inject MetalOcioRenderer between the
    // dual draw and the present blit.
    id<MTLTexture>          compositeRawDual = nil;
    int                     compositeRawDualW = 0;
    int                     compositeRawDualH = 0;

    int frameCount = 0;
};

MetalPlayerRenderer::MetalPlayerRenderer()
    : m_impl(std::make_unique<Impl>())
{
}

MetalPlayerRenderer::~MetalPlayerRenderer()
{
    shutdown();
}

bool MetalPlayerRenderer::init(PlayerWindow *window)
{
    if (m_running.load()) return true;
    if (!window) {
        qWarning("MetalPlayerRenderer: null window");
        return false;
    }

    auto &mgr = MetalDeviceManager::instance();
    if (!mgr.initialize(window)) {
        qWarning("MetalPlayerRenderer: device manager init failed");
        return false;
    }

    m_impl->device       = (__bridge id<MTLDevice>)       mgr.device();
    m_impl->commandQueue = (__bridge id<MTLCommandQueue>) mgr.commandQueue();
    m_impl->layer        = (__bridge CAMetalLayer *)      mgr.metalLayer();
    if (!m_impl->device || !m_impl->commandQueue || !m_impl->layer) {
        qWarning("MetalPlayerRenderer: device manager returned null handles");
        return false;
    }

    m_impl->hdr.initialize(mgr.metalLayer(), window);
    m_impl->hdr.setMode(m_hdrMode);   // honor any pre-init setHdrMode call

    // Phase 7.5 B.4: shared MetalTexturePool needs to be initialized
    // before the upload thread can hand textures to the renderer.
    MetalTexturePool::instance().initialize();
    // The thumbnail pool is normally initialized lazily by
    // TimelineThumbnailCache on first upload. Initialize it up front
    // (idempotent; matches the cache's 256 MB budget) so any small
    // RGBA8 overlay texture (e.g. hover thumbnails) can be created
    // before the first timeline thumbnail exists.
    MetalTexturePool::thumbnailInstance().initialize(256ULL * 1024 * 1024);

    // Phase 7.5 B.5: start the GPU upload worker. From here on, any
    // PixelData pushed via the cache callback flows here, becomes a
    // pool MTLTexture, and is cached by frame.
    m_impl->upload.start();

    // Phase A2 — canvas compositor bakes at RGBA16F (compositeRaw
    // target); present compositor bakes at the swapchain format
    // (drawable target). Two instances because the pipeline state
    // is per-target-format and we render to two different formats
    // each frame.
    if (!m_impl->compositor.initialize(
            static_cast<int>(MTLPixelFormatRGBA16Float))) {
        qWarning("MetalPlayerRenderer: canvas compositor init failed");
    }
    if (!m_impl->presentCompositor.initialize(
            static_cast<int>(m_impl->layer.pixelFormat),
            /*enableSrcOverBlending=*/true)) {
        qWarning("MetalPlayerRenderer: present compositor init failed");
    }

    // Phase 7.5 B.6.3: bring up the YUV→RGBA compute chain.
    if (!m_impl->yuv.initialize()) {
        qWarning("MetalPlayerRenderer: YUV renderer init failed");
    }
    if (!m_impl->pixbufBridge.initialize()) {
        qWarning("MetalPlayerRenderer: CVPixelBuffer bridge init failed");
    }

    // Phase 7.5 B.6.4: bring up the OCIO compute chain.
    if (!m_impl->ocio.initialize()) {
        qWarning("MetalPlayerRenderer: OCIO renderer init failed");
    }

    // Phase 7.5 B.6.5: annotation render pipeline at the active
    // pixel format (re-baked when HDR mode flips, same as compositor).
    if (!m_impl->annotations.initialize(static_cast<int>(m_impl->layer.pixelFormat))) {
        qWarning("MetalPlayerRenderer: annotation renderer init failed");
    }

    // Phase 7.5 B.6.2: if the cache was already wired before init,
    // hook the GPU push callback now.
    if (m_cache) {
        MetalGpuUploadThread *upload = &m_impl->upload;
        ImageSequenceCache   *cache  = m_cache;
        m_cache->setGpuPushCallback(
            [upload, cache](int frame, std::shared_ptr<const PixelData> pixels) {
                upload->enqueue(frame, std::move(pixels),
                                cache->requestGeneration());
            });
        m_cache->setFrameEvictedCallback(
            [upload](int frame) { upload->evictFrame(frame); });
        m_cacheBoundToUpload = m_cache;
        m_lastCacheGen       = m_cache->requestGeneration();
        m_impl->upload.setGeneration(m_lastCacheGen);
    }

    m_impl->frameCount = 0;

    m_stopRequested.store(false);
    m_running.store(true);
    m_renderThread = std::thread(&MetalPlayerRenderer::renderThreadProc, this);

    qInfo("MetalPlayerRenderer: present loop started "
          "(EDR-supported=%s, headroom=%.2f)",
          m_impl->hdr.isHdrSupported() ? "yes" : "no",
          static_cast<double>(m_impl->hdr.maxEdrHeadroom()));
    return true;
}

void MetalPlayerRenderer::shutdown()
{
    if (!m_running.load()) return;

    m_stopRequested.store(true);
    if (m_renderThread.joinable()) {
        m_renderThread.join();
    }
    m_running.store(false);

    // Detach cache callbacks before tearing down the upload thread —
    // any in-flight cache I/O finish that lands during stop() would
    // otherwise call into our soon-to-be-destroyed upload object via
    // the lambda's captured pointer.
    if (m_cache) {
        m_cache->setGpuPushCallback(nullptr);
        m_cache->setFrameEvictedCallback(nullptr);
    }
    m_cacheBoundToUpload = nullptr;

    // Stop the upload worker before tearing down the pool: it may
    // still be enqueueing texture creates against the device.
    m_impl->upload.stop();

    // Drop ALL the WindowManager-owned pointers without touching
    // them. WindowManager destructs BEFORE ~PlayerWindow (Qt's
    // child-window cleanup runs after WindowManager's own destructor
    // returns), so any of these can be dangling at the time
    // ~MetalPlayerRenderer fires. We don't dereference them here;
    // the proper detach is via the setX(nullptr) flow earlier.
    m_cache     = nullptr;
    m_decoder   = nullptr;
    m_ocio      = nullptr;
    m_annotator = nullptr;
    m_safety    = nullptr;

    // The CAMetalLayer is still attached to the NSView and will be
    // released when the view's layer reference drops. We just clear
    // our own non-owning copies.
    m_impl->yuv.shutdown();
    m_impl->pixbufBridge.shutdown();
    m_impl->ocio.shutdown();
    m_impl->annotations.shutdown();
    m_impl->videoFrameRgba           = nil;
    m_impl->videoHandle              = FrameHandle{};
    m_impl->lastGoodImageSeqTexture  = nil;
    m_impl->lastGoodImageSeqW        = 0;
    m_impl->lastGoodImageSeqH        = 0;
    m_impl->lastDisplayTexture       = nullptr;
    m_impl->lastSourceTexture        = nullptr;
    m_impl->lastSourceW              = 0;
    m_impl->lastSourceH              = 0;
    m_impl->captureSourceRgba16f     = nil;
    m_impl->captureSourceRgba16fW    = 0;
    m_impl->captureSourceRgba16fH    = 0;
    m_impl->compositeRaw             = nil;
    m_impl->compositeW               = 0;
    m_impl->compositeH               = 0;
    m_impl->compositeRawDual         = nil;
    m_impl->compositeRawDualW        = 0;
    m_impl->compositeRawDualH        = 0;
    m_impl->compositor.shutdown();
    m_impl->presentCompositor.shutdown();
    m_impl->captureCompositor.shutdown();
    m_impl->dualCaptureCompositor.shutdown();
    m_impl->hdr.shutdown();
    m_impl->layer        = nil;
    m_impl->commandQueue = nil;
    m_impl->device       = nil;

    qInfo("MetalPlayerRenderer: shut down (rendered %d frames)",
          m_impl->frameCount);
}

void MetalPlayerRenderer::requestUpdate()
{
    // Phase B.2: the render thread runs an unconditional loop, so
    // there's nothing to wake. Phase B.6.2 switches to event-driven
    // updates and this becomes a condition_variable notify.
}

void MetalPlayerRenderer::resize(int width, int height)
{
    if (!m_impl->layer) return;
    // CAMetalLayer.drawableSize is in pixels; QWindow gives logical
    // points, so we'd want to multiply by devicePixelRatio.
    const CGFloat dpr = m_impl->layer.contentsScale > 0
        ? m_impl->layer.contentsScale : 1.0;
    m_impl->layer.drawableSize = CGSizeMake(width * dpr, height * dpr);

    // Our CAMetalLayer is a sublayer of Qt's QContainerLayer; its
    // frame doesn't auto-track the parent's bounds. Update it here
    // so the rendered content fills the window. CATransaction with
    // disabled animations matches old-app pattern — no implicit
    // frame animation on rapid resize.
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    m_impl->layer.frame = CGRectMake(0, 0, width, height);
    [CATransaction commit];
}

void MetalPlayerRenderer::setHdrMode(HdrMode mode)
{
    m_hdrMode = mode;
    // Render thread picks up the new mode before nextDrawable: in
    // applyPendingOnRenderThread(). Safe to call before init().
    // The compositor pipeline needs to be re-baked at the new pixel
    // format too; render thread does that lazily before drawing.
    m_impl->hdr.setMode(mode);
}

void MetalPlayerRenderer::setBrightness(float brightness)
{
    // Applied in the present-blit (drawable target) compositor —
    // single difference from D3D11, where one `compositor` does both
    // the pre-OCIO source draw (single flow) and the post-OCIO present
    // blit (dual flow). Metal has separate canvas + present compositors,
    // and the present one is the single pass common to both flows;
    // routing brightness there lights up the slider in single AND dual
    // with one apply.
    //
    // Correctness: SDR is linear gain commuting with the View transform
    // (identical to pre-OCIO). HDR on macOS is EDR (extended-linear sRGB
    // / Display P3 per metal_hdr_swapchain.h:6) — values >1.0 are
    // scene-referred linear nits, so a scalar multiply IS a real
    // exposure adjustment with headroom-aware behavior. No PQ on this
    // path; that caveat only applies to the Windows/Vulkan HDR route.
    //
    // Render loop bakes the value into the UBO on the next frame; no
    // requestUpdate needed (Phase B.2 free-running loop).
    m_impl->presentCompositor.setBrightness(brightness);
}

void MetalPlayerRenderer::setImageSeqCache(ImageSequenceCache *c)
{
    // Pointer-change wiping: when the active sequence changes (or
    // unloads to nullptr), drop every GPU texture from the upload
    // thread's map. Otherwise, returning to a previously loaded
    // sequence would briefly show a stale frame from the OLD sequence
    // because the upload thread still has its pre-unload textures
    // keyed by frame number.
    const bool pointerChanged = (c != m_cacheBoundToUpload);
    if (pointerChanged && m_running.load()) {
        m_impl->upload.clearAll();
        m_impl->lastGoodImageSeqTexture = nil;
        m_impl->lastGoodImageSeqW = 0;
        m_impl->lastGoodImageSeqH = 0;
    }

    m_cache = c;

    if (!c) {
        m_cacheBoundToUpload = nullptr;
        m_lastCacheGen       = 0;
        return;
    }

    // Phase 7.5 B.6.2: cache freshly loaded frames flow through here.
    // The lambda holds a raw pointer to the upload thread, which is
    // owned by Impl and lives as long as `this`. The cache callback
    // is cleared in shutdown() before the upload thread is torn down,
    // so the pointer can't dangle.
    if (m_running.load()) {
        MetalGpuUploadThread *upload = &m_impl->upload;
        ImageSequenceCache   *cache  = c;
        c->setGpuPushCallback(
            [upload, cache](int frame, std::shared_ptr<const PixelData> pixels) {
                upload->enqueue(frame, std::move(pixels),
                                cache->requestGeneration());
            });
        // Cache-thread eviction sink: keeps the upload thread's
        // texture map bounded by the cache window. Without this, the
        // map grows to the full sequence length over time and old
        // textures (which the pool may have already evicted from
        // under us) hang around as dangling handles.
        c->setFrameEvictedCallback(
            [upload](int frame) { upload->evictFrame(frame); });
        m_cacheBoundToUpload = c;
        m_lastCacheGen       = c->requestGeneration();
        // Reset the upload thread's epoch to the new cache's — its
        // generation gate would otherwise judge the new cache's
        // items against the previous bind's counter.
        m_impl->upload.setGeneration(m_lastCacheGen);
    }
    // If !running, init() will rewire when it starts up.
}
void MetalPlayerRenderer::setVideoDecoder(VideoDecoder *d)          { m_decoder  = d; }
void MetalPlayerRenderer::setVideoDecoderB(VideoDecoder *d)
{
    m_decoderB = d;
    if (!d) clearSourceBState();
}

void MetalPlayerRenderer::clearSourceAState()
{
    m_impl->videoFrameRgba    = nil;
    m_impl->videoFrameW       = 0;
    m_impl->videoFrameH       = 0;
    m_impl->videoHandle       = FrameHandle{};
    m_impl->pendingHandleA    = FrameHandle{};
    m_impl->cpuFrameTexA      = nil;
    m_impl->cpuFrameAW        = 0;
    m_impl->cpuFrameAH        = 0;
}

void MetalPlayerRenderer::clearSourceBState()
{
    m_impl->videoFrameRgbaB   = nil;
    m_impl->videoFrameWB      = 0;
    m_impl->videoFrameHB      = 0;
    m_impl->videoHandleB      = FrameHandle{};
    m_impl->pendingHandleB    = FrameHandle{};
    m_impl->cpuFrameTexB      = nil;
    m_impl->cpuFrameBW        = 0;
    m_impl->cpuFrameBH        = 0;
}
void MetalPlayerRenderer::setViewportAnnotator(ViewportAnnotator *a){ m_annotator = a; }
void MetalPlayerRenderer::setSafetyOverlay(SafetyOverlay *s)        { m_safety    = s; }
void MetalPlayerRenderer::setOcio(OCIOConfigManager *o)             { m_ocio     = o; }
void MetalPlayerRenderer::setCompositorMode(CompositorMode m)
{
    m_compMode = m;
    // Mirror to the dual flow's compositor — when in DualFlow mode
    // the same SBS/Wipe/Single uniform drives the dual shader.
    if (m_impl) {
        m_impl->dualCompositor.setMode(
            static_cast<dual::DualCompositor::Mode>(static_cast<int>(m)));
    }
}
void MetalPlayerRenderer::setSplitPos(float p)
{
    m_splitPos = p;
    if (m_impl) m_impl->dualCompositor.setSplitPos(p);
}
void MetalPlayerRenderer::setDiffGain(float g)
{
    if (m_impl) m_impl->dualCompositor.setDiffGain(g);
}
void MetalPlayerRenderer::setSplitSeamHighlight(float h)
{
    if (m_impl) m_impl->dualCompositor.setSeamHighlight(h);
}
void MetalPlayerRenderer::setLoadingActive(bool on)
{
    m_loadingActive.store(on);
    requestUpdate();
}
void MetalPlayerRenderer::setBackgroundMode(BackgroundMode m)       { m_bgMode = m; }
void MetalPlayerRenderer::setSourceActivity(bool aActive, bool bActive)
{
    m_aActive.store(aActive, std::memory_order_relaxed);
    m_bActive.store(bActive, std::memory_order_relaxed);
}

void MetalPlayerRenderer::setPixelAspectA(int num, int den)
{
    m_parNumA.store(num > 0 ? num : 1, std::memory_order_relaxed);
    m_parDenA.store(den > 0 ? den : 1, std::memory_order_relaxed);
    // Mirror into the dual compositor so dual flow tracks the same
    // per-side ratio (single flow reads the atomics directly).
    if (m_impl) m_impl->dualCompositor.setPixelAspectA(num, den);
}

void MetalPlayerRenderer::setPixelAspectB(int num, int den)
{
    m_parNumB.store(num > 0 ? num : 1, std::memory_order_relaxed);
    m_parDenB.store(den > 0 ? den : 1, std::memory_order_relaxed);
    if (m_impl) m_impl->dualCompositor.setPixelAspectB(num, den);
}

void MetalPlayerRenderer::setRendererMode(RendererMode m)
{
    m_rendererMode.store(static_cast<int>(m), std::memory_order_release);
}

void MetalPlayerRenderer::setHoverThumbnail(unsigned long long handle,
                                             int srcW, int srcH)
{
    m_hoverThumbHandle.store(handle, std::memory_order_release);
    m_hoverThumbW.store(srcW, std::memory_order_relaxed);
    m_hoverThumbH.store(srcH, std::memory_order_relaxed);
}

void MetalPlayerRenderer::setHoverThumbnailB(unsigned long long handle,
                                              int srcW, int srcH)
{
    m_hoverThumbHandleB.store(handle, std::memory_order_release);
    m_hoverThumbWB.store(srcW, std::memory_order_relaxed);
    m_hoverThumbHB.store(srcH, std::memory_order_relaxed);
}

void MetalPlayerRenderer::setStoredAnnotationStrokes(
    const std::vector<ActiveStroke> &strokes)
{
    std::lock_guard<std::mutex> lock(m_storedStrokesMutex);
    m_storedStrokes = strokes;
}

void MetalPlayerRenderer::setDualController(void *controllerPtr)
{
    m_dualControllerPtr.store(controllerPtr, std::memory_order_release);
    if (m_impl) {
        auto *c = static_cast<dual::DualPlaybackController *>(controllerPtr);
        m_impl->dualCompositor.setController(c);

        // Lazy-init + inject the pixbuf converter on first dual entry.
        // Cleared on null controller (dual exit) so the compositor
        // doesn't dangle a pointer through a renderer reset.
        if (c) {
            if (!m_impl->dualPixbufConverter.isInitialized()) {
                m_impl->dualPixbufConverter.initialize();
            }
            m_impl->dualCompositor.setPixbufConverter(
                &m_impl->dualPixbufConverter);
        } else {
            m_impl->dualCompositor.setPixbufConverter(nullptr);
        }

        // Dual screenshot path samples the live `dualCorrected`
        // RGBA16F canvas directly via captureCompositor — no second
        // compositor instance to wire here. The dualCaptureCompositor
        // field is kept on Impl but no longer used; it can be pruned
        // in a follow-up.
    }
}

void MetalPlayerRenderer::renderThreadProc()
{
    // The thread runs a tight loop; presentDrawable: blocks at vsync
    // when maximumDrawableCount (3) drawables are in flight, giving
    // us natural pacing without a CADisplayLink.
    while (!m_stopRequested.load()) {
        @autoreleasepool {
            drawFrame();
        }
    }
}

void MetalPlayerRenderer::drawFrame()
{
    // Apply any pending HDR mode change before acquiring the drawable.
    // The new pixel format takes effect on the next nextDrawable: call.
    m_impl->hdr.applyPendingOnRenderThread();

    // Reset the annotation renderer's vertex-buffer append cursor
    // for this frame. Multiple drawMesh calls can happen per frame
    // (per-side safety in dual SBS, stroke + safety, etc.) and they
    // need to land at distinct buffer offsets.
    if (m_impl->annotations.isInitialized()) {
        m_impl->annotations.beginFrame();
    }

    // Phase 7.7 — dual flow delegate. When the user enters dual test
    // mode (or a real Dual compositor mode in Stage 4), the renderer
    // skips the single-flow pipeline entirely and runs a minimal
    // pass that just delegates to DualCompositor. The dual island
    // owns its own decoders, pool, and master clock — all this
    // method does is acquire the drawable + run the compositor.
    const RendererMode mode = static_cast<RendererMode>(
        m_rendererMode.load(std::memory_order_acquire));
    if (mode == RendererMode::DualFlow) {
        // Stage 1 of dual OCIO/FP16 — bake the dual compositor's
        // pipeline at RGBA16F and render into a canvas intermediate
        // (compositeRawDual), then present-blit to the drawable via
        // the existing presentCompositor (already format-aware for
        // the layer's pixel format). Stage 2 will inject OCIO between
        // the two passes; for now this is a no-op refactor that sets
        // up the seam.
        constexpr int kCanvasFmt =
            static_cast<int>(MTLPixelFormatRGBA16Float);
        if (m_impl->dualLastPixelFmt != kCanvasFmt ||
            !m_impl->dualCompositor.isInitialized()) {
            m_impl->dualCompositor.shutdown();
            m_impl->dualCompositor.initialize(
                kCanvasFmt, (__bridge void *)m_impl->device);
            m_impl->dualLastPixelFmt = kCanvasFmt;
        }

        id<CAMetalDrawable> drawable = [m_impl->layer nextDrawable];
        if (!drawable) return;

        const int dstW = static_cast<int>(drawable.texture.width);
        const int dstH = static_cast<int>(drawable.texture.height);

        // Allocate / resize the FP16 canvas, capped at the same
        // kMaxComposite{W,H} ceiling the single flow uses. Without
        // this cap a 5K HiDPI drawable produces a ~14.7M-pixel canvas
        // that OCIO + the dual compositor have to process every
        // frame — slow even on M-series. The cap mirrors single's
        // 4K canvas; the present blit upscales to drawable size, so
        // perceptual fidelity at typical viewing distances is
        // unchanged.
        const int canvasW = std::max(1, std::min(dstW, Impl::kMaxCompositeW));
        const int canvasH = std::max(1, std::min(dstH, Impl::kMaxCompositeH));
        if (!m_impl->compositeRawDual
            || m_impl->compositeRawDualW != canvasW
            || m_impl->compositeRawDualH != canvasH) {
            MTLTextureDescriptor *desc =
                [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:
                    MTLPixelFormatRGBA16Float
                    width:canvasW height:canvasH mipmapped:NO];
            desc.storageMode = MTLStorageModePrivate;
            desc.usage = MTLTextureUsageShaderRead
                       | MTLTextureUsageShaderWrite
                       | MTLTextureUsageRenderTarget;
            m_impl->compositeRawDual =
                [m_impl->device newTextureWithDescriptor:desc];
            m_impl->compositeRawDualW = canvasW;
            m_impl->compositeRawDualH = canvasH;
        }

        // Re-init presentCompositor at the layer's pixel format if
        // it changed (HDR toggle flips the layer between BGRA8 and
        // RGBA16F). Same rebake rule as the single-flow branch.
        const int currentLayerFmt =
            static_cast<int>(m_impl->layer.pixelFormat);
        if (m_impl->presentCompositor.targetPixelFormat() != currentLayerFmt) {
            m_impl->presentCompositor.shutdown();
            m_impl->presentCompositor.initialize(
                currentLayerFmt, /*enableSrcOverBlending=*/true);
        }

        id<MTLCommandBuffer> cb = [m_impl->commandQueue commandBuffer];

        // Pre-render: pull frames from controller, run YUV→RGB
        // compute for any HW-decoded frames, populate per-side
        // cached RGBA textures. Encoded on `cb` BEFORE the render
        // pass starts so the composite draw sees the freshly-
        // converted textures via Metal's intra-cb dependency
        // tracking (no waitUntilCompleted).
        m_impl->dualCompositor.prepareFrames((__bridge void *)cb);

        // ---- Pass 1: dual composite → compositeRawDual (RGBA16F) ----
        {
            MTLRenderPassDescriptor *rpd =
                [MTLRenderPassDescriptor renderPassDescriptor];
            rpd.colorAttachments[0].texture     = m_impl->compositeRawDual;
            rpd.colorAttachments[0].loadAction  = MTLLoadActionClear;
            rpd.colorAttachments[0].clearColor  =
                MTLClearColorMake(0, 0, 0, 0);
            rpd.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLRenderCommandEncoder> enc =
                [cb renderCommandEncoderWithDescriptor:rpd];
            // Dual compositor draws at canvas size, not drawable
            // size. The present blit handles the canvas → drawable
            // upscale below.
            m_impl->dualCompositor.renderFrame(
                (__bridge void *)enc, canvasW, canvasH);

            // Phase 3.H.5 — hover-thumbnail corner overlays for dual
            // flow. Same drawing pass as the dual composite (writes
            // into compositeRawDual, picked up by the OCIO pass
            // below). A → BL, B → BR. Reuses the canvas compositor
            // (RGBA16F-baked, same pixel format as compositeRawDual).
            // See drawFrame's single-flow branch for the same logic.
            if (m_impl->compositor.isInitialized()) {
                const unsigned long long thumbHandleA =
                    m_hoverThumbHandle.load(std::memory_order_acquire);
                if (thumbHandleA != 0) {
                    const auto *meta = MetalTexturePool::thumbnailInstance()
                                           .texture(thumbHandleA);
                    if (meta && meta->valid && meta->texture) {
                        m_impl->compositor.renderCornerOverlay(
                            (__bridge void *)enc,
                            meta->texture,
                            parThumbW(meta->width,
                                      m_parNumA.load(std::memory_order_relaxed),
                                      m_parDenA.load(std::memory_order_relaxed)),
                            meta->height,
                            canvasW, canvasH,
                            /*corner=*/0, 0.18f, 12.0f);
                    }
                }
                const unsigned long long thumbHandleB =
                    m_hoverThumbHandleB.load(std::memory_order_acquire);
                if (thumbHandleB != 0) {
                    const auto *meta = MetalTexturePool::thumbnailInstance()
                                           .texture(thumbHandleB);
                    if (meta && meta->valid && meta->texture) {
                        m_impl->compositor.renderCornerOverlay(
                            (__bridge void *)enc,
                            meta->texture,
                            parThumbW(meta->width,
                                      m_parNumB.load(std::memory_order_relaxed),
                                      m_parDenB.load(std::memory_order_relaxed)),
                            meta->height,
                            canvasW, canvasH,
                            /*corner=*/1, 0.18f, 12.0f);
                    }
                }
            }
            [enc endEncoding];
        }

        // ---- Pass 1b: OCIO compute pass (compositeRawDual → corrected) ----
        // Per Guide 01 §7 OCIO is post-composite — one chain over the
        // canvas, not per-side. Reuses the same MetalOcioRenderer the
        // single flow uses; the renderer modes are mutually exclusive
        // so there's no contention. When OCIO is disengaged or has
        // no pipeline, `dualCorrected` aliases compositeRawDual (no
        // copy) and the present blit reads from the raw canvas.
        void *dualCorrected = (__bridge void *)m_impl->compositeRawDual;
        if (m_ocio && m_ocio->engaged()) {
            m_impl->ocio.rebuild(m_ocio);
            if (m_impl->ocio.hasPipeline()) {
                void *ocioOut = m_impl->ocio.apply(
                    (__bridge void *)cb,
                    (__bridge void *)m_impl->compositeRawDual,
                    m_impl->compositeRawDualW,
                    m_impl->compositeRawDualH);
                if (ocioOut) dualCorrected = ocioOut;
            }
        }

        // ---- Pass 2: present blit canvas → drawable ----
        // Background pass first (covers the whole drawable, includes
        // the alpha=0 letterbox / past-end regions); then a 1:1
        // single-mode aspect-fit blit of the RGBA16F (OCIO-corrected)
        // canvas onto the layer.
        {
            MTLRenderPassDescriptor *rpd =
                [MTLRenderPassDescriptor renderPassDescriptor];
            rpd.colorAttachments[0].texture     = drawable.texture;
            rpd.colorAttachments[0].loadAction  = MTLLoadActionClear;
            rpd.colorAttachments[0].storeAction = MTLStoreActionStore;
            rpd.colorAttachments[0].clearColor  =
                MTLClearColorMake(0, 0, 0, 1);
            id<MTLRenderCommandEncoder> enc =
                [cb renderCommandEncoderWithDescriptor:rpd];
            if (m_impl->presentCompositor.isInitialized()) {
                const float tilePixels = 20.0f *
                    static_cast<float>(m_impl->layer.contentsScale > 0
                                       ? m_impl->layer.contentsScale : 1.0);
                m_impl->presentCompositor.renderBackground(
                    (__bridge void *)enc, static_cast<int>(m_bgMode),
                    dstW, dstH, tilePixels);
                m_impl->presentCompositor.renderSingle(
                    (__bridge void *)enc,
                    dualCorrected,
                    canvasW, canvasH, dstW, dstH);
            }

            // Per-side safety overlay. Mirrors the dual compositor's
            // aspect-fit layout so each side's safety markers track
            // its own source aspect ratio (mixed-aspect dual, e.g.
            // 16:9 vs 1.85, gets correct safe-action / safe-title
            // rectangles per side instead of the canvas-fit single
            // overlay). v1 uses one shared SafetyOverlay preset for
            // both sides; per-side preset selection is a follow-up.
            if (m_safety && m_safety->isLoaded()
                && m_impl->annotations.isInitialized()
                && m_dualControllerPtr.load(std::memory_order_acquire)) {
                auto *ctl = static_cast<dual::DualPlaybackController *>(
                    m_dualControllerPtr.load(std::memory_order_acquire));
                auto *srcA = ctl ? ctl->sourceA() : nullptr;
                auto *srcB = ctl ? ctl->sourceB() : nullptr;

                auto fit = [](qreal x, qreal y, qreal w, qreal h,
                               qreal sw, qreal sh) -> QRectF {
                    if (sw <= 0 || sh <= 0 || w <= 0 || h <= 0) {
                        return QRectF(x, y, w, h);
                    }
                    const qreal containerAR = w / h;
                    const qreal sourceAR    = sw / sh;
                    qreal fw, fh;
                    if (sourceAR > containerAR) {
                        fw = w; fh = w / sourceAR;
                    } else {
                        fh = h; fw = h * sourceAR;
                    }
                    return QRectF(x + (w - fw) * 0.5,
                                  y + (h - fh) * 0.5, fw, fh);
                };

                // r is the side's displayed (pixel-aspect-corrected)
                // image rect; rawW/rawH are the side's square source
                // dims. meshForImage stretches the guide with the
                // picture exactly as the image un-squeezes.
                auto draw = [&](const QRectF &r, qreal rawW, qreal rawH) {
                    if (r.width() < 2 || r.height() < 2) return;
                    const TessellatedMesh m =
                        m_safety->meshForImage(r.topLeft(), r.size(),
                                               rawW, rawH);
                    if (m.vertices.empty()) return;
                    m_impl->annotations.drawMesh(
                        (__bridge void *)enc, m, dstW, dstH);
                };

                // Pixel-aspect-corrected per-side dims drive the FIT
                // rect (so the guide sits on the un-squeezed image);
                // the raw square dims drive meshForImage's source fit.
                auto effD = [](qreal w, qreal h, int n, int d)
                    -> std::pair<qreal, qreal> {
                    if (w <= 0 || h <= 0 || n <= 0 || d <= 0 || n == d)
                        return { w, h };
                    if (n > d) return { w * n / d, h };
                    return { w, h * d / n };
                };
                const qreal rawAW = srcA ? srcA->width()  : 0.0;
                const qreal rawAH = srcA ? srcA->height() : 0.0;
                const qreal rawBW = srcB ? srcB->width()  : 0.0;
                const qreal rawBH = srcB ? srcB->height() : 0.0;
                const auto [sAW, sAH] = effD(
                    rawAW, rawAH,
                    m_parNumA.load(std::memory_order_relaxed),
                    m_parDenA.load(std::memory_order_relaxed));
                const auto [sBW, sBH] = effD(
                    rawBW, rawBH,
                    m_parNumB.load(std::memory_order_relaxed),
                    m_parDenB.load(std::memory_order_relaxed));

                // Mode 1 = SBS: A in left half, B in right half.
                // Mode 2 = Wipe: both at full drawable; v1 draws
                // only A's safety to avoid overlapping markers
                // (per-side wipe-clipped safety is a follow-up).
                // Mode 0 = Single: A only at full drawable (this
                // branch only runs in dual mode, so Single ≡ A
                // alone via the dual single-source codepath).
                if (m_compMode == CompositorMode::SideBySide) {
                    if (srcA) draw(fit(0, 0, dstW * 0.5, dstH, sAW, sAH),
                                   rawAW, rawAH);
                    if (srcB) draw(fit(dstW * 0.5, 0,
                                        dstW * 0.5, dstH, sBW, sBH),
                                   rawBW, rawBH);
                } else {
                    if (srcA) draw(fit(0, 0, dstW, dstH, sAW, sAH),
                                   rawAW, rawAH);
                }
            }

            [enc endEncoding];
        }

        // Dual-mode screenshot capture. Dump the post-OCIO canvas
        // (`dualCorrected`, RGBA16F at canvasW × canvasH) directly
        // into an RGBA8 shared texture via a 1:1 blit. The canvas
        // size tracks the drawable (capped at 4K), so screenshots
        // come out at viewport resolution. Native-source-resolution
        // capture would need a separate full-res render pass; that
        // attempt broke dual playback and was reverted.
        bool wantCapture = false;
        {
            std::lock_guard<std::mutex> lk(m_impl->captureMutex);
            wantCapture = m_impl->capturePending;
        }
        if (wantCapture && dualCorrected
            && canvasW > 0 && canvasH > 0) {
            @autoreleasepool {
                constexpr int kCaptureFmt =
                    static_cast<int>(MTLPixelFormatRGBA8Unorm);
                if (!m_impl->captureCompositor.isInitialized()
                    || m_impl->captureCompositor.targetPixelFormat()
                        != kCaptureFmt) {
                    m_impl->captureCompositor.shutdown();
                    m_impl->captureCompositor.initialize(kCaptureFmt);
                }

                MTLTextureDescriptor *dstDesc =
                    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:
                        MTLPixelFormatRGBA8Unorm
                        width:canvasW height:canvasH mipmapped:NO];
                dstDesc.storageMode = MTLStorageModeShared;
                dstDesc.usage = MTLTextureUsageShaderRead
                              | MTLTextureUsageRenderTarget;
                id<MTLTexture> dstTex =
                    [m_impl->device newTextureWithDescriptor:dstDesc];

                MTLRenderPassDescriptor *capRpd =
                    [MTLRenderPassDescriptor renderPassDescriptor];
                capRpd.colorAttachments[0].texture     = dstTex;
                capRpd.colorAttachments[0].loadAction  = MTLLoadActionClear;
                capRpd.colorAttachments[0].clearColor  =
                    MTLClearColorMake(0, 0, 0, 0);
                capRpd.colorAttachments[0].storeAction = MTLStoreActionStore;
                id<MTLRenderCommandEncoder> capEnc =
                    [cb renderCommandEncoderWithDescriptor:capRpd];

                m_impl->captureCompositor.renderSource(
                    (__bridge void *)capEnc,
                    dualCorrected,
                    canvasW, canvasH, canvasW, canvasH,
                    MetalCompositor::Single, 0.5f);

                if (m_safety && m_safety->isLoaded()
                    && m_impl->annotations.isInitialized()
                    && m_dualControllerPtr.load(std::memory_order_acquire)) {
                    auto *ctl = static_cast<dual::DualPlaybackController *>(
                        m_dualControllerPtr.load(std::memory_order_acquire));
                    auto *srcA = ctl ? ctl->sourceA() : nullptr;
                    auto *srcB = ctl ? ctl->sourceB() : nullptr;

                    auto fit = [](qreal x, qreal y, qreal w, qreal h,
                                   qreal sw, qreal sh) -> QRectF {
                        if (sw <= 0 || sh <= 0 || w <= 0 || h <= 0) {
                            return QRectF(x, y, w, h);
                        }
                        const qreal containerAR = w / h;
                        const qreal sourceAR    = sw / sh;
                        qreal fw, fh;
                        if (sourceAR > containerAR) {
                            fw = w; fh = w / sourceAR;
                        } else {
                            fh = h; fw = h * sourceAR;
                        }
                        return QRectF(x + (w - fw) * 0.5,
                                      y + (h - fh) * 0.5, fw, fh);
                    };
                    auto draw = [&](const QRectF &r) {
                        if (r.width() < 2 || r.height() < 2) return;
                        const TessellatedMesh m =
                            m_safety->mesh(r.topLeft(), r.size());
                        if (m.vertices.empty()) return;
                        m_impl->annotations.drawMesh(
                            (__bridge void *)capEnc, m, canvasW, canvasH);
                    };

                    const qreal sAW = srcA ? srcA->width() : 0.0;
                    const qreal sAH = srcA ? srcA->height() : 0.0;
                    const qreal sBW = srcB ? srcB->width() : 0.0;
                    const qreal sBH = srcB ? srcB->height() : 0.0;
                    if (m_compMode == CompositorMode::SideBySide) {
                        if (srcA) draw(fit(0, 0,
                                           canvasW * 0.5, canvasH, sAW, sAH));
                        if (srcB) draw(fit(canvasW * 0.5, 0,
                                           canvasW * 0.5, canvasH, sBW, sBH));
                    } else {
                        if (srcA) draw(fit(0, 0, canvasW, canvasH, sAW, sAH));
                    }
                }

                [capEnc endEncoding];

                [cb presentDrawable:drawable];
                [cb commit];
                [cb waitUntilCompleted];

                QImage img(canvasW, canvasH, QImage::Format_RGBA8888);
                [dstTex getBytes:img.bits()
                    bytesPerRow:img.bytesPerLine()
                     fromRegion:MTLRegionMake2D(0, 0, canvasW, canvasH)
                    mipmapLevel:0];
                img.setColorSpace(QColorSpace(QColorSpace::SRgb));

                std::lock_guard<std::mutex> lk(m_impl->captureMutex);
                m_impl->captureImage   = img.copy();
                m_impl->capturePending = false;
            }
            m_impl->captureCv.notify_all();
            ++m_impl->frameCount;
            return;
        }

        [cb presentDrawable:drawable];
        [cb commit];
        return;
    }

    // Pipelines that target the SWAPCHAIN re-bake when its pixel
    // format flips on HDR mode change. The canvas compositor stays
    // pinned at RGBA16F (compositeRaw's format), so it doesn't
    // need to re-bake here.
    const int currentLayerFmt = static_cast<int>(m_impl->layer.pixelFormat);
    if (m_impl->presentCompositor.targetPixelFormat() != currentLayerFmt) {
        m_impl->presentCompositor.shutdown();
        m_impl->presentCompositor.initialize(
            currentLayerFmt, /*enableSrcOverBlending=*/true);
    }
    if (m_impl->annotations.targetPixelFormat() != currentLayerFmt) {
        m_impl->annotations.shutdown();
        m_impl->annotations.initialize(currentLayerFmt);
    }

    // Phase 7.5 B.5: graveyard tick — release any deferred-delete
    // pool textures whose 3-frame quarantine has expired. Both the
    // main pool (image-seq evictions) and the thumbnail pool (hover-
    // thumbnail evictions) must be drained; the thumbnail drain was
    // missing, leaking thumb textures until shutdown.
    MetalTexturePool::instance().processPendingDeletions();
    MetalTexturePool::thumbnailInstance().processPendingDeletions();

    id<CAMetalDrawable> drawable = [m_impl->layer nextDrawable];
    if (!drawable) {
        // nextDrawable can return nil if the layer is configured for
        // non-drawing (e.g. minimized). Bail; the next loop iteration
        // tries again.
        return;
    }

    // Single command buffer per frame. YUV-A → YUV-B → composite →
    // OCIO → present all encode into this one cb so Metal's
    // intra-cb dependency tracking keeps reads ordered after writes
    // without any CPU-side waitUntilCompleted. The buffer commits
    // at the end of drawFrame; vsync pacing happens at the
    // presentDrawable: backpressure (3-drawable max).
    id<MTLCommandBuffer> cb = [m_impl->commandQueue commandBuffer];

    // Phase 7.5 B.6.2: if an image-sequence cache is set, ask the
    // upload thread for the playhead's GPU texture. If ready, render
    // it via the compositor; otherwise fall back to the rainbow
    // sentinel + clear color (lets the user see the loop is alive
    // before the cache catches up).
    MetalGpuUploadThread::GpuTexture frameTex;
    if (m_cache) {
        // Big-seek / layer-change detection: cache bumps its
        // generation on user seeks beyond the small-rewind threshold
        // and when the EXR layer is swapped. Mirror that to the
        // upload thread so any pre-seek items still in its queue are
        // dropped + stale-epoch textures are pruned from the map
        // (avoids "scrubbed past a frame, scrubbed back, saw the
        // post-seek-but-stale texture" flicker). Fresh current-gen
        // uploads survive the prune — see GpuTexture::generation.
        const int curGen = m_cache->requestGeneration();
        if (curGen != m_lastCacheGen) {
            m_lastCacheGen = curGen;
            m_impl->upload.setGeneration(curGen);
            // Hold last-good across seeks (and stride misses): old
            // app's GetTexture (direct_exr_cache.cpp:457-484) only
            // resets last_good_texture_ on Shutdown — never on seek.
            // Leaving the previous frame on screen while the new
            // one loads is the whole point of the fallback. If the
            // cache's window has fully migrated, the held handle
            // gets queueDelete'd by the frame-eviction sink + the
            // pool's 3-frame graveyard; the renderer then drops
            // the stale handle in the per-frame valid check below.
        }
        const int playhead = m_cache->currentPlayhead();
        frameTex = m_impl->upload.textureForFrame(playhead);

        // Drop any leftover video RGBA from a prior video session.
        // Without this, the fallback chain below would show the
        // stale video frame whenever the cache hasn't yet produced
        // a texture for the current playhead (cold-start, big seek,
        // or just behind on the I/O queue). Render-thread-local;
        // safe to assign here.
        if (m_impl->videoFrameRgba) {
            m_impl->videoFrameRgba = nil;
            m_impl->videoFrameW    = 0;
            m_impl->videoFrameH    = 0;
            m_impl->videoHandle    = FrameHandle{};
        }
    }

    // Phase 7.5 B.6.3 + atomic-pair gate (Guide 01 §5).
    //
    // Step 1 — fetch the latest published FrameHandle from each
    // decoder into a `pending` slot. fetchLatest semantics: returns
    // the most-recent publish, dropping intermediate frames if the
    // render thread fell behind. Replacing pendingA/B with a newer
    // handle here is fine — we always render the latest available.
    if (m_decoder && !m_cache) {
        FrameHandle h;
        if (m_decoder->fetchLatest(&h) && h.isValid()) {
            m_impl->pendingHandleA = std::move(h);
        }
    }
    if (m_decoderB) {
        FrameHandle h;
        if (m_decoderB->fetchLatest(&h) && h.isValid()) {
            m_impl->pendingHandleB = std::move(h);
        }
    }

    // Step 2 — independent per-side commit. We dropped the strict
    // PTS pair-gate after observing what the old QCView app does
    // (src/player/dual_view_pipeline.cpp:144-248): each side
    // commits its latest fresh frame on its own schedule, and the
    // side without a fresh frame keeps showing its last-good
    // texture. Strict pair-gating froze the viewport during seeks
    // into B-frame media because the two decoders walk through
    // their reorder buffers at different rates — gate would never
    // see a tolerance-matching pair until both finished
    // independently. Drift between sides during steady playback
    // is at most a frame and resolves on the next iteration when
    // the slower side catches up; the visual cost is far smaller
    // than seek-induced freezes.
    const bool dualMode =
        m_decoderB && !m_decoderB->sourcePath().isEmpty();

    // Step 3 — upload helper for either side. Bridges the
    // FrameHandle through the YUV path (or CPU path) and writes
    // the resulting MTLTexture + dims into the supplied slots.
    // YUV→RGBA encodes into `cb` (the active per-frame command
    // buffer) so the compositor / OCIO / present encoders that
    // follow share the same buffer and Metal's automatic resource
    // hazard tracking keeps them ordered. No CPU-side wait, no
    // separate command-buffer commit — we collapse the cross-cb
    // sync that used to stall the render thread (twice in dual
    // mode) into the single per-frame commit at end-of-drawFrame.
    auto uploadHandle = [this, cb](const FrameHandle &h,
                                    __strong id<MTLTexture> &outRgba,
                                    int &outW, int &outH,
                                    __strong id<MTLTexture> &cpuCache,
                                    int &cpuCacheW, int &cpuCacheH,
                                    int rangeOverride) {
        if (h.kind() == FrameHandle::Kind::Metal) {
            CvPixbufMetalBridge::PlaneSet planes;
            const auto rc = m_impl->pixbufBridge.bridge(h, &planes);
            if (rc != CvPixbufMetalBridge::Result::Ok || !planes.valid()) {
                return;
            }
            // Phase 3.G — apply per-clip range override. Bridge derives
            // `fullRange` from the CVPixelFormatType (`*VideoRange` vs
            // `*FullRange`); when the user has overridden the value
            // (1 = Full, 2 = Limited) we stomp that flag before the
            // YUV→RGB kernel runs.
            bool fullRange = planes.fullRange;
            if (rangeOverride == 1) fullRange = true;
            else if (rangeOverride == 2) fullRange = false;

            void *output = nullptr;
            if (planes.isPacked()) {
                output = m_impl->yuv.renderInterleavedToRgba(
                    (__bridge void *)cb,
                    planes.packedTexture,
                    planes.width, planes.height,
                    planes.bitDepth, fullRange,
                    planes.isBt2020, planes.isHdr);
            } else {
                output = m_impl->yuv.renderToRgba(
                    (__bridge void *)cb,
                    planes.yTexture, planes.uvTexture,
                    planes.width, planes.height,
                    planes.bitDepth, fullRange,
                    planes.isBt2020, planes.isHdr,
                    planes.subsampling);
            }
            if (output) {
                outRgba = (__bridge_transfer id<MTLTexture>)output;
                outW    = planes.width;
                outH    = planes.height;
            }
        } else if (h.kind() == FrameHandle::Kind::Cpu) {
            const QImage &img = h.cpuImage();
            id<MTLTexture> tex = uploadCpuFrameRgba(
                m_impl->device, img,
                cpuCache, cpuCacheW, cpuCacheH);
            if (tex) {
                outRgba = tex;
                outW    = img.width();
                outH    = img.height();
            }
        }
    };

    // Side A: commit whenever a fresh frame is pending. Move the
    // handle into the long-lived `videoHandle` slot so its
    // CVPixelBuffer stays alive across the present. When no fresh
    // frame is pending, videoFrameRgba retains its last-good
    // value and the compositor samples the previous frame —
    // no flicker, no freeze.
    if (m_impl->pendingHandleA.isValid()) {
        const int rangeOvA = m_decoder ? m_decoder->rangeOverride() : 0;
        uploadHandle(m_impl->pendingHandleA,
                     m_impl->videoFrameRgba,
                     m_impl->videoFrameW, m_impl->videoFrameH,
                     m_impl->cpuFrameTexA,
                     m_impl->cpuFrameAW, m_impl->cpuFrameAH,
                     rangeOvA);
        m_impl->videoHandle = std::move(m_impl->pendingHandleA);
        m_impl->pendingHandleA = FrameHandle{};
    }

    // Side B: same independent commit, regardless of whether A is
    // a video decoder (dual-video) or an image-seq cache (hybrid).
    // Both used to be special-cased — the pair-gate path for the
    // former, an independent block for the latter — but the old
    // QCView precedent says they're the same problem.
    if (dualMode && m_impl->pendingHandleB.isValid()) {
        const int rangeOvB = m_decoderB ? m_decoderB->rangeOverride() : 0;
        uploadHandle(m_impl->pendingHandleB,
                     m_impl->videoFrameRgbaB,
                     m_impl->videoFrameWB, m_impl->videoFrameHB,
                     m_impl->cpuFrameTexB,
                     m_impl->cpuFrameBW, m_impl->cpuFrameBH,
                     rangeOvB);
        m_impl->videoHandleB = std::move(m_impl->pendingHandleB);
        m_impl->pendingHandleB = FrameHandle{};
    }

    // Resolve source texture pointer + dimensions from whichever path
    // produced one. Image-seq path takes precedence when both are
    // wired (the cache pointer is set only when an image-sequence is
    // active; video path runs only when m_cache is null).
    void *sourceTexture = nullptr;
    int   sourceW = 0;
    int   sourceH = 0;
    if (frameTex.valid && frameTex.handle != 0) {
        const auto *tex = MetalTexturePool::instance().texture(frameTex.handle);
        if (tex && tex->valid && tex->texture) {
            sourceTexture = tex->texture;
            sourceW = frameTex.width;
            sourceH = frameTex.height;
            MetalTexturePool::instance().touch(frameTex.handle);
            // ARC retain: assigning to id<MTLTexture> increments the
            // refcount; the previous lastGood is released here, the
            // new one survives any subsequent pool eviction.
            m_impl->lastGoodImageSeqTexture =
                (__bridge id<MTLTexture>)tex->texture;
            m_impl->lastGoodImageSeqW = frameTex.width;
            m_impl->lastGoodImageSeqH = frameTex.height;
        }
    } else if (m_cache && m_impl->lastGoodImageSeqTexture) {
        // Image-seq fallback — hold the previously-shown texture
        // (we own a strong ref, so pool eviction can't pull it out
        // from under us). Old app's last_good_texture_ pattern,
        // direct_exr_cache.cpp:457-484.
        sourceTexture = (__bridge void *)m_impl->lastGoodImageSeqTexture;
        sourceW = m_impl->lastGoodImageSeqW;
        sourceH = m_impl->lastGoodImageSeqH;
    } else if (m_impl->videoFrameRgba) {
        sourceTexture = (__bridge void *)m_impl->videoFrameRgba;
        sourceW = m_impl->videoFrameW;
        sourceH = m_impl->videoFrameH;
    }

    const bool haveSourceTexture = sourceTexture != nullptr;

    // Pixel-aspect un-squeeze for source A. The compositor + every
    // overlay rect derive the source aspect from these effective dims;
    // sampleFit normalizes its sample by srcSize so feeding widened
    // dims stretches the real texture (no shader change). 1/1 (square,
    // the default) leaves dims untouched — identical prior behavior.
    // Widen the larger-pixel axis up to preserve resolution.
    auto effDim = [](int w, int h, int num, int den) -> std::pair<int,int> {
        if (w <= 0 || h <= 0 || num <= 0 || den <= 0 || num == den)
            return { w, h };
        if (num > den)
            return { static_cast<int>(std::lround(double(w) * num / den)), h };
        return { w, static_cast<int>(std::lround(double(h) * den / num)) };
    };
    const auto [effSourceW, effSourceH] = effDim(
        sourceW, sourceH,
        m_parNumA.load(std::memory_order_relaxed),
        m_parDenA.load(std::memory_order_relaxed));

    // (cb was created right after nextDrawable; YUV uploads above
    //  already encoded their compute passes into it.)

    const int dstW = static_cast<int>(m_impl->layer.drawableSize.width);
    const int dstH = static_cast<int>(m_impl->layer.drawableSize.height);

    // Phase A2 — composite intermediate sized as min(canvas, cap).
    // Recreate only when the required size actually changes.
    const int desiredCompositeW =
        std::max(1, std::min(dstW, Impl::kMaxCompositeW));
    const int desiredCompositeH =
        std::max(1, std::min(dstH, Impl::kMaxCompositeH));
    if (m_impl->compositeRaw == nil
        || m_impl->compositeW != desiredCompositeW
        || m_impl->compositeH != desiredCompositeH) {
        MTLTextureDescriptor *desc =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:
                MTLPixelFormatRGBA16Float
                width:desiredCompositeW
                height:desiredCompositeH
                mipmapped:NO];
        desc.storageMode = MTLStorageModePrivate;
        desc.usage = MTLTextureUsageShaderRead
                   | MTLTextureUsageShaderWrite
                   | MTLTextureUsageRenderTarget;
        m_impl->compositeRaw = [m_impl->device newTextureWithDescriptor:desc];
        m_impl->compositeW   = desiredCompositeW;
        m_impl->compositeH   = desiredCompositeH;
    }

    // ---- Render Pass 1: canvas compositor → compositeRaw ----
    // Cleared to alpha=0; the compositor MSL discards letterbox
    // fragments so they stay alpha=0 and the present blit can
    // discard them in turn (background shows through).
    if (haveSourceTexture && m_impl->compositor.isInitialized()
        && m_impl->compositeRaw) {
        // Source B feed — when m_decoderB is loaded, route its
        // freshly-decoded RGBA into the compositor's srcB slot.
        // Otherwise mirror A so SBS / Wipe still work as a no-op
        // compare. The compositor's renderSources also auto-mirrors
        // when one side is null, so we could pass nullptr; this
        // explicit fall-back keeps the dims sensible.
        void *srcBTex = sourceTexture;
        int   srcBW   = sourceW;
        int   srcBH   = sourceH;
        int   parNumB = m_parNumA.load(std::memory_order_relaxed);
        int   parDenB = m_parDenA.load(std::memory_order_relaxed);
        if (m_decoderB && m_impl->videoFrameRgbaB) {
            srcBTex = (__bridge void *)m_impl->videoFrameRgbaB;
            srcBW   = m_impl->videoFrameWB;
            srcBH   = m_impl->videoFrameHB;
            parNumB = m_parNumB.load(std::memory_order_relaxed);
            parDenB = m_parDenB.load(std::memory_order_relaxed);
        }
        const auto [effBW, effBH] = effDim(srcBW, srcBH, parNumB, parDenB);

        MTLRenderPassDescriptor *rawRpd =
            [MTLRenderPassDescriptor renderPassDescriptor];
        rawRpd.colorAttachments[0].texture     = m_impl->compositeRaw;
        rawRpd.colorAttachments[0].loadAction  = MTLLoadActionClear;
        rawRpd.colorAttachments[0].clearColor  =
            MTLClearColorMake(0.0, 0.0, 0.0, 0.0);
        rawRpd.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> rawEnc =
            [cb renderCommandEncoderWithDescriptor:rawRpd];
        // Per-side activity. WindowManager pushes false when the
        // master playhead has passed that side's natural duration
        // (mismatched-clip-length case in dual mode); the
        // compositor renders that region transparent rather than
        // mirroring the other side's frame.
        const bool aAct = m_aActive.load(std::memory_order_relaxed);
        const bool bAct = m_bActive.load(std::memory_order_relaxed);
        m_impl->compositor.renderSources(
            (__bridge void *)rawEnc,
            sourceTexture, effSourceW, effSourceH,
            srcBTex,        effBW,      effBH,
            m_impl->compositeW, m_impl->compositeH,
            static_cast<int>(m_compMode), m_splitPos,
            aAct, bAct);
        // Phase 3.H.5 — hover-thumbnail corner overlays. Drawn into
        // compositeRaw BEFORE OCIO so they share the main image's
        // color treatment. A in BL, B in BR (B only populated in
        // dual mode by WindowManager). 0 = no overlay. Caller must
        // not draw anything after these calls in the same pass —
        // viewport is left at the last corner ROI; we end the
        // encoder right after.
        const unsigned long long thumbHandleA =
            m_hoverThumbHandle.load(std::memory_order_acquire);
        if (thumbHandleA != 0) {
            const auto *meta = MetalTexturePool::thumbnailInstance()
                                   .texture(thumbHandleA);
            if (meta && meta->valid && meta->texture) {
                m_impl->compositor.renderCornerOverlay(
                    (__bridge void *)rawEnc,
                    meta->texture,
                    parThumbW(meta->width,
                              m_parNumA.load(std::memory_order_relaxed),
                              m_parDenA.load(std::memory_order_relaxed)),
                    meta->height,
                    m_impl->compositeW, m_impl->compositeH,
                    /*corner=*/0,
                    /*overlayFrac=*/0.18f,
                    /*marginPx=*/12.0f);
            }
        }
        const unsigned long long thumbHandleB =
            m_hoverThumbHandleB.load(std::memory_order_acquire);
        if (thumbHandleB != 0) {
            const auto *meta = MetalTexturePool::thumbnailInstance()
                                   .texture(thumbHandleB);
            if (meta && meta->valid && meta->texture) {
                m_impl->compositor.renderCornerOverlay(
                    (__bridge void *)rawEnc,
                    meta->texture,
                    parThumbW(meta->width,
                              m_parNumB.load(std::memory_order_relaxed),
                              m_parDenB.load(std::memory_order_relaxed)),
                    meta->height,
                    m_impl->compositeW, m_impl->compositeH,
                    /*corner=*/1,
                    /*overlayFrac=*/0.18f,
                    /*marginPx=*/12.0f);
            }
        }
        [rawEnc endEncoding];
    }

    // ---- OCIO compute pass: compositeRaw → compositeCorrected ----
    // Per Guide 01 §7 OCIO is post-composite — one chain over the
    // composited canvas, not per-side. When OCIO is disengaged,
    // compositeCorrected just aliases compositeRaw (no copy).
    void *compositeCorrected = (__bridge void *)m_impl->compositeRaw;
    if (haveSourceTexture && m_ocio && m_ocio->engaged()
        && m_impl->compositeRaw) {
        m_impl->ocio.rebuild(m_ocio);
        if (m_impl->ocio.hasPipeline()) {
            void *ocioOut = m_impl->ocio.apply(
                (__bridge void *)cb,
                (__bridge void *)m_impl->compositeRaw,
                m_impl->compositeW, m_impl->compositeH);
            if (ocioOut) compositeCorrected = ocioOut;
        }
    }

    // Stash for screenshot capture. lastDisplayTexture is the
    // window-sized post-OCIO canvas (used as a fallback). The
    // capture path actually prefers lastSourceTexture so it can
    // render the source at NATIVE resolution and re-apply OCIO,
    // avoiding the canvas's baked-in letterbox bars.
    if (haveSourceTexture) {
        m_impl->lastDrawnSourceW   = m_impl->compositeW;
        m_impl->lastDrawnSourceH   = m_impl->compositeH;
        m_impl->lastDisplayTexture = compositeCorrected;
        m_impl->lastSourceTexture  = sourceTexture;
        m_impl->lastSourceW        = sourceW;
        m_impl->lastSourceH        = sourceH;
    }

    // ---- Render Pass 2: drawable ----
    // Background first (covers the whole drawable, including the
    // letterbox region the present blit will leave alpha=0). Then
    // present blit composites the canvas on top via the same
    // discard-on-alpha-0 mechanic that the canvas compositor used.
    MTLRenderPassDescriptor *rpd = [MTLRenderPassDescriptor renderPassDescriptor];
    rpd.colorAttachments[0].texture     = drawable.texture;
    rpd.colorAttachments[0].loadAction  = MTLLoadActionClear;
    rpd.colorAttachments[0].storeAction = MTLStoreActionStore;
    rpd.colorAttachments[0].clearColor  = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);

    id<MTLRenderCommandEncoder> enc =
        [cb renderCommandEncoderWithDescriptor:rpd];

    if (m_impl->presentCompositor.isInitialized()) {
        const float tilePixels =
            20.0f * static_cast<float>(m_impl->layer.contentsScale > 0
                                       ? m_impl->layer.contentsScale : 1.0);
        m_impl->presentCompositor.renderBackground(
            (__bridge void *)enc, static_cast<int>(m_bgMode),
            dstW, dstH, tilePixels);

        if (haveSourceTexture && compositeCorrected) {
            // 1:1 single-mode aspect-fit of compositeCorrected into
            // the drawable. Lower-than-canvas drawables downsample;
            // higher-than-canvas ones upsample.
            m_impl->presentCompositor.renderSingle(
                (__bridge void *)enc, compositeCorrected,
                m_impl->compositeW, m_impl->compositeH, dstW, dstH);
        }
    }

    // Image-content rect within the drawable — the single source of
    // truth for every overlay (annotator strokes + safety guides) so
    // they all frame the actual picture, not the letterboxed window.
    //
    // Uses the PIXEL-ASPECT-CORRECTED effective dims (effSourceW/H),
    // NOT compositeW/H — the composite intermediate is sized to the
    // drawable aspect with the source letterboxed inside, so its
    // aspect doesn't tell us where the image sits; and NOT the raw
    // sourceW/H, so anamorphic clips' overlays track the un-squeezed
    // image.
    const bool haveImgRect = (effSourceW > 0 && effSourceH > 0 && dstH > 0);
    float imgFitW = float(dstW), imgFitH = float(dstH);
    float imgFitX = 0.0f, imgFitY = 0.0f;
    if (haveImgRect) {
        const float srcAspect = float(effSourceW) / float(effSourceH);
        const float dstAspect = float(dstW) / float(dstH);
        if (srcAspect > dstAspect) {
            imgFitW = float(dstW);
            imgFitH = float(dstW) / srcAspect;
            imgFitX = 0.0f;
            imgFitY = (float(dstH) - imgFitH) * 0.5f;
        } else {
            imgFitH = float(dstH);
            imgFitW = float(dstH) * srcAspect;
            imgFitY = 0.0f;
            imgFitX = (float(dstW) - imgFitW) * 0.5f;
        }
    }

    // Phase 7.5 B.6.5: stroke pass — runs after the compositor in
    // the same render encoder. Pulls the active stroke from the
    // annotator (if set + drawing); tessellates against the image-
    // content rect so strokes align to the actual (un-squeezed) image.
    if (m_annotator && m_impl->annotations.isInitialized() && haveImgRect) {
        const float fitW = imgFitW, fitH = imgFitH;
        const float fitX = imgFitX, fitY = imgFitY;
        m_annotator->setViewportRect(QPointF(fitX, fitY),
                                       QSizeF(fitW, fitH));
        // Stroke widths are stored in SOURCE-pixel units (so
        // what you see on screen matches what gets saved into
        // the source-resolution capture / export). Scale by the
        // current fit-to-source ratio so a 6-source-pixel stroke
        // grows / shrinks proportionally as the window resizes.
        // effSourceW (not raw) keeps the ratio stable under un-squeeze.
        const float lineScale = float(fitW) / float(effSourceW);
        if (const ActiveStroke *stroke = m_annotator->activeStroke()) {
            const TessellatedMesh mesh = StrokeTessellator::tessellate(
                *stroke, fitX, fitY, fitW, fitH, lineScale);
            if (!mesh.vertices.empty()) {
                m_impl->annotations.drawMesh(
                    (__bridge void *)enc, mesh, dstW, dstH);
            }
        }

        // Phase 3.H.6 — stored strokes for the current frame.
        // WindowManager rebuilds this list on note change + frame
        // change; we tessellate each per draw so coords stay
        // correct under viewport resize.
        std::vector<ActiveStroke> storedSnapshot;
        {
            std::lock_guard<std::mutex> lock(m_storedStrokesMutex);
            storedSnapshot = m_storedStrokes;
        }
        for (const ActiveStroke &s : storedSnapshot) {
            const TessellatedMesh mesh = StrokeTessellator::tessellate(
                s, fitX, fitY, fitW, fitH, lineScale);
            if (!mesh.vertices.empty()) {
                m_impl->annotations.drawMesh(
                    (__bridge void *)enc, mesh, dstW, dstH);
            }
        }
    }

    // Phase 7.5 B.6.6: safety overlay — same pipeline as stroke pass
    // (alpha-blended triangles). The SVG aspect-fits into the IMAGE-
    // content rect (haveImgRect) so the guides frame the actual
    // picture, matching annotations — including under anamorphic
    // un-squeeze. Falls back to the full drawable only when no source
    // is loaded (so a guide preview still shows on an empty viewport).
    if (m_safety && m_safety->isLoaded() && m_impl->annotations.isInitialized()) {
        // Map the guide through the displayed image rect using the RAW
        // (square) source dims, so it stretches with the picture under
        // pixel-aspect un-squeeze. Falls back to a uniform drawable fit
        // when no source is loaded (guide preview on an empty viewport).
        const TessellatedMesh safetyMesh = haveImgRect
            ? m_safety->meshForImage(QPointF(imgFitX, imgFitY),
                                     QSizeF(imgFitW, imgFitH),
                                     double(sourceW), double(sourceH))
            : m_safety->mesh(QPointF(0, 0), QSizeF(dstW, dstH));
        if (!safetyMesh.vertices.empty()) {
            m_impl->annotations.drawMesh(
                (__bridge void *)enc, safetyMesh, dstW, dstH);
        }
    }

    // Loading spinner overlay — drawn last (over content) on the same
    // encoder while a media/project open blocks the main thread. The
    // free-running render thread animates it; a brief delay avoids
    // flashing on quick opens. Reuses the dual compositor's spinner.
    if (m_loadingActive.load(std::memory_order_acquire)) {
        const auto now = std::chrono::steady_clock::now();
        if (!m_impl->loadingSpinnerWasActive) {
            m_impl->loadingSpinnerWasActive = true;
            m_impl->loadingSpinnerStart     = now;
        }
        const double elapsed =
            std::chrono::duration<double>(now - m_impl->loadingSpinnerStart).count();
        if (elapsed >= 0.18) {
            m_impl->dualCompositor.encodeLoadingSpinner(
                (__bridge void *)enc, dstW, dstH,
                static_cast<int>(m_impl->layer.pixelFormat));
        }
    } else {
        m_impl->loadingSpinnerWasActive = false;
    }

    // (The unsupported-media notice is now an in-scene QML card over the
    // hidden viewport — see WindowManager's viewport-cover primitive — so
    // there's no composited notice pass here anymore.)

    [enc endEncoding];

    [cb presentDrawable:drawable];
    [cb commit];

    // Screenshot readback — service after present so the captured
    // frame matches what the user is seeing. Captures the source
    // at its NATIVE pixel resolution (not the window-sized canvas)
    // and applies OCIO at source resolution, so saved PNGs:
    //   - have no letterbox / pillarbox bars
    //   - match source dimensions exactly
    //   - carry the full OCIO color treatment the viewer is using
    //
    // Steps:
    //   1. Render lastSourceTexture 1:1 into a source-sized
    //      RGBA16F intermediate via the main compositor (no
    //      aspect-fit because src dims == dst dims).
    //   2. Run OCIO over that intermediate (if engaged).
    //   3. Render the corrected RGBA16F into a source-sized
    //      RGBA8Unorm shared texture via captureCompositor.
    //   4. Read back to QImage.
    bool wantCapture = false;
    {
        std::lock_guard<std::mutex> lk(m_impl->captureMutex);
        wantCapture = m_impl->capturePending;
    }
    if (wantCapture && m_impl->lastSourceTexture
        && m_impl->lastSourceW > 0
        && m_impl->lastSourceH > 0) {
        @autoreleasepool {
            const int srcW = m_impl->lastSourceW;
            const int srcH = m_impl->lastSourceH;

            // Lazy-init the capture compositor at RGBA8Unorm.
            constexpr int kCaptureFmt =
                static_cast<int>(MTLPixelFormatRGBA8Unorm);
            if (!m_impl->captureCompositor.isInitialized()
                || m_impl->captureCompositor.targetPixelFormat() != kCaptureFmt) {
                m_impl->captureCompositor.shutdown();
                m_impl->captureCompositor.initialize(kCaptureFmt);
            }

            // Lazy-allocate / resize the source-sized RGBA16F
            // intermediate. Reused across captures.
            if (!m_impl->captureSourceRgba16f
                || m_impl->captureSourceRgba16fW != srcW
                || m_impl->captureSourceRgba16fH != srcH) {
                MTLTextureDescriptor *intDesc =
                    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:
                        MTLPixelFormatRGBA16Float
                        width:srcW height:srcH mipmapped:NO];
                intDesc.storageMode = MTLStorageModePrivate;
                intDesc.usage = MTLTextureUsageShaderRead
                              | MTLTextureUsageShaderWrite
                              | MTLTextureUsageRenderTarget;
                m_impl->captureSourceRgba16f =
                    [m_impl->device newTextureWithDescriptor:intDesc];
                m_impl->captureSourceRgba16fW = srcW;
                m_impl->captureSourceRgba16fH = srcH;
            }

            // Source-sized RGBA8 destination (CPU-readable).
            MTLTextureDescriptor *dstDesc =
                [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:
                    MTLPixelFormatRGBA8Unorm
                    width:srcW height:srcH mipmapped:NO];
            dstDesc.storageMode = MTLStorageModeShared;
            dstDesc.usage = MTLTextureUsageShaderRead
                          | MTLTextureUsageRenderTarget;
            id<MTLTexture> dstTex =
                [m_impl->device newTextureWithDescriptor:dstDesc];

            id<MTLCommandBuffer> capCb =
                [m_impl->commandQueue commandBuffer];

            // ---- Pass 1: source 1:1 → source-sized RGBA16F ----
            {
                MTLRenderPassDescriptor *rpd1 =
                    [MTLRenderPassDescriptor renderPassDescriptor];
                rpd1.colorAttachments[0].texture     =
                    m_impl->captureSourceRgba16f;
                rpd1.colorAttachments[0].loadAction  = MTLLoadActionClear;
                rpd1.colorAttachments[0].clearColor  =
                    MTLClearColorMake(0.0, 0.0, 0.0, 0.0);
                rpd1.colorAttachments[0].storeAction = MTLStoreActionStore;
                id<MTLRenderCommandEncoder> p1Enc =
                    [capCb renderCommandEncoderWithDescriptor:rpd1];
                if (m_impl->compositor.isInitialized()) {
                    // src dims == dst dims = source dims → aspect-fit
                    // collapses to full fill, preserving every pixel.
                    m_impl->compositor.renderSource(
                        (__bridge void *)p1Enc,
                        m_impl->lastSourceTexture,
                        srcW, srcH, srcW, srcH,
                        MetalCompositor::Single, 0.5f);
                }
                [p1Enc endEncoding];
            }

            // ---- Pass 2: OCIO compute (in-place if engaged) ----
            void *correctedTex =
                (__bridge void *)m_impl->captureSourceRgba16f;
            if (m_ocio && m_ocio->engaged()) {
                m_impl->ocio.rebuild(m_ocio);
                if (m_impl->ocio.hasPipeline()) {
                    void *ocioOut = m_impl->ocio.apply(
                        (__bridge void *)capCb,
                        (__bridge void *)m_impl->captureSourceRgba16f,
                        srcW, srcH);
                    if (ocioOut) correctedTex = ocioOut;
                }
            }

            // ---- Pass 3: corrected → RGBA8 dst ----
            {
                MTLRenderPassDescriptor *rpd3 =
                    [MTLRenderPassDescriptor renderPassDescriptor];
                rpd3.colorAttachments[0].texture     = dstTex;
                rpd3.colorAttachments[0].loadAction  = MTLLoadActionClear;
                rpd3.colorAttachments[0].clearColor  =
                    MTLClearColorMake(0.0, 0.0, 0.0, 1.0);
                rpd3.colorAttachments[0].storeAction = MTLStoreActionStore;
                id<MTLRenderCommandEncoder> p3Enc =
                    [capCb renderCommandEncoderWithDescriptor:rpd3];
                m_impl->captureCompositor.renderSource(
                    (__bridge void *)p3Enc,
                    correctedTex,
                    srcW, srcH, srcW, srcH,
                    MetalCompositor::Single, 0.5f);

                // Phase 3.H.6 Stage D — bake annotation strokes
                // into the screenshot. Without this, screenshots
                // (and the saveNoteAnnotatedThumbnail output that
                // drives the notes-panel cards + exports) come
                // out as bare frames because the strokes are only
                // drawn on the swapchain drawable, not into the
                // capture target.
                if (!m_impl->captureAnnotations.isInitialized()
                    || m_impl->captureAnnotations.targetPixelFormat()
                        != static_cast<int>(MTLPixelFormatRGBA8Unorm)) {
                    m_impl->captureAnnotations.shutdown();
                    m_impl->captureAnnotations.initialize(
                        static_cast<int>(MTLPixelFormatRGBA8Unorm));
                }
                if (m_impl->captureAnnotations.isInitialized()) {
                    m_impl->captureAnnotations.beginFrame();
                    // In-flight stroke (if any). NOTE: do NOT call
                    // m_annotator->setViewportRect() here — the live
                    // present pass owns that state for input
                    // mapping. Tessellate against the source-sized
                    // capture target directly.
                    if (m_annotator) {
                        if (const ActiveStroke *s =
                                m_annotator->activeStroke()) {
                            const TessellatedMesh m = StrokeTessellator::tessellate(
                                *s, 0.0f, 0.0f,
                                static_cast<float>(srcW),
                                static_cast<float>(srcH));
                            if (!m.vertices.empty()) {
                                m_impl->captureAnnotations.drawMesh(
                                    (__bridge void *)p3Enc, m, srcW, srcH);
                            }
                        }
                    }
                    // Stored strokes for the current frame.
                    std::vector<ActiveStroke> stored;
                    {
                        std::lock_guard<std::mutex> lock(m_storedStrokesMutex);
                        stored = m_storedStrokes;
                    }
                    for (const ActiveStroke &s : stored) {
                        const TessellatedMesh m = StrokeTessellator::tessellate(
                            s, 0.0f, 0.0f,
                            static_cast<float>(srcW),
                            static_cast<float>(srcH));
                        if (!m.vertices.empty()) {
                            m_impl->captureAnnotations.drawMesh(
                                (__bridge void *)p3Enc, m, srcW, srcH);
                        }
                    }
                }
                [p3Enc endEncoding];
            }

            [capCb commit];
            [capCb waitUntilCompleted];

            QImage img(srcW, srcH, QImage::Format_RGBA8888);
            [dstTex getBytes:img.bits()
                bytesPerRow:img.bytesPerLine()
                 fromRegion:MTLRegionMake2D(0, 0, srcW, srcH)
                mipmapLevel:0];
            img.setColorSpace(QColorSpace(QColorSpace::SRgb));

            std::lock_guard<std::mutex> lk(m_impl->captureMutex);
            m_impl->captureImage   = img.copy();
            m_impl->capturePending = false;
        }
        m_impl->captureCv.notify_all();
    } else if (wantCapture) {
        // No source available (cache warming / no media). Hand
        // back an empty image so the caller can fail fast.
        std::lock_guard<std::mutex> lk(m_impl->captureMutex);
        m_impl->captureImage   = QImage();
        m_impl->capturePending = false;
        m_impl->captureCv.notify_all();
    }

    ++m_impl->frameCount;
}

QImage MetalPlayerRenderer::captureScreenshot()
{
    if (!m_running.load()) return QImage();

    std::unique_lock<std::mutex> lk(m_impl->captureMutex);
    m_impl->capturePending = true;
    m_impl->captureImage   = QImage();
    // Wait up to 250 ms for the render thread to service the
    // request. If the loop is wedged or shutting down, fail
    // quietly and return an empty image; callers handle that.
    using namespace std::chrono_literals;
    const bool ok = m_impl->captureCv.wait_for(lk, 250ms, [this] {
        return !m_impl->capturePending || !m_running.load();
    });
    if (!ok || !m_running.load()) {
        m_impl->capturePending = false;
        return QImage();
    }
    QImage out = m_impl->captureImage;
    m_impl->captureImage = QImage();
    return out;
}

} // namespace qcv
