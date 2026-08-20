#pragma once

// rgb_range — legal-range (16–235) → full-range expansion for RGB-family
// sources on the CPU RGBA8 paths.
//
// swscale applies no range handling to an RGB→RGB conversion (its
// srcRange only feeds YUV→RGB matrices), so every place that turns an
// RGB AVFrame (gbrp*, rgb48, …) into an RGBA8888 buffer has to do the
// expansion itself — otherwise the Range pill is inert for RGB and
// legal-range RGB masters (Avid DNxHR 444 RGB, MXF RGBA tagged 64/940)
// come up flat. One rule for all of them, identical to the GPU branches
// (Windows Vulkan compositor RGB path):
//
//   override Full    → never
//   override Limited → always
//   Auto             → when the frame is tagged limited (container tag,
//                      or our vendored dnxhddec's Avid legal-range
//                      convention stamp)
//
// Used by VideoDecoder::publishCpuFrame (playback), swsFrameToRgbaImage
// (single + dual scrub decoders), DualVideoDecoder's CPU path and the
// thumbnail loader — so scrub, play, dual and thumbs all agree.

#include <algorithm>
#include <array>
#include <cstdint>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
}

namespace qcv {

inline bool isRgbPixelFormat(int avPixFmt)
{
    const AVPixFmtDescriptor *desc =
        av_pix_fmt_desc_get(static_cast<AVPixelFormat>(avPixFmt));
    return desc && (desc->flags & AV_PIX_FMT_FLAG_RGB);
}

// rangeOverride: 0 = Auto, 1 = Full, 2 = Limited (MediaItem pill).
inline bool rgbFrameNeedsLegalExpansion(const AVFrame *frame, int rangeOverride)
{
    if (!frame || !isRgbPixelFormat(frame->format)) return false;
    if (rangeOverride == 1) return false;
    if (rangeOverride == 2) return true;
    return frame->color_range == AVCOL_RANGE_MPEG;
}

// In-place 16–235 → 0–255 on packed RGBA8888 rows; alpha untouched.
inline void expandRgba8LegalToFull(uint8_t *data, int width, int height, int stride)
{
    static const std::array<uint8_t, 256> kLut = [] {
        std::array<uint8_t, 256> t{};
        for (int v = 0; v < 256; ++v) {
            const int e = ((v - 16) * 255 + 109) / 219;   // round
            t[v] = static_cast<uint8_t>(std::clamp(e, 0, 255));
        }
        return t;
    }();
    if (!data || width <= 0 || height <= 0) return;
    for (int y = 0; y < height; ++y) {
        uint8_t *row = data + static_cast<std::ptrdiff_t>(y) * stride;
        for (int x = 0; x < width; ++x, row += 4) {
            row[0] = kLut[row[0]];
            row[1] = kLut[row[1]];
            row[2] = kLut[row[2]];
        }
    }
}

} // namespace qcv
