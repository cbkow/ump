#pragma once

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <memory>
#include <atomic>
#include <chrono>
#include <cmath>
#include <algorithm>

// ImGui types for color conversion utilities
#include <imgui.h>

// Include swapchain header for HDRDisplayInfo, InteropMethod, etc.
#include "d3d11_hdr_swapchain.h"

namespace ump {

// Forward declarations (D3D11HDRSwapchain is fully defined via include above)
class D3D11HDRSwapchain;

//=============================================================================
// HDR Output Manager
//
// High-level singleton that manages HDR presentation via D3D11.
// Provides a simple interface for the application to:
// - Initialize HDR output for a window
// - Present OpenGL-rendered frames with HDR support
// - Query and respond to HDR state changes
//
// Zero-copy rendering mode (NVIDIA GPUs):
//   hdr.BeginFrame();      // Binds D3D11-backed FBO
//   // ... render ImGui ...
//   hdr.EndFrame();        // Presents via DXGI
//
// Fallback mode (AMD/Intel):
//   Same API, but internally uses CPU readback
//=============================================================================

class HDROutputManager {
public:
    // Singleton access
    static HDROutputManager& Instance();

    // Delete copy/move
    HDROutputManager(const HDROutputManager&) = delete;
    HDROutputManager& operator=(const HDROutputManager&) = delete;

    //=========================================================================
    // Initialization
    //=========================================================================

    // Initialize HDR output for the given window
    // Returns true if D3D11 swapchain was created (even if HDR is not available)
    bool Initialize(HWND hwnd, int width, int height);

    // Cleanup all resources (call on application shutdown)
    void Shutdown();

    // Handle window resize
    void OnResize(int width, int height);

    //=========================================================================
    // HDR Status
    //=========================================================================

    // Check if the display supports HDR (even if not currently enabled)
    bool IsHDRSupported() const;

    // Check if HDR is currently available and active
    // Returns true only if: display supports HDR AND Windows HDR is enabled AND not in bypass mode
    bool IsHDRActive() const;

    // Check if D3D11 swapchain is initialized (may be true even without HDR)
    bool IsInitialized() const;

    // Check if zero-copy path is being used (NV_DX_interop or EXT_memory_object)
    bool IsZeroCopyEnabled() const;

    // Get interop method name for display
    const char* GetInteropMethodName() const;

    // Get detailed HDR info
    const HDRDisplayInfo* GetHDRInfo() const;

    // Refresh HDR status (call periodically, e.g., once per second)
    // This detects when user toggles Windows HDR setting
    void RefreshHDRStatus();

    //=========================================================================
    // Bypass Mode
    //=========================================================================

    // When bypass mode is enabled, BeginFrame/EndFrame do nothing and caller
    // should use glfwSwapBuffers() instead. Use this to toggle between
    // D3D11 and OpenGL presentation paths.
    bool IsInBypassMode() const { return bypass_mode_.load(); }
    void SetBypassMode(bool bypass);

    //=========================================================================
    // Frame Rendering (Zero-Copy API)
    //=========================================================================

    // Begin a frame - binds the D3D11-backed render target
    // Returns the FBO ID, or 0 if in bypass mode / not initialized
    // After this call, all OpenGL rendering goes to the HDR swapchain
    GLuint BeginFrame();

    // End a frame - presents via DXGI swapchain
    // vsync: true = wait for vertical blank (no tearing)
    void EndFrame(bool vsync = true);

    // Check if currently between BeginFrame/EndFrame
    bool IsInFrame() const;

    //=========================================================================
    // Frame Rendering (Full D3D11 Mode)
    // For rendering with D3D11 directly (no OpenGL)
    //=========================================================================

    // Begin a D3D11-only frame - binds backbuffer as render target
    // Use this when rendering with ImGui D3D11 backend
    void BeginD3D11Frame();

    // End a D3D11-only frame - presents swapchain
    void EndD3D11Frame(bool vsync = true);

    // Get the backbuffer render target view (for ImGui D3D11 backend)
    ID3D11RenderTargetView* GetBackBufferRTV() const;

    // Get device and context (for external D3D11 operations)
    ID3D11Device1* GetDevice() const;
    ID3D11DeviceContext1* GetContext() const;

    //=========================================================================
    // Legacy API (copy-based)
    //=========================================================================

    // Copy the OpenGL texture to D3D11 and present
    // Use BeginFrame/EndFrame instead for zero-copy rendering
    bool PresentFrame(GLuint gl_texture, int width, int height, bool vsync = true);

    //=========================================================================
    // Statistics
    //=========================================================================

    struct Stats {
        uint64_t total_frames = 0;
        uint64_t hdr_frames = 0;
        uint64_t sdr_frames = 0;
        uint64_t zero_copy_frames = 0;
        uint64_t cpu_readback_frames = 0;
        bool using_zero_copy = false;
        double last_present_ms = 0.0;
    };

    Stats GetStats() const;

private:
    HDROutputManager();
    ~HDROutputManager();

    std::unique_ptr<D3D11HDRSwapchain> swapchain_;

    std::atomic<bool> bypass_mode_{false};
    std::atomic<bool> initialized_{false};

    // For periodic HDR status refresh
    std::chrono::steady_clock::time_point last_hdr_check_;
    static constexpr double HDR_CHECK_INTERVAL_SEC = 2.0;
};

//=============================================================================
// HDR Color Conversion Utilities
//
// Convert SDR (sRGB) colors to HDR (PQ/ST.2084) for comfortable UI rendering.
// Use these when HDR is active to ensure UI elements appear at appropriate
// brightness (~100-200 nits) instead of being blown out at HDR luminance.
//=============================================================================

// Convert sRGB gamma color component to linear light
inline float SRGBToLinear(float srgb) {
    return srgb <= 0.04045f ? srgb / 12.92f : std::pow((srgb + 0.055f) / 1.055f, 2.4f);
}

// Convert linear light to PQ (ST.2084) encoded value
// target_nits: target luminance for the color (default 100 nits for comfortable SDR)
inline float LinearToPQ(float linear, float target_nits = 100.0f) {
    // PQ constants (ST.2084)
    constexpr float m1 = 0.1593017578125f;
    constexpr float m2 = 78.84375f;
    constexpr float c1 = 0.8359375f;
    constexpr float c2 = 18.8515625f;
    constexpr float c3 = 18.6875f;
    constexpr float pq_max_nits = 10000.0f;

    // Scale linear to PQ reference (target_nits / 10000)
    float L = linear * (target_nits / pq_max_nits);
    L = std::max(L, 0.0f);
    float Lm = std::pow(L, m1);
    return std::pow((c1 + c2 * Lm) / (1.0f + c3 * Lm), m2);
}

// Convert sRGB color to PQ for HDR display
// Returns PQ-encoded value suitable for HDR10 output
inline float SDRToPQ(float srgb, float target_nits = 100.0f) {
    return LinearToPQ(SRGBToLinear(srgb), target_nits);
}

// Convert sRGB to PQ with highlight compression
// Brighter colors get reduced target nits to avoid searing whites
// base_nits: target for dark colors, highlight_nits: target for pure white
inline float SDRToPQCompressed(float srgb, float base_nits = 100.0f, float highlight_nits = 60.0f) {
    float linear = SRGBToLinear(srgb);
    // Interpolate target nits based on brightness - brighter = lower target
    float brightness = linear;  // 0 = black, 1 = white
    float target = base_nits + (highlight_nits - base_nits) * brightness;
    return LinearToPQ(linear, target);
}

// Convert ImVec4 color (sRGB) to HDR (PQ-encoded) for UI rendering
// Preserves alpha channel unchanged
inline ImVec4 ColorToHDR(const ImVec4& color, float target_nits = 100.0f) {
    return ImVec4(
        SDRToPQ(color.x, target_nits),
        SDRToPQ(color.y, target_nits),
        SDRToPQ(color.z, target_nits),
        color.w  // Alpha unchanged
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
// Runtime HDR Color Helpers
//
// Use these throughout the application for colors that need HDR adjustment.
// They check HDR state at runtime and convert only when needed.
//=============================================================================

// Get white color appropriate for current display mode
// In HDR: returns PQ-encoded white at comfortable brightness
// In SDR: returns pure white
inline ImU32 GetUIWhite(float target_nits = 120.0f) {
    if (HDROutputManager::Instance().IsHDRActive()) {
        return Col32ToHDR(IM_COL32(255, 255, 255, 255), target_nits);
    }
    return IM_COL32(255, 255, 255, 255);
}

// Get a color appropriate for current display mode
inline ImU32 GetUIColor(ImU32 sdr_color, float target_nits = 120.0f) {
    if (HDROutputManager::Instance().IsHDRActive()) {
        return Col32ToHDR(sdr_color, target_nits);
    }
    return sdr_color;
}

// Get ImVec4 color appropriate for current display mode
inline ImVec4 GetUIColorVec4(const ImVec4& sdr_color, float target_nits = 120.0f) {
    if (HDROutputManager::Instance().IsHDRActive()) {
        return ColorToHDR(sdr_color, target_nits);
    }
    return sdr_color;
}

} // namespace ump

#endif // _WIN32
