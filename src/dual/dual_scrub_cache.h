// DualScrubEntry — cache entry for the dual-view scrub GOP cache.
//
// Mirrors src/decode/scrub_frame_cache.h (ScrubCacheEntry) for the dual
// A/B scrub decoders. Same idea: decode each GOP once, keep the frames,
// serve forward/backward motion within the decoded window from cache so
// scrub stops being O(N^2) forward / worst-case backward on inter codecs.
//
// The dual decoders publish a std::shared_ptr<DualFrame>, so an entry is
// one of:
//   - ready : a pre-made DualFrame (zero-copy Metal on macOS). The Metal
//             compositor does YUV->RGB on the GPU, so re-publishing is
//             just handing the shared_ptr back — nothing to convert.
//   - yuv   : a cloned planar AVFrame (software / hw-readback). Cloning is
//             a cheap refcount bump; the scrub decoder swscales it to an
//             RGBA Cpu DualFrame only when the frame is actually shown.
//             Deferring the convert keeps the cold GOP fill cheap (a full
//             sws_scale + RGBA malloc per prefix frame is what made an
//             eager cache slower than the old drop-everything path).
//
// As in single-flow there is no zero-copy Vulkan/D3D11/VAAPI cache kind:
// those decoders draw from a fixed-size pool that holding would deadlock,
// and on Win/Linux the dual scrub deliberately stays off the GPU compositor
// for NVIDIA stability anyway — so those backends read back into a yuv entry.

#pragma once

#include "i_dual_source.h"      // DualFrame

#include <cstddef>
#include <memory>

struct AVFrame;

namespace qcv::dual {

struct DualScrubEntry {
    int         frameNumber = -1;
    std::size_t bytes = 0;

    // Exactly one is set. `ready` short-circuits to a direct re-publish;
    // `yuv` is swscaled on demand by the decoder (which owns the SwsContext).
    std::shared_ptr<DualFrame> ready;
    std::shared_ptr<AVFrame>   yuv;   // av_frame_free deleter, set by decoder
};

} // namespace qcv::dual
