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
    // `borderPx`        — edge-frame width in px; 0 (default) = no
    //                      frame. Used by renderCornerOverlay so the
    //                      thumbnail doesn't blend into the viewport.
    // `borderR/G/B`     — frame color (only when borderPx > 0).
    // `overlayBlend` — when true, the source is blended STRAIGHT over the
    //                  existing RTV contents (non-premult src-over) and
    //                  fragments outside the source rect are transparent,
    //                  instead of compositing over an opaque bg fill. Used
    //                  for the centered viewport-notice card so its rounded
    //                  corners reveal the viewport background (parity with
    //                  the macOS present compositor). Default false.
    // `rotQuarters` — display rotation in quarter-turns CW {0..3}. The
    //                  caller passes srcW/H already SWAPPED for odd
    //                  quarters (display-orientation dims drive the fit);
    //                  the shader inverse-rotates its sampling so the
    //                  stored texture reads upright.
    void renderSingle(void *ctx,
                       void *srcSrv,
                       int   dstW, int dstH,
                       int   srcW, int srcH,
                       int   bgMode,
                       float borderPx = 0.0f,
                       float borderR = 0.0f,
                       float borderG = 0.0f,
                       float borderB = 0.0f,
                       bool  overlayBlend = false,
                       int   rotQuarters = 0);

    // Phase F.2.8 follow-up — uniform output multiplier applied to
    // the final pixel before write. 1.0 = identity. Stored as state;
    // next renderSingle picks it up.
    void setBrightness(float brightness);

    // Hover-thumb corner overlay. Mirrors MetalCompositor's
    // renderCornerOverlay (metal_compositor.mm:522). Reuses the
    // renderSingle pipeline by setting a tiny viewport at the chosen
    // corner and treating that rect as a fake canvas — the existing
    // aspect-fit shader letter-/pillarboxes the thumb inside the box
    // (bgMode 0 = black fill) and draws a faint edge frame. Box height
    // is capped at the box width, so a tall portrait thumb is never
    // taller than wide. Caller's viewport is saved + restored so
    // downstream draws (annotation, screenshot, present) aren't
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
    //   rotQuarters  — display rotation for the thumb source, quarter-
    //                  turns CW; srcW/H arrive display-swapped like
    //                  renderSingle's.
    void renderCornerOverlay(void *ctx,
                              void *srcSrv,
                              int   srcW, int srcH,
                              int   dstW, int dstH,
                              int   corner,
                              float overlayFrac,
                              float marginPx,
                              int   rotQuarters = 0);

    // Loading spinner — full-viewport dark fill + rotating accent dots.
    // No source texture. `timeSeconds` drives the animation (render
    // thread supplies elapsed-since-load-start). Caller binds the RTV +
    // viewport first.
    void renderSpinner(void *ctx, int dstW, int dstH, float timeSeconds);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace qcv
