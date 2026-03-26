#pragma once

#ifdef __APPLE__

#include <cstdint>

namespace qcview {

struct DualViewLayout;  // Forward declare

//=============================================================================
// MetalDualCompositor
//
// Composites LEFT/RIGHT textures into a single RGBA16F output.
// Port of VulkanDualCompositor for macOS.
//=============================================================================

class MetalDualCompositor {
public:
    MetalDualCompositor();
    ~MetalDualCompositor();

    MetalDualCompositor(const MetalDualCompositor&) = delete;
    MetalDualCompositor& operator=(const MetalDualCompositor&) = delete;

    bool Initialize();
    void Shutdown();
    bool IsInitialized() const { return initialized_; }

    // Composite left and right pool textures into a single output
    // Returns pool texture ID (0 = left only, right only)
    uint64_t Composite(uint64_t left_pool_id, uint64_t right_pool_id,
                       int output_width, int output_height,
                       const DualViewLayout& layout);

private:
    bool CreateComputePipeline();

    void* compute_pipeline_ = nullptr;  // id<MTLComputePipelineState>
    bool initialized_ = false;

    // Cached output texture (reused across frames if dimensions unchanged)
    uint64_t output_pool_id_ = 0;
    int output_width_ = 0;
    int output_height_ = 0;

    // 1x1 black dummy texture for missing sides (Metal requires all slots bound)
    void* dummy_texture_ = nullptr;  // id<MTLTexture>
};

} // namespace qcview

#endif // __APPLE__
