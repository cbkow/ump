#include "d3d11_compositor.h"
#include "d3d11_device_manager.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <QtLogging>

#include <algorithm>

namespace qcv {

using Microsoft::WRL::ComPtr;

namespace {

// Vertex: fullscreen triangle with Y-flip for D3D11 top-left screen
// origin. Same shape as the F.1.a/F.1.b probe vertex shader (also
// used elsewhere in this codebase).
constexpr const char *kVsHlsl = R"(
struct VsOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
VsOut VSMain(uint vid : SV_VertexID)
{
    VsOut o;
    float2 uv = float2((vid << 1) & 2, vid & 2);
    o.uv  = uv;
    o.pos = float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, 0.0f, 1.0f);
    return o;
}
)";

// Pixel: aspect-fit single source on top of a background fill (solid
// black / solid dark grey / 32 px checkerboard in two greys). hasSrc=0
// paints background only; hasSrc=1 samples src inside fitRect and
// falls back to background outside. bgMode picks which fill.
//   0 = Black                  (0.000, 0.000, 0.000)
//   1 = DarkGray   ≈ 22/255    (0.0863 — #161616, Theme.bg / the
//                               rails' well tone; keep matched with
//                               the three rail-collapse seam fills)
//   2 = DarkCheckerboard       (#1f1f1f / #2e2e2e)
//   3 = LightCheckerboard      (#b3b3b3 / #cccccc)
constexpr const char *kPsHlsl = R"(
cbuffer Constants : register(b0) {
    float2 dstSize;        // destination viewport, pixels
    float2 fitRectMin;     // source-fit top-left, pixels
    float2 fitRectSize;    // source-fit size, pixels
    float  borderPx;       // edge-frame width in px; 0 = no border
    float  _pad0;
    int    hasSrc;
    int    bgMode;
    int    overlayMode;    // 0 = composite over bg fill (opaque output);
                           // 1 = straight src for hardware src-over blend,
                           //     transparent outside the source rect (the
                           //     viewport notice card; mirrors Metal's
                           //     present-compositor src-over + discard).
    int    _pad1;
    float  brightness;     // post-composite multiplier; 1.0 = identity
    float3 borderColor;    // RGB of the edge frame (used when borderPx > 0)
};

Texture2D    src       : register(t0);
SamplerState srcSamp   : register(s0);

struct VsOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

float4 backgroundColor(float2 fragPx)
{
    if (bgMode == 1) {
        return float4(0.0863, 0.0863, 0.0863, 1.0);
    } else if (bgMode == 2) {
        // Dark checkerboard — 32 px squares.
        int2 cell = int2(fragPx) / 32;
        bool odd = ((cell.x + cell.y) & 1) != 0;
        float v = odd ? 0.180 : 0.122;
        return float4(v, v, v, 1.0);
    } else if (bgMode == 3) {
        // Light checkerboard — same 32 px grid, brighter pair.
        int2 cell = int2(fragPx) / 32;
        bool odd = ((cell.x + cell.y) & 1) != 0;
        float v = odd ? 0.800 : 0.702;
        return float4(v, v, v, 1.0);
    }
    return float4(0.0, 0.0, 0.0, 1.0);   // Black (mode 0)
}

float4 PSMain(VsOut input) : SV_TARGET
{
    float2 fragPx = input.uv * dstSize;

    // Overlay mode (viewport notice card): blend the source STRAIGHT over
    // whatever is already in the RTV via the hardware src-over blend
    // state, and emit fully-transparent outside the source rect so the
    // existing viewport background shows through the card's rounded
    // corners / margin. Mirrors MetalCompositor's present pass
    // (enableSrcOverBlending + discard on alpha==0). No bg fill, no
    // border here — the card carries its own rounded border.
    if (overlayMode != 0) {
        if (hasSrc != 0 &&
            fragPx.x >= fitRectMin.x && fragPx.x < fitRectMin.x + fitRectSize.x &&
            fragPx.y >= fitRectMin.y && fragPx.y < fitRectMin.y + fitRectSize.y)
        {
            float2 srcUv = (fragPx - fitRectMin) / fitRectSize;
            float4 v = src.Sample(srcSamp, srcUv);
            return float4(v.rgb * brightness, v.a);
        }
        return float4(0.0, 0.0, 0.0, 0.0);   // leave RTV untouched
    }

    // Edge frame — painted over everything when borderPx > 0 (corner-
    // overlay thumbnails). The main video pass leaves borderPx at 0,
    // so this is a no-op there.
    if (borderPx > 0.0 &&
        (fragPx.x < borderPx || fragPx.x > dstSize.x - borderPx ||
         fragPx.y < borderPx || fragPx.y > dstSize.y - borderPx)) {
        return float4(borderColor * brightness, 1.0);
    }
    float4 bg = backgroundColor(fragPx);
    if (hasSrc != 0 &&
        fragPx.x >= fitRectMin.x && fragPx.x <  fitRectMin.x + fitRectSize.x &&
        fragPx.y >= fitRectMin.y && fragPx.y <  fitRectMin.y + fitRectSize.y)
    {
        float2 srcUv = (fragPx - fitRectMin) / fitRectSize;
        float4 v = src.Sample(srcSamp, srcUv);
        // Source-over-alpha composite onto the bg fill, then write
        // fully-opaque to the swapchain so DComp doesn't compose
        // transparent video pixels against whatever's underneath the
        // player surface (which currently reads as a flat blue tint).
        // The user-picked Background mode shows through where the
        // video has alpha < 1.
        float3 rgb = v.rgb * v.a + bg.rgb * (1.0 - v.a);
        return float4(rgb * brightness, 1.0);
    }
    return float4(bg.rgb * brightness, 1.0);
}
)";

// Loading spinner — full-viewport dark fill + a ring of 8 rotating
// dots tinted with the cobalt accent. Self-contained (no source
// texture). Verbatim port of the Metal spin_fs in dual_compositor.mm.
// Reuses the compositor's fullscreen-triangle VS (VsOut.uv).
constexpr const char *kSpinnerPsHlsl = R"(
cbuffer SpinConstants : register(b0) {
    float2 spinDstSize;   // viewport pixels
    float  spinTime;      // seconds since the spinner started
    float  _spinPad;
};

struct VsOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

float4 SpinPSMain(VsOut input) : SV_TARGET
{
    float4 bg = float4(0.0863, 0.0863, 0.0863, 1.0);   // #161616 (Theme.bg)
    float2 px = input.uv * spinDstSize;
    float2 c  = spinDstSize * 0.5;
    float  r  = min(spinDstSize.x, spinDstSize.y) * 0.03;
    float  dotR = r * 0.18;
    const int N = 8;
    float rot = -spinTime * 6.2831853 / 1.2;   // one turn / 1.2 s
    float minDist = 1e6;
    int   minIdx  = 0;
    for (int i = 0; i < N; ++i) {
        float ang = rot + (6.2831853 * float(i) / float(N));
        float2 dotPos = c + float2(cos(ang), sin(ang)) * r;
        float d = distance(px, dotPos);
        if (d < minDist) { minDist = d; minIdx = i; }
    }
    if (minDist < dotR) {
        float  brightness = 0.35 + 0.65 * (1.0 - float(minIdx) / float(N - 1));
        float3 accent = float3(0.0039, 0.5373, 0.9451);   // #0189f1
        float  fade = smoothstep(dotR, dotR * 0.55, minDist);
        return float4(lerp(bg.rgb, accent * brightness, fade), 1.0);
    }
    return bg;
}
)";

ComPtr<ID3DBlob> compile(const char *src, const char *entry, const char *target)
{
    ComPtr<ID3DBlob> code, errors;
    const HRESULT hr = D3DCompile(
        src, std::strlen(src), entry, nullptr, nullptr,
        entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
        code.GetAddressOf(), errors.GetAddressOf());
    if (FAILED(hr)) {
        qCritical("D3D11Compositor: D3DCompile %s failed (hr=0x%08lX)\n%s",
                  entry, static_cast<unsigned long>(hr),
                  errors ? static_cast<const char *>(errors->GetBufferPointer())
                         : "(no error blob)");
        return nullptr;
    }
    return code;
}

struct SpinnerCB {
    float dstSize[2];   // 0 : 8
    float time;         // 8 : 12
    float pad;          // 12 : 16
};
static_assert(sizeof(SpinnerCB) == 16,
              "SpinnerCB must be 16-byte aligned for D3D11 cbuffer.");

// Constant buffer layout — must align with HLSL `cbuffer Constants`.
// HLSL packs in 16-byte rows; we pad explicitly to match.
struct CompositorCB {
    float dstSize[2];     // 8
    float fitMin[2];      // 16
    float fitSize[2];     // 24
    float borderPx;       // 28
    float pad0;           // 32
    int   hasSrc;         // 36
    int   bgMode;         // 40
    int   overlayMode;    // 44  (0 = bg-fill composite; 1 = src-over the RTV)
    int   pad1;           // 48
    float brightness;     // 52
    float borderColor[3]; // 64
};
static_assert(sizeof(CompositorCB) == 64,
              "CompositorCB must match HLSL packing exactly.");
static_assert(sizeof(CompositorCB) % 16 == 0,
              "CompositorCB must be 16-byte aligned for D3D11 cbuffer.");

} // namespace

struct D3D11Compositor::Impl {
    ComPtr<ID3D11VertexShader>   vs;
    ComPtr<ID3D11PixelShader>    ps;
    ComPtr<ID3D11PixelShader>    spinnerPs;
    ComPtr<ID3D11SamplerState>   sampler;
    ComPtr<ID3D11Buffer>         cbuf;
    ComPtr<ID3D11Buffer>         spinnerCbuf;
    ComPtr<ID3D11RasterizerState> rasterState;
    ComPtr<ID3D11BlendState>     blendState;       // opaque (BlendEnable=FALSE)
    ComPtr<ID3D11BlendState>     blendStateAlpha;  // non-premult src-over
    float                        brightness = 1.0f;
    bool initialized = false;
};

D3D11Compositor::D3D11Compositor() : m_impl(std::make_unique<Impl>()) {}

void D3D11Compositor::setBrightness(float brightness)
{
    if (m_impl) m_impl->brightness = brightness;
}
D3D11Compositor::~D3D11Compositor() { shutdown(); }
bool D3D11Compositor::isInitialized() const { return m_impl && m_impl->initialized; }

bool D3D11Compositor::initialize()
{
    if (m_impl->initialized) return true;
    if (!D3D11DeviceManager::instance().isInitialized()) {
        qCritical("D3D11Compositor::initialize: device manager not ready");
        return false;
    }
    auto *device = static_cast<ID3D11Device *>(
        D3D11DeviceManager::instance().device());

    ComPtr<ID3DBlob> vsBlob = compile(kVsHlsl, "VSMain", "vs_5_0");
    ComPtr<ID3DBlob> psBlob = compile(kPsHlsl, "PSMain", "ps_5_0");
    if (!vsBlob || !psBlob) return false;

    if (FAILED(device->CreateVertexShader(vsBlob->GetBufferPointer(),
                                            vsBlob->GetBufferSize(),
                                            nullptr, m_impl->vs.GetAddressOf()))) {
        qCritical("D3D11Compositor: CreateVertexShader failed");
        return false;
    }
    if (FAILED(device->CreatePixelShader(psBlob->GetBufferPointer(),
                                            psBlob->GetBufferSize(),
                                            nullptr, m_impl->ps.GetAddressOf()))) {
        qCritical("D3D11Compositor: CreatePixelShader failed");
        return false;
    }

    ComPtr<ID3DBlob> spinBlob = compile(kSpinnerPsHlsl, "SpinPSMain", "ps_5_0");
    if (!spinBlob) return false;
    if (FAILED(device->CreatePixelShader(spinBlob->GetBufferPointer(),
                                            spinBlob->GetBufferSize(),
                                            nullptr, m_impl->spinnerPs.GetAddressOf()))) {
        qCritical("D3D11Compositor: CreatePixelShader (spinner) failed");
        return false;
    }

    D3D11_SAMPLER_DESC sd{};
    sd.Filter   = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD   = D3D11_FLOAT32_MAX;
    if (FAILED(device->CreateSamplerState(&sd, m_impl->sampler.GetAddressOf()))) {
        qCritical("D3D11Compositor: CreateSamplerState failed");
        return false;
    }

    D3D11_BUFFER_DESC cbd{};
    cbd.ByteWidth      = sizeof(CompositorCB);
    cbd.Usage          = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device->CreateBuffer(&cbd, nullptr, m_impl->cbuf.GetAddressOf()))) {
        qCritical("D3D11Compositor: CreateBuffer (cbuf) failed");
        return false;
    }

    D3D11_BUFFER_DESC scbd{};
    scbd.ByteWidth      = sizeof(SpinnerCB);
    scbd.Usage          = D3D11_USAGE_DYNAMIC;
    scbd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    scbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device->CreateBuffer(&scbd, nullptr, m_impl->spinnerCbuf.GetAddressOf()))) {
        qCritical("D3D11Compositor: CreateBuffer (spinnerCbuf) failed");
        return false;
    }

    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    if (FAILED(device->CreateRasterizerState(&rd, m_impl->rasterState.GetAddressOf()))) {
        qCritical("D3D11Compositor: CreateRasterizerState failed");
        return false;
    }

    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].BlendEnable = FALSE;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(device->CreateBlendState(&bd, m_impl->blendState.GetAddressOf()))) {
        qCritical("D3D11Compositor: CreateBlendState failed");
        return false;
    }

    // Non-premultiplied source-over for the overlay (viewport notice
    // card) path — exact match for MetalCompositor's present blend
    // (metal_compositor.mm:419-425): RGB = src.a·src + (1-src.a)·dst;
    // alpha = src.a + (1-src.a)·dst.a (stays 1 over the opaque bg, so
    // the swapchain remains fully opaque and DComp doesn't composite
    // the desktop/blue tint through).
    D3D11_BLEND_DESC bda{};
    bda.RenderTarget[0].BlendEnable           = TRUE;
    bda.RenderTarget[0].SrcBlend              = D3D11_BLEND_SRC_ALPHA;
    bda.RenderTarget[0].DestBlend             = D3D11_BLEND_INV_SRC_ALPHA;
    bda.RenderTarget[0].BlendOp               = D3D11_BLEND_OP_ADD;
    bda.RenderTarget[0].SrcBlendAlpha         = D3D11_BLEND_ONE;
    bda.RenderTarget[0].DestBlendAlpha        = D3D11_BLEND_INV_SRC_ALPHA;
    bda.RenderTarget[0].BlendOpAlpha          = D3D11_BLEND_OP_ADD;
    bda.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(device->CreateBlendState(&bda, m_impl->blendStateAlpha.GetAddressOf()))) {
        qCritical("D3D11Compositor: CreateBlendState (alpha) failed");
        return false;
    }

    m_impl->initialized = true;
    qInfo("D3D11Compositor: initialized");
    return true;
}

void D3D11Compositor::shutdown()
{
    if (!m_impl) return;
    m_impl->blendState.Reset();
    m_impl->blendStateAlpha.Reset();
    m_impl->rasterState.Reset();
    m_impl->cbuf.Reset();
    m_impl->spinnerCbuf.Reset();
    m_impl->sampler.Reset();
    m_impl->spinnerPs.Reset();
    m_impl->ps.Reset();
    m_impl->vs.Reset();
    m_impl->initialized = false;
}

void D3D11Compositor::renderSingle(void *ctxVoid, void *srcSrvVoid,
                                     int dstW, int dstH,
                                     int srcW, int srcH,
                                     int bgMode,
                                     float borderPx,
                                     float borderR, float borderG, float borderB,
                                     bool overlayBlend)
{
    if (!m_impl || !m_impl->initialized) return;
    if (dstW <= 0 || dstH <= 0) return;

    auto *ctx    = static_cast<ID3D11DeviceContext *>(ctxVoid);
    auto *srcSrv = static_cast<ID3D11ShaderResourceView *>(srcSrvVoid);

    // Compute aspect-fit rect.
    float fitW = static_cast<float>(dstW);
    float fitH = static_cast<float>(dstH);
    if (srcSrv && srcW > 0 && srcH > 0) {
        const float srcAspect = static_cast<float>(srcW) / srcH;
        const float dstAspect = static_cast<float>(dstW) / dstH;
        if (srcAspect > dstAspect) {
            fitW = static_cast<float>(dstW);
            fitH = static_cast<float>(dstW) / srcAspect;
        } else {
            fitH = static_cast<float>(dstH);
            fitW = static_cast<float>(dstH) * srcAspect;
        }
    }
    const float fitX = (static_cast<float>(dstW) - fitW) * 0.5f;
    const float fitY = (static_cast<float>(dstH) - fitH) * 0.5f;

    // Map + update constant buffer.
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(ctx->Map(m_impl->cbuf.Get(), 0,
                          D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        return;
    }
    CompositorCB cb{};
    cb.dstSize[0] = static_cast<float>(dstW);
    cb.dstSize[1] = static_cast<float>(dstH);
    cb.fitMin[0]  = fitX;
    cb.fitMin[1]  = fitY;
    cb.fitSize[0] = fitW;
    cb.fitSize[1] = fitH;
    cb.hasSrc     = srcSrv ? 1 : 0;
    cb.bgMode     = bgMode;
    cb.overlayMode = overlayBlend ? 1 : 0;
    cb.brightness = m_impl->brightness;
    cb.borderPx       = borderPx;
    cb.borderColor[0] = borderR;
    cb.borderColor[1] = borderG;
    cb.borderColor[2] = borderB;
    std::memcpy(mapped.pData, &cb, sizeof(cb));
    ctx->Unmap(m_impl->cbuf.Get(), 0);

    // Pipeline state.
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->IASetInputLayout(nullptr);
    ctx->VSSetShader(m_impl->vs.Get(), nullptr, 0);
    ctx->PSSetShader(m_impl->ps.Get(), nullptr, 0);
    ID3D11Buffer *cb0 = m_impl->cbuf.Get();
    ctx->PSSetConstantBuffers(0, 1, &cb0);
    ID3D11SamplerState *s0 = m_impl->sampler.Get();
    ctx->PSSetSamplers(0, 1, &s0);
    ID3D11ShaderResourceView *srvs[1] = { srcSrv };
    ctx->PSSetShaderResources(0, 1, srvs);
    ctx->RSSetState(m_impl->rasterState.Get());
    const float blendFactor[4] = {0, 0, 0, 0};
    ctx->OMSetBlendState(
        overlayBlend ? m_impl->blendStateAlpha.Get() : m_impl->blendState.Get(),
        blendFactor, 0xFFFFFFFF);

    ctx->Draw(3, 0);

    // Unbind the source SRV so we can use the same slot as an upload
    // destination on the next frame without a hazard warning.
    ID3D11ShaderResourceView *nullSrv[1] = { nullptr };
    ctx->PSSetShaderResources(0, 1, nullSrv);
}

void D3D11Compositor::renderCornerOverlay(void *ctxVoid, void *srcSrvVoid,
                                            int srcW, int srcH,
                                            int dstW, int dstH,
                                            int corner,
                                            float overlayFrac,
                                            float marginPx)
{
    if (!m_impl || !m_impl->initialized) return;
    if (!ctxVoid || !srcSrvVoid) return;
    if (dstW <= 0 || dstH <= 0) return;
    if (srcW <= 0 || srcH <= 0) return;

    overlayFrac = std::clamp(overlayFrac, 0.05f, 0.5f);
    if (marginPx < 0.0f) marginPx = 0.0f;

    // Box width = fraction of canvas width; height follows the source
    // aspect (landscape thumbs get a wider box). Cap height at the box
    // width so a tall portrait thumb is never taller than it is wide —
    // the source then pillarboxes inside the resulting square box.
    const float boxW = static_cast<float>(dstW) * overlayFrac;
    const float aspect =
        static_cast<float>(srcW) / static_cast<float>(std::max(srcH, 1));
    float boxH = boxW / std::max(aspect, 0.0001f);
    if (boxH > boxW) boxH = boxW;

    // D3D11 viewport origin = top-left; corner=0 (BL) and corner=1
    // (BR) sit at the bottom of the canvas with marginPx inset.
    float originX = marginPx;
    float originY = static_cast<float>(dstH) - boxH - marginPx;
    if (corner == 1) {
        originX = static_cast<float>(dstW) - boxW - marginPx;
    } else if (corner == 2) { // center (viewport notice card)
        originX = (static_cast<float>(dstW) - boxW) * 0.5f;
        originY = (static_cast<float>(dstH) - boxH) * 0.5f;
    }
    if (originX < 0.0f) originX = 0.0f;
    if (originY < 0.0f) originY = 0.0f;

    auto *ctx = static_cast<ID3D11DeviceContext *>(ctxVoid);

    // Save caller's viewport so downstream passes (screenshot,
    // present) see the canvas extent they expect.
    UINT prevCount = 1;
    D3D11_VIEWPORT prev{};
    ctx->RSGetViewports(&prevCount, &prev);

    D3D11_VIEWPORT vp{};
    vp.TopLeftX = originX;
    vp.TopLeftY = originY;
    vp.Width    = boxW;
    vp.Height   = boxH;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    ctx->RSSetViewports(1, &vp);

    // Reuse renderSingle: it computes its own aspect-fit math with
    // the box dims as the canvas, draws the same fullscreen-triangle
    // (rasterizer clips to the corner viewport), and unbinds the SRV
    // afterward. A faint grey frame (borderPx > 0) keeps the thumb
    // from blending into the viewport behind it.
    renderSingle(ctxVoid, srcSrvVoid,
                 static_cast<int>(boxW), static_cast<int>(boxH),
                 srcW, srcH,
                 /*bgMode=*/0,
                 // No compositor frame for the centered notice card
                 // (corner==2): it carries its own rounded border in the
                 // source image; a second rectangular frame reads as a
                 // doubled border. Hover thumbs (0/1) keep the 2 px frame.
                 /*borderPx=*/(corner == 2 ? 0.0f : 2.0f),
                 /*borderRGB ≈ #474747=*/0.28f, 0.28f, 0.28f,
                 // Notice card (corner==2): src-over the live viewport
                 // background so its rounded corners stay transparent —
                 // matches macOS. Hover thumbs stay opaque tiles.
                 /*overlayBlend=*/(corner == 2));

    ctx->RSSetViewports(1, &prev);
}

void D3D11Compositor::renderSpinner(void *ctxVoid, int dstW, int dstH,
                                     float timeSeconds)
{
    if (!m_impl || !m_impl->initialized) return;
    if (!ctxVoid || dstW <= 0 || dstH <= 0) return;
    auto *ctx = static_cast<ID3D11DeviceContext *>(ctxVoid);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(ctx->Map(m_impl->spinnerCbuf.Get(), 0,
                          D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        return;
    }
    SpinnerCB cb{};
    cb.dstSize[0] = static_cast<float>(dstW);
    cb.dstSize[1] = static_cast<float>(dstH);
    cb.time       = timeSeconds;
    std::memcpy(mapped.pData, &cb, sizeof(cb));
    ctx->Unmap(m_impl->spinnerCbuf.Get(), 0);

    // Fullscreen triangle, opaque fill — no source texture/sampler.
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->IASetInputLayout(nullptr);
    ctx->VSSetShader(m_impl->vs.Get(), nullptr, 0);
    ctx->PSSetShader(m_impl->spinnerPs.Get(), nullptr, 0);
    ID3D11Buffer *cb0 = m_impl->spinnerCbuf.Get();
    ctx->PSSetConstantBuffers(0, 1, &cb0);
    ctx->RSSetState(m_impl->rasterState.Get());
    const float blendFactor[4] = {0, 0, 0, 0};
    ctx->OMSetBlendState(m_impl->blendState.Get(), blendFactor, 0xFFFFFFFF);
    ctx->Draw(3, 0);
}

} // namespace qcv
