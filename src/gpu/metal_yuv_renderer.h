#pragma once

#ifdef __APPLE__

#include <string>
#include <cstdint>
#include <atomic>
#include <memory>

namespace qcview {

// Re-use existing YUV types (also defined in vulkan_yuv_renderer.h under __linux__)
#ifndef QCVIEW_YUV_COLOR_SPACE_DEFINED
#define QCVIEW_YUV_COLOR_SPACE_DEFINED
enum class YUVColorSpace {
    BT_709,
    BT_2020
};
#endif

struct YUVRenderParams;  // Forward declare if already defined

//=============================================================================
// MetalYUVRenderer
//
// Renders YUV textures (NV12/P010) to RGBA16F via Metal compute pipeline.
// Port of VulkanYUVRenderer for macOS.
//=============================================================================

class MetalYUVRenderer {
public:
    MetalYUVRenderer();
    ~MetalYUVRenderer();

    MetalYUVRenderer(const MetalYUVRenderer&) = delete;
    MetalYUVRenderer& operator=(const MetalYUVRenderer&) = delete;

    bool Initialize();
    void Shutdown();
    bool IsInitialized() const { return initialized_; }

    // Render biplanar YUV textures to an RGBA16F output texture (synchronous).
    // y_texture, uv_texture: id<MTLTexture> as void*
    // subsampling: 0=4:2:0, 1=4:2:2, 2=4:4:4
    // Returns id<MTLTexture> (RGBA16F) as void*, caller must release.
    // Blocks until GPU completes. Used by ScrubDecoder for single-frame decode.
    void* RenderToRGBA(void* y_texture, void* uv_texture,
                       int width, int height,
                       int bit_depth, bool is_full_range,
                       bool is_bt2020, bool is_hdr,
                       int subsampling = 0);

    // Async variant: commits compute but does NOT wait for GPU completion.
    // Output texture is immediately usable for pool registration — Metal's
    // same-queue ordering guarantees the compute finishes before any later
    // command buffer that reads the texture.
    //
    // Takes ownership of y_texture and uv_texture cleanup via the callback.
    // cleanup_fn is called on an arbitrary thread when the GPU finishes;
    // use it to release input textures and CVPixelBuffer.
    // Caller must NOT release y/uv textures — the callback handles it.
    void* RenderToRGBAAsync(void* y_texture, void* uv_texture,
                            int width, int height,
                            int bit_depth, bool is_full_range,
                            bool is_bt2020, bool is_hdr,
                            int subsampling,
                            void (*cleanup_fn)(void* ctx),
                            void* cleanup_ctx);

    // Wait for all in-flight async command buffers to complete.
    // Must be called before destroying the renderer or shutting down the device.
    void FlushPendingWork();

private:
    bool CreateComputePipeline();

    void* compute_pipeline_ = nullptr;  // id<MTLComputePipelineState>
    bool initialized_ = false;

    // Async command buffer tracking — shared_ptr so completion handlers
    // keep the counter alive even if the renderer is destroyed first
    // (guards against shutdown ordering issues with global singletons).
    std::shared_ptr<std::atomic<int>> in_flight_count_ = std::make_shared<std::atomic<int>>(0);

    // Cached output texture descriptor (avoid per-frame allocation)
    void* cached_output_desc_ = nullptr;  // MTLTextureDescriptor*
    int cached_desc_width_ = 0;
    int cached_desc_height_ = 0;
};

} // namespace qcview

#endif // __APPLE__
