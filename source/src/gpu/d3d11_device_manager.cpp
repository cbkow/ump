#include "d3d11_device_manager.h"

#ifdef _WIN32

#include <d3dcompiler.h>
#include "../utils/debug_utils.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace ump {

D3D11DeviceManager& D3D11DeviceManager::Instance() {
    static D3D11DeviceManager instance;
    return instance;
}

D3D11DeviceManager::D3D11DeviceManager() = default;

D3D11DeviceManager::~D3D11DeviceManager() {
    Shutdown();
}

bool D3D11DeviceManager::Initialize(HWND hwnd) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (initialized_) {
        // Already initialized, just update hwnd if provided
        if (hwnd) hwnd_ = hwnd;
        return true;
    }

    hwnd_ = hwnd;

    if (!CreateDevice()) {
        Debug::Log("D3D11DeviceManager: Failed to create D3D11 device");
        return false;
    }

    if (!GetDXGIObjects()) {
        Debug::Log("D3D11DeviceManager: Failed to get DXGI objects");
        Shutdown();
        return false;
    }

    initialized_ = true;
    Debug::Log("D3D11DeviceManager: Initialized successfully");
    Debug::Log("  Adapter: " + adapter_description_);
    Debug::Log("  Feature Level: " + std::to_string(feature_level_));
    Debug::Log("  VRAM: " + std::to_string(dedicated_video_memory_ / (1024 * 1024)) + " MB");

    return true;
}

void D3D11DeviceManager::Shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);

    staging_texture_.Reset();
    staging_width_ = 0;
    staging_height_ = 0;
    staging_format_ = DXGI_FORMAT_UNKNOWN;

    factory_.Reset();
    adapter_.Reset();
    dxgi_device_.Reset();
    context_.Reset();
    device_.Reset();

    initialized_ = false;
    Debug::Log("D3D11DeviceManager: Shutdown complete");
}

bool D3D11DeviceManager::CreateDevice() {
    UINT create_flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
    create_flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    D3D_FEATURE_LEVEL feature_levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };

    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;

    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, create_flags,
        feature_levels, _countof(feature_levels), D3D11_SDK_VERSION,
        &device, &feature_level_, &context
    );

    if (FAILED(hr)) {
        Debug::Log("D3D11DeviceManager: D3D11CreateDevice failed (hr=0x" +
                   std::to_string(hr) + ")");
        return false;
    }

    // Get ID3D11Device1 for enhanced features
    hr = device.As(&device_);
    if (FAILED(hr)) {
        Debug::Log("D3D11DeviceManager: Failed to get ID3D11Device1");
        return false;
    }

    hr = context.As(&context_);
    if (FAILED(hr)) {
        Debug::Log("D3D11DeviceManager: Failed to get ID3D11DeviceContext1");
        return false;
    }

    return true;
}

bool D3D11DeviceManager::GetDXGIObjects() {
    HRESULT hr = device_.As(&dxgi_device_);
    if (FAILED(hr)) {
        Debug::Log("D3D11DeviceManager: Failed to get IDXGIDevice");
        return false;
    }

    hr = dxgi_device_->GetAdapter(&adapter_);
    if (FAILED(hr)) {
        Debug::Log("D3D11DeviceManager: Failed to get adapter");
        return false;
    }

    hr = adapter_->GetParent(IID_PPV_ARGS(&factory_));
    if (FAILED(hr)) {
        Debug::Log("D3D11DeviceManager: Failed to get factory");
        return false;
    }

    // Get adapter info
    DXGI_ADAPTER_DESC adapter_desc;
    if (SUCCEEDED(adapter_->GetDesc(&adapter_desc))) {
        dedicated_video_memory_ = adapter_desc.DedicatedVideoMemory;

        // Convert wide string to narrow
        char desc[128];
        wcstombs(desc, adapter_desc.Description, sizeof(desc));
        adapter_description_ = desc;
    }

    return true;
}

//=============================================================================
// Texture Creation
//=============================================================================

Microsoft::WRL::ComPtr<ID3D11Texture2D> D3D11DeviceManager::CreateTexture2D(
    UINT width, UINT height,
    DXGI_FORMAT format,
    D3D11_USAGE usage,
    UINT bind_flags,
    UINT cpu_access_flags,
    UINT misc_flags,
    const D3D11_SUBRESOURCE_DATA* initial_data) {

    if (!device_) return nullptr;

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = usage;
    desc.BindFlags = bind_flags;
    desc.CPUAccessFlags = cpu_access_flags;
    desc.MiscFlags = misc_flags;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    HRESULT hr = device_->CreateTexture2D(&desc, initial_data, &texture);

    if (FAILED(hr)) {
        Debug::Log("D3D11DeviceManager: CreateTexture2D failed (hr=0x" +
                   std::to_string(hr) + ")");
        return nullptr;
    }

    return texture;
}

Microsoft::WRL::ComPtr<ID3D11Texture2D> D3D11DeviceManager::CreateStagingTexture(
    UINT width, UINT height,
    DXGI_FORMAT format,
    bool cpu_read,
    bool cpu_write) {

    UINT cpu_access = 0;
    if (cpu_read) cpu_access |= D3D11_CPU_ACCESS_READ;
    if (cpu_write) cpu_access |= D3D11_CPU_ACCESS_WRITE;

    return CreateTexture2D(width, height, format,
                          D3D11_USAGE_STAGING, 0, cpu_access);
}

Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> D3D11DeviceManager::CreateSRV(
    ID3D11Texture2D* texture,
    DXGI_FORMAT format) {

    if (!device_ || !texture) return nullptr;

    D3D11_TEXTURE2D_DESC tex_desc;
    texture->GetDesc(&tex_desc);

    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
    srv_desc.Format = (format != DXGI_FORMAT_UNKNOWN) ? format : tex_desc.Format;
    srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Texture2D.MipLevels = 1;

    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
    HRESULT hr = device_->CreateShaderResourceView(texture, &srv_desc, &srv);

    if (FAILED(hr)) {
        Debug::Log("D3D11DeviceManager: CreateSRV failed (hr=0x" +
                   std::to_string(hr) + ")");
        return nullptr;
    }

    return srv;
}

Microsoft::WRL::ComPtr<ID3D11RenderTargetView> D3D11DeviceManager::CreateRTV(
    ID3D11Texture2D* texture,
    DXGI_FORMAT format) {

    if (!device_ || !texture) return nullptr;

    D3D11_TEXTURE2D_DESC tex_desc;
    texture->GetDesc(&tex_desc);

    D3D11_RENDER_TARGET_VIEW_DESC rtv_desc = {};
    rtv_desc.Format = (format != DXGI_FORMAT_UNKNOWN) ? format : tex_desc.Format;
    rtv_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;

    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
    HRESULT hr = device_->CreateRenderTargetView(texture, &rtv_desc, &rtv);

    if (FAILED(hr)) {
        Debug::Log("D3D11DeviceManager: CreateRTV failed (hr=0x" +
                   std::to_string(hr) + ")");
        return nullptr;
    }

    return rtv;
}

Microsoft::WRL::ComPtr<ID3D11Texture1D> D3D11DeviceManager::CreateTexture1D(
    UINT width,
    DXGI_FORMAT format,
    const void* data) {

    if (!device_) return nullptr;

    D3D11_TEXTURE1D_DESC desc = {};
    desc.Width = width;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA init_data = {};
    D3D11_SUBRESOURCE_DATA* init_ptr = nullptr;

    if (data) {
        // Calculate row pitch based on format
        UINT bytes_per_element = 0;
        switch (format) {
            case DXGI_FORMAT_R32_FLOAT: bytes_per_element = 4; break;
            case DXGI_FORMAT_R32G32B32_FLOAT: bytes_per_element = 12; break;
            case DXGI_FORMAT_R32G32B32A32_FLOAT: bytes_per_element = 16; break;
            default: bytes_per_element = 4; break;
        }
        init_data.pSysMem = data;
        init_data.SysMemPitch = width * bytes_per_element;
        init_ptr = &init_data;
    }

    Microsoft::WRL::ComPtr<ID3D11Texture1D> texture;
    HRESULT hr = device_->CreateTexture1D(&desc, init_ptr, &texture);

    if (FAILED(hr)) {
        Debug::Log("D3D11DeviceManager: CreateTexture1D failed (hr=0x" +
                   std::to_string(hr) + ")");
        return nullptr;
    }

    return texture;
}

Microsoft::WRL::ComPtr<ID3D11Texture3D> D3D11DeviceManager::CreateTexture3D(
    UINT width, UINT height, UINT depth,
    DXGI_FORMAT format,
    const void* data) {

    if (!device_) return nullptr;

    D3D11_TEXTURE3D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.Depth = depth;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA init_data = {};
    D3D11_SUBRESOURCE_DATA* init_ptr = nullptr;

    if (data) {
        UINT bytes_per_element = 0;
        switch (format) {
            case DXGI_FORMAT_R32G32B32_FLOAT: bytes_per_element = 12; break;
            case DXGI_FORMAT_R32G32B32A32_FLOAT: bytes_per_element = 16; break;
            default: bytes_per_element = 12; break;
        }
        init_data.pSysMem = data;
        init_data.SysMemPitch = width * bytes_per_element;
        init_data.SysMemSlicePitch = width * height * bytes_per_element;
        init_ptr = &init_data;
    }

    Microsoft::WRL::ComPtr<ID3D11Texture3D> texture;
    HRESULT hr = device_->CreateTexture3D(&desc, init_ptr, &texture);

    if (FAILED(hr)) {
        Debug::Log("D3D11DeviceManager: CreateTexture3D failed (hr=0x" +
                   std::to_string(hr) + ")");
        return nullptr;
    }

    return texture;
}

Microsoft::WRL::ComPtr<ID3D11SamplerState> D3D11DeviceManager::CreateSamplerState(
    D3D11_FILTER filter,
    D3D11_TEXTURE_ADDRESS_MODE address_mode) {

    if (!device_) return nullptr;

    D3D11_SAMPLER_DESC desc = {};
    desc.Filter = filter;
    desc.AddressU = address_mode;
    desc.AddressV = address_mode;
    desc.AddressW = address_mode;
    desc.MaxAnisotropy = 1;
    desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    desc.MinLOD = 0;
    desc.MaxLOD = D3D11_FLOAT32_MAX;

    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler;
    HRESULT hr = device_->CreateSamplerState(&desc, &sampler);

    if (FAILED(hr)) {
        Debug::Log("D3D11DeviceManager: CreateSamplerState failed");
        return nullptr;
    }

    return sampler;
}

Microsoft::WRL::ComPtr<ID3D11Buffer> D3D11DeviceManager::CreateConstantBuffer(UINT size) {
    if (!device_) return nullptr;

    // Constant buffers must be 16-byte aligned
    size = (size + 15) & ~15;

    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth = size;
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    Microsoft::WRL::ComPtr<ID3D11Buffer> buffer;
    HRESULT hr = device_->CreateBuffer(&desc, nullptr, &buffer);

    if (FAILED(hr)) {
        Debug::Log("D3D11DeviceManager: CreateConstantBuffer failed");
        return nullptr;
    }

    return buffer;
}

Microsoft::WRL::ComPtr<ID3D11Buffer> D3D11DeviceManager::CreateVertexBuffer(
    const void* data, UINT size, bool dynamic) {

    if (!device_) return nullptr;

    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth = size;
    desc.Usage = dynamic ? D3D11_USAGE_DYNAMIC : D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    desc.CPUAccessFlags = dynamic ? D3D11_CPU_ACCESS_WRITE : 0;

    D3D11_SUBRESOURCE_DATA init_data = {};
    D3D11_SUBRESOURCE_DATA* init_ptr = nullptr;

    if (data) {
        init_data.pSysMem = data;
        init_ptr = &init_data;
    }

    Microsoft::WRL::ComPtr<ID3D11Buffer> buffer;
    HRESULT hr = device_->CreateBuffer(&desc, init_ptr, &buffer);

    if (FAILED(hr)) {
        Debug::Log("D3D11DeviceManager: CreateVertexBuffer failed");
        return nullptr;
    }

    return buffer;
}

Microsoft::WRL::ComPtr<ID3D11Buffer> D3D11DeviceManager::CreateIndexBuffer(
    const void* data, UINT size) {

    if (!device_) return nullptr;

    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth = size;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_INDEX_BUFFER;

    D3D11_SUBRESOURCE_DATA init_data = {};
    init_data.pSysMem = data;

    Microsoft::WRL::ComPtr<ID3D11Buffer> buffer;
    HRESULT hr = device_->CreateBuffer(&desc, data ? &init_data : nullptr, &buffer);

    if (FAILED(hr)) {
        Debug::Log("D3D11DeviceManager: CreateIndexBuffer failed");
        return nullptr;
    }

    return buffer;
}

//=============================================================================
// Texture Upload
//=============================================================================

bool D3D11DeviceManager::UploadTextureData(
    ID3D11Texture2D* dest_texture,
    const void* pixel_data,
    UINT width, UINT height,
    UINT row_pitch) {

    if (!device_ || !context_ || !dest_texture || !pixel_data) return false;

    D3D11_TEXTURE2D_DESC dest_desc;
    dest_texture->GetDesc(&dest_desc);

    // Check if we need to recreate staging texture
    if (!staging_texture_ ||
        staging_width_ != width ||
        staging_height_ != height ||
        staging_format_ != dest_desc.Format) {

        staging_texture_ = CreateStagingTexture(width, height, dest_desc.Format, false, true);
        if (!staging_texture_) return false;

        staging_width_ = width;
        staging_height_ = height;
        staging_format_ = dest_desc.Format;
    }

    // Map staging texture
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = context_->Map(staging_texture_.Get(), 0, D3D11_MAP_WRITE, 0, &mapped);
    if (FAILED(hr)) {
        Debug::Log("D3D11DeviceManager: Failed to map staging texture");
        return false;
    }

    // Copy data row by row (handles row pitch differences)
    const uint8_t* src = static_cast<const uint8_t*>(pixel_data);
    uint8_t* dst = static_cast<uint8_t*>(mapped.pData);
    UINT copy_pitch = std::min(row_pitch, mapped.RowPitch);

    for (UINT y = 0; y < height; ++y) {
        memcpy(dst, src, copy_pitch);
        src += row_pitch;
        dst += mapped.RowPitch;
    }

    context_->Unmap(staging_texture_.Get(), 0);

    // Copy to destination texture
    context_->CopyResource(dest_texture, staging_texture_.Get());

    return true;
}

bool D3D11DeviceManager::UpdateDynamicTexture(
    ID3D11Texture2D* texture,
    const void* pixel_data,
    UINT row_pitch) {

    if (!context_ || !texture || !pixel_data) return false;

    D3D11_TEXTURE2D_DESC desc;
    texture->GetDesc(&desc);

    if (desc.Usage != D3D11_USAGE_DYNAMIC) {
        Debug::Log("D3D11DeviceManager: Texture is not dynamic");
        return false;
    }

    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = context_->Map(texture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) return false;

    const uint8_t* src = static_cast<const uint8_t*>(pixel_data);
    uint8_t* dst = static_cast<uint8_t*>(mapped.pData);
    UINT copy_pitch = std::min(row_pitch, mapped.RowPitch);

    for (UINT y = 0; y < desc.Height; ++y) {
        memcpy(dst, src, copy_pitch);
        src += row_pitch;
        dst += mapped.RowPitch;
    }

    context_->Unmap(texture, 0);
    return true;
}

//=============================================================================
// Shader Compilation
//=============================================================================

Microsoft::WRL::ComPtr<ID3D11VertexShader> D3D11DeviceManager::CompileVertexShader(
    const std::string& hlsl_source,
    const std::string& entry_point,
    Microsoft::WRL::ComPtr<ID3DBlob>* out_bytecode) {

    if (!device_) return nullptr;

    Microsoft::WRL::ComPtr<ID3DBlob> bytecode;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;

    HRESULT hr = D3DCompile(
        hlsl_source.c_str(), hlsl_source.length(),
        nullptr, nullptr, nullptr,
        entry_point.c_str(), "vs_5_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
        &bytecode, &errors
    );

    if (FAILED(hr)) {
        if (errors) {
            Debug::Log("D3D11DeviceManager: Vertex shader compilation failed: " +
                      std::string(static_cast<char*>(errors->GetBufferPointer())));
        }
        return nullptr;
    }

    Microsoft::WRL::ComPtr<ID3D11VertexShader> shader;
    hr = device_->CreateVertexShader(
        bytecode->GetBufferPointer(), bytecode->GetBufferSize(),
        nullptr, &shader
    );

    if (FAILED(hr)) {
        Debug::Log("D3D11DeviceManager: CreateVertexShader failed");
        return nullptr;
    }

    if (out_bytecode) {
        *out_bytecode = bytecode;
    }

    return shader;
}

Microsoft::WRL::ComPtr<ID3D11PixelShader> D3D11DeviceManager::CompilePixelShader(
    const std::string& hlsl_source,
    const std::string& entry_point) {

    if (!device_) return nullptr;

    Microsoft::WRL::ComPtr<ID3DBlob> bytecode;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;

    HRESULT hr = D3DCompile(
        hlsl_source.c_str(), hlsl_source.length(),
        nullptr, nullptr, nullptr,
        entry_point.c_str(), "ps_5_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
        &bytecode, &errors
    );

    if (FAILED(hr)) {
        if (errors) {
            Debug::Log("D3D11DeviceManager: Pixel shader compilation failed: " +
                      std::string(static_cast<char*>(errors->GetBufferPointer())));
        }
        return nullptr;
    }

    Microsoft::WRL::ComPtr<ID3D11PixelShader> shader;
    hr = device_->CreatePixelShader(
        bytecode->GetBufferPointer(), bytecode->GetBufferSize(),
        nullptr, &shader
    );

    if (FAILED(hr)) {
        Debug::Log("D3D11DeviceManager: CreatePixelShader failed");
        return nullptr;
    }

    return shader;
}

Microsoft::WRL::ComPtr<ID3D11InputLayout> D3D11DeviceManager::CreateInputLayout(
    const D3D11_INPUT_ELEMENT_DESC* elements,
    UINT num_elements,
    ID3DBlob* vs_bytecode) {

    if (!device_ || !vs_bytecode) return nullptr;

    Microsoft::WRL::ComPtr<ID3D11InputLayout> layout;
    HRESULT hr = device_->CreateInputLayout(
        elements, num_elements,
        vs_bytecode->GetBufferPointer(), vs_bytecode->GetBufferSize(),
        &layout
    );

    if (FAILED(hr)) {
        Debug::Log("D3D11DeviceManager: CreateInputLayout failed");
        return nullptr;
    }

    return layout;
}

std::string D3D11DeviceManager::GetAdapterDescription() const {
    return adapter_description_;
}

} // namespace ump

#endif // _WIN32
