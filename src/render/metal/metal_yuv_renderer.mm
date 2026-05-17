// MetalYuvRenderer — see header for adaptation notes.

#include "metal_yuv_renderer.h"

#include "metal_device_manager.h"

#import <Metal/Metal.h>

#include <QtLogging>

namespace qcv {

namespace {

// MSL compute kernels — byte-identical to old app's
// metal_yuv_renderer.mm. Color-matrix constants are BT.709 NCL
// (default) and BT.2020 NCL (for 10-bit HDR sources).
constexpr const char *kYuvShaderSource = R"(
#include <metal_stdlib>
using namespace metal;

struct YUVParams {
    uint width;
    uint height;
    uint bit_depth;
    uint is_full_range;
    uint is_bt2020;
    uint is_hdr;
    uint subsampling;  // 0=4:2:0, 1=4:2:2, 2=4:4:4
};

kernel void yuv_to_rgba(
    texture2d<float, access::read>  y_tex   [[texture(0)]],
    texture2d<float, access::read>  uv_tex  [[texture(1)]],
    texture2d<float, access::write> out_tex [[texture(2)]],
    constant YUVParams &params              [[buffer(0)]],
    uint2 gid                               [[thread_position_in_grid]])
{
    if (gid.x >= params.width || gid.y >= params.height) return;

    float y_val = y_tex.read(gid).r;

    uint2 uv_pos;
    if (params.subsampling == 2) {
        uv_pos = gid;                          // 4:4:4
    } else if (params.subsampling == 1) {
        uv_pos = uint2(gid.x / 2, gid.y);      // 4:2:2
    } else {
        uv_pos = uint2(gid.x / 2, gid.y / 2);  // 4:2:0
    }
    float2 uv_val = uv_tex.read(uv_pos).rg;

    float y, cb, cr;
    if (params.is_full_range) {
        y  = y_val;
        cb = uv_val.x - 0.5;
        cr = uv_val.y - 0.5;
    } else {
        // Video range: Y [16/255, 235/255], UV [16/255, 240/255]
        y  = (y_val     - 16.0/255.0)  * (255.0/219.0);
        cb = (uv_val.x  - 128.0/255.0) * (255.0/224.0);
        cr = (uv_val.y  - 128.0/255.0) * (255.0/224.0);
    }

    float r, g, b;
    if (params.is_bt2020) {
        r = y + 1.4746   * cr;
        g = y - 0.16455  * cb - 0.57135 * cr;
        b = y + 1.8814   * cb;
    } else {
        r = y + 1.5748   * cr;
        g = y - 0.1873   * cb - 0.4681  * cr;
        b = y + 1.8556   * cb;
    }

    out_tex.write(float4(clamp(r, 0.0, 1.0),
                         clamp(g, 0.0, 1.0),
                         clamp(b, 0.0, 1.0), 1.0), gid);
}

// Y416 / Y408 layout: A,Y,Cb,Cr packed into RGBA channels of one
// 16-bit unorm texture. Used by ProRes 4444 alpha video.
kernel void yuv_interleaved_to_rgba(
    texture2d<float, access::read>  in_tex  [[texture(0)]],
    texture2d<float, access::write> out_tex [[texture(1)]],
    constant YUVParams &params              [[buffer(0)]],
    uint2 gid                               [[thread_position_in_grid]])
{
    if (gid.x >= params.width || gid.y >= params.height) return;

    float4 sample = in_tex.read(gid);
    float alpha  = sample.r;
    float y_val  = sample.g;
    float cb_raw = sample.b;
    float cr_raw = sample.a;

    float y, cb, cr;
    if (params.is_full_range) {
        y  = y_val;
        cb = cb_raw - 0.5;
        cr = cr_raw - 0.5;
    } else {
        y  = (y_val  - 16.0/255.0)  * (255.0/219.0);
        cb = (cb_raw - 128.0/255.0) * (255.0/224.0);
        cr = (cr_raw - 128.0/255.0) * (255.0/224.0);
    }

    float r, g, b;
    if (params.is_bt2020) {
        r = y + 1.4746   * cr;
        g = y - 0.16455  * cb - 0.57135 * cr;
        b = y + 1.8814   * cb;
    } else {
        r = y + 1.5748   * cr;
        g = y - 0.1873   * cb - 0.4681  * cr;
        b = y + 1.8556   * cb;
    }

    out_tex.write(float4(clamp(r, 0.0, 1.0),
                         clamp(g, 0.0, 1.0),
                         clamp(b, 0.0, 1.0), alpha), gid);
}
)";

} // namespace

struct MetalYuvRenderer::Impl {
    id<MTLComputePipelineState> biplanarPso    = nil;
    id<MTLComputePipelineState> interleavedPso = nil;

    // Cached output texture descriptor — same dimensions across all
    // frames of a clip, so we re-use it instead of re-computing.
    MTLTextureDescriptor *cachedOutputDesc = nil;
    int                  cachedDescW = 0;
    int                  cachedDescH = 0;

    bool initialized = false;
};

MetalYuvRenderer::MetalYuvRenderer()
    : m_impl(new Impl())
{
}

MetalYuvRenderer::~MetalYuvRenderer()
{
    shutdown();
    delete m_impl;
}

bool MetalYuvRenderer::initialize()
{
    if (m_impl->initialized) return true;
    if (!createComputePipelines()) return false;
    m_impl->initialized = true;
    qInfo("MetalYuvRenderer: initialized");
    return true;
}

void MetalYuvRenderer::shutdown()
{
    if (!m_impl || !m_impl->initialized) return;
    m_impl->biplanarPso     = nil;
    m_impl->interleavedPso  = nil;
    m_impl->cachedOutputDesc = nil;
    m_impl->cachedDescW = 0;
    m_impl->cachedDescH = 0;
    m_impl->initialized = false;
}

bool MetalYuvRenderer::isInitialized() const
{
    return m_impl && m_impl->initialized;
}

bool MetalYuvRenderer::createComputePipelines()
{
    auto &mgr = MetalDeviceManager::instance();
    id<MTLDevice> device = (__bridge id<MTLDevice>)mgr.device();
    if (!device) {
        qWarning("MetalYuvRenderer: no Metal device");
        return false;
    }

    NSError *err = nil;
    NSString *src = [NSString stringWithUTF8String:kYuvShaderSource];
    id<MTLLibrary> lib = [device newLibraryWithSource:src options:nil error:&err];
    if (!lib) {
        qWarning("MetalYuvRenderer: shader compile failed: %s",
                 err ? [err.localizedDescription UTF8String] : "(no error)");
        return false;
    }

    id<MTLFunction> biplanarFn = [lib newFunctionWithName:@"yuv_to_rgba"];
    if (!biplanarFn) {
        qWarning("MetalYuvRenderer: yuv_to_rgba kernel not found");
        return false;
    }
    m_impl->biplanarPso =
        [device newComputePipelineStateWithFunction:biplanarFn error:&err];
    if (!m_impl->biplanarPso) {
        qWarning("MetalYuvRenderer: biplanar pipeline creation failed: %s",
                 err ? [err.localizedDescription UTF8String] : "(no error)");
        return false;
    }

    id<MTLFunction> interleavedFn =
        [lib newFunctionWithName:@"yuv_interleaved_to_rgba"];
    if (interleavedFn) {
        m_impl->interleavedPso =
            [device newComputePipelineStateWithFunction:interleavedFn error:&err];
        if (!m_impl->interleavedPso) {
            qWarning("MetalYuvRenderer: interleaved pipeline creation failed: %s",
                     err ? [err.localizedDescription UTF8String] : "(no error)");
            // Non-fatal — biplanar still works for non-ProRes-4444.
        }
    }
    return true;
}

namespace {

// YUVParams uniform block — must match MSL struct layout.
struct YuvUniforms {
    uint32_t width;
    uint32_t height;
    uint32_t bitDepth;
    uint32_t isFullRange;
    uint32_t isBt2020;
    uint32_t isHdr;
    uint32_t subsampling;
};

// Refresh the cached output descriptor if the requested size differs.
void refreshOutputDesc(MetalYuvRenderer::Impl *impl, int width, int height)
{
    if (impl->cachedOutputDesc && width == impl->cachedDescW &&
        height == impl->cachedDescH) {
        return;
    }
    MTLTextureDescriptor *desc = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
                                     width:width
                                    height:height
                                 mipmapped:NO];
    desc.usage       = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    desc.storageMode = MTLStorageModePrivate;
    impl->cachedOutputDesc = desc;
    impl->cachedDescW      = width;
    impl->cachedDescH      = height;
}

} // namespace

void *MetalYuvRenderer::renderToRgba(void *cbPtr,
                                     void *yTexture, void *uvTexture,
                                     int width, int height,
                                     int bitDepth, bool isFullRange,
                                     bool isBt2020, bool isHdr,
                                     int subsampling)
{
    if (!m_impl->initialized || !yTexture || !uvTexture) return nullptr;
    if (!m_impl->biplanarPso || !cbPtr) return nullptr;

    auto &mgr = MetalDeviceManager::instance();
    id<MTLDevice> device = (__bridge id<MTLDevice>)mgr.device();

    refreshOutputDesc(m_impl, width, height);
    id<MTLTexture> output =
        [device newTextureWithDescriptor:m_impl->cachedOutputDesc];
    if (!output) {
        qWarning("MetalYuvRenderer: newTexture failed (%dx%d)", width, height);
        return nullptr;
    }

    // Encode into the caller's command buffer — no separate cb,
    // no commit, no waitUntilCompleted. The compositor encoder
    // that runs later in the same buffer reads `output` with
    // Metal's automatic resource hazard tracking guaranteeing
    // ordering.
    id<MTLCommandBuffer>         cb  = (__bridge id<MTLCommandBuffer>)cbPtr;
    id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
    [enc setComputePipelineState:m_impl->biplanarPso];

    [enc setTexture:(__bridge id<MTLTexture>)yTexture  atIndex:0];
    [enc setTexture:(__bridge id<MTLTexture>)uvTexture atIndex:1];
    [enc setTexture:output                              atIndex:2];

    YuvUniforms u = {
        static_cast<uint32_t>(width),
        static_cast<uint32_t>(height),
        static_cast<uint32_t>(bitDepth),
        isFullRange ? 1u : 0u,
        isBt2020    ? 1u : 0u,
        isHdr       ? 1u : 0u,
        static_cast<uint32_t>(subsampling),
    };
    [enc setBytes:&u length:sizeof(u) atIndex:0];

    const MTLSize tg   = MTLSizeMake(16, 16, 1);
    const MTLSize grid = MTLSizeMake(width, height, 1);
    [enc dispatchThreads:grid threadsPerThreadgroup:tg];

    [enc endEncoding];

    // __bridge_retained: hand a +1 reference out to the caller.
    return (__bridge_retained void *)output;
}

void *MetalYuvRenderer::renderInterleavedToRgba(void *cbPtr,
                                                void *inTexture,
                                                int width, int height,
                                                int bitDepth, bool isFullRange,
                                                bool isBt2020, bool isHdr)
{
    if (!m_impl->initialized || !inTexture) return nullptr;
    if (!m_impl->interleavedPso) {
        qWarning("MetalYuvRenderer: interleaved pipeline unavailable");
        return nullptr;
    }
    if (!cbPtr) return nullptr;

    auto &mgr = MetalDeviceManager::instance();
    id<MTLDevice> device = (__bridge id<MTLDevice>)mgr.device();

    refreshOutputDesc(m_impl, width, height);
    id<MTLTexture> output =
        [device newTextureWithDescriptor:m_impl->cachedOutputDesc];
    if (!output) {
        qWarning("MetalYuvRenderer: newTexture failed (%dx%d, interleaved)",
                 width, height);
        return nullptr;
    }

    id<MTLCommandBuffer>         cb  = (__bridge id<MTLCommandBuffer>)cbPtr;
    id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
    [enc setComputePipelineState:m_impl->interleavedPso];

    [enc setTexture:(__bridge id<MTLTexture>)inTexture atIndex:0];
    [enc setTexture:output                              atIndex:1];

    YuvUniforms u = {
        static_cast<uint32_t>(width),
        static_cast<uint32_t>(height),
        static_cast<uint32_t>(bitDepth),
        isFullRange ? 1u : 0u,
        isBt2020    ? 1u : 0u,
        isHdr       ? 1u : 0u,
        2u,    // interleaved is always 4:4:4 by definition
    };
    [enc setBytes:&u length:sizeof(u) atIndex:0];

    const MTLSize tg   = MTLSizeMake(16, 16, 1);
    const MTLSize grid = MTLSizeMake(width, height, 1);
    [enc dispatchThreads:grid threadsPerThreadgroup:tg];

    [enc endEncoding];

    return (__bridge_retained void *)output;
}

} // namespace qcv
