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
    for (int i = 0; i < kInteropBufferCount; i++) {
        if (ext_memory_objects_[i]) {
            glDeleteMemoryObjectsEXT_(1, &ext_memory_objects_[i]);
            ext_memory_objects_[i] = 0;
        }

        if (shared_handles_[i]) {
            CloseHandle(shared_handles_[i]);
            shared_handles_[i] = nullptr;
        }
    }
}

bool D3D11VideoInterop::CreateSharedTexture(int width, int height, DXGI_FORMAT format) {
    if (!initialized_) {
        return false;
    }

    // Check if textures already exist (use array-based check)
    bool textures_exist = shared_textures_[0] != nullptr;

    // Destroy existing texture if dimensions changed
    if (textures_exist && (width != width_ || height != height_ || format != format_)) {
        DestroySharedTexture();
        textures_exist = false;
    }

    // Return if texture already exists with correct dimensions
    if (textures_exist) {
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
    // Create triple-buffered D3D11 textures
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    for (int i = 0; i < kInteropBufferCount; i++) {
        HRESULT hr = d3d_device_->CreateTexture2D(&desc, nullptr, &shared_textures_[i]);
        if (FAILED(hr)) {
            Debug::Log("D3D11VideoInterop: Failed to create D3D11 texture " + std::to_string(i));
            // Clean up already created textures
            for (int j = 0; j < i; j++) {
                shared_textures_[j].Reset();
            }
            return false;
        }

        // Create RTV for each texture
        hr = d3d_device_->CreateRenderTargetView(shared_textures_[i].Get(), nullptr, &rtvs_[i]);
        if (FAILED(hr)) {
            Debug::Log("D3D11VideoInterop: Failed to create RTV " + std::to_string(i));
            for (int j = 0; j <= i; j++) {
                shared_textures_[j].Reset();
                if (j < i) rtvs_[j].Reset();
            }
            return false;
        }

        // Create SRV for each texture
        hr = d3d_device_->CreateShaderResourceView(shared_textures_[i].Get(), nullptr, &srvs_[i]);
        if (FAILED(hr)) {
            Debug::Log("D3D11VideoInterop: Failed to create SRV " + std::to_string(i));
            for (int j = 0; j <= i; j++) {
                shared_textures_[j].Reset();
                rtvs_[j].Reset();
                if (j < i) srvs_[j].Reset();
            }
            return false;
        }

        // Create OpenGL texture for each buffer
        glGenTextures(1, &gl_textures_[i]);
        if (!gl_textures_[i]) {
            Debug::Log("D3D11VideoInterop: Failed to create GL texture " + std::to_string(i));
            for (int j = 0; j <= i; j++) {
                shared_textures_[j].Reset();
                rtvs_[j].Reset();
                srvs_[j].Reset();
                if (j < i) glDeleteTextures(1, &gl_textures_[j]);
            }
            return false;
        }

        // Register D3D11 texture with OpenGL
        nv_interop_objects_[i] = wglDXRegisterObjectNV_(
            nv_interop_device_,
            shared_textures_[i].Get(),
            gl_textures_[i],
            GL_TEXTURE_2D,
            WGL_ACCESS_READ_ONLY_NV  // GL only reads, D3D11 writes
        );

        if (!nv_interop_objects_[i]) {
            Debug::Log("D3D11VideoInterop: wglDXRegisterObjectNV failed for buffer " + std::to_string(i));
            for (int j = 0; j <= i; j++) {
                if (j < i && nv_interop_objects_[j]) {
                    wglDXUnregisterObjectNV_(nv_interop_device_, nv_interop_objects_[j]);
                    nv_interop_objects_[j] = nullptr;
                }
                glDeleteTextures(1, &gl_textures_[j]);
                gl_textures_[j] = 0;
                srvs_[j].Reset();
                rtvs_[j].Reset();
                shared_textures_[j].Reset();
            }
            return false;
        }

        // Initially lock all buffers for GL (they start in GL-owned state)
        if (!wglDXLockObjectsNV_(nv_interop_device_, 1, &nv_interop_objects_[i])) {
            Debug::Log("D3D11VideoInterop: Initial wglDXLockObjectsNV failed for buffer " + std::to_string(i));
            for (int j = 0; j <= i; j++) {
                if (j < i) {
                    wglDXUnlockObjectsNV_(nv_interop_device_, 1, &nv_interop_objects_[j]);
                }
                wglDXUnregisterObjectNV_(nv_interop_device_, nv_interop_objects_[j]);
                nv_interop_objects_[j] = nullptr;
                glDeleteTextures(1, &gl_textures_[j]);
                gl_textures_[j] = 0;
                srvs_[j].Reset();
                rtvs_[j].Reset();
                shared_textures_[j].Reset();
            }
            return false;
        }
        buffer_locked_[i] = false;  // GL-owned = not locked for D3D11
    }

    // Initialize ring buffer indices
    write_index_ = 0;
    read_index_ = kInteropBufferCount - 1;  // Read from oldest buffer

    // Set up legacy pointers for API compatibility
    shared_texture_ = shared_textures_[write_index_];
    rtv_ = rtvs_[write_index_];
    srv_ = srvs_[write_index_];
    gl_texture_ = gl_textures_[read_index_];

    locked_for_d3d11_ = false;

    Debug::Log("D3D11VideoInterop: Created triple-buffered NV interop textures");
    return true;
}

bool D3D11VideoInterop::CreateEXTMemoryTexture(int width, int height, DXGI_FORMAT format) {
    // Create triple-buffered D3D11 textures with shared handles
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

    // Map DXGI format to GL format
    GLenum gl_internal_format = GL_RGBA16F;  // Default for HDR
    if (format == DXGI_FORMAT_R8G8B8A8_UNORM) {
        gl_internal_format = GL_RGBA8;
    } else if (format == DXGI_FORMAT_R10G10B10A2_UNORM) {
        gl_internal_format = GL_RGB10_A2;
    } else if (format == DXGI_FORMAT_R16G16B16A16_UNORM) {
        gl_internal_format = GL_RGBA16;  // HIGH_RES mode (16-bit integer)
    }

    // Calculate memory size (approximate)
    GLuint64 memory_size = width * height * 8;  // 16-bit RGBA = 8 bytes

    for (int i = 0; i < kInteropBufferCount; i++) {
        HRESULT hr = d3d_device_->CreateTexture2D(&desc, nullptr, &shared_textures_[i]);
        if (FAILED(hr)) {
            Debug::Log("D3D11VideoInterop: Failed to create shared D3D11 texture " + std::to_string(i));
            goto cleanup;
        }

        // Create RTV
        hr = d3d_device_->CreateRenderTargetView(shared_textures_[i].Get(), nullptr, &rtvs_[i]);
        if (FAILED(hr)) {
            goto cleanup;
        }

        // Create SRV
        hr = d3d_device_->CreateShaderResourceView(shared_textures_[i].Get(), nullptr, &srvs_[i]);
        if (FAILED(hr)) {
            goto cleanup;
        }

        // Get shared handle
        Microsoft::WRL::ComPtr<IDXGIResource1> dxgi_resource;
        hr = shared_textures_[i].As(&dxgi_resource);
        if (FAILED(hr)) {
            goto cleanup;
        }

        hr = dxgi_resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ, nullptr, &shared_handles_[i]);
        if (FAILED(hr)) {
            goto cleanup;
        }

        // Create OpenGL memory object
        glCreateMemoryObjectsEXT_(1, &ext_memory_objects_[i]);

        // Import the shared handle
        glImportMemoryWin32HandleEXT_(
            ext_memory_objects_[i],
            memory_size,
            GL_HANDLE_TYPE_D3D11_IMAGE_EXT,
            shared_handles_[i]
        );

        // Create GL texture with imported memory
        glGenTextures(1, &gl_textures_[i]);
        glBindTexture(GL_TEXTURE_2D, gl_textures_[i]);
        glTexStorageMem2DEXT_(GL_TEXTURE_2D, 1, gl_internal_format, width, height, ext_memory_objects_[i], 0);
        glBindTexture(GL_TEXTURE_2D, 0);

        if (!gl_textures_[i]) {
            goto cleanup;
        }
    }

    // Initialize ring buffer indices
    write_index_ = 0;
    read_index_ = kInteropBufferCount - 1;

    // Set up legacy pointers for API compatibility
    shared_texture_ = shared_textures_[write_index_];
    rtv_ = rtvs_[write_index_];
    srv_ = srvs_[write_index_];
    gl_texture_ = gl_textures_[read_index_];

    Debug::Log("D3D11VideoInterop: Created triple-buffered EXT memory textures");
    return true;

cleanup:
    // Clean up on failure
    for (int j = 0; j < kInteropBufferCount; j++) {
        if (gl_textures_[j]) {
            glDeleteTextures(1, &gl_textures_[j]);
            gl_textures_[j] = 0;
        }
        if (ext_memory_objects_[j]) {
            glDeleteMemoryObjectsEXT_(1, &ext_memory_objects_[j]);
            ext_memory_objects_[j] = 0;
        }
        if (shared_handles_[j]) {
            CloseHandle(shared_handles_[j]);
            shared_handles_[j] = nullptr;
        }
        srvs_[j].Reset();
        rtvs_[j].Reset();
        shared_textures_[j].Reset();
    }
    return false;
}

bool D3D11VideoInterop::CreateFallbackTexture(int width, int height, DXGI_FORMAT format) {
    // Create triple-buffered D3D11 render target textures
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    // Map DXGI format to GL format/type (used for all buffers)
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

    for (int i = 0; i < kInteropBufferCount; i++) {
        HRESULT hr = d3d_device_->CreateTexture2D(&desc, nullptr, &shared_textures_[i]);
        if (FAILED(hr)) {
            for (int j = 0; j < i; j++) {
                shared_textures_[j].Reset();
                rtvs_[j].Reset();
                srvs_[j].Reset();
                glDeleteTextures(1, &gl_textures_[j]);
                gl_textures_[j] = 0;
            }
            return false;
        }

        // Create RTV
        hr = d3d_device_->CreateRenderTargetView(shared_textures_[i].Get(), nullptr, &rtvs_[i]);
        if (FAILED(hr)) {
            shared_textures_[i].Reset();
            for (int j = 0; j < i; j++) {
                shared_textures_[j].Reset();
                rtvs_[j].Reset();
                srvs_[j].Reset();
                glDeleteTextures(1, &gl_textures_[j]);
                gl_textures_[j] = 0;
            }
            return false;
        }

        // Create SRV
        hr = d3d_device_->CreateShaderResourceView(shared_textures_[i].Get(), nullptr, &srvs_[i]);
        if (FAILED(hr)) {
            rtvs_[i].Reset();
            shared_textures_[i].Reset();
            for (int j = 0; j < i; j++) {
                shared_textures_[j].Reset();
                rtvs_[j].Reset();
                srvs_[j].Reset();
                glDeleteTextures(1, &gl_textures_[j]);
                gl_textures_[j] = 0;
            }
            return false;
        }

        // Create OpenGL texture for this buffer
        glGenTextures(1, &gl_textures_[i]);
        glBindTexture(GL_TEXTURE_2D, gl_textures_[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, gl_internal_format, width, height, 0, gl_format, gl_type, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    // Create staging texture for CPU readback (only need one for fallback path)
    D3D11_TEXTURE2D_DESC staging_desc = desc;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.BindFlags = 0;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    HRESULT hr = d3d_device_->CreateTexture2D(&staging_desc, nullptr, &staging_texture_);
    if (FAILED(hr)) {
        for (int i = 0; i < kInteropBufferCount; i++) {
            srvs_[i].Reset();
            rtvs_[i].Reset();
            shared_textures_[i].Reset();
            glDeleteTextures(1, &gl_textures_[i]);
            gl_textures_[i] = 0;
        }
        return false;
    }

    // Initialize ring buffer indices
    write_index_ = 0;
    read_index_ = kInteropBufferCount - 1;

    // Set up legacy pointers for API compatibility
    shared_texture_ = shared_textures_[write_index_];
    rtv_ = rtvs_[write_index_];
    srv_ = srvs_[write_index_];
    gl_texture_ = gl_textures_[read_index_];

    Debug::Log("D3D11VideoInterop: Created triple-buffered fallback textures");
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

    // Clear all array-based resources
    for (int i = 0; i < kInteropBufferCount; i++) {
        srvs_[i].Reset();
        rtvs_[i].Reset();
        shared_textures_[i].Reset();
        buffer_locked_[i] = false;
    }

    // Clear legacy pointers
    srv_.Reset();
    rtv_.Reset();
    shared_texture_.Reset();
    staging_texture_.Reset();
    gl_texture_ = 0;

    // Reset indices
    write_index_ = 0;
    read_index_ = kInteropBufferCount - 1;

    width_ = 0;
    height_ = 0;
    format_ = DXGI_FORMAT_UNKNOWN;
}

void D3D11VideoInterop::DestroyNVInteropTexture() {
    // Clean up all triple-buffered interop objects
    for (int i = 0; i < kInteropBufferCount; i++) {
        if (nv_interop_objects_[i] && nv_interop_device_) {
            // Texture must be unlocked from GL (D3D11-owned) before unregistering
            if (!buffer_locked_[i]) {
                // Currently GL-owned, unlock to D3D11
                wglDXUnlockObjectsNV_(nv_interop_device_, 1, &nv_interop_objects_[i]);
            }
            wglDXUnregisterObjectNV_(nv_interop_device_, nv_interop_objects_[i]);
            nv_interop_objects_[i] = nullptr;
        }

        if (gl_textures_[i]) {
            glDeleteTextures(1, &gl_textures_[i]);
            gl_textures_[i] = 0;
        }

        shared_textures_[i].Reset();
        rtvs_[i].Reset();
        srvs_[i].Reset();
        buffer_locked_[i] = false;
    }

    // Clear legacy pointers
    gl_texture_ = 0;
    shared_texture_.Reset();
    rtv_.Reset();
    srv_.Reset();
}

void D3D11VideoInterop::DestroyEXTMemoryTexture() {
    for (int i = 0; i < kInteropBufferCount; i++) {
        if (gl_textures_[i]) {
            glDeleteTextures(1, &gl_textures_[i]);
            gl_textures_[i] = 0;
        }

        if (ext_memory_objects_[i]) {
            glDeleteMemoryObjectsEXT_(1, &ext_memory_objects_[i]);
            ext_memory_objects_[i] = 0;
        }

        if (shared_handles_[i]) {
            CloseHandle(shared_handles_[i]);
            shared_handles_[i] = nullptr;
        }

        shared_textures_[i].Reset();
        rtvs_[i].Reset();
        srvs_[i].Reset();
    }

    gl_texture_ = 0;
    shared_texture_.Reset();
    rtv_.Reset();
    srv_.Reset();
}

void D3D11VideoInterop::DestroyFallbackTexture() {
    for (int i = 0; i < kInteropBufferCount; i++) {
        if (gl_textures_[i]) {
            glDeleteTextures(1, &gl_textures_[i]);
            gl_textures_[i] = 0;
        }
        shared_textures_[i].Reset();
        rtvs_[i].Reset();
        srvs_[i].Reset();
    }
    gl_texture_ = 0;
    shared_texture_.Reset();
    rtv_.Reset();
    srv_.Reset();
}

bool D3D11VideoInterop::LockForD3D11() {
    if (!initialized_ || !shared_textures_[write_index_]) {
        return false;
    }

    if (locked_for_d3d11_) {
        return true;  // Already locked for D3D11
    }

    if (has_nv_interop_ && nv_interop_objects_[write_index_]) {
        // Unlock write buffer from GL to give D3D11 access
        // NV_DX_interop: "lock" = GL owns, "unlock" = D3D11 owns
        if (!buffer_locked_[write_index_]) {
            if (!wglDXUnlockObjectsNV_(nv_interop_device_, 1, &nv_interop_objects_[write_index_])) {
                Debug::Log("D3D11VideoInterop: wglDXUnlockObjectsNV failed for write buffer");
                return false;
            }
            buffer_locked_[write_index_] = true;
        }

        // Update legacy pointers to current write buffer
        shared_texture_ = shared_textures_[write_index_];
        rtv_ = rtvs_[write_index_];
        srv_ = srvs_[write_index_];

        locked_for_d3d11_ = true;
        return true;
    }

    if (has_ext_memory_) {
        // EXT_memory uses keyed mutex for synchronization
        // Acquire mutex for D3D11
        shared_texture_ = shared_textures_[write_index_];
        rtv_ = rtvs_[write_index_];
        srv_ = srvs_[write_index_];
        locked_for_d3d11_ = true;
        return true;
    }

    // Fallback: no explicit locking needed
    shared_texture_ = shared_textures_[write_index_];
    rtv_ = rtvs_[write_index_];
    srv_ = srvs_[write_index_];
    locked_for_d3d11_ = true;
    return true;
}

bool D3D11VideoInterop::UnlockForGL() {
    if (!initialized_ || !shared_textures_[write_index_]) {
        return false;
    }

    if (!locked_for_d3d11_) {
        return true;  // Already unlocked
    }

    if (has_nv_interop_ && nv_interop_objects_[write_index_]) {
        // Lock the write buffer back for GL access (D3D11 rendering complete)
        if (buffer_locked_[write_index_]) {
            if (!wglDXLockObjectsNV_(nv_interop_device_, 1, &nv_interop_objects_[write_index_])) {
                Debug::Log("D3D11VideoInterop: wglDXLockObjectsNV failed for write buffer");
                return false;
            }
            buffer_locked_[write_index_] = false;
        }

        // Return the buffer we just rendered to (for correct frame content)
        gl_texture_ = gl_textures_[write_index_];

        // Advance write index for next frame (so next render uses different buffer)
        // This allows GL to keep reading current buffer while next frame renders
        write_index_ = (write_index_ + 1) % kInteropBufferCount;

        locked_for_d3d11_ = false;
        return true;
    }

    if (has_ext_memory_) {
        // Return the buffer we just rendered to
        gl_texture_ = gl_textures_[write_index_];
        // Advance write index for next frame
        write_index_ = (write_index_ + 1) % kInteropBufferCount;
        locked_for_d3d11_ = false;
        return true;
    }

    // Fallback: copy from D3D11 to GL via CPU
    if (staging_texture_ && gl_textures_[write_index_]) {
        // Copy render target to staging
        d3d_context_->CopyResource(staging_texture_.Get(), shared_textures_[write_index_].Get());

        // Map staging texture
        D3D11_MAPPED_SUBRESOURCE mapped;
        HRESULT hr = d3d_context_->Map(staging_texture_.Get(), 0, D3D11_MAP_READ, 0, &mapped);
        if (SUCCEEDED(hr)) {
            // Upload to GL texture (use write buffer - the one we just rendered to)
            glBindTexture(GL_TEXTURE_2D, gl_textures_[write_index_]);

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

        // Return the buffer we just rendered to
        gl_texture_ = gl_textures_[write_index_];
        // Advance write index for next frame
        write_index_ = (write_index_ + 1) % kInteropBufferCount;
    }

    locked_for_d3d11_ = false;
    return true;
}

void D3D11VideoInterop::AdvanceBuffers() {
    // Advance write index (circular buffer)
    write_index_ = (write_index_ + 1) % kInteropBufferCount;

    // Read index follows write by (kInteropBufferCount - 1) frames
    // This ensures GL reads from the oldest completed buffer
    read_index_ = (write_index_ + 1) % kInteropBufferCount;
}

const char* D3D11VideoInterop::GetInteropMethodName() const {
    if (has_nv_interop_) {
        return "NV_DX_interop (triple-buffered)";
    } else if (has_ext_memory_) {
        return "EXT_memory_object (triple-buffered)";
    } else {
        return "CPU Fallback (triple-buffered)";
    }
}

} // namespace ump

#endif // _WIN32
