#pragma once

#ifdef _WIN32

#include <d3d11_1.h>
#include <wrl/client.h>
#include "dual_view_layout.h"

namespace ump {

//=============================================================================
// D3D11DualCompositor
//
// Composites two D3D11 textures (LEFT and RIGHT video frames) into a single
// texture for efficient D3D11->GL interop. This eliminates the need for
// two competing interops, solving the buffer exhaustion issue.
//
// The compositor renders both sources side-by-side (or top-bottom for tall
// sources) into a single render target. The OpenGL player then uses UV
// coordinates to sample the appropriate half for each viewport.
//
// HLSL shader handles:
// - Horizontal (side-by-side) and vertical (top-bottom) layouts
// - Aspect-ratio-preserving letterbox/pillarbox
// - Transparent output for missing sources (source < duration)
// - RGBA16F output for HDR content preservation
//=============================================================================

class D3D11DualCompositor {
public:
    D3D11DualCompositor();
    ~D3D11DualCompositor();

    // Non-copyable
    D3D11DualCompositor(const D3D11DualCompositor&) = delete;
    D3D11DualCompositor& operator=(const D3D11DualCompositor&) = delete;

    //=========================================================================
    // Initialization
    //=========================================================================

    bool Initialize(ID3D11Device* device);
    void Shutdown();
    bool IsInitialized() const { return initialized_; }

    //=========================================================================
    // Composite Target Management
    //=========================================================================

    // Create composite render target based on layout dimensions
    // format: DXGI_FORMAT (RGBA16F for HDR, RGBA8 for SDR)
    bool CreateCompositeTarget(const DualViewLayout& layout,
                                DXGI_FORMAT format = DXGI_FORMAT_R16G16B16A16_FLOAT);
    void DestroyCompositeTarget();

    // Get current target dimensions
    int GetWidth() const { return composite_width_; }
    int GetHeight() const { return composite_height_; }

    //=========================================================================
    // Compositing
    //=========================================================================

    // Composite two source textures into the render target
    // left_srv, right_srv: Source shader resource views (can be null for gaps)
    // layout: Describes where each source is placed in the composite
    // Returns true on success
    bool Composite(ID3D11ShaderResourceView* left_srv,
                   ID3D11ShaderResourceView* right_srv,
                   const DualViewLayout& layout);

    // Composite to an external render target (for interop integration)
    bool CompositeToTarget(ID3D11ShaderResourceView* left_srv,
                            ID3D11ShaderResourceView* right_srv,
                            ID3D11RenderTargetView* target_rtv,
                            const DualViewLayout& layout);

    //=========================================================================
    // Result Access
    //=========================================================================

    // Get the composited texture
    ID3D11Texture2D* GetCompositeTexture() const { return composite_texture_.Get(); }

    // Get shader resource view for sampling the composite
    ID3D11ShaderResourceView* GetCompositeSRV() const { return composite_srv_.Get(); }

    // Get render target view (for direct rendering)
    ID3D11RenderTargetView* GetCompositeRTV() const { return composite_rtv_.Get(); }

private:
    //=========================================================================
    // Shader Creation
    //=========================================================================

    bool CreateShaders();
    bool CreateResources();

    //=========================================================================
    // Internal Rendering
    //=========================================================================

    void RenderQuad(ID3D11ShaderResourceView* left_srv,
                    ID3D11ShaderResourceView* right_srv,
                    ID3D11RenderTargetView* target_rtv,
                    const DualViewLayout& layout);

    //=========================================================================
    // D3D11 Resources
    //=========================================================================

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;

    // Composite texture and views
    Microsoft::WRL::ComPtr<ID3D11Texture2D> composite_texture_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> composite_rtv_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> composite_srv_;

    // Transparent fallback texture (for missing sources)
    Microsoft::WRL::ComPtr<ID3D11Texture2D> transparent_texture_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> transparent_srv_;

    // Shaders
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertex_shader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> pixel_shader_;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> input_layout_;

    // Resources
    Microsoft::WRL::ComPtr<ID3D11Buffer> vertex_buffer_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> constant_buffer_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;

    // Render states
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizer_state_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> blend_state_;

    // State
    int composite_width_ = 0;
    int composite_height_ = 0;
    DXGI_FORMAT composite_format_ = DXGI_FORMAT_UNKNOWN;
    bool initialized_ = false;
};

} // namespace ump

#endif // _WIN32
