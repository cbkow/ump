#include "hdr_color_utils.h"

#ifdef _WIN32
#include "hdr_output_manager.h"
#endif

#ifdef __linux__
#include "vulkan_hdr_swapchain.h"
#endif

#ifdef __APPLE__
#include "metal_hdr_swapchain.h"
#endif

namespace qcview {

// User-adjustable UI brightness for HDR mode (default 400 nits)
static float g_hdr_ui_nits = 400.0f;

void SetHDRUIBrightness(float nits) {
    g_hdr_ui_nits = nits;
}

float GetHDRUIBrightness() {
    return g_hdr_ui_nits;
}

// Global pointer set by application init — avoids circular dependency
// with VulkanHDRSwapchain while keeping the query lightweight.
#ifdef __linux__
static VulkanHDRSwapchain* g_vulkan_hdr_swapchain = nullptr;

void SetVulkanHDRSwapchain(VulkanHDRSwapchain* swapchain) {
    g_vulkan_hdr_swapchain = swapchain;
}
#endif

#ifdef __APPLE__
static MetalHDRSwapchain* g_metal_hdr_swapchain = nullptr;

void SetMetalHDRSwapchain(MetalHDRSwapchain* swapchain) {
    g_metal_hdr_swapchain = swapchain;
}
#endif

bool IsHDRDisplayActive() {
#ifdef _WIN32
    return HDROutputManager::Instance().IsHDRActive();
#elif defined(__linux__)
    return g_vulkan_hdr_swapchain && g_vulkan_hdr_swapchain->IsHDRActive();
#elif defined(__APPLE__)
    return g_metal_hdr_swapchain && g_metal_hdr_swapchain->IsHDRActive();
#else
    return false;
#endif
}

bool IsHDROutputActive() {
#ifdef _WIN32
    // Windows/OpenGL: CPU-side PQ conversion needed
    return HDROutputManager::Instance().IsHDRActive();
#elif defined(__linux__)
    // Linux/Vulkan: ImGui HDR fragment shader handles sRGB→PQ on the GPU
    // CPU-side macros must NOT pre-convert (would double-convert)
    return false;
#elif defined(__APPLE__)
    // macOS/Metal: ImGui EDR fragment shader handles sRGB→linear on the GPU
    // CPU-side macros must NOT pre-convert (would double-convert)
    return false;
#else
    return false;
#endif
}

bool IsEDRLinearActive() {
#ifdef __APPLE__
    if (!g_metal_hdr_swapchain || !g_metal_hdr_swapchain->IsHDRActive())
        return false;
    return EDRColorspaceIsLinear(g_metal_hdr_swapchain->GetEDRColorspace());
#else
    return false;
#endif
}

} // namespace qcview
