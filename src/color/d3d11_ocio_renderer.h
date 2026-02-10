#pragma once

#ifdef _WIN32

#include <d3d11_1.h>
#include <wrl/client.h>
#include <vector>
#include <string>

// Forward declaration - OCIOPipeline is in global namespace
class OCIOPipeline;

namespace ump {

//=============================================================================
// D3D11OCIORenderer
//
// Renders OCIO color transforms using D3D11.
// Draws a fullscreen quad with the OCIO HLSL shader.
//
// Usage:
//   D3D11OCIORenderer renderer;
//   renderer.Initialize(device);
//   renderer.Apply(pipeline, source_srv, dest_rtv, width, height);
//=============================================================================

class D3D11OCIORenderer {
public:
    D3D11OCIORenderer();
    ~D3D11OCIORenderer();

    // Delete copy/move
    D3D11OCIORenderer(const D3D11OCIORenderer&) = delete;
    D3D11OCIORenderer& operator=(const D3D11OCIORenderer&) = delete;

    //=========================================================================
    // Initialization
    //=========================================================================

    // Initialize renderer resources (quad geometry, samplers, etc.)
    bool Initialize();

    // Shutdown and release all resources
    void Shutdown();

    bool IsInitialized() const { return initialized_; }

    //=========================================================================
    // Rendering
    //=========================================================================

    // Apply OCIO color transform from source to destination
    // pipeline: The OCIOPipeline with compiled D3D11 shaders
    // source: Input texture SRV
    // dest: Output render target
    // width, height: Render dimensions
    bool Apply(
        ::OCIOPipeline* pipeline,
        ID3D11ShaderResourceView* source,
        ID3D11RenderTargetView* dest,
        int width, int height);

    // Apply passthrough (copy source to dest without color transform)
    bool ApplyPassthrough(
        ID3D11ShaderResourceView* source,
        ID3D11RenderTargetView* dest,
        int width, int height);

    //=========================================================================
    // State Management
    //=========================================================================

    // Save current D3D11 state before rendering
    void SaveState();

    // Restore D3D11 state after rendering
    void RestoreState();

private:
    bool CreateQuadGeometry();
    bool CreatePassthroughShader();
    bool CreateSamplers();
    bool CreateRasterizerState();
    bool CreateBlendState();

    // Setup common render state for fullscreen quad
    void SetupRenderState(ID3D11RenderTargetView* rtv, int width, int height);

    // Vertex format for fullscreen quad
    struct QuadVertex {
        float x, y;      // Position
        float u, v;      // Texture coordinates
    };

    // D3D11 resources
    Microsoft::WRL::ComPtr<ID3D11Buffer> quad_vertex_buffer_;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> input_layout_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> passthrough_vs_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> passthrough_ps_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> linear_sampler_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> point_sampler_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizer_state_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> blend_state_;

    // Saved state for restore
    struct SavedState {
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
        Microsoft::WRL::ComPtr<ID3D11DepthStencilView> dsv;
        D3D11_VIEWPORT viewport;
        UINT num_viewports;
        Microsoft::WRL::ComPtr<ID3D11VertexShader> vs;
        Microsoft::WRL::ComPtr<ID3D11PixelShader> ps;
        Microsoft::WRL::ComPtr<ID3D11InputLayout> layout;
        D3D11_PRIMITIVE_TOPOLOGY topology;
        Microsoft::WRL::ComPtr<ID3D11Buffer> vb;
        UINT vb_stride;
        UINT vb_offset;
        Microsoft::WRL::ComPtr<ID3D11RasterizerState> rs;
        Microsoft::WRL::ComPtr<ID3D11BlendState> blend;
        float blend_factor[4];
        UINT sample_mask;
    };
    SavedState saved_state_;
    bool state_saved_ = false;

    bool initialized_ = false;
};

//=============================================================================
// HLSL Shader Sources
//=============================================================================

// Vertex shader for fullscreen quad
const char* const OCIO_QUAD_VS_HLSL = R"(
struct VSInput {
    float2 Position : POSITION;
    float2 TexCoord : TEXCOORD0;
};

struct VSOutput {
    float4 Position : SV_POSITION;
    float2 TexCoord : TEXCOORD0;
};

VSOutput VSMain(VSInput input) {
    VSOutput output;
    output.Position = float4(input.Position, 0.0, 1.0);
    output.TexCoord = input.TexCoord;
    return output;
}
)";

// Passthrough pixel shader (identity transform)
const char* const OCIO_PASSTHROUGH_PS_HLSL = R"(
Texture2D videoTexture : register(t0);
SamplerState linearSampler : register(s0);

struct PSInput {
    float4 Position : SV_POSITION;
    float2 TexCoord : TEXCOORD0;
};

float4 PSMain(PSInput input) : SV_TARGET {
    return videoTexture.Sample(linearSampler, input.TexCoord);
}
)";

} // namespace ump

#endif // _WIN32
