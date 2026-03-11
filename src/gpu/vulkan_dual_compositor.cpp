#include "vulkan_dual_compositor.h"

#ifdef __linux__

#include "vulkan_device.h"
#include "vulkan_shader_compiler.h"
#include "vulkan_texture_pool.h"
#include "../utils/debug_utils.h"

#include <cstring>
#include <array>
#include <vector>

namespace qcview {

//=============================================================================
// Embedded Shader Sources
//=============================================================================

// Fullscreen triangle vertex shader (same pattern as VulkanYUVRenderer)
static const char* g_compositor_vert_glsl = R"(
#version 450

layout(location = 0) out vec2 v_uv;

void main() {
    vec2 pos = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
    v_uv = pos;
}
)";

// Dual view compositor fragment shader (port of D3D11 HLSL)
static const char* g_compositor_frag_glsl = R"(
#version 450

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D texLeft;
layout(set = 0, binding = 1) uniform sampler2D texRight;

layout(push_constant) uniform CompositeParams {
    float horizontal;       // 1.0 = side-by-side, 0.0 = top-bottom
    float hasLeft;
    float hasRight;
    float _pad;
    vec4 leftRect;          // x, y, width, height (normalized 0-1)
    vec4 rightRect;         // x, y, width, height (normalized 0-1)
};

void main() {
    vec2 uv = v_uv;
    out_color = vec4(0.0, 0.0, 0.0, 0.0);  // Transparent background

    if (horizontal > 0.5) {
        // Horizontal layout (side-by-side)
        float leftEnd = leftRect.x + leftRect.z;

        if (uv.x < leftEnd && hasLeft > 0.5) {
            if (uv.x >= leftRect.x && uv.x <= leftRect.x + leftRect.z &&
                uv.y >= leftRect.y && uv.y <= leftRect.y + leftRect.w) {
                vec2 leftUV;
                leftUV.x = (uv.x - leftRect.x) / leftRect.z;
                leftUV.y = (uv.y - leftRect.y) / leftRect.w;
                out_color = texture(texLeft, leftUV);
            }
        } else if (uv.x >= rightRect.x && hasRight > 0.5) {
            if (uv.x >= rightRect.x && uv.x <= rightRect.x + rightRect.z &&
                uv.y >= rightRect.y && uv.y <= rightRect.y + rightRect.w) {
                vec2 rightUV;
                rightUV.x = (uv.x - rightRect.x) / rightRect.z;
                rightUV.y = (uv.y - rightRect.y) / rightRect.w;
                out_color = texture(texRight, rightUV);
            }
        }
    } else {
        // Vertical layout (top-bottom)
        float leftEnd = leftRect.y + leftRect.w;

        if (uv.y < leftEnd && hasLeft > 0.5) {
            if (uv.x >= leftRect.x && uv.x <= leftRect.x + leftRect.z &&
                uv.y >= leftRect.y && uv.y <= leftRect.y + leftRect.w) {
                vec2 leftUV;
                leftUV.x = (uv.x - leftRect.x) / leftRect.z;
                leftUV.y = (uv.y - leftRect.y) / leftRect.w;
                out_color = texture(texLeft, leftUV);
            }
        } else if (uv.y >= rightRect.y && hasRight > 0.5) {
            if (uv.x >= rightRect.x && uv.x <= rightRect.x + rightRect.z &&
                uv.y >= rightRect.y && uv.y <= rightRect.y + rightRect.w) {
                vec2 rightUV;
                rightUV.x = (uv.x - rightRect.x) / rightRect.z;
                rightUV.y = (uv.y - rightRect.y) / rightRect.w;
                out_color = texture(texRight, rightUV);
            }
        }
    }
}
)";

//=============================================================================
// Push Constant Layout (must match shader)
//=============================================================================

struct CompositePushConstants {
    float horizontal;
    float hasLeft;
    float hasRight;
    float _pad;
    float leftRect[4];     // x, y, width, height (normalized 0-1)
    float rightRect[4];    // x, y, width, height (normalized 0-1)
};

static_assert(sizeof(CompositePushConstants) <= 128,
              "Push constants must fit in 128 bytes (minimum guaranteed)");

//=============================================================================
// Implementation
//=============================================================================

VulkanDualCompositor::VulkanDualCompositor() = default;

VulkanDualCompositor::~VulkanDualCompositor() {
    Shutdown();
}

bool VulkanDualCompositor::Initialize() {
    if (initialized_) return true;

    auto& dev = VulkanDeviceManager::Instance();
    if (!dev.IsInitialized()) {
        Debug::Log("VulkanDualCompositor: Device not initialized");
        return false;
    }

    // Compile shaders
    VulkanShaderCompiler compiler;

    vert_module_ = compiler.CompileAndCreate(g_compositor_vert_glsl, ShaderStage::VERTEX, "compositor_vert");
    if (vert_module_ == VK_NULL_HANDLE) {
        Debug::Log("VulkanDualCompositor: Failed to compile vertex shader");
        return false;
    }

    frag_module_ = compiler.CompileAndCreate(g_compositor_frag_glsl, ShaderStage::FRAGMENT, "compositor_frag");
    if (frag_module_ == VK_NULL_HANDLE) {
        Debug::Log("VulkanDualCompositor: Failed to compile fragment shader");
        Shutdown();
        return false;
    }

    if (!CreateSampler()) { Shutdown(); return false; }
    if (!CreateRenderPass()) { Shutdown(); return false; }
    if (!CreateDescriptorSetLayout()) { Shutdown(); return false; }
    if (!CreatePipelineLayout()) { Shutdown(); return false; }
    if (!CreateGraphicsPipeline()) { Shutdown(); return false; }
    if (!CreateDescriptorPool()) { Shutdown(); return false; }
    if (!CreateTransparentTexture()) { Shutdown(); return false; }

    // Allocate descriptor set
    {
        VkDescriptorSetAllocateInfo alloc_info{};
        alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc_info.descriptorPool = desc_pool_;
        alloc_info.descriptorSetCount = 1;
        alloc_info.pSetLayouts = &desc_layout_;

        if (vkAllocateDescriptorSets(dev.GetDevice(), &alloc_info, &desc_set_) != VK_SUCCESS) {
            Debug::Log("VulkanDualCompositor: Failed to allocate descriptor set");
            Shutdown();
            return false;
        }
    }

    initialized_ = true;
    Debug::Log("VulkanDualCompositor: Initialized");
    return true;
}

void VulkanDualCompositor::Shutdown() {
    auto& dev = VulkanDeviceManager::Instance();
    VkDevice device = dev.GetDevice();

    if (device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device);

        // Destroy cached framebuffers
        for (auto& fb : cached_framebuffers_) {
            if (fb.framebuffer != VK_NULL_HANDLE)
                vkDestroyFramebuffer(device, fb.framebuffer, nullptr);
        }
        cached_framebuffers_.clear();

        DestroyOutputImage();

        // Destroy transparent texture
        if (transparent_view_ != VK_NULL_HANDLE)
            vkDestroyImageView(device, transparent_view_, nullptr);
        if (transparent_image_ != VK_NULL_HANDLE)
            vmaDestroyImage(dev.GetAllocator(), transparent_image_, transparent_alloc_);

        // Destroy pipeline resources
        if (desc_pool_ != VK_NULL_HANDLE)
            vkDestroyDescriptorPool(device, desc_pool_, nullptr);
        if (pipeline_ != VK_NULL_HANDLE)
            vkDestroyPipeline(device, pipeline_, nullptr);
        if (pipeline_layout_ != VK_NULL_HANDLE)
            vkDestroyPipelineLayout(device, pipeline_layout_, nullptr);
        if (desc_layout_ != VK_NULL_HANDLE)
            vkDestroyDescriptorSetLayout(device, desc_layout_, nullptr);
        if (render_pass_ != VK_NULL_HANDLE)
            vkDestroyRenderPass(device, render_pass_, nullptr);
        if (sampler_ != VK_NULL_HANDLE)
            vkDestroySampler(device, sampler_, nullptr);
        if (vert_module_ != VK_NULL_HANDLE)
            vkDestroyShaderModule(device, vert_module_, nullptr);
        if (frag_module_ != VK_NULL_HANDLE)
            vkDestroyShaderModule(device, frag_module_, nullptr);
    }

    transparent_view_ = VK_NULL_HANDLE;
    transparent_image_ = VK_NULL_HANDLE;
    transparent_alloc_ = VK_NULL_HANDLE;
    desc_pool_ = VK_NULL_HANDLE;
    desc_set_ = VK_NULL_HANDLE;
    pipeline_ = VK_NULL_HANDLE;
    pipeline_layout_ = VK_NULL_HANDLE;
    desc_layout_ = VK_NULL_HANDLE;
    render_pass_ = VK_NULL_HANDLE;
    sampler_ = VK_NULL_HANDLE;
    vert_module_ = VK_NULL_HANDLE;
    frag_module_ = VK_NULL_HANDLE;

    output_pool_id_ = 0;
    initialized_ = false;
}

//=============================================================================
// Resource Creation
//=============================================================================

bool VulkanDualCompositor::CreateRenderPass() {
    VkAttachmentDescription color_attachment{};
    color_attachment.format = VK_FORMAT_R16G16B16A16_SFLOAT;  // RGBA16F
    color_attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;    // Clear to transparent
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
    dependency.srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rp_info{};
    rp_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rp_info.attachmentCount = 1;
    rp_info.pAttachments = &color_attachment;
    rp_info.subpassCount = 1;
    rp_info.pSubpasses = &subpass;
    rp_info.dependencyCount = 1;
    rp_info.pDependencies = &dependency;

    auto device = VulkanDeviceManager::Instance().GetDevice();
    if (vkCreateRenderPass(device, &rp_info, nullptr, &render_pass_) != VK_SUCCESS) {
        Debug::Log("VulkanDualCompositor: Failed to create render pass");
        return false;
    }

    return true;
}

bool VulkanDualCompositor::CreateDescriptorSetLayout() {
    // Binding 0: texLeft (combined image sampler)
    // Binding 1: texRight (combined image sampler)
    std::array<VkDescriptorSetLayoutBinding, 2> bindings{};

    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_info.bindingCount = static_cast<uint32_t>(bindings.size());
    layout_info.pBindings = bindings.data();

    auto device = VulkanDeviceManager::Instance().GetDevice();
    if (vkCreateDescriptorSetLayout(device, &layout_info, nullptr, &desc_layout_) != VK_SUCCESS) {
        Debug::Log("VulkanDualCompositor: Failed to create descriptor set layout");
        return false;
    }

    return true;
}

bool VulkanDualCompositor::CreatePipelineLayout() {
    // Push constant range for composite params
    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    push_range.offset = 0;
    push_range.size = sizeof(CompositePushConstants);

    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &desc_layout_;
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &push_range;

    auto device = VulkanDeviceManager::Instance().GetDevice();
    if (vkCreatePipelineLayout(device, &layout_info, nullptr, &pipeline_layout_) != VK_SUCCESS) {
        Debug::Log("VulkanDualCompositor: Failed to create pipeline layout");
        return false;
    }

    return true;
}

bool VulkanDualCompositor::CreateGraphicsPipeline() {
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

    VkPipelineShaderStageCreateInfo stages[] = { vert_stage, frag_stage };

    // No vertex input (fullscreen triangle generated in shader)
    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

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
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState blend_attachment{};
    blend_attachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    blend_attachment.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo color_blending{};
    color_blending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    color_blending.attachmentCount = 1;
    color_blending.pAttachments = &blend_attachment;

    VkDynamicState dynamic_states[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
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
    pipeline_info.renderPass = render_pass_;
    pipeline_info.subpass = 0;

    auto cache = VulkanDeviceManager::Instance().GetPipelineCache();
    if (vkCreateGraphicsPipelines(device, cache, 1, &pipeline_info, nullptr, &pipeline_) != VK_SUCCESS) {
        Debug::Log("VulkanDualCompositor: Failed to create graphics pipeline");
        return false;
    }

    return true;
}

bool VulkanDualCompositor::CreateSampler() {
    VkSamplerCreateInfo sampler_info{};
    sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_info.magFilter = VK_FILTER_LINEAR;
    sampler_info.minFilter = VK_FILTER_LINEAR;
    sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.maxAnisotropy = 1.0f;
    sampler_info.maxLod = VK_LOD_CLAMP_NONE;

    auto device = VulkanDeviceManager::Instance().GetDevice();
    if (vkCreateSampler(device, &sampler_info, nullptr, &sampler_) != VK_SUCCESS) {
        Debug::Log("VulkanDualCompositor: Failed to create sampler");
        return false;
    }

    return true;
}

bool VulkanDualCompositor::CreateDescriptorPool() {
    VkDescriptorPoolSize pool_size{};
    pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_size.descriptorCount = 2;  // left + right textures

    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &pool_size;
    pool_info.maxSets = 1;

    auto device = VulkanDeviceManager::Instance().GetDevice();
    if (vkCreateDescriptorPool(device, &pool_info, nullptr, &desc_pool_) != VK_SUCCESS) {
        Debug::Log("VulkanDualCompositor: Failed to create descriptor pool");
        return false;
    }

    return true;
}

bool VulkanDualCompositor::CreateTransparentTexture() {
    auto& dev = VulkanDeviceManager::Instance();

    // Create 1x1 transparent RGBA16F texture
    VkImageCreateInfo img_info{};
    img_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    img_info.imageType = VK_IMAGE_TYPE_2D;
    img_info.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    img_info.extent = {1, 1, 1};
    img_info.mipLevels = 1;
    img_info.arrayLayers = 1;
    img_info.samples = VK_SAMPLE_COUNT_1_BIT;
    img_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    img_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    img_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo alloc_info{};
    alloc_info.usage = VMA_MEMORY_USAGE_GPU_ONLY;

    if (vmaCreateImage(dev.GetAllocator(), &img_info, &alloc_info,
                       &transparent_image_, &transparent_alloc_, nullptr) != VK_SUCCESS) {
        Debug::Log("VulkanDualCompositor: Failed to create transparent image");
        return false;
    }

    // Upload transparent pixel and transition to SHADER_READ_ONLY
    {
        // Create staging buffer with transparent pixel data
        float transparent_pixel[4] = {0.0f, 0.0f, 0.0f, 0.0f};

        VkBufferCreateInfo buf_info{};
        buf_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buf_info.size = sizeof(transparent_pixel);
        buf_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

        VmaAllocationCreateInfo staging_alloc_info{};
        staging_alloc_info.usage = VMA_MEMORY_USAGE_CPU_ONLY;
        staging_alloc_info.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;

        VkBuffer staging_buffer = VK_NULL_HANDLE;
        VmaAllocation staging_alloc = VK_NULL_HANDLE;
        VmaAllocationInfo staging_info{};

        vmaCreateBuffer(dev.GetAllocator(), &buf_info, &staging_alloc_info,
                        &staging_buffer, &staging_alloc, &staging_info);
        std::memcpy(staging_info.pMappedData, transparent_pixel, sizeof(transparent_pixel));

        VkCommandBuffer cmd = dev.BeginSingleTimeCommands();

        // Transition to TRANSFER_DST
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = transparent_image_;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.layerCount = 1;
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &barrier);

        // Copy buffer to image
        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = {1, 1, 1};
        vkCmdCopyBufferToImage(cmd, staging_buffer, transparent_image_,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        // Transition to SHADER_READ_ONLY
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &barrier);

        dev.EndSingleTimeCommands(cmd);
        vmaDestroyBuffer(dev.GetAllocator(), staging_buffer, staging_alloc);
    }

    // Create image view
    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = transparent_image_;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.layerCount = 1;

    if (vkCreateImageView(dev.GetDevice(), &view_info, nullptr, &transparent_view_) != VK_SUCCESS) {
        Debug::Log("VulkanDualCompositor: Failed to create transparent image view");
        return false;
    }

    return true;
}

//=============================================================================
// Output Image Management
//=============================================================================

bool VulkanDualCompositor::CreateOutputImage(int width, int height) {
    if (output_image_ != VK_NULL_HANDLE &&
        output_width_ == width && output_height_ == height) {
        return true;  // Already correct size
    }

    // Delete old pool texture if it exists
    if (output_pool_id_ != 0) {
        VulkanTexturePool::Instance().QueueDelete(output_pool_id_);
        output_pool_id_ = 0;
    }

    DestroyOutputImage();

    auto& dev = VulkanDeviceManager::Instance();

    // Create RGBA16F image with COLOR_ATTACHMENT + SAMPLED + TRANSFER_SRC usage
    VkImageCreateInfo img_info{};
    img_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    img_info.imageType = VK_IMAGE_TYPE_2D;
    img_info.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    img_info.extent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};
    img_info.mipLevels = 1;
    img_info.arrayLayers = 1;
    img_info.samples = VK_SAMPLE_COUNT_1_BIT;
    img_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    img_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                     VK_IMAGE_USAGE_SAMPLED_BIT |
                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    img_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    img_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo alloc_info{};
    alloc_info.usage = VMA_MEMORY_USAGE_GPU_ONLY;

    if (vmaCreateImage(dev.GetAllocator(), &img_info, &alloc_info,
                       &output_image_, &output_alloc_, nullptr) != VK_SUCCESS) {
        Debug::Log("VulkanDualCompositor: Failed to create output image " +
                   std::to_string(width) + "x" + std::to_string(height));
        return false;
    }

    // Create image view
    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = output_image_;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.layerCount = 1;

    if (vkCreateImageView(dev.GetDevice(), &view_info, nullptr, &output_view_) != VK_SUCCESS) {
        Debug::Log("VulkanDualCompositor: Failed to create output image view");
        vmaDestroyImage(dev.GetAllocator(), output_image_, output_alloc_);
        output_image_ = VK_NULL_HANDLE;
        output_alloc_ = VK_NULL_HANDLE;
        return false;
    }

    // Transition to COLOR_ATTACHMENT_OPTIMAL for first use
    VkCommandBuffer cmd = dev.BeginSingleTimeCommands();
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = output_image_;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);
    dev.EndSingleTimeCommands(cmd);

    output_width_ = width;
    output_height_ = height;

    Debug::Log("VulkanDualCompositor: Created output image " +
               std::to_string(width) + "x" + std::to_string(height));
    return true;
}

void VulkanDualCompositor::DestroyOutputImage() {
    auto& dev = VulkanDeviceManager::Instance();
    VkDevice device = dev.GetDevice();

    if (output_view_ != VK_NULL_HANDLE) {
        vkDestroyImageView(device, output_view_, nullptr);
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
// Per-Render Helpers
//=============================================================================

VkFramebuffer VulkanDualCompositor::GetOrCreateFramebuffer(VkImageView view, int width, int height) {
    // Check cache for matching dimensions
    for (auto& fb : cached_framebuffers_) {
        if (fb.width == width && fb.height == height) {
            auto device = VulkanDeviceManager::Instance().GetDevice();
            vkDestroyFramebuffer(device, fb.framebuffer, nullptr);

            VkFramebufferCreateInfo fb_info{};
            fb_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            fb_info.renderPass = render_pass_;
            fb_info.attachmentCount = 1;
            fb_info.pAttachments = &view;
            fb_info.width = width;
            fb_info.height = height;
            fb_info.layers = 1;

            vkCreateFramebuffer(device, &fb_info, nullptr, &fb.framebuffer);
            return fb.framebuffer;
        }
    }

    // Create new
    auto device = VulkanDeviceManager::Instance().GetDevice();

    VkFramebufferCreateInfo fb_info{};
    fb_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fb_info.renderPass = render_pass_;
    fb_info.attachmentCount = 1;
    fb_info.pAttachments = &view;
    fb_info.width = width;
    fb_info.height = height;
    fb_info.layers = 1;

    CachedFramebuffer cached{};
    cached.width = width;
    cached.height = height;
    vkCreateFramebuffer(device, &fb_info, nullptr, &cached.framebuffer);
    cached_framebuffers_.push_back(cached);

    return cached.framebuffer;
}

void VulkanDualCompositor::UpdateDescriptorSet(VkImageView left_view, VkImageView right_view) {
    auto device = VulkanDeviceManager::Instance().GetDevice();

    std::array<VkDescriptorImageInfo, 2> image_infos{};
    image_infos[0].sampler = sampler_;
    image_infos[0].imageView = left_view;
    image_infos[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    image_infos[1].sampler = sampler_;
    image_infos[1].imageView = right_view;
    image_infos[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    std::array<VkWriteDescriptorSet, 2> writes{};
    for (int i = 0; i < 2; i++) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = desc_set_;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &image_infos[i];
    }

    vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()),
                           writes.data(), 0, nullptr);
}

//=============================================================================
// Compositing
//=============================================================================

uint64_t VulkanDualCompositor::Composite(uint64_t left_pool_id, uint64_t right_pool_id,
                                          const DualViewLayout& layout) {
    if (!initialized_) return 0;
    if (layout.composite_width <= 0 || layout.composite_height <= 0) return 0;

    // Create/resize output image if needed
    if (!CreateOutputImage(layout.composite_width, layout.composite_height)) {
        return 0;
    }

    // Get source texture views from pool
    auto& pool = VulkanTexturePool::Instance();
    VkImageView left_view = transparent_view_;
    VkImageView right_view = transparent_view_;

    if (left_pool_id != 0) {
        const auto* left_tex = pool.GetTexture(left_pool_id);
        if (left_tex && left_tex->is_valid) {
            left_view = left_tex->image_view;
        }
    }

    if (right_pool_id != 0) {
        const auto* right_tex = pool.GetTexture(right_pool_id);
        if (right_tex && right_tex->is_valid) {
            right_view = right_tex->image_view;
        }
    }

    // Update descriptor set with source textures
    UpdateDescriptorSet(left_view, right_view);

    // Prepare push constants
    CompositePushConstants pc{};
    pc.horizontal = layout.horizontal ? 1.0f : 0.0f;
    pc.hasLeft = (left_pool_id != 0 && left_view != transparent_view_) ? 1.0f : 0.0f;
    pc.hasRight = (right_pool_id != 0 && right_view != transparent_view_) ? 1.0f : 0.0f;

    float invW = 1.0f / static_cast<float>(layout.composite_width);
    float invH = 1.0f / static_cast<float>(layout.composite_height);

    pc.leftRect[0] = static_cast<float>(layout.left_x) * invW;
    pc.leftRect[1] = static_cast<float>(layout.left_y) * invH;
    pc.leftRect[2] = static_cast<float>(layout.left_w) * invW;
    pc.leftRect[3] = static_cast<float>(layout.left_h) * invH;

    pc.rightRect[0] = static_cast<float>(layout.right_x) * invW;
    pc.rightRect[1] = static_cast<float>(layout.right_y) * invH;
    pc.rightRect[2] = static_cast<float>(layout.right_w) * invW;
    pc.rightRect[3] = static_cast<float>(layout.right_h) * invH;

    // Get framebuffer
    VkFramebuffer fb = GetOrCreateFramebuffer(output_view_, output_width_, output_height_);
    if (fb == VK_NULL_HANDLE) return 0;

    auto& dev = VulkanDeviceManager::Instance();

    // Record and execute render commands
    VkCommandBuffer cmd = dev.BeginSingleTimeCommands();

    // Begin render pass (loadOp=CLEAR clears to transparent)
    VkRenderPassBeginInfo rp_begin{};
    rp_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp_begin.renderPass = render_pass_;
    rp_begin.framebuffer = fb;
    rp_begin.renderArea.extent = {static_cast<uint32_t>(output_width_),
                                   static_cast<uint32_t>(output_height_)};
    VkClearValue clear_value{};
    clear_value.color = {{0.0f, 0.0f, 0.0f, 0.0f}};  // Transparent
    rp_begin.clearValueCount = 1;
    rp_begin.pClearValues = &clear_value;

    vkCmdBeginRenderPass(cmd, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);

    // Bind pipeline
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);

    // Set viewport and scissor
    VkViewport viewport{};
    viewport.width = static_cast<float>(output_width_);
    viewport.height = static_cast<float>(output_height_);
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.extent = {static_cast<uint32_t>(output_width_),
                      static_cast<uint32_t>(output_height_)};
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    // Bind descriptor set
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipeline_layout_, 0, 1, &desc_set_, 0, nullptr);

    // Push constants
    vkCmdPushConstants(cmd, pipeline_layout_, VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(CompositePushConstants), &pc);

    // Draw fullscreen triangle
    vkCmdDraw(cmd, 3, 1, 0, 0);

    vkCmdEndRenderPass(cmd);

    dev.EndSingleTimeCommands(cmd);

    // Register output in pool (first time) or update (subsequent calls)
    // UpdateTextureFromImage can fail if the pool evicted our texture during
    // memory pressure (e.g., scrubbing creates many new textures rapidly).
    // In that case, fall back to creating a fresh pool entry.
    if (output_pool_id_ != 0) {
        if (!pool.UpdateTextureFromImage(output_pool_id_, output_image_,
                                         output_width_, output_height_)) {
            // Pool texture was evicted — clear and recreate below
            output_pool_id_ = 0;
        }
    }
    if (output_pool_id_ == 0) {
        output_pool_id_ = pool.CreateTextureFromImage(
            output_image_, output_width_, output_height_,
            VK_FORMAT_R16G16B16A16_SFLOAT);
    }

    // CreateTextureFromImage/UpdateTextureFromImage leave the source image in
    // TRANSFER_SRC_OPTIMAL. Transition back to COLOR_ATTACHMENT_OPTIMAL so the
    // next Composite() render pass finds the expected layout.
    {
        VkCommandBuffer cmd2 = dev.BeginSingleTimeCommands();
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = output_image_;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.layerCount = 1;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        vkCmdPipelineBarrier(cmd2,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);
        dev.EndSingleTimeCommands(cmd2);
    }

    current_layout_ = layout;
    return output_pool_id_;
}

} // namespace qcview

#endif // __linux__
