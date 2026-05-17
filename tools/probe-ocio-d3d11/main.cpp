// probe-ocio-d3d11 — Phase F.1.a spike.
//
// Verifies OCIO 2.x can drive a D3D11 / HLSL render pipeline on Intel Arc
// (and any other Windows GPU). The Phase F D3D11 rewrite is gated on this
// path working — if OCIO's HLSL emitter is fundamentally broken or the
// 1D-LUT-as-2D / 3D-LUT binding is mismatched against D3D11 expectations,
// the rewrite has to take a different OCIO integration approach.
//
// What this does:
//   1. Load OCIO config (default: assets/OCIO/Blender5.1/config.ocio).
//   2. Build a DisplayViewTransform processor (CPU + GPU).
//   3. Configure GpuShaderDesc with GPU_LANGUAGE_HLSL_SM_5_0.
//   4. Extract the HLSL shader text + iterate LUT bindings.
//   5. Wrap the OCIO function in a pixel-shader main() that samples a
//      source texture and emits the OCIO-transformed result.
//   6. Spin up a headless D3D11 device (no window).
//   7. Compile vertex + pixel shaders via D3DCompile.
//   8. Upload LUT data to D3D11 textures (1D→Texture2D Nx1, 3D→Texture3D).
//   9. Generate a test input texture (256x256 RGBA16F gradient).
//  10. Render OCIO-transformed result into a 256x256 RGBA16F RTV.
//  11. Stage the RTV back to CPU, compare against the same gradient
//      pushed through OCIO's CPU processor.
//  12. PASS if max abs diff ≤ tolerance, FAIL otherwise (print stats).
//
// Usage:  probe-ocio-d3d11 [config-path] [input-cs] [display] [view]
// Defaults: assets/OCIO/Blender5.1/config.ocio + scene_linear + default
//           display + default view.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <OpenColorIO/OpenColorIO.h>

#include <fstream>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace OCIO = OCIO_NAMESPACE;
using Microsoft::WRL::ComPtr;

namespace {

constexpr int kTestSize = 256;

// ---------------------------------------------------------------------
// Half-float helpers (RGBA16F readback / upload)
// ---------------------------------------------------------------------
uint16_t floatToHalf(float f)
{
    union { float f; uint32_t u; } in{f};
    uint32_t b   = in.u;
    uint32_t sign = (b >> 16) & 0x8000;
    int32_t  e    = ((b >> 23) & 0xFF) - (127 - 15);
    uint32_t m    = b & 0x7FFFFF;
    if (e <= 0) {
        if (e < -10) return static_cast<uint16_t>(sign);
        m = (m | 0x800000) >> (1 - e);
        return static_cast<uint16_t>(sign | (m >> 13));
    }
    if (e >= 31) return static_cast<uint16_t>(sign | 0x7C00);
    return static_cast<uint16_t>(sign | (e << 10) | (m >> 13));
}
float halfToFloat(uint16_t h)
{
    uint32_t sign = (h & 0x8000) << 16;
    uint32_t e    = (h >> 10) & 0x1F;
    uint32_t m    = h & 0x3FF;
    uint32_t b;
    if (e == 0) {
        if (m == 0) { b = sign; }
        else {
            while (!(m & 0x400)) { m <<= 1; --e; }
            ++e; m &= 0x3FF;
            b = sign | ((e + (127 - 15)) << 23) | (m << 13);
        }
    } else if (e == 31) {
        b = sign | 0x7F800000 | (m << 13);
    } else {
        b = sign | ((e + (127 - 15)) << 23) | (m << 13);
    }
    union { uint32_t u; float f; } out{b};
    return out.f;
}

void die(const char *msg, HRESULT hr = S_OK)
{
    if (hr != S_OK)
        std::fprintf(stderr, "FATAL: %s (hr=0x%08lX)\n", msg, static_cast<unsigned long>(hr));
    else
        std::fprintf(stderr, "FATAL: %s\n", msg);
    std::exit(1);
}

// ---------------------------------------------------------------------
// Compile a single HLSL stage with D3DCompile, dumping errors clearly.
// ---------------------------------------------------------------------
ComPtr<ID3DBlob> compileHlsl(const std::string &source,
                                const char *entry, const char *target,
                                const char *labelForErrors)
{
    ComPtr<ID3DBlob> code, errors;
    const HRESULT hr = D3DCompile(
        source.data(), source.size(),
        labelForErrors, nullptr, nullptr,
        entry, target,
        D3DCOMPILE_OPTIMIZATION_LEVEL3,
        0,
        code.GetAddressOf(),
        errors.GetAddressOf());
    if (FAILED(hr)) {
        std::fprintf(stderr, "D3DCompile %s FAILED (hr=0x%08lX)\n",
                     labelForErrors, static_cast<unsigned long>(hr));
        if (errors) {
            std::fprintf(stderr, "--- compile errors ---\n%s\n",
                         static_cast<const char *>(errors->GetBufferPointer()));
        }
        // Dump source for inspection
        std::ofstream dump("probe_ocio_d3d11.hlsl.dump", std::ios::binary);
        if (dump) {
            dump.write(source.data(), static_cast<std::streamsize>(source.size()));
            std::fprintf(stderr, "Source dumped to: probe_ocio_d3d11.hlsl.dump\n");
        }
        std::exit(1);
    }
    return code;
}

// ---------------------------------------------------------------------
// Wrap OCIO's HLSL function in a pixel-shader main(). The vertex shader
// is a separate fullscreen-triangle pass.
// ---------------------------------------------------------------------
std::string buildPixelShaderHlsl(const std::string &ocioFunction,
                                   const std::string &funcName,
                                   const std::vector<std::string> &lutLines)
{
    // Header: source texture binding at t0/s0; OCIO LUTs come after that.
    // OCIO's emitted HLSL already declares its own SamplerState +
    // Texture* — we don't add them here. But we DO need to make sure the
    // OCIO declarations don't conflict with our source-texture slot 0.
    //
    // Empirically, OCIO 2.x's HLSL emitter assigns explicit register slots
    // to its samplers/textures starting at t1/s1 (or whatever offset).
    // If it doesn't, the LUT lines we built (with t1+, s1+) handle it.
    std::string s;
    s += "Texture2D    uSrc       : register(t0);\n";
    s += "SamplerState uSrcSampler: register(s0);\n";
    s += "\n";
    s += ocioFunction;
    s += "\n\n";
    s += "struct VsOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };\n";
    s += "float4 PSMain(VsOut input) : SV_TARGET\n";
    s += "{\n";
    s += "    float4 src = uSrc.Sample(uSrcSampler, input.uv);\n";
    s += "    return " + funcName + "(src);\n";
    s += "}\n";
    (void)lutLines;  // OCIO HLSL emitter handles its own declarations
    return s;
}

constexpr const char *kVertexHlsl = R"(
struct VsOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
VsOut VSMain(uint vid : SV_VertexID)
{
    VsOut o;
    float2 uv = float2((vid << 1) & 2, vid & 2);
    o.uv  = uv;
    // Flip Y on the position so screen pixel (x, y) maps to texel (x, y).
    // D3D11 screen origin is top-left (+Y down); without the flip the
    // fullscreen-triangle UV convention makes screen y=0 sample texel y=N
    // instead of texel y=0. That mismatch shows up as a max-abs diff of
    // ~1.0 when comparing against CPU's pixel-by-pixel reference.
    o.pos = float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, 0.0f, 1.0f);
    return o;
}
)";

// ---------------------------------------------------------------------
// Build a test input: 256x256 RGBA16F gradient. R/G across the
// rectangle, B fixed at 0.5, A=1. Pixel-perfect deterministic so we can
// compare CPU vs GPU outputs.
// ---------------------------------------------------------------------
std::vector<float> buildGradientFloat(int w, int h)
{
    std::vector<float> data(static_cast<size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const size_t i = (static_cast<size_t>(y) * w + x) * 4;
            data[i + 0] = x / float(w - 1);        // R
            data[i + 1] = y / float(h - 1);        // G
            data[i + 2] = 0.5f;                    // B
            data[i + 3] = 1.0f;                    // A
        }
    }
    return data;
}

std::vector<uint16_t> floatRgbaToHalf(const std::vector<float> &src)
{
    std::vector<uint16_t> dst(src.size());
    for (size_t i = 0; i < src.size(); ++i) dst[i] = floatToHalf(src[i]);
    return dst;
}

} // namespace

// ---------------------------------------------------------------------
// main
// ---------------------------------------------------------------------
int main(int argc, char **argv)
{
    const char *configPath = (argc >= 2)
        ? argv[1]
        : "assets/OCIO/Blender5.1/config.ocio";
    const char *inputArg   = (argc >= 3) ? argv[2] : nullptr;
    const char *displayArg = (argc >= 4) ? argv[3] : nullptr;
    const char *viewArg    = (argc >= 5) ? argv[4] : nullptr;

    // ----- OCIO setup ------------------------------------------------
    OCIO::ConstConfigRcPtr cfg;
    try {
        cfg = OCIO::Config::CreateFromFile(configPath);
    } catch (const OCIO::Exception &e) {
        std::fprintf(stderr, "OCIO config load failed: %s\n", e.what());
        return 1;
    }

    auto rolePicker = [&](const char *role) -> std::string {
        if (const char *cs = cfg->getRoleColorSpace(role); cs && *cs) return cs;
        return {};
    };

    std::string inputCs = inputArg ? inputArg : rolePicker("scene_linear");
    if (inputCs.empty()) inputCs = rolePicker("color_picking");
    if (inputCs.empty()) inputCs = cfg->getColorSpaceNameByIndex(0);

    std::string display = displayArg ? displayArg : cfg->getDefaultDisplay();
    std::string view = viewArg ? viewArg : cfg->getDefaultView(display.c_str());

    std::printf("=== probe-ocio-d3d11 — Phase F.1.a spike ===\n");
    std::printf("config:  %s\n", configPath);
    std::printf("input:   %s\n", inputCs.c_str());
    std::printf("display: %s\n", display.c_str());
    std::printf("view:    %s\n", view.c_str());
    std::printf("\n");

    OCIO::ConstProcessorRcPtr proc;
    try {
        auto dvt = OCIO::DisplayViewTransform::Create();
        dvt->setSrc(inputCs.c_str());
        dvt->setDisplay(display.c_str());
        dvt->setView(view.c_str());
        proc = cfg->getProcessor(dvt);
    } catch (const OCIO::Exception &e) {
        std::fprintf(stderr, "OCIO processor build failed: %s\n", e.what());
        return 1;
    }

    // CPU processor — the ground truth we'll diff against.
    OCIO::ConstCPUProcessorRcPtr cpuProc = proc->getDefaultCPUProcessor();
    OCIO::ConstGPUProcessorRcPtr gpuProc = proc->getDefaultGPUProcessor();

    // ----- Extract HLSL shader + LUT inventory -----------------------
    OCIO::GpuShaderDescRcPtr shaderDesc = OCIO::GpuShaderDesc::CreateShaderDesc();
    shaderDesc->setLanguage(OCIO::GPU_LANGUAGE_HLSL_SM_5_0);
    shaderDesc->setFunctionName("OCIODisplay");
    shaderDesc->setResourcePrefix("ocio_");

    try {
        gpuProc->extractGpuShaderInfo(shaderDesc);
    } catch (const OCIO::Exception &e) {
        std::fprintf(stderr, "OCIO extractGpuShaderInfo failed: %s\n", e.what());
        return 1;
    }

    std::string ocioHlsl = shaderDesc->getShaderText();
    const int n1d = shaderDesc->getNumTextures();
    const int n3d = shaderDesc->getNum3DTextures();
    const int nU  = shaderDesc->getNumUniforms();

    std::printf("OCIO HLSL emit:      %zu bytes\n", ocioHlsl.size());
    std::printf("1D/2D LUTs:          %d\n", n1d);
    std::printf("3D LUTs:             %d\n", n3d);
    std::printf("Uniforms:            %d\n", nU);
    std::printf("\n");

    // Dump for inspection regardless of pass/fail
    {
        std::ofstream dump("probe_ocio_d3d11.ocio.hlsl", std::ios::binary);
        if (dump) dump.write(ocioHlsl.data(),
                                static_cast<std::streamsize>(ocioHlsl.size()));
    }

    // Print LUT inventory (informational)
    struct LutInfo {
        bool                              is3d;
        std::string                       sampler;
        unsigned                          edge;     // 3D: edge length; 1D: width
        unsigned                          h;        // 1D height (1 or 2)
        OCIO::GpuShaderDesc::TextureType  channel;  // 1D only
        const float                      *values;
    };
    // CRITICAL ordering: OCIO 2.x's HLSL emitter declares 3D LUTs BEFORE
    // 1D/2D LUTs in the source text. D3DCompile assigns register slots
    // by declaration order when no explicit register() qualifier is
    // present — so 3D LUTs land at t1.., then 1D/2D at the following
    // slots. We MUST match that here when building the bind table.
    // Getting it backwards binds the wrong texture type at each slot
    // (1D data behind a Texture3D handle and vice versa) → wrong pixels.
    std::vector<LutInfo> luts;
    for (int i = 0; i < n3d; ++i) {
        const char *texName = nullptr, *samplerName = nullptr;
        unsigned edge = 0;
        OCIO::Interpolation interp = OCIO::INTERP_DEFAULT;
        shaderDesc->get3DTexture(static_cast<unsigned>(i), texName, samplerName, edge, interp);
        const float *values = nullptr;
        shaderDesc->get3DTextureValues(static_cast<unsigned>(i), values);
        luts.push_back({true, samplerName ? samplerName : "?", edge, 0,
                        OCIO::GpuShaderDesc::TEXTURE_RGB_CHANNEL, values});
        std::printf("  3D[%d] sampler=%s edge=%u\n", i, samplerName, edge);
    }
    for (int i = 0; i < n1d; ++i) {
        const char *texName = nullptr, *samplerName = nullptr;
        unsigned w = 0, h = 0;
        OCIO::GpuShaderDesc::TextureType chan = OCIO::GpuShaderDesc::TEXTURE_RGB_CHANNEL;
        OCIO::GpuShaderDesc::TextureDimensions dim = OCIO::GpuShaderDesc::TEXTURE_1D;
        OCIO::Interpolation interp = OCIO::INTERP_DEFAULT;
        shaderDesc->getTexture(static_cast<unsigned>(i), texName, samplerName,
                                w, h, chan, dim, interp);
        const float *values = nullptr;
        shaderDesc->getTextureValues(static_cast<unsigned>(i), values);
        luts.push_back({false, samplerName ? samplerName : "?", w, h, chan, values});
        std::printf("  1D[%d] sampler=%s w=%u h=%u chan=%s\n", i,
                    samplerName, w, h,
                    chan == OCIO::GpuShaderDesc::TEXTURE_RED_CHANNEL ? "R" : "RGB");
    }
    std::printf("\n");

    // ----- D3D11 device (headless, no swapchain) ----------------------
    ComPtr<ID3D11Device>        device;
    ComPtr<ID3D11DeviceContext> ctx;
    D3D_FEATURE_LEVEL           flOut = D3D_FEATURE_LEVEL_11_0;
    const D3D_FEATURE_LEVEL flReq[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        flReq, _countof(flReq), D3D11_SDK_VERSION,
        device.GetAddressOf(), &flOut, ctx.GetAddressOf());
    if (FAILED(hr)) die("D3D11CreateDevice", hr);
    std::printf("D3D11 device:        feature level 0x%X\n", flOut);

    // ----- Compile shaders --------------------------------------------
    const std::string psSource = buildPixelShaderHlsl(ocioHlsl, "OCIODisplay", {});
    {
        std::ofstream dump("probe_ocio_d3d11.full.hlsl", std::ios::binary);
        if (dump) dump.write(psSource.data(),
                                static_cast<std::streamsize>(psSource.size()));
        std::printf("Full HLSL dump:      probe_ocio_d3d11.full.hlsl\n");
    }
    ComPtr<ID3DBlob> psCode = compileHlsl(psSource, "PSMain", "ps_5_0", "PSMain");
    std::string vsSrc = kVertexHlsl;
    ComPtr<ID3DBlob> vsCode = compileHlsl(vsSrc, "VSMain", "vs_5_0", "VSMain");

    ComPtr<ID3D11VertexShader> vs;
    device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(),
                                 nullptr, vs.GetAddressOf());
    ComPtr<ID3D11PixelShader> ps;
    device->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(),
                                nullptr, ps.GetAddressOf());
    std::printf("Shaders compiled:    PS=%zu bytes  VS=%zu bytes\n",
                psCode->GetBufferSize(), vsCode->GetBufferSize());

    // ----- Create LUT textures + SRVs --------------------------------
    // OCIO's HLSL declarations use register(tN) where N starts at some
    // offset. We bind our LUTs at PS shader resource slots 1.. (slot 0 is
    // the source texture). Verify in the dumped HLSL that OCIO's registers
    // match.
    std::vector<ComPtr<ID3D11ShaderResourceView>> lutSrvs;
    std::vector<ComPtr<ID3D11SamplerState>>       lutSamplers;
    for (size_t li = 0; li < luts.size(); ++li) {
        const auto &lut = luts[li];
        ComPtr<ID3D11ShaderResourceView> srv;
        if (lut.is3d) {
            D3D11_TEXTURE3D_DESC td{};
            td.Width  = lut.edge;
            td.Height = lut.edge;
            td.Depth  = lut.edge;
            td.MipLevels = 1;
            td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
            td.Usage  = D3D11_USAGE_IMMUTABLE;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            // OCIO 3D LUT is RGB packed; D3D11 has no RGB32_FLOAT 3D, so we
            // unpack to RGBA32F. (RGB32 is 1D/2D only in D3D11.)
            const size_t voxels = static_cast<size_t>(lut.edge) * lut.edge * lut.edge;
            std::vector<float> rgba(voxels * 4);
            for (size_t v = 0; v < voxels; ++v) {
                rgba[v*4 + 0] = lut.values[v*3 + 0];
                rgba[v*4 + 1] = lut.values[v*3 + 1];
                rgba[v*4 + 2] = lut.values[v*3 + 2];
                rgba[v*4 + 3] = 1.0f;
            }
            std::vector<D3D11_SUBRESOURCE_DATA> initData(1);
            initData[0].pSysMem          = rgba.data();
            initData[0].SysMemPitch      = lut.edge * 4 * sizeof(float);
            initData[0].SysMemSlicePitch = lut.edge * lut.edge * 4 * sizeof(float);
            ComPtr<ID3D11Texture3D> tex;
            HRESULT thr = device->CreateTexture3D(&td, initData.data(), tex.GetAddressOf());
            if (FAILED(thr)) die("CreateTexture3D LUT3D", thr);
            D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = td.Format;
            sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
            sd.Texture3D.MipLevels = 1;
            device->CreateShaderResourceView(tex.Get(), &sd, srv.GetAddressOf());
        } else {
            // 1D LUT bound as Texture2D of (width, height=1 or 2).
            // Channel: RED_CHANNEL → R32_FLOAT; RGB → R32G32B32_FLOAT (1D/2D
            // OK). OCIO docs say each entry is one GPU texture.
            const unsigned w = lut.edge;
            const unsigned h = lut.h == 0 ? 1 : lut.h;
            DXGI_FORMAT fmt = DXGI_FORMAT_R32_FLOAT;
            int channels = 1;
            if (lut.channel == OCIO::GpuShaderDesc::TEXTURE_RGB_CHANNEL) {
                fmt = DXGI_FORMAT_R32G32B32_FLOAT;
                channels = 3;
            }
            const size_t pixels = static_cast<size_t>(w) * h;
            std::vector<float> data(pixels * 4);
            // Pack input into target format (memcpy works because OCIO
            // already stores R for RED_CHANNEL, RGB for RGB_CHANNEL)
            std::memcpy(data.data(), lut.values,
                         pixels * static_cast<size_t>(channels) * sizeof(float));

            D3D11_TEXTURE2D_DESC td{};
            td.Width  = w;
            td.Height = h;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format    = fmt;
            td.SampleDesc.Count = 1;
            td.Usage     = D3D11_USAGE_IMMUTABLE;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA initData{};
            initData.pSysMem     = data.data();
            initData.SysMemPitch = w * channels * sizeof(float);
            ComPtr<ID3D11Texture2D> tex;
            HRESULT thr = device->CreateTexture2D(&td, &initData, tex.GetAddressOf());
            if (FAILED(thr)) die("CreateTexture2D LUT1D", thr);
            D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = fmt;
            sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            sd.Texture2D.MipLevels = 1;
            device->CreateShaderResourceView(tex.Get(), &sd, srv.GetAddressOf());
        }
        lutSrvs.push_back(srv);

        // Sampler per LUT (linear filter, clamp)
        ComPtr<ID3D11SamplerState> sampler;
        D3D11_SAMPLER_DESC ss{};
        ss.Filter   = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        ss.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        ss.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        ss.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        ss.MaxLOD   = D3D11_FLOAT32_MAX;
        device->CreateSamplerState(&ss, sampler.GetAddressOf());
        lutSamplers.push_back(sampler);
    }
    std::printf("LUT textures:        %zu created\n", luts.size());

    // ----- Create source (input gradient) texture --------------------
    const std::vector<float>    inputFloat = buildGradientFloat(kTestSize, kTestSize);
    const std::vector<uint16_t> inputHalf  = floatRgbaToHalf(inputFloat);

    D3D11_TEXTURE2D_DESC srcDesc{};
    srcDesc.Width  = kTestSize;
    srcDesc.Height = kTestSize;
    srcDesc.MipLevels = 1;
    srcDesc.ArraySize = 1;
    srcDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    srcDesc.SampleDesc.Count = 1;
    srcDesc.Usage     = D3D11_USAGE_IMMUTABLE;
    srcDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA srcInit{};
    srcInit.pSysMem     = inputHalf.data();
    srcInit.SysMemPitch = kTestSize * 4 * sizeof(uint16_t);
    ComPtr<ID3D11Texture2D> srcTex;
    if (FAILED(hr = device->CreateTexture2D(&srcDesc, &srcInit, srcTex.GetAddressOf())))
        die("CreateTexture2D input", hr);
    ComPtr<ID3D11ShaderResourceView> srcSrv;
    if (FAILED(hr = device->CreateShaderResourceView(srcTex.Get(), nullptr, srcSrv.GetAddressOf())))
        die("CreateShaderResourceView input", hr);

    ComPtr<ID3D11SamplerState> srcSampler;
    {
        D3D11_SAMPLER_DESC ss{};
        ss.Filter   = D3D11_FILTER_MIN_MAG_MIP_POINT;  // exact pixel sample
        ss.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        ss.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        ss.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        ss.MaxLOD   = D3D11_FLOAT32_MAX;
        device->CreateSamplerState(&ss, srcSampler.GetAddressOf());
    }

    // ----- Create render target --------------------------------------
    D3D11_TEXTURE2D_DESC rtDesc = srcDesc;
    rtDesc.Usage     = D3D11_USAGE_DEFAULT;
    rtDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> rtTex;
    if (FAILED(hr = device->CreateTexture2D(&rtDesc, nullptr, rtTex.GetAddressOf())))
        die("CreateTexture2D RT", hr);
    ComPtr<ID3D11RenderTargetView> rtv;
    if (FAILED(hr = device->CreateRenderTargetView(rtTex.Get(), nullptr, rtv.GetAddressOf())))
        die("CreateRenderTargetView", hr);

    // ----- Render pass -----------------------------------------------
    ctx->VSSetShader(vs.Get(), nullptr, 0);
    ctx->PSSetShader(ps.Get(), nullptr, 0);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->IASetInputLayout(nullptr);

    D3D11_VIEWPORT vp{};
    vp.TopLeftX = 0; vp.TopLeftY = 0;
    vp.Width    = float(kTestSize);
    vp.Height   = float(kTestSize);
    vp.MinDepth = 0; vp.MaxDepth = 1;
    ctx->RSSetViewports(1, &vp);

    ctx->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
    const float clear[4] = {0, 0, 0, 0};
    ctx->ClearRenderTargetView(rtv.Get(), clear);

    // Bind source at t0/s0; LUTs at t1+/s1+.
    ID3D11ShaderResourceView *srvs[16] = {srcSrv.Get()};
    ID3D11SamplerState       *samps[16] = {srcSampler.Get()};
    for (size_t i = 0; i < lutSrvs.size() && i < 15; ++i) {
        srvs[i + 1]  = lutSrvs[i].Get();
        samps[i + 1] = lutSamplers[i].Get();
    }
    const UINT numSrvs = static_cast<UINT>(std::min<size_t>(1 + lutSrvs.size(), 16));
    ctx->PSSetShaderResources(0, numSrvs, srvs);
    ctx->PSSetSamplers(0, numSrvs, samps);

    ctx->Draw(3, 0);
    ctx->Flush();

    // ----- Stage RT back to CPU --------------------------------------
    D3D11_TEXTURE2D_DESC stagingDesc = srcDesc;
    stagingDesc.Usage     = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(hr = device->CreateTexture2D(&stagingDesc, nullptr, staging.GetAddressOf())))
        die("CreateTexture2D staging", hr);
    ctx->CopyResource(staging.Get(), rtTex.Get());

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(hr = ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
        die("Map staging", hr);

    std::vector<float> gpuFloat(static_cast<size_t>(kTestSize) * kTestSize * 4);
    {
        const uint8_t *row = static_cast<const uint8_t *>(mapped.pData);
        for (int y = 0; y < kTestSize; ++y) {
            const uint16_t *halfs = reinterpret_cast<const uint16_t *>(row);
            for (int x = 0; x < kTestSize; ++x) {
                const size_t outIdx = (static_cast<size_t>(y) * kTestSize + x) * 4;
                gpuFloat[outIdx + 0] = halfToFloat(halfs[x*4 + 0]);
                gpuFloat[outIdx + 1] = halfToFloat(halfs[x*4 + 1]);
                gpuFloat[outIdx + 2] = halfToFloat(halfs[x*4 + 2]);
                gpuFloat[outIdx + 3] = halfToFloat(halfs[x*4 + 3]);
            }
            row += mapped.RowPitch;
        }
    }
    ctx->Unmap(staging.Get(), 0);

    // ----- CPU reference (apply OCIO to the same gradient) -----------
    std::vector<float> cpuRef = inputFloat;
    {
        OCIO::PackedImageDesc desc(cpuRef.data(), kTestSize, kTestSize, 4);
        cpuProc->apply(desc);
    }

    // ----- Diff ------------------------------------------------------
    double sumAbs = 0.0, sumSq = 0.0;
    float  maxAbs = 0.0f;
    int    maxAbsX = 0, maxAbsY = 0, maxAbsCh = 0;
    int    overTol = 0;
    constexpr float kTol = 5e-3f;  // RGBA16F resolution + LUT linear interp
    for (int y = 0; y < kTestSize; ++y) {
        for (int x = 0; x < kTestSize; ++x) {
            for (int c = 0; c < 3; ++c) {
                const size_t i = (static_cast<size_t>(y) * kTestSize + x) * 4 + c;
                const float d = std::fabs(gpuFloat[i] - cpuRef[i]);
                sumAbs += d;
                sumSq  += static_cast<double>(d) * d;
                if (d > maxAbs) {
                    maxAbs = d; maxAbsX = x; maxAbsY = y; maxAbsCh = c;
                }
                if (d > kTol) ++overTol;
            }
        }
    }
    const double n   = double(kTestSize) * kTestSize * 3;
    const double avg = sumAbs / n;
    const double rms = std::sqrt(sumSq / n);

    std::printf("\n--- diff (GPU vs CPU reference) ---\n");
    std::printf("avg abs diff:        %.6f\n", avg);
    std::printf("rms diff:            %.6f\n", rms);
    std::printf("max abs diff:        %.6f  at (x=%d, y=%d, ch=%d)\n",
                maxAbs, maxAbsX, maxAbsY, maxAbsCh);
    std::printf("samples over tol:    %d / %d  (tol=%.4f)\n",
                overTol, static_cast<int>(n), kTol);

    if (maxAbs <= kTol) {
        std::printf("\nresult: PASS — OCIO HLSL output matches CPU reference within tolerance.\n");
        std::printf("Phase F.2+ D3D11 renderer rewrite is GREEN-LIT for OCIO integration.\n");
        return 0;
    } else {
        std::printf("\nresult: FAIL — max abs diff %.6f exceeds tolerance %.4f.\n", maxAbs, kTol);
        std::printf("Inspect probe_ocio_d3d11.full.hlsl and the GPU output for clues.\n");
        return 1;
    }
}
