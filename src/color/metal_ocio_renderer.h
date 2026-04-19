#pragma once

#ifdef __APPLE__

#include <string>
#include <vector>
#include <cstdint>

class OCIOPipeline;  // Global namespace (not in qcview)

namespace qcview {

//=============================================================================
// MetalOCIORenderer
//
// Applies OCIO color transforms via Metal compute shaders.
// Port of VulkanOCIORenderer for macOS.
//=============================================================================

class MetalOCIORenderer {
public:
    MetalOCIORenderer();
    ~MetalOCIORenderer();

    MetalOCIORenderer(const MetalOCIORenderer&) = delete;
    MetalOCIORenderer& operator=(const MetalOCIORenderer&) = delete;

    bool Initialize();
    void Shutdown();
    bool IsInitialized() const { return initialized_; }

    // Apply OCIO color transform to a source texture (renders to persistent output).
    // source_pool_id: pool texture ID (RGBA16F input, decoded video).
    // Returns id<MTLTexture> (as void*) of the color-corrected output, owned by
    // this renderer. The texture is retained for the renderer's lifetime and is
    // NOT in MetalTexturePool, so it cannot be evicted.
    void* Apply(OCIOPipeline* pipeline, uint64_t source_pool_id,
                int width, int height);

    // Passthrough: returns the source pool texture's MTLTexture pointer directly.
    // Pool retains ownership; caller must not release.
    void* ApplyPassthrough(uint64_t source_pool_id, int width, int height);

    // Linear-to-sRGB encode (EDR pre-encode). Input is an MTLTexture pointer
    // (typically the return of Apply). Returns persistent sRGB MTLTexture owned
    // by this renderer.
    void* ApplyLinearToSRGB(void* source_mtl_texture, int width, int height);

    // Create a synchronous copy of an MTLTexture into a fresh pool texture
    // (for screenshots). Caller owns the returned pool id.
    uint64_t CopyTextureSync(void* source_mtl_texture, int width, int height);

    // Invalidate cached pipeline state (call when OCIO config changes)
    void InvalidateCache();

private:
    bool BuildPipelineForOCIO(OCIOPipeline* pipeline);
    void EnsureOutputTexture(int width, int height);
    void EnsureSRGBOutputTexture(int width, int height);

    // Dispatch a compute pipeline to output texture (no GPU wait)
    void DispatchCompute(void* pipeline_state, void* src_texture, void* dst_texture,
                         int width, int height);

    // Cached OCIO pipeline state
    void* compute_pipeline_ = nullptr;  // id<MTLComputePipelineState>
    std::string cached_shader_hash_;

    // LUT textures
    struct LUTTexture {
        void* texture = nullptr;  // id<MTLTexture>
        bool is_3d = false;
    };
    std::vector<LUTTexture> lut_textures_;

    // Passthrough pipeline
    void* passthrough_pipeline_ = nullptr;  // id<MTLComputePipelineState>

    // Linear-to-sRGB pipeline (for EDR pre-encoding)
    void* linear_to_srgb_pipeline_ = nullptr;  // id<MTLComputePipelineState>

    // Persistent output MTLTextures — retained directly (MRC), never in
    // MetalTexturePool so they cannot be LRU-evicted. Reused across frames;
    // reallocated on dimension change; released on Shutdown.
    void* output_texture_ = nullptr;       // id<MTLTexture>, RGBA16Float
    int output_width_ = 0;
    int output_height_ = 0;
    void* srgb_output_texture_ = nullptr;  // id<MTLTexture>, RGBA16Float
    int srgb_output_width_ = 0;
    int srgb_output_height_ = 0;

    // Shared linear sampler for LUT texture sampling
    void* linear_sampler_ = nullptr;  // id<MTLSamplerState>

    bool initialized_ = false;
};

} // namespace qcview

#endif // __APPLE__
