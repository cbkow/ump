#pragma once

#ifdef __linux__

#include <vulkan/vulkan.h>
#include <vector>
#include <string>
#include <mutex>
#include <atomic>

struct GLFWwindow;

namespace qcview {

//=============================================================================
// HDR Display Info (Linux/Vulkan)
//
// Information about HDR capability queried from the Vulkan surface.
//=============================================================================

struct VulkanHDRDisplayInfo {
    bool hdr_supported = false;       // Surface supports HDR10 ST.2084
    bool hdr_active = false;          // HDR swapchain is currently in use
    float min_luminance = 0.0f;       // Display min luminance (nits)
    float max_luminance = 1000.0f;    // Display max luminance (nits)
    float max_full_frame_luminance = 400.0f;
    VkFormat hdr_format = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR hdr_color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
};

//=============================================================================
// Vulkan HDR Swapchain
//
// Manages swapchain creation with optional HDR10 (PQ/ST.2084) support.
// On Linux, HDR is user-toggleable at runtime — swapchain is recreated
// with the appropriate format and color space.
//
// SDR mode:  B8G8R8A8_UNORM  + SRGB_NONLINEAR
// HDR mode:  A2B10G10R10_UNORM + HDR10_ST2084
//
// Usage:
//   swapchain.Initialize(window);
//   swapchain.SetHDREnabled(true);  // Toggle at runtime
//   // ... render loop uses swapchain members ...
//   swapchain.Shutdown();
//=============================================================================

class VulkanHDRSwapchain {
public:
    VulkanHDRSwapchain();
    ~VulkanHDRSwapchain();

    // Delete copy/move
    VulkanHDRSwapchain(const VulkanHDRSwapchain&) = delete;
    VulkanHDRSwapchain& operator=(const VulkanHDRSwapchain&) = delete;

    //=========================================================================
    // Initialization
    //=========================================================================

    // Initialize surface and create swapchain. Returns false on failure.
    bool Initialize(GLFWwindow* window);
    void Shutdown();
    bool IsInitialized() const { return initialized_; }

    //=========================================================================
    // HDR Control
    //=========================================================================

    // Query if the display/surface supports HDR10
    bool IsHDRSupported() const { return hdr_info_.hdr_supported; }

    // Query if HDR is currently active (enabled + supported)
    bool IsHDRActive() const { return hdr_info_.hdr_active; }

    // Toggle HDR on/off. Recreates swapchain with appropriate format.
    // Returns true if the toggle succeeded.
    bool SetHDREnabled(bool enabled);

    // Set UI brightness in nits (updates ImGui HDR shader immediately if HDR active)
    void SetUIBrightness(float nits);
    float GetUIBrightness() const { return ui_nits_; }

    // Get detailed HDR info
    const VulkanHDRDisplayInfo& GetHDRInfo() const { return hdr_info_; }

    //=========================================================================
    // Swapchain Access (for render loop)
    //=========================================================================

    VkSurfaceKHR GetSurface() const { return surface_; }
    VkSwapchainKHR GetSwapchain() const { return swapchain_; }
    VkFormat GetFormat() const { return swapchain_format_; }
    VkColorSpaceKHR GetColorSpace() const { return swapchain_color_space_; }
    VkExtent2D GetExtent() const { return swapchain_extent_; }
    VkRenderPass GetRenderPass() const { return render_pass_; }

    uint32_t GetImageCount() const { return static_cast<uint32_t>(swapchain_images_.size()); }
    const std::vector<VkImage>& GetImages() const { return swapchain_images_; }
    const std::vector<VkImageView>& GetImageViews() const { return swapchain_image_views_; }
    const std::vector<VkFramebuffer>& GetFramebuffers() const { return framebuffers_; }
    const std::vector<VkCommandBuffer>& GetCommandBuffers() const { return command_buffers_; }

    // Sync objects (2-frame in-flight)
    VkSemaphore GetImageAvailableSemaphore(uint32_t frame_idx) const { return image_available_semaphores_[frame_idx]; }
    VkSemaphore GetRenderFinishedSemaphore(uint32_t frame_idx) const { return render_finished_semaphores_[frame_idx]; }
    VkFence GetInFlightFence(uint32_t frame_idx) const { return in_flight_fences_[frame_idx]; }

    //=========================================================================
    // ImGui HDR Shader
    //=========================================================================

    // Get the compiled HDR fragment shader SPIR-V for ImGui custom shader hook.
    // Call after Initialize(). The returned data is owned by this class and
    // remains valid until Shutdown().
    const std::vector<uint32_t>& GetHDRFragShaderSPIRV() const { return hdr_frag_spirv_; }
    bool HasHDRFragShader() const { return !hdr_frag_spirv_.empty(); }

    //=========================================================================
    // Swapchain Management
    //=========================================================================

    // Recreate swapchain (e.g. on resize). Preserves current HDR mode.
    bool Recreate();

    // Recreate only if framebuffer size changed. Returns true if recreated.
    bool RecreateIfNeeded(GLFWwindow* window);

private:
    // Internal helpers
    bool QueryHDRSupport();
    bool CreateSwapchain(VkSwapchainKHR old_swapchain = VK_NULL_HANDLE);
    bool CreateRenderPass();
    bool CreateFramebuffers();
    bool CreateCommandBuffers();
    bool CreateSyncObjects();
    void DestroySwapchainResources();
    void DestroySyncObjects();

    // Select format based on HDR mode
    VkSurfaceFormatKHR SelectSurfaceFormat(bool hdr) const;

    // Window
    GLFWwindow* window_ = nullptr;

    // Surface
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    std::vector<VkSurfaceFormatKHR> available_formats_;

    // Swapchain
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat swapchain_format_ = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR swapchain_color_space_ = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkExtent2D swapchain_extent_ = {0, 0};
    std::vector<VkImage> swapchain_images_;
    std::vector<VkImageView> swapchain_image_views_;

    // Render pass & framebuffers
    VkRenderPass render_pass_ = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> framebuffers_;

    // Command buffers
    VkCommandPool command_pool_ = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> command_buffers_;

    // Sync objects (2-frame in-flight)
    static constexpr uint32_t MAX_FRAMES_IN_FLIGHT = 2;
    std::vector<VkSemaphore> image_available_semaphores_;
    std::vector<VkSemaphore> render_finished_semaphores_;
    std::vector<VkFence> in_flight_fences_;

    // HDR state
    VulkanHDRDisplayInfo hdr_info_;
    bool hdr_requested_ = false;  // User wants HDR (persisted in settings)
    float ui_nits_ = 400.0f;     // UI brightness in nits for HDR mode

    // Compiled HDR fragment shader SPIR-V (compiled once at init, reused)
    std::vector<uint32_t> hdr_frag_spirv_;
    bool CompileHDRShader();

    bool initialized_ = false;
    std::mutex mutex_;
};

} // namespace qcview

#endif // __linux__
