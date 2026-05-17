// D3D11AnnotationRenderer — see header for porting notes.

#include "d3d11_annotation_renderer.h"

#include "annotations/stroke_tessellator.h"   // TessellatedMesh + TessVertex
#include "d3d11_device_manager.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <QtLogging>

#include <atomic>
#include <cstring>
#include <string>

namespace qcv {

using Microsoft::WRL::ComPtr;

namespace {

// Vertex layout matches TessVertex exactly (24 bytes: float2 pos +
// float4 color). The CPU tessellator writes into TessVertex memory;
// we memcpy that straight into the GPU vertex buffer.
static_assert(sizeof(TessVertex) == 6 * sizeof(float),
              "TessVertex layout mismatch — annotation upload assumes "
              "{x, y, r, g, b, a} packed tightly");

constexpr const char *kVsHlsl = R"(
cbuffer Constants : register(b0) {
    float2 targetSize;   // viewport size in pixels
    float2 _pad;
};
struct VsIn  { float2 pos : POSITION; float4 col : COLOR; };
struct VsOut { float4 pos : SV_POSITION; float4 col : COLOR; };
VsOut VSMain(VsIn i)
{
    VsOut o;
    // pixel coords → NDC. D3D11 top-left origin → flip Y so the
    // tessellator's (0,0) at top-left maps to NDC (-1, +1) and
    // (W,H) at bottom-right maps to (+1, -1).
    o.pos = float4(
        i.pos.x / targetSize.x * 2.0 - 1.0,
        1.0 - i.pos.y / targetSize.y * 2.0,
        0.0, 1.0);
    o.col = i.col;
    return o;
}
)";

// Phase F.2.9 — per-swapchain-format pixel-shader variants.
//
// Tessellator writes sRGB-picked RGBA (the user-chosen color in 0..1
// sRGB display space). Each swapchain colorspace needs a different
// interpretation:
//
//   SDR BGRA8 (G22_NONE_P709): OS expects sRGB-encoded values; pass
//     through verbatim. Matches Metal annotation behavior on macOS.
//
//   scRGB R16G16B16A16_FLOAT (G10_NONE_P709): swapchain is linear-light
//     with 1.0 = 80 nits. Apply sRGB→linear EOTF and scale by refNits/80
//     so strokes land at the stated reference luminance.
//
//   HDR10 PQ R10G10B10A2_UNORM (G2084_NONE_P2020): swapchain expects
//     PQ-encoded BT.2020 values. Convert sRGB→linear, primary-rotate
//     BT.709→BT.2020, normalize to 10,000 nits (PQ's full range), then
//     ST.2084-encode. Without this step a 1.0 white stroke would clip
//     to 10,000 nits on the display — destructively bright.
//
// All variants share the b0 cbuffer for uRefNits; SDR ignores it.
constexpr const char *kPsHlslCommon = R"(
struct VsOut { float4 pos : SV_POSITION; float4 col : COLOR; };

cbuffer PsConstants : register(b0) {
    float uRefNits;
    float3 _ps_pad;
};

float3 srgbToLinear(float3 c)
{
    float3 lo = c / 12.92;
    float3 hi = pow(max((c + 0.055) / 1.055, 0.0), 2.4);
    float3 s  = step(0.04045, c);
    return lerp(lo, hi, s);
}

static const float3x3 kBt709ToBt2020 = float3x3(
    0.6274040, 0.3292820, 0.0433136,
    0.0690970, 0.9195400, 0.0113612,
    0.0163914, 0.0880132, 0.8955950
);

float3 pqEncode(float3 lin)
{
    // ST.2084 inverse-EOTF. lin is normalized so that 1.0 = 10,000 nits.
    const float m1 = 0.1593017578125;   // 2610 / 16384
    const float m2 = 78.84375;          // 2523 /    32
    const float c1 =  0.8359375;        // 3424 /  4096
    const float c2 = 18.8515625;        // 2413 /   128
    const float c3 = 18.6875;           // 2392 /   128
    float3 Y   = max(lin, 0.0);
    float3 Ym1 = pow(Y, m1);
    float3 num = c1 + c2 * Ym1;
    float3 den = 1.0 + c3 * Ym1;
    return pow(num / den, m2);
}
)";

constexpr const char *kPsHlslSdr = R"(
float4 PSMain(VsOut i) : SV_TARGET
{
    // SDR (G22_NONE_P709): swapchain holds sRGB-encoded values, the
    // OS does no extra encoding. Pass the user-picked color through
    // verbatim — matches MetalAnnotationRenderer.
    return i.col;
}
)";

constexpr const char *kPsHlslScrgb = R"(
float4 PSMain(VsOut i) : SV_TARGET
{
    float3 lin   = srgbToLinear(i.col.rgb);
    float  scale = uRefNits / 80.0;  // scRGB: 1.0 = 80 nits
    return float4(lin * scale, i.col.a);
}
)";

constexpr const char *kPsHlslHdr10Pq = R"(
float4 PSMain(VsOut i) : SV_TARGET
{
    float3 lin709  = srgbToLinear(i.col.rgb);
    float3 lin2020 = mul(kBt709ToBt2020, lin709);
    float3 norm    = lin2020 * (uRefNits / 10000.0);
    return float4(pqEncode(norm), i.col.a);
}
)";

ComPtr<ID3DBlob> compileHlsl(const char *src, const char *entry, const char *target)
{
    ComPtr<ID3DBlob> code, errors;
    HRESULT hr = D3DCompile(src, std::strlen(src), entry, nullptr, nullptr,
                              entry, target,
                              D3DCOMPILE_OPTIMIZATION_LEVEL1, 0,
                              code.GetAddressOf(), errors.GetAddressOf());
    if (FAILED(hr)) {
        qWarning("D3D11AnnotationRenderer: compile %s failed (hr=0x%08lX)%s%s",
                 entry, static_cast<unsigned long>(hr),
                 errors ? " — " : "",
                 errors ? static_cast<const char *>(errors->GetBufferPointer()) : "");
        return nullptr;
    }
    return code;
}

constexpr UINT kInitialVbCapacityBytes = 1 * 1024 * 1024;   // 1 MB
constexpr UINT kVertexStride           = sizeof(TessVertex); // 24

} // namespace

struct D3D11AnnotationRenderer::Impl {
    ID3D11Device              *device   = nullptr;
    int                        targetFormat = 0;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader>  ps;
    ComPtr<ID3D11InputLayout>  inputLayout;
    ComPtr<ID3D11Buffer>       cbuf;        // VS targetSize cbuffer (b0 VS)
    ComPtr<ID3D11Buffer>       psCbuf;      // PS uRefNits cbuffer    (b0 PS)
    ComPtr<ID3D11Buffer>       vbuf;        // dynamic vertex buffer
    ComPtr<ID3D11BlendState>   blend;       // alpha source-over
    ComPtr<ID3D11RasterizerState> raster;   // solid + no cull

    UINT  vbCapacityBytes = 0;
    UINT  vbAppendOffset  = 0;
    bool  frameStarted    = false;

    // Phase F.2.9 — stroke reference luminance, nits. Used by the
    // scRGB / PQ variants only; SDR variant ignores it. Stored
    // atomically for cross-thread updates from the GUI side.
    std::atomic<float>          refNits{200.0f};
};

D3D11AnnotationRenderer::D3D11AnnotationRenderer()
    : m_impl(std::make_unique<Impl>()) {}

D3D11AnnotationRenderer::~D3D11AnnotationRenderer()
{
    shutdown();
}

bool D3D11AnnotationRenderer::initialize(int targetFormat)
{
    auto &mgr = D3D11DeviceManager::instance();
    m_impl->device = static_cast<ID3D11Device *>(mgr.device());
    if (!m_impl->device) {
        qWarning("D3D11AnnotationRenderer: no D3D11 device");
        return false;
    }
    m_impl->targetFormat = targetFormat;

    // Phase F.2.9 — pick the PS variant by swapchain DXGI format.
    // DXGI_FORMAT values referenced inline (header avoids dragging
    // <dxgiformat.h> through the public API):
    //   R10G10B10A2_UNORM  (24) → HDR10 PQ
    //   R16G16B16A16_FLOAT (10) → scRGB linear
    //   anything else            → SDR (pass-through)
    const char *psVariantBody = kPsHlslSdr;
    const char *psVariantName = "SDR";
    switch (static_cast<DXGI_FORMAT>(targetFormat)) {
        case DXGI_FORMAT_R10G10B10A2_UNORM:
            psVariantBody = kPsHlslHdr10Pq;
            psVariantName = "HDR10 PQ";
            break;
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
            psVariantBody = kPsHlslScrgb;
            psVariantName = "scRGB linear";
            break;
        default:
            break;
    }
    const std::string psSource = std::string(kPsHlslCommon) + psVariantBody;

    auto vsBlob = compileHlsl(kVsHlsl, "VSMain", "vs_5_0");
    auto psBlob = compileHlsl(psSource.c_str(), "PSMain", "ps_5_0");
    if (!vsBlob || !psBlob) return false;
    qInfo("D3D11AnnotationRenderer: PS variant = %s", psVariantName);

    if (FAILED(m_impl->device->CreateVertexShader(
            vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
            nullptr, m_impl->vs.GetAddressOf()))) {
        qWarning("D3D11AnnotationRenderer: CreateVertexShader failed");
        return false;
    }
    if (FAILED(m_impl->device->CreatePixelShader(
            psBlob->GetBufferPointer(), psBlob->GetBufferSize(),
            nullptr, m_impl->ps.GetAddressOf()))) {
        qWarning("D3D11AnnotationRenderer: CreatePixelShader failed");
        return false;
    }

    // Input layout matches TessVertex: 2 floats position, 4 floats RGBA.
    const D3D11_INPUT_ELEMENT_DESC ied[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT,       0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0,  8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    if (FAILED(m_impl->device->CreateInputLayout(
            ied, _countof(ied),
            vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
            m_impl->inputLayout.GetAddressOf()))) {
        qWarning("D3D11AnnotationRenderer: CreateInputLayout failed");
        return false;
    }

    // VS constant buffer for viewport size (16 bytes — float2 + pad).
    D3D11_BUFFER_DESC cbd{};
    cbd.ByteWidth      = 16;
    cbd.Usage          = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(m_impl->device->CreateBuffer(&cbd, nullptr, m_impl->cbuf.GetAddressOf()))) {
        qWarning("D3D11AnnotationRenderer: CreateBuffer (cbuf) failed");
        return false;
    }

    // PS constant buffer for reference luminance (16 bytes — float + pad).
    D3D11_BUFFER_DESC pcbd{};
    pcbd.ByteWidth      = 16;
    pcbd.Usage          = D3D11_USAGE_DYNAMIC;
    pcbd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    pcbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(m_impl->device->CreateBuffer(&pcbd, nullptr, m_impl->psCbuf.GetAddressOf()))) {
        qWarning("D3D11AnnotationRenderer: CreateBuffer (psCbuf) failed");
        return false;
    }

    // Initial vertex buffer — grown later if a frame's tessellation
    // outruns its capacity.
    D3D11_BUFFER_DESC vbd{};
    vbd.ByteWidth      = kInitialVbCapacityBytes;
    vbd.Usage          = D3D11_USAGE_DYNAMIC;
    vbd.BindFlags      = D3D11_BIND_VERTEX_BUFFER;
    vbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(m_impl->device->CreateBuffer(&vbd, nullptr, m_impl->vbuf.GetAddressOf()))) {
        qWarning("D3D11AnnotationRenderer: CreateBuffer (vbuf) failed");
        return false;
    }
    m_impl->vbCapacityBytes = kInitialVbCapacityBytes;
    m_impl->vbAppendOffset  = 0;

    // Alpha source-over blend state matching MetalAnnotationRenderer.
    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].BlendEnable    = TRUE;
    bd.RenderTarget[0].SrcBlend       = D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend      = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp        = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha  = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOpAlpha   = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(m_impl->device->CreateBlendState(&bd, m_impl->blend.GetAddressOf()))) {
        qWarning("D3D11AnnotationRenderer: CreateBlendState failed");
        return false;
    }

    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    if (FAILED(m_impl->device->CreateRasterizerState(&rd, m_impl->raster.GetAddressOf()))) {
        qWarning("D3D11AnnotationRenderer: CreateRasterizerState failed");
        return false;
    }

    qInfo("D3D11AnnotationRenderer: initialized (target DXGI_FORMAT=%d)", targetFormat);
    return true;
}

void D3D11AnnotationRenderer::shutdown()
{
    if (!m_impl) return;
    m_impl->vs.Reset();
    m_impl->ps.Reset();
    m_impl->inputLayout.Reset();
    m_impl->cbuf.Reset();
    m_impl->psCbuf.Reset();
    m_impl->vbuf.Reset();
    m_impl->blend.Reset();
    m_impl->raster.Reset();
    m_impl->device = nullptr;
    m_impl->vbCapacityBytes = 0;
    m_impl->vbAppendOffset  = 0;
    m_impl->frameStarted    = false;
}

bool D3D11AnnotationRenderer::isInitialized() const
{
    return m_impl && m_impl->device && m_impl->vs && m_impl->ps;
}

int D3D11AnnotationRenderer::targetPixelFormat() const
{
    return m_impl ? m_impl->targetFormat : 0;
}

void D3D11AnnotationRenderer::beginFrame()
{
    if (!m_impl) return;
    m_impl->vbAppendOffset = 0;
    m_impl->frameStarted   = true;
}

void D3D11AnnotationRenderer::setReferenceLuminance(float nits)
{
    if (!m_impl) return;
    if (nits < 1.0f) nits = 1.0f;        // clamp to sane positive
    m_impl->refNits.store(nits, std::memory_order_relaxed);
}

void D3D11AnnotationRenderer::drawMesh(void *ctxPtr,
                                         const TessellatedMesh &mesh,
                                         int targetWidth, int targetHeight)
{
    if (!isInitialized() || !ctxPtr || targetWidth <= 0 || targetHeight <= 0) return;
    if (mesh.vertices.empty()) return;
    auto *ctx = static_cast<ID3D11DeviceContext *>(ctxPtr);

    const UINT bytesNeeded =
        static_cast<UINT>(mesh.vertices.size()) * kVertexStride;

    // Grow the vertex buffer if this single mesh exceeds capacity OR
    // the running per-frame total would overflow. Re-create at the
    // next power-of-two so we don't thrash on incremental growth.
    if (m_impl->vbAppendOffset + bytesNeeded > m_impl->vbCapacityBytes) {
        UINT newCap = m_impl->vbCapacityBytes;
        if (newCap == 0) newCap = kInitialVbCapacityBytes;
        while (newCap < m_impl->vbAppendOffset + bytesNeeded) newCap *= 2;

        D3D11_BUFFER_DESC vbd{};
        vbd.ByteWidth      = newCap;
        vbd.Usage          = D3D11_USAGE_DYNAMIC;
        vbd.BindFlags      = D3D11_BIND_VERTEX_BUFFER;
        vbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        ComPtr<ID3D11Buffer> newBuf;
        if (FAILED(m_impl->device->CreateBuffer(&vbd, nullptr, newBuf.GetAddressOf()))) {
            qWarning("D3D11AnnotationRenderer: vbuf grow to %u bytes failed", newCap);
            return;
        }
        // Discard the in-frame appends so far — we can't easily copy
        // bytes between a DYNAMIC buffer and a new DYNAMIC buffer
        // without a STAGING staging step. In practice the 1MB
        // starting capacity covers typical scenes; the only path
        // here is a heavy SBS + safety-overlay frame on the first
        // run after init, and dropping one frame's earlier mesh is
        // imperceptible.
        m_impl->vbuf            = newBuf;
        m_impl->vbCapacityBytes = newCap;
        m_impl->vbAppendOffset  = 0;
    }

    // Map. First mesh of the frame uses WRITE_DISCARD (renames the
    // underlying GPU memory; lets us write while the GPU may still
    // be sampling the previous frame's buffer). Subsequent meshes
    // in the same frame use WRITE_NO_OVERWRITE (no rename — the
    // driver trusts us not to clobber already-Drawn vertices).
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const D3D11_MAP mapType =
        (m_impl->vbAppendOffset == 0)
            ? D3D11_MAP_WRITE_DISCARD
            : D3D11_MAP_WRITE_NO_OVERWRITE;
    if (FAILED(ctx->Map(m_impl->vbuf.Get(), 0, mapType, 0, &mapped))) {
        qWarning("D3D11AnnotationRenderer: vbuf Map failed");
        return;
    }
    auto *dst = static_cast<unsigned char *>(mapped.pData) + m_impl->vbAppendOffset;
    std::memcpy(dst, mesh.vertices.data(), bytesNeeded);
    ctx->Unmap(m_impl->vbuf.Get(), 0);

    // Update the per-draw constant buffer with the current viewport
    // size. Could share across draws in a frame if dst dims don't
    // change, but the cost is one ctx->Map per draw — negligible.
    D3D11_MAPPED_SUBRESOURCE cmapped{};
    if (FAILED(ctx->Map(m_impl->cbuf.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &cmapped))) {
        qWarning("D3D11AnnotationRenderer: cbuf Map failed");
        return;
    }
    float cb[4] = { static_cast<float>(targetWidth),
                    static_cast<float>(targetHeight),
                    0.0f, 0.0f };
    std::memcpy(cmapped.pData, cb, sizeof(cb));
    ctx->Unmap(m_impl->cbuf.Get(), 0);

    // Pipeline state.
    ctx->IASetInputLayout(m_impl->inputLayout.Get());
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11Buffer *vbs[1] = { m_impl->vbuf.Get() };
    UINT strides[1] = { kVertexStride };
    UINT offsets[1] = { m_impl->vbAppendOffset };
    ctx->IASetVertexBuffers(0, 1, vbs, strides, offsets);

    ctx->VSSetShader(m_impl->vs.Get(), nullptr, 0);
    ID3D11Buffer *cbs[1] = { m_impl->cbuf.Get() };
    ctx->VSSetConstantBuffers(0, 1, cbs);
    ctx->PSSetShader(m_impl->ps.Get(), nullptr, 0);

    // Update + bind the PS reference-luminance cbuffer at b0.
    if (m_impl->psCbuf) {
        D3D11_MAPPED_SUBRESOURCE pmapped{};
        if (SUCCEEDED(ctx->Map(m_impl->psCbuf.Get(), 0,
                                  D3D11_MAP_WRITE_DISCARD, 0, &pmapped))) {
            float pcb[4] = { m_impl->refNits.load(std::memory_order_relaxed),
                             0.0f, 0.0f, 0.0f };
            std::memcpy(pmapped.pData, pcb, sizeof(pcb));
            ctx->Unmap(m_impl->psCbuf.Get(), 0);
        }
        ID3D11Buffer *pcbs[1] = { m_impl->psCbuf.Get() };
        ctx->PSSetConstantBuffers(0, 1, pcbs);
    }

    ctx->RSSetState(m_impl->raster.Get());
    const float blendFactor[4] = {0, 0, 0, 0};
    ctx->OMSetBlendState(m_impl->blend.Get(), blendFactor, 0xFFFFFFFF);

    ctx->Draw(static_cast<UINT>(mesh.vertices.size()), 0);

    m_impl->vbAppendOffset += bytesNeeded;
}

} // namespace qcv
