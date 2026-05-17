// TIFFImageLoader — Phase 7.4.c port from old QCView's
// `image_loaders.cpp` TIFFLoader namespace + TIFFImageLoader class.
//
// libtiff-backed loader. Handles 8-bit (via TIFFReadRGBAImageOriented
// → ABGR uint32 → RGBA8) and 16-bit (per-scanline reads → RGBA16),
// including 3-channel sources where alpha is synthesized as full-
// opacity. The byte-swap detail is load-bearing: libtiff handles
// host-endian conversion automatically when TIFFIsByteSwapped() is
// true, so we do NOT swap manually — doing so on a host that's
// already swapped would corrupt the pixels.
//
// 32-bit float TIFF surfaces ULTRA_HIGH_RES which the renderer maps
// to FP16 in 7.4.b once the EXR FP16 path lands; until then those
// files load as broken sentinels (the loader itself is fine, but
// our FrameHandle adapter doesn't ship a float kind yet).
//
// Thread safety: each LoadFrame creates its own libtiff context.
// Multiple instances decode concurrently — the I/O thread allocates
// fresh per task.

#pragma once

#include "image_loader.h"

namespace qcv {

class TIFFImageLoader : public IImageLoader {
public:
    std::shared_ptr<PixelData> loadFrame(
        const std::string &path,
        const std::string &layer,           // ignored
        PipelineMode pipeline_mode) override;

    bool getDimensions(const std::string &path,
                       int &width, int &height) override;

    std::string loaderName() const override { return "TIFF"; }
};

namespace TIFFLoader {

bool load(const std::string &path,
          std::vector<std::uint8_t> &pixel_data,
          int &width, int &height,
          PipelineMode &mode);

bool getInfo(const std::string &path,
             int &width, int &height,
             int &channels, int &bit_depth,
             bool &is_float);

} // namespace TIFFLoader

} // namespace qcv
