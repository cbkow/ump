#include "d3d11_dual_compositor.h"

#ifdef _WIN32

#include <d3dcompiler.h>
#include "../utils/debug_utils.h"

#pragma comment(lib, "d3dcompiler.lib")

namespace qcview {

//=============================================================================
// HLSL Shaders (embedded as strings)
//=============================================================================

static const char* g_compositor_vs_hlsl = R"(
struct VS_INPUT {
    float2 pos : POSITION;
    float2 uv : TEXCOORD0;
};

struct VS_OUTPUT {
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD0;
};

VS_OUTPUT VSMain(VS_INPUT input) {
    VS_OUTPUT output;
    output.pos = float4(input.pos, 0.0, 1.0);
    output.uv = input.uv;
    return output;
}
)";

static const char* g_compositor_ps_hlsl = R"(
//=============================================================================
// Dual View Compositor Pixel Shader
//
// Composites LEFT and RIGHT source textures into a single output.
// Supports horizontal (side-by-side) and vertical (top-bottom) layouts.
// Handles aspect-ratio-aware placement via UV region parameters.
//=============================================================================

cbuffer CompositeParams : register(b0) {
    // Layout control
    float horizontal;        // 1.0 = side-by-side, 0.0 = top-bottom
    float compositeWidth;    // Total composite texture width
    float compositeHeight;   // Total composite texture height
    float padding1;

    // LEFT source placement (normalized 0-1 in composite space)
    float4 leftRect;         // x, y, width, height

    // RIGHT source placement (normalized 0-1 in composite space)
    float4 rightRect;        // x, y, width, height

    // Flags
    float hasLeft;           // 1.0 if left source is available
    float hasRight;          // 1.0 if right source is available
    float2 padding2;
};

Texture2D texLeft : register(t0);
Texture2D texRight : register(t1);
SamplerState sampLinear : register(s0);

struct PS_INPUT {
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD0;
};

float4 PSMain(PS_INPUT input) : SV_TARGET {
    float2 uv = input.uv;
    float4 color = float4(0.0, 0.0, 0.0, 0.0);  // Transparent background

    if (horizontal > 0.5) {
        // Horizontal layout (side-by-side)
        // LEFT is on left side, RIGHT is on right side
        float leftEnd = leftRect.x + leftRect.z;  // left_x + left_width

        if (uv.x < leftEnd && hasLeft > 0.5) {
            // We're in the LEFT region - check if within actual source rect
            if (uv.x >= leftRect.x && uv.x <= leftRect.x + leftRect.z &&
                uv.y >= leftRect.y && uv.y <= leftRect.y + leftRect.w) {
                // Map UV to source texture coordinates
                float2 leftUV;
                leftUV.x = (uv.x - leftRect.x) / leftRect.z;
                leftUV.y = (uv.y - leftRect.y) / leftRect.w;
                color = texLeft.Sample(sampLinear, leftUV);
            }
        } else if (uv.x >= rightRect.x && hasRight > 0.5) {
            // We're in the RIGHT region
            if (uv.x >= rightRect.x && uv.x <= rightRect.x + rightRect.z &&
                uv.y >= rightRect.y && uv.y <= rightRect.y + rightRect.w) {
                // Map UV to source texture coordinates
                float2 rightUV;
                rightUV.x = (uv.x - rightRect.x) / rightRect.z;
                rightUV.y = (uv.y - rightRect.y) / rightRect.w;
                color = texRight.Sample(sampLinear, rightUV);
            }
        }
    } else {
        // Vertical layout (top-bottom)
        // LEFT is on top, RIGHT is on bottom
        float leftEnd = leftRect.y + leftRect.w;  // left_y + left_height

        if (uv.y < leftEnd && hasLeft > 0.5) {
            // We're in the LEFT (top) region
            if (uv.x >= leftRect.x && uv.x <= leftRect.x + leftRect.z &&
                uv.y >= leftRect.y && uv.y <= leftRect.y + leftRect.w) {
                float2 leftUV;
                leftUV.x = (uv.x - leftRect.x) / leftRect.z;
                leftUV.y = (uv.y - leftRect.y) / leftRect.w;
                color = texLeft.Sample(sampLinear, leftUV);
            }
        } else if (uv.y >= rightRect.y && hasRight > 0.5) {
            // We're in the RIGHT (bottom) region
            if (uv.x >= rightRect.x && uv.x <= rightRect.x + rightRect.z &&
                uv.y >= rightRect.y && uv.y <= rightRect.y + rightRect.w) {
                float2 rightUV;
                rightUV.x = (uv.x - rightRect.x) / rightRect.z;
                rightUV.y = (uv.y - rightRect.y) / rightRect.w;
                color = texRight.Sample(sampLinear, rightUV);
            }
        }
    }

    return color;
}
)";

//=============================================================================
// Fullscreen Quad Vertex Data
//=============================================================================

struct CompositorVertex {
    float x, y;   // Position (-1 to 1)
    float u, v;   // Texcoord (0 to 1)
};

static const CompositorVertex g_compositor_vertices[] = {
    { -1.0f,  1.0f, 0.0f, 0.0f },  // Top-left
    {  1.0f,  1.0f, 1.0f, 0.0f },  // Top-right
    { -1.0f, -1.0f, 0.0f, 1.0f },  // Bottom-left
    {  1.0f, -1.0f, 1.0f, 1.0f },  // Bottom-right
};

//=============================================================================
// Constant Buffer Layout
//=============================================================================

struct CompositeConstantBuffer {
    float horizontal;
    float compositeWidth;
    float compositeHeight;
    float padding1;

    float leftRect[4];   // x, y, width, height (normalized)
    float rightRect[4];  // x, y, width, height (normalized)

    float hasLeft;
    float hasRight;
    float padding2[2];
};

//=============================================================================
// Implementation
//=============================================================================

D3D11DualCompositor::D3D11DualCompositor() = default;

D3D11DualCompositor::~D3D11DualCompositor() {
    Shutdown();
}

bool D3D11DualCompositor::Initialize(ID3D11Device* device) {
    if (initialized_) {
        return true;
    }

    if (!device) {
        Debug::Log("D3D11DualCompositor: Device is null");
        return false;
    }

    device_ = device;
    device_->GetImmediateContext(&context_);

    if (!CreateShaders()) {
        Debug::Log("D3D11DualCompositor: Failed to create shaders");
        Shutdown();
        return false;
    }

    if (!CreateResources()) {
        Debug::Log("D3D11DualCompositor: Failed to create resources");
        Shutdown();
        return false;
    }

    // Create small transparent texture (1x1) for fallback
    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = 1;
    texDesc.Height = 1;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    float transparentPixel[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem = transparentPixel;
    initData.SysMemPitch = sizeof(transparentPixel);

    HRESULT hr = device_->CreateTexture2D(&texDesc, &initData, &transparent_texture_);
    if (FAILED(hr)) {
        Debug::Log("D3D11DualCompositor: Failed to create transparent texture");
        Shutdown();
        return false;
    }

    hr = device_->CreateShaderResourceView(transparent_texture_.Get(), nullptr, &transparent_srv_);
    if (FAILED(hr)) {
        Debug::Log("D3D11DualCompositor: Failed to create transparent SRV");
        Shutdown();
        return false;
    }

    initialized_ = true;
    Debug::Log("D3D11DualCompositor: Initialized successfully");
    return true;
}

void D3D11DualCompositor::Shutdown() {
    DestroyCompositeTarget();

    transparent_srv_.Reset();
    transparent_texture_.Reset();
    vertex_buffer_.Reset();
    constant_buffer_.Reset();
    sampler_.Reset();
    rasterizer_state_.Reset();
    blend_state_.Reset();
    input_layout_.Reset();
    pixel_shader_.Reset();
    vertex_shader_.Reset();
    context_.Reset();
    device_.Reset();

    initialized_ = false;
}

bool D3D11DualCompositor::CreateShaders() {
    HRESULT hr;
    Microsoft::WRL::ComPtr<ID3DBlob> vsBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> psBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> errorBlob;

    // Compile vertex shader
    hr = D3DCompile(g_compositor_vs_hlsl, strlen(g_compositor_vs_hlsl),
                    "CompositorVS", nullptr, nullptr,
                    "VSMain", "vs_5_0",
                    D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                    &vsBlob, &errorBlob);
    if (FAILED(hr)) {
        if (errorBlob) {
            Debug::Log("D3D11DualCompositor: VS compile error: " +
                       std::string((char*)errorBlob->GetBufferPointer()));
        }
        return false;
    }

    hr = device_->CreateVertexShader(vsBlob->GetBufferPointer(),
                                      vsBlob->GetBufferSize(),
                                      nullptr, &vertex_shader_);
    if (FAILED(hr)) {
        Debug::Log("D3D11DualCompositor: Failed to create vertex shader");
        return false;
    }

    // Compile pixel shader
    hr = D3DCompile(g_compositor_ps_hlsl, strlen(g_compositor_ps_hlsl),
                    "CompositorPS", nullptr, nullptr,
                    "PSMain", "ps_5_0",
                    D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                    &psBlob, &errorBlob);
    if (FAILED(hr)) {
        if (errorBlob) {
            Debug::Log("D3D11DualCompositor: PS compile error: " +
                       std::string((char*)errorBlob->GetBufferPointer()));
        }
        return false;
    }

    hr = device_->CreatePixelShader(psBlob->GetBufferPointer(),
                                     psBlob->GetBufferSize(),
                                     nullptr, &pixel_shader_);
    if (FAILED(hr)) {
        Debug::Log("D3D11DualCompositor: Failed to create pixel shader");
        return false;
    }

    // Create input layout
    D3D11_INPUT_ELEMENT_DESC layoutDesc[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 }
    };

    hr = device_->CreateInputLayout(layoutDesc, 2,
                                     vsBlob->GetBufferPointer(),
                                     vsBlob->GetBufferSize(),
                                     &input_layout_);
    if (FAILED(hr)) {
        Debug::Log("D3D11DualCompositor: Failed to create input layout");
        return false;
    }

    return true;
}

bool D3D11DualCompositor::CreateResources() {
    HRESULT hr;

    // Create vertex buffer
    D3D11_BUFFER_DESC vbDesc = {};
    vbDesc.ByteWidth = sizeof(g_compositor_vertices);
    vbDesc.Usage = D3D11_USAGE_IMMUTABLE;
    vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;

    D3D11_SUBRESOURCE_DATA vbData = {};
    vbData.pSysMem = g_compositor_vertices;

    hr = device_->CreateBuffer(&vbDesc, &vbData, &vertex_buffer_);
    if (FAILED(hr)) {
        Debug::Log("D3D11DualCompositor: Failed to create vertex buffer");
        return false;
    }

    // Create constant buffer
    D3D11_BUFFER_DESC cbDesc = {};
    cbDesc.ByteWidth = sizeof(CompositeConstantBuffer);
    cbDesc.Usage = D3D11_USAGE_DYNAMIC;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    hr = device_->CreateBuffer(&cbDesc, nullptr, &constant_buffer_);
    if (FAILED(hr)) {
        Debug::Log("D3D11DualCompositor: Failed to create constant buffer");
        return false;
    }

    // Create sampler state
    D3D11_SAMPLER_DESC sampDesc = {};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampDesc.MinLOD = 0;
    sampDesc.MaxLOD = D3D11_FLOAT32_MAX;

    hr = device_->CreateSamplerState(&sampDesc, &sampler_);
    if (FAILED(hr)) {
        Debug::Log("D3D11DualCompositor: Failed to create sampler state");
        return false;
    }

    // Create rasterizer state
    D3D11_RASTERIZER_DESC rastDesc = {};
    rastDesc.FillMode = D3D11_FILL_SOLID;
    rastDesc.CullMode = D3D11_CULL_NONE;
    rastDesc.FrontCounterClockwise = FALSE;
    rastDesc.DepthClipEnable = TRUE;

    hr = device_->CreateRasterizerState(&rastDesc, &rasterizer_state_);
    if (FAILED(hr)) {
        Debug::Log("D3D11DualCompositor: Failed to create rasterizer state");
        return false;
    }

    // Create blend state (for alpha compositing)
    D3D11_BLEND_DESC blendDesc = {};
    blendDesc.RenderTarget[0].BlendEnable = FALSE;
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

    hr = device_->CreateBlendState(&blendDesc, &blend_state_);
    if (FAILED(hr)) {
        Debug::Log("D3D11DualCompositor: Failed to create blend state");
        return false;
    }

    return true;
}

bool D3D11DualCompositor::CreateCompositeTarget(const DualViewLayout& layout, DXGI_FORMAT format) {
    if (!initialized_) {
        Debug::Log("D3D11DualCompositor: Not initialized");
        return false;
    }

    // Check if we need to recreate
    if (composite_texture_ &&
        composite_width_ == layout.composite_width &&
        composite_height_ == layout.composite_height &&
        composite_format_ == format) {
        return true;  // Already created with matching dimensions
    }

    DestroyCompositeTarget();

    if (layout.composite_width <= 0 || layout.composite_height <= 0) {
        Debug::Log("D3D11DualCompositor: Invalid layout dimensions");
        return false;
    }

    // Create composite texture
    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = layout.composite_width;
    texDesc.Height = layout.composite_height;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = format;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    HRESULT hr = device_->CreateTexture2D(&texDesc, nullptr, &composite_texture_);
    if (FAILED(hr)) {
        Debug::Log("D3D11DualCompositor: Failed to create composite texture");
        return false;
    }

    // Create RTV
    hr = device_->CreateRenderTargetView(composite_texture_.Get(), nullptr, &composite_rtv_);
    if (FAILED(hr)) {
        Debug::Log("D3D11DualCompositor: Failed to create composite RTV");
        DestroyCompositeTarget();
        return false;
    }

    // Create SRV
    hr = device_->CreateShaderResourceView(composite_texture_.Get(), nullptr, &composite_srv_);
    if (FAILED(hr)) {
        Debug::Log("D3D11DualCompositor: Failed to create composite SRV");
        DestroyCompositeTarget();
        return false;
    }

    composite_width_ = layout.composite_width;
    composite_height_ = layout.composite_height;
    composite_format_ = format;

    Debug::Log("D3D11DualCompositor: Created composite target " +
               std::to_string(composite_width_) + "x" + std::to_string(composite_height_));
    return true;
}

void D3D11DualCompositor::DestroyCompositeTarget() {
    composite_srv_.Reset();
    composite_rtv_.Reset();
    composite_texture_.Reset();
    composite_width_ = 0;
    composite_height_ = 0;
    composite_format_ = DXGI_FORMAT_UNKNOWN;
}

bool D3D11DualCompositor::Composite(ID3D11ShaderResourceView* left_srv,
                                     ID3D11ShaderResourceView* right_srv,
                                     const DualViewLayout& layout) {
    if (!composite_rtv_) {
        if (!CreateCompositeTarget(layout)) {
            return false;
        }
    }

    RenderQuad(left_srv, right_srv, composite_rtv_.Get(), layout);
    return true;
}

bool D3D11DualCompositor::CompositeToTarget(ID3D11ShaderResourceView* left_srv,
                                             ID3D11ShaderResourceView* right_srv,
                                             ID3D11RenderTargetView* target_rtv,
                                             const DualViewLayout& layout) {
    if (!target_rtv) {
        Debug::Log("D3D11DualCompositor: Target RTV is null");
        return false;
    }

    RenderQuad(left_srv, right_srv, target_rtv, layout);
    return true;
}

void D3D11DualCompositor::RenderQuad(ID3D11ShaderResourceView* left_srv,
                                      ID3D11ShaderResourceView* right_srv,
                                      ID3D11RenderTargetView* target_rtv,
                                      const DualViewLayout& layout) {
    if (!initialized_ || !context_) return;

    // Use transparent fallback for null sources
    ID3D11ShaderResourceView* left = left_srv ? left_srv : transparent_srv_.Get();
    ID3D11ShaderResourceView* right = right_srv ? right_srv : transparent_srv_.Get();

    // Update constant buffer
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = context_->Map(constant_buffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (SUCCEEDED(hr)) {
        CompositeConstantBuffer* cb = static_cast<CompositeConstantBuffer*>(mapped.pData);

        cb->horizontal = layout.horizontal ? 1.0f : 0.0f;
        cb->compositeWidth = static_cast<float>(layout.composite_width);
        cb->compositeHeight = static_cast<float>(layout.composite_height);
        cb->padding1 = 0.0f;

        // Normalize rect coordinates to 0-1 range
        float invW = 1.0f / static_cast<float>(layout.composite_width);
        float invH = 1.0f / static_cast<float>(layout.composite_height);

        cb->leftRect[0] = static_cast<float>(layout.left_x) * invW;
        cb->leftRect[1] = static_cast<float>(layout.left_y) * invH;
        cb->leftRect[2] = static_cast<float>(layout.left_w) * invW;
        cb->leftRect[3] = static_cast<float>(layout.left_h) * invH;

        cb->rightRect[0] = static_cast<float>(layout.right_x) * invW;
        cb->rightRect[1] = static_cast<float>(layout.right_y) * invH;
        cb->rightRect[2] = static_cast<float>(layout.right_w) * invW;
        cb->rightRect[3] = static_cast<float>(layout.right_h) * invH;

        cb->hasLeft = left_srv ? 1.0f : 0.0f;
        cb->hasRight = right_srv ? 1.0f : 0.0f;
        cb->padding2[0] = cb->padding2[1] = 0.0f;

        context_->Unmap(constant_buffer_.Get(), 0);
    }

    // Set viewport
    D3D11_VIEWPORT viewport = {};
    viewport.Width = static_cast<float>(layout.composite_width);
    viewport.Height = static_cast<float>(layout.composite_height);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    context_->RSSetViewports(1, &viewport);

    // Clear render target to transparent
    float clearColor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    context_->ClearRenderTargetView(target_rtv, clearColor);

    // Set render target
    context_->OMSetRenderTargets(1, &target_rtv, nullptr);

    // Set shaders
    context_->VSSetShader(vertex_shader_.Get(), nullptr, 0);
    context_->PSSetShader(pixel_shader_.Get(), nullptr, 0);
    context_->IASetInputLayout(input_layout_.Get());

    // Set vertex buffer
    UINT stride = sizeof(CompositorVertex);
    UINT offset = 0;
    ID3D11Buffer* vbs[] = { vertex_buffer_.Get() };
    context_->IASetVertexBuffers(0, 1, vbs, &stride, &offset);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

    // Set constant buffer
    ID3D11Buffer* cbs[] = { constant_buffer_.Get() };
    context_->PSSetConstantBuffers(0, 1, cbs);

    // Set textures
    ID3D11ShaderResourceView* srvs[] = { left, right };
    context_->PSSetShaderResources(0, 2, srvs);

    // Set sampler
    ID3D11SamplerState* samplers[] = { sampler_.Get() };
    context_->PSSetSamplers(0, 1, samplers);

    // Set states
    context_->RSSetState(rasterizer_state_.Get());
    context_->OMSetBlendState(blend_state_.Get(), nullptr, 0xFFFFFFFF);

    // Draw
    context_->Draw(4, 0);

    // Unbind resources
    ID3D11ShaderResourceView* nullSRVs[2] = { nullptr, nullptr };
    context_->PSSetShaderResources(0, 2, nullSRVs);
    ID3D11RenderTargetView* nullRTV = nullptr;
    context_->OMSetRenderTargets(1, &nullRTV, nullptr);
}

} // namespace qcview

#endif // _WIN32
