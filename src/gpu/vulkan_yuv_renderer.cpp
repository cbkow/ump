#include "vulkan_yuv_renderer.h"

#ifdef __linux__

#include "vulkan_device.h"
#include "vulkan_shader_compiler.h"
#include "../utils/debug_utils.h"

#include <cstring>
#include <array>

namespace qcview {

//=============================================================================
// Embedded Shader Sources
//=============================================================================

static const char* g_yuv_vert_glsl = R"(
#version 450

layout(location = 0) out vec2 v_uv;

void main() {
    vec2 pos = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
    v_uv = pos;
}
)";

static const char* g_yuv_frag_glsl = R"(
#version 450

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

layout(std140, set = 0, binding = 0) uniform YUVParams {
    float bitDepth;
    float applyPQ;
    float isFullRange;
    float colorSpace;
    float planeCount;
    float hasAlpha;
    float isRGBPlanar;
    float bitDepthScale;
};

layout(set = 0, binding = 1) uniform sampler2D texPlane0;
layout(set = 0, binding = 2) uniform sampler2D texPlane1_NV12;
layout(set = 0, binding = 3) uniform sampler2D texPlane1_U;
layout(set = 0, binding = 4) uniform sampler2D texPlane2_V;
layout(set = 0, binding = 5) uniform sampler2D texPlane3_A;

const mat3 BT709_MAT = mat3(
    1.0,      1.0,      1.0,
    0.0,     -0.18732,  1.8556,
    1.5748,  -0.46812,  0.0
);

const mat3 BT2020_MAT = mat3(
    1.0,      1.0,      1.0,
    0.0,     -0.16455,  1.8814,
    1.4746,  -0.57135,  0.0
);

void main() {
    vec2 uv = v_uv;
    vec3 rgb;
    float A = 1.0;

    if (isRGBPlanar > 0.5) {
        float G = texture(texPlane0, uv).r * bitDepthScale;
        float B = texture(texPlane1_U, uv).r * bitDepthScale;
        float R = texture(texPlane2_V, uv).r * bitDepthScale;
        rgb = vec3(R, G, B);
        if (hasAlpha > 0.5) {
            A = texture(texPlane3_A, uv).r * bitDepthScale;
        }
    } else {
        float Y, U, V;
        bool needsScaling = false;

        if (planeCount < 2.5) {
            Y = texture(texPlane0, uv).r;
            vec2 UV = texture(texPlane1_NV12, uv).rg;
            U = UV.x;
            V = UV.y;
            needsScaling = false;
        } else {
            Y = texture(texPlane0, uv).r;
            U = texture(texPlane1_U, uv).r;
            V = texture(texPlane2_V, uv).r;
            needsScaling = (bitDepthScale > 1.5);
        }

        if (needsScaling) {
            Y = Y * bitDepthScale;
            U = U * bitDepthScale;
            V = V * bitDepthScale;
        }

        if (isFullRange < 0.5) {
            const float foot = 16.0 / 255.0;
            const float yHead = 235.0 / 255.0;
            const float uvHead = 240.0 / 255.0;
            Y = clamp((Y - foot) / (yHead - foot), 0.0, 1.0);
            U = clamp((U - foot) / (uvHead - foot), 0.0, 1.0);
            V = clamp((V - foot) / (uvHead - foot), 0.0, 1.0);
        }

        U = U - 0.5;
        V = V - 0.5;

        mat3 colorMat = (colorSpace > 0.5) ? BT2020_MAT : BT709_MAT;
        rgb = colorMat * vec3(Y, U, V);

        if (hasAlpha > 0.5) {
            float rawA = texture(texPlane3_A, uv).r;
            A = (bitDepthScale > 1.5) ? rawA * bitDepthScale : rawA;
        }
    }

    out_color = vec4(clamp(rgb, 0.0, 1.0), A);
}
)";

//=============================================================================
// UBO Layout (must match shader's std140)
//=============================================================================

struct YUVUniformData {
    float bitDepth;
    float applyPQ;
    float isFullRange;
    float colorSpace;
    float planeCount;
    float hasAlpha;
    float isRGBPlanar;
    float bitDepthScale;
};

//=============================================================================
// Implementation
//=============================================================================

VulkanYUVRenderer::VulkanYUVRenderer() = default;

VulkanYUVRenderer::~VulkanYUVRenderer() {
    Shutdown();
}

bool VulkanYUVRenderer::Initialize() {
    if (initialized_) return true;

    auto& dev = VulkanDeviceManager::Instance();
    if (!dev.IsInitialized()) {
        Debug::Log("VulkanYUVRenderer: Device not initialized");
        return false;
    }

    // Compile shaders
    VulkanShaderCompiler compiler;

    vert_module_ = compiler.CompileAndCreate(g_yuv_vert_glsl, ShaderStage::VERTEX, "yuv_vert");
    if (vert_module_ == VK_NULL_HANDLE) {
        Debug::Log("VulkanYUVRenderer: Failed to compile vertex shader");
        return false;
    }

    frag_module_ = compiler.CompileAndCreate(g_yuv_frag_glsl, ShaderStage::FRAGMENT, "yuv_frag");
    if (frag_module_ == VK_NULL_HANDLE) {
        Debug::Log("VulkanYUVRenderer: Failed to compile fragment shader");
        Shutdown();
        return false;
    }

    if (!CreateSampler()) { Shutdown(); return false; }
    if (!CreateRenderPass()) { Shutdown(); return false; }
    if (!CreateDescriptorSetLayout()) { Shutdown(); return false; }
    if (!CreatePipelineLayout()) { Shutdown(); return false; }
    if (!CreateGraphicsPipeline()) { Shutdown(); return false; }
    if (!CreateDescriptorPool()) { Shutdown(); return false; }
    if (!CreateUniformBuffer()) { Shutdown(); return false; }

    // Create dummy 1x1 texture for unused plane slots
    {
        VkImageCreateInfo img_info{};
        img_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        img_info.imageType = VK_IMAGE_TYPE_2D;
        img_info.format = VK_FORMAT_R8_UNORM;
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
                           &dummy_image_, &dummy_allocation_, nullptr) != VK_SUCCESS) {
            Debug::Log("VulkanYUVRenderer: Failed to create dummy image");
            Shutdown();
            return false;
        }

        // Transition to SHADER_READ_ONLY
        VkCommandBuffer cmd = dev.BeginSingleTimeCommands();
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = dummy_image_;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.layerCount = 1;
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &barrier);
        dev.EndSingleTimeCommands(cmd);

        VkImageViewCreateInfo view_info{};
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image = dummy_image_;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = VK_FORMAT_R8_UNORM;
        view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view_info.subresourceRange.levelCount = 1;
        view_info.subresourceRange.layerCount = 1;

        if (vkCreateImageView(dev.GetDevice(), &view_info, nullptr, &dummy_view_) != VK_SUCCESS) {
            Debug::Log("VulkanYUVRenderer: Failed to create dummy image view");
            Shutdown();
            return false;
        }
    }

    // Allocate descriptor set
    {
        VkDescriptorSetAllocateInfo alloc_info{};
        alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc_info.descriptorPool = descriptor_pool_;
        alloc_info.descriptorSetCount = 1;
        alloc_info.pSetLayouts = &descriptor_set_layout_;

        if (vkAllocateDescriptorSets(dev.GetDevice(), &alloc_info, &descriptor_set_) != VK_SUCCESS) {
            Debug::Log("VulkanYUVRenderer: Failed to allocate descriptor set");
            Shutdown();
            return false;
        }
    }

    initialized_ = true;
    Debug::Log("VulkanYUVRenderer: Initialized");
    return true;
}

void VulkanYUVRenderer::Shutdown() {
    auto& dev = VulkanDeviceManager::Instance();
    VkDevice device = dev.GetDevice();

    if (device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device);

        for (auto& fb : cached_framebuffers_) {
            if (fb.framebuffer != VK_NULL_HANDLE)
                vkDestroyFramebuffer(device, fb.framebuffer, nullptr);
        }
        cached_framebuffers_.clear();

        if (dummy_view_ != VK_NULL_HANDLE)
            vkDestroyImageView(device, dummy_view_, nullptr);
        if (dummy_image_ != VK_NULL_HANDLE)
            vmaDestroyImage(dev.GetAllocator(), dummy_image_, dummy_allocation_);

        if (uniform_buffer_ != VK_NULL_HANDLE)
            vmaDestroyBuffer(dev.GetAllocator(), uniform_buffer_, uniform_allocation_);

        if (descriptor_pool_ != VK_NULL_HANDLE)
            vkDestroyDescriptorPool(device, descriptor_pool_, nullptr);
        if (graphics_pipeline_ != VK_NULL_HANDLE)
            vkDestroyPipeline(device, graphics_pipeline_, nullptr);
        if (pipeline_layout_ != VK_NULL_HANDLE)
            vkDestroyPipelineLayout(device, pipeline_layout_, nullptr);
        if (descriptor_set_layout_ != VK_NULL_HANDLE)
            vkDestroyDescriptorSetLayout(device, descriptor_set_layout_, nullptr);
        if (render_pass_ != VK_NULL_HANDLE)
            vkDestroyRenderPass(device, render_pass_, nullptr);
        if (sampler_ != VK_NULL_HANDLE)
            vkDestroySampler(device, sampler_, nullptr);
        if (vert_module_ != VK_NULL_HANDLE)
            vkDestroyShaderModule(device, vert_module_, nullptr);
        if (frag_module_ != VK_NULL_HANDLE)
            vkDestroyShaderModule(device, frag_module_, nullptr);
    }

    dummy_view_ = VK_NULL_HANDLE;
    dummy_image_ = VK_NULL_HANDLE;
    dummy_allocation_ = VK_NULL_HANDLE;
    uniform_buffer_ = VK_NULL_HANDLE;
    uniform_allocation_ = VK_NULL_HANDLE;
    uniform_mapped_ = nullptr;
    descriptor_pool_ = VK_NULL_HANDLE;
    descriptor_set_ = VK_NULL_HANDLE;
    graphics_pipeline_ = VK_NULL_HANDLE;
    pipeline_layout_ = VK_NULL_HANDLE;
    descriptor_set_layout_ = VK_NULL_HANDLE;
    render_pass_ = VK_NULL_HANDLE;
    sampler_ = VK_NULL_HANDLE;
    vert_module_ = VK_NULL_HANDLE;
    frag_module_ = VK_NULL_HANDLE;

    initialized_ = false;
}

//=============================================================================
// Resource Creation
//=============================================================================

bool VulkanYUVRenderer::CreateRenderPass() {
    VkAttachmentDescription color_attachment{};
    color_attachment.format = VK_FORMAT_R16G16B16A16_SFLOAT;  // RGBA16F
    color_attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color_attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color_attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color_attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;  // Don't care about previous contents
    color_attachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

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
        Debug::Log("VulkanYUVRenderer: Failed to create render pass");
        return false;
    }

    return true;
}

bool VulkanYUVRenderer::CreateDescriptorSetLayout() {
    // Binding 0: UBO (YUVParams)
    // Binding 1-5: Combined image samplers (5 texture planes)
    std::array<VkDescriptorSetLayoutBinding, 6> bindings{};

    // UBO
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    // Texture planes
    for (int i = 1; i <= 5; i++) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }

    VkDescriptorSetLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_info.bindingCount = static_cast<uint32_t>(bindings.size());
    layout_info.pBindings = bindings.data();

    auto device = VulkanDeviceManager::Instance().GetDevice();
    if (vkCreateDescriptorSetLayout(device, &layout_info, nullptr, &descriptor_set_layout_) != VK_SUCCESS) {
        Debug::Log("VulkanYUVRenderer: Failed to create descriptor set layout");
        return false;
    }

    return true;
}

bool VulkanYUVRenderer::CreatePipelineLayout() {
    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &descriptor_set_layout_;

    auto device = VulkanDeviceManager::Instance().GetDevice();
    if (vkCreatePipelineLayout(device, &layout_info, nullptr, &pipeline_layout_) != VK_SUCCESS) {
        Debug::Log("VulkanYUVRenderer: Failed to create pipeline layout");
        return false;
    }

    return true;
}

bool VulkanYUVRenderer::CreateGraphicsPipeline() {
    auto device = VulkanDeviceManager::Instance().GetDevice();

    // Shader stages
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

    // Dynamic viewport and scissor
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
    if (vkCreateGraphicsPipelines(device, cache, 1, &pipeline_info, nullptr, &graphics_pipeline_) != VK_SUCCESS) {
        Debug::Log("VulkanYUVRenderer: Failed to create graphics pipeline");
        return false;
    }

    return true;
}

bool VulkanYUVRenderer::CreateSampler() {
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
        Debug::Log("VulkanYUVRenderer: Failed to create sampler");
        return false;
    }

    return true;
}

bool VulkanYUVRenderer::CreateDescriptorPool() {
    std::array<VkDescriptorPoolSize, 2> pool_sizes{};
    pool_sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    pool_sizes[0].descriptorCount = 1;
    pool_sizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_sizes[1].descriptorCount = 5;  // 5 plane textures

    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.maxSets = 1;
    pool_info.poolSizeCount = static_cast<uint32_t>(pool_sizes.size());
    pool_info.pPoolSizes = pool_sizes.data();

    auto device = VulkanDeviceManager::Instance().GetDevice();
    if (vkCreateDescriptorPool(device, &pool_info, nullptr, &descriptor_pool_) != VK_SUCCESS) {
        Debug::Log("VulkanYUVRenderer: Failed to create descriptor pool");
        return false;
    }

    return true;
}

bool VulkanYUVRenderer::CreateUniformBuffer() {
    VkBufferCreateInfo buf_info{};
    buf_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buf_info.size = sizeof(YUVUniformData);
    buf_info.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;

    VmaAllocationCreateInfo alloc_info{};
    alloc_info.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
    alloc_info.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;

    VmaAllocationInfo alloc_result{};
    auto allocator = VulkanDeviceManager::Instance().GetAllocator();
    if (vmaCreateBuffer(allocator, &buf_info, &alloc_info,
                        &uniform_buffer_, &uniform_allocation_, &alloc_result) != VK_SUCCESS) {
        Debug::Log("VulkanYUVRenderer: Failed to create uniform buffer");
        return false;
    }

    uniform_mapped_ = alloc_result.pMappedData;
    return true;
}

//=============================================================================
// Per-Render Helpers
//=============================================================================

VkFramebuffer VulkanYUVRenderer::GetOrCreateFramebuffer(VkImageView view, int width, int height) {
    // Check cache
    for (auto& fb : cached_framebuffers_) {
        // Reuse if same dimensions (view may differ but framebuffer is compatible)
        if (fb.width == width && fb.height == height) {
            // Destroy old and recreate with new view
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

void VulkanYUVRenderer::UpdateUniformBuffer(const YUVRenderParams& params) {
    if (!uniform_mapped_) return;

    YUVUniformData data{};
    data.bitDepth = static_cast<float>(params.bit_depth);
    data.applyPQ = params.is_hdr ? 1.0f : 0.0f;
    // HDR PQ content is always full range
    data.isFullRange = (params.is_hdr || params.is_full_range) ? 1.0f : 0.0f;
    data.colorSpace = (params.color_space == YUVColorSpace::BT_2020) ? 1.0f : 0.0f;
    data.planeCount = static_cast<float>(params.plane_count);
    data.hasAlpha = params.has_alpha ? 1.0f : 0.0f;
    data.isRGBPlanar = params.is_rgb_planar ? 1.0f : 0.0f;

    if (params.bit_depth == 10) {
        data.bitDepthScale = 65535.0f / 1023.0f;
    } else if (params.bit_depth == 12) {
        data.bitDepthScale = 65535.0f / 4095.0f;
    } else {
        data.bitDepthScale = 1.0f;
    }

    std::memcpy(uniform_mapped_, &data, sizeof(data));
}

void VulkanYUVRenderer::UpdateDescriptorSet(const VkImageView* plane_views, int num_plane_views) {
    auto device = VulkanDeviceManager::Instance().GetDevice();

    // UBO write
    VkDescriptorBufferInfo buffer_info{};
    buffer_info.buffer = uniform_buffer_;
    buffer_info.offset = 0;
    buffer_info.range = sizeof(YUVUniformData);

    VkWriteDescriptorSet ubo_write{};
    ubo_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    ubo_write.dstSet = descriptor_set_;
    ubo_write.dstBinding = 0;
    ubo_write.descriptorCount = 1;
    ubo_write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    ubo_write.pBufferInfo = &buffer_info;

    // Texture writes (bindings 1-5)
    std::array<VkDescriptorImageInfo, 5> image_infos{};
    std::array<VkWriteDescriptorSet, 5> image_writes{};

    for (int i = 0; i < 5; i++) {
        image_infos[i].sampler = sampler_;
        image_infos[i].imageView = (i < num_plane_views && plane_views[i] != VK_NULL_HANDLE)
                                       ? plane_views[i]
                                       : dummy_view_;
        image_infos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        image_writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        image_writes[i].dstSet = descriptor_set_;
        image_writes[i].dstBinding = i + 1;
        image_writes[i].descriptorCount = 1;
        image_writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        image_writes[i].pImageInfo = &image_infos[i];
    }

    // Combine all writes
    std::array<VkWriteDescriptorSet, 6> all_writes{};
    all_writes[0] = ubo_write;
    for (int i = 0; i < 5; i++) {
        all_writes[i + 1] = image_writes[i];
    }

    vkUpdateDescriptorSets(device, static_cast<uint32_t>(all_writes.size()),
                           all_writes.data(), 0, nullptr);
}

//=============================================================================
// Rendering
//=============================================================================

bool VulkanYUVRenderer::Render(
    const VkImageView* plane_views,
    int num_plane_views,
    VkImage dest_image,
    VkImageView dest_view,
    int dest_width, int dest_height,
    const YUVRenderParams& params) {

    if (!initialized_) return false;

    auto& dev = VulkanDeviceManager::Instance();

    // Update UBO
    UpdateUniformBuffer(params);

    // Update descriptor set with current textures
    UpdateDescriptorSet(plane_views, num_plane_views);

    // Get or create framebuffer
    VkFramebuffer fb = GetOrCreateFramebuffer(dest_view, dest_width, dest_height);
    if (fb == VK_NULL_HANDLE) return false;

    // Record commands
    VkCommandBuffer cmd = dev.BeginSingleTimeCommands();

    // Begin render pass
    VkRenderPassBeginInfo rp_begin{};
    rp_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp_begin.renderPass = render_pass_;
    rp_begin.framebuffer = fb;
    rp_begin.renderArea.extent = {static_cast<uint32_t>(dest_width),
                                   static_cast<uint32_t>(dest_height)};

    vkCmdBeginRenderPass(cmd, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);

    // Bind pipeline
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, graphics_pipeline_);

    // Set viewport and scissor
    VkViewport viewport{};
    viewport.width = static_cast<float>(dest_width);
    viewport.height = static_cast<float>(dest_height);
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.extent = {static_cast<uint32_t>(dest_width),
                      static_cast<uint32_t>(dest_height)};
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    // Bind descriptor set
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipeline_layout_, 0, 1, &descriptor_set_, 0, nullptr);

    // Draw fullscreen triangle
    vkCmdDraw(cmd, 3, 1, 0, 0);

    vkCmdEndRenderPass(cmd);

    // Submit and wait
    dev.EndSingleTimeCommands(cmd);

    return true;
}

} // namespace qcview

#endif // __linux__
