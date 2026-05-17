// D3D11Compositor — Phase F.2.3.
//
// Single-pass D3D11 pixel-shader that draws a video / image source into
// a destination RTV with aspect-preserving fit (letterbox / pillarbox).
// Background fill where the source rect doesn't cover the destination.
//
// F.2.3 scope: Single source, RGBA8 input, Black background only.
// SideBySide / Wipe modes, the other background fills (DarkGray, light
// + dark checkerboards), and per-side activity flags land in F.2.3+
// follow-ons before the OCIO pass (F.2.5).
//
// Mirrors MetalCompositor's public API loosely — the C++ side is
// platform-specific (D3D11 PSO state objects + HLSL shaders embedded
// as strings), but the entry point shape (renderSingle with src view +
// dest extent + source extent) is the same so the renderer's per-frame
// invocation matches macOS.

#pragma once

#include <memory>

namespace qcv {

class D3D11Compositor {
public:
    D3D11Compositor();
    ~D3D11Compositor();

    D3D11Compositor(const D3D11Compositor &)            = delete;
    D3D11Compositor &operator=(const D3D11Compositor &) = delete;

    // Compile shaders + build pipeline state. Requires
    // D3D11DeviceManager to be initialized.
    bool initialize();
    void shutdown();
    bool isInitialized() const;

    // Draw one fullscreen-triangle pass into the bound RTV.
    // `ctx`     — immediate context (caller has already bound RTV +
    //              set viewport).
    // `srcSrv`  — shader resource view of the source texture; may be
    //              nullptr (renders background only).
    // `dstW/H`  — destination viewport extent in pixels.
    // `srcW/H`  — source content size (for aspect-fit math); ignored
    //              when srcSrv is null.
    // `bgMode`  — qcv::BackgroundMode enum:
    //              0 = Black, 1 = DarkGray,
    //              2 = DarkCheckerboard, 3 = LightCheckerboard.
    void renderSingle(void *ctx,
                       void *srcSrv,
                       int   dstW, int dstH,
                       int   srcW, int srcH,
                       int   bgMode);

    // Phase F.2.8 follow-up — uniform output multiplier applied to
    // the final pixel before write. 1.0 = identity. Stored as state;
    // next renderSingle picks it up.
    void setBrightness(float brightness);

    // Hover-thumb corner overlay. Mirrors MetalCompositor's
    // renderCornerOverlay (metal_compositor.mm:522). Reuses the
    // renderSingle pipeline by setting a tiny viewport at the chosen
    // corner and treating that rect as a fake canvas — the existing
    // aspect-fit shader letter-/pillarboxes the thumb inside the box
    // (bgMode 0 = black fill). Caller's viewport is saved + restored
    // so downstream draws (annotation, screenshot, present) aren't
    // affected.
    //
    //   ctx          — immediate context, RTV already bound by caller.
    //   srcSrv       — thumbnail SRV (from
    //                  D3D11TexturePool::thumbnailInstance().texture()).
    //   srcW/H       — thumbnail source dimensions for aspect ratio.
    //   dstW/H       — caller's full draw extent in pixels (the canvas
    //                  the corner anchors INTO; in dual flow pass the
    //                  canvas-fit rect, NOT the raw swapchain, so
    //                  overlays don't land in letterbox bars).
    //   corner       — 0 = bottom-left (side A), 1 = bottom-right (B).
    //   overlayFrac  — fraction of dstW the box width occupies;
    //                  clamped to [0.05, 0.5].
    //   marginPx     — inset from the canvas edges.
    void renderCornerOverlay(void *ctx,
                              void *srcSrv,
                              int   srcW, int srcH,
                              int   dstW, int dstH,
                              int   corner,
                              float overlayFrac,
                              float marginPx);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace qcv
