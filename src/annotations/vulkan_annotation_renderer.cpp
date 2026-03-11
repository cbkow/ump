#include "vulkan_annotation_renderer.h"

#ifdef __linux__

#include "../gpu/vulkan_device.h"
#include "../gpu/vulkan_shader_compiler.h"
#include "../gpu/vulkan_texture_pool.h"
#include "../utils/debug_utils.h"

#include <cstring>
#include <array>

namespace qcview {
namespace Annotations {

//=============================================================================
// Embedded Shader Sources
//=============================================================================

static const char* g_annotation_vert_glsl = R"(
#version 450

layout(location = 0) in vec2 inPos;    // Pixel coordinates
layout(location = 1) in vec4 inColor;  // RGBA

layout(location = 0) out vec4 v_color;

layout(push_constant) uniform PC {
    float viewport_width;
    float viewport_height;
};

void main() {
    // Convert pixel coords to NDC [-1, 1]
    float ndc_x = (inPos.x / viewport_width) * 2.0 - 1.0;
    float ndc_y = (inPos.y / viewport_height) * 2.0 - 1.0;
    gl_Position = vec4(ndc_x, ndc_y, 0.0, 1.0);
    v_color = inColor;
}
)";

static const char* g_annotation_frag_glsl = R"(
#version 450

layout(location = 0) in vec4 v_color;
layout(location = 0) out vec4 out_color;

void main() {
    out_color = v_color;
}
)";

// Compute shader: un-premultiply alpha in-place on a storage image.
// MSAA resolve produces premultiplied content (RGB scaled by coverage).
// This converts back to straight alpha for correct ImGui compositing.
static const char* g_unpremultiply_comp_glsl = R"(
#version 450

layout(local_size_x = 16, local_size_y = 16) in;

layout(set = 0, binding = 0, rgba8) uniform image2D img;

void main() {
    ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(img);
    if (pos.x >= size.x || pos.y >= size.y) return;

    vec4 c = imageLoad(img, pos);
    if (c.a > 0.001) {
        c.rgb /= c.a;
    }
    imageStore(img, pos, c);
}
)";

//=============================================================================
// Implementation
//=============================================================================

VulkanAnnotationRenderer::VulkanAnnotationRenderer() = default;

VulkanAnnotationRenderer::~VulkanAnnotationRenderer() {
    Shutdown();
}

bool VulkanAnnotationRenderer::Initialize() {
    if (initialized_) return true;

    auto& dev = VulkanDeviceManager::Instance();
    if (!dev.IsInitialized()) {
        Debug::Log("VulkanAnnotationRenderer: Device not initialized");
        return false;
    }

    // Compile shaders
    VulkanShaderCompiler compiler;

    vert_module_ = compiler.CompileAndCreate(g_annotation_vert_glsl, ShaderStage::VERTEX, "annotation_vert");
    if (vert_module_ == VK_NULL_HANDLE) {
        Debug::Log("VulkanAnnotationRenderer: Failed to compile vertex shader");
        return false;
    }

    frag_module_ = compiler.CompileAndCreate(g_annotation_frag_glsl, ShaderStage::FRAGMENT, "annotation_frag");
    if (frag_module_ == VK_NULL_HANDLE) {
        Debug::Log("VulkanAnnotationRenderer: Failed to compile fragment shader");
        Shutdown();
        return false;
    }

    if (!CreateMSAARenderPass()) { Shutdown(); return false; }
    if (!CreateExportRenderPass()) { Shutdown(); return false; }
    if (!CreatePipelineLayout()) { Shutdown(); return false; }
    if (!CreateGraphicsPipeline(msaa_render_pass_, VK_SAMPLE_COUNT_4_BIT, msaa_pipeline_)) { Shutdown(); return false; }
    if (!CreateGraphicsPipeline(export_render_pass_, VK_SAMPLE_COUNT_1_BIT, export_pipeline_)) { Shutdown(); return false; }
    if (!CreateUnpremultiplyPipeline()) { Shutdown(); return false; }
    if (!EnsureVertexBuffer(kInitialVertexBufferSize)) { Shutdown(); return false; }

    initialized_ = true;
    Debug::Log("VulkanAnnotationRenderer: Initialized");
    return true;
}

void VulkanAnnotationRenderer::Shutdown() {
    auto& dev = VulkanDeviceManager::Instance();
    VkDevice device = dev.GetDevice();

    if (device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device);

        if (cached_msaa_fb_ != VK_NULL_HANDLE)
            vkDestroyFramebuffer(device, cached_msaa_fb_, nullptr);

        DestroyMSAAImage();
        DestroyOutputImage();

        if (vertex_buffer_ != VK_NULL_HANDLE)
            vmaDestroyBuffer(dev.GetAllocator(), vertex_buffer_, vertex_alloc_);

        if (msaa_pipeline_ != VK_NULL_HANDLE)
            vkDestroyPipeline(device, msaa_pipeline_, nullptr);
        if (export_pipeline_ != VK_NULL_HANDLE)
            vkDestroyPipeline(device, export_pipeline_, nullptr);
        if (pipeline_layout_ != VK_NULL_HANDLE)
            vkDestroyPipelineLayout(device, pipeline_layout_, nullptr);
        if (msaa_render_pass_ != VK_NULL_HANDLE)
            vkDestroyRenderPass(device, msaa_render_pass_, nullptr);
        if (export_render_pass_ != VK_NULL_HANDLE)
            vkDestroyRenderPass(device, export_render_pass_, nullptr);
        if (vert_module_ != VK_NULL_HANDLE)
            vkDestroyShaderModule(device, vert_module_, nullptr);
        if (frag_module_ != VK_NULL_HANDLE)
            vkDestroyShaderModule(device, frag_module_, nullptr);

        // Compute un-premultiply resources
        if (unpremul_pipeline_ != VK_NULL_HANDLE)
            vkDestroyPipeline(device, unpremul_pipeline_, nullptr);
        if (unpremul_pipe_layout_ != VK_NULL_HANDLE)
            vkDestroyPipelineLayout(device, unpremul_pipe_layout_, nullptr);
        if (unpremul_desc_pool_ != VK_NULL_HANDLE)
            vkDestroyDescriptorPool(device, unpremul_desc_pool_, nullptr);
        if (unpremul_desc_layout_ != VK_NULL_HANDLE)
            vkDestroyDescriptorSetLayout(device, unpremul_desc_layout_, nullptr);
        if (unpremul_shader_ != VK_NULL_HANDLE)
            vkDestroyShaderModule(device, unpremul_shader_, nullptr);
    }

    // Delete pool texture
    if (output_pool_id_ != 0) {
        VulkanTexturePool::Instance().QueueDelete(output_pool_id_);
        output_pool_id_ = 0;
    }

    cached_msaa_fb_ = VK_NULL_HANDLE;
    cached_msaa_fb_w_ = 0;
    cached_msaa_fb_h_ = 0;
    vertex_buffer_ = VK_NULL_HANDLE;
    vertex_alloc_ = VK_NULL_HANDLE;
    vertex_buffer_size_ = 0;
    msaa_pipeline_ = VK_NULL_HANDLE;
    export_pipeline_ = VK_NULL_HANDLE;
    pipeline_layout_ = VK_NULL_HANDLE;
    msaa_render_pass_ = VK_NULL_HANDLE;
    export_render_pass_ = VK_NULL_HANDLE;
    vert_module_ = VK_NULL_HANDLE;
    frag_module_ = VK_NULL_HANDLE;

    unpremul_pipeline_ = VK_NULL_HANDLE;
    unpremul_pipe_layout_ = VK_NULL_HANDLE;
    unpremul_desc_pool_ = VK_NULL_HANDLE;
    unpremul_desc_set_ = VK_NULL_HANDLE;
    unpremul_desc_layout_ = VK_NULL_HANDLE;
    unpremul_shader_ = VK_NULL_HANDLE;

    initialized_ = false;
}

//=============================================================================
// Render Pass Creation
//=============================================================================

bool VulkanAnnotationRenderer::CreateMSAARenderPass() {
    // Attachment 0: MSAA 4x color (transient)
    // Attachment 1: Resolve target (single-sample RGBA8)
    std::array<VkAttachmentDescription, 2> attachments{};

    // MSAA color attachment
    attachments[0].format = VK_FORMAT_R8G8B8A8_UNORM;
    attachments[0].samples = VK_SAMPLE_COUNT_4_BIT;
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    // Resolve attachment — finalLayout GENERAL for compute access
    attachments[1].format = VK_FORMAT_R8G8B8A8_UNORM;
    attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[1].finalLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkAttachmentReference color_ref{};
    color_ref.attachment = 0;
    color_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference resolve_ref{};
    resolve_ref.attachment = 1;
    resolve_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_ref;
    subpass.pResolveAttachments = &resolve_ref;

    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rp_info{};
    rp_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rp_info.attachmentCount = static_cast<uint32_t>(attachments.size());
    rp_info.pAttachments = attachments.data();
    rp_info.subpassCount = 1;
    rp_info.pSubpasses = &subpass;
    rp_info.dependencyCount = 1;
    rp_info.pDependencies = &dependency;

    auto device = VulkanDeviceManager::Instance().GetDevice();
    if (vkCreateRenderPass(device, &rp_info, nullptr, &msaa_render_pass_) != VK_SUCCESS) {
        Debug::Log("VulkanAnnotationRenderer: Failed to create MSAA render pass");
        return false;
    }
    return true;
}

bool VulkanAnnotationRenderer::CreateExportRenderPass() {
    VkAttachmentDescription color_attachment{};
    color_attachment.format = VK_FORMAT_R8G8B8A8_UNORM;
    color_attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color_attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color_attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color_attachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color_attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference color_ref{};
    color_ref.attachment = 0;
    color_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_ref;

    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rp_info{};
    rp_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rp_info.attachmentCount = 1;
    rp_info.pAttachments = &color_attachment;
    rp_info.subpassCount = 1;
    rp_info.pSubpasses = &subpass;
    rp_info.dependencyCount = 1;
    rp_info.pDependencies = &dependency;

    auto device = VulkanDeviceManager::Instance().GetDevice();
    if (vkCreateRenderPass(device, &rp_info, nullptr, &export_render_pass_) != VK_SUCCESS) {
        Debug::Log("VulkanAnnotationRenderer: Failed to create export render pass");
        return false;
    }
    return true;
}

//=============================================================================
// Pipeline Creation
//=============================================================================

bool VulkanAnnotationRenderer::CreatePipelineLayout() {
    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    push_range.offset = 0;
    push_range.size = sizeof(PushConstants);

    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &push_range;

    auto device = VulkanDeviceManager::Instance().GetDevice();
    if (vkCreatePipelineLayout(device, &layout_info, nullptr, &pipeline_layout_) != VK_SUCCESS) {
        Debug::Log("VulkanAnnotationRenderer: Failed to create pipeline layout");
        return false;
    }
    return true;
}

bool VulkanAnnotationRenderer::CreateGraphicsPipeline(
    VkRenderPass rp, VkSampleCountFlagBits samples, VkPipeline& pipeline
) {
    auto device = VulkanDeviceManager::Instance().GetDevice();

    VkPipelineShaderStageCreateInfo vert_stage{};
    vert_stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vert_stage.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vert_stage.module = vert_module_;
    vert_stage.pName = "main";

    VkPipelineShaderStageCreateInfo frag_stage{};
    frag_stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    frag_stage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    frag_stage.module = frag_module_;
    frag_stage.pName = "main";

    VkPipelineShaderStageCreateInfo stages[] = {vert_stage, frag_stage};

    // Vertex input: position (vec2) + color (vec4)
    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(TessVertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    std::array<VkVertexInputAttributeDescription, 2> attrs{};
    attrs[0].binding = 0;
    attrs[0].location = 0;
    attrs[0].format = VK_FORMAT_R32G32_SFLOAT;
    attrs[0].offset = offsetof(TessVertex, x);
    attrs[1].binding = 0;
    attrs[1].location = 1;
    attrs[1].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    attrs[1].offset = offsetof(TessVertex, r);

    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input.vertexBindingDescriptionCount = 1;
    vertex_input.pVertexBindingDescriptions = &binding;
    vertex_input.vertexAttributeDescriptionCount = static_cast<uint32_t>(attrs.size());
    vertex_input.pVertexAttributeDescriptions = attrs.data();

    VkPipelineInputAssemblyStateCreateInfo input_assembly{};
    input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewport_state{};
    viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = samples;

    // Standard alpha blending: srcAlpha/1-srcAlpha
    VkPipelineColorBlendAttachmentState blend_attachment{};
    blend_attachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    blend_attachment.blendEnable = VK_TRUE;
    blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend_attachment.colorBlendOp = VK_BLEND_OP_ADD;
    blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend_attachment.alphaBlendOp = VK_BLEND_OP_ADD;

    VkPipelineColorBlendStateCreateInfo color_blending{};
    color_blending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    color_blending.attachmentCount = 1;
    color_blending.pAttachments = &blend_attachment;

    VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic_state{};
    dynamic_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic_state.dynamicStateCount = 2;
    dynamic_state.pDynamicStates = dynamic_states;

    VkGraphicsPipelineCreateInfo pipeline_info{};
    pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline_info.stageCount = 2;
    pipeline_info.pStages = stages;
    pipeline_info.pVertexInputState = &vertex_input;
    pipeline_info.pInputAssemblyState = &input_assembly;
    pipeline_info.pViewportState = &viewport_state;
    pipeline_info.pRasterizationState = &rasterizer;
    pipeline_info.pMultisampleState = &multisampling;
    pipeline_info.pColorBlendState = &color_blending;
    pipeline_info.pDynamicState = &dynamic_state;
    pipeline_info.layout = pipeline_layout_;
    pipeline_info.renderPass = rp;
    pipeline_info.subpass = 0;

    auto cache = VulkanDeviceManager::Instance().GetPipelineCache();
    if (vkCreateGraphicsPipelines(device, cache, 1, &pipeline_info, nullptr, &pipeline) != VK_SUCCESS) {
        Debug::Log("VulkanAnnotationRenderer: Failed to create graphics pipeline");
        return false;
    }
    return true;
}

//=============================================================================
// Un-premultiply Compute Pipeline
//=============================================================================

bool VulkanAnnotationRenderer::CreateUnpremultiplyPipeline() {
    auto device = VulkanDeviceManager::Instance().GetDevice();

    // Compile compute shader
    VulkanShaderCompiler compiler;
    unpremul_shader_ = compiler.CompileAndCreate(
        g_unpremultiply_comp_glsl, ShaderStage::COMPUTE, "annotation_unpremul");
    if (unpremul_shader_ == VK_NULL_HANDLE) {
        Debug::Log("VulkanAnnotationRenderer: Failed to compile un-premultiply compute shader");
        return false;
    }

    // Descriptor set layout: 1 storage image
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_info.bindingCount = 1;
    layout_info.pBindings = &binding;

    if (vkCreateDescriptorSetLayout(device, &layout_info, nullptr, &unpremul_desc_layout_) != VK_SUCCESS) {
        Debug::Log("VulkanAnnotationRenderer: Failed to create un-premultiply descriptor set layout");
        return false;
    }

    // Pipeline layout
    VkPipelineLayoutCreateInfo pipe_layout_info{};
    pipe_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipe_layout_info.setLayoutCount = 1;
    pipe_layout_info.pSetLayouts = &unpremul_desc_layout_;

    if (vkCreatePipelineLayout(device, &pipe_layout_info, nullptr, &unpremul_pipe_layout_) != VK_SUCCESS) {
        Debug::Log("VulkanAnnotationRenderer: Failed to create un-premultiply pipeline layout");
        return false;
    }

    // Compute pipeline
    VkComputePipelineCreateInfo comp_info{};
    comp_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    comp_info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    comp_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    comp_info.stage.module = unpremul_shader_;
    comp_info.stage.pName = "main";
    comp_info.layout = unpremul_pipe_layout_;

    auto cache = VulkanDeviceManager::Instance().GetPipelineCache();
    if (vkCreateComputePipelines(device, cache, 1, &comp_info, nullptr, &unpremul_pipeline_) != VK_SUCCESS) {
        Debug::Log("VulkanAnnotationRenderer: Failed to create un-premultiply compute pipeline");
        return false;
    }

    // Descriptor pool (1 set, 1 storage image descriptor)
    VkDescriptorPoolSize pool_size{};
    pool_size.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    pool_size.descriptorCount = 1;

    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.maxSets = 1;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &pool_size;

    if (vkCreateDescriptorPool(device, &pool_info, nullptr, &unpremul_desc_pool_) != VK_SUCCESS) {
        Debug::Log("VulkanAnnotationRenderer: Failed to create un-premultiply descriptor pool");
        return false;
    }

    // Allocate descriptor set
    VkDescriptorSetAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc_info.descriptorPool = unpremul_desc_pool_;
    alloc_info.descriptorSetCount = 1;
    alloc_info.pSetLayouts = &unpremul_desc_layout_;

    if (vkAllocateDescriptorSets(device, &alloc_info, &unpremul_desc_set_) != VK_SUCCESS) {
        Debug::Log("VulkanAnnotationRenderer: Failed to allocate un-premultiply descriptor set");
        return false;
    }

    return true;
}

void VulkanAnnotationRenderer::DispatchUnpremultiply(int width, int height) {
    auto& dev = VulkanDeviceManager::Instance();

    // Update descriptor set to point at output_image_
    VkDescriptorImageInfo img_info{};
    img_info.imageView = output_view_;
    img_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = unpremul_desc_set_;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    write.pImageInfo = &img_info;

    vkUpdateDescriptorSets(dev.GetDevice(), 1, &write, 0, nullptr);

    // Dispatch
    VkCommandBuffer cmd = dev.BeginSingleTimeCommands();

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, unpremul_pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            unpremul_pipe_layout_, 0, 1, &unpremul_desc_set_, 0, nullptr);

    uint32_t gx = (width + 15) / 16;
    uint32_t gy = (height + 15) / 16;
    vkCmdDispatch(cmd, gx, gy, 1);

    // Barrier: compute write → fragment shader read
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = output_image_;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);

    dev.EndSingleTimeCommands(cmd);
}

//=============================================================================
// Vertex Buffer
//=============================================================================

bool VulkanAnnotationRenderer::EnsureVertexBuffer(size_t required_bytes) {
    if (vertex_buffer_ != VK_NULL_HANDLE && vertex_buffer_size_ >= required_bytes) {
        return true;
    }

    auto& dev = VulkanDeviceManager::Instance();

    if (vertex_buffer_ != VK_NULL_HANDLE) {
        vmaDestroyBuffer(dev.GetAllocator(), vertex_buffer_, vertex_alloc_);
        vertex_buffer_ = VK_NULL_HANDLE;
        vertex_alloc_ = VK_NULL_HANDLE;
    }

    size_t new_size = vertex_buffer_size_ > 0 ? vertex_buffer_size_ : kInitialVertexBufferSize;
    while (new_size < required_bytes) {
        new_size *= 2;
    }

    VkBufferCreateInfo buf_info{};
    buf_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buf_info.size = new_size;
    buf_info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;

    VmaAllocationCreateInfo alloc_info{};
    alloc_info.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
    alloc_info.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;

    VmaAllocationInfo alloc_result{};
    if (vmaCreateBuffer(dev.GetAllocator(), &buf_info, &alloc_info,
                        &vertex_buffer_, &vertex_alloc_, &alloc_result) != VK_SUCCESS) {
        Debug::Log("VulkanAnnotationRenderer: Failed to create vertex buffer (" +
                   std::to_string(new_size) + " bytes)");
        return false;
    }

    vertex_buffer_size_ = new_size;
    return true;
}

//=============================================================================
// Image Management
//=============================================================================

bool VulkanAnnotationRenderer::CreateMSAAImage(int width, int height) {
    if (msaa_image_ != VK_NULL_HANDLE &&
        msaa_width_ == width && msaa_height_ == height) {
        return true;
    }

    DestroyMSAAImage();

    auto& dev = VulkanDeviceManager::Instance();

    VkImageCreateInfo img_info{};
    img_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    img_info.imageType = VK_IMAGE_TYPE_2D;
    img_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    img_info.extent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};
    img_info.mipLevels = 1;
    img_info.arrayLayers = 1;
    img_info.samples = VK_SAMPLE_COUNT_4_BIT;
    img_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    img_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
    img_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo alloc_info{};
    alloc_info.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    alloc_info.preferredFlags = VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT;

    if (vmaCreateImage(dev.GetAllocator(), &img_info, &alloc_info,
                       &msaa_image_, &msaa_alloc_, nullptr) != VK_SUCCESS) {
        Debug::Log("VulkanAnnotationRenderer: Failed to create MSAA image");
        return false;
    }

    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = msaa_image_;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.layerCount = 1;

    if (vkCreateImageView(dev.GetDevice(), &view_info, nullptr, &msaa_view_) != VK_SUCCESS) {
        Debug::Log("VulkanAnnotationRenderer: Failed to create MSAA image view");
        vmaDestroyImage(dev.GetAllocator(), msaa_image_, msaa_alloc_);
        msaa_image_ = VK_NULL_HANDLE;
        msaa_alloc_ = VK_NULL_HANDLE;
        return false;
    }

    msaa_width_ = width;
    msaa_height_ = height;
    return true;
}

void VulkanAnnotationRenderer::DestroyMSAAImage() {
    auto& dev = VulkanDeviceManager::Instance();

    if (msaa_view_ != VK_NULL_HANDLE) {
        vkDestroyImageView(dev.GetDevice(), msaa_view_, nullptr);
        msaa_view_ = VK_NULL_HANDLE;
    }
    if (msaa_image_ != VK_NULL_HANDLE) {
        vmaDestroyImage(dev.GetAllocator(), msaa_image_, msaa_alloc_);
        msaa_image_ = VK_NULL_HANDLE;
        msaa_alloc_ = VK_NULL_HANDLE;
    }
    msaa_width_ = 0;
    msaa_height_ = 0;
}

bool VulkanAnnotationRenderer::CreateOutputImage(int width, int height) {
    if (output_image_ != VK_NULL_HANDLE &&
        output_width_ == width && output_height_ == height) {
        return true;
    }

    DestroyOutputImage();

    auto& dev = VulkanDeviceManager::Instance();

    VkImageCreateInfo img_info{};
    img_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    img_info.imageType = VK_IMAGE_TYPE_2D;
    img_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    img_info.extent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};
    img_info.mipLevels = 1;
    img_info.arrayLayers = 1;
    img_info.samples = VK_SAMPLE_COUNT_1_BIT;
    img_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    img_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                     VK_IMAGE_USAGE_SAMPLED_BIT |
                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                     VK_IMAGE_USAGE_STORAGE_BIT;  // For compute un-premultiply
    img_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo alloc_info{};
    alloc_info.usage = VMA_MEMORY_USAGE_GPU_ONLY;

    if (vmaCreateImage(dev.GetAllocator(), &img_info, &alloc_info,
                       &output_image_, &output_alloc_, nullptr) != VK_SUCCESS) {
        Debug::Log("VulkanAnnotationRenderer: Failed to create output image");
        return false;
    }

    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = output_image_;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.layerCount = 1;

    if (vkCreateImageView(dev.GetDevice(), &view_info, nullptr, &output_view_) != VK_SUCCESS) {
        Debug::Log("VulkanAnnotationRenderer: Failed to create output image view");
        vmaDestroyImage(dev.GetAllocator(), output_image_, output_alloc_);
        output_image_ = VK_NULL_HANDLE;
        output_alloc_ = VK_NULL_HANDLE;
        return false;
    }

    output_width_ = width;
    output_height_ = height;
    return true;
}

void VulkanAnnotationRenderer::DestroyOutputImage() {
    auto& dev = VulkanDeviceManager::Instance();

    if (output_view_ != VK_NULL_HANDLE) {
        vkDestroyImageView(dev.GetDevice(), output_view_, nullptr);
        output_view_ = VK_NULL_HANDLE;
    }
    if (output_image_ != VK_NULL_HANDLE) {
        vmaDestroyImage(dev.GetAllocator(), output_image_, output_alloc_);
        output_image_ = VK_NULL_HANDLE;
        output_alloc_ = VK_NULL_HANDLE;
    }
    output_width_ = 0;
    output_height_ = 0;
}

//=============================================================================
// Framebuffer
//=============================================================================

VkFramebuffer VulkanAnnotationRenderer::CreateFramebuffer(
    VkRenderPass rp, const VkImageView* attachments,
    uint32_t attachment_count, int width, int height
) {
    VkFramebufferCreateInfo fb_info{};
    fb_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fb_info.renderPass = rp;
    fb_info.attachmentCount = attachment_count;
    fb_info.pAttachments = attachments;
    fb_info.width = width;
    fb_info.height = height;
    fb_info.layers = 1;

    VkFramebuffer fb = VK_NULL_HANDLE;
    vkCreateFramebuffer(VulkanDeviceManager::Instance().GetDevice(), &fb_info, nullptr, &fb);
    return fb;
}

//=============================================================================
// Render Mesh
//=============================================================================

bool VulkanAnnotationRenderer::RenderMesh(
    const TessellatedMesh& mesh,
    VkRenderPass rp, VkPipeline pipeline,
    VkFramebuffer fb, int width, int height,
    VkAttachmentLoadOp load_op
) {
    if (mesh.vertices.empty()) return true;

    auto& dev = VulkanDeviceManager::Instance();

    // Upload vertex data
    size_t data_size = mesh.vertices.size() * sizeof(TessVertex);
    if (!EnsureVertexBuffer(data_size)) return false;

    VmaAllocationInfo alloc_info{};
    vmaGetAllocationInfo(dev.GetAllocator(), vertex_alloc_, &alloc_info);
    std::memcpy(alloc_info.pMappedData, mesh.vertices.data(), data_size);

    // Record commands
    VkCommandBuffer cmd = dev.BeginSingleTimeCommands();

    VkRenderPassBeginInfo rp_begin{};
    rp_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp_begin.renderPass = rp;
    rp_begin.framebuffer = fb;
    rp_begin.renderArea.extent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height)};

    VkClearValue clear_values[2]{};
    clear_values[0].color = {{0.0f, 0.0f, 0.0f, 0.0f}};
    clear_values[1].color = {{0.0f, 0.0f, 0.0f, 0.0f}};
    rp_begin.clearValueCount = 2;
    rp_begin.pClearValues = clear_values;

    vkCmdBeginRenderPass(cmd, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

    VkViewport viewport{};
    viewport.width = static_cast<float>(width);
    viewport.height = static_cast<float>(height);
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.extent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    PushConstants pc{static_cast<float>(width), static_cast<float>(height)};
    vkCmdPushConstants(cmd, pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT,
                       0, sizeof(PushConstants), &pc);

    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vertex_buffer_, &offset);

    vkCmdDraw(cmd, static_cast<uint32_t>(mesh.vertices.size()), 1, 0, 0);

    vkCmdEndRenderPass(cmd);

    dev.EndSingleTimeCommands(cmd);
    return true;
}

//=============================================================================
// Public API: Viewport Rendering
//=============================================================================

uint64_t VulkanAnnotationRenderer::RenderAnnotations(
    const std::vector<ActiveStroke>& strokes,
    int width, int height,
    float display_pos_x, float display_pos_y,
    float display_width, float display_height,
    float line_width_scale,
    bool apply_smoothing
) {
    if (!initialized_ || strokes.empty() || width <= 0 || height <= 0) return 0;

    // Tessellate all strokes into a single mesh
    TessellatedMesh combined;
    for (const auto& stroke : strokes) {
        auto mesh = StrokeTessellator::Tessellate(
            stroke, display_pos_x, display_pos_y, display_width, display_height,
            line_width_scale, apply_smoothing);
        combined.vertices.insert(combined.vertices.end(),
                                 mesh.vertices.begin(), mesh.vertices.end());
    }

    if (combined.vertices.empty()) return 0;

    // Ensure MSAA and output images exist at correct size
    if (!CreateMSAAImage(width, height)) return 0;
    if (!CreateOutputImage(width, height)) return 0;

    // Rebuild framebuffer if dimensions changed
    if (cached_msaa_fb_ == VK_NULL_HANDLE ||
        cached_msaa_fb_w_ != width || cached_msaa_fb_h_ != height) {
        auto device = VulkanDeviceManager::Instance().GetDevice();
        if (cached_msaa_fb_ != VK_NULL_HANDLE)
            vkDestroyFramebuffer(device, cached_msaa_fb_, nullptr);

        VkImageView attachments[] = {msaa_view_, output_view_};
        cached_msaa_fb_ = CreateFramebuffer(msaa_render_pass_, attachments, 2, width, height);
        cached_msaa_fb_w_ = width;
        cached_msaa_fb_h_ = height;
    }

    // Render with MSAA 4x
    if (!RenderMesh(combined, msaa_render_pass_, msaa_pipeline_,
                    cached_msaa_fb_, width, height, VK_ATTACHMENT_LOAD_OP_CLEAR)) {
        return 0;
    }

    // After MSAA resolve, output_image_ is in GENERAL layout (set by render pass).
    // Run compute shader to un-premultiply alpha in-place.
    DispatchUnpremultiply(width, height);

    // After compute, output_image_ is in SHADER_READ_ONLY_OPTIMAL.
    // Register/update in texture pool.
    auto& pool = VulkanTexturePool::Instance();

    if (output_pool_id_ != 0) {
        // Transition to TRANSFER_SRC for pool update
        auto& dev = VulkanDeviceManager::Instance();
        VkCommandBuffer cmd = dev.BeginSingleTimeCommands();
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = output_image_;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.layerCount = 1;
        barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &barrier);
        dev.EndSingleTimeCommands(cmd);

        if (!pool.UpdateTextureFromImage(output_pool_id_, output_image_, width, height)) {
            output_pool_id_ = 0;
        }
    }

    if (output_pool_id_ == 0) {
        // Transition to TRANSFER_SRC for pool creation
        auto& dev = VulkanDeviceManager::Instance();
        VkCommandBuffer cmd = dev.BeginSingleTimeCommands();
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = output_image_;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.layerCount = 1;
        barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &barrier);
        dev.EndSingleTimeCommands(cmd);

        output_pool_id_ = pool.CreateTextureFromImage(
            output_image_, width, height, VK_FORMAT_R8G8B8A8_UNORM);
    }

    return output_pool_id_;
}

//=============================================================================
// Public API: Export Rendering
//=============================================================================

bool VulkanAnnotationRenderer::RenderAnnotationsToImage(
    const std::vector<ActiveStroke>& strokes,
    VkImage target_image, VkImageView target_view,
    int width, int height,
    float display_pos_x, float display_pos_y,
    float display_width, float display_height,
    float line_width_scale,
    bool apply_smoothing
) {
    if (!initialized_ || strokes.empty() || width <= 0 || height <= 0) return true;

    // Tessellate all strokes
    TessellatedMesh combined;
    for (const auto& stroke : strokes) {
        auto mesh = StrokeTessellator::Tessellate(
            stroke, display_pos_x, display_pos_y, display_width, display_height,
            line_width_scale, apply_smoothing);
        combined.vertices.insert(combined.vertices.end(),
                                 mesh.vertices.begin(), mesh.vertices.end());
    }

    if (combined.vertices.empty()) return true;

    // Create temporary framebuffer for the target image
    VkFramebuffer fb = CreateFramebuffer(export_render_pass_, &target_view, 1, width, height);
    if (fb == VK_NULL_HANDLE) return false;

    bool ok = RenderMesh(combined, export_render_pass_, export_pipeline_,
                         fb, width, height, VK_ATTACHMENT_LOAD_OP_LOAD);

    vkDestroyFramebuffer(VulkanDeviceManager::Instance().GetDevice(), fb, nullptr);
    return ok;
}

} // namespace Annotations
} // namespace qcview

#endif // __linux__
