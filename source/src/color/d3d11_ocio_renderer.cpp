#include "d3d11_ocio_renderer.h"
#include "ocio_pipeline.h"
#include "../gpu/d3d11_device_manager.h"

#ifdef _WIN32

#include "../utils/debug_utils.h"

namespace ump {

D3D11OCIORenderer::D3D11OCIORenderer() = default;

D3D11OCIORenderer::~D3D11OCIORenderer() {
    Shutdown();
}

bool D3D11OCIORenderer::Initialize() {
    if (initialized_) return true;

    auto& device_mgr = D3D11DeviceManager::Instance();
    if (!device_mgr.IsInitialized()) {
        Debug::Log("D3D11OCIORenderer: Device manager not initialized");
        return false;
    }

    if (!CreateQuadGeometry()) {
        Debug::Log("D3D11OCIORenderer: Failed to create quad geometry");
        return false;
    }

    if (!CreatePassthroughShader()) {
        Debug::Log("D3D11OCIORenderer: Failed to create passthrough shader");
        Shutdown();
        return false;
    }

    if (!CreateSamplers()) {
        Debug::Log("D3D11OCIORenderer: Failed to create samplers");
        Shutdown();
        return false;
    }

    if (!CreateRasterizerState()) {
        Debug::Log("D3D11OCIORenderer: Failed to create rasterizer state");
        Shutdown();
        return false;
    }

    if (!CreateBlendState()) {
        Debug::Log("D3D11OCIORenderer: Failed to create blend state");
        Shutdown();
        return false;
    }

    initialized_ = true;
    Debug::Log("D3D11OCIORenderer: Initialized successfully");
    return true;
}

void D3D11OCIORenderer::Shutdown() {
    blend_state_.Reset();
    rasterizer_state_.Reset();
    point_sampler_.Reset();
    linear_sampler_.Reset();
    passthrough_ps_.Reset();
    passthrough_vs_.Reset();
    input_layout_.Reset();
    quad_vertex_buffer_.Reset();

    initialized_ = false;
    Debug::Log("D3D11OCIORenderer: Shutdown complete");
}

bool D3D11OCIORenderer::CreateQuadGeometry() {
    auto& device_mgr = D3D11DeviceManager::Instance();

    // Fullscreen quad vertices (position XY, texcoord UV)
    // NDC coordinates: -1 to 1, texture coordinates: 0 to 1
    QuadVertex vertices[] = {
        // Triangle 1
        { -1.0f,  1.0f,  0.0f, 0.0f },  // Top-left
        {  1.0f,  1.0f,  1.0f, 0.0f },  // Top-right
        { -1.0f, -1.0f,  0.0f, 1.0f },  // Bottom-left
        // Triangle 2
        {  1.0f,  1.0f,  1.0f, 0.0f },  // Top-right
        {  1.0f, -1.0f,  1.0f, 1.0f },  // Bottom-right
        { -1.0f, -1.0f,  0.0f, 1.0f },  // Bottom-left
    };

    quad_vertex_buffer_ = device_mgr.CreateVertexBuffer(
        vertices, sizeof(vertices), false);

    return quad_vertex_buffer_ != nullptr;
}

bool D3D11OCIORenderer::CreatePassthroughShader() {
    auto& device_mgr = D3D11DeviceManager::Instance();

    // Compile vertex shader and get bytecode for input layout
    Microsoft::WRL::ComPtr<ID3DBlob> vs_bytecode;
    passthrough_vs_ = device_mgr.CompileVertexShader(
        OCIO_QUAD_VS_HLSL, "VSMain", &vs_bytecode);

    if (!passthrough_vs_) {
        Debug::Log("D3D11OCIORenderer: Failed to compile vertex shader");
        return false;
    }

    // Create input layout
    D3D11_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };

    input_layout_ = device_mgr.CreateInputLayout(
        layout, _countof(layout), vs_bytecode.Get());

    if (!input_layout_) {
        Debug::Log("D3D11OCIORenderer: Failed to create input layout");
        return false;
    }

    // Compile passthrough pixel shader
    passthrough_ps_ = device_mgr.CompilePixelShader(
        OCIO_PASSTHROUGH_PS_HLSL, "PSMain");

    if (!passthrough_ps_) {
        Debug::Log("D3D11OCIORenderer: Failed to compile passthrough pixel shader");
        return false;
    }

    return true;
}

bool D3D11OCIORenderer::CreateSamplers() {
    auto& device_mgr = D3D11DeviceManager::Instance();

    linear_sampler_ = device_mgr.CreateSamplerState(
        D3D11_FILTER_MIN_MAG_MIP_LINEAR,
        D3D11_TEXTURE_ADDRESS_CLAMP);

    if (!linear_sampler_) return false;

    point_sampler_ = device_mgr.CreateSamplerState(
        D3D11_FILTER_MIN_MAG_MIP_POINT,
        D3D11_TEXTURE_ADDRESS_CLAMP);

    return point_sampler_ != nullptr;
}

bool D3D11OCIORenderer::CreateRasterizerState() {
    auto* device = D3D11DeviceManager::Instance().GetDevice();
    if (!device) return false;

    D3D11_RASTERIZER_DESC desc = {};
    desc.FillMode = D3D11_FILL_SOLID;
    desc.CullMode = D3D11_CULL_NONE;
    desc.FrontCounterClockwise = FALSE;
    desc.DepthClipEnable = TRUE;
    desc.ScissorEnable = FALSE;
    desc.MultisampleEnable = FALSE;
    desc.AntialiasedLineEnable = FALSE;

    HRESULT hr = device->CreateRasterizerState(&desc, &rasterizer_state_);
    return SUCCEEDED(hr);
}

bool D3D11OCIORenderer::CreateBlendState() {
    auto* device = D3D11DeviceManager::Instance().GetDevice();
    if (!device) return false;

    D3D11_BLEND_DESC desc = {};
    desc.RenderTarget[0].BlendEnable = FALSE;
    desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

    HRESULT hr = device->CreateBlendState(&desc, &blend_state_);
    return SUCCEEDED(hr);
}

void D3D11OCIORenderer::SetupRenderState(ID3D11RenderTargetView* rtv, int width, int height) {
    auto* ctx = D3D11DeviceManager::Instance().GetContext();
    if (!ctx) return;

    // Set render target
    ctx->OMSetRenderTargets(1, &rtv, nullptr);

    // Set viewport
    D3D11_VIEWPORT viewport = {};
    viewport.Width = static_cast<float>(width);
    viewport.Height = static_cast<float>(height);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    ctx->RSSetViewports(1, &viewport);

    // Set rasterizer state
    ctx->RSSetState(rasterizer_state_.Get());

    // Set blend state
    float blend_factor[4] = { 0, 0, 0, 0 };
    ctx->OMSetBlendState(blend_state_.Get(), blend_factor, 0xFFFFFFFF);

    // Set input layout and topology
    ctx->IASetInputLayout(input_layout_.Get());
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // Set vertex buffer
    UINT stride = sizeof(QuadVertex);
    UINT offset = 0;
    ID3D11Buffer* vb = quad_vertex_buffer_.Get();
    ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
}

void D3D11OCIORenderer::SaveState() {
    auto* ctx = D3D11DeviceManager::Instance().GetContext();
    if (!ctx) return;

    // Save render targets
    ctx->OMGetRenderTargets(1, &saved_state_.rtv, &saved_state_.dsv);

    // Save viewport
    saved_state_.num_viewports = 1;
    ctx->RSGetViewports(&saved_state_.num_viewports, &saved_state_.viewport);

    // Save shaders
    ctx->VSGetShader(&saved_state_.vs, nullptr, nullptr);
    ctx->PSGetShader(&saved_state_.ps, nullptr, nullptr);

    // Save input assembler state
    ctx->IAGetInputLayout(&saved_state_.layout);
    ctx->IAGetPrimitiveTopology(&saved_state_.topology);
    ctx->IAGetVertexBuffers(0, 1, &saved_state_.vb, &saved_state_.vb_stride, &saved_state_.vb_offset);

    // Save rasterizer state
    ctx->RSGetState(&saved_state_.rs);

    // Save blend state
    ctx->OMGetBlendState(&saved_state_.blend, saved_state_.blend_factor, &saved_state_.sample_mask);

    state_saved_ = true;
}

void D3D11OCIORenderer::RestoreState() {
    if (!state_saved_) return;

    auto* ctx = D3D11DeviceManager::Instance().GetContext();
    if (!ctx) return;

    // Restore render targets
    ID3D11RenderTargetView* rtvs[] = { saved_state_.rtv.Get() };
    ctx->OMSetRenderTargets(1, rtvs, saved_state_.dsv.Get());

    // Restore viewport
    ctx->RSSetViewports(saved_state_.num_viewports, &saved_state_.viewport);

    // Restore shaders
    ctx->VSSetShader(saved_state_.vs.Get(), nullptr, 0);
    ctx->PSSetShader(saved_state_.ps.Get(), nullptr, 0);

    // Restore input assembler state
    ctx->IASetInputLayout(saved_state_.layout.Get());
    ctx->IASetPrimitiveTopology(saved_state_.topology);
    ID3D11Buffer* vbs[] = { saved_state_.vb.Get() };
    ctx->IASetVertexBuffers(0, 1, vbs, &saved_state_.vb_stride, &saved_state_.vb_offset);

    // Restore rasterizer state
    ctx->RSSetState(saved_state_.rs.Get());

    // Restore blend state
    ctx->OMSetBlendState(saved_state_.blend.Get(), saved_state_.blend_factor, saved_state_.sample_mask);

    // Release saved COM references
    saved_state_.rtv.Reset();
    saved_state_.dsv.Reset();
    saved_state_.vs.Reset();
    saved_state_.ps.Reset();
    saved_state_.layout.Reset();
    saved_state_.vb.Reset();
    saved_state_.rs.Reset();
    saved_state_.blend.Reset();

    state_saved_ = false;
}

bool D3D11OCIORenderer::Apply(
    ::OCIOPipeline* pipeline,
    ID3D11ShaderResourceView* source,
    ID3D11RenderTargetView* dest,
    int width, int height) {

    if (!initialized_ || !pipeline || !source || !dest) return false;

    auto* ctx = D3D11DeviceManager::Instance().GetContext();
    if (!ctx) return false;

    // Check if pipeline has D3D11 shaders
    if (!pipeline->HasD3D11Shaders()) {
        // Fall back to passthrough if no D3D11 shaders available
        return ApplyPassthrough(source, dest, width, height);
    }

    // Setup render state
    SetupRenderState(dest, width, height);

    // Set vertex shader (use the pipeline's or our own)
    auto* vs = pipeline->GetD3D11VertexShader();
    if (vs) {
        ctx->VSSetShader(vs, nullptr, 0);
    } else {
        ctx->VSSetShader(passthrough_vs_.Get(), nullptr, 0);
    }

    // Set pixel shader from pipeline
    ctx->PSSetShader(pipeline->GetD3D11PixelShader(), nullptr, 0);

    // Set video texture
    ctx->PSSetShaderResources(0, 1, &source);

    // Set sampler for video texture
    ID3D11SamplerState* samplers[] = { linear_sampler_.Get() };
    ctx->PSSetSamplers(0, 1, samplers);

    // Bind LUT textures and samplers from pipeline
    const auto& lut_srvs = pipeline->GetD3D11LUTSRVs();
    const auto& lut_samplers = pipeline->GetD3D11LUTSamplers();

    if (!lut_srvs.empty()) {
        // Bind LUT SRVs starting at slot 1
        std::vector<ID3D11ShaderResourceView*> srv_ptrs;
        for (const auto& srv : lut_srvs) {
            srv_ptrs.push_back(srv.Get());
        }
        ctx->PSSetShaderResources(1, static_cast<UINT>(srv_ptrs.size()), srv_ptrs.data());
    }

    if (!lut_samplers.empty()) {
        // Bind LUT samplers starting at slot 1
        std::vector<ID3D11SamplerState*> sampler_ptrs;
        for (const auto& sampler : lut_samplers) {
            sampler_ptrs.push_back(sampler.Get());
        }
        ctx->PSSetSamplers(1, static_cast<UINT>(sampler_ptrs.size()), sampler_ptrs.data());
    }

    // Update constant buffer if pipeline has one
    auto* cb = pipeline->GetD3D11ConstantBuffer();
    if (cb) {
        ctx->PSSetConstantBuffers(0, 1, &cb);
    }

    // Draw fullscreen quad
    ctx->Draw(6, 0);

    // Unbind resources
    ID3D11ShaderResourceView* null_srvs[16] = { nullptr };
    ctx->PSSetShaderResources(0, 16, null_srvs);

    return true;
}

bool D3D11OCIORenderer::ApplyPassthrough(
    ID3D11ShaderResourceView* source,
    ID3D11RenderTargetView* dest,
    int width, int height) {

    if (!initialized_ || !source || !dest) return false;

    auto* ctx = D3D11DeviceManager::Instance().GetContext();
    if (!ctx) return false;

    // Setup render state
    SetupRenderState(dest, width, height);

    // Set shaders
    ctx->VSSetShader(passthrough_vs_.Get(), nullptr, 0);
    ctx->PSSetShader(passthrough_ps_.Get(), nullptr, 0);

    // Set source texture
    ctx->PSSetShaderResources(0, 1, &source);

    // Set sampler
    ID3D11SamplerState* samplers[] = { linear_sampler_.Get() };
    ctx->PSSetSamplers(0, 1, samplers);

    // Draw fullscreen quad
    ctx->Draw(6, 0);

    // Unbind resources
    ID3D11ShaderResourceView* null_srv = nullptr;
    ctx->PSSetShaderResources(0, 1, &null_srv);

    return true;
}

} // namespace ump

#endif // _WIN32
