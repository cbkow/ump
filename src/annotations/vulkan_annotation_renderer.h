#pragma once

#ifdef __linux__

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <vector>
#include <cstdint>
#include "viewport_annotator.h"
#include "stroke_tessellator.h"

namespace qcview {
namespace Annotations {

class VulkanAnnotationRenderer {
public:
    VulkanAnnotationRenderer();
    ~VulkanAnnotationRenderer();

    VulkanAnnotationRenderer(const VulkanAnnotationRenderer&) = delete;
    VulkanAnnotationRenderer& operator=(const VulkanAnnotationRenderer&) = delete;

    bool Initialize();
    void Shutdown();
    bool IsInitialized() const { return initialized_; }

    // Render annotations to an offscreen texture for viewport overlay.
    // Returns a VulkanTexturePool ID for ImGui display, or 0 on failure.
    uint64_t RenderAnnotations(
        const std::vector<ActiveStroke>& strokes,
        int width, int height,
        float display_pos_x, float display_pos_y,
        float display_width, float display_height,
        float line_width_scale,
        bool apply_smoothing
    );

    // Render annotations onto an existing VkImage (for export).
    // The target image must be in COLOR_ATTACHMENT_OPTIMAL layout.
    bool RenderAnnotationsToImage(
        const std::vector<ActiveStroke>& strokes,
        VkImage target_image, VkImageView target_view,
        int width, int height,
        float display_pos_x, float display_pos_y,
        float display_width, float display_height,
        float line_width_scale,
        bool apply_smoothing
    );

private:
    // Pipeline setup
    bool CreateMSAARenderPass();
    bool CreateExportRenderPass();
    bool CreatePipelineLayout();
    bool CreateGraphicsPipeline(VkRenderPass rp, VkSampleCountFlagBits samples, VkPipeline& pipeline);
    bool EnsureVertexBuffer(size_t required_bytes);

    // Un-premultiply compute pipeline
    bool CreateUnpremultiplyPipeline();

    // Image management
    bool CreateMSAAImage(int width, int height);
    void DestroyMSAAImage();
    bool CreateOutputImage(int width, int height);
    void DestroyOutputImage();

    // Per-render helpers
    VkFramebuffer CreateFramebuffer(VkRenderPass rp, const VkImageView* attachments,
                                     uint32_t attachment_count, int width, int height);

    // Upload vertex data and render
    bool RenderMesh(const TessellatedMesh& mesh, VkRenderPass rp, VkPipeline pipeline,
                    VkFramebuffer fb, int width, int height,
                    VkAttachmentLoadOp load_op);

    // Dispatch un-premultiply compute on output image
    void DispatchUnpremultiply(int width, int height);

    bool initialized_ = false;

    // Shader modules
    VkShaderModule vert_module_ = VK_NULL_HANDLE;
    VkShaderModule frag_module_ = VK_NULL_HANDLE;

    // Render passes
    VkRenderPass msaa_render_pass_ = VK_NULL_HANDLE;   // MSAA 4x → resolve
    VkRenderPass export_render_pass_ = VK_NULL_HANDLE;  // Single-sample, LOAD_OP_LOAD

    // Graphics pipelines
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline msaa_pipeline_ = VK_NULL_HANDLE;     // For viewport (MSAA 4x)
    VkPipeline export_pipeline_ = VK_NULL_HANDLE;   // For export (single-sample)

    // Push constant: viewport size for pixel→NDC conversion
    struct PushConstants {
        float viewport_width;
        float viewport_height;
    };

    // Un-premultiply compute resources
    VkShaderModule unpremul_shader_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout unpremul_desc_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout unpremul_pipe_layout_ = VK_NULL_HANDLE;
    VkPipeline unpremul_pipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool unpremul_desc_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet unpremul_desc_set_ = VK_NULL_HANDLE;

    // Vertex buffer (CPU_TO_GPU, resizable)
    VkBuffer vertex_buffer_ = VK_NULL_HANDLE;
    VmaAllocation vertex_alloc_ = VK_NULL_HANDLE;
    size_t vertex_buffer_size_ = 0;
    static constexpr size_t kInitialVertexBufferSize = 1024 * 1024;  // 1 MB

    // MSAA image (4x, transient)
    VkImage msaa_image_ = VK_NULL_HANDLE;
    VmaAllocation msaa_alloc_ = VK_NULL_HANDLE;
    VkImageView msaa_view_ = VK_NULL_HANDLE;
    int msaa_width_ = 0;
    int msaa_height_ = 0;

    // Output image (single-sample, MSAA resolve target + compute in-place + pool)
    VkImage output_image_ = VK_NULL_HANDLE;
    VmaAllocation output_alloc_ = VK_NULL_HANDLE;
    VkImageView output_view_ = VK_NULL_HANDLE;
    int output_width_ = 0;
    int output_height_ = 0;

    // Pool texture ID for the output
    uint64_t output_pool_id_ = 0;

    // Cached MSAA framebuffer (2 attachments: MSAA + resolve)
    VkFramebuffer cached_msaa_fb_ = VK_NULL_HANDLE;
    int cached_msaa_fb_w_ = 0;
    int cached_msaa_fb_h_ = 0;
};

} // namespace Annotations
} // namespace qcview

#endif // __linux__
