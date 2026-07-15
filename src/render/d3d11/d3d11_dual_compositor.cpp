#include "d3d11_dual_compositor.h"
#include "d3d11_device_manager.h"
#include "d3d11_vulkan_decode_bridge.h"
#include "d3d11_vulkan_yuv_compositor.h"

#include "render/i_dual_frame_source.h"
#include "render/iplayer_renderer.h"   // CompositorMode enum

#include <array>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

// Forward decl so we can cast IDualFrameSource's opaque
// avFrameOwning pointer into the bridge's consumeAVFrame entry
// without dragging FFmpeg headers into this compositor.
struct AVFrame;

#include <QtLogging>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace qcv {

using Microsoft::WRL::ComPtr;

namespace {

// Vertex: fullscreen triangle via SV_VertexID (Draw(3, 0)). UV (0,0)
// at top-left, (1,1) at bottom-right — matches the Metal compositor's
// 6-vertex topology after Y-flip. Same trick as D3D11Compositor.
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

// Pixel: verbatim port of dual_compositor.mm:81-136 (Metal). Three
// branches on `mode`:
//   0 = Single — A fills the canvas, aspect-fit, aActive gates.
//   1 = SBS    — A on the left half, B on the right half, each
//                aspect-fit inside its half. Per-side active flags.
//   2 = Wipe   — A draws where uv.x < splitPos, B where >= . Both
//                aspect-fit to full canvas. 1-pixel opaque white
//                splitter line.
// sampleFit returns float4(0) for pixels outside the side's dst
// rect OR outside the aspect-fit source rectangle (letterbox).
// discard at the end means the cleared canvas (alpha=0) survives;
// the background pass after this compositor fills those regions.
constexpr const char *kPsHlsl = R"(
cbuffer Constants : register(b0) {
    float2 dstSize;       // 8
    float2 srcSizeA;      // 16
    float2 srcSizeB;      // 24
    int    rotA;          // 28 — per-side display rotation, quarter-
    int    rotB;          // 32   turns CW {0..3}
    int    mode;          // 36
    float  splitPos;      // 40
    int    aActive;       // 44
    int    bActive;       // 48
    float  seamHighlight; // 52 — split seam: 0 = faint grey, 1 = white
    float  diffGain;      // 56 — Difference mode: amplify abs(A-B)
};

Texture2D    srcA : register(t0);
Texture2D    srcB : register(t1);
SamplerState smp  : register(s0);

struct VsOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

float4 sampleFit(Texture2D src, SamplerState s,
                 float2 dstPx, float2 rectOrigin,
                 float2 rectSize, float2 srcSize, int rot)
{
    float2 rectPxPos = dstPx - rectOrigin;
    if (rectPxPos.x < 0.0 || rectPxPos.x >= rectSize.x ||
        rectPxPos.y < 0.0 || rectPxPos.y >= rectSize.y) {
        return float4(0, 0, 0, 0);
    }
    float scale = min(rectSize.x / srcSize.x, rectSize.y / srcSize.y);
    float2 scaledSrc = srcSize * scale;
    float2 offset    = (rectSize - scaledSrc) * 0.5;
    float2 inSrcPx   = (rectPxPos - offset) / scale;
    if (inSrcPx.x < 0.0 || inSrcPx.x >= srcSize.x ||
        inSrcPx.y < 0.0 || inSrcPx.y >= srcSize.y) {
        return float4(0, 0, 0, 0);
    }
    // srcSize arrives display-swapped for odd quarter-turns; inverse-
    // rotate the normalized display UV back onto the stored texture.
    float2 t = inSrcPx / srcSize;
    if (rot == 1)      t = float2(t.y, 1.0 - t.x);          //  90 CW
    else if (rot == 2) t = float2(1.0 - t.x, 1.0 - t.y);    // 180
    else if (rot == 3) t = float2(1.0 - t.y, t.x);          // 270 CW
    return src.Sample(s, t);
}

float4 PSMain(VsOut input) : SV_TARGET
{
    float2 uv    = input.uv;
    float2 dstPx = uv * dstSize;

    float4 color = float4(0, 0, 0, 0);

    if (mode == 1) {
        float halfW = dstSize.x * 0.5;
        if (dstPx.x < halfW) {
            color = (aActive == 0)
                ? float4(0, 0, 0, 0)
                : sampleFit(srcA, smp, dstPx, float2(0, 0),
                            float2(halfW, dstSize.y), srcSizeA, rotA);
        } else {
            color = (bActive == 0)
                ? float4(0, 0, 0, 0)
                : sampleFit(srcB, smp, dstPx, float2(halfW, 0),
                            float2(halfW, dstSize.y), srcSizeB, rotB);
        }
        // Faint grey divider between the two sides.
        if (abs(uv.x - 0.5) * dstSize.x < 1.0) {
            return float4(0.4, 0.4, 0.4, 1.0);
        }
    } else if (mode == 2) {
        if (uv.x < splitPos) {
            color = (aActive == 0)
                ? float4(0, 0, 0, 0)
                : sampleFit(srcA, smp, dstPx, float2(0, 0),
                            dstSize, srcSizeA, rotA);
        } else {
            color = (bActive == 0)
                ? float4(0, 0, 0, 0)
                : sampleFit(srcB, smp, dstPx, float2(0, 0),
                            dstSize, srcSizeB, rotB);
        }
        // Split seam — faint grey at rest, full white on hover/drag.
        if (abs(uv.x - splitPos) * dstSize.x < 1.0) {
            float3 seam = lerp(float3(0.4, 0.4, 0.4),
                               float3(1.0, 1.0, 1.0),
                               saturate(seamHighlight));
            return float4(seam, 1.0);
        }
    } else if (mode == 3) {
        // Difference (Adobe-style) — both sides fit the FULL canvas,
        // output |A - B| per channel * gain. Aligned same-res pair -> black.
        float4 ca = (aActive == 0)
            ? float4(0, 0, 0, 0)
            : sampleFit(srcA, smp, dstPx, float2(0, 0), dstSize, srcSizeA,
                        rotA);
        float4 cb = (bActive == 0)
            ? float4(0, 0, 0, 0)
            : sampleFit(srcB, smp, dstPx, float2(0, 0), dstSize, srcSizeB,
                        rotB);
        color = float4(abs(ca.rgb - cb.rgb) * diffGain, max(ca.a, cb.a));
    } else {
        color = (aActive == 0)
            ? float4(0, 0, 0, 0)
            : sampleFit(srcA, smp, dstPx, float2(0, 0),
                        dstSize, srcSizeA, rotA);
    }

    if (color.a == 0.0) discard;
    return color;
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
        qCritical("D3D11DualCompositor: D3DCompile %s failed (hr=0x%08lX)\n%s",
                  entry, static_cast<unsigned long>(hr),
                  errors ? static_cast<const char *>(errors->GetBufferPointer())
                         : "(no error blob)");
        return nullptr;
    }
    return code;
}

struct DualCB {
    float dstSize[2];     // 0  : 8
    float srcSizeA[2];    // 8  : 16
    float srcSizeB[2];    // 16 : 24
    int   rotA;           // 24 : 28 — quarter-turns CW
    int   rotB;           // 28 : 32
    int   mode;           // 32 : 36
    float splitPos;       // 36 : 40
    int   aActive;        // 40 : 44
    int   bActive;        // 44 : 48
    float seamHighlight;  // 48 : 52
    float diffGain;       // 52 : 56
    float pad1[2];        // 56 : 64
};
static_assert(sizeof(DualCB) == 64,
              "DualCB must match HLSL packing exactly.");
static_assert(sizeof(DualCB) % 16 == 0,
              "DualCB must be 16-byte aligned for D3D11 cbuffer.");

// Big-seek threshold (matches dual_compositor.mm:495). Frame deltas
// > this drop the last-good-frame caches.
constexpr int kSeekJumpThreshold = 16;

// F.2.12.e — per-slot ownership enum mirroring
// D3D11PlayerRenderer::Impl::SlotOwner (d3d11_player_renderer.cpp:147).
// Cpu owns its own texture+SRV via CreateTexture2D + CreateSRV.
// Bridge holds an AddRef'd reference to a bridge-owned SRV; no
// texture allocation on the compositor side (the bridge does that).
enum class SlotOwner { None, Cpu, Bridge };

struct CachedSide {
    SlotOwner    owner   = SlotOwner::None;
    ComPtr<ID3D11Texture2D>          texture;   // Cpu kind only
    ComPtr<ID3D11ShaderResourceView> srv;       // both kinds (AddRef'd for Bridge)
    int          width  = 0;
    int          height = 0;
    DXGI_FORMAT  format = DXGI_FORMAT_UNKNOWN;  // Cpu kind only

    void reset() {
        srv.Reset();
        texture.Reset();
        owner  = SlotOwner::None;
        width  = height = 0;
        format = DXGI_FORMAT_UNKNOWN;
    }

    bool fitsCpu(int w, int h, DXGI_FORMAT f) const {
        return owner == SlotOwner::Cpu && texture && srv
            && width == w && height == h && format == f;
    }

    bool recreateCpu(ID3D11Device *device, int w, int h, DXGI_FORMAT f) {
        reset();
        D3D11_TEXTURE2D_DESC td{};
        td.Width            = static_cast<UINT>(w);
        td.Height           = static_cast<UINT>(h);
        td.MipLevels        = 1;
        td.ArraySize        = 1;
        td.Format           = f;
        td.SampleDesc.Count = 1;
        td.Usage            = D3D11_USAGE_DEFAULT;
        td.BindFlags        = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(device->CreateTexture2D(&td, nullptr,
                                             texture.GetAddressOf()))) {
            qWarning("D3D11DualCompositor: CreateTexture2D %dx%d failed", w, h);
            return false;
        }
        if (FAILED(device->CreateShaderResourceView(texture.Get(), nullptr,
                                                     srv.GetAddressOf()))) {
            qWarning("D3D11DualCompositor: CreateSRV failed");
            texture.Reset();
            return false;
        }
        owner  = SlotOwner::Cpu;
        width  = w;
        height = h;
        format = f;
        return true;
    }

    // Adopt a bridge-owned SRV. ComPtr assignment from raw pointer
    // AddRefs; the previous slot's resources (whichever owner) get
    // released as ComPtrs are reassigned.
    void adoptBridgeSrv(ID3D11ShaderResourceView *bridgeSrv, int w, int h) {
        texture.Reset();        // bridge owns its own texture
        srv = bridgeSrv;        // AddRef via ComPtr=
        owner  = SlotOwner::Bridge;
        width  = w;
        height = h;
        format = DXGI_FORMAT_UNKNOWN;
    }
};

DXGI_FORMAT formatFor(DualFramePayload::Kind k)
{
    switch (k) {
    case DualFramePayload::Kind::CpuRgba8:     return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DualFramePayload::Kind::CpuRgba16F:   return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DualFramePayload::Kind::VulkanShared: return DXGI_FORMAT_UNKNOWN;  // bridge picks
    case DualFramePayload::Kind::Empty:        return DXGI_FORMAT_UNKNOWN;
    }
    return DXGI_FORMAT_UNKNOWN;
}

} // namespace

struct D3D11DualCompositor::Impl {
    ComPtr<ID3D11VertexShader>   vs;
    ComPtr<ID3D11PixelShader>    ps;
    ComPtr<ID3D11SamplerState>   sampler;
    ComPtr<ID3D11Buffer>         cbuf;
    ComPtr<ID3D11RasterizerState> rasterState;
    ComPtr<ID3D11BlendState>     blendState;

    CachedSide cachedA;
    CachedSide cachedB;

    // F.2.13 — per-slot Vulkan-decode bridges. Each holds only the
    // per-stream cache (output texture + viewCache + format/dim);
    // the heavy compute pipeline lives in `externalYuv` and is
    // shared across both slots. Mirrors macOS
    // `DualPixbufConverterImpl::bridges[2]` + shared `yuv`. Lazy-
    // initialized on first VulkanShared payload.
    std::array<D3D11VulkanDecodeBridge, 2> bridges;

    // Renderer-owned shared compute pipeline. Non-owning. Must
    // outlive `bridges`.
    D3D11VulkanYuvCompositor *externalYuv = nullptr;

    IDualFrameSource *source = nullptr;

    int   mode     = 0;     // Single by default
    float splitPos = 0.5f;
    float seamHighlight = 0.0f;   // split seam: 0 grey at rest, 1 white on hover/drag
    float diffGain = 1.0f;        // Difference mode: amplify abs(A-B)
    // Per-side pixel aspect (anamorphic un-squeeze). Widens the
    // effective srcSize fed to the shader; 1/1 = square (default).
    int   parNumA = 1, parDenA = 1;
    int   parNumB = 1, parDenB = 1;
    // Per-side display rotation in quarter-turns CW {0..3}. Swaps the
    // effective srcSize for odd quarters; the shader inverse-rotates
    // its sampling to match.
    int   rotQA = 0, rotQB = 0;

    // Prepare → render handoff state. Set in prepareFrames, consumed
    // (and cleared) in renderFrame.
    bool  prepared       = false;
    bool  aActive        = false;
    bool  bActive        = false;
    int   srcAW          = 1;
    int   srcAH          = 1;
    int   srcBW          = 1;
    int   srcBH          = 1;

    // Cache invalidation seams (dual_compositor.mm:484-504).
    int   cachedGeneration       = -1;
    int   lastRenderedMasterFrame = -1;

    bool  initialized = false;
};

D3D11DualCompositor::D3D11DualCompositor() : m_impl(std::make_unique<Impl>()) {}
D3D11DualCompositor::~D3D11DualCompositor() { shutdown(); }

bool D3D11DualCompositor::isInitialized() const
{
    return m_impl && m_impl->initialized;
}

bool D3D11DualCompositor::initialize()
{
    if (m_impl->initialized) return true;
    if (!D3D11DeviceManager::instance().isInitialized()) {
        qCritical("D3D11DualCompositor::initialize: device manager not ready");
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
        qCritical("D3D11DualCompositor: CreateVertexShader failed");
        return false;
    }
    if (FAILED(device->CreatePixelShader(psBlob->GetBufferPointer(),
                                            psBlob->GetBufferSize(),
                                            nullptr, m_impl->ps.GetAddressOf()))) {
        qCritical("D3D11DualCompositor: CreatePixelShader failed");
        return false;
    }

    D3D11_SAMPLER_DESC sd{};
    sd.Filter   = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD   = D3D11_FLOAT32_MAX;
    if (FAILED(device->CreateSamplerState(&sd, m_impl->sampler.GetAddressOf()))) {
        qCritical("D3D11DualCompositor: CreateSamplerState failed");
        return false;
    }

    D3D11_BUFFER_DESC cbd{};
    cbd.ByteWidth      = sizeof(DualCB);
    cbd.Usage          = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device->CreateBuffer(&cbd, nullptr, m_impl->cbuf.GetAddressOf()))) {
        qCritical("D3D11DualCompositor: CreateBuffer (cbuf) failed");
        return false;
    }

    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    if (FAILED(device->CreateRasterizerState(&rd, m_impl->rasterState.GetAddressOf()))) {
        qCritical("D3D11DualCompositor: CreateRasterizerState failed");
        return false;
    }

    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].BlendEnable = FALSE;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(device->CreateBlendState(&bd, m_impl->blendState.GetAddressOf()))) {
        qCritical("D3D11DualCompositor: CreateBlendState failed");
        return false;
    }

    m_impl->initialized = true;
    qInfo("D3D11DualCompositor: initialized");
    return true;
}

void D3D11DualCompositor::shutdown()
{
    if (!m_impl) return;
    m_impl->cachedA.reset();
    m_impl->cachedB.reset();
    m_impl->bridges[0].shutdown();
    m_impl->bridges[1].shutdown();
    m_impl->blendState.Reset();
    m_impl->rasterState.Reset();
    m_impl->cbuf.Reset();
    m_impl->sampler.Reset();
    m_impl->ps.Reset();
    m_impl->vs.Reset();
    m_impl->prepared = false;
    m_impl->cachedGeneration = -1;
    m_impl->lastRenderedMasterFrame = -1;
    m_impl->initialized = false;
}

void D3D11DualCompositor::setFrameSource(IDualFrameSource *source)
{
    if (!m_impl) return;
    if (m_impl->source == source) return;
    m_impl->source = source;
    // Drop caches when source swaps — they're sized for previous content.
    m_impl->cachedA.reset();
    m_impl->cachedB.reset();
    m_impl->cachedGeneration = -1;
    m_impl->lastRenderedMasterFrame = -1;
    m_impl->prepared = false;
}

void D3D11DualCompositor::setExternalYuvCompositor(D3D11VulkanYuvCompositor *yuv)
{
    if (!m_impl) return;
    if (m_impl->externalYuv == yuv) return;
    // Drop bridges if the compositor swaps under us — they hold a
    // non-owning pointer that would dangle otherwise. In practice the
    // renderer only calls this once at first DualFlow entry, but a
    // detach-then-reattach (mode toggle) still needs to be safe.
    m_impl->bridges[0].shutdown();
    m_impl->bridges[1].shutdown();
    m_impl->cachedA.reset();
    m_impl->cachedB.reset();
    m_impl->externalYuv = yuv;
}

void D3D11DualCompositor::setMode(CompositorMode mode)
{
    if (!m_impl) return;
    m_impl->mode = static_cast<int>(mode);
}

void D3D11DualCompositor::setSplitPos(float pos)
{
    if (!m_impl) return;
    m_impl->splitPos = std::clamp(pos, 0.0f, 1.0f);
}

void D3D11DualCompositor::setSeamHighlight(float h)
{
    if (!m_impl) return;
    m_impl->seamHighlight = std::clamp(h, 0.0f, 1.0f);
}

void D3D11DualCompositor::setDiffGain(float g)
{
    if (!m_impl) return;
    m_impl->diffGain = g;
}

void D3D11DualCompositor::setPixelAspectA(int num, int den)
{
    if (!m_impl) return;
    m_impl->parNumA = num > 0 ? num : 1;
    m_impl->parDenA = den > 0 ? den : 1;
}

void D3D11DualCompositor::setPixelAspectB(int num, int den)
{
    if (!m_impl) return;
    m_impl->parNumB = num > 0 ? num : 1;
    m_impl->parDenB = den > 0 ? den : 1;
}

void D3D11DualCompositor::setRotationA(int quarters)
{
    if (!m_impl) return;
    m_impl->rotQA = quarters & 3;
}

void D3D11DualCompositor::setRotationB(int quarters)
{
    if (!m_impl) return;
    m_impl->rotQB = quarters & 3;
}

void D3D11DualCompositor::prepareFrames(void *ctxVoid)
{
    if (!m_impl) return;
    m_impl->prepared = false;
    if (!m_impl->initialized || !m_impl->source) return;

    auto *ctx = static_cast<ID3D11DeviceContext *>(ctxVoid);
    if (!ctx) return;
    auto *device = static_cast<ID3D11Device *>(
        D3D11DeviceManager::instance().device());
    if (!device) return;

    IDualFrameSource *src = m_impl->source;
    const int masterFrame = src->currentFrame();

    // Drop cache on timeline edit (timelineChanged → gen bump in
    // DualPlaybackController::setTimeline). EXCEPT during a live
    // scrub/edit session: slip/trim bumps the generation every drag
    // delta, and dropping the last-good cache each tick flickers black
    // on any null pull (scrub frame not yet decoded / transient gap).
    // Keep the cache as the fallback during scrub; just sync the marker.
    const int curGen = src->timelineGeneration();
    if (curGen != m_impl->cachedGeneration) {
        if (!src->isScrubbing()) {
            m_impl->cachedA.reset();
            m_impl->cachedB.reset();
        }
        m_impl->cachedGeneration = curGen;
    }

    // Drop cache on big seek (dual_compositor.mm:495).
    // H.5 (2026-05-14) — loop-wrap detection. A big NEGATIVE jump
    // from near loopOut to near loopIn is a wrap, not a seek; the
    // last-good-frame hold should survive it so the renderer
    // displays the pre-wrap frame while the source's cache catches
    // up on the loop-start frames. Otherwise: dropped cache + cache
    // miss on pull = blank viewport until cache catches up (visible
    // on Windows render-on-demand; masked on macOS vsync polling).
    bool isLoopWrap = false;
    if (m_impl->lastRenderedMasterFrame >= 0) {
        int loIn = 0, loOut = 0;
        if (src->loopRange(&loIn, &loOut) && loOut > loIn) {
            const int prev = m_impl->lastRenderedMasterFrame;
            // Forward wrap: prev near loOut, new near loIn.
            const bool fwdWrap =
                (prev  >= loOut - kSeekJumpThreshold) &&
                (masterFrame <= loIn  + kSeekJumpThreshold) &&
                (masterFrame <  prev);
            isLoopWrap = fwdWrap;
        }
    }
    if (m_impl->lastRenderedMasterFrame >= 0
        && std::abs(masterFrame - m_impl->lastRenderedMasterFrame)
           > kSeekJumpThreshold
        && !isLoopWrap) {
        m_impl->cachedA.reset();
        m_impl->cachedB.reset();
    }
    m_impl->lastRenderedMasterFrame = masterFrame;

    const bool aPastEnd = src->aPastEnd(masterFrame);
    const bool bPastEnd = src->bPastEnd(masterFrame);

    // Past-end → drop the cached frame so the side renders transparent
    // rather than holding a stale pixel (dual_compositor.mm:512-519).
    if (aPastEnd) m_impl->cachedA.reset();
    if (bPastEnd) m_impl->cachedB.reset();

    auto resolveSide = [&](IDualFrameSource *frameSrc, bool pastEnd, int slot,
                            CachedSide &cache,
                            D3D11VulkanDecodeBridge &bridge,
                            int &outW, int &outH) {
        if (pastEnd) return;
        DualFramePayload payload =
            (slot == 0) ? frameSrc->pullA(masterFrame)
                        : frameSrc->pullB(masterFrame);
        if (payload.kind == DualFramePayload::Kind::Empty) return;

        // F.2.13 — Vulkan zero-copy path. The bridge is now per-slot
        // (only output texture + viewCache + format/dim); the heavy
        // compute pipeline lives in `externalYuv` and is shared
        // across both slots. Mirrors macOS's per-slot CvPixbufMetalBridge
        // + shared MetalYuvRenderer.
        if (payload.kind == DualFramePayload::Kind::VulkanShared) {
            if (!payload.avFrameOwning) return;
            if (!m_impl->externalYuv || !m_impl->externalYuv->isInitialized()) {
                // Renderer didn't inject a yuv compositor or it failed
                // to init — Vulkan frames must be dropped this run.
                return;
            }
            if (!bridge.isInitialized()) {
                if (!bridge.initialize(m_impl->externalYuv)) {
                    qWarning("D3D11DualCompositor: bridge init failed for "
                             "slot %d — Vulkan frames will be dropped", slot);
                    return;
                }
            }
            // payload.rangeOverride mirrors the per-side Inspector
            // Range pill (0 = Auto, 1 = Full, 2 = Limited). Adapter
            // populates from the per-side source MediaItem; the
            // bridge applies it inside the YUV→RGB compute.
            auto *imp = bridge.consumeAVFrame(
                static_cast<AVFrame *>(payload.avFrameOwning),
                payload.rangeOverride);
            if (!imp || imp->planes.empty()) return;
            const auto &p = imp->planes.front();
            if (!p.srv) return;
            cache.adoptBridgeSrv(p.srv, imp->pictureWidth, imp->pictureHeight);
            outW = imp->pictureWidth;
            outH = imp->pictureHeight;
            return;
        }

        // CPU path.
        if (!payload.cpuBits || payload.width <= 0 || payload.height <= 0) return;
        const DXGI_FORMAT fmt = formatFor(payload.kind);
        if (fmt == DXGI_FORMAT_UNKNOWN) return;

        if (!cache.fitsCpu(payload.width, payload.height, fmt)) {
            if (!cache.recreateCpu(device, payload.width, payload.height, fmt)) {
                return;
            }
        }
        ctx->UpdateSubresource(cache.texture.Get(), 0, nullptr,
                                payload.cpuBits,
                                static_cast<UINT>(payload.cpuStride), 0);
        outW = payload.width;
        outH = payload.height;
        // payload's keepAlive (shared_ptr<DualFrame>) drops here as
        // the local goes out of scope. UpdateSubresource has already
        // copied the bytes; the source frame is free.
    };

    int srcAW = m_impl->cachedA.width;
    int srcAH = m_impl->cachedA.height;
    int srcBW = m_impl->cachedB.width;
    int srcBH = m_impl->cachedB.height;

    resolveSide(src, aPastEnd, /*slot=*/0, m_impl->cachedA, m_impl->bridges[0],
                srcAW, srcAH);
    resolveSide(src, bPastEnd, /*slot=*/1, m_impl->cachedB, m_impl->bridges[1],
                srcBW, srcBH);

    m_impl->aActive = !aPastEnd && m_impl->cachedA.srv.Get() != nullptr;
    m_impl->bActive = !bPastEnd && m_impl->cachedB.srv.Get() != nullptr;
    m_impl->srcAW   = std::max(1, srcAW);
    m_impl->srcAH   = std::max(1, srcAH);
    m_impl->srcBW   = std::max(1, srcBW);
    m_impl->srcBH   = std::max(1, srcBH);
    m_impl->prepared = true;
}

void D3D11DualCompositor::renderFrame(void *ctxVoid, int dstW, int dstH)
{
    if (!m_impl || !m_impl->initialized) return;
    if (dstW <= 0 || dstH <= 0) return;
    if (!m_impl->prepared) return;

    auto *ctx = static_cast<ID3D11DeviceContext *>(ctxVoid);
    if (!ctx) return;

    // Update constant buffer.
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(ctx->Map(m_impl->cbuf.Get(), 0,
                          D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        m_impl->prepared = false;
        return;
    }
    DualCB cb{};
    cb.dstSize[0]  = static_cast<float>(dstW);
    cb.dstSize[1]  = static_cast<float>(dstH);
    // Pixel-aspect un-squeeze — widen the effective srcSize so the
    // shader's per-side sampleFit lands on the corrected display
    // aspect (it normalizes the sample by srcSize, so this stretches
    // the real texture with no shader change). 1/1 = unchanged.
    auto effDim = [](int w, int h, int n, int d) -> std::pair<int, int> {
        if (w <= 0 || h <= 0 || n <= 0 || d <= 0 || n == d) return { w, h };
        if (n > d)
            return { static_cast<int>(std::lround(double(w) * n / d)), h };
        return { w, static_cast<int>(std::lround(double(h) * d / n)) };
    };
    const auto [effAW, effAH] = effDim(m_impl->srcAW, m_impl->srcAH,
                                       m_impl->parNumA, m_impl->parDenA);
    const auto [effBW, effBH] = effDim(m_impl->srcBW, m_impl->srcBH,
                                       m_impl->parNumB, m_impl->parDenB);
    // Display rotation: un-squeeze first (stored space), then swap
    // the effective dims for odd quarter-turns so each side's fit
    // frames the upright image. The shader inverse-rotates sampling.
    const int rotA = m_impl->rotQA;
    const int rotB = m_impl->rotQB;
    cb.srcSizeA[0] = static_cast<float>((rotA & 1) ? effAH : effAW);
    cb.srcSizeA[1] = static_cast<float>((rotA & 1) ? effAW : effAH);
    cb.srcSizeB[0] = static_cast<float>((rotB & 1) ? effBH : effBW);
    cb.srcSizeB[1] = static_cast<float>((rotB & 1) ? effBW : effBH);
    cb.rotA        = rotA;
    cb.rotB        = rotB;
    cb.mode        = m_impl->mode;
    cb.splitPos    = m_impl->splitPos;
    cb.aActive     = m_impl->aActive ? 1 : 0;
    cb.bActive     = m_impl->bActive ? 1 : 0;
    cb.seamHighlight = m_impl->seamHighlight;
    cb.diffGain    = m_impl->diffGain;
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

    // Either SRV may be null — HLSL Sample on a NULL SRV returns 0,
    // and our shader's aActive/bActive flags discard those sides
    // before reaching the sample anyway. No C++-side mirror needed
    // (the mirror in dual_compositor.mm:689-690 is a Metal-API
    // requirement that doesn't apply to D3D11 — see guide 23 §2.1).
    ID3D11ShaderResourceView *srvs[2] = {
        m_impl->cachedA.srv.Get(),
        m_impl->cachedB.srv.Get(),
    };
    ctx->PSSetShaderResources(0, 2, srvs);

    ctx->RSSetState(m_impl->rasterState.Get());
    const float blendFactor[4] = {0, 0, 0, 0};
    ctx->OMSetBlendState(m_impl->blendState.Get(), blendFactor, 0xFFFFFFFF);

    ctx->Draw(3, 0);

    // Unbind SRVs so the next frame's UpdateSubresource on the same
    // textures doesn't trigger a hazard warning.
    ID3D11ShaderResourceView *nullSrvs[2] = { nullptr, nullptr };
    ctx->PSSetShaderResources(0, 2, nullSrvs);

    // Consume the prepared state — next frame must call prepareFrames
    // again before renderFrame is valid.
    m_impl->prepared = false;
}

} // namespace qcv
