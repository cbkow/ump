#include "vulkan_hdr_swapchain.h"

#ifdef __linux__

#include "../gpu/vulkan_device.h"
#include "../gpu/vulkan_shader_compiler.h"
#include "../utils/debug_utils.h"
#include <imgui.h>
#include <backends/imgui_impl_vulkan.h>
#include <GLFW/glfw3.h>
#include <algorithm>
#include <cstring>

namespace qcview {

VulkanHDRSwapchain::VulkanHDRSwapchain() = default;

VulkanHDRSwapchain::~VulkanHDRSwapchain() {
    Shutdown();
}

bool VulkanHDRSwapchain::Initialize(GLFWwindow* window) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (initialized_) {
        Debug::Log("VulkanHDRSwapchain: Already initialized");
        return true;
    }

    window_ = window;
    auto& vk = VulkanDeviceManager::Instance();

    // Create surface via GLFW
    VkResult err = glfwCreateWindowSurface(vk.GetInstance(), window, nullptr, &surface_);
    if (err != VK_SUCCESS) {
        Debug::Log("VulkanHDRSwapchain: Failed to create window surface");
        return false;
    }

    // Verify surface support on graphics queue
    VkBool32 surface_supported = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(vk.GetPhysicalDevice(),
        vk.GetGraphicsQueueFamily(), surface_, &surface_supported);
    if (!surface_supported) {
        Debug::Log("VulkanHDRSwapchain: Surface not supported on graphics queue");
        return false;
    }

    // Query available surface formats
    uint32_t format_count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(vk.GetPhysicalDevice(), surface_, &format_count, nullptr);
    available_formats_.resize(format_count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(vk.GetPhysicalDevice(), surface_, &format_count, available_formats_.data());

    // Check HDR support
    QueryHDRSupport();

    // Compile HDR fragment shader for ImGui
    CompileHDRShader();

    // Create swapchain (SDR initially, user can toggle HDR)
    if (!CreateSwapchain()) {
        Debug::Log("VulkanHDRSwapchain: Failed to create swapchain");
        return false;
    }

    if (!CreateRenderPass()) {
        Debug::Log("VulkanHDRSwapchain: Failed to create render pass");
        return false;
    }

    if (!CreateFramebuffers()) {
        Debug::Log("VulkanHDRSwapchain: Failed to create framebuffers");
        return false;
    }

    if (!CreateCommandBuffers()) {
        Debug::Log("VulkanHDRSwapchain: Failed to create command buffers");
        return false;
    }

    if (!CreateSyncObjects()) {
        Debug::Log("VulkanHDRSwapchain: Failed to create sync objects");
        return false;
    }

    initialized_ = true;

    Debug::Log("VulkanHDRSwapchain: Initialized (" +
               std::to_string(swapchain_extent_.width) + "x" +
               std::to_string(swapchain_extent_.height) + ", " +
               std::to_string(GetImageCount()) + " images, " +
               (hdr_info_.hdr_supported ? "HDR supported" : "SDR only") + ")");

    return true;
}

void VulkanHDRSwapchain::Shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!initialized_) return;

    auto& vk = VulkanDeviceManager::Instance();
    vkDeviceWaitIdle(vk.GetDevice());

    DestroySyncObjects();

    if (command_pool_ != VK_NULL_HANDLE) {
        vkDestroyCommandPool(vk.GetDevice(), command_pool_, nullptr);
        command_pool_ = VK_NULL_HANDLE;
        command_buffers_.clear();
    }

    DestroySwapchainResources();

    if (render_pass_ != VK_NULL_HANDLE) {
        vkDestroyRenderPass(vk.GetDevice(), render_pass_, nullptr);
        render_pass_ = VK_NULL_HANDLE;
    }

    if (swapchain_ != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(vk.GetDevice(), swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }

    if (surface_ != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(vk.GetInstance(), surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
    }

    initialized_ = false;
    Debug::Log("VulkanHDRSwapchain: Shutdown complete");
}

//=============================================================================
// HDR Control
//=============================================================================

bool VulkanHDRSwapchain::QueryHDRSupport() {
    hdr_info_ = {};

    for (const auto& fmt : available_formats_) {
        if (fmt.colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT &&
            fmt.format == VK_FORMAT_A2B10G10R10_UNORM_PACK32) {
            hdr_info_.hdr_supported = true;
            hdr_info_.hdr_format = fmt.format;
            hdr_info_.hdr_color_space = fmt.colorSpace;
            Debug::Log("VulkanHDRSwapchain: HDR10 ST.2084 supported (A2B10G10R10_UNORM)");
            return true;
        }
    }

    // Fallback: check for A2R10G10B10 variant
    for (const auto& fmt : available_formats_) {
        if (fmt.colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT &&
            fmt.format == VK_FORMAT_A2R10G10B10_UNORM_PACK32) {
            hdr_info_.hdr_supported = true;
            hdr_info_.hdr_format = fmt.format;
            hdr_info_.hdr_color_space = fmt.colorSpace;
            Debug::Log("VulkanHDRSwapchain: HDR10 ST.2084 supported (A2R10G10B10_UNORM)");
            return true;
        }
    }

    Debug::Log("VulkanHDRSwapchain: HDR10 not supported by surface");
    return false;
}

bool VulkanHDRSwapchain::SetHDREnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!initialized_) return false;

    if (enabled && !hdr_info_.hdr_supported) {
        Debug::Log("VulkanHDRSwapchain: Cannot enable HDR — not supported by surface");
        return false;
    }

    if (enabled == hdr_requested_) return true;  // No change needed

    hdr_requested_ = enabled;

    auto& vk = VulkanDeviceManager::Instance();
    vkDeviceWaitIdle(vk.GetDevice());

    // Destroy old resources (keep render pass — it will be recreated for new format)
    DestroySwapchainResources();

    if (render_pass_ != VK_NULL_HANDLE) {
        vkDestroyRenderPass(vk.GetDevice(), render_pass_, nullptr);
        render_pass_ = VK_NULL_HANDLE;
    }

    VkSwapchainKHR old_swapchain = swapchain_;
    swapchain_ = VK_NULL_HANDLE;

    if (!CreateSwapchain(old_swapchain)) {
        Debug::Log("VulkanHDRSwapchain: Failed to recreate swapchain for HDR toggle");
        return false;
    }

    if (old_swapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(vk.GetDevice(), old_swapchain, nullptr);
    }

    if (!CreateRenderPass() || !CreateFramebuffers()) {
        Debug::Log("VulkanHDRSwapchain: Failed to recreate render resources");
        return false;
    }

    // Reallocate command buffers if image count changed
    if (command_buffers_.size() != swapchain_images_.size()) {
        vkFreeCommandBuffers(vk.GetDevice(), command_pool_,
            static_cast<uint32_t>(command_buffers_.size()), command_buffers_.data());
        if (!CreateCommandBuffers()) return false;
    }

    hdr_info_.hdr_active = enabled && hdr_info_.hdr_supported;

    // Recreate ImGui pipeline with the new render pass (format changed)
    ImGui_ImplVulkan_PipelineInfo pipeline_info{};
    pipeline_info.RenderPass = render_pass_;
    ImGui_ImplVulkan_CreateMainPipeline(&pipeline_info);

    // Update ImGui HDR mode
    ImGui_ImplVulkan_SetHDRMode(hdr_info_.hdr_active, ui_nits_);

    // Set HDR metadata if HDR is active
    if (hdr_info_.hdr_active && vk.HasExtension(VK_EXT_HDR_METADATA_EXTENSION_NAME)) {
        auto vkSetHdrMetadataEXT = (PFN_vkSetHdrMetadataEXT)
            vkGetDeviceProcAddr(vk.GetDevice(), "vkSetHdrMetadataEXT");

        if (vkSetHdrMetadataEXT) {
            VkHdrMetadataEXT metadata{};
            metadata.sType = VK_STRUCTURE_TYPE_HDR_METADATA_EXT;
            // BT.2020 primaries
            metadata.displayPrimaryRed   = {0.708f, 0.292f};
            metadata.displayPrimaryGreen = {0.170f, 0.797f};
            metadata.displayPrimaryBlue  = {0.131f, 0.046f};
            metadata.whitePoint          = {0.3127f, 0.3290f};  // D65
            metadata.maxLuminance = hdr_info_.max_luminance;
            metadata.minLuminance = hdr_info_.min_luminance;
            metadata.maxContentLightLevel = 1000.0f;
            metadata.maxFrameAverageLightLevel = 400.0f;

            vkSetHdrMetadataEXT(vk.GetDevice(), 1, &swapchain_, &metadata);
            Debug::Log("VulkanHDRSwapchain: HDR metadata set (max " +
                       std::to_string((int)hdr_info_.max_luminance) + " nits)");
        }
    }

    Debug::Log("VulkanHDRSwapchain: HDR " + std::string(enabled ? "enabled" : "disabled") +
               " (" + std::to_string(swapchain_extent_.width) + "x" +
               std::to_string(swapchain_extent_.height) + ")");

    return true;
}

void VulkanHDRSwapchain::SetUIBrightness(float nits) {
    ui_nits_ = nits;
    if (hdr_info_.hdr_active) {
        ImGui_ImplVulkan_SetHDRMode(true, ui_nits_);
    }
}

//=============================================================================
// Swapchain Recreation
//=============================================================================

bool VulkanHDRSwapchain::Recreate() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!initialized_) return false;

    auto& vk = VulkanDeviceManager::Instance();
    vkDeviceWaitIdle(vk.GetDevice());

    DestroySwapchainResources();

    if (render_pass_ != VK_NULL_HANDLE) {
        vkDestroyRenderPass(vk.GetDevice(), render_pass_, nullptr);
        render_pass_ = VK_NULL_HANDLE;
    }

    VkSwapchainKHR old_swapchain = swapchain_;
    swapchain_ = VK_NULL_HANDLE;

    if (!CreateSwapchain(old_swapchain)) return false;

    if (old_swapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(vk.GetDevice(), old_swapchain, nullptr);
    }

    if (!CreateRenderPass() || !CreateFramebuffers()) return false;

    // Reallocate command buffers if image count changed
    if (command_buffers_.size() != swapchain_images_.size()) {
        vkFreeCommandBuffers(vk.GetDevice(), command_pool_,
            static_cast<uint32_t>(command_buffers_.size()), command_buffers_.data());
        if (!CreateCommandBuffers()) return false;
    }

    Debug::Log("VulkanHDRSwapchain: Recreated (" +
               std::to_string(swapchain_extent_.width) + "x" +
               std::to_string(swapchain_extent_.height) + ")");

    return true;
}

bool VulkanHDRSwapchain::RecreateIfNeeded(GLFWwindow* window) {
    int fb_w, fb_h;
    glfwGetFramebufferSize(window, &fb_w, &fb_h);

    if (fb_w > 0 && fb_h > 0 &&
        (swapchain_extent_.width != static_cast<uint32_t>(fb_w) ||
         swapchain_extent_.height != static_cast<uint32_t>(fb_h))) {
        return Recreate();
    }
    return false;
}

//=============================================================================
// Internal: Surface Format Selection
//=============================================================================

VkSurfaceFormatKHR VulkanHDRSwapchain::SelectSurfaceFormat(bool hdr) const {
    if (hdr && hdr_info_.hdr_supported) {
        // Return the HDR format we found during QueryHDRSupport
        return { hdr_info_.hdr_format, hdr_info_.hdr_color_space };
    }

    // SDR: prefer B8G8R8A8_UNORM to avoid double-gamma from SRGB format
    // (ImGui outputs already-sRGB colors)
    for (const auto& fmt : available_formats_) {
        if (fmt.format == VK_FORMAT_B8G8R8A8_UNORM &&
            fmt.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            return fmt;
        }
    }

    return available_formats_[0];
}

//=============================================================================
// Internal: Swapchain Creation
//=============================================================================

bool VulkanHDRSwapchain::CreateSwapchain(VkSwapchainKHR old_swapchain) {
    auto& vk = VulkanDeviceManager::Instance();

    VkSurfaceCapabilitiesKHR surface_caps{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk.GetPhysicalDevice(), surface_, &surface_caps);

    // Get framebuffer size
    int fb_width, fb_height;
    glfwGetFramebufferSize(window_, &fb_width, &fb_height);
    VkExtent2D extent = { static_cast<uint32_t>(fb_width), static_cast<uint32_t>(fb_height) };
    extent.width = std::clamp(extent.width, surface_caps.minImageExtent.width, surface_caps.maxImageExtent.width);
    extent.height = std::clamp(extent.height, surface_caps.minImageExtent.height, surface_caps.maxImageExtent.height);

    if (extent.width == 0 || extent.height == 0) return false;  // Minimized

    uint32_t image_count = surface_caps.minImageCount + 1;
    if (surface_caps.maxImageCount > 0 && image_count > surface_caps.maxImageCount) {
        image_count = surface_caps.maxImageCount;
    }

    VkSurfaceFormatKHR selected_format = SelectSurfaceFormat(hdr_requested_);

    VkSwapchainCreateInfoKHR swapchain_info{};
    swapchain_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    swapchain_info.surface = surface_;
    swapchain_info.minImageCount = image_count;
    swapchain_info.imageFormat = selected_format.format;
    swapchain_info.imageColorSpace = selected_format.colorSpace;
    swapchain_info.imageExtent = extent;
    swapchain_info.imageArrayLayers = 1;
    swapchain_info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    swapchain_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    swapchain_info.preTransform = surface_caps.currentTransform;
    swapchain_info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    swapchain_info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    swapchain_info.clipped = VK_TRUE;
    swapchain_info.oldSwapchain = old_swapchain;

    VkResult err = vkCreateSwapchainKHR(vk.GetDevice(), &swapchain_info, nullptr, &swapchain_);
    if (err != VK_SUCCESS) {
        Debug::Log("VulkanHDRSwapchain: vkCreateSwapchainKHR failed (" + std::to_string(err) + ")");
        return false;
    }

    swapchain_format_ = selected_format.format;
    swapchain_color_space_ = selected_format.colorSpace;
    swapchain_extent_ = extent;

    // Get swapchain images
    vkGetSwapchainImagesKHR(vk.GetDevice(), swapchain_, &image_count, nullptr);
    swapchain_images_.resize(image_count);
    vkGetSwapchainImagesKHR(vk.GetDevice(), swapchain_, &image_count, swapchain_images_.data());

    // Create image views
    swapchain_image_views_.resize(image_count);
    for (uint32_t i = 0; i < image_count; ++i) {
        VkImageViewCreateInfo view_info{};
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image = swapchain_images_[i];
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = swapchain_format_;
        view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view_info.subresourceRange.baseMipLevel = 0;
        view_info.subresourceRange.levelCount = 1;
        view_info.subresourceRange.baseArrayLayer = 0;
        view_info.subresourceRange.layerCount = 1;

        err = vkCreateImageView(vk.GetDevice(), &view_info, nullptr, &swapchain_image_views_[i]);
        if (err != VK_SUCCESS) return false;
    }

    return true;
}

//=============================================================================
// Internal: Render Pass
//=============================================================================

bool VulkanHDRSwapchain::CreateRenderPass() {
    auto& vk = VulkanDeviceManager::Instance();

    VkAttachmentDescription color_attachment{};
    color_attachment.format = swapchain_format_;
    color_attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color_attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color_attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color_attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color_attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

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
    dependency.srcAccessMask = 0;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo render_pass_info{};
    render_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    render_pass_info.attachmentCount = 1;
    render_pass_info.pAttachments = &color_attachment;
    render_pass_info.subpassCount = 1;
    render_pass_info.pSubpasses = &subpass;
    render_pass_info.dependencyCount = 1;
    render_pass_info.pDependencies = &dependency;

    VkResult err = vkCreateRenderPass(vk.GetDevice(), &render_pass_info, nullptr, &render_pass_);
    return err == VK_SUCCESS;
}

//=============================================================================
// Internal: Framebuffers
//=============================================================================

bool VulkanHDRSwapchain::CreateFramebuffers() {
    auto& vk = VulkanDeviceManager::Instance();

    framebuffers_.resize(swapchain_images_.size());
    for (size_t i = 0; i < swapchain_images_.size(); ++i) {
        VkFramebufferCreateInfo fb_info{};
        fb_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fb_info.renderPass = render_pass_;
        fb_info.attachmentCount = 1;
        fb_info.pAttachments = &swapchain_image_views_[i];
        fb_info.width = swapchain_extent_.width;
        fb_info.height = swapchain_extent_.height;
        fb_info.layers = 1;

        VkResult err = vkCreateFramebuffer(vk.GetDevice(), &fb_info, nullptr, &framebuffers_[i]);
        if (err != VK_SUCCESS) return false;
    }

    return true;
}

//=============================================================================
// Internal: Command Buffers
//=============================================================================

bool VulkanHDRSwapchain::CreateCommandBuffers() {
    auto& vk = VulkanDeviceManager::Instance();

    if (command_pool_ == VK_NULL_HANDLE) {
        command_pool_ = vk.CreateCommandPool(vk.GetGraphicsQueueFamily());
        if (command_pool_ == VK_NULL_HANDLE) return false;
    }

    uint32_t count = static_cast<uint32_t>(swapchain_images_.size());
    command_buffers_.resize(count);

    VkCommandBufferAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc_info.commandPool = command_pool_;
    alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandBufferCount = count;

    VkResult err = vkAllocateCommandBuffers(vk.GetDevice(), &alloc_info, command_buffers_.data());
    return err == VK_SUCCESS;
}

//=============================================================================
// Internal: Sync Objects
//=============================================================================

bool VulkanHDRSwapchain::CreateSyncObjects() {
    auto& vk = VulkanDeviceManager::Instance();

    image_available_semaphores_.resize(MAX_FRAMES_IN_FLIGHT);
    render_finished_semaphores_.resize(MAX_FRAMES_IN_FLIGHT);
    in_flight_fences_.resize(MAX_FRAMES_IN_FLIGHT);

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        image_available_semaphores_[i] = vk.CreateSemaphore();
        render_finished_semaphores_[i] = vk.CreateSemaphore();
        in_flight_fences_[i] = vk.CreateFence(true);  // Signaled initially

        if (image_available_semaphores_[i] == VK_NULL_HANDLE ||
            render_finished_semaphores_[i] == VK_NULL_HANDLE ||
            in_flight_fences_[i] == VK_NULL_HANDLE) {
            return false;
        }
    }

    return true;
}

//=============================================================================
// Internal: Cleanup Helpers
//=============================================================================

void VulkanHDRSwapchain::DestroySwapchainResources() {
    auto& vk = VulkanDeviceManager::Instance();

    for (auto& fb : framebuffers_) {
        if (fb != VK_NULL_HANDLE) vkDestroyFramebuffer(vk.GetDevice(), fb, nullptr);
    }
    framebuffers_.clear();

    for (auto& iv : swapchain_image_views_) {
        if (iv != VK_NULL_HANDLE) vkDestroyImageView(vk.GetDevice(), iv, nullptr);
    }
    swapchain_image_views_.clear();
    swapchain_images_.clear();
}

void VulkanHDRSwapchain::DestroySyncObjects() {
    auto& vk = VulkanDeviceManager::Instance();

    for (auto& s : image_available_semaphores_) {
        if (s != VK_NULL_HANDLE) vkDestroySemaphore(vk.GetDevice(), s, nullptr);
    }
    image_available_semaphores_.clear();

    for (auto& s : render_finished_semaphores_) {
        if (s != VK_NULL_HANDLE) vkDestroySemaphore(vk.GetDevice(), s, nullptr);
    }
    render_finished_semaphores_.clear();

    for (auto& f : in_flight_fences_) {
        if (f != VK_NULL_HANDLE) vkDestroyFence(vk.GetDevice(), f, nullptr);
    }
    in_flight_fences_.clear();
}

//=============================================================================
// Internal: HDR Shader Compilation
//=============================================================================

bool VulkanHDRSwapchain::CompileHDRShader() {
    static const char* hdr_frag_glsl = R"(
#version 450 core

layout(location = 0) out vec4 fColor;
layout(set=0, binding=0) uniform sampler2D sTexture;
layout(location = 0) in struct { vec4 Color; vec2 UV; } In;

layout(push_constant) uniform uHDRConstants {
    layout(offset = 16) int HDRMode;
    int TargetNits;
    int HDRPassthrough;
} hdr;

vec3 srgb_to_linear(vec3 srgb) {
    return mix(srgb / 12.92, pow((srgb + 0.055) / 1.055, vec3(2.4)), step(0.04045, srgb));
}

vec3 bt709_to_bt2020(vec3 rgb) {
    const mat3 m = mat3(
        0.6274, 0.0691, 0.0164,
        0.3293, 0.9195, 0.0880,
        0.0433, 0.0114, 0.8956
    );
    return m * rgb;
}

vec3 linear_to_pq(vec3 linear_col) {
    const float m1 = 0.1593017578125;
    const float m2 = 78.84375;
    const float c1 = 0.8359375;
    const float c2 = 18.8515625;
    const float c3 = 18.6875;
    vec3 L = max(linear_col * (float(hdr.TargetNits) / 100000.0), vec3(0.0));
    vec3 Lm = pow(L, vec3(m1));
    return pow((c1 + c2 * Lm) / (1.0 + c3 * Lm), vec3(m2));
}

void main()
{
    vec4 color = In.Color * texture(sTexture, In.UV.st);
    if (hdr.HDRMode != 0 && hdr.HDRPassthrough == 0) {
        color.rgb = linear_to_pq(bt709_to_bt2020(srgb_to_linear(color.rgb)));
    }
    fColor = color;
}
)";

    VulkanShaderCompiler compiler;
    hdr_frag_spirv_ = compiler.CompileGLSL(hdr_frag_glsl, ShaderStage::FRAGMENT, "imgui_hdr_frag");

    if (hdr_frag_spirv_.empty()) {
        Debug::Log("VulkanHDRSwapchain: Failed to compile HDR fragment shader: " + compiler.GetLastError());
        return false;
    }

    Debug::Log("VulkanHDRSwapchain: HDR fragment shader compiled (" +
               std::to_string(hdr_frag_spirv_.size() * 4) + " bytes SPIR-V)");
    return true;
}

} // namespace qcview

#endif // __linux__
