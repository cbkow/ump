#include "vulkan_ocio_renderer.h"

#ifdef __linux__

#include "ocio_pipeline.h"
#include "../gpu/vulkan_device.h"
#include "../gpu/vulkan_shader_compiler.h"
#include "../utils/debug_utils.h"

#include <cstring>
#include <array>
#include <sstream>
#include <functional>

namespace qcview {

//=============================================================================
// Embedded Shader Sources
//=============================================================================

static const char* g_passthrough_vert_glsl = R"(
#version 450

layout(location = 0) out vec2 v_uv;

void main() {
    vec2 pos = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
    v_uv = pos;
}
)";

static const char* g_passthrough_frag_glsl = R"(
#version 450

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D inputTexture;

void main() {
    out_color = texture(inputTexture, v_uv);
}
)";

//=============================================================================
// Implementation
//=============================================================================

VulkanOCIORenderer::VulkanOCIORenderer() = default;

VulkanOCIORenderer::~VulkanOCIORenderer() {
    Shutdown();
}

bool VulkanOCIORenderer::Initialize() {
    if (initialized_) return true;

    auto& dev = VulkanDeviceManager::Instance();
    if (!dev.IsInitialized()) {
        Debug::Log("VulkanOCIORenderer: Device not initialized");
        return false;
    }

    if (!CreateSampler()) return false;
    if (!CreateRenderPass()) { Shutdown(); return false; }
    if (!CreateDescriptorPool()) { Shutdown(); return false; }
    if (!CreatePassthroughPipeline()) { Shutdown(); return false; }

    initialized_ = true;
    Debug::Log("VulkanOCIORenderer: Initialized");
    return true;
}

void VulkanOCIORenderer::Shutdown() {
    auto& dev = VulkanDeviceManager::Instance();
    VkDevice device = dev.GetDevice();

    if (device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device);

        DestroyOCIOPipelineState(ocio_state_);

        for (auto& fb : cached_framebuffers_) {
            if (fb.framebuffer != VK_NULL_HANDLE)
                vkDestroyFramebuffer(device, fb.framebuffer, nullptr);
        }
        cached_framebuffers_.clear();

        if (passthrough_pipeline_ != VK_NULL_HANDLE)
            vkDestroyPipeline(device, passthrough_pipeline_, nullptr);
        if (passthrough_pipe_layout_ != VK_NULL_HANDLE)
            vkDestroyPipelineLayout(device, passthrough_pipe_layout_, nullptr);
        if (passthrough_desc_layout_ != VK_NULL_HANDLE)
            vkDestroyDescriptorSetLayout(device, passthrough_desc_layout_, nullptr);
        if (passthrough_vert_ != VK_NULL_HANDLE)
            vkDestroyShaderModule(device, passthrough_vert_, nullptr);
        if (passthrough_frag_ != VK_NULL_HANDLE)
            vkDestroyShaderModule(device, passthrough_frag_, nullptr);
        if (descriptor_pool_ != VK_NULL_HANDLE)
            vkDestroyDescriptorPool(device, descriptor_pool_, nullptr);
        if (render_pass_ != VK_NULL_HANDLE)
            vkDestroyRenderPass(device, render_pass_, nullptr);
        if (linear_sampler_ != VK_NULL_HANDLE)
            vkDestroySampler(device, linear_sampler_, nullptr);
    }

    passthrough_pipeline_ = VK_NULL_HANDLE;
    passthrough_pipe_layout_ = VK_NULL_HANDLE;
    passthrough_desc_layout_ = VK_NULL_HANDLE;
    passthrough_desc_set_ = VK_NULL_HANDLE;
    passthrough_vert_ = VK_NULL_HANDLE;
    passthrough_frag_ = VK_NULL_HANDLE;
    descriptor_pool_ = VK_NULL_HANDLE;
    render_pass_ = VK_NULL_HANDLE;
    linear_sampler_ = VK_NULL_HANDLE;

    initialized_ = false;
}

void VulkanOCIORenderer::InvalidateOCIOPipeline() {
    if (ocio_state_.valid) {
        auto& dev = VulkanDeviceManager::Instance();
        if (dev.GetDevice() != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(dev.GetDevice());
        }
        DestroyOCIOPipelineState(ocio_state_);
        Debug::Log("VulkanOCIORenderer: OCIO pipeline invalidated");
    }
}

//=============================================================================
// Resource Creation
//=============================================================================

bool VulkanOCIORenderer::CreateRenderPass() {
    // Output can be RGBA8 or RGBA16F depending on pipeline mode
    // Use RGBA8 for the OCIO output (final display format)
    VkAttachmentDescription color_attachment{};
    color_attachment.format = VK_FORMAT_R8G8B8A8_UNORM;
    color_attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color_attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color_attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    // UNDEFINED: we always overwrite all pixels (fullscreen triangle + DONT_CARE load)
    // This handles both first-frame (image starts UNDEFINED) and subsequent frames
    // (image is SHADER_READ_ONLY after previous render pass)
    color_attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
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
        Debug::Log("VulkanOCIORenderer: Failed to create render pass");
        return false;
    }

    return true;
}

bool VulkanOCIORenderer::CreateSampler() {
    VkSamplerCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter = VK_FILTER_LINEAR;
    info.minFilter = VK_FILTER_LINEAR;
    info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.maxAnisotropy = 1.0f;
    info.maxLod = VK_LOD_CLAMP_NONE;

    auto device = VulkanDeviceManager::Instance().GetDevice();
    if (vkCreateSampler(device, &info, nullptr, &linear_sampler_) != VK_SUCCESS) {
        Debug::Log("VulkanOCIORenderer: Failed to create sampler");
        return false;
    }

    return true;
}

bool VulkanOCIORenderer::CreateDescriptorPool() {
    // Pool for passthrough + OCIO pipeline descriptors
    // Passthrough needs 1 combined_image_sampler
    // OCIO needs 1 combined_image_sampler (video) + up to 16 LUT textures
    std::array<VkDescriptorPoolSize, 1> pool_sizes{};
    pool_sizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_sizes[0].descriptorCount = 32;  // Generous for LUTs

    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pool_info.maxSets = 4;
    pool_info.poolSizeCount = static_cast<uint32_t>(pool_sizes.size());
    pool_info.pPoolSizes = pool_sizes.data();

    auto device = VulkanDeviceManager::Instance().GetDevice();
    if (vkCreateDescriptorPool(device, &pool_info, nullptr, &descriptor_pool_) != VK_SUCCESS) {
        Debug::Log("VulkanOCIORenderer: Failed to create descriptor pool");
        return false;
    }

    return true;
}

bool VulkanOCIORenderer::CreatePassthroughPipeline() {
    auto& dev = VulkanDeviceManager::Instance();
    VkDevice device = dev.GetDevice();

    VulkanShaderCompiler compiler;

    passthrough_vert_ = compiler.CompileAndCreate(
        g_passthrough_vert_glsl, ShaderStage::VERTEX, "ocio_passthrough_vert");
    if (passthrough_vert_ == VK_NULL_HANDLE) return false;

    passthrough_frag_ = compiler.CompileAndCreate(
        g_passthrough_frag_glsl, ShaderStage::FRAGMENT, "ocio_passthrough_frag");
    if (passthrough_frag_ == VK_NULL_HANDLE) return false;

    // Descriptor set layout: 1 combined image sampler (input texture)
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo desc_layout_info{};
    desc_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    desc_layout_info.bindingCount = 1;
    desc_layout_info.pBindings = &binding;

    if (vkCreateDescriptorSetLayout(device, &desc_layout_info, nullptr,
                                     &passthrough_desc_layout_) != VK_SUCCESS) {
        return false;
    }

    // Pipeline layout
    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &passthrough_desc_layout_;

    if (vkCreatePipelineLayout(device, &layout_info, nullptr,
                                &passthrough_pipe_layout_) != VK_SUCCESS) {
        return false;
    }

    // Graphics pipeline
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = passthrough_vert_;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = passthrough_frag_;
    stages[1].pName = "main";

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
    pipeline_info.layout = passthrough_pipe_layout_;
    pipeline_info.renderPass = render_pass_;
    pipeline_info.subpass = 0;

    auto cache = VulkanDeviceManager::Instance().GetPipelineCache();
    if (vkCreateGraphicsPipelines(device, cache, 1, &pipeline_info, nullptr,
                                   &passthrough_pipeline_) != VK_SUCCESS) {
        Debug::Log("VulkanOCIORenderer: Failed to create passthrough pipeline");
        return false;
    }

    // Allocate descriptor set for passthrough
    VkDescriptorSetAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc_info.descriptorPool = descriptor_pool_;
    alloc_info.descriptorSetCount = 1;
    alloc_info.pSetLayouts = &passthrough_desc_layout_;

    if (vkAllocateDescriptorSets(device, &alloc_info, &passthrough_desc_set_) != VK_SUCCESS) {
        Debug::Log("VulkanOCIORenderer: Failed to allocate passthrough descriptor set");
        return false;
    }

    return true;
}

//=============================================================================
// Framebuffer Cache
//=============================================================================

VkFramebuffer VulkanOCIORenderer::GetOrCreateFramebuffer(VkImageView view, int width, int height) {
    auto device = VulkanDeviceManager::Instance().GetDevice();

    // Check if we have one with matching dimensions (recreate with new view)
    for (auto& fb : cached_framebuffers_) {
        if (fb.width == width && fb.height == height) {
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

//=============================================================================
// OCIO Pipeline State
//=============================================================================

void VulkanOCIORenderer::DestroyOCIOPipelineState(OCIOPipelineState& state) {
    auto& dev = VulkanDeviceManager::Instance();
    VkDevice device = dev.GetDevice();

    if (device == VK_NULL_HANDLE) return;

    for (auto& lut : state.lut_textures) {
        if (lut.view != VK_NULL_HANDLE)
            vkDestroyImageView(device, lut.view, nullptr);
        if (lut.image != VK_NULL_HANDLE)
            vmaDestroyImage(dev.GetAllocator(), lut.image, lut.allocation);
        if (lut.sampler != VK_NULL_HANDLE && lut.sampler != linear_sampler_)
            vkDestroySampler(device, lut.sampler, nullptr);
    }
    state.lut_textures.clear();

    if (state.desc_set != VK_NULL_HANDLE) {
        vkFreeDescriptorSets(device, descriptor_pool_, 1, &state.desc_set);
    }
    if (state.pipeline != VK_NULL_HANDLE)
        vkDestroyPipeline(device, state.pipeline, nullptr);
    if (state.layout != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(device, state.layout, nullptr);
    if (state.desc_layout != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(device, state.desc_layout, nullptr);
    if (state.frag_module != VK_NULL_HANDLE)
        vkDestroyShaderModule(device, state.frag_module, nullptr);

    state = OCIOPipelineState{};
}

//=============================================================================
// Rendering
//=============================================================================

bool VulkanOCIORenderer::ApplyPassthrough(
    VkImageView source_view,
    VkImage dest_image,
    VkImageView dest_view,
    int width, int height) {

    if (!initialized_) return false;

    auto& dev = VulkanDeviceManager::Instance();
    VkDevice device = dev.GetDevice();

    // Update descriptor set with source texture
    VkDescriptorImageInfo img_info{};
    img_info.sampler = linear_sampler_;
    img_info.imageView = source_view;
    img_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = passthrough_desc_set_;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &img_info;

    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);

    // Get framebuffer
    VkFramebuffer fb = GetOrCreateFramebuffer(dest_view, width, height);
    if (fb == VK_NULL_HANDLE) return false;

    // Record commands
    VkCommandBuffer cmd = dev.BeginSingleTimeCommands();

    VkRenderPassBeginInfo rp_begin{};
    rp_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp_begin.renderPass = render_pass_;
    rp_begin.framebuffer = fb;
    rp_begin.renderArea.extent = {static_cast<uint32_t>(width),
                                   static_cast<uint32_t>(height)};

    vkCmdBeginRenderPass(cmd, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, passthrough_pipeline_);

    VkViewport viewport{};
    viewport.width = static_cast<float>(width);
    viewport.height = static_cast<float>(height);
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.extent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            passthrough_pipe_layout_, 0, 1, &passthrough_desc_set_, 0, nullptr);

    vkCmdDraw(cmd, 3, 1, 0, 0);

    vkCmdEndRenderPass(cmd);

    dev.EndSingleTimeCommands(cmd);

    return true;
}

bool VulkanOCIORenderer::Apply(
    ::OCIOPipeline* pipeline,
    VkImageView source_view,
    VkImage dest_image,
    VkImageView dest_view,
    int width, int height) {

    if (!initialized_ || !pipeline) return false;

    // If pipeline is passthrough, use the fast path
    if (pipeline->IsPassthrough()) {
        return ApplyPassthrough(source_view, dest_image, dest_view, width, height);
    }

    // Build/rebuild OCIO pipeline if needed
    if (!ocio_state_.valid) {
        if (!BuildOCIOPipelineState(pipeline)) {
            Debug::Log("VulkanOCIORenderer: Failed to build OCIO pipeline, falling back to passthrough");
            return ApplyPassthrough(source_view, dest_image, dest_view, width, height);
        }
    }

    auto& dev = VulkanDeviceManager::Instance();
    VkDevice device = dev.GetDevice();

    // Update descriptor set: binding 0 = source video texture
    VkDescriptorImageInfo src_img_info{};
    src_img_info.sampler = linear_sampler_;
    src_img_info.imageView = source_view;
    src_img_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = ocio_state_.desc_set;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &src_img_info;

    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);

    // Get framebuffer
    VkFramebuffer fb = GetOrCreateFramebuffer(dest_view, width, height);
    if (fb == VK_NULL_HANDLE) return false;

    // Record commands
    VkCommandBuffer cmd = dev.BeginSingleTimeCommands();

    VkRenderPassBeginInfo rp_begin{};
    rp_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp_begin.renderPass = render_pass_;
    rp_begin.framebuffer = fb;
    rp_begin.renderArea.extent = {static_cast<uint32_t>(width),
                                   static_cast<uint32_t>(height)};

    vkCmdBeginRenderPass(cmd, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ocio_state_.pipeline);

    VkViewport viewport{};
    viewport.width = static_cast<float>(width);
    viewport.height = static_cast<float>(height);
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.extent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            ocio_state_.layout, 0, 1, &ocio_state_.desc_set, 0, nullptr);

    vkCmdDraw(cmd, 3, 1, 0, 0);

    vkCmdEndRenderPass(cmd);

    dev.EndSingleTimeCommands(cmd);

    return true;
}

//=============================================================================
// OCIO Pipeline Building
//=============================================================================

std::string VulkanOCIORenderer::BuildOCIOFragmentGLSL(
    const std::string& ocio_function_glsl, int num_lut_bindings) {

    std::stringstream frag;
    frag << "#version 450\n\n";
    frag << "layout(location = 0) in vec2 v_uv;\n";
    frag << "layout(location = 0) out vec4 out_color;\n\n";

    // Binding 0 = video input texture
    frag << "layout(set = 0, binding = 0) uniform sampler2D videoTexture;\n\n";

    // OCIO generates uniform sampler declarations in its shader text.
    // We need to rewrite them to use Vulkan descriptor bindings.
    // Process the OCIO GLSL: find "uniform sampler" declarations and add layout qualifiers.
    std::string ocio_glsl = ocio_function_glsl;

    // The OCIO shader text contains lines like:
    //   uniform sampler2D ocio_lut1d_0Sampler;
    //   uniform sampler3D ocio_lut3d_0Sampler;
    // We need to prepend layout(set=0, binding=N) to each.
    // LUT bindings start at 1 (binding 0 is the video texture).
    int binding_index = 1;
    std::string processed_glsl;
    std::istringstream stream(ocio_glsl);
    std::string line;

    while (std::getline(stream, line)) {
        // Check if this line declares a uniform sampler
        size_t uniform_pos = line.find("uniform sampler");
        if (uniform_pos != std::string::npos) {
            // Insert layout qualifier before "uniform"
            std::string prefix = line.substr(0, uniform_pos);
            std::string remainder = line.substr(uniform_pos);
            processed_glsl += prefix + "layout(set = 0, binding = " +
                              std::to_string(binding_index) + ") " + remainder + "\n";
            binding_index++;
        } else {
            processed_glsl += line + "\n";
        }
    }

    // Add the processed OCIO code (with layout-qualified sampler declarations)
    frag << processed_glsl << "\n";

    // Main function: sample video texture, apply OCIO transform
    frag << "void main() {\n";
    frag << "    vec4 col = texture(videoTexture, v_uv);\n";
    frag << "    out_color = OCIODisplay(col);\n";
    frag << "}\n";

    return frag.str();
}

bool VulkanOCIORenderer::CreateLUTTexture(
    OCIOPipelineState::LUTTexture& lut,
    const float* data, unsigned width, unsigned height,
    bool is_3d, unsigned edge_len, bool is_red_channel) {

    auto& dev = VulkanDeviceManager::Instance();
    VkDevice device = dev.GetDevice();

    lut.is_3d = is_3d;

    // Determine format
    VkFormat format;
    if (is_3d) {
        format = VK_FORMAT_R32G32B32_SFLOAT;  // RGB32F for 3D LUTs
    } else {
        format = is_red_channel ? VK_FORMAT_R32_SFLOAT : VK_FORMAT_R32G32B32_SFLOAT;
    }

    // Note: RGB32F (3-component) may not be supported for sampling on all GPUs.
    // Use RGBA32F instead and expand the data.
    bool need_rgba_expand = false;
    VkFormat actual_format = format;

    // Check if RGB32F is supported for sampled images
    VkFormatProperties props;
    vkGetPhysicalDeviceFormatProperties(dev.GetPhysicalDevice(), format, &props);
    if (!(props.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) {
        // Fall back to RGBA32F
        actual_format = VK_FORMAT_R32G32B32A32_SFLOAT;
        need_rgba_expand = !is_red_channel;
    }

    // Create VkImage
    VkImageCreateInfo img_info{};
    img_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    img_info.format = actual_format;
    img_info.samples = VK_SAMPLE_COUNT_1_BIT;
    img_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    img_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    img_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    img_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    img_info.mipLevels = 1;
    img_info.arrayLayers = 1;

    if (is_3d) {
        img_info.imageType = VK_IMAGE_TYPE_3D;
        img_info.extent = {edge_len, edge_len, edge_len};
    } else {
        img_info.imageType = VK_IMAGE_TYPE_2D;
        img_info.extent = {width, std::max(height, 1u), 1};
    }

    VmaAllocationCreateInfo alloc_ci{};
    alloc_ci.usage = VMA_MEMORY_USAGE_GPU_ONLY;

    if (vmaCreateImage(dev.GetAllocator(), &img_info, &alloc_ci,
                       &lut.image, &lut.allocation, nullptr) != VK_SUCCESS) {
        Debug::Log("VulkanOCIORenderer: Failed to create LUT image");
        return false;
    }

    // Create staging buffer and upload
    unsigned total_pixels;
    if (is_3d) {
        total_pixels = edge_len * edge_len * edge_len;
    } else {
        total_pixels = width * std::max(height, 1u);
    }

    size_t staging_size;
    std::vector<float> rgba_data;

    const void* upload_data;
    if (need_rgba_expand) {
        // Expand RGB → RGBA
        rgba_data.resize(total_pixels * 4);
        unsigned src_components = 3;
        for (unsigned i = 0; i < total_pixels; ++i) {
            rgba_data[i * 4 + 0] = data[i * src_components + 0];
            rgba_data[i * 4 + 1] = data[i * src_components + 1];
            rgba_data[i * 4 + 2] = data[i * src_components + 2];
            rgba_data[i * 4 + 3] = 1.0f;
        }
        upload_data = rgba_data.data();
        staging_size = rgba_data.size() * sizeof(float);
    } else {
        unsigned components = is_red_channel ? 1 : (actual_format == VK_FORMAT_R32G32B32A32_SFLOAT ? 4 : 3);
        staging_size = total_pixels * components * sizeof(float);
        upload_data = data;
    }

    // Create staging buffer
    VkBufferCreateInfo buf_info{};
    buf_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buf_info.size = staging_size;
    buf_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

    VmaAllocationCreateInfo buf_alloc{};
    buf_alloc.usage = VMA_MEMORY_USAGE_CPU_ONLY;
    buf_alloc.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;

    VkBuffer staging_buf;
    VmaAllocation staging_alloc;
    VmaAllocationInfo staging_info;

    if (vmaCreateBuffer(dev.GetAllocator(), &buf_info, &buf_alloc,
                        &staging_buf, &staging_alloc, &staging_info) != VK_SUCCESS) {
        Debug::Log("VulkanOCIORenderer: Failed to create LUT staging buffer");
        vmaDestroyImage(dev.GetAllocator(), lut.image, lut.allocation);
        lut.image = VK_NULL_HANDLE;
        return false;
    }

    std::memcpy(staging_info.pMappedData, upload_data, staging_size);

    // Transfer: transition image, copy, transition to shader read
    VkCommandBuffer cmd = dev.BeginSingleTimeCommands();

    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = lut.image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);

    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = img_info.extent;

    vkCmdCopyBufferToImage(cmd, staging_buf, lut.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);

    dev.EndSingleTimeCommands(cmd);

    vmaDestroyBuffer(dev.GetAllocator(), staging_buf, staging_alloc);

    // Create image view
    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = lut.image;
    view_info.viewType = is_3d ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = actual_format;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.layerCount = 1;

    if (vkCreateImageView(device, &view_info, nullptr, &lut.view) != VK_SUCCESS) {
        Debug::Log("VulkanOCIORenderer: Failed to create LUT image view");
        return false;
    }

    // Use the shared linear sampler
    lut.sampler = linear_sampler_;

    return true;
}

bool VulkanOCIORenderer::BuildOCIOPipelineState(::OCIOPipeline* pipeline) {
    // Extract OCIO shader info
    OCIOPipeline::VulkanShaderInfo shader_info;
    if (!pipeline->GenerateVulkanShaderInfo(shader_info) || !shader_info.valid) {
        Debug::Log("VulkanOCIORenderer: Failed to get Vulkan shader info from OCIO pipeline");
        return false;
    }

    // Check if shader changed (cache by hash)
    std::hash<std::string> hasher;
    std::string new_hash = std::to_string(hasher(shader_info.ocio_function_glsl));

    if (ocio_state_.valid && ocio_state_.shader_hash == new_hash) {
        return true;  // Pipeline is still valid
    }

    // Destroy old state
    DestroyOCIOPipelineState(ocio_state_);

    auto& dev = VulkanDeviceManager::Instance();
    VkDevice device = dev.GetDevice();

    int num_luts = static_cast<int>(shader_info.luts.size());

    // Build the fragment shader GLSL
    std::string frag_glsl = BuildOCIOFragmentGLSL(shader_info.ocio_function_glsl, num_luts);

    // Compile fragment shader to SPIR-V
    VulkanShaderCompiler compiler;
    ocio_state_.frag_module = compiler.CompileAndCreate(
        frag_glsl.c_str(), ShaderStage::FRAGMENT, "ocio_color_transform");

    if (ocio_state_.frag_module == VK_NULL_HANDLE) {
        Debug::Log("VulkanOCIORenderer: Failed to compile OCIO fragment shader");
        // Log first 1000 chars of the shader for debugging
        Debug::Log("OCIO shader preview:\n" + frag_glsl.substr(0, 1000));
        return false;
    }

    // Create LUT textures
    for (const auto& lut_info : shader_info.luts) {
        OCIOPipelineState::LUTTexture lut{};

        bool ok;
        if (lut_info.is_3d) {
            ok = CreateLUTTexture(lut, lut_info.data.data(),
                                  0, 0, true, lut_info.edge_len, false);
        } else {
            ok = CreateLUTTexture(lut, lut_info.data.data(),
                                  lut_info.width, lut_info.height,
                                  false, 0, lut_info.is_red_channel);
        }

        if (!ok) {
            Debug::Log("VulkanOCIORenderer: Failed to create LUT texture '" + lut_info.sampler_name + "'");
            DestroyOCIOPipelineState(ocio_state_);
            return false;
        }

        ocio_state_.lut_textures.push_back(lut);
    }

    // Create descriptor set layout: binding 0 = video texture, bindings 1..N = LUT textures
    int total_bindings = 1 + num_luts;
    std::vector<VkDescriptorSetLayoutBinding> bindings(total_bindings);

    // Binding 0: video texture
    bindings[0] = {};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    // Bindings 1..N: LUT textures
    for (int i = 0; i < num_luts; ++i) {
        bindings[1 + i] = {};
        bindings[1 + i].binding = 1 + i;
        bindings[1 + i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[1 + i].descriptorCount = 1;
        bindings[1 + i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }

    VkDescriptorSetLayoutCreateInfo desc_layout_info{};
    desc_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    desc_layout_info.bindingCount = static_cast<uint32_t>(total_bindings);
    desc_layout_info.pBindings = bindings.data();

    if (vkCreateDescriptorSetLayout(device, &desc_layout_info, nullptr,
                                     &ocio_state_.desc_layout) != VK_SUCCESS) {
        Debug::Log("VulkanOCIORenderer: Failed to create OCIO descriptor set layout");
        DestroyOCIOPipelineState(ocio_state_);
        return false;
    }

    // Pipeline layout
    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &ocio_state_.desc_layout;

    if (vkCreatePipelineLayout(device, &layout_info, nullptr,
                                &ocio_state_.layout) != VK_SUCCESS) {
        Debug::Log("VulkanOCIORenderer: Failed to create OCIO pipeline layout");
        DestroyOCIOPipelineState(ocio_state_);
        return false;
    }

    // Create graphics pipeline (reuse passthrough vertex shader)
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = passthrough_vert_;  // Reuse fullscreen triangle vertex shader
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = ocio_state_.frag_module;
    stages[1].pName = "main";

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
    pipeline_info.layout = ocio_state_.layout;
    pipeline_info.renderPass = render_pass_;
    pipeline_info.subpass = 0;

    auto cache = VulkanDeviceManager::Instance().GetPipelineCache();
    if (vkCreateGraphicsPipelines(device, cache, 1, &pipeline_info, nullptr,
                                   &ocio_state_.pipeline) != VK_SUCCESS) {
        Debug::Log("VulkanOCIORenderer: Failed to create OCIO graphics pipeline");
        DestroyOCIOPipelineState(ocio_state_);
        return false;
    }

    // Allocate descriptor set
    VkDescriptorSetAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc_info.descriptorPool = descriptor_pool_;
    alloc_info.descriptorSetCount = 1;
    alloc_info.pSetLayouts = &ocio_state_.desc_layout;

    if (vkAllocateDescriptorSets(device, &alloc_info, &ocio_state_.desc_set) != VK_SUCCESS) {
        Debug::Log("VulkanOCIORenderer: Failed to allocate OCIO descriptor set");
        DestroyOCIOPipelineState(ocio_state_);
        return false;
    }

    // Write LUT texture descriptors (bindings 1..N)
    // (Binding 0 = video texture is written at render time since it changes per-frame)
    std::vector<VkDescriptorImageInfo> lut_img_infos(num_luts);
    std::vector<VkWriteDescriptorSet> writes(num_luts);

    for (int i = 0; i < num_luts; ++i) {
        lut_img_infos[i] = {};
        lut_img_infos[i].sampler = ocio_state_.lut_textures[i].sampler;
        lut_img_infos[i].imageView = ocio_state_.lut_textures[i].view;
        lut_img_infos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        writes[i] = {};
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = ocio_state_.desc_set;
        writes[i].dstBinding = 1 + i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &lut_img_infos[i];
    }

    if (!writes.empty()) {
        vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()),
                               writes.data(), 0, nullptr);
    }

    ocio_state_.shader_hash = new_hash;
    ocio_state_.valid = true;

    Debug::Log("VulkanOCIORenderer: Built OCIO pipeline with " +
               std::to_string(num_luts) + " LUT texture(s)");
    return true;
}

} // namespace qcview

#endif // __linux__
