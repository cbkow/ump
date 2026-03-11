#pragma once

#ifdef __linux__

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <vector>
#include "dual_view_layout.h"

namespace qcview {

//=============================================================================
// VulkanDualCompositor
//
// Composites two VulkanTexturePool textures (LEFT and RIGHT video frames)
// into a single RGBA16F texture for unified dual view display.
//
// Port of D3D11DualCompositor. Key differences:
// - No interop needed (output is VkImage in same device as ImGui)
// - Uses VulkanShaderCompiler for runtime GLSL->SPIR-V
// - Output registered in VulkanTexturePool for ImGui display
// - Push constants instead of constant buffer
//
// GLSL fragment shader handles:
// - Horizontal (side-by-side) and vertical (top-bottom) layouts
// - Aspect-ratio-preserving letterbox/pillarbox
// - Transparent output for missing sources
// - RGBA16F output for HDR content preservation
//=============================================================================

class VulkanDualCompositor {
public:
    VulkanDualCompositor();
    ~VulkanDualCompositor();

    VulkanDualCompositor(const VulkanDualCompositor&) = delete;
    VulkanDualCompositor& operator=(const VulkanDualCompositor&) = delete;

    //=========================================================================
    // Initialization
    //=========================================================================

    bool Initialize();
    void Shutdown();
    bool IsInitialized() const { return initialized_; }

    //=========================================================================
    // Compositing
    //=========================================================================

    // Composite two pool textures into one. Returns pool texture ID.
    // left_pool_id/right_pool_id: VulkanTexturePool IDs (0 = gap/transparent)
    // layout: DualViewLayout with rects and dimensions
    // Returns pool texture ID for the composite, or 0 on failure.
    uint64_t Composite(uint64_t left_pool_id, uint64_t right_pool_id,
                       const DualViewLayout& layout);

    const DualViewLayout& GetLayout() const { return current_layout_; }

private:
    //=========================================================================
    // Resource Creation
    //=========================================================================

    bool CreateRenderPass();
    bool CreateDescriptorSetLayout();
    bool CreatePipelineLayout();
    bool CreateGraphicsPipeline();
    bool CreateSampler();
    bool CreateDescriptorPool();
    bool CreateTransparentTexture();

    //=========================================================================
    // Output Management
    //=========================================================================

    bool CreateOutputImage(int width, int height);
    void DestroyOutputImage();

    //=========================================================================
    // Per-Render Helpers
    //=========================================================================

    VkFramebuffer GetOrCreateFramebuffer(VkImageView view, int width, int height);
    void UpdateDescriptorSet(VkImageView left_view, VkImageView right_view);

    //=========================================================================
    // Vulkan Resources
    //=========================================================================

    bool initialized_ = false;

    // Shader modules
    VkShaderModule vert_module_ = VK_NULL_HANDLE;
    VkShaderModule frag_module_ = VK_NULL_HANDLE;

    // Render pass (RGBA16F, loadOp=CLEAR)
    VkRenderPass render_pass_ = VK_NULL_HANDLE;

    // Pipeline
    VkDescriptorSetLayout desc_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;

    // Descriptor pool and set
    VkDescriptorPool desc_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet desc_set_ = VK_NULL_HANDLE;

    // Sampler (linear, clamp)
    VkSampler sampler_ = VK_NULL_HANDLE;

    // Output image (reused, recreated on resolution change)
    VkImage output_image_ = VK_NULL_HANDLE;
    VmaAllocation output_alloc_ = VK_NULL_HANDLE;
    VkImageView output_view_ = VK_NULL_HANDLE;
    int output_width_ = 0;
    int output_height_ = 0;

    // Pool texture ID for the output
    uint64_t output_pool_id_ = 0;

    // 1x1 transparent texture for gap sources
    VkImage transparent_image_ = VK_NULL_HANDLE;
    VmaAllocation transparent_alloc_ = VK_NULL_HANDLE;
    VkImageView transparent_view_ = VK_NULL_HANDLE;

    // Cached framebuffer
    struct CachedFramebuffer {
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        int width = 0;
        int height = 0;
    };
    std::vector<CachedFramebuffer> cached_framebuffers_;

    // Current layout (for GetLayout())
    DualViewLayout current_layout_;
};

} // namespace qcview

#endif // __linux__
