#pragma once

#ifdef _WIN32

#include <d3d11_1.h>
#include <wrl/client.h>
#include <glad/gl.h>
#include <mutex>
#include <vector>

namespace ump {

//=============================================================================
// D3D11VideoInterop
//
// Manages D3D11 to OpenGL texture sharing for decoded video frames.
// This is the "reverse" of the HDR output interop (GL->D3D11).
//
// Flow:
// 1. D3D11 YUV renderer outputs to shared D3D11 texture
// 2. D3D11VideoInterop shares that texture to OpenGL
// 3. OpenGL pipeline uses the texture for OCIO, ImGui, etc.
//
// Supports:
// - NVIDIA: WGL_NV_DX_interop2 (zero-copy)
// - Intel/AMD: EXT_external_objects (zero-copy)
// - Fallback: CPU staging texture copy
//=============================================================================

class D3D11VideoInterop {
public:
    D3D11VideoInterop();
    ~D3D11VideoInterop();

    // Delete copy/move
    D3D11VideoInterop(const D3D11VideoInterop&) = delete;
    D3D11VideoInterop& operator=(const D3D11VideoInterop&) = delete;

    //=========================================================================
    // Initialization
    //=========================================================================

    // Initialize with D3D11 device
    bool Initialize(ID3D11Device* d3d_device);

    // Shutdown and release all resources
    void Shutdown();

    // Check if initialized
    bool IsInitialized() const { return initialized_; }

    //=========================================================================
    // Shared Texture Management
    //=========================================================================

    // Create a D3D11 texture that can be shared to OpenGL
    // format: DXGI_FORMAT (e.g., DXGI_FORMAT_R16G16B16A16_FLOAT for HDR)
    bool CreateSharedTexture(int width, int height, DXGI_FORMAT format);

    // Destroy the shared texture
    void DestroySharedTexture();

    // Get current texture dimensions
    int GetWidth() const { return width_; }
    int GetHeight() const { return height_; }

    //=========================================================================
    // D3D11 Access (for YUV renderer output)
    //=========================================================================

    // Get the D3D11 texture (for use as render target)
    ID3D11Texture2D* GetD3D11Texture() const { return shared_texture_.Get(); }

    // Get the D3D11 render target view (for YUV shader output)
    ID3D11RenderTargetView* GetRTV() const { return rtv_.Get(); }

    // Get the D3D11 shader resource view (if needed for preview)
    ID3D11ShaderResourceView* GetSRV() const { return srv_.Get(); }

    //=========================================================================
    // OpenGL Access (for existing pipeline)
    //=========================================================================

    // Get the OpenGL texture ID (for use in existing OpenGL pipeline)
    GLuint GetGLTexture() const { return gl_texture_; }

    //=========================================================================
    // Synchronization
    //=========================================================================

    // Lock for D3D11 rendering (before YUV shader renders)
    // Returns true if lock succeeded
    bool LockForD3D11();

    // Unlock for OpenGL use (after YUV shader completes, before GL uses texture)
    // Returns true if unlock succeeded
    bool UnlockForGL();

    // Check current lock state
    bool IsLockedForD3D11() const { return locked_for_d3d11_; }

    //=========================================================================
    // Triple-Buffer Management
    //=========================================================================

    // Advance to next buffer in ring (call after each frame completes)
    void AdvanceBuffers();

    // Get current write index (for debugging)
    int GetWriteIndex() const { return write_index_; }

    // Get current read index (for debugging)
    int GetReadIndex() const { return read_index_; }

    //=========================================================================
    // Interop Status
    //=========================================================================

    // Check if zero-copy interop is available
    bool HasZeroCopyInterop() const { return has_nv_interop_ || has_ext_memory_; }

    // Check specific interop method
    bool HasNVInterop() const { return has_nv_interop_; }
    bool HasEXTMemory() const { return has_ext_memory_; }

    // Get interop method name for debug display
    const char* GetInteropMethodName() const;

    // Try to reinitialize zero-copy interop if currently using CPU fallback
    // Call this from render thread when GL context is definitely current
    bool TryReinitializeZeroCopy();

    //=========================================================================
    // Thread-Safe GL Resource Cleanup
    //=========================================================================

    // Queue GL texture for deletion from any thread (thread-safe)
    // Must call ProcessPendingGLDeletions() from main thread to actually delete
    static void QueueGLTextureDeletion(GLuint texture);

    // Process pending GL texture deletions - MUST be called from main/GL thread
    static void ProcessPendingGLDeletions();

private:
    //=========================================================================
    // Interop Initialization
    //=========================================================================

    bool InitializeNVInterop();
    bool InitializeEXTMemoryInterop();
    void ShutdownNVInterop();
    void ShutdownEXTMemoryInterop();

    bool CreateNVInteropTexture(int width, int height, DXGI_FORMAT format);
    bool CreateEXTMemoryTexture(int width, int height, DXGI_FORMAT format);
    bool CreateFallbackTexture(int width, int height, DXGI_FORMAT format);

    void DestroyNVInteropTexture();
    void DestroyEXTMemoryTexture();
    void DestroyFallbackTexture();

    //=========================================================================
    // WGL_NV_DX_interop function pointers
    //=========================================================================

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

    //=========================================================================
    // GL_EXT_memory_object function pointers
    //=========================================================================

    using PFNGLCREATEMEMORYOBJECTSEXTPROC = void(*)(GLsizei, GLuint*);
    using PFNGLDELETEMEMORYOBJECTSEXTPROC = void(*)(GLsizei, const GLuint*);
    using PFNGLTEXSTORAGEMEM2DEXTPROC = void(*)(GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLuint, GLuint64);
    using PFNGLIMPORTMEMORYWIN32HANDLEEXTPROC = void(*)(GLuint, GLuint64, GLenum, void*);

    PFNGLCREATEMEMORYOBJECTSEXTPROC glCreateMemoryObjectsEXT_ = nullptr;
    PFNGLDELETEMEMORYOBJECTSEXTPROC glDeleteMemoryObjectsEXT_ = nullptr;
    PFNGLTEXSTORAGEMEM2DEXTPROC glTexStorageMem2DEXT_ = nullptr;
    PFNGLIMPORTMEMORYWIN32HANDLEEXTPROC glImportMemoryWin32HandleEXT_ = nullptr;

    //=========================================================================
    // State
    //=========================================================================

    Microsoft::WRL::ComPtr<ID3D11Device> d3d_device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3d_context_;

    // Triple-buffer constants
    static constexpr int kInteropBufferCount = 3;

    // Triple-buffered shared textures (D3D11 side)
    Microsoft::WRL::ComPtr<ID3D11Texture2D> shared_textures_[kInteropBufferCount];
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtvs_[kInteropBufferCount];
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srvs_[kInteropBufferCount];

    // Triple-buffered OpenGL textures
    GLuint gl_textures_[kInteropBufferCount] = {0, 0, 0};

    // Triple-buffered NV_DX_interop handles
    HANDLE nv_interop_device_ = nullptr;
    HANDLE nv_interop_objects_[kInteropBufferCount] = {nullptr, nullptr, nullptr};

    // Triple-buffer ring indices
    int write_index_ = 0;  // D3D11 writes here (current render target)
    int read_index_ = 2;   // GL reads here (2 frames behind write for max parallelism)

    // Legacy single-buffer pointers (point into arrays for API compatibility)
    Microsoft::WRL::ComPtr<ID3D11Texture2D> shared_texture_;  // Points to shared_textures_[write_index_]
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv_;      // Points to rtvs_[write_index_]
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv_;    // Points to srvs_[write_index_]
    GLuint gl_texture_ = 0;  // Points to gl_textures_[read_index_]

    // EXT_memory_object handles (per-buffer)
    GLuint ext_memory_objects_[kInteropBufferCount] = {0, 0, 0};
    HANDLE shared_handles_[kInteropBufferCount] = {nullptr, nullptr, nullptr};

    // Fallback staging texture
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging_texture_;

    int width_ = 0;
    int height_ = 0;
    DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;

    bool initialized_ = false;
    bool has_nv_interop_ = false;
    bool has_ext_memory_ = false;
    bool locked_for_d3d11_ = false;

    // Track which buffers are locked
    bool buffer_locked_[kInteropBufferCount] = {false, false, false};

    //=========================================================================
    // Static thread-safe deletion queue
    //=========================================================================
    static std::mutex s_deletion_mutex_;
    static std::vector<GLuint> s_pending_gl_deletions_;
};

} // namespace ump

#endif // _WIN32
