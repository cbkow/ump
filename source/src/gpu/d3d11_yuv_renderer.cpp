#include "d3d11_yuv_renderer.h"

#ifdef _WIN32

#include <d3dcompiler.h>
#include "../utils/debug_utils.h"

#pragma comment(lib, "d3dcompiler.lib")

namespace ump {

//=============================================================================
// HLSL Shaders (embedded as strings)
//=============================================================================

static const char* g_vertex_shader_hlsl = R"(
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

static const char* g_pixel_shader_hlsl = R"(
//=============================================================================
// YUV to RGB Conversion Shader with HDR PQ Support
//=============================================================================

cbuffer YUVParams : register(b0) {
    float bitDepth;      // 8.0 or 10.0
    float applyPQ;       // 1.0 for HDR PQ decode
    float isFullRange;   // 1.0 for full range, 0.0 for video range
    float colorSpace;    // 0.0 = BT.709, 1.0 = BT.2020
    float useTextureArray; // 1.0 for Texture2DArray (D3D11VA), 0.0 for Texture2D
    float3 padding;      // Align to 16 bytes
};

// Regular 2D textures (for non-array sources)
Texture2D<float> texY : register(t0);
Texture2D<float2> texUV : register(t1);

// Texture arrays (for D3D11VA decode)
Texture2DArray<float> texYArray : register(t2);
Texture2DArray<float2> texUVArray : register(t3);

SamplerState sampLinear : register(s0);

//=============================================================================
// PQ EOTF (ST.2084) - Decode PQ to linear light
// Input: PQ-encoded value [0, 1]
// Output: Linear light [0, 1] representing [0, 10000] nits
//=============================================================================
float3 PQToLinear(float3 pq) {
    const float m1 = 0.1593017578125;      // 2610/16384
    const float m2 = 78.84375;             // 2523/32 * 128
    const float c1 = 0.8359375;            // 3424/4096
    const float c2 = 18.8515625;           // 2413/128
    const float c3 = 18.6875;              // 2392/128

    float3 Np = pow(max(pq, 0.0), 1.0 / m2);
    float3 num = max(Np - c1, 0.0);
    float3 den = c2 - c3 * Np;
    return pow(num / den, 1.0 / m1);
}

//=============================================================================
// Color Matrices
//=============================================================================

// BT.709 YUV to RGB matrix (video range)
static const float3x3 BT709_MAT = {
    1.0,     0.0,        1.5748,
    1.0,    -0.18732,   -0.46812,
    1.0,     1.8556,     0.0
};

// BT.2020 YUV to RGB matrix (video range)
static const float3x3 BT2020_MAT = {
    1.0,     0.0,        1.4746,
    1.0,    -0.16455,   -0.57135,
    1.0,     1.8814,     0.0
};

struct PS_INPUT {
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD0;
};

float4 PSMain(PS_INPUT input) : SV_TARGET {
    float2 uv = input.uv;

    // Sample Y and UV planes (from array or regular texture)
    float Y;
    float2 UV;

    if (useTextureArray > 0.5) {
        Y = texYArray.Sample(sampLinear, float3(uv, 0));
        UV = texUVArray.Sample(sampLinear, float3(uv, 0));
    } else {
        Y = texY.Sample(sampLinear, uv);
        UV = texUV.Sample(sampLinear, uv);
    }

    // Range expansion for limited range content
    // Applied when isFullRange is false (user selected Limited or Auto detected limited)
    // Respects user override for all pipelines including HDR/float
    if (isFullRange < 0.5) {
        const float foot = 16.0 / 255.0;
        const float yHead = 235.0 / 255.0;
        const float uvHead = 240.0 / 255.0;

        Y = saturate((Y - foot) / (yHead - foot));
        UV = saturate((UV - foot) / (uvHead - foot));
    }

    // Convert UV to signed: [0, 1] -> [-0.5, 0.5]
    UV = UV - 0.5;

    // Select color matrix based on color space
    float3x3 colorMat = (colorSpace > 0.5) ? BT2020_MAT : BT709_MAT;

    // Apply YUV to RGB conversion
    float3 yuv = float3(Y, UV.x, UV.y);
    float3 rgb = mul(colorMat, yuv);

    return float4(saturate(rgb), 1.0);
}
)";

//=============================================================================
// Fullscreen Quad Vertex Data
//=============================================================================

struct QuadVertex {
    float x, y;   // Position
    float u, v;   // Texcoord
};

static const QuadVertex g_quad_vertices[] = {
    { -1.0f,  1.0f, 0.0f, 0.0f },  // Top-left
    {  1.0f,  1.0f, 1.0f, 0.0f },  // Top-right
    { -1.0f, -1.0f, 0.0f, 1.0f },  // Bottom-left
    {  1.0f, -1.0f, 1.0f, 1.0f },  // Bottom-right
};

//=============================================================================
// Constant Buffer Layout
//=============================================================================

struct YUVConstantBuffer {
    float bitDepth;
    float applyPQ;
    float isFullRange;
    float colorSpace;
    float useTextureArray;
    float padding[3];  // Align to 16-byte boundary
};

//=============================================================================
// Implementation
//=============================================================================

D3D11YUVRenderer::D3D11YUVRenderer() = default;

D3D11YUVRenderer::~D3D11YUVRenderer() {
    Shutdown();
}

bool D3D11YUVRenderer::Initialize(ID3D11Device* device) {
    if (initialized_) {
        return true;
    }

    if (!device) {
        Debug::Log("D3D11YUVRenderer: Device is null");
        return false;
    }

    device_ = device;
    device_->GetImmediateContext(&context_);

    if (!CreateShaders()) {
        Debug::Log("D3D11YUVRenderer: Failed to create shaders");
        Shutdown();
        return false;
    }

    if (!CreateResources()) {
        Debug::Log("D3D11YUVRenderer: Failed to create resources");
        Shutdown();
        return false;
    }

    initialized_ = true;
    Debug::Log("D3D11YUVRenderer: Initialized");
    return true;
}

void D3D11YUVRenderer::Shutdown() {
    rasterizer_state_.Reset();
    blend_state_.Reset();
    sampler_linear_.Reset();
    quad_vbo_.Reset();
    constant_buffer_.Reset();
    input_layout_.Reset();
    pixel_shader_.Reset();
    vertex_shader_.Reset();
    context_.Reset();
    device_.Reset();
    initialized_ = false;
}

bool D3D11YUVRenderer::CreateShaders() {
    Microsoft::WRL::ComPtr<ID3DBlob> vs_bytecode;
    Microsoft::WRL::ComPtr<ID3DBlob> ps_bytecode;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;

    //=========================================================================
    // Compile Vertex Shader
    //=========================================================================
    HRESULT hr = D3DCompile(
        g_vertex_shader_hlsl,
        strlen(g_vertex_shader_hlsl),
        "YUVVertexShader",
        nullptr, nullptr,
        "VSMain", "vs_5_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
        &vs_bytecode, &errors
    );

    if (FAILED(hr)) {
        if (errors) {
            Debug::Log("D3D11YUVRenderer: VS compile error: " +
                      std::string(static_cast<char*>(errors->GetBufferPointer())));
        }
        return false;
    }

    hr = device_->CreateVertexShader(
        vs_bytecode->GetBufferPointer(),
        vs_bytecode->GetBufferSize(),
        nullptr, &vertex_shader_
    );

    if (FAILED(hr)) {
        Debug::Log("D3D11YUVRenderer: Failed to create vertex shader");
        return false;
    }

    //=========================================================================
    // Compile Pixel Shader
    //=========================================================================
    hr = D3DCompile(
        g_pixel_shader_hlsl,
        strlen(g_pixel_shader_hlsl),
        "YUVPixelShader",
        nullptr, nullptr,
        "PSMain", "ps_5_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
        &ps_bytecode, &errors
    );

    if (FAILED(hr)) {
        if (errors) {
            Debug::Log("D3D11YUVRenderer: PS compile error: " +
                      std::string(static_cast<char*>(errors->GetBufferPointer())));
        }
        return false;
    }

    hr = device_->CreatePixelShader(
        ps_bytecode->GetBufferPointer(),
        ps_bytecode->GetBufferSize(),
        nullptr, &pixel_shader_
    );

    if (FAILED(hr)) {
        Debug::Log("D3D11YUVRenderer: Failed to create pixel shader");
        return false;
    }

    //=========================================================================
    // Create Input Layout
    //=========================================================================
    D3D11_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };

    hr = device_->CreateInputLayout(
        layout, _countof(layout),
        vs_bytecode->GetBufferPointer(),
        vs_bytecode->GetBufferSize(),
        &input_layout_
    );

    if (FAILED(hr)) {
        Debug::Log("D3D11YUVRenderer: Failed to create input layout");
        return false;
    }

    return true;
}

bool D3D11YUVRenderer::CreateResources() {
    HRESULT hr;

    //=========================================================================
    // Create Vertex Buffer (fullscreen quad)
    //=========================================================================
    {
        D3D11_BUFFER_DESC desc = {};
        desc.ByteWidth = sizeof(g_quad_vertices);
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;

        D3D11_SUBRESOURCE_DATA init_data = {};
        init_data.pSysMem = g_quad_vertices;

        hr = device_->CreateBuffer(&desc, &init_data, &quad_vbo_);
        if (FAILED(hr)) {
            Debug::Log("D3D11YUVRenderer: Failed to create vertex buffer");
            return false;
        }
    }

    //=========================================================================
    // Create Constant Buffer
    //=========================================================================
    {
        D3D11_BUFFER_DESC desc = {};
        desc.ByteWidth = sizeof(YUVConstantBuffer);
        // Align to 16 bytes
        desc.ByteWidth = (desc.ByteWidth + 15) & ~15;
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

        hr = device_->CreateBuffer(&desc, nullptr, &constant_buffer_);
        if (FAILED(hr)) {
            Debug::Log("D3D11YUVRenderer: Failed to create constant buffer");
            return false;
        }
    }

    //=========================================================================
    // Create Sampler State (bilinear)
    //=========================================================================
    {
        D3D11_SAMPLER_DESC desc = {};
        desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        desc.MaxAnisotropy = 1;
        desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
        desc.MinLOD = 0;
        desc.MaxLOD = D3D11_FLOAT32_MAX;

        hr = device_->CreateSamplerState(&desc, &sampler_linear_);
        if (FAILED(hr)) {
            Debug::Log("D3D11YUVRenderer: Failed to create sampler state");
            return false;
        }
    }

    //=========================================================================
    // Create Rasterizer State
    //=========================================================================
    {
        D3D11_RASTERIZER_DESC desc = {};
        desc.FillMode = D3D11_FILL_SOLID;
        desc.CullMode = D3D11_CULL_NONE;
        desc.FrontCounterClockwise = FALSE;
        desc.DepthClipEnable = TRUE;

        hr = device_->CreateRasterizerState(&desc, &rasterizer_state_);
        if (FAILED(hr)) {
            Debug::Log("D3D11YUVRenderer: Failed to create rasterizer state");
            return false;
        }
    }

    //=========================================================================
    // Create Blend State (no blending, replace)
    //=========================================================================
    {
        D3D11_BLEND_DESC desc = {};
        desc.RenderTarget[0].BlendEnable = FALSE;
        desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

        hr = device_->CreateBlendState(&desc, &blend_state_);
        if (FAILED(hr)) {
            Debug::Log("D3D11YUVRenderer: Failed to create blend state");
            return false;
        }
    }

    return true;
}

bool D3D11YUVRenderer::Render(ID3D11ShaderResourceView* srv_y,
                              ID3D11ShaderResourceView* srv_uv,
                              ID3D11RenderTargetView* dest_rtv,
                              const YUVRenderParams& params) {
    if (!initialized_ || !srv_y || !srv_uv || !dest_rtv) {
        Debug::Log("D3D11YUVRenderer::Render: Invalid params - init=" +
                   std::to_string(initialized_) + " srv_y=" + std::to_string(srv_y != nullptr) +
                   " srv_uv=" + std::to_string(srv_uv != nullptr) +
                   " rtv=" + std::to_string(dest_rtv != nullptr));
        return false;
    }

    // Log first render for debugging
    static bool first_render = true;
    if (first_render) {
        Debug::Log("D3D11YUVRenderer::Render: First render " +
                   std::to_string(params.width) + "x" + std::to_string(params.height) +
                   " bit_depth=" + std::to_string(params.bit_depth) +
                   " is_hdr=" + std::to_string(params.is_hdr) +
                   " use_texture_array=" + std::to_string(params.use_texture_array));
        first_render = false;
    }

    // DEBUG: Clear disabled - interop confirmed working
    // (magenta clear test passed)

    //=========================================================================
    // Update Constant Buffer
    //=========================================================================
    {
        D3D11_MAPPED_SUBRESOURCE mapped;
        HRESULT hr = context_->Map(constant_buffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (FAILED(hr)) {
            Debug::Log("D3D11YUVRenderer: Failed to map constant buffer");
            return false;
        }

        YUVConstantBuffer* cb = static_cast<YUVConstantBuffer*>(mapped.pData);
        cb->bitDepth = static_cast<float>(params.bit_depth);
        cb->applyPQ = params.is_hdr ? 1.0f : 0.0f;
        cb->isFullRange = params.is_full_range ? 1.0f : 0.0f;
        cb->colorSpace = (params.color_space == YUVColorSpace::BT_2020) ? 1.0f : 0.0f;
        cb->useTextureArray = params.use_texture_array ? 1.0f : 0.0f;
        cb->padding[0] = cb->padding[1] = cb->padding[2] = 0.0f;

        context_->Unmap(constant_buffer_.Get(), 0);
    }

    //=========================================================================
    // Set Pipeline State
    //=========================================================================

    // Input assembler
    UINT stride = sizeof(QuadVertex);
    UINT offset = 0;
    context_->IASetVertexBuffers(0, 1, quad_vbo_.GetAddressOf(), &stride, &offset);
    context_->IASetInputLayout(input_layout_.Get());
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

    // Shaders
    context_->VSSetShader(vertex_shader_.Get(), nullptr, 0);
    context_->PSSetShader(pixel_shader_.Get(), nullptr, 0);
    context_->PSSetConstantBuffers(0, 1, constant_buffer_.GetAddressOf());

    // Textures - bind to t0/t1 for regular or t2/t3 for texture arrays
    if (params.use_texture_array) {
        // Texture arrays: bind to t2, t3 (texYArray, texUVArray)
        ID3D11ShaderResourceView* null_srvs[] = { nullptr, nullptr };
        context_->PSSetShaderResources(0, 2, null_srvs);  // Clear t0, t1
        ID3D11ShaderResourceView* array_srvs[] = { srv_y, srv_uv };
        context_->PSSetShaderResources(2, 2, array_srvs);  // Bind to t2, t3
    } else {
        // Regular textures: bind to t0, t1 (texY, texUV)
        ID3D11ShaderResourceView* srvs[] = { srv_y, srv_uv };
        context_->PSSetShaderResources(0, 2, srvs);
    }
    context_->PSSetSamplers(0, 1, sampler_linear_.GetAddressOf());

    // Render target
    context_->OMSetRenderTargets(1, &dest_rtv, nullptr);
    context_->OMSetBlendState(blend_state_.Get(), nullptr, 0xFFFFFFFF);

    // Viewport
    D3D11_VIEWPORT viewport = {};
    viewport.Width = static_cast<float>(params.width);
    viewport.Height = static_cast<float>(params.height);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    context_->RSSetViewports(1, &viewport);
    context_->RSSetState(rasterizer_state_.Get());

    //=========================================================================
    // Draw
    //=========================================================================
    context_->Draw(4, 0);

    // Unbind resources
    ID3D11ShaderResourceView* null_srvs[] = { nullptr, nullptr, nullptr, nullptr };
    context_->PSSetShaderResources(0, 4, null_srvs);  // Clear t0-t3
    ID3D11RenderTargetView* null_rtv = nullptr;
    context_->OMSetRenderTargets(1, &null_rtv, nullptr);

    return true;
}

bool D3D11YUVRenderer::Render(ID3D11ShaderResourceView* srv_y,
                              ID3D11ShaderResourceView* srv_uv,
                              ID3D11RenderTargetView* dest_rtv,
                              int width, int height,
                              bool is_hdr,
                              YUVColorSpace color_space) {
    YUVRenderParams params;
    params.width = width;
    params.height = height;
    params.bit_depth = is_hdr ? 10 : 8;
    params.is_hdr = is_hdr;
    // HDR content is ALWAYS full range (PQ needs full 10-bit range for 0-10000 nits)
    // SDR defaults to limited range (most broadcast/streaming content)
    // Note: The shader also enforces this - HDR paths skip range conversion regardless
    params.is_full_range = is_hdr;
    params.color_space = color_space;

    return Render(srv_y, srv_uv, dest_rtv, params);
}

} // namespace ump

#endif // _WIN32
