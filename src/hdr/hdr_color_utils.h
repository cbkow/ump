#pragma once

#include <cmath>
#include <algorithm>
#include <imgui.h>

namespace qcview {

//=============================================================================
// HDR Color Conversion Utilities (Cross-Platform)
//
// Convert SDR (sRGB) colors to HDR (PQ/ST.2084) for comfortable UI rendering.
// Use these when HDR is active to ensure UI elements appear at appropriate
// brightness (~100-200 nits) instead of being blown out at HDR luminance.
//
// These functions are pure math — no platform dependencies.
//=============================================================================

// Convert sRGB gamma color component to linear light
inline float SRGBToLinear(float srgb) {
    return srgb <= 0.04045f ? srgb / 12.92f : std::pow((srgb + 0.055f) / 1.055f, 2.4f);
}

// Convert linear light to PQ (ST.2084) encoded value
// target_nits: target luminance for the color (default 100 nits for comfortable SDR)
inline float LinearToPQ(float linear, float target_nits = 100.0f) {
    constexpr float m1 = 0.1593017578125f;
    constexpr float m2 = 78.84375f;
    constexpr float c1 = 0.8359375f;
    constexpr float c2 = 18.8515625f;
    constexpr float c3 = 18.6875f;
    constexpr float pq_max_nits = 10000.0f;

    float L = linear * (target_nits / pq_max_nits);
    L = std::max(L, 0.0f);
    float Lm = std::pow(L, m1);
    return std::pow((c1 + c2 * Lm) / (1.0f + c3 * Lm), m2);
}

// Convert sRGB color to PQ for HDR display
inline float SDRToPQ(float srgb, float target_nits = 100.0f) {
    return LinearToPQ(SRGBToLinear(srgb), target_nits);
}

// Convert sRGB to PQ with highlight compression
inline float SDRToPQCompressed(float srgb, float base_nits = 100.0f, float highlight_nits = 60.0f) {
    float linear = SRGBToLinear(srgb);
    float brightness = linear;
    float target = base_nits + (highlight_nits - base_nits) * brightness;
    return LinearToPQ(linear, target);
}

// Convert ImVec4 color (sRGB) to HDR (PQ-encoded) for UI rendering
inline ImVec4 ColorToHDR(const ImVec4& color, float target_nits = 100.0f) {
    return ImVec4(
        SDRToPQ(color.x, target_nits),
        SDRToPQ(color.y, target_nits),
        SDRToPQ(color.z, target_nits),
        color.w
    );
}

// Convert IM_COL32 packed color to HDR-adjusted ImVec4
inline ImVec4 ColorU32ToHDR(ImU32 col, float target_nits = 100.0f) {
    float r = ((col >> IM_COL32_R_SHIFT) & 0xFF) / 255.0f;
    float g = ((col >> IM_COL32_G_SHIFT) & 0xFF) / 255.0f;
    float b = ((col >> IM_COL32_B_SHIFT) & 0xFF) / 255.0f;
    float a = ((col >> IM_COL32_A_SHIFT) & 0xFF) / 255.0f;
    return ImVec4(
        SDRToPQ(r, target_nits),
        SDRToPQ(g, target_nits),
        SDRToPQ(b, target_nits),
        a
    );
}

// Convert IM_COL32 to HDR-adjusted IM_COL32 (for draw list operations)
inline ImU32 Col32ToHDR(ImU32 col, float target_nits = 100.0f) {
    float r = ((col >> IM_COL32_R_SHIFT) & 0xFF) / 255.0f;
    float g = ((col >> IM_COL32_G_SHIFT) & 0xFF) / 255.0f;
    float b = ((col >> IM_COL32_B_SHIFT) & 0xFF) / 255.0f;
    int a = (col >> IM_COL32_A_SHIFT) & 0xFF;
    return IM_COL32(
        (int)(SDRToPQ(r, target_nits) * 255.0f),
        (int)(SDRToPQ(g, target_nits) * 255.0f),
        (int)(SDRToPQ(b, target_nits) * 255.0f),
        a
    );
}

//=============================================================================
// Cross-Platform HDR State Query
//=============================================================================

// Returns true if HDR output is currently active (for functional decisions
// like disabling auto-121 OCIO). Works on all platforms.
bool IsHDRDisplayActive();

// Returns true if CPU-side color conversion to PQ is needed.
// On Linux/Vulkan this returns false because the ImGui HDR fragment shader
// handles sRGB→PQ on the GPU. On Windows/OpenGL this returns true.
bool IsHDROutputActive();

// Returns true if EDR is active with a linear-encoded colorspace (macOS only).
// When true, video textures need linear→sRGB pre-encoding so the EDR shader
// can linearize back correctly. When false (gamma EDR), no pre-encoding needed.
bool IsEDRLinearActive();

// Set/get the UI brightness level in nits (cross-platform).
// On Windows this controls CPU-side PQ conversion target.
// On Linux this is stored here for settings persistence; the actual
// conversion happens in the ImGui Vulkan HDR fragment shader.
void SetHDRUIBrightness(float nits);
float GetHDRUIBrightness();

#ifdef __linux__
class VulkanHDRSwapchain;
void SetVulkanHDRSwapchain(VulkanHDRSwapchain* swapchain);
#endif

#ifdef __APPLE__
class MetalHDRSwapchain;
void SetMetalHDRSwapchain(MetalHDRSwapchain* swapchain);
#endif

//=============================================================================
// Runtime HDR Color Helpers
//
// Use these throughout the application for colors that need HDR adjustment.
// They check HDR state at runtime and use the user-configured brightness.
//=============================================================================

inline ImU32 GetUIWhite() {
    if (IsHDROutputActive()) {
        return Col32ToHDR(IM_COL32(255, 255, 255, 255), GetHDRUIBrightness());
    }
    return IM_COL32(255, 255, 255, 255);
}

inline ImU32 GetUIColor(ImU32 sdr_color) {
    if (IsHDROutputActive()) {
        return Col32ToHDR(sdr_color, GetHDRUIBrightness());
    }
    return sdr_color;
}

inline ImVec4 GetUIColorVec4(const ImVec4& sdr_color) {
    if (IsHDROutputActive()) {
        return ColorToHDR(sdr_color, GetHDRUIBrightness());
    }
    return sdr_color;
}

} // namespace qcview
