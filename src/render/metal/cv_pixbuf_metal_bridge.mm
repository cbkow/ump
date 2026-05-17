// CvPixbufMetalBridge — see header for adaptation notes.

#include "cv_pixbuf_metal_bridge.h"

#include "decode/frame_handle.h"
#include "metal_device_manager.h"

#import <CoreVideo/CoreVideo.h>
#import <Metal/Metal.h>

#include <QtLogging>

#include <vector>

namespace qcv {

namespace {

// CVPixelFormat → layout descriptor. Matches the same set
// zero_copy_bridge_metal.mm understands.
enum class Layout {
    // Naming convention: family + bit-depth. NVxx = 8-bit biplanar
    // (R8/RG8). Pxxx = 10-bit biplanar (R16/RG16, MSB-aligned).
    // SVxx = 16-bit biplanar (R16/RG16, MSB-aligned). Last digit
    // pair is the chroma subsampling (12=4:2:0, 16=4:2:2, 24=4:4:4).
    NV12_8bit,        // 4:2:0 8-bit   (420v/420f) H.264 / HEVC SDR
    NV16_8bit,        // 4:2:2 8-bit   (422v/422f)
    NV24_8bit,        // 4:4:4 8-bit   (444v/444f)
    P010_10bit,       // 4:2:0 10-bit  (x420/xf20) HEVC 10-bit / HDR10
    P210_10bit,       // 4:2:2 10-bit  (x422/xf22) ProRes 422 HQ / HEVC 4:2:2
    P410_10bit,       // 4:4:4 10-bit  (x444/xf44) HEVC 4:4:4 10-bit
    SV22_16bit,       // 4:2:2 16-bit  (sv22) ProRes 422 12-bit, HEVC 4:2:2 hi
    SV44_16bit,       // 4:4:4 16-bit  (sv44) ProRes 4444 12-bit, HEVC 4:4:4 hi
    AYpCbCr16,        // 4:4:4 16-bit packed (y416) ProRes 4444 alpha
    Unknown,
};

Layout layoutFor(OSType fmt)
{
    switch (fmt) {
        case kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange:
        case kCVPixelFormatType_420YpCbCr8BiPlanarFullRange:
            return Layout::NV12_8bit;
        case kCVPixelFormatType_422YpCbCr8BiPlanarVideoRange:
        case kCVPixelFormatType_422YpCbCr8BiPlanarFullRange:
            return Layout::NV16_8bit;
        case kCVPixelFormatType_444YpCbCr8BiPlanarVideoRange:
        case kCVPixelFormatType_444YpCbCr8BiPlanarFullRange:
            return Layout::NV24_8bit;
        case kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange:
        case kCVPixelFormatType_420YpCbCr10BiPlanarFullRange:
            return Layout::P010_10bit;
        case kCVPixelFormatType_422YpCbCr10BiPlanarVideoRange:
        case kCVPixelFormatType_422YpCbCr10BiPlanarFullRange:
            return Layout::P210_10bit;
        case kCVPixelFormatType_444YpCbCr10BiPlanarVideoRange:
        case kCVPixelFormatType_444YpCbCr10BiPlanarFullRange:
            return Layout::P410_10bit;
        case kCVPixelFormatType_422YpCbCr16BiPlanarVideoRange:
            return Layout::SV22_16bit;
        case kCVPixelFormatType_444YpCbCr16BiPlanarVideoRange:
            return Layout::SV44_16bit;
        case kCVPixelFormatType_4444AYpCbCr16:
            return Layout::AYpCbCr16;
        default:
            return Layout::Unknown;
    }
}

bool isFullRange(OSType fmt)
{
    switch (fmt) {
        case kCVPixelFormatType_420YpCbCr8BiPlanarFullRange:
        case kCVPixelFormatType_422YpCbCr8BiPlanarFullRange:
        case kCVPixelFormatType_444YpCbCr8BiPlanarFullRange:
        case kCVPixelFormatType_420YpCbCr10BiPlanarFullRange:
        case kCVPixelFormatType_422YpCbCr10BiPlanarFullRange:
        case kCVPixelFormatType_444YpCbCr10BiPlanarFullRange:
            return true;
        default:
            return false;
    }
}

int bitDepthFor(Layout l)
{
    switch (l) {
        case Layout::NV12_8bit:
        case Layout::NV16_8bit:
        case Layout::NV24_8bit:    return 8;
        case Layout::P010_10bit:
        case Layout::P210_10bit:
        case Layout::P410_10bit:   return 10;
        case Layout::SV22_16bit:
        case Layout::SV44_16bit:
        case Layout::AYpCbCr16:    return 16;
        default:                   return 8;
    }
}

int subsamplingFor(Layout l)
{
    switch (l) {
        case Layout::NV12_8bit:
        case Layout::P010_10bit:   return 0;   // 4:2:0
        case Layout::NV16_8bit:
        case Layout::P210_10bit:
        case Layout::SV22_16bit:   return 1;   // 4:2:2
        case Layout::NV24_8bit:
        case Layout::P410_10bit:
        case Layout::SV44_16bit:
        case Layout::AYpCbCr16:    return 2;   // 4:4:4
        default:                   return 0;
    }
}

void planeFormats(Layout l, MTLPixelFormat *yFmt, MTLPixelFormat *uvFmt)
{
    switch (l) {
        case Layout::NV12_8bit:
        case Layout::NV16_8bit:
        case Layout::NV24_8bit:
            *yFmt  = MTLPixelFormatR8Unorm;
            *uvFmt = MTLPixelFormatRG8Unorm;
            return;
        case Layout::P010_10bit:
        case Layout::P210_10bit:
        case Layout::P410_10bit:
        case Layout::SV22_16bit:
        case Layout::SV44_16bit:
            *yFmt  = MTLPixelFormatR16Unorm;
            *uvFmt = MTLPixelFormatRG16Unorm;
            return;
        default:
            *yFmt = *uvFmt = MTLPixelFormatInvalid;
    }
}

bool isBt2020Matrix(CVPixelBufferRef pix)
{
    CFTypeRef raw = CVBufferCopyAttachment(pix,
        kCVImageBufferYCbCrMatrixKey, nullptr);
    if (!raw) return false;
    bool result = false;
    if (CFGetTypeID(raw) == CFStringGetTypeID()) {
        CFStringRef s = static_cast<CFStringRef>(raw);
        if (CFEqual(s, kCVImageBufferYCbCrMatrix_ITU_R_2020)) {
            result = true;
        }
    }
    CFRelease(raw);
    return result;
}

bool isHdrTransfer(CVPixelBufferRef pix)
{
    CFTypeRef raw = CVBufferCopyAttachment(pix,
        kCVImageBufferTransferFunctionKey, nullptr);
    if (!raw) return false;
    bool result = false;
    if (CFGetTypeID(raw) == CFStringGetTypeID()) {
        CFStringRef s = static_cast<CFStringRef>(raw);
        if (CFEqual(s, kCVImageBufferTransferFunction_SMPTE_ST_2084_PQ) ||
            CFEqual(s, kCVImageBufferTransferFunction_ITU_R_2100_HLG)) {
            result = true;
        }
    }
    CFRelease(raw);
    return result;
}

} // namespace

struct CvPixbufMetalBridge::Impl {
    id<MTLDevice>          device = nil;
    CVMetalTextureCacheRef cache  = nullptr;

    // Refs to release on next bridge(): keeps the wrappers alive
    // while the GPU consumes them, then drops them when we wrap the
    // next frame.
    std::vector<CVMetalTextureRef> prevRefs;

    void releasePrev()
    {
        for (CVMetalTextureRef r : prevRefs) {
            if (r) CFRelease(r);
        }
        prevRefs.clear();
    }
};

CvPixbufMetalBridge::CvPixbufMetalBridge()
    : m_impl(new Impl())
{
}

CvPixbufMetalBridge::~CvPixbufMetalBridge()
{
    shutdown();
    delete m_impl;
}

bool CvPixbufMetalBridge::initialize()
{
    if (m_impl->cache) return true;

    auto &mgr = MetalDeviceManager::instance();
    m_impl->device = (__bridge id<MTLDevice>)mgr.device();
    if (!m_impl->device) {
        qWarning("CvPixbufMetalBridge: no Metal device");
        return false;
    }

    CVReturn rc = CVMetalTextureCacheCreate(
        kCFAllocatorDefault, nullptr, m_impl->device, nullptr, &m_impl->cache);
    if (rc != kCVReturnSuccess || !m_impl->cache) {
        qWarning("CvPixbufMetalBridge: CVMetalTextureCacheCreate failed (%d)", rc);
        m_impl->cache = nullptr;
        return false;
    }
    return true;
}

void CvPixbufMetalBridge::shutdown()
{
    if (!m_impl) return;
    m_impl->releasePrev();
    if (m_impl->cache) {
        CFRelease(m_impl->cache);
        m_impl->cache = nullptr;
    }
    m_impl->device = nil;
}

bool CvPixbufMetalBridge::isInitialized() const
{
    return m_impl && m_impl->cache != nullptr;
}

CvPixbufMetalBridge::Result
CvPixbufMetalBridge::bridge(const FrameHandle &handle, PlaneSet *out)
{
    if (!out) return Result::Failed;
    *out = {};

    if (handle.kind() != FrameHandle::Kind::Metal) return Result::Failed;
    if (!m_impl || !m_impl->cache) return Result::Failed;

    auto pix = static_cast<CVPixelBufferRef>(handle.metalPixelBuffer());
    if (!pix) return Result::Failed;

    const OSType fmt    = CVPixelBufferGetPixelFormatType(pix);
    const Layout layout = layoutFor(fmt);
    if (layout == Layout::Unknown) return Result::Unsupported;

    // Drop previous frame's wrappers BEFORE creating new ones — the
    // cache may reuse internals when it can.
    m_impl->releasePrev();

    out->bitDepth    = bitDepthFor(layout);
    out->subsampling = subsamplingFor(layout);
    out->fullRange   = isFullRange(fmt);
    out->isBt2020    = isBt2020Matrix(pix);
    out->isHdr       = isHdrTransfer(pix);

    if (layout == Layout::AYpCbCr16) {
        const std::size_t w = CVPixelBufferGetWidth(pix);
        const std::size_t h = CVPixelBufferGetHeight(pix);

        CVMetalTextureRef cv = nullptr;
        CVReturn rc = CVMetalTextureCacheCreateTextureFromImage(
            kCFAllocatorDefault, m_impl->cache, pix, nullptr,
            MTLPixelFormatRGBA16Unorm, w, h, 0, &cv);
        if (rc != kCVReturnSuccess || !cv) {
            qWarning("CvPixbufMetalBridge: packed-plane create failed (%d)", rc);
            if (cv) CFRelease(cv);
            return Result::Failed;
        }

        out->packedTexture = (__bridge void *)CVMetalTextureGetTexture(cv);
        out->width  = static_cast<int>(w);
        out->height = static_cast<int>(h);

        m_impl->prevRefs.push_back(cv);
        return Result::Ok;
    }

    // Biplanar layouts.
    MTLPixelFormat yFmt = MTLPixelFormatInvalid, uvFmt = MTLPixelFormatInvalid;
    planeFormats(layout, &yFmt, &uvFmt);

    const std::size_t yW  = CVPixelBufferGetWidthOfPlane(pix, 0);
    const std::size_t yH  = CVPixelBufferGetHeightOfPlane(pix, 0);
    const std::size_t uvW = CVPixelBufferGetWidthOfPlane(pix, 1);
    const std::size_t uvH = CVPixelBufferGetHeightOfPlane(pix, 1);

    CVMetalTextureRef yCv = nullptr;
    CVReturn rc = CVMetalTextureCacheCreateTextureFromImage(
        kCFAllocatorDefault, m_impl->cache, pix, nullptr,
        yFmt, yW, yH, 0, &yCv);
    if (rc != kCVReturnSuccess || !yCv) {
        qWarning("CvPixbufMetalBridge: Y plane create failed (%d)", rc);
        if (yCv) CFRelease(yCv);
        return Result::Failed;
    }

    CVMetalTextureRef uvCv = nullptr;
    rc = CVMetalTextureCacheCreateTextureFromImage(
        kCFAllocatorDefault, m_impl->cache, pix, nullptr,
        uvFmt, uvW, uvH, 1, &uvCv);
    if (rc != kCVReturnSuccess || !uvCv) {
        qWarning("CvPixbufMetalBridge: UV plane create failed (%d)", rc);
        CFRelease(yCv);
        if (uvCv) CFRelease(uvCv);
        return Result::Failed;
    }

    out->yTexture  = (__bridge void *)CVMetalTextureGetTexture(yCv);
    out->uvTexture = (__bridge void *)CVMetalTextureGetTexture(uvCv);
    out->width     = static_cast<int>(yW);
    out->height    = static_cast<int>(yH);

    m_impl->prevRefs.push_back(yCv);
    m_impl->prevRefs.push_back(uvCv);
    return Result::Ok;
}

} // namespace qcv
