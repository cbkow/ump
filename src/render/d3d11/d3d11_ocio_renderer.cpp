// D3D11OcioRenderer — see header for adaptation notes.

#include "d3d11_ocio_renderer.h"

#include "color/ocio_chain_builder.h"
#include "color/ocio_config_manager.h"
#include "d3d11_device_manager.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3d11shader.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <QElapsedTimer>

#include <atomic>
#include <functional>
#include <mutex>
#include <thread>

#include <QFile>
#include <QStandardPaths>
#include <QtLogging>

#include <cstring>
#include <vector>

namespace OCIO = OCIO_NAMESPACE;

namespace qcv {

using Microsoft::WRL::ComPtr;

namespace {

// Fullscreen-triangle VS — same Y-flip as the F.1.a probe spike, so
// screen pixel (x, y) maps to texel (x, y).
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

// PS wrapper for OCIO. OCIO 2.x emits its own SamplerState + Texture*
// declarations with explicit register() qualifiers inside its shader
// text, so all we have to do is declare our source texture at t0/s0
// and call OCIODisplay(src). The function takes (and returns) float4
// when OCIO is configured for 4-channel — the spike used the same form.
//
// Phase F.2.9: brightness adjustment lives in D3D11Compositor (pre-OCIO,
// linear-light). The OCIO PS is brightness-agnostic — it just runs
// OCIODisplay and writes the result. Post-OCIO scaling was prototyped
// (an SDR-reference-white knob to compensate for Windows' lower
// reference white vs macOS EDR) but rejected because it breaks the
// PQ-in → PQ-out 1:1 round-trip invariant. Linear-light exposure
// adjustment via the compositor's brightness slider is the right
// link in the chain: at gain=1.0 OCIO 1:1 holds; gain>1 lifts scene
// values pre-OCIO so the View transform re-encodes consistently.
std::string buildPsHlsl(const std::string &ocioFunction,
                          const std::string &funcName)
{
    std::string s;
    s.reserve(ocioFunction.size() + 512);
    s += "Texture2D    uSrc        : register(t0);\n";
    s += "SamplerState uSrcSampler : register(s0);\n";
    s += "\n";
    s += ocioFunction;
    s += "\n\nstruct VsOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };\n";
    s += "float4 PSMain(VsOut input) : SV_TARGET\n";
    s += "{\n";
    s += "    float4 src = uSrc.Sample(uSrcSampler, input.uv);\n";
    s += "    return " + funcName + "(src);\n";
    s += "}\n";
    return s;
}

ComPtr<ID3DBlob> compileHlsl(const std::string &source,
                                const char *entry, const char *target,
                                QString *errorOut)
{
    ComPtr<ID3DBlob> code, errors;
    // LEVEL_1 trades runtime PS speed for compile speed. OCIO chains
    // are straight-line math per pixel — little for the optimizer to
    // win — and we pay compile cost on every chain change. LEVEL_3 is
    // multi-second on large chains; LEVEL_1 is sub-second. SKIP was
    // tried and broke OCIO output (suspected register-packing diff
    // with large chains), so LEVEL_1 is the conservative pick.
    const HRESULT hr = D3DCompile(
        source.data(), source.size(),
        entry, nullptr, nullptr,
        entry, target,
        D3DCOMPILE_OPTIMIZATION_LEVEL1, 0,
        code.GetAddressOf(), errors.GetAddressOf());
    if (FAILED(hr)) {
        QString msg = QStringLiteral("D3DCompile %1 failed (hr=0x%2)")
            .arg(QString::fromUtf8(entry))
            .arg(static_cast<quint32>(hr), 8, 16, QLatin1Char('0'));
        if (errors) {
            msg += QStringLiteral(" — ") +
                   QString::fromUtf8(static_cast<const char *>(errors->GetBufferPointer()));
        }
        if (errorOut) *errorOut = msg;
        return nullptr;
    }
    return code;
}

struct LutResource {
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11SamplerState>       sampler;
    std::string                      texName;     // OCIO's emitted Texture* name
    std::string                      smpName;     // OCIO's emitted SamplerState name
    bool                             is3d = false;
    const char                      *dimLabel = "1D";  // "1D" / "2D" (for logging only)
    // Bind slots discovered via D3D11 shader reflection — set after
    // PS compile so we don't have to guess at iteration-order matching
    // OCIO's emitted register() qualifiers. Both default to -1 (skip).
    int                              texSlot = -1;
    int                              smpSlot = -1;
};

ComPtr<ID3D11SamplerState> makeLinearClampSampler(ID3D11Device *device)
{
    D3D11_SAMPLER_DESC ss{};
    ss.Filter   = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    ss.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    ss.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    ss.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    ss.MaxLOD   = D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState> smp;
    device->CreateSamplerState(&ss, smp.GetAddressOf());
    return smp;
}

} // namespace

struct D3D11OcioRenderer::Impl {
    ID3D11Device        *device  = nullptr;
    ComPtr<ID3D11VertexShader>  vs;
    ComPtr<ID3D11PixelShader>   ps;
    ComPtr<ID3D11SamplerState>  srcSampler;

    // Active LUTs in OCIO's declaration order (3D first, then 1D/2D).
    // Slot positions inside each LutResource are reflection-resolved
    // by OCIO's texture/sampler name, mirroring how MetalOcioRenderer
    // binds via MSL function parameter names.
    std::vector<LutResource> luts;

    int     lastChainGeneration = -1;
    QString lastError;

    // --- Async rebuild plumbing -------------------------------------
    // D3DCompile on heavy OCIO chains (AgX with multi-hundred-line
    // helper functions + large static arrays) can take 5-15 seconds
    // even at OPTIMIZATION_LEVEL_1. Doing it on the render thread
    // freezes playback. We compile + create LUT resources on a
    // worker thread, stage the result under `swapMutex`, and the
    // render thread swaps the active pipeline in on its next rebuild()
    // call. While the worker is busy, rebuild() returns false so
    // drawFrame() bypasses OCIO and shows uncorrected source — better
    // than dropping all frames during compile.
    std::atomic<bool>          rebuildInProgress{false};
    std::thread                rebuildThread;

    std::mutex                 swapMutex;       // guards the four pending* fields
    bool                       pendingReady = false;
    ComPtr<ID3D11PixelShader>  pendingPs;
    std::vector<LutResource>   pendingLuts;
    int                        pendingGen   = -1;
    QString                    pendingError;

    // Render-thread wake callback fired from the worker when a fresh
    // pipeline has been staged. Lets a paused-playback view switch
    // pick up the new chain without waiting for the next decoded
    // frame. Held by value (copyable std::function) so the worker
    // calling it doesn't race with renderer teardown after we join
    // the thread in shutdown().
    std::function<void()>      wakeCallback;
};

D3D11OcioRenderer::D3D11OcioRenderer()
    : m_impl(std::make_unique<Impl>()) {}

D3D11OcioRenderer::~D3D11OcioRenderer()
{
    shutdown();
}

bool D3D11OcioRenderer::initialize()
{
    auto &mgr = D3D11DeviceManager::instance();
    m_impl->device = static_cast<ID3D11Device *>(mgr.device());
    if (!m_impl->device) {
        qWarning("D3D11OcioRenderer: no D3D11 device");
        return false;
    }

    QString vsErr;
    ComPtr<ID3DBlob> vsBlob = compileHlsl(kVsHlsl, "VSMain", "vs_5_0", &vsErr);
    if (!vsBlob) {
        m_impl->lastError = vsErr;
        qWarning("%s", qPrintable(m_impl->lastError));
        return false;
    }
    if (FAILED(m_impl->device->CreateVertexShader(
            vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
            nullptr, m_impl->vs.GetAddressOf()))) {
        m_impl->lastError = QStringLiteral("D3D11OcioRenderer: CreateVertexShader failed");
        return false;
    }

    m_impl->srcSampler = makeLinearClampSampler(m_impl->device);
    if (!m_impl->srcSampler) {
        m_impl->lastError = QStringLiteral("D3D11OcioRenderer: srcSampler create failed");
        return false;
    }
    return true;
}

void D3D11OcioRenderer::shutdown()
{
    if (!m_impl) return;
    // Wait for any in-flight worker so it doesn't keep referencing
    // the device after teardown. D3DCompile can't be cancelled, so
    // this may block up to a few seconds.
    if (m_impl->rebuildThread.joinable()) {
        m_impl->rebuildThread.join();
    }
    {
        std::lock_guard lock(m_impl->swapMutex);
        m_impl->pendingPs.Reset();
        m_impl->pendingLuts.clear();
        m_impl->pendingReady = false;
    }
    m_impl->vs.Reset();
    m_impl->ps.Reset();
    m_impl->srcSampler.Reset();
    m_impl->luts.clear();
    m_impl->device = nullptr;
    m_impl->lastChainGeneration = -1;
}

bool D3D11OcioRenderer::isInitialized() const
{
    return m_impl && m_impl->device != nullptr && m_impl->vs && m_impl->srcSampler;
}

bool D3D11OcioRenderer::hasPipeline() const
{
    return m_impl && m_impl->ps;
}

const QString &D3D11OcioRenderer::lastError() const
{
    return m_impl->lastError;
}

void D3D11OcioRenderer::setWakeCallback(std::function<void()> cb)
{
    m_impl->wakeCallback = std::move(cb);
}

bool D3D11OcioRenderer::rebuild(OCIOConfigManager *ocio)
{
    if (!ocio || !isInitialized()) return false;

    const int gen = ocio->activeChainGeneration();

    // 1. Drain any pending result from a previously-finished worker.
    {
        std::lock_guard lock(m_impl->swapMutex);
        if (m_impl->pendingReady) {
            m_impl->ps                  = std::move(m_impl->pendingPs);
            m_impl->luts                = std::move(m_impl->pendingLuts);
            m_impl->lastChainGeneration = m_impl->pendingGen;
            m_impl->lastError           = std::move(m_impl->pendingError);
            m_impl->pendingPs.Reset();
            m_impl->pendingLuts.clear();
            m_impl->pendingError.clear();
            m_impl->pendingReady = false;
        }
    }

    // 2. Already at the right gen? Trust whatever the last worker
    //    produced — success means ps is non-null, failure means we
    //    cached the failed gen here so the next frame doesn't
    //    re-spawn a worker compiling the same broken chain.
    if (gen == m_impl->lastChainGeneration) {
        return static_cast<bool>(m_impl->ps);
    }

    // 3. Worker still running? Keep using the old pipeline (if any).
    //    Engine bypasses OCIO when this returns false (first-time
    //    compile) — user sees uncorrected source for a few seconds
    //    rather than dropping all frames.
    if (m_impl->rebuildInProgress.load()) {
        return static_cast<bool>(m_impl->ps);
    }

    // 4. Spawn a worker for the new gen. Join any previous thread
    //    object first (it's finished — rebuildInProgress would be
    //    false otherwise — just bookkeeping).
    if (m_impl->rebuildThread.joinable()) {
        m_impl->rebuildThread.join();
    }
    m_impl->rebuildInProgress.store(true);
    m_impl->rebuildThread = std::thread([this, gen, ocio]() {
        doRebuildWork(gen, ocio);
        m_impl->rebuildInProgress.store(false);
    });

    return static_cast<bool>(m_impl->ps);
}

void D3D11OcioRenderer::doRebuildWork(int gen, OCIOConfigManager *ocio)
{
    auto stageEmpty = [this, gen](const QString &err) {
        {
            std::lock_guard lock(m_impl->swapMutex);
            m_impl->pendingPs.Reset();
            m_impl->pendingLuts.clear();
            m_impl->pendingGen   = gen;
            m_impl->pendingError = err;
            m_impl->pendingReady = true;
        }
        if (m_impl->wakeCallback) m_impl->wakeCallback();
    };

    OcioChain chain =
        OcioChainBuilder::build(ocio, OcioChainBuilder::Language::Hlsl_Sm_5_0);
    if (!chain.ok) {
        stageEmpty(chain.errorMessage);
        return;
    }

    auto desc = chain.desc;
    const int n3d = desc->getNum3DTextures();
    const int n1d = desc->getNumTextures();

    std::vector<LutResource> newLuts;
    newLuts.reserve(static_cast<std::size_t>(n3d + n1d));

    // --- 3D LUTs (declared first by OCIO HLSL emit) ---
    for (int i = 0; i < n3d; ++i) {
        const char *texName = nullptr, *smpName = nullptr;
        unsigned edge = 0;
        OCIO::Interpolation interp = OCIO::INTERP_DEFAULT;
        desc->get3DTexture(static_cast<unsigned>(i), texName, smpName, edge, interp);
        const float *values = nullptr;
        desc->get3DTextureValues(static_cast<unsigned>(i), values);
        if (!values || edge == 0) continue;

        // D3D11 has no RGB32_FLOAT 3D — unpack RGB → RGBA per voxel.
        const std::size_t voxels = static_cast<std::size_t>(edge) * edge * edge;
        std::vector<float> rgba(voxels * 4);
        for (std::size_t v = 0; v < voxels; ++v) {
            rgba[v*4 + 0] = values[v*3 + 0];
            rgba[v*4 + 1] = values[v*3 + 1];
            rgba[v*4 + 2] = values[v*3 + 2];
            rgba[v*4 + 3] = 1.0f;
        }

        D3D11_TEXTURE3D_DESC td{};
        td.Width     = edge;
        td.Height    = edge;
        td.Depth     = edge;
        td.MipLevels = 1;
        td.Format    = DXGI_FORMAT_R32G32B32A32_FLOAT;
        td.Usage     = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA init{};
        init.pSysMem          = rgba.data();
        init.SysMemPitch      = edge * 4 * sizeof(float);
        init.SysMemSlicePitch = edge * edge * 4 * sizeof(float);
        ComPtr<ID3D11Texture3D> tex;
        if (FAILED(m_impl->device->CreateTexture3D(&td, &init, tex.GetAddressOf()))) {
            stageEmpty(QStringLiteral("D3D11OcioRenderer: CreateTexture3D LUT3D failed"));
            return;
        }
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format        = td.Format;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
        sd.Texture3D.MipLevels = 1;
        LutResource r;
        r.is3d    = true;
        r.texName = texName ? texName : "";
        r.smpName = smpName ? smpName : "";
        if (FAILED(m_impl->device->CreateShaderResourceView(tex.Get(), &sd,
                                                              r.srv.GetAddressOf()))) {
            stageEmpty(QStringLiteral("D3D11OcioRenderer: SRV LUT3D failed"));
            return;
        }
        r.sampler = makeLinearClampSampler(m_impl->device);
        newLuts.push_back(std::move(r));
    }

    // --- 1D/2D LUTs ---
    // OCIO 2.x HLSL emits real `Texture1D` declarations for 1D LUTs
    // (height == 1) — not `Texture2D` as Nx1. Binding a Texture2D
    // SRV at a slot the HLSL declares as Texture1D is a resource-
    // type mismatch: D3D11 silently returns zeros instead of
    // sampling — which renders OCIO transforms as no-op / wrong
    // colors without a validation error. We branch here on OCIO's
    // reported dim so the SRV type matches the HLSL declaration.
    // Mirrors the Metal renderer's `is1D ? MTLTextureType1D :
    // MTLTextureType2D` branch.
    for (int i = 0; i < n1d; ++i) {
        const char *texName = nullptr, *smpName = nullptr;
        unsigned w = 0, h = 0;
        OCIO::GpuShaderDesc::TextureType chan =
            OCIO::GpuShaderDesc::TEXTURE_RGB_CHANNEL;
        OCIO::GpuShaderDesc::TextureDimensions dim =
            OCIO::GpuShaderDesc::TEXTURE_1D;
        OCIO::Interpolation interp = OCIO::INTERP_DEFAULT;
        desc->getTexture(static_cast<unsigned>(i), texName, smpName, w, h, chan, dim, interp);
        const float *values = nullptr;
        desc->getTextureValues(static_cast<unsigned>(i), values);
        if (!values || w == 0) continue;

        const bool is1D  = (dim == OCIO::GpuShaderDesc::TEXTURE_1D);
        const bool isRgb = (chan == OCIO::GpuShaderDesc::TEXTURE_RGB_CHANNEL);
        const DXGI_FORMAT fmt = isRgb ? DXGI_FORMAT_R32G32B32_FLOAT
                                       : DXGI_FORMAT_R32_FLOAT;
        const int channels = isRgb ? 3 : 1;

        ComPtr<ID3D11Resource> tex;
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = fmt;
        if (is1D) {
            D3D11_TEXTURE1D_DESC td{};
            td.Width     = w;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format    = fmt;
            td.Usage     = D3D11_USAGE_IMMUTABLE;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA init{};
            init.pSysMem = values;
            // SysMemPitch is ignored for Texture1D, but we set it for
            // clarity and to keep it correct if this layout is reused
            // elsewhere.
            init.SysMemPitch = w * channels * sizeof(float);
            ComPtr<ID3D11Texture1D> t1d;
            if (FAILED(m_impl->device->CreateTexture1D(&td, &init, t1d.GetAddressOf()))) {
                stageEmpty(QStringLiteral("D3D11OcioRenderer: CreateTexture1D LUT1D failed"));
                return;
            }
            sd.ViewDimension       = D3D11_SRV_DIMENSION_TEXTURE1D;
            sd.Texture1D.MipLevels = 1;
            tex = t1d;
        } else {
            const unsigned height = h > 0 ? h : 1;
            D3D11_TEXTURE2D_DESC td{};
            td.Width     = w;
            td.Height    = height;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format    = fmt;
            td.SampleDesc.Count = 1;
            td.Usage     = D3D11_USAGE_IMMUTABLE;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA init{};
            init.pSysMem     = values;
            init.SysMemPitch = w * channels * sizeof(float);
            ComPtr<ID3D11Texture2D> t2d;
            if (FAILED(m_impl->device->CreateTexture2D(&td, &init, t2d.GetAddressOf()))) {
                stageEmpty(QStringLiteral("D3D11OcioRenderer: CreateTexture2D LUT2D failed"));
                return;
            }
            sd.ViewDimension       = D3D11_SRV_DIMENSION_TEXTURE2D;
            sd.Texture2D.MipLevels = 1;
            tex = t2d;
        }

        LutResource r;
        r.is3d     = false;
        r.dimLabel = is1D ? "1D" : "2D";
        r.texName  = texName ? texName : "";
        r.smpName  = smpName ? smpName : "";
        if (FAILED(m_impl->device->CreateShaderResourceView(tex.Get(), &sd,
                                                              r.srv.GetAddressOf()))) {
            stageEmpty(QStringLiteral(
                "D3D11OcioRenderer: SRV %1 failed").arg(is1D ? "1D" : "2D"));
            return;
        }
        r.sampler = makeLinearClampSampler(m_impl->device);
        newLuts.push_back(std::move(r));
    }

    // Compile PS combining OCIO function + thin wrapper.
    const std::string psSource =
        buildPsHlsl(chain.shaderText.toStdString(), "OCIODisplay");
    QElapsedTimer compileTimer;
    compileTimer.start();
    QString psErr;
    ComPtr<ID3DBlob> psBlob = compileHlsl(psSource, "PSMain", "ps_5_0", &psErr);
    const qint64 compileMs = compileTimer.elapsed();
    if (!psBlob) {
        qWarning("%s", qPrintable(psErr));
        // Dump for inspection.
        const QString tmpDir =
            QStandardPaths::writableLocation(QStandardPaths::TempLocation);
        const QString dumpPath = tmpDir + QStringLiteral("/qcv-ocio-hlsl-fail.hlsl");
        if (QFile f(dumpPath); f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            f.write(psSource.data(), psSource.size());
            qInfo("D3D11OcioRenderer: dumped failing HLSL to %s",
                  qPrintable(dumpPath));
        }
        stageEmpty(psErr);
        return;
    }
    ComPtr<ID3D11PixelShader> newPs;
    if (FAILED(m_impl->device->CreatePixelShader(
            psBlob->GetBufferPointer(), psBlob->GetBufferSize(),
            nullptr, newPs.GetAddressOf()))) {
        stageEmpty(QStringLiteral("D3D11OcioRenderer: CreatePixelShader failed"));
        return;
    }

    // Reflect the compiled PS to discover each LUT's bind slot by
    // name — matches the macOS Metal pattern (which names function
    // parameters) rather than relying on iteration-order matching
    // OCIO's emitted `: register(tN)` qualifiers. If a LUT can't be
    // found by name (some OCIO emitters rename or drop unused LUTs
    // during shader-text emission), we fall back to the iteration-
    // order slot the F.1.a spike was proven to work with.
    ComPtr<ID3D11ShaderReflection> refl;
    // __uuidof avoids needing dxguid.lib at link time — the COM IID
    // is attached to the interface declaration in d3d11shader.h via
    // MIDL's __declspec(uuid("...")) attribute.
    HRESULT rhr = D3DReflect(psBlob->GetBufferPointer(), psBlob->GetBufferSize(),
                                __uuidof(ID3D11ShaderReflection),
                                reinterpret_cast<void **>(refl.GetAddressOf()));
    int slotsByName = 0, slotsFallback = 0;
    int iterSlot = 1;   // t0 = src, LUTs at t1+ in OCIO's declaration order
    for (auto &lut : newLuts) {
        D3D11_SHADER_INPUT_BIND_DESC bd{};
        if (refl &&
            SUCCEEDED(refl->GetResourceBindingDescByName(lut.texName.c_str(), &bd))) {
            lut.texSlot = static_cast<int>(bd.BindPoint);
            ++slotsByName;
        } else {
            lut.texSlot = iterSlot;
            ++slotsFallback;
        }
        if (refl &&
            SUCCEEDED(refl->GetResourceBindingDescByName(lut.smpName.c_str(), &bd))) {
            lut.smpSlot = static_cast<int>(bd.BindPoint);
        } else {
            lut.smpSlot = lut.texSlot;  // matched-pair convention
        }
        ++iterSlot;
    }

    // Dump the OCIO HLSL + the wrapped PS source on every rebuild —
    // overwrites each time, so the file always reflects the latest
    // chain. Useful for diagnosing "wrong colors / black output" by
    // hand-inspecting which registers OCIO actually assigned.
    {
        const QString tmpDir =
            QStandardPaths::writableLocation(QStandardPaths::TempLocation);
        const QString hlslPath = tmpDir + QStringLiteral("/qcv-ocio.hlsl");
        if (QFile f(hlslPath); f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            f.write(psSource.data(), psSource.size());
        }
        qInfo("D3D11OcioRenderer: HLSL dumped to %s", qPrintable(hlslPath));
    }
    for (const auto &lut : newLuts) {
        qInfo("  LUT %s (%s) → tex t%d, smp s%d",
              lut.texName.c_str(),
              lut.is3d ? "3D" : lut.dimLabel,
              lut.texSlot, lut.smpSlot);
    }

    int n3dKept = 0, n1dKept = 0;
    for (const auto &l : newLuts) (l.is3d ? n3dKept : n1dKept)++;
    qInfo("D3D11OcioRenderer: worker built gen %d (compile %lldms, %d 3D + %d 1D LUTs; "
          "%d slots by reflection, %d by fallback) — staging for render-thread swap",
          gen, static_cast<long long>(compileMs), n3dKept, n1dKept,
          slotsByName, slotsFallback);

    // Stage the fresh pipeline + LUTs. The render thread will pick
    // them up on its next rebuild() call and atomically swap into
    // the active slot.
    {
        std::lock_guard lock(m_impl->swapMutex);
        m_impl->pendingPs    = newPs;
        m_impl->pendingLuts  = std::move(newLuts);
        m_impl->pendingGen   = gen;
        m_impl->pendingError.clear();
        m_impl->pendingReady = true;
    }
    // Wake the render thread (it may be in a paused-playback wait
    // with no decoded frames to nudge it otherwise).
    if (m_impl->wakeCallback) m_impl->wakeCallback();
}

void D3D11OcioRenderer::apply(void *ctxPtr,
                                 void *srcSrvPtr,
                                 void *dstRtvPtr,
                                 int   dstW, int dstH)
{
    if (!hasPipeline() || !ctxPtr || !srcSrvPtr || !dstRtvPtr ||
        dstW <= 0 || dstH <= 0) {
        return;
    }
    auto *ctx     = static_cast<ID3D11DeviceContext *>(ctxPtr);
    auto *srcSrv  = static_cast<ID3D11ShaderResourceView *>(srcSrvPtr);
    auto *dstRtv  = static_cast<ID3D11RenderTargetView *>(dstRtvPtr);

    ctx->OMSetRenderTargets(1, &dstRtv, nullptr);
    D3D11_VIEWPORT vp{};
    vp.Width    = static_cast<float>(dstW);
    vp.Height   = static_cast<float>(dstH);
    vp.MinDepth = 0; vp.MaxDepth = 1;
    ctx->RSSetViewports(1, &vp);

    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->IASetInputLayout(nullptr);
    ctx->VSSetShader(m_impl->vs.Get(), nullptr, 0);
    ctx->PSSetShader(m_impl->ps.Get(), nullptr, 0);

    // Bind by reflected slot — each LUT goes at the BindPoint D3D's
    // shader reflector reported for its name. Source stays at t0/s0
    // by convention (we declared it explicitly in buildPsHlsl).
    constexpr int kMaxSrvs = 16;
    ID3D11ShaderResourceView *srvs[kMaxSrvs] = {nullptr};
    ID3D11SamplerState       *smps[kMaxSrvs] = {nullptr};
    srvs[0] = srcSrv;
    smps[0] = m_impl->srcSampler.Get();
    int maxSlot = 0;
    for (const auto &lut : m_impl->luts) {
        if (lut.texSlot >= 0 && lut.texSlot < kMaxSrvs) {
            srvs[lut.texSlot] = lut.srv.Get();
            if (lut.texSlot > maxSlot) maxSlot = lut.texSlot;
        }
        if (lut.smpSlot >= 0 && lut.smpSlot < kMaxSrvs) {
            smps[lut.smpSlot] = lut.sampler.Get();
            if (lut.smpSlot > maxSlot) maxSlot = lut.smpSlot;
        }
    }
    const UINT nBound = static_cast<UINT>(maxSlot + 1);
    ctx->PSSetShaderResources(0, nBound, srvs);
    ctx->PSSetSamplers       (0, nBound, smps);

    ctx->Draw(3, 0);

    // Unbind the source SRV slot so the next pass (or a subsequent
    // CopyResource into the same texture) doesn't trip D3D11's
    // "resource still bound" debug-layer warning.
    ID3D11ShaderResourceView *nullSrv = nullptr;
    ctx->PSSetShaderResources(0, 1, &nullSrv);
}

} // namespace qcv
