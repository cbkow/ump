// D3D11PlayerRenderer — Phase F.2.1.
//
// Concrete IPlayerRenderer for Windows. Mirrors MetalPlayerRenderer's
// structural shape (PIMPL + dedicated render thread + render-on-demand
// loop driven by requestUpdate / m_wakeCv). Compositor / OCIO /
// annotation / YUV-decode plumbing all land in F.2.2+ phases.
//
// F.2.1 scope (this file at landing):
//   - init(PlayerWindow*) creates a child HWND of the UI window's HWND,
//     a D3D11 swapchain via IDXGIFactory2::CreateSwapChainForComposition
//     (flip-discard, BGRA8), and an IDCompositionTarget+Visual composing
//     that swapchain on the child HWND. Spawns the render thread.
//   - renderThreadProc clears the swapchain to a fixed debug color
//     (dark teal) per frame and presents. Verifies end-to-end plumbing
//     without any content pipeline.
//   - setViewportRect(x, y, w, h) updates the child HWND geometry +
//     queues a swapchain resize. Called from WindowManager's centerStage
//     geometry tracking (mirrors the macOS path's syncPlayerGeometry).
//   - All other IPlayerRenderer setters store-only; real implementations
//     follow in F.2.2+ (texture pool / YUV / compositor / OCIO / ann).

#pragma once

#include "render/iplayer_renderer.h"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace qcv {

class D3D11PlayerRenderer final : public IPlayerRenderer {
public:
    D3D11PlayerRenderer();
    ~D3D11PlayerRenderer() override;

    D3D11PlayerRenderer(const D3D11PlayerRenderer &)            = delete;
    D3D11PlayerRenderer &operator=(const D3D11PlayerRenderer &) = delete;

    // ---- IPlayerRenderer ----
    bool init(PlayerWindow *window) override;
    void shutdown() override;
    void requestUpdate() override;
    void resize(int width, int height) override;

    void setHdrMode(HdrMode mode) override;
    void setBrightness(float brightness) override;
    void setImageSeqCache(ImageSequenceCache *c) override;
    void setVideoDecoder(VideoDecoder *d) override;
    void setVideoDecoderB(VideoDecoder *d) override;
    void clearSourceAState() override;
    void clearSourceBState() override;
    void setSourceActivity(bool aActive, bool bActive) override;
    void setOcio(OCIOConfigManager *o) override;
    void setCompositorMode(CompositorMode mode) override;
    void setSplitPos(float pos) override;
    void setSplitSeamHighlight(float h) override;
    void setBackgroundMode(BackgroundMode mode) override;
    void setViewportAnnotator(ViewportAnnotator *a) override;
    void setSafetyOverlay(SafetyOverlay *s) override;
    void setRendererMode(RendererMode m) override;
    void setDualController(void *controllerPtr) override;
    void setDualFrameSource(IDualFrameSource *source) override;
    void setHoverThumbnail(unsigned long long handle,
                            int srcW, int srcH) override;
    void setHoverThumbnailB(unsigned long long handle,
                             int srcW, int srcH) override;
    void setStoredAnnotationStrokes(
        const std::vector<ActiveStroke> &strokes) override;
    QImage captureScreenshot() override;

    // ---- Windows-specific (called by WindowManager) ----
    // The renderer's child HWND tracks the centerStage rect inside the
    // UI window. WindowManager calls this from its centerStage geometry
    // signals (mirrors syncPlayerGeometry on macOS, which uses Qt's
    // QWindow::setGeometry on the player QWindow).
    void setViewportRect(int x, int y, int width, int height);

    // Tell the renderer which HWND to parent its DComp child window
    // under. MUST be called BEFORE init() — the renderer can't find
    // the UI window via Qt's QObject::parent() chain because
    // WindowManager owns PlayerWindow's QObject lifetime, not the UI
    // QWindow. Pass `m_uiWindow->winId()` cast to void*.
    void setParentHwnd(void *hwnd);

private:
    void renderThreadProc();
    void drawFrame();
    // Phase F.2.11.d — DualFlow branch dispatched from drawFrame()
    // when m_rendererMode == DualFlow. Mirrors MetalPlayerRenderer's
    // dual-flow drawFrame block (metal_player_renderer.mm:695+):
    // composite both sides into a capped RGBA16F canvas, optional
    // OCIO pass, present-blit canvas → swapchain.
    void drawDualFrame();
    void applyPendingResizeIfNeeded();
    // Pull latest FrameHandle from m_decoder and upload its pixels
    // to the video-A slot. Returns true if a new frame was uploaded
    // (caller forces a redraw). F.2.3 CPU path only — F.2.4 will
    // add Vulkan-decode FrameHandle::Vulkan import via NT shared
    // handle.
    bool consumeLatestVideoFrame();
    // Phase F.2.7.1 — render-thread worker that runs inside drawFrame
    // when the GUI thread set Impl::screenshotPending. Blits the
    // current back buffer to a staging texture, copies bytes to
    // QImage RGBA8888 (BGRA→RGBA swizzle), stores into
    // Impl::screenshotResult, signals the cv.
    void serviceScreenshotRequest();

    struct Impl;
    std::unique_ptr<Impl>      m_impl;
    std::thread                m_renderThread;
    std::atomic<bool>          m_stopRequested{false};
    std::atomic<bool>          m_running{false};

    // Render-on-demand wake primitive — mirrors the Vulkan render-loop
    // pattern from the Phase E archive (F.0 doc 07). Set by requestUpdate
    // + setters; consumed by the render-thread cv wait.
    std::atomic<bool>          m_pendingDraw{false};
    std::mutex                 m_wakeMutex;
    std::condition_variable    m_wakeCv;

    // Pending resize, posted by resize() / setViewportRect(); consumed
    // by the render thread under m_resizeMutex on its next iteration.
    std::mutex                 m_resizeMutex;
    int                        m_pendingResizeW = 0;
    int                        m_pendingResizeH = 0;
    bool                       m_resizePending  = false;

    // Pending viewport rect (child HWND position). Same protection.
    int                        m_pendingViewportX = 0;
    int                        m_pendingViewportY = 0;
    bool                       m_viewportPending  = false;

    // ---- Config (stub stores for F.2.1; real consumers in F.2.2+) ----
    std::atomic<int>           m_hdrMode{static_cast<int>(HdrMode::SdrSRgb)};
    ImageSequenceCache        *m_cache     = nullptr;
    VideoDecoder              *m_decoder   = nullptr;
    VideoDecoder              *m_decoderB  = nullptr;
    OCIOConfigManager         *m_ocio      = nullptr;
    ViewportAnnotator         *m_annotator = nullptr;
    SafetyOverlay             *m_safety    = nullptr;
    std::atomic<int>           m_compMode  {static_cast<int>(CompositorMode::Single)};
    std::atomic<float>         m_splitPos  {0.5f};
    std::atomic<float>         m_splitSeamHighlight {0.0f};
    std::atomic<int>           m_bgMode    {static_cast<int>(BackgroundMode::Black)};
    std::atomic<bool>          m_aActive   {true};
    std::atomic<bool>          m_bActive   {true};
    std::atomic<int>           m_rendererMode{static_cast<int>(RendererMode::SingleFlow)};
    std::atomic<void *>        m_dualControllerPtr{nullptr};
    // Phase F.2.11.d — typed pull seam for the D3D11 dual flow.
    // Stored independently of m_dualControllerPtr because qcv_render
    // doesn't link qcv_dual on non-Apple; the qcv_window adapter
    // implements IDualFrameSource against DualPlaybackController.
    std::atomic<IDualFrameSource *> m_dualFrameSource{nullptr};
    std::atomic<unsigned long long> m_hoverThumbHandle{0};
    std::atomic<int>                m_hoverThumbW{0};
    std::atomic<int>                m_hoverThumbH{0};
    std::atomic<unsigned long long> m_hoverThumbHandleB{0};
    std::atomic<int>                m_hoverThumbWB{0};
    std::atomic<int>                m_hoverThumbHB{0};
    std::mutex                 m_storedStrokesMutex;
    std::vector<ActiveStroke>  m_storedStrokes;
};

} // namespace qcv
