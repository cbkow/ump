// D3D11HdrSwapchain — Phase F.2.8.
//
// Owns the DXGI swapchain + DComp visual lifecycle for the player
// surface, including HDR-mode-driven swapchain format + colorspace
// transitions. Mirrors MetalHdrSwapchain's role (which mutates the
// CAMetalLayer's pixelFormat / colorspace / wantsExtendedDynamicRange
// in place); on DXGI the swapchain's format is fixed at creation,
// so a format change means releasing the swapchain and creating a
// fresh one, then re-anchoring it to the DComp visual.
//
// Mode → format/colorspace mapping (mirrors macOS where possible —
// scRGB linear is the analog of macOS EDR-extended sRGB):
//   SdrSRgb            B8G8R8A8_UNORM     + G22_NONE_P709 (sRGB SDR)
//   SdrDisplayP3       B8G8R8A8_UNORM     + G22_NONE_P709 (no DXGI
//                      P3-SDR enum exists; falls back to sRGB)
//   ExtendedLinearSRgb R16G16B16A16_FLOAT + G10_NONE_P709 (scRGB)
//   ExtendedLinearDisplayP3 same as ExtendedLinearSRgb (Win has no
//                           P3-tagged scRGB; OS handles primaries
//                           per display profile)
//   Hdr10              Maps to ExtendedLinearSRgb with a warning —
//                      D3D11 path uses scRGB linear, not PQ.
//
// Threading: setMode() is GUI-thread (stores atomically);
// applyPendingOnRenderThread() runs at the top of drawFrame and
// performs the swapchain swap if the mode changed.

#pragma once

#include "render/iplayer_renderer.h"   // HdrMode

#include <memory>

struct IDXGIFactory2;
struct ID3D11Device;
struct IDCompositionDevice;
typedef void *HANDLE;
#ifndef WIN32
typedef struct HWND__ *HWND;
#endif

namespace qcv {

class D3D11HdrSwapchain {
public:
    D3D11HdrSwapchain();
    ~D3D11HdrSwapchain();

    D3D11HdrSwapchain(const D3D11HdrSwapchain &)            = delete;
    D3D11HdrSwapchain &operator=(const D3D11HdrSwapchain &) = delete;

    // Creates the initial swapchain (B8G8R8A8 / sRGB), DComp target
    // for childHwnd, and DComp visual containing the swapchain. The
    // caller owns the dcomp device + HWND lifetimes; we just borrow.
    bool initialize(IDXGIFactory2     *factory,
                    ID3D11Device      *device,
                    IDCompositionDevice *dcomp,
                    HWND               childHwnd,
                    int                width,
                    int                height);
    void shutdown();
    bool isInitialized() const;

    // GUI thread: record the requested mode. The render thread picks
    // it up on its next drawFrame iteration.
    void setMode(HdrMode mode);

    // Render thread: if the pending mode differs from the applied
    // mode AND requires a different DXGI format, recreate the
    // swapchain at the new format/colorspace, re-anchor the DComp
    // visual, rebuild the RTV. Returns true when the swapchain
    // format changed — caller should reinit any PSO that targets
    // the swapchain RTV (annotations is the main one; compositor +
    // OCIO write into our RTV with no format-bound PSO state).
    bool applyPendingOnRenderThread();

    // Render thread: standard ResizeBuffers + RTV rebuild for window
    // size changes. Format is unchanged. Returns true on success.
    bool resize(int width, int height);

    // Caller bookkeeping.
    HdrMode appliedMode() const;
    int     currentFormat() const;  // DXGI_FORMAT cast to int
    int     currentWidth() const;
    int     currentHeight() const;

    // Borrowed accessors — caller uses for one frame, doesn't hold
    // refs across applyPendingOnRenderThread (which may recreate).
    // Returned as void* so this header doesn't drag in <d3d11.h>.
    void *swapchain() const;         // IDXGISwapChain1 *
    void *rtv() const;               // ID3D11RenderTargetView *
    void *dcompVisual() const;       // IDCompositionVisual *

    // Capability queries (best-effort; DXGI reports per-output, we
    // query the swapchain's containing output).
    bool  isHdrSupported() const;
    float maxHdrHeadroom() const;     // nits ÷ 80 (SDR-diffuse-white)

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace qcv
