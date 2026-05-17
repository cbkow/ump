// MetalCompositor — see header for adaptation notes.

#include "metal_compositor.h"

#include "metal_device_manager.h"

#import <Metal/Metal.h>

#include <QtLogging>

namespace qcv {

namespace {

// Two-source compositor. Fragment samples srcA / srcB based on the
// active mode + UV position. Per Guide 01 §5: writes a single
// composite (both halves come from the same render pass — atomic
// pair guarantee). Solo case: caller passes A as both srcA and
// srcB so SBS / Wipe still work as a no-op-compare.
constexpr const char *kCompositorMSL = R"(
#include <metal_stdlib>
using namespace metal;

struct UBO {
    float2 dstSize;      // canvas / drawable pixels
    float2 srcSizeA;     // source A pixels
    float2 srcSizeB;     // source B pixels (== srcSizeA in solo)
    int    mode;         // 0=Single, 1=SBS, 2=Wipe
    float  splitPos;     // 0..1 (Wipe only)
    int    aActive;      // 1 = sample srcA, 0 = transparent in A region
    int    bActive;      // 1 = sample srcB, 0 = transparent in B region
    float  brightness;   // linear-light multiplier; 1.0 = identity
};

struct VsOut {
    float4 position [[position]];
    float2 uv;
};

vertex VsOut vs_main(uint vid [[vertex_id]])
{
    constexpr float2 ndc[6] = {
        float2(-1.0, -1.0), float2( 1.0, -1.0), float2(-1.0,  1.0),
        float2(-1.0,  1.0), float2( 1.0, -1.0), float2( 1.0,  1.0)
    };
    constexpr float2 uvs[6] = {
        float2(0.0, 1.0), float2(1.0, 1.0), float2(0.0, 0.0),
        float2(0.0, 0.0), float2(1.0, 1.0), float2(1.0, 0.0)
    };
    VsOut o;
    o.position = float4(ndc[vid], 0.0, 1.0);
    o.uv       = uvs[vid];
    return o;
}

// Aspect-fit `srcSize` into the rect at `rectOrigin` of size
// `rectSize` (both in canvas pixels) and sample `src` at the
// fragment's `dstPx`. Returns float4(0) if the fragment falls
// outside the fitted rect (letterbox / pillarbox / outside region).
static inline float4 sampleFit(texture2d<float, access::sample> src,
                               sampler smp,
                               float2 dstPx,
                               float2 rectOrigin,
                               float2 rectSize,
                               float2 srcSize)
{
    float2 rectPxPos = dstPx - rectOrigin;
    if (rectPxPos.x < 0.0 || rectPxPos.x >= rectSize.x ||
        rectPxPos.y < 0.0 || rectPxPos.y >= rectSize.y) {
        return float4(0.0);
    }
    float scale = min(rectSize.x / srcSize.x, rectSize.y / srcSize.y);
    float2 scaledSrc = srcSize * scale;
    float2 offset    = (rectSize - scaledSrc) * 0.5;
    float2 inSrcPx   = (rectPxPos - offset) / scale;
    if (inSrcPx.x < 0.0 || inSrcPx.x >= srcSize.x ||
        inSrcPx.y < 0.0 || inSrcPx.y >= srcSize.y) {
        return float4(0.0);
    }
    return src.sample(smp, inSrcPx / srcSize);
}

fragment float4 fs_main(VsOut in [[stage_in]],
                        texture2d<float, access::sample> srcA [[texture(0)]],
                        texture2d<float, access::sample> srcB [[texture(1)]],
                        sampler smp [[sampler(0)]],
                        constant UBO &u [[buffer(0)]])
{
    const float2 uv    = in.uv;
    const float2 dstPx = uv * u.dstSize;

    float4 color = float4(0.0);

    if (u.mode == 1) {
        // Side-by-Side — left half = A, right half = B. Each side
        // gets aspect-fit into its half-rect independently so two
        // sources of different aspect both look right.
        const float halfW = u.dstSize.x * 0.5;
        if (dstPx.x < halfW) {
            color = (u.aActive == 0)
                ? float4(0.0)
                : sampleFit(srcA, smp, dstPx,
                            float2(0.0, 0.0),
                            float2(halfW, u.dstSize.y),
                            u.srcSizeA);
        } else {
            color = (u.bActive == 0)
                ? float4(0.0)
                : sampleFit(srcB, smp, dstPx,
                            float2(halfW, 0.0),
                            float2(halfW, u.dstSize.y),
                            u.srcSizeB);
        }
    } else if (u.mode == 2) {
        // Split-Wipe — full canvas; uv.x < splitPos picks A, else
        // B. Both aspect-fit to the full canvas so the two sources
        // overlap in screen-space; the wipe just toggles which one
        // wins per fragment. 1 px white separator at splitPos.
        if (uv.x < u.splitPos) {
            color = (u.aActive == 0)
                ? float4(0.0)
                : sampleFit(srcA, smp, dstPx,
                            float2(0.0, 0.0),
                            u.dstSize, u.srcSizeA);
        } else {
            color = (u.bActive == 0)
                ? float4(0.0)
                : sampleFit(srcB, smp, dstPx,
                            float2(0.0, 0.0),
                            u.dstSize, u.srcSizeB);
        }
        if (abs(uv.x - u.splitPos) * u.dstSize.x < 1.0) {
            return float4(1.0, 1.0, 1.0, 1.0);
        }
    } else {
        // Single — full canvas, srcA only.
        color = (u.aActive == 0)
            ? float4(0.0)
            : sampleFit(srcA, smp, dstPx,
                        float2(0.0, 0.0),
                        u.dstSize, u.srcSizeA);
    }

    // Letterbox / pillarbox / outside source area → discard so the
    // background pass underneath shows through. Sample-fit returns
    // float4(0) (alpha=0) for those regions.
    if (color.a == 0.0) discard_fragment();
    color.rgb *= u.brightness;
    return color;
}
)";

// Viewport background fill — solid color or 2-tile checkerboard.
// Matches old QCView's DrawVideoBackground (panel_viewport.cpp:1594).
constexpr const char *kBackgroundMSL = R"(
#include <metal_stdlib>
using namespace metal;

struct BgUBO {
    float2 dstSize;
    int    mode;        // 0 black, 1 darkgray, 2 darkChecker, 3 lightChecker
    float  tilePixels;  // checkerboard tile size in device pixels
};

struct VsOut {
    float4 position [[position]];
    float2 uv;
};

vertex VsOut bg_vs(uint vid [[vertex_id]])
{
    constexpr float2 ndc[6] = {
        float2(-1.0, -1.0), float2( 1.0, -1.0), float2(-1.0,  1.0),
        float2(-1.0,  1.0), float2( 1.0, -1.0), float2( 1.0,  1.0)
    };
    constexpr float2 uvs[6] = {
        float2(0.0, 1.0), float2(1.0, 1.0), float2(0.0, 0.0),
        float2(0.0, 0.0), float2(1.0, 1.0), float2(1.0, 0.0)
    };
    VsOut o;
    o.position = float4(ndc[vid], 0.0, 1.0);
    o.uv       = uvs[vid];
    return o;
}

fragment float4 bg_fs(VsOut in [[stage_in]],
                      constant BgUBO &u [[buffer(0)]])
{
    if (u.mode == 0) {
        return float4(0.0, 0.0, 0.0, 1.0);
    }
    if (u.mode == 1) {
        // 27/255 = 0.1058 — old app DEFAULT
        return float4(0.1058, 0.1058, 0.1058, 1.0);
    }

    // Checkerboard — pixel-coord based so tile size is constant
    // regardless of the drawable's logical aspect.
    const float2 px = in.uv * u.dstSize;
    const float tile = max(8.0, u.tilePixels);
    int cx = int(floor(px.x / tile));
    int cy = int(floor(px.y / tile));
    bool even = ((cx + cy) & 1) == 0;

    if (u.mode == 3) {
        // Light checker: 200/255 vs ~UI_LIGHT_GRAY (~178/255)
        return even ? float4(0.7843, 0.7843, 0.7843, 1.0)
                    : float4(0.6980, 0.6980, 0.6980, 1.0);
    }
    // Dark checker: 30/255 vs 20/255 — the most common review choice
    return even ? float4(0.1176, 0.1176, 0.1176, 1.0)
                : float4(0.0784, 0.0784, 0.0784, 1.0);
}
)";

// Full SMPTE EG 1-1990 color bars — three-band layout used as the
// "no source yet" placeholder. Self-contained; the fragment derives
// everything from UV.
//
//   Top    (0.000–0.667 v): 7 vertical bars at 75% — white, yellow,
//                            cyan, green, magenta, red, blue.
//   Middle (0.667–0.750 v): chroma-reverse strip — blue, black,
//                            magenta, black, cyan, black, white-75
//                            (matches the field's chroma colors
//                            inverted left-to-right).
//   Bottom (0.750–1.000 v): -I, white-100, +Q, then black with the
//                            PLUGE pattern (subblack 3.5% / black
//                            7.5% / superblack 11.5%).
constexpr const char *kColorBarsMSL = R"(
#include <metal_stdlib>
using namespace metal;

struct BarsUBO {
    float2 dstSize;
};

struct VsOut {
    float4 position [[position]];
    float2 uv;
};

vertex VsOut bars_vs(uint vid [[vertex_id]])
{
    constexpr float2 ndc[6] = {
        float2(-1.0, -1.0), float2( 1.0, -1.0), float2(-1.0,  1.0),
        float2(-1.0,  1.0), float2( 1.0, -1.0), float2( 1.0,  1.0)
    };
    constexpr float2 uvs[6] = {
        float2(0.0, 1.0), float2(1.0, 1.0), float2(0.0, 0.0),
        float2(0.0, 0.0), float2(1.0, 1.0), float2(1.0, 0.0)
    };
    VsOut o;
    o.position = float4(ndc[vid], 0.0, 1.0);
    o.uv       = uvs[vid];
    return o;
}

fragment float4 bars_fs(VsOut in [[stage_in]],
                        constant BarsUBO &u [[buffer(0)]])
{
    const float u_x = in.uv.x;
    const float v   = in.uv.y;

    // -- Top band: 7 color bars, 75% ----------------------------------
    if (v < (2.0 / 3.0)) {
        constexpr float3 bars[7] = {
            float3(0.75, 0.75, 0.75),  // White 75
            float3(0.75, 0.75, 0.00),  // Yellow
            float3(0.00, 0.75, 0.75),  // Cyan
            float3(0.00, 0.75, 0.00),  // Green
            float3(0.75, 0.00, 0.75),  // Magenta
            float3(0.75, 0.00, 0.00),  // Red
            float3(0.00, 0.00, 0.75),  // Blue
        };
        int idx = clamp(int(u_x * 7.0), 0, 6);
        return float4(bars[idx], 1.0);
    }

    // -- Middle band: chroma-reverse strip ----------------------------
    if (v < 0.75) {
        constexpr float3 rev[7] = {
            float3(0.00, 0.00, 0.75),   // Blue
            float3(0.075, 0.075, 0.075),// Black 7.5%
            float3(0.75, 0.00, 0.75),   // Magenta
            float3(0.075, 0.075, 0.075),
            float3(0.00, 0.75, 0.75),   // Cyan
            float3(0.075, 0.075, 0.075),
            float3(0.75, 0.75, 0.75),   // White 75
        };
        int idx = clamp(int(u_x * 7.0), 0, 6);
        return float4(rev[idx], 1.0);
    }

    // -- Bottom band: -I / 100% white / +Q / PLUGE --------------------
    // Sections (in u space): 0–0.25 -I, 0.25–0.50 white, 0.50–0.75 +Q,
    // 0.75–1.00 black + PLUGE pattern.
    if (u_x < 0.25) {
        return float4(0.00, 0.184, 0.349, 1.0);   // -I
    }
    if (u_x < 0.50) {
        return float4(1.00, 1.00, 1.00, 1.0);     // White 100
    }
    if (u_x < 0.75) {
        return float4(0.290, 0.00, 0.455, 1.0);   // +Q
    }

    // PLUGE quarter: 4 sub-zones across [0.75, 1.0]
    const float local = (u_x - 0.75) / 0.25;
    if (local < 0.40) {
        return float4(0.075, 0.075, 0.075, 1.0); // Black 7.5% lead-in
    }
    if (local < 0.55) {
        return float4(0.035, 0.035, 0.035, 1.0); // Subblack 3.5%
    }
    if (local < 0.70) {
        return float4(0.075, 0.075, 0.075, 1.0); // Black 7.5%
    }
    if (local < 0.85) {
        return float4(0.115, 0.115, 0.115, 1.0); // Superblack 11.5%
    }
    return float4(0.075, 0.075, 0.075, 1.0);     // Black 7.5% trailer
}
)";

} // namespace

struct MetalCompositor::Impl {
    id<MTLRenderPipelineState> pipeline       = nil;
    id<MTLSamplerState>        sampler        = nil;
    int                        targetPixelFmt = 0;     // MTLPixelFormat as int

    // Bars placeholder. Built on first renderColorBars() call so it
    // doesn't add cost when never used.
    id<MTLRenderPipelineState> barsPipeline    = nil;
    int                        barsPixelFmt    = 0;

    // Background fill (lazy, same rebuild-on-format-change rule).
    id<MTLRenderPipelineState> bgPipeline      = nil;
    int                        bgPixelFmt      = 0;

    float                      brightness      = 1.0f;
};

MetalCompositor::MetalCompositor()
    : m_impl(new Impl())
{
}

MetalCompositor::~MetalCompositor()
{
    shutdown();
    delete m_impl;
}

void MetalCompositor::setBrightness(float brightness)
{
    if (m_impl) m_impl->brightness = brightness;
}

bool MetalCompositor::initialize(int targetPixelFormat,
                                  bool enableSrcOverBlending)
{
    if (m_impl->pipeline) return true;

    auto &mgr = MetalDeviceManager::instance();
    id<MTLDevice> device = (__bridge id<MTLDevice>)mgr.device();
    if (!device) {
        qWarning("MetalCompositor: no Metal device");
        return false;
    }

    NSError *err = nil;
    NSString *src = [NSString stringWithUTF8String:kCompositorMSL];
    id<MTLLibrary> lib = [device newLibraryWithSource:src options:nil error:&err];
    if (!lib) {
        qWarning("MetalCompositor: shader compile failed: %s",
                 err ? [err.localizedDescription UTF8String] : "(no error)");
        return false;
    }
    id<MTLFunction> vsFn = [lib newFunctionWithName:@"vs_main"];
    id<MTLFunction> fsFn = [lib newFunctionWithName:@"fs_main"];
    if (!vsFn || !fsFn) {
        qWarning("MetalCompositor: vs_main/fs_main not found in library");
        return false;
    }

    MTLRenderPipelineDescriptor *desc = [MTLRenderPipelineDescriptor new];
    desc.vertexFunction   = vsFn;
    desc.fragmentFunction = fsFn;
    desc.colorAttachments[0].pixelFormat =
        static_cast<MTLPixelFormat>(targetPixelFormat);

    if (enableSrcOverBlending) {
        // Standard non-premultiplied source-over for the present
        // pass: source modulated by its own alpha blends over the
        // bg fill drawn earlier in the same render pass. Output
        // alpha settles at 1 because the bg is opaque, so the
        // swapchain stays fully opaque and macOS doesn't drag the
        // desktop through the source's alpha.
        desc.colorAttachments[0].blendingEnabled             = YES;
        desc.colorAttachments[0].rgbBlendOperation           = MTLBlendOperationAdd;
        desc.colorAttachments[0].alphaBlendOperation         = MTLBlendOperationAdd;
        desc.colorAttachments[0].sourceRGBBlendFactor        = MTLBlendFactorSourceAlpha;
        desc.colorAttachments[0].destinationRGBBlendFactor   = MTLBlendFactorOneMinusSourceAlpha;
        desc.colorAttachments[0].sourceAlphaBlendFactor      = MTLBlendFactorOne;
        desc.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    }

    id<MTLRenderPipelineState> pso =
        [device newRenderPipelineStateWithDescriptor:desc error:&err];
    if (!pso) {
        qWarning("MetalCompositor: pipeline creation failed: %s",
                 err ? [err.localizedDescription UTF8String] : "(no error)");
        return false;
    }

    MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
    sd.minFilter = MTLSamplerMinMagFilterLinear;
    sd.magFilter = MTLSamplerMinMagFilterLinear;
    sd.sAddressMode = MTLSamplerAddressModeClampToEdge;
    sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
    id<MTLSamplerState> smp = [device newSamplerStateWithDescriptor:sd];

    m_impl->pipeline       = pso;
    m_impl->sampler        = smp;
    m_impl->targetPixelFmt = targetPixelFormat;
    return true;
}

void MetalCompositor::shutdown()
{
    if (m_impl) {
        m_impl->pipeline       = nil;
        m_impl->sampler        = nil;
        m_impl->targetPixelFmt = 0;
        m_impl->barsPipeline   = nil;
        m_impl->barsPixelFmt   = 0;
        m_impl->bgPipeline     = nil;
        m_impl->bgPixelFmt     = 0;
    }
}

bool MetalCompositor::isInitialized() const
{
    return m_impl && m_impl->pipeline != nil;
}

int MetalCompositor::targetPixelFormat() const
{
    return m_impl ? m_impl->targetPixelFmt : 0;
}

void MetalCompositor::renderSources(void *encoderPtr,
                                    void *srcAPtr, int srcAW, int srcAH,
                                    void *srcBPtr, int srcBW, int srcBH,
                                    int dstWidth, int dstHeight,
                                    int mode, float splitPos,
                                    bool aActive, bool bActive)
{
    if (!isInitialized() || !encoderPtr) return;
    if (dstWidth <= 0 || dstHeight <= 0) return;

    // "Intentionally inactive" → drop that side's pointer + size.
    // The MSL renders its region transparent without mirroring the
    // other side. (Distinct from "expected but no texture yet" —
    // single-source case below still mirrors so SBS / Wipe show
    // the same image on both halves.)
    if (!aActive) { srcAPtr = nullptr; srcAW = 0; srcAH = 0; }
    if (!bActive) { srcBPtr = nullptr; srcBW = 0; srcBH = 0; }

    // If both sides are inactive, nothing to draw — background
    // pass already covered the canvas.
    if (!aActive && !bActive) return;
    // At least one bound texture is required for the pipeline.
    if (!srcAPtr && !srcBPtr) return;

    id<MTLRenderCommandEncoder> enc =
        (__bridge id<MTLRenderCommandEncoder>)encoderPtr;
    id<MTLTexture> texA = srcAPtr ? (__bridge id<MTLTexture>)srcAPtr : nil;
    id<MTLTexture> texB = srcBPtr ? (__bridge id<MTLTexture>)srcBPtr : nil;
    // Mirror a single-side load into both slots so the MSL's
    // single-fragment branch always has *something* to sample —
    // but only when the missing side is *expected* (active).
    // For an inactive side we leave its slot empty; the shader's
    // `aActive == 0` / `bActive == 0` test returns transparent.
    if (!texA && texB && aActive) {
        texA = texB;
        srcAW = srcBW; srcAH = srcBH;
    } else if (!texA) {
        // Bind B (or any non-nil texture) to texA's slot so the
        // pipeline isn't fed a nil sampler argument; the shader
        // never reads it because aActive == 0.
        texA = texB;
        srcAW = std::max(srcAW, 1); srcAH = std::max(srcAH, 1);
    }
    if (!texB && texA && bActive) {
        texB = texA;
        srcBW = srcAW; srcBH = srcAH;
    } else if (!texB) {
        texB = texA;
        srcBW = std::max(srcBW, 1); srcBH = std::max(srcBH, 1);
    }
    if (srcAW <= 0 || srcAH <= 0) { srcAW = dstWidth; srcAH = dstHeight; }
    if (srcBW <= 0 || srcBH <= 0) { srcBW = dstWidth; srcBH = dstHeight; }

    struct UBO {
        float dstSizeX, dstSizeY;
        float srcSizeAX, srcSizeAY;
        float srcSizeBX, srcSizeBY;
        int   mode;
        float splitPos;
        int   aActive;
        int   bActive;
        float brightness;
    } ubo = {
        static_cast<float>(dstWidth),  static_cast<float>(dstHeight),
        static_cast<float>(srcAW),     static_cast<float>(srcAH),
        static_cast<float>(srcBW),     static_cast<float>(srcBH),
        mode, std::clamp(splitPos, 0.0f, 1.0f),
        aActive ? 1 : 0,
        bActive ? 1 : 0,
        m_impl->brightness,
    };

    [enc setRenderPipelineState:m_impl->pipeline];
    [enc setFragmentTexture:texA atIndex:0];
    [enc setFragmentTexture:texB atIndex:1];
    [enc setFragmentSamplerState:m_impl->sampler atIndex:0];
    [enc setFragmentBytes:&ubo length:sizeof(ubo) atIndex:0];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
}

void MetalCompositor::renderCornerOverlay(void *encoderPtr,
                                           void *thumbPtr,
                                           int thumbW, int thumbH,
                                           int dstWidth, int dstHeight,
                                           int corner,
                                           float overlayFrac,
                                           float marginPx)
{
    // Reuses the same composite pipeline + Single-mode shader path.
    // We set the Metal viewport to the corner rect; the shader sees
    // a tiny canvas via UBO.dstSize and aspect-fits the thumb into
    // it. Caller must NOT issue further draws into this encoder
    // expecting the full-canvas viewport — call this last in the
    // pass (renderer does, right before endEncoding before OCIO).
    if (!isInitialized() || !encoderPtr || !thumbPtr) return;
    if (dstWidth <= 0 || dstHeight <= 0) return;
    if (thumbW <= 0 || thumbH <= 0) return;

    overlayFrac = std::clamp(overlayFrac, 0.05f, 0.5f);
    if (marginPx < 0.0f) marginPx = 0.0f;

    // Corner box dimensions. Width = dstWidth * overlayFrac;
    // height follows the THUMB aspect — that way landscape thumbs
    // get a wider box and portrait thumbs a taller one. Letterbox
    // is handled by the shader's sampleFit anyway, but matching the
    // box to the thumb keeps the visual tight.
    const float boxW = static_cast<float>(dstWidth) * overlayFrac;
    const float aspect =
        static_cast<float>(thumbW) / static_cast<float>(thumbH);
    float boxH = boxW / std::max(aspect, 0.0001f);
    if (boxH > dstHeight * 0.5f) {
        boxH = dstHeight * 0.5f;
    }

    // Origin in viewport coords (Metal viewport origin = top-left).
    float originX = marginPx;
    float originY = static_cast<float>(dstHeight) - boxH - marginPx;
    if (corner == 1) {       // BR
        originX = static_cast<float>(dstWidth) - boxW - marginPx;
    }
    if (originX < 0) originX = 0;
    if (originY < 0) originY = 0;

    id<MTLRenderCommandEncoder> enc =
        (__bridge id<MTLRenderCommandEncoder>)encoderPtr;
    id<MTLTexture> tex = (__bridge id<MTLTexture>)thumbPtr;

    MTLViewport vp;
    vp.originX = originX;
    vp.originY = originY;
    vp.width   = boxW;
    vp.height  = boxH;
    vp.znear   = 0.0;
    vp.zfar    = 1.0;
    [enc setViewport:vp];

    struct UBO {
        float dstSizeX, dstSizeY;
        float srcSizeAX, srcSizeAY;
        float srcSizeBX, srcSizeBY;
        int   mode;
        float splitPos;
        int   aActive;
        int   bActive;
        float brightness;
    } ubo = {
        boxW, boxH,
        static_cast<float>(thumbW), static_cast<float>(thumbH),
        static_cast<float>(thumbW), static_cast<float>(thumbH),
        /*Mode=*/0,
        0.5f,
        /*aActive=*/1,
        /*bActive=*/0,
        m_impl->brightness,
    };

    [enc setRenderPipelineState:m_impl->pipeline];
    [enc setFragmentTexture:tex atIndex:0];
    [enc setFragmentTexture:tex atIndex:1];
    [enc setFragmentSamplerState:m_impl->sampler atIndex:0];
    [enc setFragmentBytes:&ubo length:sizeof(ubo) atIndex:0];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle
            vertexStart:0 vertexCount:6];
}

void MetalCompositor::renderBackground(void *encoderPtr, int mode,
                                       int dstWidth, int dstHeight,
                                       float tilePixels)
{
    if (!m_impl || !encoderPtr) return;
    if (dstWidth <= 0 || dstHeight <= 0) return;

    if (!m_impl->bgPipeline ||
        m_impl->bgPixelFmt != m_impl->targetPixelFmt) {
        auto &mgr = MetalDeviceManager::instance();
        id<MTLDevice> device = (__bridge id<MTLDevice>)mgr.device();
        if (!device) return;

        NSError *err = nil;
        NSString *src = [NSString stringWithUTF8String:kBackgroundMSL];
        id<MTLLibrary> lib =
            [device newLibraryWithSource:src options:nil error:&err];
        if (!lib) {
            qWarning("MetalCompositor: bg shader compile failed: %s",
                     err ? [err.localizedDescription UTF8String] : "(no error)");
            return;
        }
        id<MTLFunction> vsFn = [lib newFunctionWithName:@"bg_vs"];
        id<MTLFunction> fsFn = [lib newFunctionWithName:@"bg_fs"];
        if (!vsFn || !fsFn) {
            qWarning("MetalCompositor: bg functions not found");
            return;
        }

        MTLRenderPipelineDescriptor *desc = [MTLRenderPipelineDescriptor new];
        desc.vertexFunction   = vsFn;
        desc.fragmentFunction = fsFn;
        desc.colorAttachments[0].pixelFormat =
            static_cast<MTLPixelFormat>(m_impl->targetPixelFmt);

        id<MTLRenderPipelineState> pso =
            [device newRenderPipelineStateWithDescriptor:desc error:&err];
        if (!pso) {
            qWarning("MetalCompositor: bg pipeline creation failed: %s",
                     err ? [err.localizedDescription UTF8String] : "(no error)");
            return;
        }
        m_impl->bgPipeline = pso;
        m_impl->bgPixelFmt = m_impl->targetPixelFmt;
    }

    id<MTLRenderCommandEncoder> enc =
        (__bridge id<MTLRenderCommandEncoder>)encoderPtr;

    struct BgUBO {
        float dstSizeX, dstSizeY;
        int   mode;
        float tilePixels;
    } ubo = {
        static_cast<float>(dstWidth),
        static_cast<float>(dstHeight),
        std::max(0, std::min(3, mode)),
        std::max(8.0f, tilePixels),
    };

    [enc setRenderPipelineState:m_impl->bgPipeline];
    [enc setFragmentBytes:&ubo length:sizeof(ubo) atIndex:0];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
}

void MetalCompositor::renderColorBars(void *encoderPtr,
                                      int dstWidth, int dstHeight)
{
    if (!m_impl || !encoderPtr) return;
    if (dstWidth <= 0 || dstHeight <= 0) return;

    // Lazy build / re-bake on pixel-format change. The main compositor
    // pipeline is already (re)built when the layer's pixel format
    // flips (HDR toggle); piggy-back on that signal via
    // targetPixelFmt to know when to rebuild this one too.
    if (!m_impl->barsPipeline ||
        m_impl->barsPixelFmt != m_impl->targetPixelFmt) {
        auto &mgr = MetalDeviceManager::instance();
        id<MTLDevice> device = (__bridge id<MTLDevice>)mgr.device();
        if (!device) return;

        NSError *err = nil;
        NSString *src = [NSString stringWithUTF8String:kColorBarsMSL];
        id<MTLLibrary> lib =
            [device newLibraryWithSource:src options:nil error:&err];
        if (!lib) {
            qWarning("MetalCompositor: bars shader compile failed: %s",
                     err ? [err.localizedDescription UTF8String] : "(no error)");
            return;
        }
        id<MTLFunction> vsFn = [lib newFunctionWithName:@"bars_vs"];
        id<MTLFunction> fsFn = [lib newFunctionWithName:@"bars_fs"];
        if (!vsFn || !fsFn) {
            qWarning("MetalCompositor: bars functions not found");
            return;
        }

        MTLRenderPipelineDescriptor *desc = [MTLRenderPipelineDescriptor new];
        desc.vertexFunction   = vsFn;
        desc.fragmentFunction = fsFn;
        desc.colorAttachments[0].pixelFormat =
            static_cast<MTLPixelFormat>(m_impl->targetPixelFmt);

        id<MTLRenderPipelineState> pso =
            [device newRenderPipelineStateWithDescriptor:desc error:&err];
        if (!pso) {
            qWarning("MetalCompositor: bars pipeline creation failed: %s",
                     err ? [err.localizedDescription UTF8String] : "(no error)");
            return;
        }
        m_impl->barsPipeline = pso;
        m_impl->barsPixelFmt = m_impl->targetPixelFmt;
    }

    id<MTLRenderCommandEncoder> enc =
        (__bridge id<MTLRenderCommandEncoder>)encoderPtr;

    struct BarsUBO {
        float dstSizeX, dstSizeY;
    } ubo = {
        static_cast<float>(dstWidth),
        static_cast<float>(dstHeight),
    };

    [enc setRenderPipelineState:m_impl->barsPipeline];
    [enc setFragmentBytes:&ubo length:sizeof(ubo) atIndex:0];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
}

} // namespace qcv
