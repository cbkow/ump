#include "d3d11_player_renderer.h"
#include "d3d11_annotation_renderer.h"
#include "d3d11_compositor.h"
#include "d3d11_device_manager.h"
#include "d3d11_drop_target.h"
#include "d3d11_dual_compositor.h"
#include "d3d11_gpu_upload_thread.h"
#include "d3d11_hdr_swapchain.h"
#include "d3d11_ocio_renderer.h"
#include "d3d11_texture_pool.h"
#include "d3d11_vulkan_decode_bridge.h"
#include "d3d11_vulkan_yuv_compositor.h"
#include "render/i_dual_frame_source.h"

#include "annotations/stroke_tessellator.h"
#include "annotations/viewport_annotator.h"
#include "color/ocio_config_manager.h"
#include "decode/image_loader.h"          // PixelData
#include "decode/image_sequence_cache.h"
#include "render/safety_overlay.h"

#include "annotations/active_stroke.h"   // full ActiveStroke definition for std::vector
#include "decode/frame_handle.h"
#include "decode/video_decoder.h"
#include "render/player_window.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dxgi1_2.h>
#include <ole2.h>
#include <oleidl.h>
#include <wrl/client.h>

#include <QColorSpace>
#include <QImage>
#include <QWindow>
#include <QtLogging>

#include <chrono>

namespace qcv {

using Microsoft::WRL::ComPtr;

namespace {

// Pre-content clear color — matches Theme.bg (#161616 = 22/255) so
// the brief flash before the first frame renders blends with the UI
// chrome instead of being a visible blue rectangle.
constexpr float kClearR = 0.0863f;   // 22/255
constexpr float kClearG = 0.0863f;
constexpr float kClearB = 0.0863f;
constexpr float kClearA = 1.00f;

constexpr DXGI_FORMAT kSwapchainFormat = DXGI_FORMAT_B8G8R8A8_UNORM;

// Phase F.2.11.d — DualFlow canvas cap. Mirrors
// MetalPlayerRenderer::Impl::kMaxCompositeW/H (3840×2160). A 5K HiDPI
// drawable would otherwise feed OCIO 14.7M pixels per frame — slow
// even on M-series. The present-blit pass upscales canvas → swapchain
// so perceptual fidelity at viewing distances is unchanged.
constexpr int kMaxDualCanvasW = 3840;
constexpr int kMaxDualCanvasH = 2160;

// Window class for the renderer's child HWND. Registered lazily on
// first init(); HTTRANSPARENT in WM_NCHITTEST so mouse events fall
// through to Qt's parent HWND (Qt's QML scene handles all input).
constexpr const wchar_t *kChildWindowClassName = L"QcvD3D11PlayerChild";

LRESULT CALLBACK childWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
        case WM_NCHITTEST:
            return HTTRANSPARENT;
        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

bool registerChildWindowClass()
{
    static bool registered = false;
    if (registered) return true;
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = childWndProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = kChildWindowClassName;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    // No background brush — DComp owns composition; we don't want GDI
    // erasing the surface between presents.
    if (!RegisterClassExW(&wc)) {
        const DWORD gle = GetLastError();
        if (gle != ERROR_CLASS_ALREADY_EXISTS) {
            qCritical("D3D11PlayerRenderer: RegisterClassExW failed (GLE=%lu)", gle);
            return false;
        }
    }
    registered = true;
    return true;
}

} // namespace

struct D3D11PlayerRenderer::Impl {
    HWND parentHwnd = nullptr;
    HWND childHwnd  = nullptr;

    // Phase F.2.11.x — drag-drop forwarding for the child HWND. OLE
    // drag-drop is HWND-scoped (no HTTRANSPARENT fall-through), so
    // every file drop over the player area dies unless the child HWND
    // has an IDropTarget. We register one here and forward URLs into
    // PlayerWindow::notifyFilesDropped (same path macOS already takes
    // via QWindow::event override).
    PlayerWindow                    *playerWindow = nullptr;  // not owned
    ComPtr<IDropTarget>              dropTarget;
    bool                             oleInitialized = false;

    // Owned DComp device; swapchain + RTV + DComp target/visual live
    // inside hdrSwapchain (so HDR mode flips can recreate the
    // swapchain + re-anchor the visual without touching the parent
    // renderer's state).
    ComPtr<IDCompositionDevice>      dcompDevice;
    D3D11HdrSwapchain                hdrSwapchain;

    // Mirror of hdrSwapchain.currentWidth/Height. Kept here so the
    // resize-pending path can compare against pending dims without a
    // virtual dispatch every frame. Updated whenever hdrSwapchain
    // accepts a resize().
    int currentW = 0;
    int currentH = 0;

    D3D11Compositor                  compositor;

    // Phase F.2.7 — annotation overlay (active stroke + stored
    // strokes + safety overlay all use this single pass; runs after
    // OCIO/compositor finishes writing the swapchain RTV).
    D3D11AnnotationRenderer          annotations;

    // Phase F.2.5 — OCIO color pipeline. When OCIO is engaged the
    // compositor renders into `composite` (RGBA16F at swapchain size)
    // and ocio.apply() transforms that into the swapchain back buffer.
    // When OCIO is disengaged we bypass the intermediate and the
    // compositor draws directly to the swapchain RTV.
    D3D11OcioRenderer                ocio;
    ComPtr<ID3D11Texture2D>          compositeTex;     // RGBA16F at swapchain size
    ComPtr<ID3D11RenderTargetView>   compositeRtv;
    ComPtr<ID3D11ShaderResourceView> compositeSrv;
    int                              compositeW = 0;
    int                              compositeH = 0;

    // Phase F.2.13 — shared Vulkan YUV→RGB compute pipeline. Owned
    // here, injected into every bridge (single-flow's vulkanBridge AND
    // both dual-flow per-slot bridges inside dualCompositor). Declared
    // BEFORE every bridge so it outlives them all on destruction.
    D3D11VulkanYuvCompositor         yuvCompositor;

    // Phase F.2.4.3 — bridge that imports Vulkan-decoded VkImages
    // (from FrameHandle::Vulkan) into D3D11 textures via NT shared
    // handles. After F.2.13 the bridge is a thin per-stream cache;
    // the heavy compute pipeline lives in yuvCompositor above.
    D3D11VulkanDecodeBridge          vulkanBridge;

    // Phase F.2.11.d — DualFlow pipeline. Compositor pulls per-side
    // frames through the IDualFrameSource adapter (qcv_window). Canvas
    // texture (RGBA16F, capped at kMaxDualCanvas{W,H}) sits between
    // the dual composite and the present-blit, mirroring Metal's
    // compositeRawDual. Lazy-initialized on first DualFlow entry.
    D3D11DualCompositor              dualCompositor;
    bool                             dualCompositorInited = false;
    ComPtr<ID3D11Texture2D>          dualCanvas;        // RGBA16F at canvasW×H
    ComPtr<ID3D11RenderTargetView>   dualCanvasRtv;
    ComPtr<ID3D11ShaderResourceView> dualCanvasSrv;
    int                              dualCanvasW = 0;
    int                              dualCanvasH = 0;
    // OCIO output intermediate at canvas size. Allocated only when
    // OCIO is engaged; when disengaged the present-blit reads from
    // dualCanvasSrv directly.
    ComPtr<ID3D11Texture2D>          dualOcioOut;
    ComPtr<ID3D11RenderTargetView>   dualOcioOutRtv;
    ComPtr<ID3D11ShaderResourceView> dualOcioOutSrv;
    int                              dualOcioOutW = 0;
    int                              dualOcioOutH = 0;

    // Source-A video slot — one persistent ID3D11Texture2D updated
    // per FrameHandle::Cpu received from VideoDecoder. UpdateSubresource
    // in-place when dims unchanged; tear down + recreate when the
    // source resolution changes (rare during playback).
    //
    // Owner enum gates the texture lifetime model:
    //   - Cpu:      slot allocates + owns texture/srv directly
    //   - Bridge:   Vulkan-decode bridge owns; slot AddRefs raw pointers
    //   - ImageSeq: D3D11GpuUploadThread / D3D11TexturePool owns; slot
    //               AddRefs SRV via shared handle (texture left null)
    enum class SlotOwner { None, Cpu, Bridge, ImageSeq };
    struct VideoSlot {
        ComPtr<ID3D11Texture2D>          texture;
        ComPtr<ID3D11ShaderResourceView> srv;
        int       width  = 0;
        int       height = 0;
        SlotOwner owner  = SlotOwner::None;
    };
    VideoSlot videoA;

    // Phase F.2.6 — image-sequence playback (EXR / DPX / PNG / etc).
    // Mirrors MetalPlayerRenderer's wiring: cache pushes new pixel
    // data to `upload`, which uploads to the singleton texture pool
    // off the render thread; render thread polls textureForFrame()
    // each iteration.
    D3D11GpuUploadThread             upload;
    ImageSequenceCache              *cacheBoundToUpload = nullptr;
    int                              lastCacheGen       = 0;

    // Loading-spinner timing — render-thread-owned (no atomics needed;
    // only the render thread touches these). spinnerStart is set when
    // m_loadingActive is first observed true.
    bool                                  spinnerWasActive = false;
    std::chrono::steady_clock::time_point spinnerStart;

    // Phase F.2.7.1 — captureScreenshot — produces FULL-resolution
    // source-frame images (NOT a viewport readback). Three-pass:
    //   1. compositor: videoA.srv 1:1 → captureSourceRgba16f
    //   2. ocio (if engaged) OR compositor passthrough: → captureDstRgba8
    //   3. captureAnnotations: bake active + stored strokes at
    //      (0, 0, srcW, srcH) into the same RGBA8 RTV
    // Then CopyResource → captureStaging → Map → QImage RGBA8888.
    //
    // The capture's annotation renderer is a SEPARATE instance from
    // the live one so its dynamic vertex buffer state doesn't
    // collide with the live render's per-frame cursor. Same pattern
    // Metal uses (m_impl->captureAnnotations vs m_impl->annotations).
    std::atomic<bool>                screenshotPending{false};
    std::mutex                       screenshotMutex;
    std::condition_variable          screenshotCv;
    QImage                           screenshotResult;
    ComPtr<ID3D11Texture2D>          captureSourceRgba16f;
    ComPtr<ID3D11RenderTargetView>   captureSourceRgba16fRtv;
    ComPtr<ID3D11ShaderResourceView> captureSourceRgba16fSrv;
    ComPtr<ID3D11Texture2D>          captureDstRgba8;
    ComPtr<ID3D11RenderTargetView>   captureDstRgba8Rtv;
    ComPtr<ID3D11Texture2D>          captureStaging;
    int                              captureSrcW = 0;
    int                              captureSrcH = 0;
    D3D11AnnotationRenderer          captureAnnotations;
};

D3D11PlayerRenderer::D3D11PlayerRenderer()
    : m_impl(std::make_unique<Impl>()) {}

D3D11PlayerRenderer::~D3D11PlayerRenderer()
{
    shutdown();
}

// ---------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------
bool D3D11PlayerRenderer::init(PlayerWindow *window)
{
    if (!window) {
        qCritical("D3D11PlayerRenderer::init: null PlayerWindow");
        return false;
    }

    if (!m_impl->parentHwnd) {
        qCritical("D3D11PlayerRenderer::init: parent HWND not set — "
                  "WindowManager must call setParentHwnd() before init().");
        return false;
    }
    const HWND parentHwnd = m_impl->parentHwnd;

    if (!D3D11DeviceManager::instance().initialize()) {
        qCritical("D3D11PlayerRenderer::init: D3D11DeviceManager failed");
        return false;
    }
    auto *device  = static_cast<ID3D11Device *>(
        D3D11DeviceManager::instance().device());
    auto *factory = static_cast<IDXGIFactory2 *>(
        D3D11DeviceManager::instance().factory());

    if (!registerChildWindowClass()) return false;

    // Default size — WindowManager will call setViewportRect with the
    // real centerStage geometry as soon as the layout settles. Start
    // at a sensible non-zero size so the swapchain creates.
    constexpr int kInitW = 640;
    constexpr int kInitH = 360;

    m_impl->childHwnd = CreateWindowExW(
        WS_EX_NOREDIRECTIONBITMAP,   // required for DComp ownership
        kChildWindowClassName, L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
        0, 0, kInitW, kInitH,
        parentHwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!m_impl->childHwnd) {
        qCritical("D3D11PlayerRenderer::init: CreateWindowExW child HWND failed (GLE=%lu)",
                  GetLastError());
        return false;
    }
    m_impl->currentW = kInitW;
    m_impl->currentH = kInitH;

    // Phase F.2.11.x — register an IDropTarget on the child HWND so
    // OS drags over the player surface reach PlayerWindow's existing
    // filesDropped signal (which WindowManager wires to addMediaFile +
    // setActiveItem at line 941). Without this the child HWND blocks
    // every drag-drop because OLE drag-drop is HWND-scoped — neither
    // the parent's Qt drop machinery nor the QML DropAreas see drops
    // that land over the player area. OleInitialize is reference-
    // counted; Qt's GUI thread typically already calls it, so this
    // returns S_FALSE in practice. Drop-target callback fires on the
    // GUI thread (OLE dispatches on the registering thread).
    m_impl->playerWindow = window;
    {
        const HRESULT olehr = OleInitialize(nullptr);
        if (olehr == S_OK || olehr == S_FALSE) {
            m_impl->oleInitialized = (olehr == S_OK);
            PlayerWindow *pw = window;
            auto *raw = new D3D11DropTarget(
                [pw](const QList<QUrl> &urls) {
                    if (pw) pw->notifyFilesDropped(urls);
                });
            m_impl->dropTarget.Attach(raw);   // takes the refcount-of-1
            HRESULT rhr = RegisterDragDrop(m_impl->childHwnd,
                                             m_impl->dropTarget.Get());
            if (FAILED(rhr)) {
                qWarning("D3D11PlayerRenderer::init: RegisterDragDrop failed "
                         "(hr=0x%08lX); player-surface drag-drop disabled",
                         static_cast<unsigned long>(rhr));
                m_impl->dropTarget.Reset();
            }
        } else {
            qWarning("D3D11PlayerRenderer::init: OleInitialize failed "
                     "(hr=0x%08lX); player-surface drag-drop disabled",
                     static_cast<unsigned long>(olehr));
        }
    }

    // DComp device for the child HWND. Owned here; passed by raw
    // pointer to hdrSwapchain so it can create its target/visual.
    ComPtr<IDXGIDevice> dxgiDevice;
    HRESULT hr = static_cast<ID3D11Device *>(D3D11DeviceManager::instance().device())
                     ->QueryInterface(IID_PPV_ARGS(dxgiDevice.GetAddressOf()));
    if (FAILED(hr)) {
        qCritical("D3D11PlayerRenderer::init: QI IDXGIDevice (for DComp) failed (hr=0x%08lX)",
                  static_cast<unsigned long>(hr));
        return false;
    }
    if (FAILED(hr = DCompositionCreateDevice(dxgiDevice.Get(),
                                               IID_PPV_ARGS(m_impl->dcompDevice.GetAddressOf())))) {
        qCritical("D3D11PlayerRenderer::init: DCompositionCreateDevice failed (hr=0x%08lX)",
                  static_cast<unsigned long>(hr));
        return false;
    }

    // Swapchain + colorspace / HDR-mode plumbing now lives in
    // D3D11HdrSwapchain (Phase F.2.8). Owns the IDXGISwapChain1,
    // RTV, DComp target/visual; recreates the swapchain on HDR-mode
    // format flips. Seeds the initial pending mode to the renderer's
    // current m_hdrMode (defaults SdrSRgb but a setHdrMode call may
    // already have landed before init runs).
    if (!m_impl->hdrSwapchain.initialize(factory, device,
                                           m_impl->dcompDevice.Get(),
                                           m_impl->childHwnd,
                                           kInitW, kInitH)) {
        qCritical("D3D11PlayerRenderer::init: D3D11HdrSwapchain init failed");
        return false;
    }
    m_impl->hdrSwapchain.setMode(
        static_cast<HdrMode>(m_hdrMode.load(std::memory_order_acquire)));

    if (!m_impl->compositor.initialize()) {
        qCritical("D3D11PlayerRenderer::init: compositor init failed");
        return false;
    }

    // Annotation renderer targets the swapchain back buffer's pixel
    // format. F.2.8 reinits this on every HDR-mode flip; F.2.9 picks
    // a PS variant (SDR / scRGB / HDR10 PQ) keyed off the format.
    if (!m_impl->annotations.initialize(static_cast<int>(kSwapchainFormat))) {
        qWarning("D3D11PlayerRenderer::init: annotation renderer init failed "
                 "(strokes + safety overlay will not draw)");
    }
    // F.2.9 — stroke reference luminance (Guide 06 D11 default 200 nits).
    // SDR variant ignores this; scRGB + PQ variants use it to map sRGB-
    // picked stroke colors to a stable on-screen brightness.
    m_impl->annotations.setReferenceLuminance(200.0f);
    // captureAnnotations targets RGBA8_UNORM (capture textures, not
    // the BGRA8 swapchain). Separate instance keeps its dynamic
    // vertex buffer state isolated from the live pass.
    if (!m_impl->captureAnnotations.initialize(
            static_cast<int>(DXGI_FORMAT_R8G8B8A8_UNORM))) {
        qWarning("D3D11PlayerRenderer::init: capture annotation renderer init "
                 "failed (note thumbnails will lack baked-in strokes)");
    }

    if (!m_impl->ocio.initialize()) {
        // OCIO renderer init failure is non-fatal — the engine falls
        // back to the bypass path (compositor → swapchain). Real
        // pipeline build is lazy in drawFrame() once a chain bumps.
        qWarning("D3D11PlayerRenderer::init: OCIO renderer init failed "
                 "(continuing without color management): %s",
                 qPrintable(m_impl->ocio.lastError()));
    } else {
        // Wake the render thread when the OCIO worker finishes — so
        // a paused-playback OCIO change picks up the new pipeline
        // without waiting for the next decoded frame to nudge us.
        m_impl->ocio.setWakeCallback([this]{ requestUpdate(); });
    }

    // Image-sequence texture pool + GPU upload thread (Phase F.2.6).
    // Pool is a singleton; idempotent initialize(). Upload thread is
    // per-renderer. setImageSeqCache() wires the cache's push +
    // eviction callbacks to this upload thread.
    if (!D3D11TexturePool::instance().isInitialized()) {
        if (!D3D11TexturePool::instance().initialize()) {
            qWarning("D3D11PlayerRenderer::init: D3D11TexturePool init failed; "
                     "image-sequence playback unavailable");
        }
    }
    if (!m_impl->upload.start()) {
        qWarning("D3D11PlayerRenderer::init: D3D11GpuUploadThread start failed");
    }

    // F.2.13 — bring up the shared YUV compute pipeline FIRST so the
    // bridge can hand it to its init() call. Failures are soft: the
    // single-flow bridge sees a null compositor and disables Vulkan
    // intake; dual flow does the same via its setExternalYuvCompositor
    // injection path.
    if (!m_impl->yuvCompositor.initialize()) {
        qInfo("D3D11PlayerRenderer::init: YuvCompositor unavailable; "
              "FrameHandle::Vulkan frames will be silently dropped");
    } else if (!m_impl->vulkanBridge.initialize(&m_impl->yuvCompositor)) {
        qInfo("D3D11PlayerRenderer::init: VulkanDecodeBridge init failed; "
              "FrameHandle::Vulkan frames will be silently dropped");
    }

    qInfo("D3D11PlayerRenderer: ready (child HWND %p on parent %p, "
          "%dx%d initial, BGRA8 flip-discard via DComp, compositor ready)",
          m_impl->childHwnd, parentHwnd, kInitW, kInitH);

    // Spin up the render thread.
    m_stopRequested.store(false);
    m_running.store(true);
    m_renderThread = std::thread([this]{ renderThreadProc(); });

    // Late-bind cache callbacks if setImageSeqCache(c) was called
    // BEFORE init() ran. Until m_running flipped true above, that
    // setter stashed the cache pointer but the m_running gate
    // skipped callback installation. Mirror MetalPlayerRenderer's
    // init lines 363-375 so either ordering (cache-first or init-
    // first) ends up with the GPU push + eviction sinks wired.
    if (m_cache) {
        D3D11GpuUploadThread *upload = &m_impl->upload;
        ImageSequenceCache   *cache  = m_cache;
        cache->setGpuPushCallback(
            [upload, cache](int frame, std::shared_ptr<const PixelData> pixels) {
                upload->enqueue(frame, std::move(pixels),
                                cache->requestGeneration());
            });
        cache->setFrameEvictedCallback(
            [upload](int frame) { upload->evictFrame(frame); });
        m_impl->cacheBoundToUpload = m_cache;
        m_impl->lastCacheGen       = m_cache->requestGeneration();
        qInfo("D3D11PlayerRenderer::init: rebound cache callbacks for "
              "pre-init image-sequence (gen=%d)", m_impl->lastCacheGen);
    }

    requestUpdate();   // kick the first frame
    return true;
}

void D3D11PlayerRenderer::shutdown()
{
    if (!m_running.exchange(false)) return;
    m_stopRequested.store(true);
    m_wakeCv.notify_all();
    if (m_renderThread.joinable()) m_renderThread.join();

    // Detach from the cache before tearing the upload thread down —
    // otherwise an in-flight cache callback (cache thread) could call
    // into upload after stop() returns and trip a use-after-free.
    if (m_cache) {
        m_cache->setGpuPushCallback({});
        m_cache->setFrameEvictedCallback({});
    }
    m_impl->cacheBoundToUpload = nullptr;
    m_impl->upload.stop();

    // If a GUI thread is parked in captureScreenshot waiting on the
    // cv, release it with an empty result instead of letting it
    // time out 250 ms later.
    if (m_impl->screenshotPending.load()) {
        std::lock_guard lock(m_impl->screenshotMutex);
        m_impl->screenshotResult = QImage();
        m_impl->screenshotPending.store(false);
        m_impl->screenshotCv.notify_all();
    }
    m_impl->captureSourceRgba16fSrv.Reset();
    m_impl->captureSourceRgba16fRtv.Reset();
    m_impl->captureSourceRgba16f.Reset();
    m_impl->captureDstRgba8Rtv.Reset();
    m_impl->captureDstRgba8.Reset();
    m_impl->captureStaging.Reset();
    m_impl->captureSrcW = 0;
    m_impl->captureSrcH = 0;
    m_impl->captureAnnotations.shutdown();

    m_impl->vulkanBridge.shutdown();
    m_impl->annotations.shutdown();
    m_impl->ocio.shutdown();
    m_impl->compositeSrv.Reset();
    m_impl->compositeRtv.Reset();
    m_impl->compositeTex.Reset();
    m_impl->compositeW = 0;
    m_impl->compositeH = 0;
    m_impl->compositor.shutdown();
    m_impl->videoA.srv.Reset();
    m_impl->videoA.texture.Reset();
    m_impl->videoA.width  = 0;
    m_impl->videoA.height = 0;

    // Phase F.2.11.d — release DualFlow resources. Compositor's
    // shutdown drops its cached per-side textures + PSO state.
    m_impl->dualOcioOutSrv.Reset();
    m_impl->dualOcioOutRtv.Reset();
    m_impl->dualOcioOut.Reset();
    m_impl->dualOcioOutW = 0;
    m_impl->dualOcioOutH = 0;
    m_impl->dualCanvasSrv.Reset();
    m_impl->dualCanvasRtv.Reset();
    m_impl->dualCanvas.Reset();
    m_impl->dualCanvasW = 0;
    m_impl->dualCanvasH = 0;
    m_impl->dualCompositor.shutdown();
    m_impl->dualCompositorInited = false;

    // F.2.13 — tear down the shared yuv compositor AFTER the single-
    // flow bridge (above) AND after the dual compositor (which holds
    // its own two bridges). Both groups of bridges referenced
    // yuvCompositor via non-owning pointer.
    m_impl->yuvCompositor.shutdown();

    m_impl->hdrSwapchain.shutdown();
    m_impl->dcompDevice.Reset();

    // Phase F.2.11.x — revoke + release the drop target BEFORE the
    // child HWND goes away. RevokeDragDrop releases the AddRef it
    // took during register; the ComPtr drops the construction
    // refcount when reset below → drop target deletes.
    if (m_impl->childHwnd && m_impl->dropTarget) {
        RevokeDragDrop(m_impl->childHwnd);
    }
    m_impl->dropTarget.Reset();
    m_impl->playerWindow = nullptr;
    if (m_impl->oleInitialized) {
        OleUninitialize();
        m_impl->oleInitialized = false;
    }

    if (m_impl->childHwnd) {
        DestroyWindow(m_impl->childHwnd);
        m_impl->childHwnd = nullptr;
    }
    m_impl->parentHwnd = nullptr;
}

// ---------------------------------------------------------------------
// Wake primitives + resize plumbing
// ---------------------------------------------------------------------
void D3D11PlayerRenderer::requestUpdate()
{
    m_pendingDraw.store(true, std::memory_order_release);
    m_wakeCv.notify_one();
}

void D3D11PlayerRenderer::resize(int width, int height)
{
    if (width <= 0 || height <= 0) return;
    {
        std::lock_guard lock(m_resizeMutex);
        m_pendingResizeW = width;
        m_pendingResizeH = height;
        m_resizePending  = true;
    }
    requestUpdate();
}

void D3D11PlayerRenderer::setViewportRect(int x, int y, int width, int height)
{
    if (width <= 0 || height <= 0) return;
    {
        std::lock_guard lock(m_resizeMutex);
        m_pendingViewportX = x;
        m_pendingViewportY = y;
        m_pendingResizeW   = width;
        m_pendingResizeH   = height;
        m_viewportPending  = true;
        m_resizePending    = true;
    }
    requestUpdate();
}

void D3D11PlayerRenderer::applyPendingResizeIfNeeded()
{
    int x = 0, y = 0, w = 0, h = 0;
    bool sizeChange = false;
    bool posChange  = false;
    {
        std::lock_guard lock(m_resizeMutex);
        if (!m_resizePending && !m_viewportPending) return;
        x = m_pendingViewportX;
        y = m_pendingViewportY;
        w = m_pendingResizeW;
        h = m_pendingResizeH;
        sizeChange = m_resizePending;
        posChange  = m_viewportPending;
        m_resizePending   = false;
        m_viewportPending = false;
    }
    if (w <= 0 || h <= 0) return;

    // Order matters: resize the swapchain FIRST, then move the HWND.
    // Otherwise DComp briefly sees the old (smaller) back buffer
    // stretched across the new (larger) HWND — which during fast
    // transitions (e.g. borderless-fullscreen) can look like tiled /
    // duplicated content against the bg fill.
    bool sizeActuallyChanged = false;
    if (sizeChange && (w != m_impl->currentW || h != m_impl->currentH)) {
        if (!m_impl->hdrSwapchain.resize(w, h)) {
            qWarning("D3D11PlayerRenderer: hdrSwapchain.resize(%d, %d) failed",
                     w, h);
            return;
        }
        m_impl->currentW = w;
        m_impl->currentH = h;
        sizeActuallyChanged = true;
        // Drop the OCIO intermediate; drawFrame() will recreate it
        // at the new dims the next time OCIO is engaged.
        m_impl->compositeSrv.Reset();
        m_impl->compositeRtv.Reset();
        m_impl->compositeTex.Reset();
        m_impl->compositeW = 0;
        m_impl->compositeH = 0;
    }
    if ((posChange || sizeChange) && m_impl->childHwnd) {
        SetWindowPos(m_impl->childHwnd, nullptr, x, y, w, h,
                      SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
    // After a real resize, give DComp a freshly-presented back buffer
    // at the new dims immediately — don't wait for the next wake-cv
    // tick. Seed with #1b1b1b (27/255 = 0.1059) to match the QML
    // centerStage Rectangle and WindowManager's WM_ERASEBKGND fill,
    // so the 1-frame seam during rail-collapse animations blends in.
    // The normal drawFrame() right after this will overlay the real
    // content.
    auto *rtv       = static_cast<ID3D11RenderTargetView *>(m_impl->hdrSwapchain.rtv());
    auto *swapchain = static_cast<IDXGISwapChain1 *>(m_impl->hdrSwapchain.swapchain());
    if (sizeActuallyChanged && rtv && swapchain) {
        auto *ctx = static_cast<ID3D11DeviceContext *>(
            D3D11DeviceManager::instance().context());
        const float seed[4] = {0.1059f, 0.1059f, 0.1059f, 1.f};
        ctx->ClearRenderTargetView(rtv, seed);
        swapchain->Present(0, 0);
        m_pendingDraw.store(true, std::memory_order_release);
    }
}

// ---------------------------------------------------------------------
// Render loop (render-on-demand pattern from F.0 archive doc 07)
// ---------------------------------------------------------------------
void D3D11PlayerRenderer::renderThreadProc()
{
    using namespace std::chrono_literals;
    int lastOcioGen = -1;
    while (!m_stopRequested.load()) {
        applyPendingResizeIfNeeded();

        // Pull the latest decoded frame each iteration. fetchLatest
        // returns false if nothing's new; cheap when there's no
        // playback in progress. If we got a new frame, force a draw.
        const bool gotNewFrame = consumeLatestVideoFrame();

        // OCIO chain-gen polling — the user can change view/look/LUT
        // while playback is paused. With no decoded frame arriving
        // to nudge the loop, drawFrame() (and therefore the OCIO
        // rebuild()) wouldn't otherwise run. Poll once per wake tick
        // (~16ms) and force a draw when the chain bumps.
        bool ocioBumped = false;
        if (m_ocio && m_ocio->engaged()) {
            const int gen = m_ocio->activeChainGeneration();
            if (gen != lastOcioGen) {
                lastOcioGen = gen;
                ocioBumped  = true;
            }
        }

        const bool dirty = m_pendingDraw.exchange(false, std::memory_order_acq_rel);
        // While a load is in progress force a draw each tick so the
        // loading spinner animates even though the main thread is
        // blocked in the open call.
        if (dirty || gotNewFrame || ocioBumped
            || m_loadingActive.load(std::memory_order_acquire)) {
            drawFrame();
        }

        std::unique_lock lk(m_wakeMutex);
        m_wakeCv.wait_for(lk, 16ms, [this]{
            return m_stopRequested.load() || m_pendingDraw.load();
        });
    }
}

bool D3D11PlayerRenderer::consumeLatestVideoFrame()
{
    // Phase F.2.6 — image-sequence path takes precedence over video.
    // The cache owns the active sequence; the upload thread holds
    // GPU textures keyed by frame number; we route the playhead's
    // texture into the videoA slot via the same compositor path the
    // video decoder uses.
    if (m_cache) {
        // Big-seek / layer-change detection: cache bumps generation
        // on user seeks beyond the small-rewind threshold or on EXR
        // layer swap. Mirror that to the upload thread so any items
        // queued under the old gen are dropped + the texture map
        // wipes.
        const int curGen = m_cache->requestGeneration();
        if (curGen != m_impl->lastCacheGen) {
            m_impl->lastCacheGen = curGen;
            m_impl->upload.setGeneration(curGen);
        }
        const int playhead = m_cache->currentPlayhead();
        const auto tex = m_impl->upload.textureForFrame(playhead);

        if (tex.valid && tex.handle) {
            const auto *pt = D3D11TexturePool::instance().texture(tex.handle);
            if (pt && pt->valid && pt->srv) {
                auto *srv = static_cast<ID3D11ShaderResourceView *>(pt->srv);
                auto &slot = m_impl->videoA;
                // Drop any prior video-owned texture (Cpu/Bridge) so
                // the previous frame doesn't linger if the user
                // toggled from video → image-seq.
                if (slot.owner != Impl::SlotOwner::ImageSeq ||
                    slot.srv.Get() != srv) {
                    slot.srv.Reset();
                    slot.texture.Reset();
                    srv->AddRef();
                    slot.srv.Attach(srv);
                    slot.width  = pt->width;
                    slot.height = pt->height;
                    slot.owner  = Impl::SlotOwner::ImageSeq;
                    D3D11TexturePool::instance().touch(tex.handle);
                    return true;   // forced redraw on slot swap
                }
                // Slot already points at this texture — nothing new.
                // Still tell the pool it's hot so eviction skips it.
                D3D11TexturePool::instance().touch(tex.handle);
            }
        }
        // Cache miss / not-yet-uploaded: keep the last-good texture
        // bound (slot stays put). Eviction sink will purge stale
        // handles via the pool's 3-frame graveyard.
        return false;
    }

    // When no cache is active but a prior session left an image-seq
    // texture bound, release it so the video path can rebuild.
    if (m_impl->videoA.owner == Impl::SlotOwner::ImageSeq) {
        m_impl->videoA.srv.Reset();
        m_impl->videoA.texture.Reset();
        m_impl->videoA.width  = 0;
        m_impl->videoA.height = 0;
        m_impl->videoA.owner  = Impl::SlotOwner::None;
    }

    if (!m_decoder) return false;
    FrameHandle h;
    if (!m_decoder->fetchLatest(&h) || !h.isValid()) return false;

    // Phase F.2.4.3 Stage C: route Vulkan-decoded frames through the
    // D3D11VulkanDecodeBridge. The bridge returns an ImportedFrame
    // whose single plane's SRV references the D3D11-allocated /
    // Vulkan-imported shared RGBA16F texture. We point the videoA
    // slot at that SRV so the compositor samples it. Until the
    // Stage C.2 compute pass writes pixels, the texture is D3D11's
    // default zero-init (black) — but the plumbing is alive end-to-end.
    if (h.kind() == FrameHandle::Kind::Vulkan) {
        if (!m_impl->vulkanBridge.isInitialized()) return false;
        // Forward the user's per-clip Range pill into the YUV→RGB
        // compute. macOS does the same via the rangeOverride arg
        // through the Metal renderer's drawSlot helper.
        const int rangeOv = m_decoder ? m_decoder->rangeOverride() : 0;
        const auto *imp = m_impl->vulkanBridge.consume(h, rangeOv);
        if (!imp || imp->planes.empty()) return false;
        const auto &p = imp->planes.front();
        if (!p.srv) return false;

        auto &slot = m_impl->videoA;
        // The bridge owns the texture + srv lifetimes. We hold
        // non-owning ComPtrs by AddRef'ing.
        if (slot.srv.Get() != p.srv) {
            slot.srv.Reset();
            slot.texture.Reset();
            p.srv->AddRef();
            slot.srv.Attach(p.srv);
            if (p.texture) {
                p.texture->AddRef();
                slot.texture.Attach(p.texture);
            }
            slot.width  = imp->pictureWidth;
            slot.height = imp->pictureHeight;
            slot.owner  = Impl::SlotOwner::Bridge;
        }
        return true;   // dirty: trigger a redraw of the (currently black) texture
    }
    if (h.kind() != FrameHandle::Kind::Cpu) return false;

    // If the slot was last set by the Vulkan bridge (RGBA16F shared
    // texture we don't own), drop those references so the CPU path
    // can rebuild a renderer-owned RGBA8 texture from scratch. Without
    // this transition reset, the next UpdateSubresource would write
    // RGBA8 bytes into the bridge's RGBA16F memory and produce
    // garbage (or persist the bridge's last contents — the visible
    // "blue rectangle on alpha drop" bug).
    if (m_impl->videoA.owner == Impl::SlotOwner::Bridge) {
        m_impl->videoA.srv.Reset();
        m_impl->videoA.texture.Reset();
        m_impl->videoA.width  = 0;
        m_impl->videoA.height = 0;
        m_impl->videoA.owner  = Impl::SlotOwner::None;
    }
    const QImage img = h.cpuImage();
    if (img.isNull()) return false;

    QImage frame = img;
    if (frame.format() != QImage::Format_RGBA8888) {
        frame = frame.convertToFormat(QImage::Format_RGBA8888);
    }
    const int w = frame.width();
    const int h2 = frame.height();
    if (w <= 0 || h2 <= 0) return false;

    auto *device = static_cast<ID3D11Device *>(
        D3D11DeviceManager::instance().device());
    auto *ctx    = static_cast<ID3D11DeviceContext *>(
        D3D11DeviceManager::instance().context());
    if (!device || !ctx) return false;

    auto &slot = m_impl->videoA;
    if (slot.width != w || slot.height != h2 || !slot.texture) {
        // Recreate slot at new dimensions.
        slot.srv.Reset();
        slot.texture.Reset();

        D3D11_TEXTURE2D_DESC td{};
        td.Width  = static_cast<UINT>(w);
        td.Height = static_cast<UINT>(h2);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format    = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage     = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA init{};
        init.pSysMem     = frame.constBits();
        init.SysMemPitch = static_cast<UINT>(frame.bytesPerLine());

        if (FAILED(device->CreateTexture2D(&td, &init, slot.texture.GetAddressOf()))) {
            qWarning("D3D11PlayerRenderer: video slot CreateTexture2D %dx%d failed",
                     w, h2);
            return false;
        }
        if (FAILED(device->CreateShaderResourceView(slot.texture.Get(), nullptr,
                                                      slot.srv.GetAddressOf()))) {
            slot.texture.Reset();
            qWarning("D3D11PlayerRenderer: video slot CreateShaderResourceView failed");
            return false;
        }
        slot.width  = w;
        slot.height = h2;
        slot.owner  = Impl::SlotOwner::Cpu;
    } else {
        // In-place upload — frame dims unchanged.
        ctx->UpdateSubresource(slot.texture.Get(), 0, nullptr,
                                 frame.constBits(),
                                 static_cast<UINT>(frame.bytesPerLine()), 0);
    }
    return true;
}

void D3D11PlayerRenderer::drawFrame()
{
    // Phase F.2.11.d — DualFlow dispatch. When the user enters
    // SBS / Wipe, WindowManager flips m_rendererMode to DualFlow
    // and hands us an IDualFrameSource via setDualFrameSource. The
    // dual branch runs an independent composite → OCIO → present-
    // blit pipeline; control returns here only after Present.
    const RendererMode mode =
        static_cast<RendererMode>(m_rendererMode.load(std::memory_order_acquire));

    // F.2.13 — single-flow bridge lifecycle pinned to mode. macOS
    // closes the single-flow decoder on dual entry so its YUV state
    // goes idle (no consume() calls); we additionally tear down our
    // vulkanBridge here so its SharedRgba D3D11 texture + viewCache
    // VkImageViews aren't held against a now-defunct AVHWFramesContext.
    // On dual exit we re-initialize it before single-flow's first
    // consume(). Runs on the render thread to avoid a race with
    // consumeLatestVideoFrame.
    if (mode == RendererMode::DualFlow) {
        if (m_impl->vulkanBridge.isInitialized()) {
            m_impl->vulkanBridge.shutdown();
            qInfo("D3D11PlayerRenderer: dual entry — single-flow vulkanBridge "
                  "torn down (mirrors macOS idle pixbufBridge)");
        }
    } else {
        if (m_impl->yuvCompositor.isInitialized() &&
            !m_impl->vulkanBridge.isInitialized()) {
            if (m_impl->vulkanBridge.initialize(&m_impl->yuvCompositor)) {
                qInfo("D3D11PlayerRenderer: single entry — vulkanBridge restored");
            }
        }
    }

    if (mode == RendererMode::DualFlow) {
        drawDualFrame();
        return;
    }

    // Phase F.2.8 — HDR mode pivot. If the user toggled the HDR mode
    // since the last frame, hdrSwapchain may have recreated the
    // swapchain at a new format/colorspace. The annotation renderer
    // is the only PSO that targets the swapchain RTV with format-
    // bound state (capture instance uses RGBA8 always; compositor +
    // OCIO write to the RTV with format-agnostic state). Reinit it
    // when the format changes.
    if (m_impl->hdrSwapchain.applyPendingOnRenderThread()) {
        m_impl->annotations.shutdown();
        if (!m_impl->annotations.initialize(m_impl->hdrSwapchain.currentFormat())) {
            qWarning("D3D11PlayerRenderer: annotation renderer reinit after HDR "
                     "mode change failed");
        }
        // Reference luminance is per-instance state; shutdown()
        // releases it, so re-set after each format-flip init.
        m_impl->annotations.setReferenceLuminance(200.0f);
    }

    auto *rtv       = static_cast<ID3D11RenderTargetView *>(m_impl->hdrSwapchain.rtv());
    auto *swapchain = static_cast<IDXGISwapChain1 *>(m_impl->hdrSwapchain.swapchain());
    if (!rtv || !swapchain) return;

    // Tick the graveyard each frame so textures evicted N frames ago
    // actually get released. The MAIN pool drains image-seq cache
    // evictions; the THUMBNAIL pool drains hover-thumbnail evictions.
    // Without the thumbnail drain, thumb textures queued for deletion
    // piled up forever (handles leaked → VRAM exhaustion → render
    // thread stalling on the pool mutex). Cheap when nothing's pending.
    D3D11TexturePool::instance().processPendingDeletions();
    D3D11TexturePool::thumbnailInstance().processPendingDeletions();
    auto *ctx = static_cast<ID3D11DeviceContext *>(
        D3D11DeviceManager::instance().context());
    auto *device = static_cast<ID3D11Device *>(
        D3D11DeviceManager::instance().context() ?
            D3D11DeviceManager::instance().device() : nullptr);

    // Decide whether OCIO is engaged this frame. Three conditions
    // must all hold: a manager is attached, it's engaged, and the
    // active chain is buildable (rebuild() succeeds OR has succeeded
    // earlier on the same generation).
    bool useOcio = (m_ocio != nullptr) && m_ocio->engaged() &&
                   m_impl->ocio.isInitialized();
    if (useOcio) {
        useOcio = m_impl->ocio.rebuild(m_ocio);
    }

    const int  W = m_impl->currentW;
    const int  H = m_impl->currentH;

    if (useOcio) {
        // Lazy (re)create the RGBA16F intermediate at swapchain size.
        if (!m_impl->compositeTex ||
            m_impl->compositeW != W || m_impl->compositeH != H) {
            m_impl->compositeSrv.Reset();
            m_impl->compositeRtv.Reset();
            m_impl->compositeTex.Reset();
            D3D11_TEXTURE2D_DESC td{};
            td.Width     = static_cast<UINT>(W);
            td.Height    = static_cast<UINT>(H);
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format    = DXGI_FORMAT_R16G16B16A16_FLOAT;
            td.SampleDesc.Count = 1;
            td.Usage     = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            HRESULT thr = device->CreateTexture2D(&td, nullptr, m_impl->compositeTex.GetAddressOf());
            if (FAILED(thr)) {
                qWarning("D3D11PlayerRenderer: composite RGBA16F create %dx%d failed (hr=0x%08lX)",
                         W, H, static_cast<unsigned long>(thr));
                useOcio = false;
            } else if (FAILED(device->CreateRenderTargetView(
                       m_impl->compositeTex.Get(), nullptr,
                       m_impl->compositeRtv.GetAddressOf())) ||
                FAILED(device->CreateShaderResourceView(
                       m_impl->compositeTex.Get(), nullptr,
                       m_impl->compositeSrv.GetAddressOf()))) {
                qWarning("D3D11PlayerRenderer: composite RTV/SRV create failed");
                useOcio = false;
                m_impl->compositeTex.Reset();
            } else {
                m_impl->compositeW = W;
                m_impl->compositeH = H;
            }
        }
    }

    // Hover-thumb corner overlays. Drawn into the compositor's
    // destination (intermediate RGBA16F when OCIO engaged, or
    // directly into the swapchain when bypassed) IMMEDIATELY after
    // the compositor and BEFORE OCIO runs — so OCIO color-treats
    // the thumb the same way it treats the main image. Mirrors the
    // dual-flow placement at drawDualFrame and the macOS Metal
    // single-flow placement at metal_player_renderer.mm:1388-1426.
    // A in BL, B in BR (B only populated in dual mode by
    // WindowManager). 0 = no overlay.
    auto drawHoverThumbs = [&](int dstW, int dstH) {
        const unsigned long long hA = m_hoverThumbHandle.load();
        if (hA != 0) {
            const auto *t = D3D11TexturePool::thumbnailInstance().texture(hA);
            if (t && t->srv) {
                m_impl->compositor.renderCornerOverlay(
                    ctx, t->srv,
                    t->width, t->height,
                    dstW, dstH,
                    /*corner=*/0, /*overlayFrac=*/0.18f, /*marginPx=*/12.0f);
            }
        }
        const unsigned long long hB = m_hoverThumbHandleB.load();
        if (hB != 0) {
            const auto *t = D3D11TexturePool::thumbnailInstance().texture(hB);
            if (t && t->srv) {
                m_impl->compositor.renderCornerOverlay(
                    ctx, t->srv,
                    t->width, t->height,
                    dstW, dstH,
                    /*corner=*/1, 0.18f, 12.0f);
            }
        }
    };

    if (useOcio) {
        // Compositor → intermediate RTV.
        ID3D11RenderTargetView *interRtv = m_impl->compositeRtv.Get();
        ctx->OMSetRenderTargets(1, &interRtv, nullptr);
        D3D11_VIEWPORT vp{};
        vp.Width  = static_cast<float>(W);
        vp.Height = static_cast<float>(H);
        vp.MinDepth = 0; vp.MaxDepth = 1;
        ctx->RSSetViewports(1, &vp);

        m_impl->compositor.renderSingle(
            ctx,
            m_impl->videoA.srv.Get(),
            W, H,
            m_impl->videoA.width, m_impl->videoA.height,
            m_bgMode.load());

        // Thumbs into intermediate so OCIO processes them too.
        drawHoverThumbs(W, H);

        // OCIO → swapchain RTV.
        m_impl->ocio.apply(
            ctx,
            m_impl->compositeSrv.Get(),
            rtv,
            W, H);
    } else {
        // Bypass: compositor draws directly to the swapchain. Matches
        // the pre-F.2.5 path verbatim.
        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        D3D11_VIEWPORT vp{};
        vp.Width  = static_cast<float>(W);
        vp.Height = static_cast<float>(H);
        vp.MinDepth = 0; vp.MaxDepth = 1;
        ctx->RSSetViewports(1, &vp);

        m_impl->compositor.renderSingle(
            ctx,
            m_impl->videoA.srv.Get(),
            W, H,
            m_impl->videoA.width, m_impl->videoA.height,
            m_bgMode.load());

        // No OCIO either way — thumbs into swapchain at the same
        // place the video pixels landed, so they share whatever
        // (lack of) color treatment the bypass path produces.
        drawHoverThumbs(W, H);
    }

    // Phase F.2.7 — annotation + safety-overlay pass. Drawn POST-OCIO
    // (after color correction) and PRE-present, into the swapchain
    // RTV. Mirrors MetalPlayerRenderer's draw lines 1504-1567:
    //
    //   beginFrame() resets the vertex-buffer append cursor →
    //   active stroke (if any) → stored strokes (snapshot under
    //   mutex) → safety overlay.
    //
    // Aspect-fit math is duplicated from D3D11Compositor::renderSingle
    // (single source of fitRect would be cleaner; defer until F.2.10
    // cleanup). Annotator viewport sync uses the same letterbox rect
    // so normalized stroke coords map to the source's visible area.
    if (m_impl->annotations.isInitialized()) {
        m_impl->annotations.beginFrame();

        const float dstWf = static_cast<float>(W);
        const float dstHf = static_cast<float>(H);
        float fitW = dstWf, fitH = dstHf;
        const int srcW = m_impl->videoA.width;
        const int srcH = m_impl->videoA.height;
        if (m_impl->videoA.srv && srcW > 0 && srcH > 0) {
            const float srcAspect = static_cast<float>(srcW) / srcH;
            const float dstAspect = dstWf / dstHf;
            if (srcAspect > dstAspect) {
                fitW = dstWf;
                fitH = dstWf / srcAspect;
            } else {
                fitH = dstHf;
                fitW = dstHf * srcAspect;
            }
        }
        const float fitX = (dstWf - fitW) * 0.5f;
        const float fitY = (dstHf - fitH) * 0.5f;
        // Stroke thickness scales with the fit width relative to the
        // source — keeps line weight perceptually constant across
        // zoom + window resize. Falls back to 1.0 when no source.
        const float lineScale = (srcW > 0) ? (fitW / static_cast<float>(srcW))
                                            : 1.0f;

        // Active stroke (real-time, the one the user is drawing).
        // Snapshot under lock — onPointerEvent() mutates the stroke's
        // points vector on the GUI thread, and dereferencing the raw
        // pointer mid-mutation trips MSVC Debug CRT's iterator
        // checks. Metal's raw-pointer path works only because libc++
        // is more permissive.
        if (m_annotator) {
            m_annotator->setViewportRect(QPointF(fitX, fitY),
                                          QSizeF(fitW, fitH));
            ActiveStroke activeCopy;
            if (m_annotator->snapshotActiveStroke(activeCopy)) {
                TessellatedMesh mesh = StrokeTessellator::tessellate(
                    activeCopy, fitX, fitY, fitW, fitH, lineScale);
                if (!mesh.vertices.empty()) {
                    m_impl->annotations.drawMesh(ctx, mesh, W, H);
                }
            }
        }

        // Stored strokes (committed annotations for the current frame).
        {
            std::vector<ActiveStroke> snapshot;
            {
                std::lock_guard lock(m_storedStrokesMutex);
                snapshot = m_storedStrokes;
            }
            for (const ActiveStroke &s : snapshot) {
                TessellatedMesh mesh = StrokeTessellator::tessellate(
                    s, fitX, fitY, fitW, fitH, lineScale);
                if (!mesh.vertices.empty()) {
                    m_impl->annotations.drawMesh(ctx, mesh, W, H);
                }
            }
        }

        // Safety overlay (TV-safe guides etc.). Aspect-fit into the
        // displayed video rect so safety markers track the source's
        // visible area — not the bg-fill bars on letterboxed /
        // pillarboxed clips. Falls back to the full drawable when
        // there's no source loaded (shows the SVG at canvas size as
        // a visible preview while the user picks one).
        //
        // NOTE: this intentionally diverges from MetalPlayerRenderer's
        // current canvas-fit behavior. macOS Metal is left untouched
        // per the Phase F rule; we can unify post-Phase F if needed.
        if (m_safety && m_safety->isLoaded()) {
            const bool haveSource = m_impl->videoA.srv && srcW > 0 && srcH > 0;
            const QPointF origin = haveSource
                                   ? QPointF(fitX, fitY)
                                   : QPointF(0, 0);
            const QSizeF  size   = haveSource
                                   ? QSizeF(fitW, fitH)
                                   : QSizeF(dstWf, dstHf);
            TessellatedMesh mesh = m_safety->mesh(origin, size);
            if (!mesh.vertices.empty()) {
                m_impl->annotations.drawMesh(ctx, mesh, W, H);
            }
        }
    }

    // (Hover-thumb overlays moved to be drawn into the compositor's
    // destination BEFORE OCIO, above — so OCIO color-treats them
    // the same way it treats the main image. Matches Metal single-
    // flow at metal_player_renderer.mm:1388-1426 + dual-flow
    // placement at drawDualFrame.)

    // Phase F.2.7.1 — capture-after-render. Done BEFORE Present so
    // we read the back buffer we just rendered (FLIP_DISCARD rotates
    // buffer 0 to the next render target on Present — capturing
    // post-Present would read the wrong one).
    if (m_impl->screenshotPending.load(std::memory_order_acquire)) {
        serviceScreenshotRequest();
    }

    // Loading spinner overlay — after the screenshot capture so it's
    // not baked into screenshots, and last so it's on top of content.
    drawLoadingSpinner(ctx, rtv);

    swapchain->Present(1 /*sync to vsync*/, 0);
}

// Phase F.2.11.d — DualFlow render path. Mirrors
// MetalPlayerRenderer's dual branch (metal_player_renderer.mm:695-
// 990): composite both sides into a capped RGBA16F canvas, optional
// OCIO over the canvas, present-blit canvas → swapchain, draw
// annotations + safety overlay on the swapchain, Present.
//
// v1: canvas-fit annotations + safety overlay (single rect across
// the whole canvas, not per-side). Per-side safety overlay rects
// are a follow-up (Metal does this; we'll match once basic
// rendering is verified). See guide 23 §1.5.
void D3D11PlayerRenderer::drawDualFrame()
{
    // HDR mode pivot — same handling as single-flow. Re-init the
    // annotation renderer if the swapchain format flipped.
    if (m_impl->hdrSwapchain.applyPendingOnRenderThread()) {
        m_impl->annotations.shutdown();
        if (!m_impl->annotations.initialize(m_impl->hdrSwapchain.currentFormat())) {
            qWarning("D3D11PlayerRenderer: annotation renderer reinit after HDR "
                     "mode change failed (dual flow)");
        }
        m_impl->annotations.setReferenceLuminance(200.0f);
    }

    auto *rtv       = static_cast<ID3D11RenderTargetView *>(m_impl->hdrSwapchain.rtv());
    auto *swapchain = static_cast<IDXGISwapChain1 *>(m_impl->hdrSwapchain.swapchain());
    if (!rtv || !swapchain) return;

    D3D11TexturePool::instance().processPendingDeletions();
    D3D11TexturePool::thumbnailInstance().processPendingDeletions();

    auto *ctx = static_cast<ID3D11DeviceContext *>(
        D3D11DeviceManager::instance().context());
    auto *device = static_cast<ID3D11Device *>(
        D3D11DeviceManager::instance().device());
    if (!ctx || !device) return;

    IDualFrameSource *frameSource =
        m_dualFrameSource.load(std::memory_order_acquire);

    // Lazy-init the dual compositor on first DualFlow entry.
    if (!m_impl->dualCompositorInited) {
        if (!m_impl->dualCompositor.initialize()) {
            qWarning("D3D11PlayerRenderer::drawDualFrame: dual compositor init "
                     "failed; skipping frame");
            // Bail before Present so the swapchain doesn't show garbage.
            return;
        }
        // F.2.13 — hand the shared yuv compositor to the dual compositor
        // so its per-slot bridges initialize against the same pipeline
        // single-flow uses. One compute pipeline across both slots is
        // the macOS-faithful shape that fixes dual-mode stutter.
        m_impl->dualCompositor.setExternalYuvCompositor(
            m_impl->yuvCompositor.isInitialized() ? &m_impl->yuvCompositor : nullptr);
        m_impl->dualCompositorInited = true;
    }

    // Push current source / mode / split each frame — cheap; the
    // compositor's setters early-out on unchanged values.
    m_impl->dualCompositor.setFrameSource(frameSource);
    m_impl->dualCompositor.setMode(
        static_cast<CompositorMode>(m_compMode.load(std::memory_order_acquire)));
    m_impl->dualCompositor.setSplitPos(m_splitPos.load(std::memory_order_acquire));
    m_impl->dualCompositor.setSeamHighlight(
        m_splitSeamHighlight.load(std::memory_order_acquire));

    const int W = m_impl->currentW;
    const int H = m_impl->currentH;
    const int canvasW = std::max(1, std::min(W, kMaxDualCanvasW));
    const int canvasH = std::max(1, std::min(H, kMaxDualCanvasH));

    // Lazy-create / resize the canvas RGBA16F texture.
    if (!m_impl->dualCanvas ||
        m_impl->dualCanvasW != canvasW || m_impl->dualCanvasH != canvasH) {
        m_impl->dualCanvasSrv.Reset();
        m_impl->dualCanvasRtv.Reset();
        m_impl->dualCanvas.Reset();
        D3D11_TEXTURE2D_DESC td{};
        td.Width     = static_cast<UINT>(canvasW);
        td.Height    = static_cast<UINT>(canvasH);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format    = DXGI_FORMAT_R16G16B16A16_FLOAT;
        td.SampleDesc.Count = 1;
        td.Usage     = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        HRESULT hr = device->CreateTexture2D(&td, nullptr,
                                              m_impl->dualCanvas.GetAddressOf());
        if (FAILED(hr) ||
            FAILED(device->CreateRenderTargetView(
                       m_impl->dualCanvas.Get(), nullptr,
                       m_impl->dualCanvasRtv.GetAddressOf())) ||
            FAILED(device->CreateShaderResourceView(
                       m_impl->dualCanvas.Get(), nullptr,
                       m_impl->dualCanvasSrv.GetAddressOf()))) {
            qWarning("D3D11PlayerRenderer::drawDualFrame: dualCanvas %dx%d "
                     "alloc failed (hr=0x%08lX)",
                     canvasW, canvasH, static_cast<unsigned long>(hr));
            return;
        }
        m_impl->dualCanvasW = canvasW;
        m_impl->dualCanvasH = canvasH;
    }

    // Pull frames + populate per-side cached textures inside the
    // compositor. Cheap no-op if frameSource is null (compositor
    // sets prepared=false, renderFrame returns).
    m_impl->dualCompositor.prepareFrames(ctx);

    // Pass 1 — dual composite into the canvas.
    {
        ID3D11RenderTargetView *canvasRtv = m_impl->dualCanvasRtv.Get();
        ctx->OMSetRenderTargets(1, &canvasRtv, nullptr);
        const float clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        ctx->ClearRenderTargetView(canvasRtv, clear);
        D3D11_VIEWPORT vp{};
        vp.Width    = static_cast<float>(canvasW);
        vp.Height   = static_cast<float>(canvasH);
        vp.MinDepth = 0; vp.MaxDepth = 1;
        ctx->RSSetViewports(1, &vp);
        m_impl->dualCompositor.renderFrame(ctx, canvasW, canvasH);

        // Hover-thumb corner overlays — drawn into the canvas BEFORE
        // OCIO so the thumb shares the main image's color treatment,
        // and rides through the present-blit's aspect-fit upscale
        // naturally (no need to anchor to the swapchain's canvas-fit
        // rect). Mirrors metal_player_renderer.mm:784-819.
        const unsigned long long hA = m_hoverThumbHandle.load();
        if (hA != 0) {
            const auto *t = D3D11TexturePool::thumbnailInstance().texture(hA);
            if (t && t->srv) {
                m_impl->compositor.renderCornerOverlay(
                    ctx, t->srv,
                    t->width, t->height,
                    canvasW, canvasH,
                    /*corner=*/0, /*overlayFrac=*/0.18f, /*marginPx=*/12.0f);
            }
        }
        const unsigned long long hB = m_hoverThumbHandleB.load();
        if (hB != 0) {
            const auto *t = D3D11TexturePool::thumbnailInstance().texture(hB);
            if (t && t->srv) {
                m_impl->compositor.renderCornerOverlay(
                    ctx, t->srv,
                    t->width, t->height,
                    canvasW, canvasH,
                    /*corner=*/1, 0.18f, 12.0f);
            }
        }
    }

    // Pass 1b — OCIO over the canvas (one chain over the composited
    // image, not per-side; matches Metal's behavior per Guide 01 §7).
    ID3D11ShaderResourceView *correctedSrv = m_impl->dualCanvasSrv.Get();
    {
        bool useOcio = (m_ocio != nullptr) && m_ocio->engaged() &&
                       m_impl->ocio.isInitialized();
        if (useOcio) useOcio = m_impl->ocio.rebuild(m_ocio);

        if (useOcio) {
            // Lazy-create / resize the OCIO output intermediate at canvas size.
            if (!m_impl->dualOcioOut ||
                m_impl->dualOcioOutW != canvasW ||
                m_impl->dualOcioOutH != canvasH) {
                m_impl->dualOcioOutSrv.Reset();
                m_impl->dualOcioOutRtv.Reset();
                m_impl->dualOcioOut.Reset();
                D3D11_TEXTURE2D_DESC td{};
                td.Width     = static_cast<UINT>(canvasW);
                td.Height    = static_cast<UINT>(canvasH);
                td.MipLevels = 1;
                td.ArraySize = 1;
                td.Format    = DXGI_FORMAT_R16G16B16A16_FLOAT;
                td.SampleDesc.Count = 1;
                td.Usage     = D3D11_USAGE_DEFAULT;
                td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
                HRESULT hr = device->CreateTexture2D(&td, nullptr,
                                                       m_impl->dualOcioOut.GetAddressOf());
                if (FAILED(hr) ||
                    FAILED(device->CreateRenderTargetView(
                               m_impl->dualOcioOut.Get(), nullptr,
                               m_impl->dualOcioOutRtv.GetAddressOf())) ||
                    FAILED(device->CreateShaderResourceView(
                               m_impl->dualOcioOut.Get(), nullptr,
                               m_impl->dualOcioOutSrv.GetAddressOf()))) {
                    qWarning("D3D11PlayerRenderer::drawDualFrame: dualOcioOut "
                             "%dx%d alloc failed", canvasW, canvasH);
                    useOcio = false;
                    m_impl->dualOcioOut.Reset();
                } else {
                    m_impl->dualOcioOutW = canvasW;
                    m_impl->dualOcioOutH = canvasH;
                }
            }
        }

        if (useOcio) {
            m_impl->ocio.apply(
                ctx,
                m_impl->dualCanvasSrv.Get(),
                m_impl->dualOcioOutRtv.Get(),
                canvasW, canvasH);
            correctedSrv = m_impl->dualOcioOutSrv.Get();
        }
    }

    // Pass 2 — present-blit canvas → swapchain. The single compositor
    // does background fill + aspect-fit blit in one PS, same as the
    // single-flow path uses to letterbox a video into the canvas.
    {
        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        D3D11_VIEWPORT vp{};
        vp.Width    = static_cast<float>(W);
        vp.Height   = static_cast<float>(H);
        vp.MinDepth = 0; vp.MaxDepth = 1;
        ctx->RSSetViewports(1, &vp);

        m_impl->compositor.renderSingle(
            ctx,
            correctedSrv,
            W, H,
            canvasW, canvasH,
            m_bgMode.load());
    }

    // Pass 3 — annotations + safety overlay on the swapchain RTV.
    // Canvas-fit (single rect over the displayed canvas) for v1.
    if (m_impl->annotations.isInitialized()) {
        m_impl->annotations.beginFrame();

        const float dstWf = static_cast<float>(W);
        const float dstHf = static_cast<float>(H);
        // Aspect-fit the canvas onto the swapchain so strokes / safety
        // markers land on the visible canvas, not the bg-fill bars.
        float fitW = dstWf, fitH = dstHf;
        if (canvasW > 0 && canvasH > 0) {
            const float canvasAspect =
                static_cast<float>(canvasW) / static_cast<float>(canvasH);
            const float dstAspect = dstWf / dstHf;
            if (canvasAspect > dstAspect) {
                fitW = dstWf;
                fitH = dstWf / canvasAspect;
            } else {
                fitH = dstHf;
                fitW = dstHf * canvasAspect;
            }
        }
        const float fitX = (dstWf - fitW) * 0.5f;
        const float fitY = (dstHf - fitH) * 0.5f;
        const float lineScale = (canvasW > 0)
            ? (fitW / static_cast<float>(canvasW)) : 1.0f;

        if (m_annotator) {
            m_annotator->setViewportRect(QPointF(fitX, fitY),
                                          QSizeF(fitW, fitH));
            ActiveStroke activeCopy;
            if (m_annotator->snapshotActiveStroke(activeCopy)) {
                TessellatedMesh mesh = StrokeTessellator::tessellate(
                    activeCopy, fitX, fitY, fitW, fitH, lineScale);
                if (!mesh.vertices.empty()) {
                    m_impl->annotations.drawMesh(ctx, mesh, W, H);
                }
            }
        }
        {
            std::vector<ActiveStroke> snapshot;
            {
                std::lock_guard lock(m_storedStrokesMutex);
                snapshot = m_storedStrokes;
            }
            for (const ActiveStroke &s : snapshot) {
                TessellatedMesh mesh = StrokeTessellator::tessellate(
                    s, fitX, fitY, fitW, fitH, lineScale);
                if (!mesh.vertices.empty()) {
                    m_impl->annotations.drawMesh(ctx, mesh, W, H);
                }
            }
        }

        // H.9 (2026-05-14) — per-side safety overlay, mirroring macOS
        // Metal at metal_player_renderer.mm:871-931. Each side gets a
        // mesh aspect-fit to ITS source aspect ratio inside its half
        // of the canvas (SBS) or the full canvas (Wipe → A only, to
        // match macOS v1; per-side wipe-clipped safety is a follow-up).
        // Without this, a mixed-aspect dual (16:9 + 1.85, etc.) shows
        // a single canvas-fit overlay that lies about safe-action /
        // safe-title for at least one side.
        if (m_safety && m_safety->isLoaded()) {
            auto *src = m_dualFrameSource.load(std::memory_order_acquire);
            const int compMode = m_compMode.load(std::memory_order_acquire);

            auto fitInto = [](float x, float y, float w, float h,
                                float sw, float sh) -> QRectF {
                if (sw <= 0 || sh <= 0 || w <= 0 || h <= 0) {
                    return QRectF(x, y, w, h);
                }
                const float containerAR = w / h;
                const float sourceAR    = sw / sh;
                float fw, fh;
                if (sourceAR > containerAR) {
                    fw = w; fh = w / sourceAR;
                } else {
                    fh = h; fw = h * sourceAR;
                }
                return QRectF(x + (w - fw) * 0.5f,
                              y + (h - fh) * 0.5f, fw, fh);
            };
            auto drawSide = [&](const QRectF &r) {
                if (r.width() < 2.0 || r.height() < 2.0) return;
                const TessellatedMesh m =
                    m_safety->mesh(r.topLeft(), r.size());
                if (!m.vertices.empty()) {
                    m_impl->annotations.drawMesh(ctx, m, W, H);
                }
            };

            const float sAW = src ? static_cast<float>(src->sourceWidth('A'))  : 0.0f;
            const float sAH = src ? static_cast<float>(src->sourceHeight('A')) : 0.0f;
            const float sBW = src ? static_cast<float>(src->sourceWidth('B'))  : 0.0f;
            const float sBH = src ? static_cast<float>(src->sourceHeight('B')) : 0.0f;

            // The canvas (where the compositor draws) is the fitW x fitH
            // rect inside the swapchain RTV; safety must fit inside THAT,
            // not the full RTV — the bg-fill bars outside fitW x fitH
            // are not part of the image area.
            if (compMode == static_cast<int>(CompositorMode::SideBySide)) {
                const float halfW = fitW * 0.5f;
                if (sAW > 0.0f)
                    drawSide(fitInto(fitX, fitY, halfW, fitH, sAW, sAH));
                if (sBW > 0.0f)
                    drawSide(fitInto(fitX + halfW, fitY, halfW, fitH, sBW, sBH));
            } else {
                // Wipe / Single-in-dual: A only at full canvas (matches
                // macOS v1 behavior at metal_player_renderer.mm:928-930).
                if (sAW > 0.0f)
                    drawSide(fitInto(fitX, fitY, fitW, fitH, sAW, sAH));
            }
        }
    }

    if (m_impl->screenshotPending.load(std::memory_order_acquire)) {
        serviceScreenshotRequest();
    }

    // Loading spinner overlay (dual path) — same as single flow.
    drawLoadingSpinner(ctx, rtv);

    swapchain->Present(1 /*sync to vsync*/, 0);
}

void D3D11PlayerRenderer::serviceScreenshotRequest()
{
    auto deliver = [this](QImage img) {
        {
            std::lock_guard lock(m_impl->screenshotMutex);
            m_impl->screenshotResult = std::move(img);
        }
        m_impl->screenshotPending.store(false, std::memory_order_release);
        m_impl->screenshotCv.notify_all();
    };

    // Mirrors MetalPlayerRenderer's single-flow capture path
    // (metal_player_renderer.mm:1585-1774). Three render passes into
    // a source-resolution RGBA8 texture, then GPU→CPU readback.
    // Output PNG is at native source dims (not viewport / swapchain
    // dims) so NotesPanel + exports match the on-disk frame.

    if (!m_impl->videoA.srv) { deliver(QImage()); return; }
    const int srcW = m_impl->videoA.width;
    const int srcH = m_impl->videoA.height;
    if (srcW <= 0 || srcH <= 0) { deliver(QImage()); return; }

    auto *device = static_cast<ID3D11Device *>(
        D3D11DeviceManager::instance().device());
    auto *ctx    = static_cast<ID3D11DeviceContext *>(
        D3D11DeviceManager::instance().context());
    if (!device || !ctx) { deliver(QImage()); return; }

    // Lazy / resize the capture textures + staging when source dims
    // change (rare during playback — only on media swap).
    if (!m_impl->captureSourceRgba16f ||
        m_impl->captureSrcW != srcW || m_impl->captureSrcH != srcH) {
        m_impl->captureSourceRgba16fSrv.Reset();
        m_impl->captureSourceRgba16fRtv.Reset();
        m_impl->captureSourceRgba16f.Reset();
        m_impl->captureDstRgba8Rtv.Reset();
        m_impl->captureDstRgba8.Reset();
        m_impl->captureStaging.Reset();

        D3D11_TEXTURE2D_DESC td16{};
        td16.Width     = static_cast<UINT>(srcW);
        td16.Height    = static_cast<UINT>(srcH);
        td16.MipLevels = 1;
        td16.ArraySize = 1;
        td16.Format    = DXGI_FORMAT_R16G16B16A16_FLOAT;
        td16.SampleDesc.Count = 1;
        td16.Usage     = D3D11_USAGE_DEFAULT;
        td16.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(device->CreateTexture2D(&td16, nullptr,
                                             m_impl->captureSourceRgba16f.GetAddressOf())) ||
            FAILED(device->CreateRenderTargetView(
                       m_impl->captureSourceRgba16f.Get(), nullptr,
                       m_impl->captureSourceRgba16fRtv.GetAddressOf())) ||
            FAILED(device->CreateShaderResourceView(
                       m_impl->captureSourceRgba16f.Get(), nullptr,
                       m_impl->captureSourceRgba16fSrv.GetAddressOf()))) {
            qWarning("D3D11PlayerRenderer: capture RGBA16F %dx%d alloc failed", srcW, srcH);
            deliver(QImage());
            return;
        }

        D3D11_TEXTURE2D_DESC td8{};
        td8.Width     = static_cast<UINT>(srcW);
        td8.Height    = static_cast<UINT>(srcH);
        td8.MipLevels = 1;
        td8.ArraySize = 1;
        td8.Format    = DXGI_FORMAT_R8G8B8A8_UNORM;
        td8.SampleDesc.Count = 1;
        td8.Usage     = D3D11_USAGE_DEFAULT;
        td8.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (FAILED(device->CreateTexture2D(&td8, nullptr,
                                             m_impl->captureDstRgba8.GetAddressOf())) ||
            FAILED(device->CreateRenderTargetView(
                       m_impl->captureDstRgba8.Get(), nullptr,
                       m_impl->captureDstRgba8Rtv.GetAddressOf()))) {
            qWarning("D3D11PlayerRenderer: capture RGBA8 %dx%d alloc failed", srcW, srcH);
            deliver(QImage());
            return;
        }

        D3D11_TEXTURE2D_DESC tds = td8;
        tds.Usage          = D3D11_USAGE_STAGING;
        tds.BindFlags      = 0;
        tds.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(device->CreateTexture2D(&tds, nullptr,
                                             m_impl->captureStaging.GetAddressOf()))) {
            qWarning("D3D11PlayerRenderer: capture staging %dx%d alloc failed", srcW, srcH);
            deliver(QImage());
            return;
        }
        m_impl->captureSrcW = srcW;
        m_impl->captureSrcH = srcH;
    }

    // ---- Pass 1: compositor renders source 1:1 into RGBA16F ----
    // Black background, no aspect-fit (src dims == dst dims so the
    // compositor's letterbox math collapses to full fill).
    {
        ID3D11RenderTargetView *rtv = m_impl->captureSourceRgba16fRtv.Get();
        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        D3D11_VIEWPORT vp{};
        vp.Width    = static_cast<float>(srcW);
        vp.Height   = static_cast<float>(srcH);
        vp.MinDepth = 0; vp.MaxDepth = 1;
        ctx->RSSetViewports(1, &vp);
        m_impl->compositor.renderSingle(
            ctx, m_impl->videoA.srv.Get(),
            srcW, srcH, srcW, srcH,
            /*bgMode=*/0 /*Black — screenshots never need checker*/);
    }

    // ---- Pass 2: OCIO if engaged, else compositor passthrough ----
    // Both paths land in captureDstRgba8 (RGBA8 RTV).
    {
        const bool useOcio = (m_ocio && m_ocio->engaged() &&
                              m_impl->ocio.isInitialized() &&
                              m_impl->ocio.rebuild(m_ocio));
        if (useOcio) {
            m_impl->ocio.apply(
                ctx,
                m_impl->captureSourceRgba16fSrv.Get(),
                m_impl->captureDstRgba8Rtv.Get(),
                srcW, srcH);
        } else {
            ID3D11RenderTargetView *rtv = m_impl->captureDstRgba8Rtv.Get();
            ctx->OMSetRenderTargets(1, &rtv, nullptr);
            D3D11_VIEWPORT vp{};
            vp.Width    = static_cast<float>(srcW);
            vp.Height   = static_cast<float>(srcH);
            vp.MinDepth = 0; vp.MaxDepth = 1;
            ctx->RSSetViewports(1, &vp);
            // Treat the RGBA16F intermediate as the new source. Same
            // 1:1 / full-fill geometry.
            m_impl->compositor.renderSingle(
                ctx, m_impl->captureSourceRgba16fSrv.Get(),
                srcW, srcH, srcW, srcH, 0);
        }
    }

    // ---- Pass 3: bake annotations into captureDstRgba8 ----
    // No safety overlay in the screenshot — mirrors Metal's single-
    // flow path (safety stays on the live present pass only).
    if (m_impl->captureAnnotations.isInitialized()) {
        // Make sure the RTV is bound (OCIO's apply already did this
        // in the useOcio branch; redundant for the passthrough
        // branch but harmless).
        ID3D11RenderTargetView *rtv = m_impl->captureDstRgba8Rtv.Get();
        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        D3D11_VIEWPORT vp{};
        vp.Width    = static_cast<float>(srcW);
        vp.Height   = static_cast<float>(srcH);
        vp.MinDepth = 0; vp.MaxDepth = 1;
        ctx->RSSetViewports(1, &vp);

        m_impl->captureAnnotations.beginFrame();

        // Active stroke at full source bounds (no letterbox).
        if (m_annotator) {
            ActiveStroke activeCopy;
            if (m_annotator->snapshotActiveStroke(activeCopy)) {
                TessellatedMesh mesh = StrokeTessellator::tessellate(
                    activeCopy, 0.0f, 0.0f,
                    static_cast<float>(srcW),
                    static_cast<float>(srcH));
                if (!mesh.vertices.empty()) {
                    m_impl->captureAnnotations.drawMesh(ctx, mesh, srcW, srcH);
                }
            }
        }
        // Stored strokes at full source bounds.
        std::vector<ActiveStroke> snapshot;
        {
            std::lock_guard lock(m_storedStrokesMutex);
            snapshot = m_storedStrokes;
        }
        for (const ActiveStroke &s : snapshot) {
            TessellatedMesh mesh = StrokeTessellator::tessellate(
                s, 0.0f, 0.0f,
                static_cast<float>(srcW), static_cast<float>(srcH));
            if (!mesh.vertices.empty()) {
                m_impl->captureAnnotations.drawMesh(ctx, mesh, srcW, srcH);
            }
        }
    }

    // ---- Readback: GPU → CPU → QImage ----
    ctx->CopyResource(m_impl->captureStaging.Get(),
                        m_impl->captureDstRgba8.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(ctx->Map(m_impl->captureStaging.Get(), 0,
                          D3D11_MAP_READ, 0, &mapped))) {
        qWarning("D3D11PlayerRenderer: capture staging Map failed");
        deliver(QImage());
        return;
    }
    QImage img(srcW, srcH, QImage::Format_RGBA8888);
    const std::uint8_t *src = static_cast<const std::uint8_t *>(mapped.pData);
    // RGBA8_UNORM is already in R,G,B,A order — no swizzle needed
    // (unlike the BGRA8 swapchain). One memcpy per row honors the
    // possibly-larger driver row pitch.
    const std::size_t copyBytes = static_cast<std::size_t>(srcW) * 4;
    for (int y = 0; y < srcH; ++y) {
        std::memcpy(img.scanLine(y), src, copyBytes);
        src += mapped.RowPitch;
    }
    ctx->Unmap(m_impl->captureStaging.Get(), 0);

    img.setColorSpace(QColorSpace(QColorSpace::SRgb));
    deliver(std::move(img));
}

// ---------------------------------------------------------------------
// IPlayerRenderer stub setters (real wiring in F.2.2+ phases)
// ---------------------------------------------------------------------
void D3D11PlayerRenderer::setHdrMode(HdrMode mode)
{
    m_hdrMode.store(static_cast<int>(mode));
    // Forward to the swapchain helper — applied on the render thread
    // at the next drawFrame iteration (which also re-inits the
    // annotation PSO if the format changes).
    m_impl->hdrSwapchain.setMode(mode);
    requestUpdate();
}

void D3D11PlayerRenderer::setBrightness(float brightness)
{
    // Phase F.2.9: brightness is applied in the compositor only, in
    // linear-light pre-OCIO space. Applying it again in the OCIO PS
    // (the pre-F.2.9 path) double-multiplied on every frame and was
    // mathematically broken for the PQ output path (PQ values can't
    // be linearly scaled). One multiplier, one place.
    m_impl->compositor.setBrightness(brightness);
    requestUpdate();
}

void D3D11PlayerRenderer::setImageSeqCache(ImageSequenceCache *c)
{
    // Mirrors MetalPlayerRenderer::setImageSeqCache. On pointer
    // change, wipe the upload thread's texture map + drop the
    // image-seq slot — otherwise returning to a previously loaded
    // sequence would briefly show a stale frame keyed by frame
    // number from the prior load.
    const bool pointerChanged = (c != m_impl->cacheBoundToUpload);
    if (pointerChanged && m_running.load()) {
        m_impl->upload.clearAll();
        if (m_impl->videoA.owner == Impl::SlotOwner::ImageSeq) {
            m_impl->videoA.srv.Reset();
            m_impl->videoA.texture.Reset();
            m_impl->videoA.width  = 0;
            m_impl->videoA.height = 0;
            m_impl->videoA.owner  = Impl::SlotOwner::None;
        }
    }

    // Unwire the OLD cache's callbacks first — without this, an
    // in-flight callback from the previous cache could fire into the
    // upload thread after we've remapped state to the new cache.
    if (m_cache && m_cache != c) {
        m_cache->setGpuPushCallback({});
        m_cache->setFrameEvictedCallback({});
    }

    m_cache = c;

    if (!c) {
        m_impl->cacheBoundToUpload = nullptr;
        m_impl->lastCacheGen       = 0;
        requestUpdate();
        return;
    }

    if (m_running.load()) {
        D3D11GpuUploadThread *upload = &m_impl->upload;
        ImageSequenceCache   *cache  = c;
        // The cache callback runs on the cache I/O thread. The
        // upload thread instance is owned by Impl, which lives for
        // the renderer's lifetime; setImageSeqCache(nullptr) +
        // shutdown() clear the callback before the upload thread is
        // stopped, so the lambda's `upload` pointer can't dangle.
        c->setGpuPushCallback(
            [upload, cache](int frame, std::shared_ptr<const PixelData> pixels) {
                upload->enqueue(frame, std::move(pixels),
                                cache->requestGeneration());
            });
        c->setFrameEvictedCallback(
            [upload](int frame) { upload->evictFrame(frame); });
        m_impl->cacheBoundToUpload = c;
        m_impl->lastCacheGen       = c->requestGeneration();
    }

    requestUpdate();
}
void D3D11PlayerRenderer::setVideoDecoder(VideoDecoder *d)       { m_decoder = d; requestUpdate(); }
void D3D11PlayerRenderer::setVideoDecoderB(VideoDecoder *d)      { m_decoderB = d; requestUpdate(); }
void D3D11PlayerRenderer::clearSourceAState()             {}
void D3D11PlayerRenderer::clearSourceBState()             {}
void D3D11PlayerRenderer::setSourceActivity(bool a, bool b) {
    m_aActive.store(a); m_bActive.store(b);
}
void D3D11PlayerRenderer::setOcio(OCIOConfigManager *o)   { m_ocio = o; requestUpdate(); }
void D3D11PlayerRenderer::setCompositorMode(CompositorMode m) {
    m_compMode.store(static_cast<int>(m)); requestUpdate();
}
void D3D11PlayerRenderer::setSplitPos(float p)            { m_splitPos.store(p); requestUpdate(); }
void D3D11PlayerRenderer::setSplitSeamHighlight(float h)  { m_splitSeamHighlight.store(h); requestUpdate(); }
void D3D11PlayerRenderer::setLoadingActive(bool on)       { m_loadingActive.store(on); requestUpdate(); }

namespace { constexpr double kLoadingSpinnerDelaySec = 0.18; }

void D3D11PlayerRenderer::drawLoadingSpinner(void *ctxVoid, void *rtvVoid)
{
    if (!m_loadingActive.load(std::memory_order_acquire)) {
        m_impl->spinnerWasActive = false;
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (!m_impl->spinnerWasActive) {
        m_impl->spinnerWasActive = true;
        m_impl->spinnerStart     = now;
    }
    const double elapsed =
        std::chrono::duration<double>(now - m_impl->spinnerStart).count();
    // Hold off briefly so a quick open doesn't flash the spinner.
    if (elapsed < kLoadingSpinnerDelaySec) return;
    const int W = m_impl->currentW;
    const int H = m_impl->currentH;
    if (W <= 0 || H <= 0) return;

    auto *ctx = static_cast<ID3D11DeviceContext *>(ctxVoid);
    auto *rtv = static_cast<ID3D11RenderTargetView *>(rtvVoid);
    ID3D11RenderTargetView *rtvs[1] = { rtv };
    ctx->OMSetRenderTargets(1, rtvs, nullptr);
    D3D11_VIEWPORT vp{};
    vp.Width    = static_cast<float>(W);
    vp.Height   = static_cast<float>(H);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    ctx->RSSetViewports(1, &vp);
    m_impl->compositor.renderSpinner(ctx, W, H, static_cast<float>(elapsed));
}
void D3D11PlayerRenderer::setBackgroundMode(BackgroundMode m) {
    m_bgMode.store(static_cast<int>(m)); requestUpdate();
}
void D3D11PlayerRenderer::setViewportAnnotator(ViewportAnnotator *a) { m_annotator = a; requestUpdate(); }
void D3D11PlayerRenderer::setSafetyOverlay(SafetyOverlay *s) { m_safety = s; requestUpdate(); }
void D3D11PlayerRenderer::setRendererMode(RendererMode m) {
    m_rendererMode.store(static_cast<int>(m)); requestUpdate();
}
void D3D11PlayerRenderer::setDualController(void *p)      { m_dualControllerPtr.store(p); }
void D3D11PlayerRenderer::setDualFrameSource(IDualFrameSource *s) {
    m_dualFrameSource.store(s, std::memory_order_release);
    requestUpdate();
}
void D3D11PlayerRenderer::setHoverThumbnail(unsigned long long h, int w, int hh) {
    m_hoverThumbHandle.store(h); m_hoverThumbW.store(w); m_hoverThumbH.store(hh);
    requestUpdate();
}
void D3D11PlayerRenderer::setHoverThumbnailB(unsigned long long h, int w, int hh) {
    m_hoverThumbHandleB.store(h); m_hoverThumbWB.store(w); m_hoverThumbHB.store(hh);
    requestUpdate();
}
void D3D11PlayerRenderer::setStoredAnnotationStrokes(
    const std::vector<ActiveStroke> &strokes)
{
    std::lock_guard lock(m_storedStrokesMutex);
    m_storedStrokes = strokes;
    requestUpdate();
}
QImage D3D11PlayerRenderer::captureScreenshot()
{
    // Caller (typically WindowManager on the GUI thread) blocks here
    // while the render thread captures the next frame's swapchain
    // back buffer. Mirrors MetalPlayerRenderer's 250ms render-fence
    // wait pattern — short enough to feel responsive but long enough
    // that a slow paint (resize, OCIO rebuild) doesn't truncate.
    if (!m_running.load(std::memory_order_acquire)) return QImage();
    if (!m_impl->hdrSwapchain.swapchain()) return QImage();

    {
        std::lock_guard lock(m_impl->screenshotMutex);
        m_impl->screenshotResult = QImage();
    }
    m_impl->screenshotPending.store(true, std::memory_order_release);
    requestUpdate();

    std::unique_lock lock(m_impl->screenshotMutex);
    const bool ok = m_impl->screenshotCv.wait_for(
        lock, std::chrono::milliseconds(250),
        [this] { return !m_impl->screenshotPending.load(std::memory_order_acquire); });
    if (!ok) {
        // Timeout — clear the flag so a future request doesn't pick
        // up the stale signal.
        m_impl->screenshotPending.store(false, std::memory_order_release);
        return QImage();
    }
    return std::move(m_impl->screenshotResult);
}

void D3D11PlayerRenderer::setParentHwnd(void *hwnd)
{
    m_impl->parentHwnd = static_cast<HWND>(hwnd);
}

} // namespace qcv
