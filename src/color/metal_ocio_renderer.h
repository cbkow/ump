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

    // Apply OCIO color transform to a source texture (renders to persistent output)
    // source_pool_id: pool texture ID (RGBA16F input)
    // Returns pool texture ID of the color-corrected output (persistent, reused)
    uint64_t Apply(OCIOPipeline* pipeline, uint64_t source_pool_id,
                   int width, int height);

    // Apply passthrough (no-op: returns source directly, no GPU work)
    uint64_t ApplyPassthrough(uint64_t source_pool_id, int width, int height);

    // Apply linear-to-sRGB encode (for EDR: pre-encode so ImGui's EDR shader
    // can linearize back to original values). Uses persistent output texture.
    uint64_t ApplyLinearToSRGB(uint64_t source_pool_id, int width, int height);

    // Create a synchronous copy of a pool texture (for screenshots).
    // Allocates a new texture, copies via blit, waits for GPU completion.
    uint64_t CopyTextureSync(uint64_t source_pool_id, int width, int height);

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

    // Persistent output textures (reused across frames, reallocated on size change)
    uint64_t output_pool_id_ = 0;
    int output_width_ = 0;
    int output_height_ = 0;
    uint64_t srgb_output_pool_id_ = 0;
    int srgb_output_width_ = 0;
    int srgb_output_height_ = 0;

    // Shared linear sampler for LUT texture sampling
    void* linear_sampler_ = nullptr;  // id<MTLSamplerState>

    bool initialized_ = false;
};

} // namespace qcview

#endif // __APPLE__
