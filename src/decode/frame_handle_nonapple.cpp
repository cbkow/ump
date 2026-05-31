// FrameHandle implementation for non-Apple platforms.
//
// frame_handle.mm provides the full implementation on macOS (where it
// also imports CoreVideo for the CVPixelBuffer-backed FrameHandle::metal
// variant). On Windows / Linux we only need the cross-platform pieces
// (the CPU-image-backed variants + lifecycle); the CVPixelBuffer extern
// stubs return false so any (legacy / cross-platform) caller that still
// probes the macOS zero-copy path takes the CPU fallback branch
// gracefully.
//
// Phase F.2.0 introduced this file to make the Windows link succeed
// when QCV_NATIVE_PLAYER is ON. The cleaner long-term split is to move
// the cross-platform methods out of frame_handle.mm into a shared
// .cpp; that's a follow-up cleanup, not a Phase F prerequisite.

#include "frame_handle.h"

extern "C" {
#include <libavutil/frame.h>
}

#include <cstdint>

// Stubs for the Apple-specific extern "C" callbacks. Defined on Apple
// in frame_handle.mm using CVPixelBuffer; here we return inert values
// so callers compile + behave conservatively.
extern "C" bool cvPixelBufferIsZeroCopySupportedRaw(void * /*cvPix*/)
{
    return false;   // no zero-copy path on non-Apple — always CPU readback
}

extern "C" void cvPixelBufferRetainRaw(void * /*cvPix*/)
{
    // No-op — non-Apple FrameHandle never holds a CVPixelBuffer.
}

extern "C" void cvPixelBufferReleaseRaw(void * /*cvPix*/)
{
    // No-op — non-Apple never holds a CVPixelBuffer in the scrub cache.
}

extern "C" unsigned int cvPixelBufferFormatTypeRaw(void * /*cvPix*/)
{
    return 0;
}

extern "C" unsigned long cvPixelBufferByteSizeRaw(void * /*cvPix*/)
{
    return 0;   // never holds a CVPixelBuffer on non-Apple
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
    m_avFrame     = other.m_avFrame;
    m_keepAlive   = std::move(other.m_keepAlive);
    other.m_kind = Kind::Empty;
    other.m_pts = 0;
    other.m_width = other.m_height = 0;
    other.m_metalPixbuf = nullptr;
    other.m_avFrame     = nullptr;
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
    h.m_cpuImage  = std::move(view);
    h.m_keepAlive = std::move(keepAlive);
    return h;
}

FrameHandle FrameHandle::metal(void * /*cvPixelBuffer*/, int /*w*/, int /*h*/,
                                 int64_t /*pts*/)
{
    // Never constructed on non-Apple. Return an empty handle so any
    // accidental caller is safe.
    return FrameHandle();
}

FrameHandle FrameHandle::vulkan(AVFrame *avFrame, int width, int height,
                                int64_t pts)
{
    FrameHandle h;
    h.m_kind    = Kind::Vulkan;
    h.m_pts     = pts;
    h.m_width   = width;
    h.m_height  = height;
    h.m_avFrame = avFrame;   // takes ownership; reset() will av_frame_free
    return h;
}

void FrameHandle::reset()
{
    // CVPixelBufferRelease isn't reachable on non-Apple — m_metalPixbuf
    // is never populated. AVFrame ref drops the AVVkFrame ref, which
    // returns the VkImage to FFmpeg's decode pool when the count hits
    // zero.
    m_metalPixbuf = nullptr;
    if (m_avFrame) {
        av_frame_free(&m_avFrame);
    }
    m_cpuImage = QImage();
    m_keepAlive.reset();
    m_kind = Kind::Empty;
    m_pts = 0;
    m_width = m_height = 0;
}

} // namespace qcv
