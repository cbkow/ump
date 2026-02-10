#pragma once

#ifdef _WIN32

#include <d3d11_1.h>
#include <dxgi1_6.h>
#include <dcomp.h>
#include <wrl/client.h>
#include <mutex>
#include <string>
#include <vector>
#include <array>

namespace ump {

//=============================================================================
// D3D11DeviceManager
//
// Centralized D3D11 device and context management for the entire application.
// Singleton pattern ensures all components share the same device.
//
// Usage:
//   auto& mgr = D3D11DeviceManager::Instance();
//   if (mgr.Initialize(hwnd)) {
//       auto* device = mgr.GetDevice();
//       auto* context = mgr.GetContext();
//       // Create resources...
//   }
//=============================================================================

class D3D11DeviceManager {
public:
    static D3D11DeviceManager& Instance();

    // Delete copy/move
    D3D11DeviceManager(const D3D11DeviceManager&) = delete;
    D3D11DeviceManager& operator=(const D3D11DeviceManager&) = delete;

    //=========================================================================
    // Initialization
    //=========================================================================

    // Initialize the D3D11 device. Can be called multiple times safely.
    // hwnd is used for adapter selection (finds monitor containing window)
    bool Initialize(HWND hwnd = nullptr);

    // Shutdown and release all resources
    void Shutdown();

    // Check if device is initialized and valid
    bool IsInitialized() const { return initialized_; }

    //=========================================================================
    // Device Access
    //=========================================================================

    ID3D11Device1* GetDevice() const { return device_.Get(); }
    ID3D11DeviceContext1* GetContext() const { return context_.Get(); }
    IDXGIDevice* GetDXGIDevice() const { return dxgi_device_.Get(); }
    IDXGIAdapter* GetAdapter() const { return adapter_.Get(); }
    IDXGIFactory2* GetFactory() const { return factory_.Get(); }

    // Raw device pointer for COM interop (doesn't AddRef)
    ID3D11Device* GetRawDevice() const { return device_.Get(); }

    //=========================================================================
    // Texture Creation Helpers
    //=========================================================================

    // Create a 2D texture with specified parameters
    Microsoft::WRL::ComPtr<ID3D11Texture2D> CreateTexture2D(
        UINT width, UINT height,
        DXGI_FORMAT format,
        D3D11_USAGE usage = D3D11_USAGE_DEFAULT,
        UINT bind_flags = D3D11_BIND_SHADER_RESOURCE,
        UINT cpu_access_flags = 0,
        UINT misc_flags = 0,
        const D3D11_SUBRESOURCE_DATA* initial_data = nullptr);

    // Create a staging texture for CPU read/write
    Microsoft::WRL::ComPtr<ID3D11Texture2D> CreateStagingTexture(
        UINT width, UINT height,
        DXGI_FORMAT format,
        bool cpu_read = true,
        bool cpu_write = true);

    // Create a shader resource view for a texture
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> CreateSRV(
        ID3D11Texture2D* texture,
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN);

    // Create a render target view for a texture
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> CreateRTV(
        ID3D11Texture2D* texture,
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN);

    // Create a 1D texture (for OCIO LUTs)
    Microsoft::WRL::ComPtr<ID3D11Texture1D> CreateTexture1D(
        UINT width,
        DXGI_FORMAT format,
        const void* data = nullptr);

    // Create a 3D texture (for OCIO 3D LUTs)
    Microsoft::WRL::ComPtr<ID3D11Texture3D> CreateTexture3D(
        UINT width, UINT height, UINT depth,
        DXGI_FORMAT format,
        const void* data = nullptr);

    // Create a sampler state
    Microsoft::WRL::ComPtr<ID3D11SamplerState> CreateSamplerState(
        D3D11_FILTER filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR,
        D3D11_TEXTURE_ADDRESS_MODE address_mode = D3D11_TEXTURE_ADDRESS_CLAMP);

    // Create a constant buffer
    Microsoft::WRL::ComPtr<ID3D11Buffer> CreateConstantBuffer(UINT size);

    // Create a vertex buffer
    Microsoft::WRL::ComPtr<ID3D11Buffer> CreateVertexBuffer(
        const void* data, UINT size, bool dynamic = false);

    // Create an index buffer
    Microsoft::WRL::ComPtr<ID3D11Buffer> CreateIndexBuffer(
        const void* data, UINT size);

    //=========================================================================
    // Texture Upload Helpers
    //=========================================================================

    // Upload pixel data to a texture via staging texture
    bool UploadTextureData(
        ID3D11Texture2D* dest_texture,
        const void* pixel_data,
        UINT width, UINT height,
        UINT row_pitch);

    // Update a dynamic texture directly
    bool UpdateDynamicTexture(
        ID3D11Texture2D* texture,
        const void* pixel_data,
        UINT row_pitch);

    //=========================================================================
    // Shader Compilation
    //=========================================================================

    // Compile HLSL vertex shader from source
    Microsoft::WRL::ComPtr<ID3D11VertexShader> CompileVertexShader(
        const std::string& hlsl_source,
        const std::string& entry_point = "VSMain",
        Microsoft::WRL::ComPtr<ID3DBlob>* out_bytecode = nullptr);

    // Compile HLSL pixel shader from source
    Microsoft::WRL::ComPtr<ID3D11PixelShader> CompilePixelShader(
        const std::string& hlsl_source,
        const std::string& entry_point = "PSMain");

    // Create input layout from vertex shader bytecode
    Microsoft::WRL::ComPtr<ID3D11InputLayout> CreateInputLayout(
        const D3D11_INPUT_ELEMENT_DESC* elements,
        UINT num_elements,
        ID3DBlob* vs_bytecode);

    //=========================================================================
    // Debug/Info
    //=========================================================================

    std::string GetAdapterDescription() const;
    D3D_FEATURE_LEVEL GetFeatureLevel() const { return feature_level_; }
    size_t GetDedicatedVideoMemory() const { return dedicated_video_memory_; }

private:
    D3D11DeviceManager();
    ~D3D11DeviceManager();

    bool CreateDevice();
    bool GetDXGIObjects();

    Microsoft::WRL::ComPtr<ID3D11Device1> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext1> context_;
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_device_;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter_;
    Microsoft::WRL::ComPtr<IDXGIFactory2> factory_;

    // Staging texture pool for uploads - triple-buffering to eliminate CPU blocking
    static constexpr int STAGING_BUFFER_COUNT = 3;

    struct StagingBuffer {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        UINT width = 0;
        UINT height = 0;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    };

    std::array<StagingBuffer, STAGING_BUFFER_COUNT> staging_buffers_;
    int current_staging_index_ = 0;

    D3D_FEATURE_LEVEL feature_level_ = D3D_FEATURE_LEVEL_11_0;
    size_t dedicated_video_memory_ = 0;
    std::string adapter_description_;
    bool initialized_ = false;
    HWND hwnd_ = nullptr;

    mutable std::mutex mutex_;
};

} // namespace ump

#endif // _WIN32
