#include "scrub_frame_cache.h"

extern "C" {
#include <libavutil/frame.h>
}

// CoreVideo bridges — defined in frame_handle.mm (Apple) /
// frame_handle_nonapple.cpp (inert stubs). Declared header-free so this
// cross-platform .cpp doesn't drag CoreVideo in.
extern "C" void cvPixelBufferRetainRaw(void *cvPix);
extern "C" void cvPixelBufferReleaseRaw(void *cvPix);

namespace qcv {

std::shared_ptr<ScrubCacheEntry>
ScrubCacheEntry::yuv(AVFrame *ownedRef, int64_t pts, std::size_t bytes)
{
    auto e = std::shared_ptr<ScrubCacheEntry>(new ScrubCacheEntry());
    e->m_kind   = Kind::Yuv;
    e->m_pts    = pts;
    e->m_width  = ownedRef ? ownedRef->width : 0;
    e->m_height = ownedRef ? ownedRef->height : 0;
    e->m_bytes  = bytes;
    e->m_yuv    = ownedRef;     // takes ownership of the cloned ref
    return e;
}

std::shared_ptr<ScrubCacheEntry>
ScrubCacheEntry::metal(void *cvPixRetained, int width, int height, int64_t pts)
{
    auto e = std::shared_ptr<ScrubCacheEntry>(new ScrubCacheEntry());
    e->m_kind   = Kind::Metal;
    e->m_pts    = pts;
    e->m_width  = width;
    e->m_height = height;
    e->m_cvpix  = cvPixRetained;    // takes ownership of the passed-in retain
    // Backing-store size of the CVPixelBuffer (~12 MB / 4K NV12). The exact
    // query lives behind cvPixelBufferByteSizeRaw; a w*h*2 estimate is fine
    // here since Metal entries are bounded by the byte budget either way.
    e->m_bytes  = static_cast<std::size_t>(width) * height * 2;
    return e;
}

ScrubCacheEntry::~ScrubCacheEntry()
{
    if (m_cvpix) cvPixelBufferReleaseRaw(m_cvpix);
    if (m_yuv)   av_frame_free(&m_yuv);
}

FrameHandle ScrubCacheEntry::makeMetalHandle() const
{
    if (m_kind != Kind::Metal || !m_cvpix) return FrameHandle();
    // The cache keeps its retain; the published handle gets its own.
    cvPixelBufferRetainRaw(m_cvpix);
    return FrameHandle::metal(m_cvpix, m_width, m_height, m_pts);
}

} // namespace qcv
