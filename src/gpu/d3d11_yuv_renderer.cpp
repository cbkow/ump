#include "d3d11_yuv_renderer.h"

#ifdef _WIN32

#include <d3dcompiler.h>
#include "../utils/debug_utils.h"

#pragma comment(lib, "d3dcompiler.lib")

namespace qcview {

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
// YUV to RGB Conversion Shader - Extended for planar YUV and alpha support
//
// Supports:
// - 2-plane NV12/P010 (interleaved UV)
// - 3-plane planar YUV (YUV420P, YUV422P, YUV444P and 10/12-bit variants)
// - 3-plane GBRP RGB planar (8/10/12-bit)
// - 4-plane YUVA with alpha (8/10/12-bit)
// - 4-plane GBRAP RGB planar with alpha (8/10/12-bit)
// - D3D11VA texture arrays
// - BT.709 and BT.2020 color matrices
// - Video range (16-235) and full range (0-255)
//=============================================================================

cbuffer YUVParams : register(b0) {
    float bitDepth;        // 8.0, 10.0, or 12.0
    float applyPQ;         // 1.0 for HDR PQ decode (unused, kept for compat)
    float isFullRange;     // 1.0 = full range, 0.0 = video range (16-235)
    float colorSpace;      // 0.0 = BT.709, 1.0 = BT.2020
    float useTextureArray; // 1.0 = D3D11VA texture arrays
    float planeCount;      // 2.0 = NV12/P010, 3.0 = planar YUV, 4.0 = YUVA/GBRAP
    float hasAlpha;        // 1.0 = has alpha plane
    float isRGBPlanar;     // 1.0 = GBRP/GBRAP (plane order: G, B, R, [A])
    float bitDepthScale;   // Normalization factor: 1.0 for 8-bit, 65535/1023 for 10-bit, 65535/4095 for 12-bit
    float3 padding;        // Align to 16 bytes (48 bytes total)
};

// Textures - support up to 4 planes (each needs unique register)
// t0: Y plane (or G for GBRP)
// t1: UV interleaved (NV12/P010)
// t2: U plane (planar only, or B for GBRP)
// t3: V plane (planar only, or R for GBRP)
// t4: Y array (D3D11VA)
// t5: UV array (D3D11VA)
// t6: A plane (alpha for YUVA/GBRAP)
Texture2D<float> texPlane0 : register(t0);       // Y or G
Texture2D<float2> texPlane1_NV12 : register(t1); // UV interleaved (NV12)
Texture2D<float> texPlane1_U : register(t2);     // U or B (planar)
Texture2D<float> texPlane2_V : register(t3);     // V or R (planar)
Texture2D<float> texPlane3_A : register(t6);     // Alpha plane (YUVA/GBRAP)

// Texture arrays for D3D11VA
Texture2DArray<float> texYArray : register(t4);
Texture2DArray<float2> texUVArray : register(t5);

SamplerState sampLinear : register(s0);

//=============================================================================
// Color Matrices - YUV to RGB
//=============================================================================

// BT.709 YUV to RGB matrix
static const float3x3 BT709_MAT = {
    1.0,     0.0,        1.5748,
    1.0,    -0.18732,   -0.46812,
    1.0,     1.8556,     0.0
};

// BT.2020 YUV to RGB matrix
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
    float3 rgb;
    float A = 1.0;

    if (isRGBPlanar > 0.5) {
        // GBRP/GBRAP path: planes are G, B, R order
        // Always apply bitDepthScale for software-decoded RGB planar
        float G = texPlane0.Sample(sampLinear, uv) * bitDepthScale;
        float B = texPlane1_U.Sample(sampLinear, uv) * bitDepthScale;
        float R = texPlane2_V.Sample(sampLinear, uv) * bitDepthScale;
        rgb = float3(R, G, B);

        if (hasAlpha > 0.5) {
            A = texPlane3_A.Sample(sampLinear, uv) * bitDepthScale;
        }
    } else {
        // YUV path
        float Y, U, V;
        bool needsScaling = false;

        if (useTextureArray > 0.5) {
            // D3D11VA path (NV12/P010 texture arrays)
            // P010 values are MSB-aligned, no scaling needed
            Y = texYArray.Sample(sampLinear, float3(uv, 0));
            float2 UV = texUVArray.Sample(sampLinear, float3(uv, 0));
            U = UV.x;
            V = UV.y;
            needsScaling = false;
        } else if (planeCount < 2.5) {
            // 2-plane NV12/P010 layout (interleaved UV)
            // P010 values are MSB-aligned, no scaling needed
            Y = texPlane0.Sample(sampLinear, uv);
            float2 UV = texPlane1_NV12.Sample(sampLinear, uv);
            U = UV.x;
            V = UV.y;
            needsScaling = false;
        } else {
            // 3 or 4-plane planar YUV (YUV420P, YUV422P, YUV444P, YUVA)
            // Software decode stores values in lower bits, needs scaling for 10/12-bit
            Y = texPlane0.Sample(sampLinear, uv);
            U = texPlane1_U.Sample(sampLinear, uv);
            V = texPlane2_V.Sample(sampLinear, uv);
            // Bilinear filtering handles chroma upscale automatically
            needsScaling = (bitDepthScale > 1.5);  // Only for 10/12-bit
        }

        // Apply bit depth scaling for 10/12-bit software-decoded planar content
        if (needsScaling) {
            Y = Y * bitDepthScale;
            U = U * bitDepthScale;
            V = V * bitDepthScale;
        }

        // Range expansion for video range (16-235/16-240)
        if (isFullRange < 0.5) {
            const float foot = 16.0 / 255.0;
            const float yHead = 235.0 / 255.0;
            const float uvHead = 240.0 / 255.0;
            Y = saturate((Y - foot) / (yHead - foot));
            U = saturate((U - foot) / (uvHead - foot));
            V = saturate((V - foot) / (uvHead - foot));
        }

        // Convert UV from [0,1] to [-0.5, 0.5]
        U = U - 0.5;
        V = V - 0.5;

        // Apply color matrix (BT.709 or BT.2020)
        float3x3 colorMat = (colorSpace > 0.5) ? BT2020_MAT : BT709_MAT;
        float3 yuv = float3(Y, U, V);
        rgb = mul(colorMat, yuv);

        if (hasAlpha > 0.5) {
            // Alpha plane from software decode needs scaling for 10/12-bit
            float rawA = texPlane3_A.Sample(sampLinear, uv);
            A = (bitDepthScale > 1.5) ? rawA * bitDepthScale : rawA;
        }
    }

    return float4(saturate(rgb), A);
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
    float planeCount;
    float hasAlpha;
    float isRGBPlanar;
    float bitDepthScale;
    float padding[3];  // Align to 16-byte boundary (48 bytes total)
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
        cb->planeCount = static_cast<float>(params.plane_count);
        cb->hasAlpha = params.has_alpha ? 1.0f : 0.0f;
        cb->isRGBPlanar = params.is_rgb_planar ? 1.0f : 0.0f;
        // Bit depth scale: normalize 10/12-bit values stored in R16 to [0,1]
        // R16_UNORM stores 10-bit as 0-65535 (scaled from 0-1023)
        // R16_UNORM stores 12-bit as 0-65535 (scaled from 0-4095)
        if (params.bit_depth == 10) {
            cb->bitDepthScale = 65535.0f / 1023.0f;  // ~64.06
        } else if (params.bit_depth == 12) {
            cb->bitDepthScale = 65535.0f / 4095.0f;  // ~16.00
        } else {
            cb->bitDepthScale = 1.0f;  // 8-bit, already normalized
        }
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

    // Textures - bind based on mode
    // t0: Y plane
    // t1: UV interleaved (NV12)
    // t2: U plane (planar)
    // t3: V plane (planar)
    // t4: Y array (D3D11VA)
    // t5: UV array (D3D11VA)
    // t6: A plane (alpha)
    if (params.use_texture_array) {
        // Texture arrays: bind to t4, t5 (texYArray, texUVArray)
        ID3D11ShaderResourceView* null_srvs[] = { nullptr, nullptr, nullptr, nullptr };
        context_->PSSetShaderResources(0, 4, null_srvs);  // Clear t0-t3
        ID3D11ShaderResourceView* array_srvs[] = { srv_y, srv_uv };
        context_->PSSetShaderResources(4, 2, array_srvs);  // Bind to t4, t5
        ID3D11ShaderResourceView* null_alpha = nullptr;
        context_->PSSetShaderResources(6, 1, &null_alpha);  // Clear t6
    } else {
        // Regular 2-plane NV12/P010: bind to t0, t1
        ID3D11ShaderResourceView* srvs[] = { srv_y, srv_uv, nullptr, nullptr };
        context_->PSSetShaderResources(0, 4, srvs);
        // Clear texture array slots and alpha
        ID3D11ShaderResourceView* null_srvs[] = { nullptr, nullptr };
        context_->PSSetShaderResources(4, 2, null_srvs);
        ID3D11ShaderResourceView* null_alpha = nullptr;
        context_->PSSetShaderResources(6, 1, &null_alpha);
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
    ID3D11ShaderResourceView* null_srvs_all[] = { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };
    context_->PSSetShaderResources(0, 7, null_srvs_all);  // Clear t0-t6
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
    params.plane_count = 2;  // This overload is for NV12/P010

    return Render(srv_y, srv_uv, dest_rtv, params);
}

//=============================================================================
// Render (3-plane planar YUV version)
//=============================================================================

bool D3D11YUVRenderer::Render(ID3D11ShaderResourceView* srv_plane0,
                              ID3D11ShaderResourceView* srv_plane1,
                              ID3D11ShaderResourceView* srv_plane2,
                              ID3D11RenderTargetView* dest_rtv,
                              const YUVRenderParams& params) {
    // For 2-plane NV12/P010, srv_plane2 will be nullptr
    if (!initialized_ || !srv_plane0 || !srv_plane1 || !dest_rtv) {
        Debug::Log("D3D11YUVRenderer::Render(3-plane): Invalid params - init=" +
                   std::to_string(initialized_) + " srv0=" + std::to_string(srv_plane0 != nullptr) +
                   " srv1=" + std::to_string(srv_plane1 != nullptr) +
                   " rtv=" + std::to_string(dest_rtv != nullptr));
        return false;
    }

    // For 3-plane mode, srv_plane2 must be present
    if (params.plane_count >= 3 && !srv_plane2) {
        Debug::Log("D3D11YUVRenderer::Render(3-plane): Missing V plane SRV for 3-plane format");
        return false;
    }

    // Log first 3-plane render for debugging
    static bool first_3plane_render = true;
    if (first_3plane_render && params.plane_count >= 3) {
        Debug::Log("D3D11YUVRenderer::Render: First 3-plane render " +
                   std::to_string(params.width) + "x" + std::to_string(params.height) +
                   " plane_count=" + std::to_string(params.plane_count));
        first_3plane_render = false;
    }

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
        cb->planeCount = static_cast<float>(params.plane_count);
        cb->hasAlpha = params.has_alpha ? 1.0f : 0.0f;
        cb->isRGBPlanar = params.is_rgb_planar ? 1.0f : 0.0f;
        // Bit depth scale: normalize 10/12-bit values stored in R16 to [0,1]
        if (params.bit_depth == 10) {
            cb->bitDepthScale = 65535.0f / 1023.0f;
        } else if (params.bit_depth == 12) {
            cb->bitDepthScale = 65535.0f / 4095.0f;
        } else {
            cb->bitDepthScale = 1.0f;
        }
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

    // Textures - bind based on plane count
    // t0: Y plane (or G for GBRP)
    // t1: UV interleaved (NV12) - used for 2-plane
    // t2: U plane (planar, or B for GBRP) - used for 3-plane
    // t3: V plane (planar, or R for GBRP) - used for 3-plane
    // t4, t5: texture arrays (not used here)
    // t6: alpha (not used in 3-plane)
    if (params.plane_count >= 3) {
        // 3-plane planar: Y at t0, U at t2, V at t3
        ID3D11ShaderResourceView* srvs[] = { srv_plane0, nullptr, srv_plane1, srv_plane2 };
        context_->PSSetShaderResources(0, 4, srvs);
    } else {
        // 2-plane NV12/P010: Y at t0, UV at t1
        ID3D11ShaderResourceView* srvs[] = { srv_plane0, srv_plane1, nullptr, nullptr };
        context_->PSSetShaderResources(0, 4, srvs);
    }
    // Clear texture array slots and alpha
    ID3D11ShaderResourceView* null_srvs[] = { nullptr, nullptr };
    context_->PSSetShaderResources(4, 2, null_srvs);
    ID3D11ShaderResourceView* null_alpha = nullptr;
    context_->PSSetShaderResources(6, 1, &null_alpha);
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
    ID3D11ShaderResourceView* null_srvs_all[] = { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };
    context_->PSSetShaderResources(0, 7, null_srvs_all);  // Clear t0-t6
    ID3D11RenderTargetView* null_rtv = nullptr;
    context_->OMSetRenderTargets(1, &null_rtv, nullptr);

    return true;
}

//=============================================================================
// Render (4-plane YUVA/GBRAP version)
//=============================================================================

bool D3D11YUVRenderer::Render(ID3D11ShaderResourceView* srv_plane0,
                              ID3D11ShaderResourceView* srv_plane1,
                              ID3D11ShaderResourceView* srv_plane2,
                              ID3D11ShaderResourceView* srv_plane3,
                              ID3D11RenderTargetView* dest_rtv,
                              const YUVRenderParams& params) {
    if (!initialized_ || !srv_plane0 || !srv_plane1 || !srv_plane2 || !srv_plane3 || !dest_rtv) {
        Debug::Log("D3D11YUVRenderer::Render(4-plane): Invalid params - init=" +
                   std::to_string(initialized_) + " srv0=" + std::to_string(srv_plane0 != nullptr) +
                   " srv1=" + std::to_string(srv_plane1 != nullptr) +
                   " srv2=" + std::to_string(srv_plane2 != nullptr) +
                   " srv3=" + std::to_string(srv_plane3 != nullptr) +
                   " rtv=" + std::to_string(dest_rtv != nullptr));
        return false;
    }

    // Log first 4-plane render for debugging
    static bool first_4plane_render = true;
    if (first_4plane_render) {
        Debug::Log("D3D11YUVRenderer::Render: First 4-plane render " +
                   std::to_string(params.width) + "x" + std::to_string(params.height) +
                   " plane_count=" + std::to_string(params.plane_count) +
                   " has_alpha=" + std::to_string(params.has_alpha) +
                   " is_rgb_planar=" + std::to_string(params.is_rgb_planar));
        first_4plane_render = false;
    }

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
        cb->planeCount = static_cast<float>(params.plane_count);
        cb->hasAlpha = params.has_alpha ? 1.0f : 0.0f;
        cb->isRGBPlanar = params.is_rgb_planar ? 1.0f : 0.0f;
        // Bit depth scale: normalize 10/12-bit values stored in R16 to [0,1]
        if (params.bit_depth == 10) {
            cb->bitDepthScale = 65535.0f / 1023.0f;
        } else if (params.bit_depth == 12) {
            cb->bitDepthScale = 65535.0f / 4095.0f;
        } else {
            cb->bitDepthScale = 1.0f;
        }
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

    // Textures - 4-plane: Y/G at t0, U/B at t2, V/R at t3, A at t6
    // t0: Y plane (or G for GBRAP)
    // t1: unused (NV12 interleaved)
    // t2: U plane (or B for GBRAP)
    // t3: V plane (or R for GBRAP)
    // t4, t5: texture arrays (not used)
    // t6: A plane (alpha)
    ID3D11ShaderResourceView* srvs[] = { srv_plane0, nullptr, srv_plane1, srv_plane2 };
    context_->PSSetShaderResources(0, 4, srvs);
    // Clear texture array slots
    ID3D11ShaderResourceView* null_srvs[] = { nullptr, nullptr };
    context_->PSSetShaderResources(4, 2, null_srvs);
    // Bind alpha plane at t6
    context_->PSSetShaderResources(6, 1, &srv_plane3);
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
    ID3D11ShaderResourceView* null_srvs_all[] = { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };
    context_->PSSetShaderResources(0, 7, null_srvs_all);  // Clear t0-t6
    ID3D11RenderTargetView* null_rtv = nullptr;
    context_->OMSetRenderTargets(1, &null_rtv, nullptr);

    return true;
}

} // namespace qcview

#endif // _WIN32
