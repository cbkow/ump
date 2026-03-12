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

namespace qcview {

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

// HDR color math + runtime helpers are in hdr_color_utils.h (cross-platform).
// On Windows, IsHDROutputActive() delegates to HDROutputManager::Instance().IsHDRActive().
#include "hdr_color_utils.h"

} // namespace qcview

#endif // _WIN32
