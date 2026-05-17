// MetalAnnotationRenderer — Phase 7.5 B.6.5 / Guide 19 §2.3.
//
// Draws annotation strokes (freehand / shape primitives) over the
// composited frame. Consumes a TessellatedMesh from StrokeTessellator
// (already lifted in A.2.2) and renders the triangles with alpha
// blending into the active render pass.
//
// Adapted from old QCView's
// src/annotations/metal_annotation_renderer.{h,mm} (~542 LoC). Phase
// B.6.5 scope is the on-screen rendering path only; the export
// pipelines (RGBA8 + un-premultiply for PNG note thumbnails) ship
// in a Phase B.6.5.b follow-up.
//
// Threading: created + used on the render thread.

#pragma once

namespace qcv {

struct TessellatedMesh;

class MetalAnnotationRenderer {
public:
    MetalAnnotationRenderer();
    ~MetalAnnotationRenderer();

    MetalAnnotationRenderer(const MetalAnnotationRenderer &)            = delete;
    MetalAnnotationRenderer &operator=(const MetalAnnotationRenderer &) = delete;

    // Pipeline state is created for the drawable's pixel format
    // (passed as an int — MTLPixelFormat). Re-init when format
    // changes (HDR mode flip).
    bool initialize(int targetPixelFormat);
    void shutdown();
    bool isInitialized() const;
    int  targetPixelFormat() const;

    // Encode triangle draw calls for `mesh` into an active
    // id<MTLRenderCommandEncoder> (passed as void*). targetWidth /
    // targetHeight are the drawable's size in pixels — used to
    // build the pixel-coords → NDC transform.
    //
    // The mesh's vertices are in pixel coordinates produced by
    // StrokeTessellator with the same display size. No per-frame
    // CPU work required beyond the vertex-buffer upload.
    void drawMesh(void *encoder,
                  const TessellatedMesh &mesh,
                  int targetWidth, int targetHeight);

    // Reset the per-frame vertex buffer append cursor. Must be
    // called at the start of each frame's render encoding (before
    // any drawMesh) so multiple drawMesh calls in the same encoder
    // append to distinct buffer offsets instead of overwriting one
    // another. Without this, a second drawMesh in the same encoder
    // clobbers the first's vertices and both encoded draws end up
    // sampling the second mesh — visible as "only the last side's
    // overlay renders" in dual SBS / multi-stroke scenes.
    void beginFrame();

private:
    struct Impl;
    Impl *m_impl = nullptr;
};

} // namespace qcv
