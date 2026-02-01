#include "d3d11_video_interop.h"

#ifdef _WIN32

#include "../utils/debug_utils.h"
#include <dxgi1_2.h>

// WGL_NV_DX_interop access modes
#define WGL_ACCESS_READ_ONLY_NV 0x0000
#define WGL_ACCESS_READ_WRITE_NV 0x0001
#define WGL_ACCESS_WRITE_DISCARD_NV 0x0002

// GL_EXT_memory_object constants
#define GL_HANDLE_TYPE_D3D11_IMAGE_EXT 0x958B
#define GL_HANDLE_TYPE_D3D11_IMAGE_KMT_EXT 0x958C
#define GL_DEDICATED_MEMORY_OBJECT_EXT 0x9581

namespace ump {

D3D11VideoInterop::D3D11VideoInterop() = default;

D3D11VideoInterop::~D3D11VideoInterop() {
    Shutdown();
}

bool D3D11VideoInterop::Initialize(ID3D11Device* d3d_device) {
    if (initialized_) {
        return true;
    }

    if (!d3d_device) {
        Debug::Log("D3D11VideoInterop: D3D11 device is null");
        return false;
    }

    d3d_device_ = d3d_device;
    d3d_device_->GetImmediateContext(&d3d_context_);

    // Try interop methods in priority order
    // 1. NVIDIA NV_DX_interop (most mature, well-tested)
    if (InitializeNVInterop()) {
        has_nv_interop_ = true;
        Debug::Log("D3D11VideoInterop: NV_DX_interop available");
    }

    // 2. EXT_external_objects (Intel Arc, AMD RDNA+)
    if (!has_nv_interop_ && InitializeEXTMemoryInterop()) {
        has_ext_memory_ = true;
        Debug::Log("D3D11VideoInterop: EXT_memory_object available");
    }

    if (!has_nv_interop_ && !has_ext_memory_) {
        Debug::Log("D3D11VideoInterop: No zero-copy interop available, will use CPU fallback");
    }

    initialized_ = true;
    Debug::Log("D3D11VideoInterop: Initialized (" + std::string(GetInteropMethodName()) + ")");
    return true;
}

void D3D11VideoInterop::Shutdown() {
    DestroySharedTexture();
    ShutdownNVInterop();
    ShutdownEXTMemoryInterop();

    d3d_context_.Reset();
    d3d_device_.Reset();

    has_nv_interop_ = false;
    has_ext_memory_ = false;
    initialized_ = false;
}

bool D3D11VideoInterop::InitializeNVInterop() {
    // Load WGL_NV_DX_interop function pointers
    HMODULE opengl32 = GetModuleHandleA("opengl32.dll");
    if (!opengl32) {
        return false;
    }

    auto wglGetProcAddress = reinterpret_cast<PROC(WINAPI*)(LPCSTR)>(
        GetProcAddress(opengl32, "wglGetProcAddress"));
    if (!wglGetProcAddress) {
        return false;
    }

    wglDXOpenDeviceNV_ = reinterpret_cast<PFNWGLDXOPENDEVICENV>(
        wglGetProcAddress("wglDXOpenDeviceNV"));
    wglDXCloseDeviceNV_ = reinterpret_cast<PFNWGLDXCLOSEDEVICENV>(
        wglGetProcAddress("wglDXCloseDeviceNV"));
    wglDXRegisterObjectNV_ = reinterpret_cast<PFNWGLDXREGISTEROBJECTNV>(
        wglGetProcAddress("wglDXRegisterObjectNV"));
    wglDXUnregisterObjectNV_ = reinterpret_cast<PFNWGLDXUNREGISTEROBJECTNV>(
        wglGetProcAddress("wglDXUnregisterObjectNV"));
    wglDXLockObjectsNV_ = reinterpret_cast<PFNWGLDXLOCKOBJECTSNV>(
        wglGetProcAddress("wglDXLockObjectsNV"));
    wglDXUnlockObjectsNV_ = reinterpret_cast<PFNWGLDXUNLOCKOBJECTSNV>(
        wglGetProcAddress("wglDXUnlockObjectsNV"));

    if (!wglDXOpenDeviceNV_ || !wglDXCloseDeviceNV_ ||
        !wglDXRegisterObjectNV_ || !wglDXUnregisterObjectNV_ ||
        !wglDXLockObjectsNV_ || !wglDXUnlockObjectsNV_) {
        return false;
    }

    // Open D3D11 device for interop
    nv_interop_device_ = wglDXOpenDeviceNV_(d3d_device_.Get());
    if (!nv_interop_device_) {
        Debug::Log("D3D11VideoInterop: wglDXOpenDeviceNV failed");
        return false;
    }

    return true;
}

bool D3D11VideoInterop::InitializeEXTMemoryInterop() {
    // Load GL_EXT_memory_object function pointers
    glCreateMemoryObjectsEXT_ = reinterpret_cast<PFNGLCREATEMEMORYOBJECTSEXTPROC>(
        wglGetProcAddress("glCreateMemoryObjectsEXT"));
    glDeleteMemoryObjectsEXT_ = reinterpret_cast<PFNGLDELETEMEMORYOBJECTSEXTPROC>(
        wglGetProcAddress("glDeleteMemoryObjectsEXT"));
    glTexStorageMem2DEXT_ = reinterpret_cast<PFNGLTEXSTORAGEMEM2DEXTPROC>(
        wglGetProcAddress("glTexStorageMem2DEXT"));
    glImportMemoryWin32HandleEXT_ = reinterpret_cast<PFNGLIMPORTMEMORYWIN32HANDLEEXTPROC>(
        wglGetProcAddress("glImportMemoryWin32HandleEXT"));

    if (!glCreateMemoryObjectsEXT_ || !glDeleteMemoryObjectsEXT_ ||
        !glTexStorageMem2DEXT_ || !glImportMemoryWin32HandleEXT_) {
        return false;
    }

    return true;
}

void D3D11VideoInterop::ShutdownNVInterop() {
    if (nv_interop_device_) {
        wglDXCloseDeviceNV_(nv_interop_device_);
        nv_interop_device_ = nullptr;
    }
}

void D3D11VideoInterop::ShutdownEXTMemoryInterop() {
    if (ext_memory_object_) {
        glDeleteMemoryObjectsEXT_(1, &ext_memory_object_);
        ext_memory_object_ = 0;
    }

    if (shared_handle_) {
        CloseHandle(shared_handle_);
        shared_handle_ = nullptr;
    }
}

bool D3D11VideoInterop::CreateSharedTexture(int width, int height, DXGI_FORMAT format) {
    if (!initialized_) {
        return false;
    }

    // Destroy existing texture if dimensions changed
    if (shared_texture_ && (width != width_ || height != height_ || format != format_)) {
        DestroySharedTexture();
    }

    // Return if texture already exists with correct dimensions
    if (shared_texture_) {
        return true;
    }

    width_ = width;
    height_ = height;
    format_ = format;

    // Try interop methods in order
    if (has_nv_interop_) {
        if (CreateNVInteropTexture(width, height, format)) {
            Debug::Log("D3D11VideoInterop: Created NV interop texture " +
                      std::to_string(width) + "x" + std::to_string(height));
            return true;
        }
    }

    if (has_ext_memory_) {
        if (CreateEXTMemoryTexture(width, height, format)) {
            Debug::Log("D3D11VideoInterop: Created EXT memory texture " +
                      std::to_string(width) + "x" + std::to_string(height));
            return true;
        }
    }

    // Fallback to CPU staging
    if (CreateFallbackTexture(width, height, format)) {
        Debug::Log("D3D11VideoInterop: Created fallback texture " +
                  std::to_string(width) + "x" + std::to_string(height));
        return true;
    }

    return false;
}

bool D3D11VideoInterop::CreateNVInteropTexture(int width, int height, DXGI_FORMAT format) {
    // Create D3D11 texture
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    HRESULT hr = d3d_device_->CreateTexture2D(&desc, nullptr, &shared_texture_);
    if (FAILED(hr)) {
        Debug::Log("D3D11VideoInterop: Failed to create D3D11 texture");
        return false;
    }

    // Create RTV
    hr = d3d_device_->CreateRenderTargetView(shared_texture_.Get(), nullptr, &rtv_);
    if (FAILED(hr)) {
        Debug::Log("D3D11VideoInterop: Failed to create RTV");
        shared_texture_.Reset();
        return false;
    }

    // Create SRV
    hr = d3d_device_->CreateShaderResourceView(shared_texture_.Get(), nullptr, &srv_);
    if (FAILED(hr)) {
        Debug::Log("D3D11VideoInterop: Failed to create SRV");
        rtv_.Reset();
        shared_texture_.Reset();
        return false;
    }

    // Create OpenGL texture
    glGenTextures(1, &gl_texture_);
    if (!gl_texture_) {
        Debug::Log("D3D11VideoInterop: Failed to create GL texture");
        srv_.Reset();
        rtv_.Reset();
        shared_texture_.Reset();
        return false;
    }

    // Register D3D11 texture with OpenGL
    nv_interop_object_ = wglDXRegisterObjectNV_(
        nv_interop_device_,
        shared_texture_.Get(),
        gl_texture_,
        GL_TEXTURE_2D,
        WGL_ACCESS_READ_ONLY_NV  // GL only reads, D3D11 writes
    );

    if (!nv_interop_object_) {
        Debug::Log("D3D11VideoInterop: wglDXRegisterObjectNV failed");
        glDeleteTextures(1, &gl_texture_);
        gl_texture_ = 0;
        srv_.Reset();
        rtv_.Reset();
        shared_texture_.Reset();
        return false;
    }

    // Initially lock for GL (texture starts in GL-owned state)
    // D3D11 will unlock before rendering, then lock back for GL after
    if (!wglDXLockObjectsNV_(nv_interop_device_, 1, &nv_interop_object_)) {
        Debug::Log("D3D11VideoInterop: Initial wglDXLockObjectsNV failed");
        wglDXUnregisterObjectNV_(nv_interop_device_, nv_interop_object_);
        nv_interop_object_ = nullptr;
        glDeleteTextures(1, &gl_texture_);
        gl_texture_ = 0;
        srv_.Reset();
        rtv_.Reset();
        shared_texture_.Reset();
        return false;
    }
    locked_for_d3d11_ = false;  // Texture is now GL-owned

    return true;
}

bool D3D11VideoInterop::CreateEXTMemoryTexture(int width, int height, DXGI_FORMAT format) {
    // Create D3D11 texture with shared handle
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

    HRESULT hr = d3d_device_->CreateTexture2D(&desc, nullptr, &shared_texture_);
    if (FAILED(hr)) {
        Debug::Log("D3D11VideoInterop: Failed to create shared D3D11 texture");
        return false;
    }

    // Create RTV
    hr = d3d_device_->CreateRenderTargetView(shared_texture_.Get(), nullptr, &rtv_);
    if (FAILED(hr)) {
        shared_texture_.Reset();
        return false;
    }

    // Create SRV
    hr = d3d_device_->CreateShaderResourceView(shared_texture_.Get(), nullptr, &srv_);
    if (FAILED(hr)) {
        rtv_.Reset();
        shared_texture_.Reset();
        return false;
    }

    // Get shared handle
    Microsoft::WRL::ComPtr<IDXGIResource1> dxgi_resource;
    hr = shared_texture_.As(&dxgi_resource);
    if (FAILED(hr)) {
        srv_.Reset();
        rtv_.Reset();
        shared_texture_.Reset();
        return false;
    }

    hr = dxgi_resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ, nullptr, &shared_handle_);
    if (FAILED(hr)) {
        srv_.Reset();
        rtv_.Reset();
        shared_texture_.Reset();
        return false;
    }

    // Create OpenGL memory object
    glCreateMemoryObjectsEXT_(1, &ext_memory_object_);

    // Calculate memory size (approximate)
    GLuint64 memory_size = width * height * 8;  // 16-bit RGBA = 8 bytes

    // Import the shared handle
    glImportMemoryWin32HandleEXT_(
        ext_memory_object_,
        memory_size,
        GL_HANDLE_TYPE_D3D11_IMAGE_EXT,
        shared_handle_
    );

    // Create GL texture with imported memory
    glGenTextures(1, &gl_texture_);
    glBindTexture(GL_TEXTURE_2D, gl_texture_);

    // Map DXGI format to GL format
    GLenum gl_internal_format = GL_RGBA16F;  // Default for HDR
    if (format == DXGI_FORMAT_R8G8B8A8_UNORM) {
        gl_internal_format = GL_RGBA8;
    } else if (format == DXGI_FORMAT_R10G10B10A2_UNORM) {
        gl_internal_format = GL_RGB10_A2;
    } else if (format == DXGI_FORMAT_R16G16B16A16_UNORM) {
        gl_internal_format = GL_RGBA16;  // HIGH_RES mode (16-bit integer)
    }

    glTexStorageMem2DEXT_(GL_TEXTURE_2D, 1, gl_internal_format, width, height, ext_memory_object_, 0);
    glBindTexture(GL_TEXTURE_2D, 0);

    return gl_texture_ != 0;
}

bool D3D11VideoInterop::CreateFallbackTexture(int width, int height, DXGI_FORMAT format) {
    // Create D3D11 render target texture
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    HRESULT hr = d3d_device_->CreateTexture2D(&desc, nullptr, &shared_texture_);
    if (FAILED(hr)) {
        return false;
    }

    // Create RTV
    hr = d3d_device_->CreateRenderTargetView(shared_texture_.Get(), nullptr, &rtv_);
    if (FAILED(hr)) {
        shared_texture_.Reset();
        return false;
    }

    // Create SRV
    hr = d3d_device_->CreateShaderResourceView(shared_texture_.Get(), nullptr, &srv_);
    if (FAILED(hr)) {
        rtv_.Reset();
        shared_texture_.Reset();
        return false;
    }

    // Create staging texture for CPU readback
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    hr = d3d_device_->CreateTexture2D(&desc, nullptr, &staging_texture_);
    if (FAILED(hr)) {
        srv_.Reset();
        rtv_.Reset();
        shared_texture_.Reset();
        return false;
    }

    // Create OpenGL texture
    glGenTextures(1, &gl_texture_);
    glBindTexture(GL_TEXTURE_2D, gl_texture_);

    // Map DXGI format to GL format/type
    GLenum gl_internal_format = GL_RGBA16F;
    GLenum gl_format = GL_RGBA;
    GLenum gl_type = GL_HALF_FLOAT;

    if (format == DXGI_FORMAT_R8G8B8A8_UNORM) {
        gl_internal_format = GL_RGBA8;
        gl_type = GL_UNSIGNED_BYTE;
    } else if (format == DXGI_FORMAT_R10G10B10A2_UNORM) {
        gl_internal_format = GL_RGB10_A2;
        gl_type = GL_UNSIGNED_INT_2_10_10_10_REV;
    } else if (format == DXGI_FORMAT_R16G16B16A16_UNORM) {
        gl_internal_format = GL_RGBA16;  // HIGH_RES mode (16-bit integer)
        gl_type = GL_UNSIGNED_SHORT;
    }

    glTexImage2D(GL_TEXTURE_2D, 0, gl_internal_format, width, height, 0, gl_format, gl_type, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D, 0);

    return true;
}

void D3D11VideoInterop::DestroySharedTexture() {
    if (has_nv_interop_) {
        DestroyNVInteropTexture();
    } else if (has_ext_memory_) {
        DestroyEXTMemoryTexture();
    } else {
        DestroyFallbackTexture();
    }

    srv_.Reset();
    rtv_.Reset();
    shared_texture_.Reset();
    staging_texture_.Reset();

    width_ = 0;
    height_ = 0;
    format_ = DXGI_FORMAT_UNKNOWN;
}

void D3D11VideoInterop::DestroyNVInteropTexture() {
    if (nv_interop_object_ && nv_interop_device_) {
        // Texture must be unlocked from GL (D3D11-owned) before unregistering
        if (!locked_for_d3d11_) {
            // Currently GL-owned, unlock to D3D11
            wglDXUnlockObjectsNV_(nv_interop_device_, 1, &nv_interop_object_);
        }
        wglDXUnregisterObjectNV_(nv_interop_device_, nv_interop_object_);
        nv_interop_object_ = nullptr;
    }

    if (gl_texture_) {
        glDeleteTextures(1, &gl_texture_);
        gl_texture_ = 0;
    }
}

void D3D11VideoInterop::DestroyEXTMemoryTexture() {
    if (gl_texture_) {
        glDeleteTextures(1, &gl_texture_);
        gl_texture_ = 0;
    }

    if (ext_memory_object_) {
        glDeleteMemoryObjectsEXT_(1, &ext_memory_object_);
        ext_memory_object_ = 0;
    }

    if (shared_handle_) {
        CloseHandle(shared_handle_);
        shared_handle_ = nullptr;
    }
}

void D3D11VideoInterop::DestroyFallbackTexture() {
    if (gl_texture_) {
        glDeleteTextures(1, &gl_texture_);
        gl_texture_ = 0;
    }
}

bool D3D11VideoInterop::LockForD3D11() {
    if (!initialized_ || !shared_texture_) {
        return false;
    }

    if (locked_for_d3d11_) {
        return true;  // Already locked for D3D11
    }

    if (has_nv_interop_ && nv_interop_object_) {
        // Unlock from GL to give D3D11 access
        // NV_DX_interop: "lock" = GL owns, "unlock" = D3D11 owns
        if (!wglDXUnlockObjectsNV_(nv_interop_device_, 1, &nv_interop_object_)) {
            Debug::Log("D3D11VideoInterop: wglDXUnlockObjectsNV failed");
            return false;
        }
        locked_for_d3d11_ = true;
        return true;
    }

    if (has_ext_memory_) {
        // EXT_memory uses keyed mutex for synchronization
        // Acquire mutex for D3D11
        locked_for_d3d11_ = true;
        return true;
    }

    // Fallback: no explicit locking needed
    locked_for_d3d11_ = true;
    return true;
}

bool D3D11VideoInterop::UnlockForGL() {
    if (!initialized_ || !shared_texture_) {
        return false;
    }

    if (!locked_for_d3d11_) {
        return true;  // Already unlocked
    }

    if (has_nv_interop_ && nv_interop_object_) {
        // Lock the object for GL access
        if (!wglDXLockObjectsNV_(nv_interop_device_, 1, &nv_interop_object_)) {
            Debug::Log("D3D11VideoInterop: wglDXLockObjectsNV failed");
            return false;
        }
        locked_for_d3d11_ = false;
        return true;
    }

    if (has_ext_memory_) {
        // Release mutex for GL
        locked_for_d3d11_ = false;
        return true;
    }

    // Fallback: copy from D3D11 to GL via CPU
    if (staging_texture_ && gl_texture_) {
        // Copy render target to staging
        d3d_context_->CopyResource(staging_texture_.Get(), shared_texture_.Get());

        // Map staging texture
        D3D11_MAPPED_SUBRESOURCE mapped;
        HRESULT hr = d3d_context_->Map(staging_texture_.Get(), 0, D3D11_MAP_READ, 0, &mapped);
        if (SUCCEEDED(hr)) {
            // Upload to GL texture
            glBindTexture(GL_TEXTURE_2D, gl_texture_);

            GLenum gl_format = GL_RGBA;
            GLenum gl_type = GL_HALF_FLOAT;

            if (format_ == DXGI_FORMAT_R8G8B8A8_UNORM) {
                gl_type = GL_UNSIGNED_BYTE;
            } else if (format_ == DXGI_FORMAT_R10G10B10A2_UNORM) {
                gl_type = GL_UNSIGNED_INT_2_10_10_10_REV;
            } else if (format_ == DXGI_FORMAT_R16G16B16A16_UNORM) {
                gl_type = GL_UNSIGNED_SHORT;  // HIGH_RES mode (16-bit integer)
            }

            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width_, height_,
                           gl_format, gl_type, mapped.pData);

            glBindTexture(GL_TEXTURE_2D, 0);
            d3d_context_->Unmap(staging_texture_.Get(), 0);
        }
    }

    locked_for_d3d11_ = false;
    return true;
}

const char* D3D11VideoInterop::GetInteropMethodName() const {
    if (has_nv_interop_) {
        return "NV_DX_interop";
    } else if (has_ext_memory_) {
        return "EXT_memory_object";
    } else {
        return "CPU Fallback";
    }
}

} // namespace ump

#endif // _WIN32
