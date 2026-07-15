// D3D11DualCompositor — Phase F.2.11.c.
//
// HLSL port of qcv::dual::DualCompositor (Metal). Single render pass
// that composites two sources into one RGBA16F canvas with Single /
// SBS / Wipe modes, per-side aspect-fit, and per-side active flags
// (mismatched-length clips render transparent past their end).
//
// Mirrors the Metal compositor's two-step contract:
//   1. prepareFrames(ctx) — pull master frame, drop cache on gen bump /
//      seek jump / past-end, UpdateSubresource per-side cached textures
//      from the IDualFrameSource adapter.
//   2. renderFrame(ctx, rtv, w, h) — bind pipeline, sample both slots,
//      draw fullscreen triangle.
//
// Sample input always comes via IDualFrameSource (qcv_render-side
// pure-virtual seam). The renderer hands one over via setFrameSource;
// the qcv_window adapter does the qcv_dual translation.
//
// CPU-only frame uploads for F.2.11.c.1 (matches the F.2.11.b.1
// payload). Vulkan-shared kind lands later when DualVideoDecoder
// produces them.

#pragma once

#include <memory>

namespace qcv {

class IDualFrameSource;
class D3D11VulkanYuvCompositor;

enum class CompositorMode;   // forward decl matches iplayer_renderer.h enum

class D3D11DualCompositor {
public:
    D3D11DualCompositor();
    ~D3D11DualCompositor();

    D3D11DualCompositor(const D3D11DualCompositor &)            = delete;
    D3D11DualCompositor &operator=(const D3D11DualCompositor &) = delete;

    // Compile shaders, build pipeline state. Requires D3D11DeviceManager.
    bool initialize();
    void shutdown();
    bool isInitialized() const;

    // Inject the renderer-owned shared YUV compositor. Per-slot
    // Vulkan-decode bridges initialize against this on first use
    // (Phase F.2.13 split — both slots share one compute pipeline,
    // mirroring macOS's MetalYuvRenderer pattern). Non-owning; the
    // pointer must outlive the bridges this compositor will lazily
    // initialize. Pass nullptr to detach (the bridges' Vulkan path
    // is then disabled).
    void setExternalYuvCompositor(D3D11VulkanYuvCompositor *yuv);

    // Set the frame source pulled at each prepareFrames call. Pass
    // nullptr to detach. Drops per-side cached textures on swap so
    // sizing from a prior session doesn't bleed through.
    void setFrameSource(IDualFrameSource *source);

    // Compositor mode + wipe split position. setMode is hot — no
    // pipeline-state rebuild needed. setSplitPos is ignored unless
    // mode == Wipe.
    void setMode(CompositorMode mode);
    void setSplitPos(float pos);
    // Wipe split-seam highlight: 0 = faint grey (rest), 1 = white
    // (hover/drag of the seam handle). Ignored unless mode == Wipe.
    void setSeamHighlight(float h);
    // Difference-mode amplification (1.0 = raw abs(A-B)). Ignored unless
    // mode == Difference.
    void setDiffGain(float g);
    // Per-side pixel aspect (anamorphic un-squeeze). Widens the
    // effective srcSize fed to the shader so each side's aspect-fit
    // lands on the corrected display aspect. 1/1 (default) = square.
    void setPixelAspectA(int num, int den);
    void setPixelAspectB(int num, int den);
    // Per-side display rotation in quarter-turns CW {0..3}. Applied
    // after the un-squeeze: odd quarters swap the effective srcSize
    // and the shader inverse-rotates its sampling.
    void setRotationA(int quarters);
    void setRotationB(int quarters);

    // Pull both sides' frames at the current master frame, drop
    // cache on seek/gen/past-end, UpdateSubresource into per-slot
    // cached textures. Must be called before renderFrame on the same
    // context. No-op if no frame source set yet.
    void prepareFrames(void *ctx);

    // Bind pipeline + SRVs + UBO, draw fullscreen triangle into the
    // already-bound RTV (caller has set viewport and cleared the
    // target). dstW/dstH set the destination size uniform (used for
    // SBS half-split + Wipe splitter line + sampleFit math).
    void renderFrame(void *ctx, int dstW, int dstH);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace qcv
