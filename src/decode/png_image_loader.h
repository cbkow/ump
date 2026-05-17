// PNGImageLoader — Phase 7.4.a port from old QCView's
// `image_loaders.cpp` PNGLoader namespace + PNGImageLoader class.
//
// libpng-backed loader. Auto-expands palette/grayscale, adds an
// opaque alpha channel when the source has none, and applies
// `png_set_swap()` on little-endian systems for 16-bit PNGs (PNG
// stores in network byte order = big-endian; the swap is the
// load-bearing detail that makes 16-bit PNG read correctly on
// macOS / Windows / x86 Linux).
//
// Thread safety: Each LoadFrame call constructs a fresh libpng
// read context — multiple instances can decode concurrently. The
// ImageSequenceDecoder I/O thread relies on this, allocating a
// fresh PNGImageLoader per async task.

#pragma once

#include "image_loader.h"

namespace qcv {

class PNGImageLoader : public IImageLoader {
public:
    std::shared_ptr<PixelData> loadFrame(
        const std::string &path,
        const std::string &layer,           // ignored
        PipelineMode pipeline_mode) override;

    // Fast thumbnail loading — same libpng transformations as the
    // full path, then per-pixel skip-downsample. PNG can't decode
    // partial regions, so we still pay the full-image read; the
    // only saving is on the destination buffer + GPU upload.
    std::shared_ptr<PixelData> loadThumbnail(
        const std::string &path, int max_size = 320) override;

    bool getDimensions(const std::string &path,
                       int &width, int &height) override;

    std::string loaderName() const override { return "PNG"; }
};

// Free-function namespace mirrors the old app's PNGLoader. Kept for
// the cases where callers want pixel data without going through the
// IImageLoader vtable (matches old-app pattern; ImageSequenceDecoder
// uses the class form).
namespace PNGLoader {

// Returns false on any libpng failure or non-existent file.
// pixel_data ends up as RGBA bytes; mode reflects detected bit depth
// (8-bit → NORMAL, 16-bit → HIGH_RES).
bool load(const std::string &path,
          std::vector<std::uint8_t> &pixel_data,
          int &width, int &height,
          PipelineMode &mode);

bool getInfo(const std::string &path,
             int &width, int &height,
             int &channels, int &bit_depth);

} // namespace PNGLoader

} // namespace qcv
