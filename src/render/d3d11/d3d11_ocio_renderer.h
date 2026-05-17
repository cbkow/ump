// D3D11OcioRenderer — Phase F.2.5.
//
// Applies the active OCIO color transform via a D3D11 pixel-shader
// pass. Direct port of MetalOcioRenderer using HLSL_SM_5_0 emit from
// OCIO instead of MSL.
//
// Architecture: OcioChainBuilder asks OCIO for an HLSL shader function
// (OCIO 2.x supports HLSL natively). We wrap it in a thin PS that
// samples a source texture and calls OCIODisplay(src). LUTs are
// declared at OCIO's own register slots; we bind their SRVs+samplers
// in OCIO's declaration order (3D LUTs first, then 1D/2D — see the
// F.1.a probe spike for the rationale).
//
// Threading: rebuild() runs on the render thread when the OCIO chain
// generation bumps; apply() runs on the render thread per frame.

#pragma once

#include <QString>

#include <functional>
#include <memory>

namespace qcv {

class OCIOConfigManager;

class D3D11OcioRenderer {
public:
    D3D11OcioRenderer();
    ~D3D11OcioRenderer();

    D3D11OcioRenderer(const D3D11OcioRenderer &)            = delete;
    D3D11OcioRenderer &operator=(const D3D11OcioRenderer &) = delete;

    bool initialize();
    void shutdown();
    bool isInitialized() const;

    // Build / rebuild the pixel shader + LUT textures from the active
    // OCIO chain. Idempotent if the chain generation hasn't changed.
    // Returns true on success.
    bool rebuild(OCIOConfigManager *ocio);

    // Apply the current pipeline. The caller has prepared:
    //   - `srcSrv`  — RGBA16F SRV of the (compositor-composited) input
    //   - `dstRtv`  — RTV of the destination (the swapchain back buffer)
    //   - `dstW/H`  — destination extent in pixels (sets viewport)
    // Bind state inside apply(): t0/s0 = src; t1+/s1+ = LUTs; full-
    // screen-triangle Draw(3,0). No OMSetRenderTargets is unbound on
    // return — caller is expected to set the next state explicitly.
    void apply(void *ctx,
               void *srcSrv,
               void *dstRtv,
               int   dstW, int dstH);

    bool          hasPipeline() const;
    const QString &lastError()  const;

    // Optional callback fired (from the worker thread) when an async
    // rebuild stages a new pipeline. The player renderer hooks this
    // to requestUpdate() so a paused-playback view-switch picks up
    // the freshly-compiled OCIO chain instead of waiting indefinitely
    // for the next decoded frame to nudge the render loop.
    void setWakeCallback(std::function<void()> cb);

private:
    // Heavy rebuild work: OcioChainBuilder::build + D3DCompile + LUT
    // texture creation + reflection. Runs on a worker thread spawned
    // from rebuild(); stages the result under Impl::swapMutex and the
    // render thread picks it up on the next rebuild() call. The
    // function only uses thread-safe D3D11 device methods (no
    // immediate-context calls) so it's safe off the render thread.
    void doRebuildWork(int gen, OCIOConfigManager *ocio);

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace qcv
