#include "d3d11_hdr_swapchain.h"

#ifdef _WIN32

#include <glad/gl.h>
#include <d3dcompiler.h>
#include "../utils/debug_utils.h"
#include <chrono>

#pragma comment(lib, "d3dcompiler.lib")

// OpenGL constants
#ifndef GL_TEXTURE_2D
#define GL_TEXTURE_2D 0x0DE1
#endif
#ifndef GL_RGBA
#define GL_RGBA 0x1908
#endif
#ifndef GL_UNSIGNED_INT_2_10_10_10_REV
#define GL_UNSIGNED_INT_2_10_10_10_REV 0x8368
#endif
#ifndef GL_RGB10_A2
#define GL_RGB10_A2 0x8059
#endif
#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER 0x8D40
#endif
#ifndef GL_COLOR_ATTACHMENT0
#define GL_COLOR_ATTACHMENT0 0x8CE0
#endif
#ifndef GL_FRAMEBUFFER_COMPLETE
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#endif
#ifndef GL_TEXTURE_MIN_FILTER
#define GL_TEXTURE_MIN_FILTER 0x2801
#endif
#ifndef GL_TEXTURE_MAG_FILTER
#define GL_TEXTURE_MAG_FILTER 0x2800
#endif
#ifndef GL_LINEAR
#define GL_LINEAR 0x2601
#endif

// WGL_NV_DX_interop access modes
#define WGL_ACCESS_READ_ONLY_NV 0x0000
#define WGL_ACCESS_READ_WRITE_NV 0x0001
#define WGL_ACCESS_WRITE_DISCARD_NV 0x0002

// GL_EXT_memory_object constants
#define GL_HANDLE_TYPE_D3D11_IMAGE_EXT 0x958B
#define GL_HANDLE_TYPE_D3D11_IMAGE_KMT_EXT 0x958C
#define GL_DEVICE_UUID_EXT 0x9597
#define GL_DRIVER_UUID_EXT 0x9598
#define GL_HANDLE_TYPE_OPAQUE_WIN32_EXT 0x9587
#define GL_HANDLE_TYPE_OPAQUE_WIN32_KMT_EXT 0x9588
#define GL_TEXTURE_TILING_EXT 0x9580
#define GL_DEDICATED_MEMORY_OBJECT_EXT 0x9581
#define GL_OPTIMAL_TILING_EXT 0x9584
#define GL_LINEAR_TILING_EXT 0x9585

// GL_EXT_semaphore constants
#define GL_HANDLE_TYPE_D3D12_FENCE_EXT 0x9594
#define GL_D3D12_FENCE_VALUE_EXT 0x9595
#define GL_LAYOUT_GENERAL_EXT 0x958D
#define GL_LAYOUT_COLOR_ATTACHMENT_EXT 0x958E
#define GL_LAYOUT_DEPTH_STENCIL_ATTACHMENT_EXT 0x958F
#define GL_LAYOUT_DEPTH_STENCIL_READ_ONLY_EXT 0x9590
#define GL_LAYOUT_SHADER_READ_ONLY_EXT 0x9591
#define GL_LAYOUT_TRANSFER_SRC_EXT 0x9592
#define GL_LAYOUT_TRANSFER_DST_EXT 0x9593

namespace ump {

D3D11HDRSwapchain::D3D11HDRSwapchain() = default;

D3D11HDRSwapchain::~D3D11HDRSwapchain() {
    Shutdown();
}

bool D3D11HDRSwapchain::Initialize(HWND hwnd, int width, int height) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (initialized_) {
        Debug::Log("D3D11HDRSwapchain: Already initialized");
        return true;
    }

    hwnd_ = hwnd;
    width_ = width;
    height_ = height;

    if (!CreateD3D11Device()) {
        Debug::Log("D3D11HDRSwapchain: Failed to create D3D11 device");
        return false;
    }

    if (!CreateSwapchain(hwnd, width, height)) {
        Debug::Log("D3D11HDRSwapchain: Failed to create swapchain");
        Shutdown();
        return false;
    }

    if (!CreateRenderTargetView()) {
        Debug::Log("D3D11HDRSwapchain: Failed to create render target view");
        Shutdown();
        return false;
    }

    DetectHDRSupport();

    if (hdr_info_.hdr_enabled) {
        SetColorSpace(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
        Debug::Log("D3D11HDRSwapchain: HDR mode active - using PQ/BT.2020 colorspace");
    } else {
        SetColorSpace(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
        Debug::Log("D3D11HDRSwapchain: SDR mode - using sRGB/BT.709 colorspace");
    }

    // Try interop methods in priority order
    // 1. NVIDIA NV_DX_interop (most mature)
    if (InitializeNVInterop()) {
        if (CreateNVInteropRenderTarget(width, height)) {
            interop_method_ = InteropMethod::NV_DX_Interop;
            Debug::Log("D3D11HDRSwapchain: Using NV_DX_interop (NVIDIA zero-copy)");
        }
    }

    // 2. EXT_external_objects (Intel Arc, AMD, also NVIDIA)
    if (interop_method_ == InteropMethod::None) {
        if (InitializeEXTMemoryInterop()) {
            if (CreateEXTMemoryRenderTarget(width, height)) {
                interop_method_ = InteropMethod::EXT_Memory;
                Debug::Log("D3D11HDRSwapchain: Using EXT_memory_object (Intel/AMD/NVIDIA zero-copy)");
            }
        }
    }

    // 3. CPU fallback
    if (interop_method_ == InteropMethod::None) {
        if (!CreateFallbackResources(width, height)) {
            Debug::Log("D3D11HDRSwapchain: Failed to create fallback resources");
            Shutdown();
            return false;
        }
        Debug::Log("D3D11HDRSwapchain: Using CPU readback fallback");
    }

    initialized_ = true;

    std::string method_str;
    switch (interop_method_) {
        case InteropMethod::NV_DX_Interop: method_str = "NV_DX_interop"; break;
        case InteropMethod::EXT_Memory: method_str = "EXT_memory_object"; break;
        default: method_str = "CPU fallback"; break;
    }
    Debug::Log("D3D11HDRSwapchain: Initialized (" + std::to_string(width) + "x" +
               std::to_string(height) + ", " + method_str + ")");

    return true;
}

void D3D11HDRSwapchain::Shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);

    DestroySharedRenderTarget();
    DestroyFallbackResources();
    ShutdownNVInterop();
    ShutdownEXTMemoryInterop();

    rtv_.Reset();
    back_buffer_.Reset();
    staging_texture_.Reset();
    output6_.Reset();
    swapchain4_.Reset();
    swapchain_.Reset();

    // DirectComposition cleanup
    dcomp_visual_.Reset();
    dcomp_target_.Reset();
    dcomp_device_.Reset();

    context_.Reset();
    device_.Reset();

    interop_method_ = InteropMethod::None;
    initialized_ = false;
    in_frame_ = false;
    Debug::Log("D3D11HDRSwapchain: Shutdown complete");
}

bool D3D11HDRSwapchain::CreateD3D11Device() {
    UINT create_flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
    create_flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    D3D_FEATURE_LEVEL feature_levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };

    D3D_FEATURE_LEVEL achieved_level;
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;

    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, create_flags,
        feature_levels, _countof(feature_levels), D3D11_SDK_VERSION,
        &device, &achieved_level, &context
    );

    if (FAILED(hr)) {
        Debug::Log("D3D11HDRSwapchain: D3D11CreateDevice failed");
        return false;
    }

    // Get ID3D11Device1 for shared resources
    hr = device.As(&device_);
    if (FAILED(hr)) {
        Debug::Log("D3D11HDRSwapchain: Failed to get ID3D11Device1");
        return false;
    }

    hr = context.As(&context_);
    if (FAILED(hr)) {
        Debug::Log("D3D11HDRSwapchain: Failed to get ID3D11DeviceContext1");
        return false;
    }

    Debug::Log("D3D11HDRSwapchain: D3D11 device created (FL " + std::to_string(achieved_level) + ")");
    return true;
}

bool D3D11HDRSwapchain::CreateSwapchain(HWND hwnd, int width, int height) {
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_device;
    HRESULT hr = device_.As(&dxgi_device);
    if (FAILED(hr)) {
        Debug::Log("D3D11HDRSwapchain: Failed to get IDXGIDevice");
        return false;
    }

    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    hr = dxgi_device->GetAdapter(&adapter);
    if (FAILED(hr)) {
        Debug::Log("D3D11HDRSwapchain: Failed to get adapter");
        return false;
    }

    Microsoft::WRL::ComPtr<IDXGIFactory2> factory;
    hr = adapter->GetParent(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        Debug::Log("D3D11HDRSwapchain: Failed to get factory");
        return false;
    }

    // Create DirectComposition device - this allows us to present D3D11 content
    // on a window that already has an OpenGL context
    hr = DCompositionCreateDevice(dxgi_device.Get(), IID_PPV_ARGS(&dcomp_device_));
    if (FAILED(hr)) {
        Debug::Log("D3D11HDRSwapchain: DCompositionCreateDevice failed (hr=0x" +
                   std::to_string(hr) + ")");
        return false;
    }

    // Create composition target for the window
    hr = dcomp_device_->CreateTargetForHwnd(hwnd, TRUE, &dcomp_target_);
    if (FAILED(hr)) {
        Debug::Log("D3D11HDRSwapchain: CreateTargetForHwnd failed (hr=0x" +
                   std::to_string(hr) + ")");
        dcomp_device_.Reset();
        return false;
    }

    // Create visual
    hr = dcomp_device_->CreateVisual(&dcomp_visual_);
    if (FAILED(hr)) {
        Debug::Log("D3D11HDRSwapchain: CreateVisual failed");
        dcomp_target_.Reset();
        dcomp_device_.Reset();
        return false;
    }

    // Create swapchain for composition (not for HWND directly)
    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.Format = swapchain_format_;
    desc.Stereo = FALSE;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;  // Required for composition
    desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;      // For proper compositing
    desc.Flags = 0;

    hr = factory->CreateSwapChainForComposition(device_.Get(), &desc, nullptr, &swapchain_);
    if (FAILED(hr)) {
        Debug::Log("D3D11HDRSwapchain: CreateSwapChainForComposition failed (hr=0x" +
                   std::to_string(hr) + ")");
        dcomp_visual_.Reset();
        dcomp_target_.Reset();
        dcomp_device_.Reset();
        return false;
    }

    // Set swapchain as visual content
    hr = dcomp_visual_->SetContent(swapchain_.Get());
    if (FAILED(hr)) {
        Debug::Log("D3D11HDRSwapchain: SetContent failed");
        swapchain_.Reset();
        dcomp_visual_.Reset();
        dcomp_target_.Reset();
        dcomp_device_.Reset();
        return false;
    }

    // Set visual as root of target
    hr = dcomp_target_->SetRoot(dcomp_visual_.Get());
    if (FAILED(hr)) {
        Debug::Log("D3D11HDRSwapchain: SetRoot failed");
        swapchain_.Reset();
        dcomp_visual_.Reset();
        dcomp_target_.Reset();
        dcomp_device_.Reset();
        return false;
    }

    // Apply vertical flip transform to correct OpenGL's bottom-left origin vs D3D11's top-left
    // This is a scale of (1, -1) with a translation to compensate
    Microsoft::WRL::ComPtr<IDCompositionScaleTransform> scale_transform;
    hr = dcomp_device_->CreateScaleTransform(&scale_transform);
    if (SUCCEEDED(hr)) {
        scale_transform->SetScaleX(1.0f);
        scale_transform->SetScaleY(-1.0f);
        scale_transform->SetCenterX(width / 2.0f);
        scale_transform->SetCenterY(height / 2.0f);
        dcomp_visual_->SetTransform(scale_transform.Get());
    }

    // Commit the composition
    hr = dcomp_device_->Commit();
    if (FAILED(hr)) {
        Debug::Log("D3D11HDRSwapchain: Commit failed");
        swapchain_.Reset();
        dcomp_visual_.Reset();
        dcomp_target_.Reset();
        dcomp_device_.Reset();
        return false;
    }

    swapchain_.As(&swapchain4_);
    Debug::Log("D3D11HDRSwapchain: Swapchain created via DirectComposition");
    return true;
}

bool D3D11HDRSwapchain::CreateRenderTargetView() {
    HRESULT hr = swapchain_->GetBuffer(0, IID_PPV_ARGS(&back_buffer_));
    if (FAILED(hr)) return false;

    hr = device_->CreateRenderTargetView(back_buffer_.Get(), nullptr, &rtv_);
    return SUCCEEDED(hr);
}

bool D3D11HDRSwapchain::DetectHDRSupport(bool log_always) {
    // Save previous state for change detection
    bool was_supported = hdr_info_.hdr_supported;
    bool was_enabled = hdr_info_.hdr_enabled;

    hdr_info_ = {};
    if (!device_) return false;

    try {
        // Get the adapter and enumerate outputs to find the one containing our window
        Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_device;
        if (FAILED(device_.As(&dxgi_device))) return false;

        Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
        if (FAILED(dxgi_device->GetAdapter(&adapter))) return false;

        // Find the output (monitor) that contains our window
        HMONITOR window_monitor = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);

        Microsoft::WRL::ComPtr<IDXGIOutput> output;
        for (UINT i = 0; adapter->EnumOutputs(i, &output) != DXGI_ERROR_NOT_FOUND; i++) {
            DXGI_OUTPUT_DESC output_desc;
            if (SUCCEEDED(output->GetDesc(&output_desc))) {
                if (output_desc.Monitor == window_monitor) {
                    break;  // Found our monitor
                }
            }
            output.Reset();
        }

        if (!output) {
            if (log_always) Debug::Log("D3D11HDRSwapchain: Could not find output for window");
            return false;
        }

        if (FAILED(output.As(&output6_))) {
            if (log_always) Debug::Log("D3D11HDRSwapchain: IDXGIOutput6 not available");
            return false;
        }

        DXGI_OUTPUT_DESC1 desc1;
        if (FAILED(output6_->GetDesc1(&desc1))) return false;

        hdr_info_.display_name = desc1.DeviceName;
        hdr_info_.min_luminance = desc1.MinLuminance;
        hdr_info_.max_luminance = desc1.MaxLuminance;
        hdr_info_.max_full_frame_luminance = desc1.MaxFullFrameLuminance;

        if (desc1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) {
            hdr_info_.hdr_supported = true;
            hdr_info_.hdr_enabled = true;
            hdr_info_.current_colorspace = desc1.ColorSpace;
            if (log_always || was_enabled != hdr_info_.hdr_enabled) {
                Debug::Log("D3D11HDRSwapchain: HDR enabled (max " + std::to_string(desc1.MaxLuminance) + " nits)");
            }
        } else if (desc1.MaxLuminance > 0 && desc1.BitsPerColor >= 10) {
            hdr_info_.hdr_supported = true;
            hdr_info_.hdr_enabled = false;
            hdr_info_.current_colorspace = desc1.ColorSpace;
            if (log_always || was_enabled != hdr_info_.hdr_enabled) {
                Debug::Log("D3D11HDRSwapchain: HDR supported but not enabled in Windows");
            }
        } else {
            hdr_info_.hdr_supported = false;
            hdr_info_.hdr_enabled = false;
            hdr_info_.current_colorspace = desc1.ColorSpace;
            if (log_always || was_supported != hdr_info_.hdr_supported) {
                Debug::Log("D3D11HDRSwapchain: SDR display detected");
            }
        }
        return true;
    } catch (...) {
        // Silently handle COM exceptions during HDR detection
        // This can happen during window state transitions
        hdr_info_.hdr_supported = was_supported;
        hdr_info_.hdr_enabled = was_enabled;
        return false;
    }
}

void D3D11HDRSwapchain::RefreshHDRStatus() {
    bool was_hdr = hdr_info_.hdr_enabled;
    DetectHDRSupport(false);  // Don't log unless status changed
    if (was_hdr != hdr_info_.hdr_enabled) {
        SetColorSpace(hdr_info_.hdr_enabled ?
            DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 :
            DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
    }
}

bool D3D11HDRSwapchain::SetColorSpace(DXGI_COLOR_SPACE_TYPE colorspace) {
    if (!swapchain4_) return false;
    if (FAILED(swapchain4_->SetColorSpace1(colorspace))) return false;
    hdr_info_.current_colorspace = colorspace;
    return true;
}

bool D3D11HDRSwapchain::Resize(int width, int height) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_ || (width == width_ && height == height_)) return true;

    rtv_.Reset();
    back_buffer_.Reset();
    DestroySharedRenderTarget();
    DestroyFallbackResources();

    HRESULT hr = swapchain_->ResizeBuffers(2, width, height, swapchain_format_, 0);
    if (FAILED(hr)) {
        Debug::Log("D3D11HDRSwapchain: ResizeBuffers failed (hr=0x" + std::to_string(hr) + ")");
        return false;
    }

    width_ = width;
    height_ = height;

    if (!CreateRenderTargetView()) return false;

    // Recreate render target for current interop method
    bool success = false;
    switch (interop_method_) {
        case InteropMethod::NV_DX_Interop:
            success = CreateNVInteropRenderTarget(width, height);
            break;
        case InteropMethod::EXT_Memory:
            success = CreateEXTMemoryRenderTarget(width, height);
            break;
        default:
            success = CreateFallbackResources(width, height);
            break;
    }

    // Update the flip transform with new center and commit
    if (dcomp_device_ && dcomp_visual_) {
        Microsoft::WRL::ComPtr<IDCompositionScaleTransform> scale_transform;
        HRESULT hr = dcomp_device_->CreateScaleTransform(&scale_transform);
        if (SUCCEEDED(hr)) {
            scale_transform->SetScaleX(1.0f);
            scale_transform->SetScaleY(-1.0f);
            scale_transform->SetCenterX(width / 2.0f);
            scale_transform->SetCenterY(height / 2.0f);
            dcomp_visual_->SetTransform(scale_transform.Get());
        }
        dcomp_device_->Commit();
    }

    Debug::Log("D3D11HDRSwapchain: Resized to " + std::to_string(width) + "x" + std::to_string(height));
    return success;
}

//=============================================================================
// NV_DX_interop (NVIDIA)
//=============================================================================

bool D3D11HDRSwapchain::InitializeNVInterop() {
    HMODULE opengl32 = GetModuleHandleA("opengl32.dll");
    if (!opengl32) return false;

    auto wglGetProcAddress = (void*(*)(const char*))GetProcAddress(opengl32, "wglGetProcAddress");
    if (!wglGetProcAddress) return false;

    wglDXOpenDeviceNV_ = (PFNWGLDXOPENDEVICENV)wglGetProcAddress("wglDXOpenDeviceNV");
    wglDXCloseDeviceNV_ = (PFNWGLDXCLOSEDEVICENV)wglGetProcAddress("wglDXCloseDeviceNV");
    wglDXRegisterObjectNV_ = (PFNWGLDXREGISTEROBJECTNV)wglGetProcAddress("wglDXRegisterObjectNV");
    wglDXUnregisterObjectNV_ = (PFNWGLDXUNREGISTEROBJECTNV)wglGetProcAddress("wglDXUnregisterObjectNV");
    wglDXLockObjectsNV_ = (PFNWGLDXLOCKOBJECTSNV)wglGetProcAddress("wglDXLockObjectsNV");
    wglDXUnlockObjectsNV_ = (PFNWGLDXUNLOCKOBJECTSNV)wglGetProcAddress("wglDXUnlockObjectsNV");

    if (!wglDXOpenDeviceNV_ || !wglDXCloseDeviceNV_ ||
        !wglDXRegisterObjectNV_ || !wglDXUnregisterObjectNV_ ||
        !wglDXLockObjectsNV_ || !wglDXUnlockObjectsNV_) {
        return false;
    }

    nv_interop_device_ = wglDXOpenDeviceNV_(device_.Get());
    if (!nv_interop_device_) {
        Debug::Log("D3D11HDRSwapchain: wglDXOpenDeviceNV failed (not NVIDIA?)");
        return false;
    }

    Debug::Log("D3D11HDRSwapchain: NV_DX_interop available");
    return true;
}

void D3D11HDRSwapchain::ShutdownNVInterop() {
    DestroyNVInteropRenderTarget();
    if (nv_interop_device_ && wglDXCloseDeviceNV_) {
        wglDXCloseDeviceNV_(nv_interop_device_);
        nv_interop_device_ = nullptr;
    }
}

bool D3D11HDRSwapchain::CreateNVInteropRenderTarget(int width, int height) {
    if (!nv_interop_device_) return false;

    DestroyNVInteropRenderTarget();  // Clean up old resources

    Debug::Log("D3D11HDRSwapchain: Creating " + std::to_string(INTEROP_BUFFER_COUNT) +
               " NV interop buffers for triple-buffering...");

    // Create 3 shared textures for triple-buffering
    for (int i = 0; i < INTEROP_BUFFER_COUNT; i++) {
        auto& buffer = interop_buffers_[i];

        // 1. Create D3D11 texture
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;  // HDR10
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        desc.MiscFlags = 0;

        HRESULT hr = device_->CreateTexture2D(&desc, nullptr, &buffer.d3d_texture);
        if (FAILED(hr)) {
            Debug::Log("D3D11HDRSwapchain: Failed to create shared texture " + std::to_string(i));
            DestroyNVInteropRenderTarget();
            return false;
        }

        // 2. Create GL texture
        glGenTextures(1, &buffer.gl_texture);
        if (!buffer.gl_texture) {
            Debug::Log("D3D11HDRSwapchain: Failed to create GL texture " + std::to_string(i));
            DestroyNVInteropRenderTarget();
            return false;
        }

        // 3. Register with NV_DX_interop
        buffer.nv_interop_object = wglDXRegisterObjectNV_(
            nv_interop_device_,
            buffer.d3d_texture.Get(),
            buffer.gl_texture,
            GL_TEXTURE_2D,
            WGL_ACCESS_READ_WRITE_NV
        );

        if (!buffer.nv_interop_object) {
            Debug::Log("D3D11HDRSwapchain: Failed to register NV interop object " + std::to_string(i));
            DestroyNVInteropRenderTarget();
            return false;
        }

        // 4. Create FBO for this buffer
        glGenFramebuffers(1, &buffer.fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, buffer.fbo);

        // Lock to attach to FBO
        if (!wglDXLockObjectsNV_(nv_interop_device_, 1, &buffer.nv_interop_object)) {
            Debug::Log("D3D11HDRSwapchain: Failed to lock for FBO setup " + std::to_string(i));
            DestroyNVInteropRenderTarget();
            return false;
        }

        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, buffer.gl_texture, 0);

        GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        wglDXUnlockObjectsNV_(nv_interop_device_, 1, &buffer.nv_interop_object);

        if (status != GL_FRAMEBUFFER_COMPLETE) {
            Debug::Log("D3D11HDRSwapchain: FBO incomplete for buffer " + std::to_string(i));
            DestroyNVInteropRenderTarget();
            return false;
        }

        buffer.initialized = true;

        Debug::Log("D3D11HDRSwapchain: Created interop buffer " + std::to_string(i) +
                   " (GL texture " + std::to_string(buffer.gl_texture) +
                   ", FBO " + std::to_string(buffer.fbo) + ")");
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // Set legacy members for backward compatibility
    render_texture_ = interop_buffers_[0].gl_texture;
    render_fbo_ = interop_buffers_[0].fbo;

    Debug::Log("D3D11HDRSwapchain: Successfully created " + std::to_string(INTEROP_BUFFER_COUNT) +
               " NV interop buffers for triple-buffering");
    return true;
}

void D3D11HDRSwapchain::DestroyNVInteropRenderTarget() {
    for (auto& buffer : interop_buffers_) {
        if (buffer.fbo) {
            glDeleteFramebuffers(1, &buffer.fbo);
            buffer.fbo = 0;
        }

        if (buffer.nv_interop_object && nv_interop_device_ && wglDXUnregisterObjectNV_) {
            wglDXUnregisterObjectNV_(nv_interop_device_, buffer.nv_interop_object);
            buffer.nv_interop_object = nullptr;
        }

        if (buffer.gl_texture) {
            glDeleteTextures(1, &buffer.gl_texture);
            buffer.gl_texture = 0;
        }

        buffer.d3d_texture.Reset();
        buffer.initialized = false;
    }

    current_write_buffer_ = 0;
    current_present_buffer_ = 0;

    // Clear legacy members
    render_texture_ = 0;
    render_fbo_ = 0;
}

//=============================================================================
// EXT_external_objects (Intel Arc, AMD, NVIDIA)
//=============================================================================

bool D3D11HDRSwapchain::InitializeEXTMemoryInterop() {
    // Check for GL_EXT_memory_object_win32 extension
    const char* extensions = (const char*)glGetString(GL_EXTENSIONS);
    if (!extensions) return false;

    bool has_memory_object = strstr(extensions, "GL_EXT_memory_object") != nullptr;
    bool has_memory_object_win32 = strstr(extensions, "GL_EXT_memory_object_win32") != nullptr;

    if (!has_memory_object || !has_memory_object_win32) {
        Debug::Log("D3D11HDRSwapchain: EXT_memory_object_win32 not available");
        return false;
    }

    // Load function pointers
    glCreateMemoryObjectsEXT_ = (PFNGLCREATEMEMORYOBJECTSEXTPROC)wglGetProcAddress("glCreateMemoryObjectsEXT");
    glDeleteMemoryObjectsEXT_ = (PFNGLDELETEMEMORYOBJECTSEXTPROC)wglGetProcAddress("glDeleteMemoryObjectsEXT");
    glTexStorageMem2DEXT_ = (PFNGLTEXSTORAGEMEM2DEXTPROC)wglGetProcAddress("glTexStorageMem2DEXT");
    glImportMemoryWin32HandleEXT_ = (PFNGLIMPORTMEMORYWIN32HANDLEEXTPROC)wglGetProcAddress("glImportMemoryWin32HandleEXT");

    if (!glCreateMemoryObjectsEXT_ || !glDeleteMemoryObjectsEXT_ ||
        !glTexStorageMem2DEXT_ || !glImportMemoryWin32HandleEXT_) {
        Debug::Log("D3D11HDRSwapchain: Failed to load EXT_memory_object functions");
        return false;
    }

    ext_memory_available_ = true;
    Debug::Log("D3D11HDRSwapchain: EXT_memory_object available");
    return true;
}

void D3D11HDRSwapchain::ShutdownEXTMemoryInterop() {
    DestroyEXTMemoryRenderTarget();
    ext_memory_available_ = false;
}

bool D3D11HDRSwapchain::CreateEXTMemoryRenderTarget(int width, int height) {
    if (!ext_memory_available_) return false;

    DestroyEXTMemoryRenderTarget();  // Clean up old resources

    Debug::Log("D3D11HDRSwapchain: Creating " + std::to_string(INTEROP_BUFFER_COUNT) +
               " EXT_memory interop buffers for triple-buffering...");

    // Create 3 shared textures for triple-buffering
    for (int i = 0; i < INTEROP_BUFFER_COUNT; i++) {
        auto& buffer = interop_buffers_[i];

        // 1. Create D3D11 texture with shared handle support
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

        if (FAILED(device_->CreateTexture2D(&desc, nullptr, &buffer.d3d_texture))) {
            Debug::Log("D3D11HDRSwapchain: Failed to create shared texture " + std::to_string(i));
            DestroyEXTMemoryRenderTarget();
            return false;
        }

        // 2. Get keyed mutex for synchronization
        if (FAILED(buffer.d3d_texture.As(&buffer.keyed_mutex))) {
            Debug::Log("D3D11HDRSwapchain: Failed to get keyed mutex " + std::to_string(i));
            DestroyEXTMemoryRenderTarget();
            return false;
        }

        // 3. Get shared handle
        Microsoft::WRL::ComPtr<IDXGIResource1> dxgi_resource;
        if (FAILED(buffer.d3d_texture.As(&dxgi_resource))) {
            Debug::Log("D3D11HDRSwapchain: Failed to get DXGI resource " + std::to_string(i));
            DestroyEXTMemoryRenderTarget();
            return false;
        }

        if (FAILED(dxgi_resource->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &buffer.shared_handle))) {
            Debug::Log("D3D11HDRSwapchain: Failed to create shared handle " + std::to_string(i));
            DestroyEXTMemoryRenderTarget();
            return false;
        }

        // 4. Create GL memory object and import the D3D11 texture
        glCreateMemoryObjectsEXT_(1, &buffer.ext_memory_object);
        if (!buffer.ext_memory_object) {
            Debug::Log("D3D11HDRSwapchain: Failed to create GL memory object " + std::to_string(i));
            DestroyEXTMemoryRenderTarget();
            return false;
        }

        // Calculate texture size (R10G10B10A2 = 4 bytes per pixel)
        GLuint64 texture_size = (GLuint64)width * height * 4;

        // Import the D3D11 texture into GL
        glImportMemoryWin32HandleEXT_(buffer.ext_memory_object, texture_size,
                                       GL_HANDLE_TYPE_D3D11_IMAGE_EXT, buffer.shared_handle);

        GLenum err = glGetError();
        if (err != GL_NO_ERROR) {
            Debug::Log("D3D11HDRSwapchain: glImportMemoryWin32HandleEXT failed for buffer " +
                      std::to_string(i) + " (GL error " + std::to_string(err) + ")");
            DestroyEXTMemoryRenderTarget();
            return false;
        }

        // 5. Create GL texture backed by the shared memory
        glGenTextures(1, &buffer.gl_texture);
        glBindTexture(GL_TEXTURE_2D, buffer.gl_texture);
        glTexStorageMem2DEXT_(GL_TEXTURE_2D, 1, GL_RGB10_A2, width, height, buffer.ext_memory_object, 0);

        err = glGetError();
        if (err != GL_NO_ERROR) {
            Debug::Log("D3D11HDRSwapchain: glTexStorageMem2DEXT failed for buffer " +
                      std::to_string(i) + " (GL error " + std::to_string(err) + ")");
            DestroyEXTMemoryRenderTarget();
            return false;
        }

        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glBindTexture(GL_TEXTURE_2D, 0);

        // 6. Create FBO
        glGenFramebuffers(1, &buffer.fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, buffer.fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, buffer.gl_texture, 0);

        GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        if (status != GL_FRAMEBUFFER_COMPLETE) {
            Debug::Log("D3D11HDRSwapchain: EXT_memory FBO incomplete for buffer " +
                      std::to_string(i) + " (status " + std::to_string(status) + ")");
            DestroyEXTMemoryRenderTarget();
            return false;
        }

        buffer.initialized = true;

        Debug::Log("D3D11HDRSwapchain: Created EXT_memory buffer " + std::to_string(i) +
                   " (GL texture " + std::to_string(buffer.gl_texture) +
                   ", FBO " + std::to_string(buffer.fbo) + ")");
    }

    // Set legacy members for backward compatibility
    render_texture_ = interop_buffers_[0].gl_texture;
    render_fbo_ = interop_buffers_[0].fbo;

    Debug::Log("D3D11HDRSwapchain: Successfully created " + std::to_string(INTEROP_BUFFER_COUNT) +
               " EXT_memory buffers for triple-buffering");
    return true;
}

void D3D11HDRSwapchain::DestroyEXTMemoryRenderTarget() {
    for (auto& buffer : interop_buffers_) {
        if (buffer.fbo) {
            glDeleteFramebuffers(1, &buffer.fbo);
            buffer.fbo = 0;
        }

        if (buffer.gl_texture) {
            glDeleteTextures(1, &buffer.gl_texture);
            buffer.gl_texture = 0;
        }

        if (buffer.ext_memory_object) {
            if (glDeleteMemoryObjectsEXT_) {
                glDeleteMemoryObjectsEXT_(1, &buffer.ext_memory_object);
            }
            buffer.ext_memory_object = 0;
        }

        if (buffer.shared_handle) {
            CloseHandle(buffer.shared_handle);
            buffer.shared_handle = nullptr;
        }

        buffer.keyed_mutex.Reset();
        buffer.d3d_texture.Reset();
        buffer.initialized = false;
    }

    current_write_buffer_ = 0;
    current_present_buffer_ = 0;

    // Clear legacy members
    render_texture_ = 0;
    render_fbo_ = 0;
}

//=============================================================================
// Shared Render Target Management
//=============================================================================

bool D3D11HDRSwapchain::CreateSharedRenderTarget(int width, int height) {
    switch (interop_method_) {
        case InteropMethod::NV_DX_Interop:
            return CreateNVInteropRenderTarget(width, height);
        case InteropMethod::EXT_Memory:
            return CreateEXTMemoryRenderTarget(width, height);
        default:
            return CreateFallbackResources(width, height);
    }
}

void D3D11HDRSwapchain::DestroySharedRenderTarget() {
    switch (interop_method_) {
        case InteropMethod::NV_DX_Interop:
            DestroyNVInteropRenderTarget();
            break;
        case InteropMethod::EXT_Memory:
            DestroyEXTMemoryRenderTarget();
            break;
        default:
            DestroyFallbackResources();
            break;
    }
}

//=============================================================================
// Fallback: CPU Readback
//=============================================================================

bool D3D11HDRSwapchain::CreateFallbackResources(int width, int height) {
    glGenTextures(1, &fallback_texture_);
    glBindTexture(GL_TEXTURE_2D, fallback_texture_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB10_A2, width, height, 0,
                 GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D, 0);

    glGenFramebuffers(1, &fallback_fbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, fallback_fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fallback_texture_, 0);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    if (status != GL_FRAMEBUFFER_COMPLETE) {
        glDeleteFramebuffers(1, &fallback_fbo_);
        fallback_fbo_ = 0;
        glDeleteTextures(1, &fallback_texture_);
        fallback_texture_ = 0;
        return false;
    }

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    if (FAILED(device_->CreateTexture2D(&desc, nullptr, &staging_texture_))) {
        glDeleteFramebuffers(1, &fallback_fbo_);
        fallback_fbo_ = 0;
        glDeleteTextures(1, &fallback_texture_);
        fallback_texture_ = 0;
        return false;
    }

    readback_buffer_.resize(width * height * 4);
    return true;
}

void D3D11HDRSwapchain::DestroyFallbackResources() {
    if (fallback_fbo_) {
        glDeleteFramebuffers(1, &fallback_fbo_);
        fallback_fbo_ = 0;
    }
    if (fallback_texture_) {
        glDeleteTextures(1, &fallback_texture_);
        fallback_texture_ = 0;
    }
    staging_texture_.Reset();
    readback_buffer_.clear();
}

bool D3D11HDRSwapchain::CopyViaFallback() {
    if (!fallback_texture_ || !staging_texture_) return false;

    glBindTexture(GL_TEXTURE_2D, fallback_texture_);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV, readback_buffer_.data());
    glBindTexture(GL_TEXTURE_2D, 0);

    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(context_->Map(staging_texture_.Get(), 0, D3D11_MAP_WRITE, 0, &mapped))) return false;

    const uint8_t* src = readback_buffer_.data();
    uint8_t* dst = static_cast<uint8_t*>(mapped.pData);
    size_t row_bytes = width_ * 4;

    for (int y = 0; y < height_; y++) {
        memcpy(dst, src, row_bytes);
        src += row_bytes;
        dst += mapped.RowPitch;
    }

    context_->Unmap(staging_texture_.Get(), 0);
    context_->CopyResource(back_buffer_.Get(), staging_texture_.Get());
    return true;
}

//=============================================================================
// Frame Rendering
//=============================================================================

GLuint D3D11HDRSwapchain::BeginFrame() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_ || in_frame_) return 0;

    GLuint result_fbo = 0;

    switch (interop_method_) {
        case InteropMethod::NV_DX_Interop: {
            // Try to find a free buffer (prefer round-robin order)
            int attempts = INTEROP_BUFFER_COUNT;
            int start_index = current_write_buffer_;

            while (attempts-- > 0) {
                auto& buffer = interop_buffers_[current_write_buffer_];

                if (!buffer.initialized) {
                    // Skip uninitialized buffers
                    current_write_buffer_ = (current_write_buffer_ + 1) % INTEROP_BUFFER_COUNT;
                    continue;
                }

                // Try to lock this buffer (non-blocking on modern drivers)
                if (wglDXLockObjectsNV_(nv_interop_device_, 1, &buffer.nv_interop_object)) {
                    // Success! This buffer is free
                    result_fbo = buffer.fbo;
                    //Debug::Log("BeginFrame: Using NV interop buffer " + std::to_string(current_write_buffer_));
                    break;
                }

                // Buffer is busy, try next one
                Debug::Log("BeginFrame: Buffer " + std::to_string(current_write_buffer_) +
                          " busy, trying next");
                current_write_buffer_ = (current_write_buffer_ + 1) % INTEROP_BUFFER_COUNT;
            }

            if (!result_fbo) {
                // All buffers are locked - this should rarely happen with 3 buffers
                Debug::Log("ERROR: All " + std::to_string(INTEROP_BUFFER_COUNT) +
                          " interop buffers are locked! GPU may be stalled.");
                // Fall back to blocking wait on the original buffer
                auto& buffer = interop_buffers_[start_index];
                if (wglDXLockObjectsNV_(nv_interop_device_, 1, &buffer.nv_interop_object)) {
                    result_fbo = buffer.fbo;
                    current_write_buffer_ = start_index;
                }
            }
            break;
        }

        case InteropMethod::EXT_Memory: {
            // Similar pattern for EXT_memory_object (use keyed mutex)
            int attempts = INTEROP_BUFFER_COUNT;

            while (attempts-- > 0) {
                auto& buffer = interop_buffers_[current_write_buffer_];

                if (!buffer.initialized || !buffer.keyed_mutex) {
                    current_write_buffer_ = (current_write_buffer_ + 1) % INTEROP_BUFFER_COUNT;
                    continue;
                }

                // Try to acquire with 0ms timeout (non-blocking)
                HRESULT hr = buffer.keyed_mutex->AcquireSync(0, 0);
                if (SUCCEEDED(hr)) {
                    result_fbo = buffer.fbo;
                    Debug::Log("BeginFrame: Using EXT_Memory buffer " + std::to_string(current_write_buffer_));
                    break;
                }

                // Busy, try next buffer
                current_write_buffer_ = (current_write_buffer_ + 1) % INTEROP_BUFFER_COUNT;
            }
            break;
        }

        default:
            // CPU fallback - single buffer is fine
            result_fbo = fallback_fbo_;
            break;
    }

    if (result_fbo) {
        glBindFramebuffer(GL_FRAMEBUFFER, result_fbo);
        in_frame_ = true;
    }

    return result_fbo;
}

void D3D11HDRSwapchain::EndFrame(bool vsync) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_ || !in_frame_) return;

    auto start_time = std::chrono::high_resolution_clock::now();

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    bool copy_success = false;

    switch (interop_method_) {
        case InteropMethod::NV_DX_Interop: {
            auto& buffer = interop_buffers_[current_write_buffer_];

            if (buffer.nv_interop_object) {
                // Unlock the buffer we just rendered into
                wglDXUnlockObjectsNV_(nv_interop_device_, 1, &buffer.nv_interop_object);

                // Copy to back buffer for presentation
                // This is async - GPU can do this while CPU prepares next frame
                context_->CopyResource(back_buffer_.Get(), buffer.d3d_texture.Get());

                copy_success = true;
                stats_.zero_copy_frames++;

                // Track which buffer we're presenting
                current_present_buffer_ = current_write_buffer_;

                // Advance to next buffer for next BeginFrame()
                current_write_buffer_ = (current_write_buffer_ + 1) % INTEROP_BUFFER_COUNT;

                /*Debug::Log("EndFrame: Released buffer " + std::to_string(current_present_buffer_) +
                          ", next buffer will be " + std::to_string(current_write_buffer_));*/
            }
            break;
        }

        case InteropMethod::EXT_Memory: {
            auto& buffer = interop_buffers_[current_write_buffer_];

            if (buffer.keyed_mutex) {
                // Flush GL commands before releasing
                glFlush();

                // Release keyed mutex for D3D access
                buffer.keyed_mutex->ReleaseSync(0);

                // Copy to back buffer
                context_->CopyResource(back_buffer_.Get(), buffer.d3d_texture.Get());

                copy_success = true;
                stats_.zero_copy_frames++;

                current_present_buffer_ = current_write_buffer_;
                current_write_buffer_ = (current_write_buffer_ + 1) % INTEROP_BUFFER_COUNT;

                Debug::Log("EndFrame: Released EXT_Memory buffer " + std::to_string(current_present_buffer_));
            }
            break;
        }

        default: {
            // CPU fallback
            auto copy_start = std::chrono::high_resolution_clock::now();
            copy_success = CopyViaFallback();
            auto copy_end = std::chrono::high_resolution_clock::now();
            stats_.last_copy_time_ms = std::chrono::duration<double, std::milli>(copy_end - copy_start).count();
            if (copy_success) stats_.cpu_readback_frames++;
            break;
        }
    }

    in_frame_ = false;

    if (!copy_success) return;

    // Present - this may block on vsync, but doesn't affect next BeginFrame()
    // because we already released the buffer and advanced to the next one
    HRESULT hr = swapchain_->Present(vsync ? 1 : 0, 0);
    if (SUCCEEDED(hr)) stats_.frames_presented++;

    auto end_time = std::chrono::high_resolution_clock::now();
    stats_.last_present_time_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();
}

//=============================================================================
// Full D3D11 Mode (No GL Interop)
//=============================================================================

void D3D11HDRSwapchain::BeginD3D11Frame() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_ || in_frame_) return;

    // Bind the backbuffer as render target
    context_->OMSetRenderTargets(1, rtv_.GetAddressOf(), nullptr);

    // Set viewport
    D3D11_VIEWPORT vp = {};
    vp.Width = static_cast<float>(width_);
    vp.Height = static_cast<float>(height_);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    context_->RSSetViewports(1, &vp);

    // Clear to dark background
    float clear_color[4] = { 27.0f/255.0f, 27.0f/255.0f, 27.0f/255.0f, 1.0f };
    context_->ClearRenderTargetView(rtv_.Get(), clear_color);

    in_frame_ = true;
}

void D3D11HDRSwapchain::EndD3D11Frame(bool vsync) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_ || !in_frame_) return;

    // Unbind render target
    ID3D11RenderTargetView* null_rtv = nullptr;
    context_->OMSetRenderTargets(1, &null_rtv, nullptr);

    in_frame_ = false;

    // Present
    HRESULT hr = swapchain_->Present(vsync ? 1 : 0, 0);
    if (SUCCEEDED(hr)) {
        stats_.frames_presented++;
    }
}

bool D3D11HDRSwapchain::CopyFromOpenGLAndPresent(GLuint gl_texture, int tex_width, int tex_height, bool vsync) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) return false;

    if (tex_width != width_ || tex_height != height_) {
        mutex_.unlock();
        Resize(tex_width, tex_height);
        mutex_.lock();
    }

    size_t required_size = tex_width * tex_height * 4;
    if (readback_buffer_.size() < required_size) readback_buffer_.resize(required_size);

    glBindTexture(GL_TEXTURE_2D, gl_texture);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV, readback_buffer_.data());
    glBindTexture(GL_TEXTURE_2D, 0);

    if (!staging_texture_) {
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = tex_width;
        desc.Height = tex_height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        device_->CreateTexture2D(&desc, nullptr, &staging_texture_);
    }

    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(context_->Map(staging_texture_.Get(), 0, D3D11_MAP_WRITE, 0, &mapped))) return false;

    const uint8_t* src = readback_buffer_.data();
    uint8_t* dst = static_cast<uint8_t*>(mapped.pData);
    for (int y = 0; y < tex_height; y++) {
        memcpy(dst, src, tex_width * 4);
        src += tex_width * 4;
        dst += mapped.RowPitch;
    }

    context_->Unmap(staging_texture_.Get(), 0);
    context_->CopyResource(back_buffer_.Get(), staging_texture_.Get());

    HRESULT hr = swapchain_->Present(vsync ? 1 : 0, 0);
    if (SUCCEEDED(hr)) {
        stats_.frames_presented++;
        stats_.cpu_readback_frames++;
    }

    return SUCCEEDED(hr);
}

} // namespace ump

#endif // _WIN32
