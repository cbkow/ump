#pragma once

// swsFrameToRgbaImage — convert a planar/NV12 AVFrame to a packed-RGBA QImage
// via swscale, writing into a destination buffer with the padding swscale's
// SIMD output routines require.
//
// Why the padding matters: swscale's vectorized YUV->RGBA writers store the
// final scanline in fixed pixel blocks (16 px on the AVX2 path), rounding the
// row width up to a whole block. For a width that is NOT a multiple of that
// block — e.g. a portrait 1080-wide clip (1080 % 16 != 0) — the last row's
// store runs a few bytes past the nominal end. A bare QImage(w, h) ends
// exactly at its allocation (often a page boundary), so that overshoot writes
// into an unmapped page and access-violates (0xC0000005). Landscape widths
// (1920/1280/3840) are 16-multiples and never tripped it, which is why only
// odd-width clips crashed. See the scrub-sws-crash diagnosis.
//
// Fix: allocate our own 32-byte-aligned RGBA target with one extra row of
// slack, scale into that, and hand the QImage ownership of the buffer (freed
// via av_free through the QImage cleanup hook). Zero-copy from here on — the
// QImage references the buffer directly. Shared by every scrub publish path
// (single-flow + dual A/B, Windows + macOS).

#include "decode/rgb_range.h"

#include <QImage>

#include <cstddef>
#include <cstdint>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/mem.h>
#include <libswscale/swscale.h>
}

namespace qcv {

// `expandLegalRgb`: apply the RGB legal→full expansion after the scale
// (see rgb_range.h) — callers pass rgbFrameNeedsLegalExpansion(yf, ov).
inline QImage swsFrameToRgbaImage(SwsContext *sws, const AVFrame *yf,
                                  bool expandLegalRgb = false)
{
    if (!sws || !yf || yf->width <= 0 || yf->height <= 0) return {};
    const int w = yf->width;
    const int h = yf->height;
    const int stride = (w * 4 + 31) & ~31;   // 32-byte-aligned RGBA pitch
    // +1 row of trailing slack absorbs swscale's last-row block overshoot.
    const std::size_t bufSize = static_cast<std::size_t>(stride) * (h + 1);
    auto *buf = static_cast<uint8_t *>(av_malloc(bufSize));
    if (!buf) return {};

    uint8_t *dst[4]  = { buf, nullptr, nullptr, nullptr };
    int dstStride[4] = { stride, 0, 0, 0 };
    sws_scale(sws, yf->data, yf->linesize, 0, h, dst, dstStride);
    if (expandLegalRgb) expandRgba8LegalToFull(buf, w, h, stride);

    return QImage(buf, w, h, stride, QImage::Format_RGBA8888,
                  [](void *p) { av_free(p); }, buf);
}

} // namespace qcv
