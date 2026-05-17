// D3D11HdrSwapchain — see header for HDR mode mapping + threading
// notes.

#include "d3d11_hdr_swapchain.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dxgi1_2.h>
#include <dxgi1_4.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <QtLogging>

#include <atomic>
#include <mutex>

namespace qcv {

using Microsoft::WRL::ComPtr;

namespace {

struct ModeMapping {
    DXGI_FORMAT      format;
    DXGI_COLOR_SPACE_TYPE colorSpace;
    const char      *name;
};

ModeMapping mappingFor(HdrMode mode)
{
    switch (mode) {
        case HdrMode::SdrSRgb:
            return { DXGI_FORMAT_B8G8R8A8_UNORM,
                     DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709,
                     "SdrSRgb" };
        case HdrMode::SdrDisplayP3:
            // DXGI has no P3-tagged SDR colorspace; the OS handles
            // P3 display profiles transparently when format is sRGB
            // 8-bit. Falls back to sRGB SDR encoding.
            return { DXGI_FORMAT_B8G8R8A8_UNORM,
                     DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709,
                     "SdrDisplayP3 (→sRGB SDR)" };
        case HdrMode::ExtendedLinearSRgb:
            return { DXGI_FORMAT_R16G16B16A16_FLOAT,
                     DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709,
                     "ExtendedLinearSRgb (scRGB)" };
        case HdrMode::ExtendedLinearDisplayP3:
            // Same scRGB linear primaries — Windows doesn't expose
            // a P3-tagged float swapchain.
            return { DXGI_FORMAT_R16G16B16A16_FLOAT,
                     DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709,
                     "ExtendedLinearDisplayP3 (→scRGB)" };
        case HdrMode::Hdr10:
            // Phase F.2.9 — real HDR10 PQ path. 10-bit unorm + BT.2020
            // primaries + ST.2084 EOTF. Pairs with SetHDRMetaData below
            // to advertise mastering display to the OS / display.
            return { DXGI_FORMAT_R10G10B10A2_UNORM,
                     DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020,
                     "HDR10 PQ (BT.2020 ST.2084)" };
    }
    return { DXGI_FORMAT_B8G8R8A8_UNORM,
             DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709,
             "SdrSRgb (fallback)" };
}

} // namespace

struct D3D11HdrSwapchain::Impl {
    IDXGIFactory2          *factory     = nullptr;   // borrowed
    ID3D11Device           *device      = nullptr;   // borrowed
    IDCompositionDevice    *dcomp       = nullptr;   // borrowed
    HWND                    childHwnd   = nullptr;

    ComPtr<IDXGISwapChain1>          swapchain;
    ComPtr<ID3D11RenderTargetView>   rtv;
    ComPtr<IDCompositionTarget>      dcompTarget;
    ComPtr<IDCompositionVisual>      dcompVisual;

    HdrMode                appliedMode = HdrMode::SdrSRgb;
    std::atomic<int>       pendingMode{static_cast<int>(HdrMode::SdrSRgb)};

    int                    width  = 0;
    int                    height = 0;

    // Guards swapchain mutation (recreate). resize() + apply both
    // take it so a stray window-resize while we're recreating doesn't
    // get a half-built rtv.
    std::mutex             mutateMutex;

    bool                   initialized = false;

    // Cached capability — queried once in initialize (and again on
    // mode change). Output may change if user drags window to a
    // different monitor; refreshed best-effort.
    bool                   hdrSupported = false;
    float                  maxHeadroom  = 1.0f;
};

D3D11HdrSwapchain::D3D11HdrSwapchain()
    : m_impl(std::make_unique<Impl>()) {}

D3D11HdrSwapchain::~D3D11HdrSwapchain()
{
    shutdown();
}

namespace {

bool createRtvFromBackBuffer(ID3D11Device *device, IDXGISwapChain1 *swapchain,
                                ComPtr<ID3D11RenderTargetView> &outRtv)
{
    ComPtr<ID3D11Texture2D> bb;
    if (FAILED(swapchain->GetBuffer(0, IID_PPV_ARGS(bb.GetAddressOf())))) {
        return false;
    }
    return SUCCEEDED(device->CreateRenderTargetView(bb.Get(), nullptr,
                                                       outRtv.GetAddressOf()));
}

// HDR10 mastering-display metadata defaults. Shipped as fixed values
// in F.2.9 — OCIO-View-derived MaxCLL / MaxFALL is a follow-up
// (Guide 06 §4). BT.2020 primaries + D65 whitepoint match the
// G2084_NONE_P2020 colorspace tag. MaxMasteringLuminance / MaxCLL of
// 1000 nits matches the common reference-monitor target; consumer
// displays use this to drive their tone-mapping curve.
//
// Per the DXGI_HDR_METADATA_HDR10 spec:
//   MaxMasteringLuminance / MinMasteringLuminance: UINT, in 0.0001 cd/m²
//   MaxContentLightLevel / MaxFrameAverageLightLevel: UINT16, in 1 cd/m²
constexpr UINT   kHdr10Default_MaxMasteringLuminance_x10k = 10000000;  // 1000 nits
constexpr UINT   kHdr10Default_MinMasteringLuminance_x10k = 0;
constexpr UINT16 kHdr10Default_MaxContentLightLevel       = 1000;
constexpr UINT16 kHdr10Default_MaxFrameAverageLightLevel  = 400;

// DXGI primary chromaticities are uint16 in units of 0.00002
// (i.e., value × 50000). BT.2020 / D65 reference values.
constexpr UINT16 kBt2020_RedX   = 35400;  // 0.708
constexpr UINT16 kBt2020_RedY   = 14600;  // 0.292
constexpr UINT16 kBt2020_GreenX = 8500;   // 0.170
constexpr UINT16 kBt2020_GreenY = 39850;  // 0.797
constexpr UINT16 kBt2020_BlueX  = 6550;   // 0.131
constexpr UINT16 kBt2020_BlueY  = 2300;   // 0.046
constexpr UINT16 kD65_WhiteX    = 15635;  // 0.3127
constexpr UINT16 kD65_WhiteY    = 16450;  // 0.3290

void setHdr10MasteringMetadata(IDXGISwapChain1 *swapchain)
{
    if (!swapchain) return;
    ComPtr<IDXGISwapChain4> sc4;
    if (FAILED(swapchain->QueryInterface(IID_PPV_ARGS(sc4.GetAddressOf()))) || !sc4) {
        qWarning("D3D11HdrSwapchain: IDXGISwapChain4 QI failed — HDR10 "
                 "metadata not signaled (driver may default to display caps)");
        return;
    }
    DXGI_HDR_METADATA_HDR10 md{};
    md.RedPrimary[0]              = kBt2020_RedX;
    md.RedPrimary[1]              = kBt2020_RedY;
    md.GreenPrimary[0]            = kBt2020_GreenX;
    md.GreenPrimary[1]            = kBt2020_GreenY;
    md.BluePrimary[0]             = kBt2020_BlueX;
    md.BluePrimary[1]             = kBt2020_BlueY;
    md.WhitePoint[0]              = kD65_WhiteX;
    md.WhitePoint[1]              = kD65_WhiteY;
    md.MaxMasteringLuminance      = kHdr10Default_MaxMasteringLuminance_x10k;
    md.MinMasteringLuminance      = kHdr10Default_MinMasteringLuminance_x10k;
    md.MaxContentLightLevel       = kHdr10Default_MaxContentLightLevel;
    md.MaxFrameAverageLightLevel  = kHdr10Default_MaxFrameAverageLightLevel;
    const HRESULT hr = sc4->SetHDRMetaData(DXGI_HDR_METADATA_TYPE_HDR10,
                                              sizeof(md), &md);
    if (FAILED(hr)) {
        qWarning("D3D11HdrSwapchain: SetHDRMetaData failed (hr=0x%08lX)",
                 static_cast<unsigned long>(hr));
    } else {
        qInfo("D3D11HdrSwapchain: SetHDRMetaData(HDR10) MaxCLL=%u MaxFALL=%u "
              "MaxMastering=%u/10k nits",
              md.MaxContentLightLevel, md.MaxFrameAverageLightLevel,
              md.MaxMasteringLuminance);
    }
}

void queryHdrCapability(IDXGIFactory2 *factory, bool &outSupported,
                          float &outHeadroom)
{
    // DComp swapchains can't tell us — they have no associated output
    // until presented. Walking adapters via IDXGIFactory1::EnumAdapters
    // + IDXGIAdapter::EnumOutputs gives us a stable answer regardless
    // of swapchain state. Worst case: this driver doesn't support
    // IDXGIOutput6 (Win10 < 1803) and we end up with outSupported=false,
    // which is a safe default (scRGB swapchain still works; the OS
    // does the right thing based on Display Settings HDR toggle).
    outSupported = false;
    outHeadroom  = 1.0f;
    if (!factory) return;
    UINT adapterIdx = 0;
    ComPtr<IDXGIAdapter> adapter;
    while (SUCCEEDED(factory->EnumAdapters(
                adapterIdx++, adapter.ReleaseAndGetAddressOf()))) {
        if (!adapter) continue;
        UINT outIdx = 0;
        ComPtr<IDXGIOutput> output;
        while (SUCCEEDED(adapter->EnumOutputs(
                    outIdx++, output.ReleaseAndGetAddressOf()))) {
            if (!output) continue;
            ComPtr<IDXGIOutput6> output6;
            if (FAILED(output.As(&output6)) || !output6) continue;
            DXGI_OUTPUT_DESC1 desc{};
            if (FAILED(output6->GetDesc1(&desc))) continue;
            const bool osHdrOn = (desc.ColorSpace ==
                                  DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
            qInfo("D3D11HdrSwapchain: output ColorSpace=0x%x (HDR-on=%s), "
                  "MaxLuminance=%.0f nits",
                  static_cast<unsigned>(desc.ColorSpace),
                  osHdrOn ? "yes" : "no", desc.MaxLuminance);
            if (osHdrOn) outSupported = true;
            if (desc.MaxLuminance > 80.0f) {
                const float h = desc.MaxLuminance / 80.0f;
                if (h > outHeadroom) outHeadroom = h;
            }
        }
    }
}

} // namespace

bool D3D11HdrSwapchain::initialize(IDXGIFactory2 *factory, ID3D11Device *device,
                                       IDCompositionDevice *dcomp,
                                       HWND childHwnd, int width, int height)
{
    qInfo("D3D11HdrSwapchain::initialize: enter (factory=%p device=%p "
          "dcomp=%p hwnd=%p %dx%d)",
          factory, device, dcomp, childHwnd, width, height);
    if (!factory || !device || !dcomp || !childHwnd ||
        width <= 0 || height <= 0) {
        qCritical("D3D11HdrSwapchain::initialize: null arg or bad size");
        return false;
    }
    if (m_impl->initialized) {
        qWarning("D3D11HdrSwapchain: already initialized");
        return true;
    }

    m_impl->factory    = factory;
    m_impl->device     = device;
    m_impl->dcomp      = dcomp;
    m_impl->childHwnd  = childHwnd;
    m_impl->width      = width;
    m_impl->height     = height;
    m_impl->appliedMode = HdrMode::SdrSRgb;
    m_impl->pendingMode.store(static_cast<int>(HdrMode::SdrSRgb));

    const auto m = mappingFor(HdrMode::SdrSRgb);
    DXGI_SWAP_CHAIN_DESC1 sc{};
    sc.Width      = width;
    sc.Height     = height;
    sc.Format     = m.format;
    sc.SampleDesc.Count = 1;
    sc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sc.BufferCount = 2;
    sc.Scaling     = DXGI_SCALING_STRETCH;
    sc.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sc.AlphaMode   = DXGI_ALPHA_MODE_PREMULTIPLIED;
    qInfo("D3D11HdrSwapchain: calling CreateSwapChainForComposition fmt=%d %dx%d",
          static_cast<int>(m.format), width, height);
    HRESULT hr = factory->CreateSwapChainForComposition(
        device, &sc, nullptr, m_impl->swapchain.GetAddressOf());
    if (FAILED(hr)) {
        qCritical("D3D11HdrSwapchain: CreateSwapChainForComposition failed hr=0x%08lX",
                  static_cast<unsigned long>(hr));
        return false;
    }
    qInfo("D3D11HdrSwapchain: swapchain created OK %p", m_impl->swapchain.Get());

    // Tag the colorspace for the initial mode. Some drivers reject
    // SetColorSpace1 on SDR sRGB (the default), so we tolerate the
    // failure but log it.
    {
        ComPtr<IDXGISwapChain3> sc3;
        if (SUCCEEDED(m_impl->swapchain.As(&sc3)) && sc3) {
            UINT support = 0;
            if (SUCCEEDED(sc3->CheckColorSpaceSupport(m.colorSpace, &support))
                && (support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT)) {
                sc3->SetColorSpace1(m.colorSpace);
            }
        }
    }

    qInfo("D3D11HdrSwapchain: creating RTV");
    if (!createRtvFromBackBuffer(device, m_impl->swapchain.Get(), m_impl->rtv)) {
        qCritical("D3D11HdrSwapchain: initial RTV create failed");
        return false;
    }
    qInfo("D3D11HdrSwapchain: RTV OK, creating DComp target/visual");

    // DComp target on child HWND + visual containing the swapchain.
    if (FAILED(dcomp->CreateTargetForHwnd(
            childHwnd, TRUE, m_impl->dcompTarget.GetAddressOf()))) {
        qCritical("D3D11HdrSwapchain: CreateTargetForHwnd failed");
        return false;
    }
    if (FAILED(dcomp->CreateVisual(m_impl->dcompVisual.GetAddressOf()))) {
        qCritical("D3D11HdrSwapchain: CreateVisual failed");
        return false;
    }
    if (FAILED(m_impl->dcompVisual->SetContent(m_impl->swapchain.Get()))) {
        qCritical("D3D11HdrSwapchain: dcompVisual->SetContent failed");
        return false;
    }
    if (FAILED(m_impl->dcompTarget->SetRoot(m_impl->dcompVisual.Get()))) {
        qCritical("D3D11HdrSwapchain: dcompTarget->SetRoot failed");
        return false;
    }
    if (FAILED(dcomp->Commit())) {
        qCritical("D3D11HdrSwapchain: initial DComp Commit failed");
        return false;
    }
    qInfo("D3D11HdrSwapchain: DComp committed, querying capability");

    queryHdrCapability(m_impl->factory,
                        m_impl->hdrSupported, m_impl->maxHeadroom);
    qInfo("D3D11HdrSwapchain: capability query done");

    m_impl->initialized = true;
    qInfo("D3D11HdrSwapchain: initialized %dx%d %s (HDR-supported=%s, "
          "max headroom=%.2fx)",
          width, height, m.name,
          m_impl->hdrSupported ? "yes" : "no",
          m_impl->maxHeadroom);
    return true;
}

void D3D11HdrSwapchain::shutdown()
{
    if (!m_impl || !m_impl->initialized) {
        if (m_impl) m_impl->initialized = false;
        return;
    }
    if (m_impl->dcomp) m_impl->dcomp->Commit();
    m_impl->rtv.Reset();
    m_impl->dcompVisual.Reset();
    m_impl->dcompTarget.Reset();
    m_impl->swapchain.Reset();
    m_impl->factory = nullptr;
    m_impl->device  = nullptr;
    m_impl->dcomp   = nullptr;
    m_impl->childHwnd = nullptr;
    m_impl->initialized = false;
}

bool D3D11HdrSwapchain::isInitialized() const
{
    return m_impl && m_impl->initialized;
}

void D3D11HdrSwapchain::setMode(HdrMode mode)
{
    if (!m_impl) return;
    m_impl->pendingMode.store(static_cast<int>(mode), std::memory_order_release);
}

bool D3D11HdrSwapchain::applyPendingOnRenderThread()
{
    if (!m_impl || !m_impl->initialized) return false;
    const HdrMode pending = static_cast<HdrMode>(
        m_impl->pendingMode.load(std::memory_order_acquire));
    if (pending == m_impl->appliedMode) return false;

    const auto applied = mappingFor(m_impl->appliedMode);
    const auto target  = mappingFor(pending);

    // If the format matches, we only need a colorspace change — no
    // swapchain recreation.
    if (applied.format == target.format) {
        ComPtr<IDXGISwapChain3> sc3;
        if (SUCCEEDED(m_impl->swapchain.As(&sc3)) && sc3) {
            UINT support = 0;
            if (SUCCEEDED(sc3->CheckColorSpaceSupport(target.colorSpace, &support))
                && (support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT)) {
                sc3->SetColorSpace1(target.colorSpace);
            }
        }
        m_impl->appliedMode = pending;
        qInfo("D3D11HdrSwapchain: mode → %s (colorspace only, no swapchain "
              "recreate)", target.name);
        return false;   // no PSO reinit needed
    }

    // Format changed → swapchain recreate.
    std::lock_guard lock(m_impl->mutateMutex);

    m_impl->rtv.Reset();
    m_impl->swapchain.Reset();

    DXGI_SWAP_CHAIN_DESC1 sc{};
    sc.Width      = m_impl->width;
    sc.Height     = m_impl->height;
    sc.Format     = target.format;
    sc.SampleDesc.Count = 1;
    sc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sc.BufferCount = 2;
    sc.Scaling     = DXGI_SCALING_STRETCH;
    sc.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sc.AlphaMode   = DXGI_ALPHA_MODE_PREMULTIPLIED;
    if (FAILED(m_impl->factory->CreateSwapChainForComposition(
            m_impl->device, &sc, nullptr, m_impl->swapchain.GetAddressOf()))) {
        qCritical("D3D11HdrSwapchain: recreate CreateSwapChainForComposition "
                  "failed (mode=%s)", target.name);
        return false;
    }

    {
        ComPtr<IDXGISwapChain3> sc3;
        if (SUCCEEDED(m_impl->swapchain.As(&sc3)) && sc3) {
            UINT support = 0;
            HRESULT chk = sc3->CheckColorSpaceSupport(target.colorSpace, &support);
            if (SUCCEEDED(chk)) {
                const bool present = (support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) != 0;
                const bool overlay = (support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_OVERLAY_PRESENT) != 0;
                if (present) {
                    HRESULT setHr = sc3->SetColorSpace1(target.colorSpace);
                    qInfo("D3D11HdrSwapchain: SetColorSpace1(%s) → 0x%08lX "
                          "(present=%d overlay=%d)",
                          target.name, static_cast<unsigned long>(setHr),
                          present ? 1 : 0, overlay ? 1 : 0);
                    // HDR10 PQ engages → push mastering metadata. Other
                    // modes don't need it (scRGB has implicit reference
                    // white at 80 nits, SDR has no HDR signaling).
                    if (SUCCEEDED(setHr) &&
                        target.colorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) {
                        setHdr10MasteringMetadata(m_impl->swapchain.Get());
                    }
                } else {
                    qWarning("D3D11HdrSwapchain: colorspace %s unsupported on "
                             "this output (CheckColorSpaceSupport=0x%x)",
                             target.name, support);
                }
            } else {
                qWarning("D3D11HdrSwapchain: CheckColorSpaceSupport(%s) failed "
                         "(hr=0x%08lX)", target.name,
                         static_cast<unsigned long>(chk));
            }
        } else {
            qWarning("D3D11HdrSwapchain: IDXGISwapChain3 QI failed — "
                     "colorspace not tagged");
        }
    }

    if (!createRtvFromBackBuffer(m_impl->device, m_impl->swapchain.Get(),
                                    m_impl->rtv)) {
        qCritical("D3D11HdrSwapchain: post-recreate RTV create failed");
        return false;
    }

    // Re-anchor the DComp visual to the new swapchain and commit.
    if (FAILED(m_impl->dcompVisual->SetContent(m_impl->swapchain.Get())) ||
        FAILED(m_impl->dcomp->Commit())) {
        qWarning("D3D11HdrSwapchain: DComp re-anchor after format change "
                 "failed; visual may show stale content for one frame");
    }

    m_impl->appliedMode = pending;
    queryHdrCapability(m_impl->factory,
                        m_impl->hdrSupported, m_impl->maxHeadroom);
    qInfo("D3D11HdrSwapchain: mode → %s (swapchain recreated; HDR-supported=%s, "
          "headroom=%.2fx)", target.name,
          m_impl->hdrSupported ? "yes" : "no", m_impl->maxHeadroom);
    return true;  // format changed → caller should reinit PSOs
}

bool D3D11HdrSwapchain::resize(int width, int height)
{
    if (!m_impl || !m_impl->initialized || width <= 0 || height <= 0) {
        return false;
    }
    std::lock_guard lock(m_impl->mutateMutex);
    if (width == m_impl->width && height == m_impl->height) return true;

    m_impl->rtv.Reset();
    HRESULT hr = m_impl->swapchain->ResizeBuffers(
        0, static_cast<UINT>(width), static_cast<UINT>(height),
        DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) {
        qWarning("D3D11HdrSwapchain: ResizeBuffers failed (hr=0x%08lX)",
                 static_cast<unsigned long>(hr));
        return false;
    }
    if (!createRtvFromBackBuffer(m_impl->device, m_impl->swapchain.Get(),
                                    m_impl->rtv)) {
        qWarning("D3D11HdrSwapchain: post-resize RTV create failed");
        return false;
    }
    m_impl->width  = width;
    m_impl->height = height;
    return true;
}

HdrMode D3D11HdrSwapchain::appliedMode() const
{
    return m_impl ? m_impl->appliedMode : HdrMode::SdrSRgb;
}

int D3D11HdrSwapchain::currentFormat() const
{
    return m_impl ? static_cast<int>(mappingFor(m_impl->appliedMode).format)
                  : static_cast<int>(DXGI_FORMAT_B8G8R8A8_UNORM);
}

int D3D11HdrSwapchain::currentWidth() const  { return m_impl ? m_impl->width  : 0; }
int D3D11HdrSwapchain::currentHeight() const { return m_impl ? m_impl->height : 0; }

void *D3D11HdrSwapchain::swapchain() const
{
    return m_impl ? m_impl->swapchain.Get() : nullptr;
}
void *D3D11HdrSwapchain::rtv() const
{
    return m_impl ? m_impl->rtv.Get() : nullptr;
}
void *D3D11HdrSwapchain::dcompVisual() const
{
    return m_impl ? m_impl->dcompVisual.Get() : nullptr;
}

bool  D3D11HdrSwapchain::isHdrSupported() const  { return m_impl && m_impl->hdrSupported; }
float D3D11HdrSwapchain::maxHdrHeadroom() const  { return m_impl ? m_impl->maxHeadroom : 1.0f; }

} // namespace qcv
