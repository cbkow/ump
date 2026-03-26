#pragma once

#ifdef __APPLE__

#include <vector>
#include <cstdint>

namespace qcview {
namespace Annotations {
    struct ActiveStroke;
}
}

namespace qcview {
namespace Annotations {

//=============================================================================
// MetalAnnotationRenderer
//
// Renders hand-drawn annotations via Metal.
// Uses tessellation approach (matching Vulkan path, not NanoVG/OpenGL).
//=============================================================================

class MetalAnnotationRenderer {
public:
    MetalAnnotationRenderer();
    ~MetalAnnotationRenderer();

    MetalAnnotationRenderer(const MetalAnnotationRenderer&) = delete;
    MetalAnnotationRenderer& operator=(const MetalAnnotationRenderer&) = delete;

    bool Initialize();
    void Shutdown();
    bool IsInitialized() const { return initialized_; }

    // Render annotations to an offscreen texture for viewport overlay
    // Returns pool texture ID of the rendered annotations
    uint64_t RenderAnnotations(
        const std::vector<ActiveStroke>& strokes,
        int video_width, int video_height,
        float display_x, float display_y,
        float display_width, float display_height,
        float line_width_scale = 1.0f,
        bool use_smoothing = true);

    // Render annotations directly to a texture for export
    // target_texture: id<MTLTexture> as void*
    void RenderAnnotationsToImage(
        const std::vector<ActiveStroke>& strokes,
        void* target_texture,
        int width, int height,
        float line_width_scale = 1.0f);

private:
    bool CreateRenderPipeline();
    bool CreateExportPipeline();
    bool CreateExportPipelineRGBA8();
    bool CreateUnpremultiplyPipeline();
    bool EnsureVertexBuffer(size_t required_bytes);
    bool EnsureRenderTargets(int width, int height);

    void* render_pipeline_ = nullptr;    // id<MTLRenderPipelineState> sampleCount=4, RGBA16Float
    void* export_pipeline_ = nullptr;    // id<MTLRenderPipelineState> sampleCount=1, RGBA16Float
    void* export_pipeline_rgba8_ = nullptr; // id<MTLRenderPipelineState> sampleCount=1, RGBA8
    void* unpremul_pipeline_ = nullptr;  // id<MTLComputePipelineState>
    void* vertex_buffer_ = nullptr;       // id<MTLBuffer>
    size_t vertex_buffer_size_ = 0;

    void* msaa_texture_ = nullptr;       // id<MTLTexture> sampleCount=4
    void* resolve_texture_ = nullptr;    // id<MTLTexture> sampleCount=1 (MSAA resolve target)
    int cached_width_ = 0;
    int cached_height_ = 0;
    uint64_t output_pool_id_ = 0;        // pool texture ID for reuse

    bool initialized_ = false;
};

} // namespace Annotations
} // namespace qcview

#endif // __APPLE__
