// D3D11AnnotationRenderer — Phase F.2.7.
//
// Direct port of MetalAnnotationRenderer to D3D11. Consumes a
// TessellatedMesh (CPU-tessellated triangle list, pixel-coords + per-
// vertex RGBA) and encodes a draw call into the active ID3D11 device
// context. Both active strokes and the safety overlay share the same
// pipeline; multiple drawMesh() calls per frame append into a single
// dynamic vertex buffer with WRITE_NO_OVERWRITE for batched uploads.
//
// HLSL shaders are inline (small) and target the swapchain's pixel
// format passed to initialize(). If the swapchain format changes
// (e.g., HDR mode flip in F.2.8) the renderer must be reinitialize()'d.
//
// Threading: created + used on the render thread.

#pragma once

#include <memory>

namespace qcv {

struct TessellatedMesh;

class D3D11AnnotationRenderer {
public:
    D3D11AnnotationRenderer();
    ~D3D11AnnotationRenderer();

    D3D11AnnotationRenderer(const D3D11AnnotationRenderer &)            = delete;
    D3D11AnnotationRenderer &operator=(const D3D11AnnotationRenderer &) = delete;

    // Build the PSO for `targetFormat` (a DXGI_FORMAT value passed as
    // int so headers don't drag in <d3d11.h>). Re-call after a
    // swapchain format change. Phase F.2.9: the pixel-shader variant
    // (SDR / scRGB-linear / HDR10 PQ) is picked from targetFormat.
    bool initialize(int targetFormat);
    void shutdown();
    bool isInitialized() const;
    int  targetPixelFormat() const;

    // Phase F.2.9 — stroke reference luminance (nits). Drives the
    // sRGB→linear scale in the scRGB-linear variant and the linear→PQ
    // normalization in the HDR10 PQ variant. SDR variant ignores it
    // (writes sRGB-encoded directly). Default 200 nits (Guide 06 D11).
    // Picked up by the next drawMesh().
    void setReferenceLuminance(float nits);

    // Encode a triangle-list draw of `mesh` into `ctx` (an
    // ID3D11DeviceContext*). targetWidth / targetHeight are the
    // viewport size in pixels — used to build the pixel-coords → NDC
    // transform (matches MetalAnnotationRenderer::drawMesh). The
    // caller has already bound the RTV the strokes draw onto.
    void drawMesh(void *ctx,
                  const TessellatedMesh &mesh,
                  int targetWidth, int targetHeight);

    // Reset the per-frame dynamic vertex buffer's append cursor.
    // MUST be called once per frame before any drawMesh — otherwise
    // multiple drawMesh calls overwrite each other (see the
    // MetalAnnotationRenderer::beginFrame() header note about the
    // SBS-mode bug). The next drawMesh() will Map with
    // WRITE_DISCARD; subsequent same-frame calls use
    // WRITE_NO_OVERWRITE.
    void beginFrame();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace qcv
