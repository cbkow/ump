#include "hw_context_manager.h"
#include "../utils/debug_utils.h"

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/buffer.h>
#include <libavcodec/avcodec.h>
}

namespace qcview {

//=============================================================================
// Singleton Instance
//=============================================================================

HWContextManager& HWContextManager::Instance() {
    static HWContextManager instance;
    return instance;
}

//=============================================================================
// Constructor / Destructor
//=============================================================================

HWContextManager::HWContextManager() {
    Debug::Log("HWContextManager: Initialized (contexts created on demand)");
}

HWContextManager::~HWContextManager() {
    Shutdown();
}

//=============================================================================
// Generic Context Access
//=============================================================================

AVBufferRef* HWContextManager::GetContext(int hw_type) {
    AVHWDeviceType type = static_cast<AVHWDeviceType>(hw_type);
    switch (type) {
        case AV_HWDEVICE_TYPE_CUDA:    return GetCUDAContext();
        case AV_HWDEVICE_TYPE_D3D11VA: return GetD3D11VAContext();
        case AV_HWDEVICE_TYPE_QSV:     return GetQSVContext();
        case AV_HWDEVICE_TYPE_DXVA2:   return GetDXVA2Context();
        case AV_HWDEVICE_TYPE_VAAPI:   return GetVAAPIContext();
        default:                        return nullptr;
    }
}

int HWContextManager::GetPixelFormat(int hw_type) const {
    AVHWDeviceType type = static_cast<AVHWDeviceType>(hw_type);
    switch (type) {
        case AV_HWDEVICE_TYPE_CUDA:    return cuda_pix_fmt_;
        case AV_HWDEVICE_TYPE_D3D11VA: return d3d11va_pix_fmt_;
        case AV_HWDEVICE_TYPE_QSV:     return qsv_pix_fmt_;
        case AV_HWDEVICE_TYPE_DXVA2:   return dxva2_pix_fmt_;
        case AV_HWDEVICE_TYPE_VAAPI:   return vaapi_pix_fmt_;
        default:                        return -1;
    }
}

bool HWContextManager::HasContext(int hw_type) const {
    AVHWDeviceType type = static_cast<AVHWDeviceType>(hw_type);
    switch (type) {
        case AV_HWDEVICE_TYPE_CUDA:    return HasCUDA();
        case AV_HWDEVICE_TYPE_D3D11VA: return HasD3D11VA();
        case AV_HWDEVICE_TYPE_QSV:     return HasQSV();
        case AV_HWDEVICE_TYPE_DXVA2:   return HasDXVA2();
        case AV_HWDEVICE_TYPE_VAAPI:   return HasVAAPI();
        default:                        return false;
    }
}

//=============================================================================
// Type-Specific Context Access
//=============================================================================

AVBufferRef* HWContextManager::GetCUDAContext() {
    if (shutdown_.load()) return nullptr;

    // Fast path: already initialized
    if (cuda_initialized_.load()) {
        return cuda_ctx_;
    }

    // Slow path: initialize (thread-safe)
    std::lock_guard<std::mutex> lock(cuda_mutex_);

    // Double-check after acquiring lock
    if (cuda_initialized_.load()) {
        return cuda_ctx_;
    }

    InitializeCUDA();
    return cuda_ctx_;
}

AVBufferRef* HWContextManager::GetD3D11VAContext() {
    if (shutdown_.load()) return nullptr;

    if (d3d11va_initialized_.load()) {
        return d3d11va_ctx_;
    }

    std::lock_guard<std::mutex> lock(d3d11va_mutex_);

    if (d3d11va_initialized_.load()) {
        return d3d11va_ctx_;
    }

    InitializeD3D11VA();
    return d3d11va_ctx_;
}

AVBufferRef* HWContextManager::GetQSVContext() {
    if (shutdown_.load()) return nullptr;

    if (qsv_initialized_.load()) {
        return qsv_ctx_;
    }

    std::lock_guard<std::mutex> lock(qsv_mutex_);

    if (qsv_initialized_.load()) {
        return qsv_ctx_;
    }

    InitializeQSV();
    return qsv_ctx_;
}

AVBufferRef* HWContextManager::GetDXVA2Context() {
    if (shutdown_.load()) return nullptr;

    if (dxva2_initialized_.load()) {
        return dxva2_ctx_;
    }

    std::lock_guard<std::mutex> lock(dxva2_mutex_);

    if (dxva2_initialized_.load()) {
        return dxva2_ctx_;
    }

    InitializeDXVA2();
    return dxva2_ctx_;
}

AVBufferRef* HWContextManager::GetVAAPIContext() {
    if (shutdown_.load()) return nullptr;

    if (vaapi_initialized_.load()) {
        return vaapi_ctx_;
    }

    std::lock_guard<std::mutex> lock(vaapi_mutex_);

    if (vaapi_initialized_.load()) {
        return vaapi_ctx_;
    }

    InitializeVAAPI();
    return vaapi_ctx_;
}

//=============================================================================
// Context Initialization
//=============================================================================

void HWContextManager::InitializeCUDA() {
    Debug::Log("HWContextManager: Creating shared CUDA device context...");

    int ret = av_hwdevice_ctx_create(&cuda_ctx_, AV_HWDEVICE_TYPE_CUDA, nullptr, nullptr, 0);
    if (ret < 0) {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        Debug::Log("HWContextManager: CUDA not available: " + std::string(errbuf));
        cuda_ctx_ = nullptr;
        cuda_pix_fmt_ = -1;
    } else {
        cuda_pix_fmt_ = static_cast<int>(AV_PIX_FMT_CUDA);
        Debug::Log("HWContextManager: CUDA context created successfully");
    }

    cuda_initialized_ = true;
}

void HWContextManager::InitializeD3D11VA() {
    Debug::Log("HWContextManager: Creating shared D3D11VA device context...");

    int ret = av_hwdevice_ctx_create(&d3d11va_ctx_, AV_HWDEVICE_TYPE_D3D11VA, nullptr, nullptr, 0);
    if (ret < 0) {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        Debug::Log("HWContextManager: D3D11VA not available: " + std::string(errbuf));
        d3d11va_ctx_ = nullptr;
        d3d11va_pix_fmt_ = -1;
    } else {
        d3d11va_pix_fmt_ = static_cast<int>(AV_PIX_FMT_D3D11);
        Debug::Log("HWContextManager: D3D11VA context created successfully");
    }

    d3d11va_initialized_ = true;
}

void HWContextManager::InitializeQSV() {
    Debug::Log("HWContextManager: Creating shared QSV device context...");

    int ret = av_hwdevice_ctx_create(&qsv_ctx_, AV_HWDEVICE_TYPE_QSV, nullptr, nullptr, 0);
    if (ret < 0) {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        Debug::Log("HWContextManager: QSV not available: " + std::string(errbuf));
        qsv_ctx_ = nullptr;
        qsv_pix_fmt_ = -1;
    } else {
        qsv_pix_fmt_ = static_cast<int>(AV_PIX_FMT_QSV);
        Debug::Log("HWContextManager: QSV context created successfully");
    }

    qsv_initialized_ = true;
}

void HWContextManager::InitializeDXVA2() {
    Debug::Log("HWContextManager: Creating shared DXVA2 device context...");

    int ret = av_hwdevice_ctx_create(&dxva2_ctx_, AV_HWDEVICE_TYPE_DXVA2, nullptr, nullptr, 0);
    if (ret < 0) {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        Debug::Log("HWContextManager: DXVA2 not available: " + std::string(errbuf));
        dxva2_ctx_ = nullptr;
        dxva2_pix_fmt_ = -1;
    } else {
        dxva2_pix_fmt_ = static_cast<int>(AV_PIX_FMT_DXVA2_VLD);
        Debug::Log("HWContextManager: DXVA2 context created successfully");
    }

    dxva2_initialized_ = true;
}

void HWContextManager::InitializeVAAPI() {
    Debug::Log("HWContextManager: Creating shared VA-API device context...");

    int ret = av_hwdevice_ctx_create(&vaapi_ctx_, AV_HWDEVICE_TYPE_VAAPI, nullptr, nullptr, 0);
    if (ret < 0) {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        Debug::Log("HWContextManager: VA-API not available: " + std::string(errbuf));
        vaapi_ctx_ = nullptr;
        vaapi_pix_fmt_ = -1;
    } else {
        vaapi_pix_fmt_ = static_cast<int>(AV_PIX_FMT_VAAPI);
        Debug::Log("HWContextManager: VA-API context created successfully");
    }

    vaapi_initialized_ = true;
}

//=============================================================================
// Lifecycle
//=============================================================================

void HWContextManager::Shutdown() {
    if (shutdown_.exchange(true)) {
        return;  // Already shut down
    }

    Debug::Log("HWContextManager: Shutting down...");

    // Free CUDA context
    {
        std::lock_guard<std::mutex> lock(cuda_mutex_);
        if (cuda_ctx_) {
            av_buffer_unref(&cuda_ctx_);
            cuda_ctx_ = nullptr;
            Debug::Log("HWContextManager: CUDA context released");
        }
    }

    // Free D3D11VA context
    {
        std::lock_guard<std::mutex> lock(d3d11va_mutex_);
        if (d3d11va_ctx_) {
            av_buffer_unref(&d3d11va_ctx_);
            d3d11va_ctx_ = nullptr;
            Debug::Log("HWContextManager: D3D11VA context released");
        }
    }

    // Free QSV context
    {
        std::lock_guard<std::mutex> lock(qsv_mutex_);
        if (qsv_ctx_) {
            av_buffer_unref(&qsv_ctx_);
            qsv_ctx_ = nullptr;
            Debug::Log("HWContextManager: QSV context released");
        }
    }

    // Free DXVA2 context
    {
        std::lock_guard<std::mutex> lock(dxva2_mutex_);
        if (dxva2_ctx_) {
            av_buffer_unref(&dxva2_ctx_);
            dxva2_ctx_ = nullptr;
            Debug::Log("HWContextManager: DXVA2 context released");
        }
    }

    // Free VAAPI context
    {
        std::lock_guard<std::mutex> lock(vaapi_mutex_);
        if (vaapi_ctx_) {
            av_buffer_unref(&vaapi_ctx_);
            vaapi_ctx_ = nullptr;
            Debug::Log("HWContextManager: VA-API context released");
        }
    }

    Debug::Log("HWContextManager: Shutdown complete");
}

} // namespace qcview
