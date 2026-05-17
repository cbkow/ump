// Phase 1.8.2b: zero-copy bridge from CVPixelBuffer to QRhiTexture.
//
// Implementation notes:
// - Uses CVMetalTextureCache, Apple's blessed API for IOSurface-backed
//   Metal texture wrapping. Internally creates one MTLTexture per
//   plane that views into the same IOSurface as the CVPixelBuffer —
//   no memcpy on either side of the boundary.
// - We hold the QRhi's id<MTLDevice> via QRhiNativeHandles; passing a
//   different device would still produce textures but they would be
//   sample-incompatible with the QRhi pipeline.
// - The previous frame's CVMetalTextureRefs are released on each
//   bridge() call. The frame handle holds the CVPixelBuffer alive;
//   the cache holds plane wrappers alive. Both must be released
//   before the FrameHandle's CVPixelBufferRelease in dtor.

#include "zero_copy_bridge_metal.h"

#include "decode/frame_handle.h"

#import <CoreVideo/CoreVideo.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

#include <QtLogging>
#include <rhi/qrhi.h>

namespace qcv {

namespace {

ZeroCopyBridgeMetal::YuvLayout layoutForCvFormat(OSType cvFmt)
{
    switch (cvFmt) {
        case kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange:
        case kCVPixelFormatType_420YpCbCr8BiPlanarFullRange:
            return ZeroCopyBridgeMetal::YuvLayout::NV12_8bit;
        case kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange:
        case kCVPixelFormatType_420YpCbCr10BiPlanarFullRange:
            return ZeroCopyBridgeMetal::YuvLayout::P010_10bit;
        case kCVPixelFormatType_422YpCbCr10BiPlanarVideoRange:
        case kCVPixelFormatType_422YpCbCr10BiPlanarFullRange:
            return ZeroCopyBridgeMetal::YuvLayout::P210_10bit;
        case kCVPixelFormatType_4444AYpCbCr16:
            return ZeroCopyBridgeMetal::YuvLayout::AYpCbCr16;
        default:
            return ZeroCopyBridgeMetal::YuvLayout::Unknown;
    }
}

bool isFullRangeFormat(OSType cvFmt)
{
    switch (cvFmt) {
        case kCVPixelFormatType_420YpCbCr8BiPlanarFullRange:
        case kCVPixelFormatType_420YpCbCr10BiPlanarFullRange:
        case kCVPixelFormatType_422YpCbCr10BiPlanarFullRange:
            return true;
        // AYpCbCr16 (ProRes 4444) is always video-range per Apple's
        // CoreVideo docs — no FullRange variant exists.
        default:
            return false;
    }
}

bool isPackedLayout(ZeroCopyBridgeMetal::YuvLayout layout)
{
    return layout == ZeroCopyBridgeMetal::YuvLayout::AYpCbCr16;
}

void planeMetalFormatsForLayout(ZeroCopyBridgeMetal::YuvLayout layout,
                                MTLPixelFormat *yFmt, MTLPixelFormat *uvFmt)
{
    switch (layout) {
        case ZeroCopyBridgeMetal::YuvLayout::NV12_8bit:
            *yFmt  = MTLPixelFormatR8Unorm;
            *uvFmt = MTLPixelFormatRG8Unorm;
            return;
        case ZeroCopyBridgeMetal::YuvLayout::P010_10bit:
        case ZeroCopyBridgeMetal::YuvLayout::P210_10bit:
            *yFmt  = MTLPixelFormatR16Unorm;
            *uvFmt = MTLPixelFormatRG16Unorm;
            return;
        case ZeroCopyBridgeMetal::YuvLayout::Unknown:
        default:
            *yFmt = *uvFmt = MTLPixelFormatInvalid;
            return;
    }
}

QRhiTexture::Format qrhiFormatFor(MTLPixelFormat metalFmt)
{
    switch (metalFmt) {
        case MTLPixelFormatR8Unorm:   return QRhiTexture::R8;
        case MTLPixelFormatRG8Unorm:  return QRhiTexture::RG8;
        case MTLPixelFormatR16Unorm:  return QRhiTexture::R16;
        case MTLPixelFormatRG16Unorm: return QRhiTexture::RG16;
        default:                      return QRhiTexture::RGBA8;
    }
}

int colorMatrixIndexFor(CVPixelBufferRef pixbuf)
{
    // Pull the YCbCr matrix from the buffer's attached metadata.
    // VideoToolbox sets kCVImageBufferYCbCrMatrixKey to one of the
    // standard values. Default to BT.709 when missing (HD assumption).
    CFTypeRef raw = CVBufferGetAttachment(pixbuf,
        kCVImageBufferYCbCrMatrixKey, nullptr);
    if (raw && CFGetTypeID(raw) == CFStringGetTypeID()) {
        CFStringRef s = static_cast<CFStringRef>(raw);
        if (CFEqual(s, kCVImageBufferYCbCrMatrix_ITU_R_601_4))    return 0;
        if (CFEqual(s, kCVImageBufferYCbCrMatrix_ITU_R_709_2))    return 1;
        if (CFEqual(s, kCVImageBufferYCbCrMatrix_ITU_R_2020))     return 2;
        if (CFEqual(s, kCVImageBufferYCbCrMatrix_SMPTE_240M_1995)) return 0;
    }
    return 1; // BT.709 default
}

} // namespace

struct ZeroCopyBridgeMetal::Impl
{
    QRhi                   *rhi = nullptr;
    id<MTLDevice>           device = nil;
    CVMetalTextureCacheRef  cache  = nullptr;

    // Ownership: holds the previous frame's plane wrappers + their
    // QRhi wrappers. Released on next bridge() or dtor.
    std::vector<CVMetalTextureRef>     prevCvMetalTextures;
    std::vector<std::unique_ptr<QRhiTexture>> prevQrhiTextures;

    void releasePrev()
    {
        prevQrhiTextures.clear();   // unique_ptr destruction frees QRhiTextures
        for (CVMetalTextureRef t : prevCvMetalTextures) {
            if (t) CFRelease(t);
        }
        prevCvMetalTextures.clear();
    }
};

ZeroCopyBridgeMetal::ZeroCopyBridgeMetal(QRhi *rhi)
    : m_impl(std::make_unique<Impl>())
{
    m_impl->rhi = rhi;

    auto *native = static_cast<const QRhiMetalNativeHandles *>(rhi->nativeHandles());
    if (!native || !native->dev) {
        qWarning("ZeroCopyBridgeMetal: QRhi has no Metal device");
        return;
    }
    m_impl->device = (__bridge id<MTLDevice>)native->dev;

    CVReturn rc = CVMetalTextureCacheCreate(
        kCFAllocatorDefault, nullptr, m_impl->device, nullptr, &m_impl->cache);
    if (rc != kCVReturnSuccess) {
        qWarning("ZeroCopyBridgeMetal: CVMetalTextureCacheCreate failed (%d)", rc);
        m_impl->cache = nullptr;
    }
}

ZeroCopyBridgeMetal::~ZeroCopyBridgeMetal()
{
    if (!m_impl) return;
    m_impl->releasePrev();
    if (m_impl->cache) {
        CFRelease(m_impl->cache);
        m_impl->cache = nullptr;
    }
}

bool ZeroCopyBridgeMetal::canBridge(void *cvPixelBuffer)
{
    if (!cvPixelBuffer) return false;
    auto pix = static_cast<CVPixelBufferRef>(cvPixelBuffer);
    return layoutForCvFormat(CVPixelBufferGetPixelFormatType(pix)) != YuvLayout::Unknown;
}

ZeroCopyBridgeMetal::Result
ZeroCopyBridgeMetal::bridge(const FrameHandle &handle, Bridged *out)
{
    if (!out) return Result::Failed;
    *out = {};

    if (handle.kind() != FrameHandle::Kind::Metal) return Result::Failed;
    if (!m_impl || !m_impl->cache || !m_impl->device) return Result::Failed;

    auto pixbuf = static_cast<CVPixelBufferRef>(handle.metalPixelBuffer());
    if (!pixbuf) return Result::Failed;

    const OSType cvFmt = CVPixelBufferGetPixelFormatType(pixbuf);
    const YuvLayout layout = layoutForCvFormat(cvFmt);
    if (layout == YuvLayout::Unknown) {
        return Result::Unsupported;
    }
    // Free the previous frame's wrappers BEFORE we create the new
    // ones — the cache will reuse internals if it can.
    m_impl->releasePrev();

    auto makeQrhi = [&](id<MTLTexture> mtl, QRhiTexture::Format fmt,
                        QSize size) -> std::unique_ptr<QRhiTexture> {
        std::unique_ptr<QRhiTexture> tex(
            m_impl->rhi->newTexture(fmt, size, 1, QRhiTexture::Flags{}));
        QRhiTexture::NativeTexture nt{
            quintptr((__bridge void *)mtl), 0
        };
        if (!tex->createFrom(nt)) {
            qWarning("ZeroCopyBridgeMetal: QRhiTexture::createFrom failed");
            return nullptr;
        }
        return tex;
    };

    if (isPackedLayout(layout)) {
        // Single-plane packed format (AYpCbCr16). Wrap as a single
        // RGBA16Unorm MTLTexture; QRhi has no RGBA16Unorm so we
        // declare the QRhiTexture as RGBA16F — the underlying
        // MTLTexture's actual format determines GPU sampling
        // behavior (returns 0..1 floats either way), QRhi just
        // tracks the format for accounting.
        const size_t w = CVPixelBufferGetWidth(pixbuf);
        const size_t h = CVPixelBufferGetHeight(pixbuf);

        CVMetalTextureRef cv = nullptr;
        CVReturn rc = CVMetalTextureCacheCreateTextureFromImage(
            kCFAllocatorDefault, m_impl->cache, pixbuf, nullptr,
            MTLPixelFormatRGBA16Unorm, w, h, 0, &cv);
        if (rc != kCVReturnSuccess || !cv) {
            qWarning("ZeroCopyBridgeMetal: packed-plane texture create failed (%d)", rc);
            if (cv) CFRelease(cv);
            return Result::Failed;
        }

        id<MTLTexture> mtl = CVMetalTextureGetTexture(cv);
        auto qrhiTex = makeQrhi(mtl, QRhiTexture::RGBA16F,
                                QSize(static_cast<int>(w),
                                      static_cast<int>(h)));
        if (!qrhiTex) {
            CFRelease(cv);
            return Result::Failed;
        }

        out->yPlane      = qrhiTex.get();
        out->uvPlane     = nullptr;
        out->layout      = layout;
        out->colorMatrix = colorMatrixIndexFor(pixbuf);
        out->fullRange   = isFullRangeFormat(cvFmt) ? 1 : 0;

        m_impl->prevQrhiTextures.push_back(std::move(qrhiTex));
        m_impl->prevCvMetalTextures.push_back(cv);
        return Result::Ok;
    }

    // ---- Biplanar layouts (NV12 / P010 / P210)
    MTLPixelFormat yMtl, uvMtl;
    planeMetalFormatsForLayout(layout, &yMtl, &uvMtl);

    const size_t yW  = CVPixelBufferGetWidthOfPlane(pixbuf, 0);
    const size_t yH  = CVPixelBufferGetHeightOfPlane(pixbuf, 0);
    const size_t uvW = CVPixelBufferGetWidthOfPlane(pixbuf, 1);
    const size_t uvH = CVPixelBufferGetHeightOfPlane(pixbuf, 1);

    CVMetalTextureRef yCv = nullptr, uvCv = nullptr;
    CVReturn rc = CVMetalTextureCacheCreateTextureFromImage(
        kCFAllocatorDefault, m_impl->cache, pixbuf, nullptr,
        yMtl, yW, yH, 0, &yCv);
    if (rc != kCVReturnSuccess || !yCv) {
        qWarning("ZeroCopyBridgeMetal: Y plane texture create failed (%d)", rc);
        if (yCv) CFRelease(yCv);
        return Result::Failed;
    }

    rc = CVMetalTextureCacheCreateTextureFromImage(
        kCFAllocatorDefault, m_impl->cache, pixbuf, nullptr,
        uvMtl, uvW, uvH, 1, &uvCv);
    if (rc != kCVReturnSuccess || !uvCv) {
        qWarning("ZeroCopyBridgeMetal: UV plane texture create failed (%d)", rc);
        CFRelease(yCv);
        if (uvCv) CFRelease(uvCv);
        return Result::Failed;
    }

    id<MTLTexture> yTex  = CVMetalTextureGetTexture(yCv);
    id<MTLTexture> uvTex = CVMetalTextureGetTexture(uvCv);

    auto yQrhi  = makeQrhi(yTex,  qrhiFormatFor(yMtl),
                            QSize(static_cast<int>(yW),
                                  static_cast<int>(yH)));
    auto uvQrhi = makeQrhi(uvTex, qrhiFormatFor(uvMtl),
                            QSize(static_cast<int>(uvW),
                                  static_cast<int>(uvH)));

    if (!yQrhi || !uvQrhi) {
        CFRelease(yCv);
        CFRelease(uvCv);
        return Result::Failed;
    }

    out->yPlane     = yQrhi.get();
    out->uvPlane    = uvQrhi.get();
    out->layout     = layout;
    out->colorMatrix = colorMatrixIndexFor(pixbuf);
    out->fullRange  = isFullRangeFormat(cvFmt) ? 1 : 0;

    // Hand ownership of the wrappers + cache refs to Impl so they
    // survive until the next bridge() call.
    m_impl->prevQrhiTextures.push_back(std::move(yQrhi));
    m_impl->prevQrhiTextures.push_back(std::move(uvQrhi));
    m_impl->prevCvMetalTextures.push_back(yCv);
    m_impl->prevCvMetalTextures.push_back(uvCv);

    return Result::Ok;
}

} // namespace qcv
