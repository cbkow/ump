#pragma once

#ifdef __linux__

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <string>
#include <vector>

// Forward declaration
class OCIOPipeline;

namespace qcview {

//=============================================================================
// VulkanOCIORenderer
//
// Renders OCIO color transforms using Vulkan.
// Draws a fullscreen triangle with the OCIO GLSL shader compiled to SPIR-V.
//
// Takes an input texture (VkImageView), applies OCIO color transform,
// renders to output texture (VkImageView).
//
// Usage:
//   VulkanOCIORenderer renderer;
//   renderer.Initialize();
//   renderer.Apply(pipeline, source_view, dest_image, dest_view, w, h);
//=============================================================================

class VulkanOCIORenderer {
public:
    VulkanOCIORenderer();
    ~VulkanOCIORenderer();

    VulkanOCIORenderer(const VulkanOCIORenderer&) = delete;
    VulkanOCIORenderer& operator=(const VulkanOCIORenderer&) = delete;

    //=========================================================================
    // Initialization
    //=========================================================================

    bool Initialize();
    void Shutdown();
    bool IsInitialized() const { return initialized_; }

    // Invalidate cached OCIO pipeline (call when color pipeline changes)
    void InvalidateOCIOPipeline();

    //=========================================================================
    // Rendering
    //=========================================================================

    // Apply OCIO color transform
    // pipeline: OCIOPipeline with Vulkan SPIR-V shader compiled
    // source_view: Input texture (SHADER_READ_ONLY_OPTIMAL)
    // dest_image: Output VkImage
    // dest_view: Output VkImageView
    // width, height: Render dimensions
    bool Apply(
        ::OCIOPipeline* pipeline,
        VkImageView source_view,
        VkImage dest_image,
        VkImageView dest_view,
        int width, int height);

    // Passthrough copy (identity transform)
    bool ApplyPassthrough(
        VkImageView source_view,
        VkImage dest_image,
        VkImageView dest_view,
        int width, int height);

private:
    //=========================================================================
    // Resource Creation
    //=========================================================================

    bool CreateRenderPass();
    bool CreatePassthroughPipeline();
    bool CreateSampler();
    bool CreateDescriptorPool();

    //=========================================================================
    // Per-Render Helpers
    //=========================================================================

    VkFramebuffer GetOrCreateFramebuffer(VkImageView view, int width, int height);

    //=========================================================================
    // OCIO Pipeline Management
    //=========================================================================

    // The OCIO pipeline may have LUT textures that need descriptor bindings
    struct OCIOPipelineState {
        VkPipeline pipeline = VK_NULL_HANDLE;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        VkDescriptorSetLayout desc_layout = VK_NULL_HANDLE;
        VkDescriptorSet desc_set = VK_NULL_HANDLE;
        VkShaderModule frag_module = VK_NULL_HANDLE;

        // LUT textures
        struct LUTTexture {
            VkImage image = VK_NULL_HANDLE;
            VmaAllocation allocation = VK_NULL_HANDLE;
            VkImageView view = VK_NULL_HANDLE;
            VkSampler sampler = VK_NULL_HANDLE;
            bool is_3d = false;
        };
        std::vector<LUTTexture> lut_textures;

        // Source hash for cache invalidation
        std::string shader_hash;

        bool valid = false;
    };

    void DestroyOCIOPipelineState(OCIOPipelineState& state);

    // Build full OCIO Vulkan pipeline state from OCIOPipeline shader info
    bool BuildOCIOPipelineState(::OCIOPipeline* pipeline);

    // Create a LUT VkImage from float data
    bool CreateLUTTexture(OCIOPipelineState::LUTTexture& lut,
                          const float* data, unsigned width, unsigned height,
                          bool is_3d, unsigned edge_len, bool is_red_channel);

    // Build the Vulkan fragment shader GLSL from OCIO function code
    std::string BuildOCIOFragmentGLSL(const std::string& ocio_function_glsl,
                                       int num_lut_bindings);

    //=========================================================================
    // Resources
    //=========================================================================

    bool initialized_ = false;

    // Render pass for the output format
    VkRenderPass render_pass_ = VK_NULL_HANDLE;

    // Passthrough pipeline (identity transform)
    VkShaderModule passthrough_vert_ = VK_NULL_HANDLE;
    VkShaderModule passthrough_frag_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout passthrough_desc_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout passthrough_pipe_layout_ = VK_NULL_HANDLE;
    VkPipeline passthrough_pipeline_ = VK_NULL_HANDLE;
    VkDescriptorSet passthrough_desc_set_ = VK_NULL_HANDLE;

    // Shared resources
    VkSampler linear_sampler_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;

    // Cached OCIO pipeline (rebuilt when shader changes)
    OCIOPipelineState ocio_state_;

    // Cached framebuffers
    struct CachedFramebuffer {
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        int width = 0;
        int height = 0;
    };
    std::vector<CachedFramebuffer> cached_framebuffers_;
};

} // namespace qcview

#endif // __linux__
