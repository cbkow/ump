// DualCompositor — see header for adaptation notes.

#include "dual_compositor.h"

#include "dual/dual_playback_controller.h"
#include "dual/i_dual_pixbuf_converter.h"
#include "render/metal/metal_device_manager.h"

#import <Metal/Metal.h>
#include <QImage>
#include <QtLogging>
#include <chrono>
#include <cmath>
#include <utility>

namespace qcv::dual {

namespace {

// Same shader topology as qcv::MetalCompositor — SBS/Wipe/Single
// with aActive/bActive transparency. Past-end-of-source flag flows
// in via aActive=0 (drops sample, returns float4(0)). Discard at
// the end means letterbox / past-end / outside source rect all let
// the background pass underneath show through.
constexpr const char *kDualCompositorMSL = R"(
#include <metal_stdlib>
using namespace metal;

struct UBO {
    float2 dstSize;
    float2 srcSizeA;
    float2 srcSizeB;
    int    mode;       // 0=Single, 1=SBS, 2=Wipe, 3=Difference
    float  splitPos;
    int    aActive;
    int    bActive;
    float  seamHighlight;  // split seam: 0 = faint grey, 1 = white
    float  diffGain;       // Difference mode: amplify abs(A-B)
    int    rotA;           // per-side display rotation, quarter-turns
    int    rotB;           //   CW {0..3}; srcSize arrives display-swapped
};

struct VsOut {
    float4 position [[position]];
    float2 uv;
};

vertex VsOut dual_vs(uint vid [[vertex_id]])
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

static inline float4 sampleFit(texture2d<float, access::sample> src,
                               sampler smp,
                               float2 dstPx,
                               float2 rectOrigin,
                               float2 rectSize,
                               float2 srcSize,
                               int rot)
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
    // srcSize arrives display-swapped for odd quarter-turns; inverse-
    // rotate the normalized display UV back onto the stored texture.
    float2 t = inSrcPx / srcSize;
    if (rot == 1)      t = float2(t.y, 1.0 - t.x);          //  90 CW
    else if (rot == 2) t = float2(1.0 - t.x, 1.0 - t.y);    // 180
    else if (rot == 3) t = float2(1.0 - t.y, t.x);          // 270 CW
    return src.sample(smp, t);
}

fragment float4 dual_fs(VsOut in [[stage_in]],
                        texture2d<float, access::sample> srcA [[texture(0)]],
                        texture2d<float, access::sample> srcB [[texture(1)]],
                        sampler smp [[sampler(0)]],
                        constant UBO &u [[buffer(0)]])
{
    const float2 uv    = in.uv;
    const float2 dstPx = uv * u.dstSize;

    float4 color = float4(0.0);

    if (u.mode == 1) {
        const float halfW = u.dstSize.x * 0.5;
        if (dstPx.x < halfW) {
            color = (u.aActive == 0)
                ? float4(0.0)
                : sampleFit(srcA, smp, dstPx,
                            float2(0.0, 0.0),
                            float2(halfW, u.dstSize.y),
                            u.srcSizeA, u.rotA);
        } else {
            color = (u.bActive == 0)
                ? float4(0.0)
                : sampleFit(srcB, smp, dstPx,
                            float2(halfW, 0.0),
                            float2(halfW, u.dstSize.y),
                            u.srcSizeB, u.rotB);
        }
        // Faint grey divider between the two sides.
        if (abs(uv.x - 0.5) * u.dstSize.x < 1.0) {
            return float4(0.4, 0.4, 0.4, 1.0);
        }
    } else if (u.mode == 2) {
        if (uv.x < u.splitPos) {
            color = (u.aActive == 0)
                ? float4(0.0)
                : sampleFit(srcA, smp, dstPx,
                            float2(0.0, 0.0),
                            u.dstSize, u.srcSizeA, u.rotA);
        } else {
            color = (u.bActive == 0)
                ? float4(0.0)
                : sampleFit(srcB, smp, dstPx,
                            float2(0.0, 0.0),
                            u.dstSize, u.srcSizeB, u.rotB);
        }
        // Split seam — faint grey at rest, full white on hover/drag.
        if (abs(uv.x - u.splitPos) * u.dstSize.x < 1.0) {
            float3 seam = mix(float3(0.4, 0.4, 0.4), float3(1.0, 1.0, 1.0),
                              saturate(u.seamHighlight));
            return float4(seam, 1.0);
        }
    } else if (u.mode == 3) {
        // Difference (Adobe-style) — both sides fit the FULL canvas,
        // output |A - B| per channel * gain. Perfectly aligned same-res
        // pair → 0 → black; gain stays black at 0 (gain * 0 = 0).
        float4 ca = (u.aActive == 0)
            ? float4(0.0)
            : sampleFit(srcA, smp, dstPx,
                        float2(0.0, 0.0), u.dstSize, u.srcSizeA,
                        u.rotA);
        float4 cb = (u.bActive == 0)
            ? float4(0.0)
            : sampleFit(srcB, smp, dstPx,
                        float2(0.0, 0.0), u.dstSize, u.srcSizeB,
                        u.rotB);
        color = float4(abs(ca.rgb - cb.rgb) * u.diffGain,
                       max(ca.a, cb.a));
    } else {
        color = (u.aActive == 0)
            ? float4(0.0)
            : sampleFit(srcA, smp, dstPx,
                        float2(0.0, 0.0),
                        u.dstSize, u.srcSizeA, u.rotA);
    }

    if (color.a == 0.0) discard_fragment();
    return color;
}
)";

// Cold-transition spinner. Rendered when neither side has its
// frame buffered yet (Single→Dual entry, big-seek warmup). A
// rotating ring of 8 dots in the canvas center, animated via the
// `time` uniform (seconds since spinner started).
constexpr const char *kSpinnerMSL = R"(
#include <metal_stdlib>
using namespace metal;

struct SpinUBO {
    float2 dstSize;
    float  time;       // seconds since transition start
};

struct VsOut {
    float4 position [[position]];
    float2 uv;
};

vertex VsOut spin_vs(uint vid [[vertex_id]])
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

fragment float4 spin_fs(VsOut in [[stage_in]],
                        constant SpinUBO &u [[buffer(0)]])
{
    // Background fill — match the player's default DarkGray
    // (#161616 / 0.0863, 22/255 — Theme.bg).
    float4 bg = float4(0.0863, 0.0863, 0.0863, 1.0);

    const float2 px = in.uv * u.dstSize;
    const float2 c  = u.dstSize * 0.5;
    const float  r  = min(u.dstSize.x, u.dstSize.y) * 0.03;   // ring radius (half of original 0.06)
    const float  dotR = r * 0.18;                              // dot radius

    // 8 dots equally spaced on the ring, rotating once every 1.2 s.
    const int   N = 8;
    const float rot = -u.time * 6.2831853 / 1.2;
    float minDist = 1e6;
    int   minIdx  = 0;
    for (int i = 0; i < N; ++i) {
        float ang = rot + (6.2831853 * float(i) / float(N));
        float2 dotPos = c + float2(cos(ang), sin(ang)) * r;
        float d = distance(px, dotPos);
        if (d < minDist) {
            minDist = d;
            minIdx  = i;
        }
    }
    if (minDist < dotR) {
        // Dot brightness eases with index — leading dot brightest.
        // (i=0 == leading). Tint with the app's cobalt accent
        // (#0189f1 ≈ 0.0039 / 0.5373 / 0.9451) modulated by the
        // brightness ramp.
        const float brightness = 0.35 + 0.65 * (1.0 - float(minIdx) / float(N - 1));
        const float3 accent = float3(0.0039, 0.5373, 0.9451);
        float fade = smoothstep(dotR, dotR * 0.55, minDist);
        return mix(bg, float4(accent * brightness, 1.0), fade);
    }
    return bg;
}
)";

struct CachedSideTexture {
    id<MTLTexture> texture = nil;
    int  width  = 0;
    int  height = 0;
    // Tracks the Metal pixel format the cache was created at. When
    // a Cpu-kind frame's QImage format changes (e.g. SDR PNG ↔ EXR
    // FP16), we recreate the texture at the matching format so the
    // replaceRegion bytes line up.
    MTLPixelFormat format = MTLPixelFormatRGBA8Unorm;

    bool fits(int w, int h, MTLPixelFormat fmt) const {
        return texture && width == w && height == h && format == fmt;
    }

    void recreate(id<MTLDevice> device, int w, int h, MTLPixelFormat fmt) {
        MTLTextureDescriptor *desc = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:fmt
                                          width:w
                                         height:h
                                      mipmapped:NO];
        desc.usage       = MTLTextureUsageShaderRead;
        desc.storageMode = MTLStorageModeShared;
        texture = [device newTextureWithDescriptor:desc];
        width   = w;
        height  = h;
        format  = fmt;
    }

    // QImage byte layout matches Metal pixel format 1:1 here:
    //   Format_RGBA8888    → MTLPixelFormatRGBA8Unorm    (4 B/px)
    //   Format_RGBA16FPx4  → MTLPixelFormatRGBA16Float   (8 B/px)
    void uploadFromQImage(const QImage &img) {
        if (!texture || img.isNull()) return;
        const int rowBytes = static_cast<int>(img.bytesPerLine());
        MTLRegion region = MTLRegionMake2D(0, 0, img.width(), img.height());
        [texture replaceRegion:region
                   mipmapLevel:0
                     withBytes:img.constBits()
                   bytesPerRow:rowBytes];
    }
};

} // namespace

struct DualCompositor::Impl {
    id<MTLRenderPipelineState> pipeline       = nil;
    id<MTLSamplerState>        sampler        = nil;
    int                        targetPixelFmt = 0;

    CachedSideTexture cachedA;
    CachedSideTexture cachedB;

    // Set by prepareFrames(); read by renderFrame(). Caches the
    // textures that will actually be bound to the composite draw.
    // For Cpu-kind frames these alias cachedA/B above. For Metal-
    // kind frames they hold the converter's owned RGBA texture (the
    // converter manages lifetime; we just hold a strong ref for the
    // single-frame window between prepare and render).
    id<MTLTexture> preparedA = nil;
    id<MTLTexture> preparedB = nil;
    int  srcAW = 0, srcAH = 0;
    int  srcBW = 0, srcBH = 0;
    bool preparedAActive = false;
    bool preparedBActive = false;
    bool preparedAPastEnd = false;
    bool preparedBPastEnd = false;
    bool prepared = false;

    // Phase 7.8 — generation tracking. Bumps when the timeline is
    // edited; we drop both cached textures so stale pre-edit
    // pixels don't show during the decoder's catch-up window.
    int cachedGeneration = -1;

    // Tracks the master frame the cache was last uploaded for.
    // A big jump between consecutive renders means a SEEK (or
    // similar discontinuity) — the cache is stale and we must
    // not display it during the decoder's seek window. -1 means
    // no prior render.
    int lastRenderedMasterFrame = -1;

    // Spinner pass — built on first cold transition, kept alive
    // until shutdown(). Cheap to leave around.
    id<MTLRenderPipelineState>            spinnerPipeline = nil;
    int                                   spinnerPixelFmt = 0;
    std::chrono::steady_clock::time_point spinnerStart;
    bool                                  spinnerActive   = false;
};

DualCompositor::DualCompositor()
    : m_impl(new Impl())
{
}

DualCompositor::~DualCompositor()
{
    shutdown();
    delete m_impl;
}

bool DualCompositor::initialize(int targetPixelFormat, void *mtlDevicePtr)
{
    if (m_impl->pipeline) return true;

    id<MTLDevice> device = (__bridge id<MTLDevice>)mtlDevicePtr;
    if (!device) {
        qWarning("DualCompositor: no Metal device");
        return false;
    }

    NSError *err = nil;
    NSString *src = [NSString stringWithUTF8String:kDualCompositorMSL];
    id<MTLLibrary> lib = [device newLibraryWithSource:src options:nil error:&err];
    if (!lib) {
        qWarning("DualCompositor: shader compile failed: %s",
                 err ? [err.localizedDescription UTF8String] : "(no error)");
        return false;
    }
    id<MTLFunction> vsFn = [lib newFunctionWithName:@"dual_vs"];
    id<MTLFunction> fsFn = [lib newFunctionWithName:@"dual_fs"];
    if (!vsFn || !fsFn) {
        qWarning("DualCompositor: dual_vs/dual_fs not found in library");
        return false;
    }

    MTLRenderPipelineDescriptor *desc = [MTLRenderPipelineDescriptor new];
    desc.vertexFunction   = vsFn;
    desc.fragmentFunction = fsFn;
    desc.colorAttachments[0].pixelFormat =
        static_cast<MTLPixelFormat>(targetPixelFormat);

    id<MTLRenderPipelineState> pso =
        [device newRenderPipelineStateWithDescriptor:desc error:&err];
    if (!pso) {
        qWarning("DualCompositor: pipeline creation failed: %s",
                 err ? [err.localizedDescription UTF8String] : "(no error)");
        return false;
    }

    MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
    sd.minFilter    = MTLSamplerMinMagFilterLinear;
    sd.magFilter    = MTLSamplerMinMagFilterLinear;
    sd.sAddressMode = MTLSamplerAddressModeClampToEdge;
    sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
    id<MTLSamplerState> smp = [device newSamplerStateWithDescriptor:sd];

    m_impl->pipeline       = pso;
    m_impl->sampler        = smp;
    m_impl->targetPixelFmt = targetPixelFormat;
    qInfo("DualCompositor: initialized (pixelFmt=%d)", targetPixelFormat);
    return true;
}

void DualCompositor::shutdown()
{
    if (m_impl) {
        m_impl->pipeline       = nil;
        m_impl->sampler        = nil;
        m_impl->targetPixelFmt = 0;
        m_impl->cachedA.texture = nil;
        m_impl->cachedA.width = m_impl->cachedA.height = 0;
        m_impl->cachedB.texture = nil;
        m_impl->cachedB.width = m_impl->cachedB.height = 0;
        m_impl->spinnerPipeline = nil;
        m_impl->spinnerPixelFmt = 0;
        m_impl->spinnerActive   = false;
    }
}

namespace {

// Lazy build of the spinner pipeline. Same pixel-format-rebake rule
// as the main compositor. Kept inline since it's only used by
// renderFrame's cold-transition branch.
bool ensureSpinnerPipeline(DualCompositor::Impl &impl,
                            id<MTLDevice> device,
                            int targetPixelFormat)
{
    if (impl.spinnerPipeline && impl.spinnerPixelFmt == targetPixelFormat) {
        return true;
    }
    NSError *err = nil;
    NSString *src = [NSString stringWithUTF8String:kSpinnerMSL];
    id<MTLLibrary> lib = [device newLibraryWithSource:src options:nil error:&err];
    if (!lib) {
        qWarning("DualCompositor: spinner shader compile failed: %s",
                 err ? [err.localizedDescription UTF8String] : "(no error)");
        return false;
    }
    id<MTLFunction> vsFn = [lib newFunctionWithName:@"spin_vs"];
    id<MTLFunction> fsFn = [lib newFunctionWithName:@"spin_fs"];
    if (!vsFn || !fsFn) {
        qWarning("DualCompositor: spin_vs/spin_fs not found");
        return false;
    }
    MTLRenderPipelineDescriptor *desc = [MTLRenderPipelineDescriptor new];
    desc.vertexFunction   = vsFn;
    desc.fragmentFunction = fsFn;
    desc.colorAttachments[0].pixelFormat =
        static_cast<MTLPixelFormat>(targetPixelFormat);
    id<MTLRenderPipelineState> pso =
        [device newRenderPipelineStateWithDescriptor:desc error:&err];
    if (!pso) {
        qWarning("DualCompositor: spinner pipeline failed: %s",
                 err ? [err.localizedDescription UTF8String] : "(no error)");
        return false;
    }
    impl.spinnerPipeline = pso;
    impl.spinnerPixelFmt = targetPixelFormat;
    return true;
}

void encodeSpinner(DualCompositor::Impl &impl,
                    id<MTLRenderCommandEncoder> enc,
                    int dstWidth, int dstHeight)
{
    if (!impl.spinnerActive) {
        impl.spinnerStart  = std::chrono::steady_clock::now();
        impl.spinnerActive = true;
    }
    const double seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - impl.spinnerStart).count();
    // alignas(8): MSL float2 dstSize forces 8-byte alignment (sizeof 16);
    // a plain C++ struct is 12 → bound buffer too small for spin_fs.
    struct alignas(8) SpinUBO {
        float dstSize[2];
        float time;
    };
    SpinUBO ubo;
    ubo.dstSize[0] = static_cast<float>(dstWidth);
    ubo.dstSize[1] = static_cast<float>(dstHeight);
    ubo.time       = static_cast<float>(seconds);

    [enc setRenderPipelineState:impl.spinnerPipeline];
    [enc setFragmentBytes:&ubo length:sizeof(ubo) atIndex:0];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
}

} // namespace

bool DualCompositor::isInitialized() const
{
    return m_impl && m_impl->pipeline != nil;
}

void DualCompositor::setController(DualPlaybackController *c)
{
    m_controller = c;
    // Drop cached textures when controller swaps — they're sized for
    // previous content. Cheap; recreated on next frame.
    if (m_impl) {
        m_impl->cachedA.texture = nil;
        m_impl->cachedA.width = m_impl->cachedA.height = 0;
        m_impl->cachedB.texture = nil;
        m_impl->cachedB.width = m_impl->cachedB.height = 0;
    }
}

void DualCompositor::prepareFrames(void *cmdBufferPtr)
{
    m_impl->prepared = false;
    if (!isInitialized() || !m_controller) return;

    // We only need a device pointer; the cmd buffer comes from
    // somewhere with a device attached, but for cache recreation
    // it's cleaner to just stash one off the existing pipeline.
    id<MTLCommandBuffer> cb =
        (__bridge id<MTLCommandBuffer>)cmdBufferPtr;
    id<MTLDevice> device = cb ? cb.device : nil;
    if (!device) return;

    const int masterFrame = m_controller->currentFrame();

    // Phase 7.8 — drop cache on timeline edit (gen bump). EXCEPT during
    // a live scrub/edit session: slip/trim bumps the generation on every
    // drag delta, and dropping the last-good cache each tick makes the
    // viewport flicker black on any null pull (scrub frame not yet
    // decoded / transient gap). Fresh scrub frames stream in continuously
    // during a scrub, so we keep the cache as the fallback and only sync
    // the marker. The non-scrub drop is for paused committed edits.
    const int curGen = m_controller->timelineGeneration();
    if (curGen != m_impl->cachedGeneration) {
        if (!m_controller->isScrubbing()) {
            m_impl->cachedA.texture = nil;
            m_impl->cachedA.width = m_impl->cachedA.height = 0;
            m_impl->cachedB.texture = nil;
            m_impl->cachedB.width = m_impl->cachedB.height = 0;
        }
        m_impl->cachedGeneration = curGen;
    }

    // Seek detection — drop cache on big master-frame jumps so the
    // last-good-frame hold doesn't display pre-seek pixels.
    constexpr int kSeekJumpThreshold = 16;
    if (m_impl->lastRenderedMasterFrame >= 0
        && std::abs(masterFrame - m_impl->lastRenderedMasterFrame)
           > kSeekJumpThreshold) {
        m_impl->cachedA.texture = nil;
        m_impl->cachedA.width = m_impl->cachedA.height = 0;
        m_impl->cachedB.texture = nil;
        m_impl->cachedB.width = m_impl->cachedB.height = 0;
    }
    m_impl->lastRenderedMasterFrame = masterFrame;

    auto frameA = m_controller->pullFrameA(masterFrame);
    auto frameB = m_controller->pullFrameB(masterFrame);

    const bool aPastEnd = m_controller->aPastEnd(masterFrame);
    const bool bPastEnd = m_controller->bPastEnd(masterFrame);

    if (aPastEnd && m_impl->cachedA.texture != nil) {
        m_impl->cachedA.texture = nil;
        m_impl->cachedA.width = m_impl->cachedA.height = 0;
    }
    if (bPastEnd && m_impl->cachedB.texture != nil) {
        m_impl->cachedB.texture = nil;
        m_impl->cachedB.width = m_impl->cachedB.height = 0;
    }

    // Default to last-known dims so a null pull this tick still
    // displays the cached texture (if any).
    int srcAW = m_impl->cachedA.width;
    int srcAH = m_impl->cachedA.height;
    int srcBW = m_impl->cachedB.width;
    int srcBH = m_impl->cachedB.height;

    // preparedA/B start as the current cache (last-good-frame).
    id<MTLTexture> texA = m_impl->cachedA.texture;
    id<MTLTexture> texB = m_impl->cachedB.texture;

    auto resolveSide = [&](const std::shared_ptr<DualFrame> &f,
                           bool pastEnd, int slot,
                           CachedSideTexture &cache,
                           int &outW, int &outH,
                           __strong id<MTLTexture> &outTex) {
        if (pastEnd || !f || !f->valid()) return;
        outW = f->width;
        outH = f->height;

        switch (f->kind) {
        case DualFrame::Kind::Cpu: {
            // CPU path — replaceRegion into the cached MTLTexture at
            // a format matching the QImage's bytes (RGBA8Unorm for
            // SDR sources, RGBA16Float for EXR's FP16 path).
            const QImage::Format qfmt = f->rgba->format();
            const MTLPixelFormat mfmt =
                (qfmt == QImage::Format_RGBA16FPx4)
                    ? MTLPixelFormatRGBA16Float
                    : MTLPixelFormatRGBA8Unorm;
            if (!cache.fits(outW, outH, mfmt)) {
                cache.recreate(device, outW, outH, mfmt);
            }
            cache.uploadFromQImage(*f->rgba);
            outTex = cache.texture;
            break;
        }
        case DualFrame::Kind::Metal: {
            // Zero-copy GPU path — converter encodes YUV→RGB on cb,
            // returns an RGBA MTLTexture it owns. We hold a strong
            // ref for the single-frame window between prepare and
            // render.
            if (!m_pixbufConverter) {
                qWarning("DualCompositor: Metal-kind frame but no "
                         "pixbuf converter wired — rendering "
                         "transparent for slot %d", slot);
                outTex = nil;
                return;
            }
            int w = 0, h = 0;
            // f->rangeOverride was stamped by DualVideoDecoder at
            // publish time from its m_rangeOverride atomic, fed by
            // DualPlaybackController::setRangeOverrideA/B. The Cpu
            // branch above doesn't need to consult it because the
            // decoder already baked it into the QImage via
            // sws_setColorspaceDetails; the Metal branch carries the
            // raw CVPixelBuffer so the YUV→RGB shader has to honor
            // it here.
            void *rgba = m_pixbufConverter->convertToRgba(
                cmdBufferPtr,
                f->cvPixelBuffer.get(),
                slot, &w, &h,
                f->rangeOverride);
            if (!rgba) {
                outTex = nil;
                return;
            }
            outTex = (__bridge id<MTLTexture>)rgba;
            // Stash in cache slot so last-good-frame hold works
            // (cache.texture stays the same MTLTexture pointer until
            // the next convert; converter guarantees that lifetime).
            cache.texture = outTex;
            cache.width   = w;
            cache.height  = h;
            outW = w; outH = h;
            break;
        }
        case DualFrame::Kind::Vulkan:
            // Windows-only DualFrame kind (F.2.12.b). Never reached on
            // macOS — DualVideoDecoder's macOS path produces Metal/Cpu,
            // not Vulkan. Listed here to keep the switch exhaustive.
            break;
        case DualFrame::Kind::Empty:
            break;
        }
    };

    resolveSide(frameA, aPastEnd, /*slot=*/0,
                m_impl->cachedA, srcAW, srcAH, texA);
    resolveSide(frameB, bPastEnd, /*slot=*/1,
                m_impl->cachedB, srcBW, srcBH, texB);

    const bool aActive = !aPastEnd && texA != nil;
    const bool bActive = !bPastEnd && texB != nil;

    // Stash everything for renderFrame().
    m_impl->preparedA        = aActive ? texA : nil;
    m_impl->preparedB        = bActive ? texB : nil;
    m_impl->srcAW            = std::max(1, srcAW);
    m_impl->srcAH            = std::max(1, srcAH);
    m_impl->srcBW            = std::max(1, srcBW);
    m_impl->srcBH            = std::max(1, srcBH);
    m_impl->preparedAActive  = aActive;
    m_impl->preparedBActive  = bActive;
    m_impl->preparedAPastEnd = aPastEnd;
    m_impl->preparedBPastEnd = bPastEnd;
    m_impl->prepared         = true;
}

void DualCompositor::renderFrame(void *encoderPtr, int dstWidth, int dstHeight)
{
    if (!isInitialized() || !encoderPtr) return;
    if (dstWidth <= 0 || dstHeight <= 0) return;
    if (!m_controller) return;

    id<MTLRenderCommandEncoder> enc =
        (__bridge id<MTLRenderCommandEncoder>)encoderPtr;
    id<MTLDevice> device = enc.device;
    if (!device) return;

    // Back-compat: if caller didn't run prepareFrames first (e.g.,
    // capture compositor that doesn't have its own cmd buffer hook
    // yet), we can't bridge HW frames. Fall back to a no-prepare
    // pull that handles only Cpu-kind frames. This preserves the
    // pre-#108 behavior for callers that haven't been migrated.
    if (!m_impl->prepared) {
        const int masterFrame = m_controller->currentFrame();
        auto fA = m_controller->pullFrameA(masterFrame);
        auto fB = m_controller->pullFrameB(masterFrame);
        const bool aPastEnd = m_controller->aPastEnd(masterFrame);
        const bool bPastEnd = m_controller->bPastEnd(masterFrame);
        auto formatFor = [](const QImage &img) {
            return (img.format() == QImage::Format_RGBA16FPx4)
                ? MTLPixelFormatRGBA16Float
                : MTLPixelFormatRGBA8Unorm;
        };
        if (!aPastEnd && fA && fA->valid()
            && fA->kind == DualFrame::Kind::Cpu) {
            const MTLPixelFormat mfmt = formatFor(*fA->rgba);
            if (!m_impl->cachedA.fits(fA->width, fA->height, mfmt)) {
                m_impl->cachedA.recreate(device, fA->width, fA->height, mfmt);
            }
            m_impl->cachedA.uploadFromQImage(*fA->rgba);
        }
        if (!bPastEnd && fB && fB->valid()
            && fB->kind == DualFrame::Kind::Cpu) {
            const MTLPixelFormat mfmt = formatFor(*fB->rgba);
            if (!m_impl->cachedB.fits(fB->width, fB->height, mfmt)) {
                m_impl->cachedB.recreate(device, fB->width, fB->height, mfmt);
            }
            m_impl->cachedB.uploadFromQImage(*fB->rgba);
        }
        m_impl->preparedA        = aPastEnd ? nil : m_impl->cachedA.texture;
        m_impl->preparedB        = bPastEnd ? nil : m_impl->cachedB.texture;
        m_impl->srcAW = std::max(1, m_impl->cachedA.width);
        m_impl->srcAH = std::max(1, m_impl->cachedA.height);
        m_impl->srcBW = std::max(1, m_impl->cachedB.width);
        m_impl->srcBH = std::max(1, m_impl->cachedB.height);
        m_impl->preparedAActive  = !aPastEnd && m_impl->preparedA != nil;
        m_impl->preparedBActive  = !bPastEnd && m_impl->preparedB != nil;
        m_impl->preparedAPastEnd = aPastEnd;
        m_impl->preparedBPastEnd = bPastEnd;
    }

    id<MTLTexture> texA = m_impl->preparedA;
    id<MTLTexture> texB = m_impl->preparedB;
    const bool aActive  = m_impl->preparedAActive;
    const bool bActive  = m_impl->preparedBActive;
    const bool aPastEnd = m_impl->preparedAPastEnd;
    const bool bPastEnd = m_impl->preparedBPastEnd;

    if (!texA && !texB) {
        if (aPastEnd && bPastEnd) {
            m_impl->prepared = false;
            return;
        }
        if (ensureSpinnerPipeline(*m_impl, device, m_impl->targetPixelFmt)) {
            encodeSpinner(*m_impl, enc, dstWidth, dstHeight);
        }
        m_impl->prepared = false;
        return;
    }
    if (!texA) texA = texB;
    if (!texB) texB = texA;
    m_impl->spinnerActive = false;

    // alignas(8): MSL float2 fields force 8-byte struct alignment (sizeof 48);
    // a plain scalar-float C++ mirror is 44 → bound buffer too small for dual_fs.
    struct alignas(8) UBO {
        float dstSize[2];
        float srcSizeA[2];
        float srcSizeB[2];
        int   mode;
        float splitPos;
        int   aActive;
        int   bActive;
        float seamHighlight;
        float diffGain;
        int   rotA;
        int   rotB;
    };
    UBO ubo;
    ubo.dstSize[0]  = static_cast<float>(dstWidth);
    ubo.dstSize[1]  = static_cast<float>(dstHeight);
    // Pixel-aspect un-squeeze: widen the effective source dims so the
    // per-side aspect-fit lands on the corrected display aspect. The
    // dual_fs shader normalizes its sample by srcSize, so feeding
    // effective dims stretches the real texture — no shader change.
    // Widen the larger-pixel axis up (never downscale) to keep
    // resolution. 1/1 leaves dims untouched.
    auto effDim = [](int w, int h, int num, int den) -> std::pair<int,int> {
        if (w <= 0 || h <= 0 || num <= 0 || den <= 0 || num == den)
            return { std::max(1, w), std::max(1, h) };
        if (num > den)
            return { static_cast<int>(std::lround(double(w) * num / den)), h };
        return { w, static_cast<int>(std::lround(double(h) * den / num)) };
    };
    const auto [effAW, effAH] = effDim(m_impl->srcAW, m_impl->srcAH,
                                       m_parNumA, m_parDenA);
    const auto [effBW, effBH] = effDim(m_impl->srcBW, m_impl->srcBH,
                                       m_parNumB, m_parDenB);
    // Display rotation: un-squeeze first (stored space), then swap
    // the effective dims for odd quarter-turns so each side's fit
    // frames the upright image. dual_fs inverse-rotates its sampling.
    ubo.srcSizeA[0] = static_cast<float>((m_rotQA & 1) ? effAH : effAW);
    ubo.srcSizeA[1] = static_cast<float>((m_rotQA & 1) ? effAW : effAH);
    ubo.srcSizeB[0] = static_cast<float>((m_rotQB & 1) ? effBH : effBW);
    ubo.srcSizeB[1] = static_cast<float>((m_rotQB & 1) ? effBW : effBH);
    ubo.rotA        = m_rotQA;
    ubo.rotB        = m_rotQB;
    ubo.mode        = static_cast<int>(m_mode);
    ubo.splitPos    = m_splitPos;
    ubo.aActive     = aActive ? 1 : 0;
    ubo.bActive     = bActive ? 1 : 0;
    ubo.seamHighlight = m_seamHighlight;
    ubo.diffGain    = m_diffGain;

    [enc setRenderPipelineState:m_impl->pipeline];
    [enc setFragmentTexture:texA atIndex:0];
    [enc setFragmentTexture:texB atIndex:1];
    [enc setFragmentSamplerState:m_impl->sampler atIndex:0];
    [enc setFragmentBytes:&ubo length:sizeof(ubo) atIndex:0];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];

    m_impl->prepared = false;   // consumed
}

void DualCompositor::encodeLoadingSpinner(void *encoderPtr,
                                          int dstWidth, int dstHeight,
                                          int targetPixelFormat)
{
    if (!encoderPtr || !m_impl) return;
    if (dstWidth <= 0 || dstHeight <= 0) return;
    id<MTLDevice> device =
        (__bridge id<MTLDevice>)MetalDeviceManager::instance().device();
    if (!device) return;
    if (!ensureSpinnerPipeline(*m_impl, device, targetPixelFormat)) return;
    id<MTLRenderCommandEncoder> enc =
        (__bridge id<MTLRenderCommandEncoder>)encoderPtr;
    encodeSpinner(*m_impl, enc, dstWidth, dstHeight);
}

} // namespace qcv::dual
