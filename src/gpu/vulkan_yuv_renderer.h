#pragma once

#ifdef __linux__

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <string>
#include <vector>

namespace qcview {

//=============================================================================
// YUV Color Space (shared with D3D11 path)
//=============================================================================

#ifndef QCVIEW_YUV_COLOR_SPACE_DEFINED
#define QCVIEW_YUV_COLOR_SPACE_DEFINED
enum class YUVColorSpace {
    BT_709,     // HD (sRGB primaries)
    BT_2020     // UHD/HDR (wide gamut)
};
#endif

//=============================================================================
// YUV Render Params (shared with D3D11 path)
//=============================================================================

struct YUVRenderParams {
    int width = 0;
    int height = 0;
    int bit_depth = 8;
    bool is_hdr = false;
    bool is_full_range = false;
    YUVColorSpace color_space = YUVColorSpace::BT_709;

    int plane_count = 2;         // 2 for NV12/P010, 3 for planar YUV, 4 for YUVA/GBRAP
    bool has_alpha = false;
    bool is_rgb_planar = false;  // GBRP/GBRAP formats
};

//=============================================================================
// VulkanYUVRenderer
//
// Renders YUV textures to RGB via Vulkan graphics pipeline.
// Port of D3D11YUVRenderer.
//
// Supports:
// - 2-plane NV12/P010 (interleaved UV)
// - 3-plane planar YUV (YUV420P, YUV422P, YUV444P)
// - 3-plane GBRP RGB planar
// - 4-plane YUVA/GBRAP with alpha
// - BT.709 and BT.2020 color matrices
// - Video range and full range
// - 8/10/12-bit depth
//
// Output is RGBA16F, suitable for OCIO processing.
//
// Usage:
//   VulkanYUVRenderer renderer;
//   renderer.Initialize();
//   renderer.Render(plane_views, dest_image, dest_view, params);
//=============================================================================

class VulkanYUVRenderer {
public:
    VulkanYUVRenderer();
    ~VulkanYUVRenderer();

    VulkanYUVRenderer(const VulkanYUVRenderer&) = delete;
    VulkanYUVRenderer& operator=(const VulkanYUVRenderer&) = delete;

    //=========================================================================
    // Initialization
    //=========================================================================

    bool Initialize();
    void Shutdown();
    bool IsInitialized() const { return initialized_; }

    //=========================================================================
    // Rendering
    //=========================================================================

    // Render YUV planes to an RGBA output image
    // plane_views: Array of VkImageView for each input plane (up to 5)
    //   [0] = Y/G plane
    //   [1] = UV interleaved (NV12/P010) - sampler reads RG
    //   [2] = U/B plane (planar)
    //   [3] = V/R plane (planar)
    //   [4] = Alpha plane (YUVA/GBRAP)
    // dest_image: Output VkImage (must be RGBA16F, TRANSFER_DST_OPTIMAL or similar)
    // dest_view: VkImageView for the output
    // params: Conversion parameters
    //
    // The caller must transition dest_image to COLOR_ATTACHMENT_OPTIMAL before
    // calling and to SHADER_READ_ONLY_OPTIMAL after for display.
    bool Render(
        const VkImageView* plane_views,
        int num_plane_views,
        VkImage dest_image,
        VkImageView dest_view,
        int dest_width, int dest_height,
        const YUVRenderParams& params);

private:
    //=========================================================================
    // Pipeline Creation
    //=========================================================================

    bool CreateRenderPass();
    bool CreateDescriptorSetLayout();
    bool CreatePipelineLayout();
    bool CreateGraphicsPipeline();
    bool CreateSampler();
    bool CreateDescriptorPool();
    bool CreateUniformBuffer();

    //=========================================================================
    // Per-Render Helpers
    //=========================================================================

    // Create or reuse a framebuffer for the given output image view + dimensions
    VkFramebuffer GetOrCreateFramebuffer(VkImageView view, int width, int height);

    // Update UBO with current params
    void UpdateUniformBuffer(const YUVRenderParams& params);

    // Update descriptor set with current textures
    void UpdateDescriptorSet(const VkImageView* plane_views, int num_plane_views);

    //=========================================================================
    // Vulkan Resources
    //=========================================================================

    bool initialized_ = false;

    // Render pass for RGBA16F output
    VkRenderPass render_pass_ = VK_NULL_HANDLE;

    // Pipeline
    VkDescriptorSetLayout descriptor_set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline graphics_pipeline_ = VK_NULL_HANDLE;

    // Shader modules
    VkShaderModule vert_module_ = VK_NULL_HANDLE;
    VkShaderModule frag_module_ = VK_NULL_HANDLE;

    // Sampler (linear, clamp)
    VkSampler sampler_ = VK_NULL_HANDLE;

    // Descriptor pool and set
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptor_set_ = VK_NULL_HANDLE;

    // Uniform buffer
    VkBuffer uniform_buffer_ = VK_NULL_HANDLE;
    VmaAllocation uniform_allocation_ = VK_NULL_HANDLE;
    void* uniform_mapped_ = nullptr;

    // Dummy texture for unused plane slots
    VkImage dummy_image_ = VK_NULL_HANDLE;
    VmaAllocation dummy_allocation_ = VK_NULL_HANDLE;
    VkImageView dummy_view_ = VK_NULL_HANDLE;

    // Cached framebuffers: key = VkImageView handle
    struct CachedFramebuffer {
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        int width = 0;
        int height = 0;
    };
    std::vector<CachedFramebuffer> cached_framebuffers_;
};

} // namespace qcview

#endif // __linux__
