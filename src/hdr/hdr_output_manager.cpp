#include "hdr_output_manager.h"

#ifdef _WIN32

#include "d3d11_hdr_swapchain.h"
#include "../utils/debug_utils.h"

namespace qcview {

HDROutputManager& HDROutputManager::Instance() {
    static HDROutputManager instance;
    return instance;
}

HDROutputManager::HDROutputManager() = default;

HDROutputManager::~HDROutputManager() {
    Shutdown();
}

bool HDROutputManager::Initialize(HWND hwnd, int width, int height) {
    if (initialized_.load()) {
        Debug::Log("HDROutputManager: Already initialized");
        return true;
    }

    swapchain_ = std::make_unique<D3D11HDRSwapchain>();

    if (!swapchain_->Initialize(hwnd, width, height)) {
        Debug::Log("HDROutputManager: Failed to initialize D3D11 swapchain");
        swapchain_.reset();
        return false;
    }

    initialized_.store(true);
    last_hdr_check_ = std::chrono::steady_clock::now();

    const auto& info = swapchain_->GetHDRInfo();
    std::string mode = swapchain_->HasNVInterop() ? "zero-copy" : "CPU readback";

    if (info.hdr_enabled) {
        Debug::Log("HDROutputManager: Initialized with HDR enabled (" + mode + ")");
    } else if (info.hdr_supported) {
        Debug::Log("HDROutputManager: Initialized - HDR supported but not enabled (" + mode + ")");
    } else {
        Debug::Log("HDROutputManager: Initialized in SDR mode (" + mode + ")");
    }

    return true;
}

void HDROutputManager::Shutdown() {
    if (!initialized_.load()) {
        return;
    }

    if (swapchain_) {
        swapchain_->Shutdown();
        swapchain_.reset();
    }

    initialized_.store(false);
    Debug::Log("HDROutputManager: Shutdown complete");
}

void HDROutputManager::OnResize(int width, int height) {
    if (!initialized_.load() || !swapchain_) {
        return;
    }

    swapchain_->Resize(width, height);
}

bool HDROutputManager::IsHDRSupported() const {
    if (!initialized_.load() || !swapchain_) {
        return false;
    }
    return swapchain_->IsHDRSupported();
}

bool HDROutputManager::IsHDRActive() const {
    if (!initialized_.load() || !swapchain_) {
        return false;
    }
    if (bypass_mode_.load()) {
        return false;
    }
    return swapchain_->IsHDREnabled();
}

bool HDROutputManager::IsInitialized() const {
    return initialized_.load() && swapchain_ && swapchain_->IsInitialized();
}

bool HDROutputManager::IsZeroCopyEnabled() const {
    if (!initialized_.load() || !swapchain_) {
        return false;
    }
    return swapchain_->HasZeroCopyInterop();
}

const char* HDROutputManager::GetInteropMethodName() const {
    if (!initialized_.load() || !swapchain_) {
        return "None";
    }
    switch (swapchain_->GetInteropMethod()) {
        case InteropMethod::NV_DX_Interop: return "NV_DX_interop (NVIDIA)";
        case InteropMethod::EXT_Memory: return "EXT_memory_object (Intel/AMD)";
        default: return "CPU Fallback";
    }
}

const HDRDisplayInfo* HDROutputManager::GetHDRInfo() const {
    if (!initialized_.load() || !swapchain_) {
        return nullptr;
    }
    return &swapchain_->GetHDRInfo();
}

void HDROutputManager::RefreshHDRStatus() {
    if (!initialized_.load() || !swapchain_) {
        return;
    }

    auto now = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(now - last_hdr_check_).count();

    if (elapsed >= HDR_CHECK_INTERVAL_SEC) {
        swapchain_->RefreshHDRStatus();
        last_hdr_check_ = now;
    }
}

void HDROutputManager::SetBypassMode(bool bypass) {
    bool was_bypass = bypass_mode_.exchange(bypass);
    if (was_bypass != bypass) {
        Debug::Log("HDROutputManager: Bypass mode " + std::string(bypass ? "enabled" : "disabled"));
    }
}

GLuint HDROutputManager::BeginFrame() {
    if (!initialized_.load() || !swapchain_) {
        return 0;
    }

    if (bypass_mode_.load()) {
        return 0;  // Caller should render to default framebuffer
    }

    // Periodically check for HDR mode changes
    RefreshHDRStatus();

    return swapchain_->BeginFrame();
}

void HDROutputManager::EndFrame(bool vsync) {
    if (!initialized_.load() || !swapchain_) {
        return;
    }

    if (bypass_mode_.load()) {
        return;  // Caller should use glfwSwapBuffers
    }

    swapchain_->EndFrame(vsync);
}

bool HDROutputManager::IsInFrame() const {
    if (!initialized_.load() || !swapchain_) {
        return false;
    }
    return swapchain_->IsInFrame();
}

bool HDROutputManager::PresentFrame(GLuint gl_texture, int width, int height, bool vsync) {
    if (!initialized_.load() || !swapchain_) {
        return false;
    }

    if (bypass_mode_.load()) {
        return false;
    }

    RefreshHDRStatus();
    return swapchain_->CopyFromOpenGLAndPresent(gl_texture, width, height, vsync);
}

//=============================================================================
// Full D3D11 Mode (No GL Interop)
//=============================================================================

void HDROutputManager::BeginD3D11Frame() {
    if (!initialized_.load() || !swapchain_) {
        return;
    }

    if (bypass_mode_.load()) {
        return;
    }

    RefreshHDRStatus();
    swapchain_->BeginD3D11Frame();
}

void HDROutputManager::EndD3D11Frame(bool vsync) {
    if (!initialized_.load() || !swapchain_) {
        return;
    }

    if (bypass_mode_.load()) {
        return;
    }

    swapchain_->EndD3D11Frame(vsync);
}

ID3D11RenderTargetView* HDROutputManager::GetBackBufferRTV() const {
    if (!initialized_.load() || !swapchain_) {
        return nullptr;
    }
    return swapchain_->GetBackBufferRTV();
}

ID3D11Device1* HDROutputManager::GetDevice() const {
    if (!initialized_.load() || !swapchain_) {
        return nullptr;
    }
    return swapchain_->GetDevice();
}

ID3D11DeviceContext1* HDROutputManager::GetContext() const {
    if (!initialized_.load() || !swapchain_) {
        return nullptr;
    }
    return swapchain_->GetContext();
}

//=============================================================================
// Statistics
//=============================================================================

HDROutputManager::Stats HDROutputManager::GetStats() const {
    Stats stats = {};

    if (!initialized_.load() || !swapchain_) {
        return stats;
    }

    const auto& sc_stats = swapchain_->GetStats();
    stats.total_frames = sc_stats.frames_presented;
    stats.zero_copy_frames = sc_stats.zero_copy_frames;
    stats.cpu_readback_frames = sc_stats.cpu_readback_frames;
    stats.using_zero_copy = swapchain_->HasZeroCopyInterop();
    stats.last_present_ms = sc_stats.last_present_time_ms;

    // HDR vs SDR frames based on colorspace
    if (swapchain_->IsHDREnabled()) {
        stats.hdr_frames = sc_stats.frames_presented;
    } else {
        stats.sdr_frames = sc_stats.frames_presented;
    }

    return stats;
}

} // namespace qcview

#endif // _WIN32
