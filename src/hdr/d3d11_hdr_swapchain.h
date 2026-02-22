#pragma once

#ifdef _WIN32

#include <d3d11_1.h>
#include <dxgi1_6.h>
#include <dcomp.h>
#include <wrl/client.h>
#include <memory>
#include <string>
#include <mutex>
#include <atomic>
#include <vector>
#include <array>
#include <chrono>

// OpenGL types for interop (avoid including glad/gl.h in header)
#ifndef __gl_h_
typedef unsigned int GLuint;
typedef unsigned int GLenum;
typedef int GLint;
typedef signed long long GLint64;
typedef unsigned long long GLuint64;
#endif

namespace qcview {

//=============================================================================
// D3D11 HDR Swapchain
//
// Manages a D3D11 swapchain that can present HDR content using:
// - DXGI_FORMAT_R10G10B10A2_UNORM for HDR10 (PQ/ST.2084)
// - DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 colorspace
//
// Zero-copy rendering paths (in priority order):
// 1. WGL_NV_DX_interop2 (NVIDIA) - Mature, well-tested
// 2. EXT_external_objects (Intel Arc, AMD RDNA+, NVIDIA) - Cross-vendor
// 3. CPU readback fallback - Universal but slow
//
// Usage:
//   swapchain.BeginFrame();   // Binds render target
//   // ... render with OpenGL/ImGui ...
//   swapchain.EndFrame();     // Presents
//=============================================================================

// Interop method being used
enum class InteropMethod {
    None,           // No interop available, using CPU fallback
    NV_DX_Interop,  // NVIDIA WGL_NV_DX_interop2
    EXT_Memory,     // EXT_external_objects_win32 (Intel, AMD, NVIDIA)
};

struct HDRDisplayInfo {
    bool hdr_supported = false;        // Display supports HDR
    bool hdr_enabled = false;          // Windows HDR mode is ON
    float min_luminance = 0.0f;        // Display min luminance (nits)
    float max_luminance = 0.0f;        // Display max luminance (nits)
    float max_full_frame_luminance = 0.0f;
    std::wstring display_name;
    DXGI_COLOR_SPACE_TYPE current_colorspace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
};

class D3D11HDRSwapchain {
public:
    D3D11HDRSwapchain();
    ~D3D11HDRSwapchain();

    // Delete copy/move
    D3D11HDRSwapchain(const D3D11HDRSwapchain&) = delete;
    D3D11HDRSwapchain& operator=(const D3D11HDRSwapchain&) = delete;

    //=========================================================================
    // Initialization
    //=========================================================================

    bool Initialize(HWND hwnd, int width, int height);
    void Shutdown();
    bool Resize(int width, int height);

    //=========================================================================
    // HDR Control
    //=========================================================================

    const HDRDisplayInfo& GetHDRInfo() const { return hdr_info_; }
    bool IsHDRSupported() const { return hdr_info_.hdr_supported; }
    bool IsHDREnabled() const { return hdr_info_.hdr_enabled; }
    void RefreshHDRStatus();
    bool SetColorSpace(DXGI_COLOR_SPACE_TYPE colorspace);

    //=========================================================================
    // Frame Rendering (GL-D3D11 Interop Mode)
    //=========================================================================

    GLuint BeginFrame();
    void EndFrame(bool vsync = true);
    GLuint GetRenderTargetFBO() const { return render_fbo_; }

    //=========================================================================
    // Frame Rendering (Full D3D11 Mode)
    // For full D3D11 rendering without OpenGL
    //=========================================================================

    // Get the backbuffer RTV for direct D3D11 rendering
    ID3D11RenderTargetView* GetBackBufferRTV() const { return rtv_.Get(); }

    // Begin a D3D11-only frame (no GL interop, binds backbuffer as render target)
    void BeginD3D11Frame();

    // End a D3D11-only frame (presents swapchain)
    void EndD3D11Frame(bool vsync = true);

    // Get device/context for direct D3D11 operations
    ID3D11Device1* GetDevice() const { return device_.Get(); }
    ID3D11DeviceContext1* GetContext() const { return context_.Get(); }

    //=========================================================================
    // Legacy: Copy-based presentation
    //=========================================================================

    bool CopyFromOpenGLAndPresent(GLuint gl_texture, int tex_width, int tex_height, bool vsync = true);

    //=========================================================================
    // State Queries
    //=========================================================================

    int GetWidth() const { return width_; }
    int GetHeight() const { return height_; }
    bool IsInitialized() const { return initialized_; }
    bool IsInFrame() const { return in_frame_; }

    // Interop status
    InteropMethod GetInteropMethod() const { return interop_method_; }
    bool HasZeroCopyInterop() const { return interop_method_ != InteropMethod::None; }
    bool HasNVInterop() const { return interop_method_ == InteropMethod::NV_DX_Interop; }
    bool HasEXTMemoryInterop() const { return interop_method_ == InteropMethod::EXT_Memory; }

    //=========================================================================
    // Debug/Statistics
    //=========================================================================

    struct Stats {
        uint64_t frames_presented = 0;
        uint64_t zero_copy_frames = 0;
        uint64_t cpu_readback_frames = 0;
        double last_present_time_ms = 0.0;
        double last_copy_time_ms = 0.0;
    };

    const Stats& GetStats() const { return stats_; }

private:
    //=========================================================================
    // D3D11/DXGI Resources
    //=========================================================================

    bool CreateD3D11Device();
    bool CreateSwapchain(HWND hwnd, int width, int height);
    bool CreateRenderTargetView();
    bool DetectHDRSupport(bool log_always = true);

    Microsoft::WRL::ComPtr<ID3D11Device1> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext1> context_;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> swapchain_;
    Microsoft::WRL::ComPtr<IDXGISwapChain4> swapchain4_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> back_buffer_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv_;
    Microsoft::WRL::ComPtr<IDXGIOutput6> output6_;

    // DirectComposition for presenting over OpenGL window
    Microsoft::WRL::ComPtr<IDCompositionDevice> dcomp_device_;
    Microsoft::WRL::ComPtr<IDCompositionTarget> dcomp_target_;
    Microsoft::WRL::ComPtr<IDCompositionVisual> dcomp_visual_;

    //=========================================================================
    // Shared Render Target (used by all interop methods)
    // Triple-buffering: 3 buffers for zero-copy interop
    //=========================================================================

    static constexpr int INTEROP_BUFFER_COUNT = 3;

    struct InteropBuffer {
        // D3D11 side
        Microsoft::WRL::ComPtr<ID3D11Texture2D> d3d_texture;

        // OpenGL side
        GLuint gl_texture = 0;
        GLuint fbo = 0;

        // NV_DX_interop handle
        HANDLE nv_interop_object = nullptr;

        // EXT_memory_object resources
        GLuint ext_memory_object = 0;
        HANDLE shared_handle = nullptr;
        Microsoft::WRL::ComPtr<IDXGIKeyedMutex> keyed_mutex;

        // State tracking
        bool initialized = false;
    };

    bool CreateSharedRenderTarget(int width, int height);
    void DestroySharedRenderTarget();

    std::array<InteropBuffer, INTEROP_BUFFER_COUNT> interop_buffers_;
    int current_write_buffer_ = 0;  // Next buffer to render into
    int current_present_buffer_ = 0;  // Last buffer sent to Present()

    // Legacy single-buffer members (for backward compatibility during migration)
    GLuint render_texture_ = 0;  // Deprecated - use interop_buffers_[].gl_texture
    GLuint render_fbo_ = 0;      // Deprecated - use interop_buffers_[].fbo

    InteropMethod interop_method_ = InteropMethod::None;

    //=========================================================================
    // NV_DX_interop (NVIDIA)
    //=========================================================================

    bool InitializeNVInterop();
    void ShutdownNVInterop();
    bool CreateNVInteropRenderTarget(int width, int height);
    void DestroyNVInteropRenderTarget();

    using PFNWGLDXOPENDEVICENV = HANDLE(WINAPI*)(void*);
    using PFNWGLDXCLOSEDEVICENV = BOOL(WINAPI*)(HANDLE);
    using PFNWGLDXREGISTEROBJECTNV = HANDLE(WINAPI*)(HANDLE, void*, GLuint, GLenum, GLenum);
    using PFNWGLDXUNREGISTEROBJECTNV = BOOL(WINAPI*)(HANDLE, HANDLE);
    using PFNWGLDXLOCKOBJECTSNV = BOOL(WINAPI*)(HANDLE, GLint, HANDLE*);
    using PFNWGLDXUNLOCKOBJECTSNV = BOOL(WINAPI*)(HANDLE, GLint, HANDLE*);

    PFNWGLDXOPENDEVICENV wglDXOpenDeviceNV_ = nullptr;
    PFNWGLDXCLOSEDEVICENV wglDXCloseDeviceNV_ = nullptr;
    PFNWGLDXREGISTEROBJECTNV wglDXRegisterObjectNV_ = nullptr;
    PFNWGLDXUNREGISTEROBJECTNV wglDXUnregisterObjectNV_ = nullptr;
    PFNWGLDXLOCKOBJECTSNV wglDXLockObjectsNV_ = nullptr;
    PFNWGLDXUNLOCKOBJECTSNV wglDXUnlockObjectsNV_ = nullptr;

    HANDLE nv_interop_device_ = nullptr;
    bool nv_interop_available_ = false;

    //=========================================================================
    // EXT_external_objects (Intel Arc, AMD, NVIDIA)
    //=========================================================================

    bool InitializeEXTMemoryInterop();
    void ShutdownEXTMemoryInterop();
    bool CreateEXTMemoryRenderTarget(int width, int height);
    void DestroyEXTMemoryRenderTarget();

    // GL_EXT_memory_object / GL_EXT_memory_object_win32
    using PFNGLCREATEMEMORYOBJECTSEXTPROC = void(*)(GLint, GLuint*);
    using PFNGLDELETEMEMORYOBJECTSEXTPROC = void(*)(GLint, const GLuint*);
    using PFNGLISMEMORYOBJECTEXTPROC = unsigned char(*)(GLuint);
    using PFNGLMEMORYOBJECTPARAMETERIVEXTPROC = void(*)(GLuint, GLenum, const GLint*);
    using PFNGLGETMEMORYOBJECTPARAMETERIVEXTPROC = void(*)(GLuint, GLenum, GLint*);
    using PFNGLTEXSTORAGEMEM2DEXTPROC = void(*)(GLenum, GLint, GLenum, GLint, GLint, GLuint, GLuint64);
    using PFNGLIMPORTMEMORYWIN32HANDLEEXTPROC = void(*)(GLuint, GLuint64, GLenum, void*);

    // GL_EXT_semaphore / GL_EXT_semaphore_win32
    using PFNGLCREATESEMAPHORESEXTPROC = void(*)(GLint, GLuint*);
    using PFNGLDELETESEMAPHORESEXTPROC = void(*)(GLint, const GLuint*);
    using PFNGLISSEMAPHOREEXTPROC = unsigned char(*)(GLuint);
    using PFNGLSEMAPHOREPARAMETERUI64VEXTPROC = void(*)(GLuint, GLenum, const GLuint64*);
    using PFNGLGETSEMAPHOREPARAMETERUI64VEXTPROC = void(*)(GLuint, GLenum, GLuint64*);
    using PFNGLWAITSEMAPHOREEXTPROC = void(*)(GLuint, GLuint, const GLuint*, GLuint, const GLuint*, const GLenum*);
    using PFNGLSIGNALSEMAPHOREEXTPROC = void(*)(GLuint, GLuint, const GLuint*, GLuint, const GLuint*, const GLenum*);
    using PFNGLIMPORTSEMAPHOREWIN32HANDLEEXTPROC = void(*)(GLuint, GLenum, void*);

    PFNGLCREATEMEMORYOBJECTSEXTPROC glCreateMemoryObjectsEXT_ = nullptr;
    PFNGLDELETEMEMORYOBJECTSEXTPROC glDeleteMemoryObjectsEXT_ = nullptr;
    PFNGLTEXSTORAGEMEM2DEXTPROC glTexStorageMem2DEXT_ = nullptr;
    PFNGLIMPORTMEMORYWIN32HANDLEEXTPROC glImportMemoryWin32HandleEXT_ = nullptr;

    PFNGLCREATESEMAPHORESEXTPROC glGenSemaphoresEXT_ = nullptr;
    PFNGLDELETESEMAPHORESEXTPROC glDeleteSemaphoresEXT_ = nullptr;
    PFNGLWAITSEMAPHOREEXTPROC glWaitSemaphoreEXT_ = nullptr;
    PFNGLSIGNALSEMAPHOREEXTPROC glSignalSemaphoreEXT_ = nullptr;
    PFNGLIMPORTSEMAPHOREWIN32HANDLEEXTPROC glImportSemaphoreWin32HandleEXT_ = nullptr;

    bool ext_memory_available_ = false;

    //=========================================================================
    // Fallback: CPU Readback Path
    //=========================================================================

    bool CreateFallbackResources(int width, int height);
    void DestroyFallbackResources();
    bool CopyViaFallback();

    GLuint fallback_texture_ = 0;
    GLuint fallback_fbo_ = 0;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging_texture_;
    std::vector<uint8_t> readback_buffer_;

    //=========================================================================
    // State
    //=========================================================================

    HWND hwnd_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    bool initialized_ = false;
    bool in_frame_ = false;
    DXGI_FORMAT swapchain_format_ = DXGI_FORMAT_R10G10B10A2_UNORM;

    HDRDisplayInfo hdr_info_;
    Stats stats_;

    std::mutex mutex_;
};

} // namespace qcview

#endif // _WIN32
