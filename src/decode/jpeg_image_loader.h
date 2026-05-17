// JPEGImageLoader — Phase 7.4.c port from old QCView's
// `image_loaders.cpp` JPEGLoader namespace + JPEGImageLoader class.
//
// libjpeg-turbo backed loader. JPEG is always 8-bit, always
// NORMAL pipeline mode — the loader synthesizes an opaque alpha
// channel (RGB → RGBA, grayscale → RGBA mono) so the output
// matches the rest of the IImageLoader contract.
//
// Phase 7.4.c ships only the full-frame load. The DCT-scaling
// thumbnail trick (libjpeg's scale_num=1 / scale_denom={2,4,8})
// from the old app's JPEGImageLoader::LoadThumbnail is the
// load-bearing optimization for thumbnails — porting that lands
// when we wire the inspector / bin thumbnail path in 7.4.b+.
//
// Thread safety: each LoadFrame creates its own jpeg_decompress
// context. Multiple instances can decode in parallel.

#pragma once

#include "image_loader.h"

namespace qcv {

class JPEGImageLoader : public IImageLoader {
public:
    std::shared_ptr<PixelData> loadFrame(
        const std::string &path,
        const std::string &layer,           // ignored
        PipelineMode pipeline_mode) override;

    bool getDimensions(const std::string &path,
                       int &width, int &height) override;

    std::string loaderName() const override { return "JPEG"; }
};

namespace JPEGLoader {

bool load(const std::string &path,
          std::vector<std::uint8_t> &pixel_data,
          int &width, int &height,
          PipelineMode &mode);

bool getInfo(const std::string &path,
             int &width, int &height,
             int &channels);

} // namespace JPEGLoader

} // namespace qcv
