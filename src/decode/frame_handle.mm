// FrameHandle implementation. Built as Objective-C++ on macOS so the
// destructor can CFRelease the CVPixelBufferRef. On other platforms
// it compiles as plain C++ (the .mm extension is harmless under
// non-macOS only when the build system is told to compile it as
// Objective-C++ explicitly — for now this file is macOS-only and
// added to the build via target_sources guarded by APPLE).

#include "frame_handle.h"

#import <CoreVideo/CoreVideo.h>

// Bridge so video_decoder.cpp can retain a CVPixelBuffer without
// including CoreVideo. Defined here where CoreVideo is already in
// scope. Header-free linkage — declared `extern` at the call site.
extern "C" void cvPixelBufferRetainRaw(void *cvPix)
{
    if (cvPix) {
        CVPixelBufferRetain(static_cast<CVPixelBufferRef>(cvPix));
    }
}

// Counterpart to cvPixelBufferRetainRaw. The scrub GOP cache holds its
// own retain on a cached CVPixelBuffer and must drop it on eviction
// without dragging CoreVideo into the cross-platform cache .cpp.
extern "C" void cvPixelBufferReleaseRaw(void *cvPix)
{
    if (cvPix) {
        CVPixelBufferRelease(static_cast<CVPixelBufferRef>(cvPix));
    }
}

// Returns true if the CVPixelBuffer's format type is one our render-
// thread bridge can sample as plane MTLTextures. Decoder uses this
// to decide between the zero-copy publish path and the CPU readback
// fallback. Mirror the format set in
// ZeroCopyBridgeMetal::layoutForCvFormat.
// Diagnostic-only — used to surface which CVPixelFormat
// VideoToolbox produced when we hit the non-zero-copy fallback.
extern "C" unsigned int cvPixelBufferFormatTypeRaw(void *cvPix)
{
    if (!cvPix) return 0;
    return static_cast<unsigned int>(
        CVPixelBufferGetPixelFormatType(
            static_cast<CVPixelBufferRef>(cvPix)));
}

// Total backing-store bytes of a CVPixelBuffer (sum of all planes),
// used by the scrub GOP cache to budget held GPU surfaces accurately
// instead of guessing from width*height*bpp.
extern "C" unsigned long cvPixelBufferByteSizeRaw(void *cvPix)
{
    if (!cvPix) return 0;
    return static_cast<unsigned long>(
        CVPixelBufferGetDataSize(static_cast<CVPixelBufferRef>(cvPix)));
}

extern "C" bool cvPixelBufferIsZeroCopySupportedRaw(void *cvPix)
{
    if (!cvPix) return false;
    const OSType fmt = CVPixelBufferGetPixelFormatType(
        static_cast<CVPixelBufferRef>(cvPix));
    switch (fmt) {
        // 8-bit biplanar — 420v/420f shipped from day one; 422v/422f
        // and 444v/444f added so high-quality SDR ProRes / HEVC 4:2:2
        // / HEVC 4:4:4 SDR doesn't fall to the CPU path.
        case kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange:
        case kCVPixelFormatType_420YpCbCr8BiPlanarFullRange:
        case kCVPixelFormatType_422YpCbCr8BiPlanarVideoRange:
        case kCVPixelFormatType_422YpCbCr8BiPlanarFullRange:
        case kCVPixelFormatType_444YpCbCr8BiPlanarVideoRange:
        case kCVPixelFormatType_444YpCbCr8BiPlanarFullRange:
        // 10-bit biplanar — 4:2:0 / 4:2:2 / 4:4:4 in MSBs of 16-bit.
        // x420/xf20: HDR10 / HEVC Main10. x422/xf22: ProRes 422 HQ /
        // HEVC 4:2:2 10. x444/xf44: HEVC 4:4:4 10.
        case kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange:
        case kCVPixelFormatType_420YpCbCr10BiPlanarFullRange:
        case kCVPixelFormatType_422YpCbCr10BiPlanarVideoRange:
        case kCVPixelFormatType_422YpCbCr10BiPlanarFullRange:
        case kCVPixelFormatType_444YpCbCr10BiPlanarVideoRange:
        case kCVPixelFormatType_444YpCbCr10BiPlanarFullRange:
        // sv22 / sv44 = 16-bit biplanar 4:2:2 / 4:4:4, video range —
        // ProRes 4444 12-bit, HEVC 4:2:2 / 4:4:4 high-bit-depth.
        case kCVPixelFormatType_422YpCbCr16BiPlanarVideoRange:
        case kCVPixelFormatType_444YpCbCr16BiPlanarVideoRange:
        // Packed 4:4:4:4 16-bit (ProRes 4444 alpha variant). Single
        // plane, sampled as RGBA16Unorm via the interleaved kernel.
        case kCVPixelFormatType_4444AYpCbCr16:
            return true;
        default:
            return false;
    }
}

namespace qcv {

FrameHandle &FrameHandle::operator=(FrameHandle &&other) noexcept
{
    if (this == &other) return *this;
    reset();
    m_kind        = other.m_kind;
    m_pts         = other.m_pts;
    m_width       = other.m_width;
    m_height      = other.m_height;
    m_cpuImage    = std::move(other.m_cpuImage);
    m_metalPixbuf = other.m_metalPixbuf;
    m_keepAlive   = std::move(other.m_keepAlive);
    other.m_kind = Kind::Empty;
    other.m_pts = 0;
    other.m_width = other.m_height = 0;
    other.m_metalPixbuf = nullptr;
    return *this;
}

FrameHandle FrameHandle::cpu(QImage image, int64_t pts)
{
    FrameHandle h;
    h.m_kind     = Kind::Cpu;
    h.m_pts      = pts;
    h.m_width    = image.width();
    h.m_height   = image.height();
    h.m_cpuImage = std::move(image);
    return h;
}

FrameHandle FrameHandle::cpuShared(QImage view,
                                   std::shared_ptr<void> keepAlive,
                                   int64_t pts)
{
    FrameHandle h;
    h.m_kind      = Kind::Cpu;
    h.m_pts       = pts;
    h.m_width     = view.width();
    h.m_height    = view.height();
    h.m_cpuImage  = std::move(view);    // non-owning view; bits valid
                                        // while keepAlive is alive
    h.m_keepAlive = std::move(keepAlive);
    return h;
}

FrameHandle FrameHandle::metal(void *cvPixelBuffer, int width, int height, int64_t pts)
{
    FrameHandle h;
    h.m_kind         = Kind::Metal;
    h.m_pts          = pts;
    h.m_width        = width;
    h.m_height       = height;
    h.m_metalPixbuf  = cvPixelBuffer;   // already retained by caller
    return h;
}

void FrameHandle::reset()
{
    if (m_kind == Kind::Metal && m_metalPixbuf) {
        CVPixelBufferRelease(static_cast<CVPixelBufferRef>(m_metalPixbuf));
    }
    m_metalPixbuf = nullptr;
    m_cpuImage = QImage();
    m_keepAlive.reset();
    m_kind = Kind::Empty;
    m_pts = 0;
    m_width = m_height = 0;
}

} // namespace qcv
