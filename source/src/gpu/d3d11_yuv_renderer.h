#pragma once

#ifdef _WIN32

#include <d3d11_1.h>
#include <wrl/client.h>

namespace ump {

//=============================================================================
// YUV Color Space
//
// Specifies the color matrix for YUV to RGB conversion.
//=============================================================================

enum class YUVColorSpace {
    BT_709,     // HD (sRGB primaries)
    BT_2020     // UHD/HDR (wide gamut)
};

//=============================================================================
// YUV Render Params
//
// Parameters for YUV to RGB conversion shader.
//=============================================================================

struct YUVRenderParams {
    int width = 0;
    int height = 0;
    int bit_depth = 8;           // 8 for NV12, 10 for P010
    bool is_hdr = false;         // Apply PQ EOTF decode
    bool is_full_range = false;  // Video vs full range
    bool use_texture_array = false; // True for D3D11VA (texture arrays)
    YUVColorSpace color_space = YUVColorSpace::BT_709;
};

//=============================================================================
// D3D11YUVRenderer
//
// Renders NV12/P010 YUV textures to RGB via HLSL shaders.
// Supports:
// - BT.709 (HD) and BT.2020 (UHD) color matrices
// - PQ/ST.2084 EOTF decode for HDR content
// - Video range (16-235/64-940) and full range (0-255/0-1023)
// - 8-bit (NV12) and 10-bit (P010) formats
//
// Output is RGBA16F linear light, suitable for OCIO processing.
//=============================================================================

class D3D11YUVRenderer {
public:
    D3D11YUVRenderer();
    ~D3D11YUVRenderer();

    // Delete copy/move
    D3D11YUVRenderer(const D3D11YUVRenderer&) = delete;
    D3D11YUVRenderer& operator=(const D3D11YUVRenderer&) = delete;

    //=========================================================================
    // Initialization
    //=========================================================================

    bool Initialize(ID3D11Device* device);
    void Shutdown();
    bool IsInitialized() const { return initialized_; }

    //=========================================================================
    // Rendering
    //=========================================================================

    // Render YUV texture to RGB render target
    // srv_y: Y plane (luma)
    // srv_uv: UV plane (chroma, half resolution)
    // dest_rtv: Destination render target (should be RGBA16F for HDR)
    // params: Conversion parameters
    bool Render(ID3D11ShaderResourceView* srv_y,
                ID3D11ShaderResourceView* srv_uv,
                ID3D11RenderTargetView* dest_rtv,
                const YUVRenderParams& params);

    // Convenience overload with explicit dimensions
    bool Render(ID3D11ShaderResourceView* srv_y,
                ID3D11ShaderResourceView* srv_uv,
                ID3D11RenderTargetView* dest_rtv,
                int width, int height,
                bool is_hdr,
                YUVColorSpace color_space = YUVColorSpace::BT_2020);

private:
    //=========================================================================
    // Shader Compilation
    //=========================================================================

    bool CreateShaders();
    bool CreateResources();

    //=========================================================================
    // D3D11 Resources
    //=========================================================================

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;

    // Shaders
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertex_shader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> pixel_shader_;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> input_layout_;

    // Constant buffer for shader parameters
    Microsoft::WRL::ComPtr<ID3D11Buffer> constant_buffer_;

    // Full-screen quad
    Microsoft::WRL::ComPtr<ID3D11Buffer> quad_vbo_;

    // Sampler state
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_linear_;

    // Rasterizer and blend states
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizer_state_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> blend_state_;

    bool initialized_ = false;
};

} // namespace ump

#endif // _WIN32
