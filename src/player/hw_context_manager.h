#pragma once

#include <mutex>
#include <atomic>

// Forward declarations for FFmpeg types
struct AVBufferRef;

namespace ump {

//=============================================================================
// Hardware Device Context Manager
//
// Singleton that manages shared hardware device contexts for all supported
// acceleration types (CUDA, D3D11VA, QSV, DXVA2).
//
// Instead of each StreamingVideoDecoder creating and destroying its own
// hardware context (which causes nvcuvid.dll load/unload cycling and
// GPU resource contention with OpenGL), we create ONE shared context per
// hardware type that persists for the application lifetime.
//
// Benefits:
// - Eliminates nvcuvid.dll load/unload cycling during seeks
// - Reduces GPU resource contention between CUDA and OpenGL
// - Faster decoder initialization (context already created)
// - Fixes ImGui flickering caused by GPU synchronization stalls
//
// Usage:
//   auto hw_ctx = HWContextManager::Instance().GetContext(AV_HWDEVICE_TYPE_CUDA);
//   if (hw_ctx) {
//       codec_ctx->hw_device_ctx = av_buffer_ref(hw_ctx);
//   }
//=============================================================================

class HWContextManager {
public:
    // Singleton access
    static HWContextManager& Instance();

    // Delete copy/move
    HWContextManager(const HWContextManager&) = delete;
    HWContextManager& operator=(const HWContextManager&) = delete;
    HWContextManager(HWContextManager&&) = delete;
    HWContextManager& operator=(HWContextManager&&) = delete;

    //=========================================================================
    // Generic Context Access
    //=========================================================================

    // Get shared context for any hardware type (creates on first use)
    // hw_type is AVHWDeviceType cast to int
    // Returns nullptr if hardware type is not available
    // Caller should use av_buffer_ref() to get a reference
    AVBufferRef* GetContext(int hw_type);

    // Get pixel format for hardware type (-1 if not available)
    int GetPixelFormat(int hw_type) const;

    // Check if hardware type context is available
    bool HasContext(int hw_type) const;

    //=========================================================================
    // Convenience Methods (type-safe wrappers)
    //=========================================================================

    AVBufferRef* GetCUDAContext();
    AVBufferRef* GetD3D11VAContext();
    AVBufferRef* GetQSVContext();
    AVBufferRef* GetDXVA2Context();

    int GetCUDAPixelFormat() const { return cuda_pix_fmt_; }
    int GetD3D11VAPixelFormat() const { return d3d11va_pix_fmt_; }
    int GetQSVPixelFormat() const { return qsv_pix_fmt_; }
    int GetDXVA2PixelFormat() const { return dxva2_pix_fmt_; }

    bool HasCUDA() const { return cuda_initialized_.load() && cuda_ctx_ != nullptr; }
    bool HasD3D11VA() const { return d3d11va_initialized_.load() && d3d11va_ctx_ != nullptr; }
    bool HasQSV() const { return qsv_initialized_.load() && qsv_ctx_ != nullptr; }
    bool HasDXVA2() const { return dxva2_initialized_.load() && dxva2_ctx_ != nullptr; }

    //=========================================================================
    // Lifecycle
    //=========================================================================

    // Release all contexts (call during application shutdown)
    void Shutdown();

private:
    HWContextManager();
    ~HWContextManager();

    // Initialize specific hardware contexts (thread-safe, called lazily)
    void InitializeCUDA();
    void InitializeD3D11VA();
    void InitializeQSV();
    void InitializeDXVA2();

    // CUDA context (NVIDIA NVDEC)
    AVBufferRef* cuda_ctx_ = nullptr;
    int cuda_pix_fmt_ = -1;
    std::atomic<bool> cuda_initialized_{false};
    std::mutex cuda_mutex_;

    // D3D11VA context (Windows 8+, works with NVIDIA/Intel/AMD)
    AVBufferRef* d3d11va_ctx_ = nullptr;
    int d3d11va_pix_fmt_ = -1;
    std::atomic<bool> d3d11va_initialized_{false};
    std::mutex d3d11va_mutex_;

    // QSV context (Intel Quick Sync Video)
    AVBufferRef* qsv_ctx_ = nullptr;
    int qsv_pix_fmt_ = -1;
    std::atomic<bool> qsv_initialized_{false};
    std::mutex qsv_mutex_;

    // DXVA2 context (Legacy DirectX 9 acceleration)
    AVBufferRef* dxva2_ctx_ = nullptr;
    int dxva2_pix_fmt_ = -1;
    std::atomic<bool> dxva2_initialized_{false};
    std::mutex dxva2_mutex_;

    // Global shutdown flag
    std::atomic<bool> shutdown_{false};
};

} // namespace ump
