// ScrubFrameCache — decoded-GOP frame cache for the scrub decoders.
//
// Why this exists: inter-frame video (h264/h265 mp4) can only be decoded
// forward, so the old scrub path re-seeked to the GOP keyframe and
// re-decoded the whole keyframe->target prefix on EVERY scrub step,
// dropping the intermediate frames it had just decoded. That makes
// forward scrub O(N^2) within a GOP and backward scrub the worst case.
//
// QuickTime / MPV / AVFoundation all avoid this the same way: decode a
// GOP once, KEEP the frames, then serve forward/backward motion within
// the decoded window from cache. This file is the "keep the frames" part:
// a byte-bounded LRU keyed by display-frame-number, populated as a free
// byproduct of the forward decode the scrub worker already does.
//
// Entries hold the decoded frame in its CHEAP NATIVE form and defer the
// expensive YUV->RGBA convert to display time. This matters on the cold
// path: filling a GOP must not pay a full sws_scale + 33 MB RGBA malloc
// per prefix frame (that made entering an uncached GOP slower than the
// old drop-everything code). Two kinds:
//   - Yuv   : owns one AVFrame ref to the decoded (software / hw-readback)
//             planar frame. Cloning is a cheap refcount bump; the scrub
//             decoder swscales it to RGBA only when the frame is shown.
//   - Metal : owns one CVPixelBuffer retain (zero-copy VideoToolbox). The
//             Metal compositor already does YUV->RGB on the GPU, so there
//             is nothing to convert — re-publish just re-retains.
//
// There is intentionally no zero-copy Vulkan/D3D11/VAAPI cache kind: those
// decoders draw output frames from a FIXED-SIZE hw_frames_ctx pool, so
// holding a GOP's worth would starve the decoder and deadlock. The scrub
// decoder reads those backends back into a Yuv entry instead. CoreVideo's
// VT pools grow on demand, so Metal is the one HW backend safe to hold.
//
// The dual scrub decoders cache std::shared_ptr<DualFrame> directly
// (DualFrame is already shared-ownership for every kind), so they only
// borrow the SimpleLRU<int, shared_ptr<...>> shape, not ScrubCacheEntry.

#pragma once

#include "frame_handle.h"

#include <cstddef>
#include <cstdint>
#include <memory>

struct AVFrame;

namespace qcv {

class ScrubCacheEntry {
public:
    enum class Kind { Yuv, Metal };

    // Yuv — `ownedRef` must be a fresh av_frame_clone()'d ref to a planar
    // (software / hw-readback) frame; the entry takes ownership and
    // av_frame_free's it on destroy. `bytes` is the decoded buffer size.
    static std::shared_ptr<ScrubCacheEntry> yuv(AVFrame *ownedRef,
                                                int64_t pts, std::size_t bytes);

    // Metal — `cvPixRetained` must already carry a retain that this entry
    // takes ownership of (released on destroy).
    static std::shared_ptr<ScrubCacheEntry> metal(void *cvPixRetained,
                                                  int width, int height,
                                                  int64_t pts);

    ~ScrubCacheEntry();
    ScrubCacheEntry(const ScrubCacheEntry &) = delete;
    ScrubCacheEntry &operator=(const ScrubCacheEntry &) = delete;

    Kind        kind() const { return m_kind; }
    int64_t     pts() const { return m_pts; }
    std::size_t bytes() const { return m_bytes; }

    // Yuv: the borrowed planar AVFrame (entry retains ownership). The scrub
    // decoder swscales this to RGBA. nullptr for Metal entries.
    AVFrame *yuvFrame() const { return m_yuv; }

    // Metal: a fresh, fully-owned zero-copy handle (re-retains the pixel
    // buffer). Empty handle for Yuv entries.
    FrameHandle makeMetalHandle() const;

private:
    ScrubCacheEntry() = default;

    Kind        m_kind = Kind::Yuv;
    int64_t     m_pts = 0;
    int         m_width = 0;
    int         m_height = 0;
    std::size_t m_bytes = 0;

    AVFrame *m_yuv = nullptr;       // Yuv: owns one ref
    void    *m_cvpix = nullptr;     // Metal: owns one retain
};

} // namespace qcv
