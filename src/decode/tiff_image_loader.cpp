#include "tiff_image_loader.h"

#include <cstring>
#include <tiffio.h>
#include <vector>

namespace qcv {

namespace TIFFLoader {

bool getInfo(const std::string &path, int &width, int &height,
             int &channels, int &bit_depth, bool &is_float)
{
    TIFF *tif = TIFFOpen(path.c_str(), "r");
    if (!tif) return false;

    std::uint32_t w = 0, h = 0;
    std::uint16_t bits = 8, fmt = SAMPLEFORMAT_UINT, spp = 3;
    TIFFGetField(tif, TIFFTAG_IMAGEWIDTH,         &w);
    TIFFGetField(tif, TIFFTAG_IMAGELENGTH,        &h);
    TIFFGetFieldDefaulted(tif, TIFFTAG_BITSPERSAMPLE, &bits);
    TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLEFORMAT,  &fmt);
    TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLESPERPIXEL, &spp);

    width    = static_cast<int>(w);
    height   = static_cast<int>(h);
    channels = spp;
    bit_depth = bits;
    is_float = (fmt == SAMPLEFORMAT_IEEEFP);

    TIFFClose(tif);
    return true;
}

bool load(const std::string &path,
          std::vector<std::uint8_t> &pixel_data,
          int &width, int &height,
          PipelineMode &mode)
{
    TIFF *tif = TIFFOpen(path.c_str(), "r");
    if (!tif) return false;

    std::uint32_t w = 0, h = 0;
    std::uint16_t bits = 8, fmt = SAMPLEFORMAT_UINT, spp = 3;
    std::uint16_t planar = PLANARCONFIG_CONTIG;
    TIFFGetField(tif, TIFFTAG_IMAGEWIDTH,         &w);
    TIFFGetField(tif, TIFFTAG_IMAGELENGTH,        &h);
    TIFFGetFieldDefaulted(tif, TIFFTAG_BITSPERSAMPLE,  &bits);
    TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLEFORMAT,   &fmt);
    TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLESPERPIXEL, &spp);
    TIFFGetFieldDefaulted(tif, TIFFTAG_PLANARCONFIG,   &planar);
    (void)planar; // 8-bit path goes through TIFFReadRGBAImageOriented
                  // which handles planar internally; 16-bit assumes
                  // PLANARCONFIG_CONTIG (separate-plane TIFFs are rare
                  // and re-add when a real sample shows up).

    width  = static_cast<int>(w);
    height = static_cast<int>(h);

    if (bits == 8) {
        mode = PipelineMode::NORMAL;
    } else if (bits == 16) {
        // 16-bit float TIFF is rare — most 16-bit TIFFs are uint.
        // The float path needs FP16 support in FrameHandle which
        // doesn't ship until 7.4.b; for now mark HIGH_RES uint and
        // emit RGBA16. If a 16-bit-float TIFF shows up the pixels
        // will look wrong (they'd be reinterpreted as uint16) —
        // worth a follow-up clamp once we know the format is real
        // in customer files.
        mode = (fmt == SAMPLEFORMAT_IEEEFP)
            ? PipelineMode::ULTRA_HIGH_RES
            : PipelineMode::HIGH_RES;
    } else if (bits == 32 && fmt == SAMPLEFORMAT_IEEEFP) {
        mode = PipelineMode::ULTRA_HIGH_RES;
    } else {
        mode = PipelineMode::NORMAL;
    }

    if (bits == 8) {
        // libtiff's RGBAImageOriented helper converts ANYTHING that
        // looks RGB-y (palette, CMYK, YCbCr, gray, separations) into
        // an ABGR uint32 buffer. We then unswizzle into our RGBA8
        // output. ORIENTATION_TOPLEFT means we don't have to flip Y.
        std::vector<std::uint32_t> abgr(
            static_cast<std::size_t>(w) * h);
        if (!TIFFReadRGBAImageOriented(tif, w, h, abgr.data(),
                                       ORIENTATION_TOPLEFT, 0)) {
            TIFFClose(tif);
            return false;
        }
        pixel_data.resize(static_cast<std::size_t>(w) * h * 4);
        for (std::size_t i = 0; i < static_cast<std::size_t>(w) * h; ++i) {
            const std::uint32_t v = abgr[i];
            pixel_data[i * 4 + 0] = TIFFGetR(v);
            pixel_data[i * 4 + 1] = TIFFGetG(v);
            pixel_data[i * 4 + 2] = TIFFGetB(v);
            pixel_data[i * 4 + 3] = TIFFGetA(v);
        }
    } else {
        // 16-bit per channel — read per-scanline, expand 3-channel
        // to RGBA16 by synthesizing alpha = 65535. libtiff handles
        // the host-endian swap when TIFFIsByteSwapped() is true,
        // so we DO NOT manually swap (that's the line-235 comment
        // in the old image_loaders.cpp marked load-bearing).
        const std::size_t bytesPerSample = (bits + 7) / 8;
        const tmsize_t scanlineSize = TIFFScanlineSize(tif);
        std::vector<std::uint8_t> scanline(scanlineSize);
        pixel_data.assign(
            static_cast<std::size_t>(h) * w * 4 * bytesPerSample, 0);

        for (std::uint32_t row = 0; row < h; ++row) {
            if (TIFFReadScanline(tif, scanline.data(),
                                 row) < 0) {
                TIFFClose(tif);
                return false;
            }
            std::uint8_t *dest =
                pixel_data.data() +
                static_cast<std::size_t>(row) * w * 4 * bytesPerSample;
            if (spp == 4) {
                // Direct copy — TIFF stores RGBA in source order.
                std::memcpy(dest, scanline.data(),
                            static_cast<std::size_t>(w) * 4 * bytesPerSample);
            } else if (spp == 3) {
                if (bits == 16) {
                    const std::uint16_t *src =
                        reinterpret_cast<const std::uint16_t *>(scanline.data());
                    std::uint16_t *dst =
                        reinterpret_cast<std::uint16_t *>(dest);
                    for (std::uint32_t x = 0; x < w; ++x) {
                        dst[x * 4 + 0] = src[x * 3 + 0];
                        dst[x * 4 + 1] = src[x * 3 + 1];
                        dst[x * 4 + 2] = src[x * 3 + 2];
                        dst[x * 4 + 3] = 65535;
                    }
                } else {
                    // 32-bit float per channel — currently treated
                    // as uint by the FrameHandle path. This branch
                    // exists for completeness; 32-float TIFFs land
                    // in 7.4.b along with EXR's FP16 surface.
                    std::memcpy(dest, scanline.data(),
                                static_cast<std::size_t>(w) * 3 * bytesPerSample);
                }
            } else {
                // Unexpected channel count — bail rather than emit
                // half-decoded pixels.
                TIFFClose(tif);
                return false;
            }
        }
    }

    TIFFClose(tif);
    return true;
}

} // namespace TIFFLoader

// ---------------------------------------------------------------------------
// TIFFImageLoader (IImageLoader implementation)
// ---------------------------------------------------------------------------

std::shared_ptr<PixelData> TIFFImageLoader::loadFrame(
    const std::string &path,
    const std::string & /*layer*/,
    PipelineMode pipeline_mode)
{
    auto pd = std::make_shared<PixelData>();
    PipelineMode detected = pipeline_mode;
    if (!TIFFLoader::load(path, pd->pixels, pd->width, pd->height, detected)) {
        return nullptr;
    }
    pd->pipeline_mode = detected;
    pd->setFormat(detected == PipelineMode::NORMAL
                  ? PixelFormat::RGBA8
                  : PixelFormat::RGBA16);
    return pd;
}

bool TIFFImageLoader::getDimensions(const std::string &path,
                                    int &width, int &height)
{
    int channels = 0, bit_depth = 0;
    bool is_float = false;
    return TIFFLoader::getInfo(path, width, height,
                               channels, bit_depth, is_float);
}

} // namespace qcv
