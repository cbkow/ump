#include "png_image_loader.h"

#include <algorithm>
#include <cstdio>
#include <png.h>

#ifdef _WIN32
#  include <string>
#endif

namespace qcv {

// ---------------------------------------------------------------------------
// PNGLoader free functions
// ---------------------------------------------------------------------------

namespace PNGLoader {

namespace {

// Cross-platform fopen — Windows path may be UTF-16, but our
// std::string paths arrive as UTF-8 from QString. _wfopen takes
// wchar_t; use _wfopen_s with a UTF-8→wide conversion in a future
// hardening pass. For now match the old app's narrow fopen on win.
std::FILE *openBinary(const std::string &path)
{
#ifdef _WIN32
    return std::fopen(path.c_str(), "rb");
#else
    return std::fopen(path.c_str(), "rb");
#endif
}

} // namespace

bool getInfo(const std::string &path, int &width, int &height,
             int &channels, int &bit_depth)
{
    std::FILE *fp = openBinary(path);
    if (!fp) return false;

    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING,
                                             nullptr, nullptr, nullptr);
    if (!png) { std::fclose(fp); return false; }

    png_infop info_png = png_create_info_struct(png);
    if (!info_png) {
        png_destroy_read_struct(&png, nullptr, nullptr);
        std::fclose(fp);
        return false;
    }

    if (setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png, &info_png, nullptr);
        std::fclose(fp);
        return false;
    }

    png_init_io(png, fp);
    png_read_info(png, info_png);

    width     = static_cast<int>(png_get_image_width(png, info_png));
    height    = static_cast<int>(png_get_image_height(png, info_png));
    channels  = png_get_channels(png, info_png);
    bit_depth = png_get_bit_depth(png, info_png);

    png_destroy_read_struct(&png, &info_png, nullptr);
    std::fclose(fp);
    return true;
}

bool load(const std::string &path,
          std::vector<std::uint8_t> &pixel_data,
          int &width, int &height,
          PipelineMode &mode)
{
    std::FILE *fp = openBinary(path);
    if (!fp) return false;

    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING,
                                             nullptr, nullptr, nullptr);
    if (!png) { std::fclose(fp); return false; }

    png_infop info_png = png_create_info_struct(png);
    if (!info_png) {
        png_destroy_read_struct(&png, nullptr, nullptr);
        std::fclose(fp);
        return false;
    }

    if (setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png, &info_png, nullptr);
        std::fclose(fp);
        return false;
    }

    png_init_io(png, fp);
    png_read_info(png, info_png);

    width  = static_cast<int>(png_get_image_width(png, info_png));
    height = static_cast<int>(png_get_image_height(png, info_png));
    const int channels  = png_get_channels(png, info_png);
    const int bit_depth = png_get_bit_depth(png, info_png);

    // Auto-expand to RGBA. These four calls are the load-bearing
    // libpng configuration — they ensure we always see RGBA in the
    // output buffer regardless of the source's color type. Any of
    // them missing would return mismatched-channel data on at
    // least one PNG variant in the wild.
    png_set_expand(png);              // palette/grayscale → 8-bit
    png_set_palette_to_rgb(png);      // palette → RGB
    png_set_tRNS_to_alpha(png);       // tRNS chunk → alpha channel
    png_set_gray_to_rgb(png);         // grayscale → RGB

    // Add opaque alpha when the source has none. RGB (3-ch) and
    // 8-bit single-channel grayscale both get a constant
    // 0xFF/0xFFFF alpha appended.
    if (channels == 3 || (channels == 1 && bit_depth == 8)) {
        png_set_add_alpha(png, (bit_depth == 16) ? 0xFFFF : 0xFF,
                          PNG_FILLER_AFTER);
    }

    // PNG stores 16-bit samples in network byte order (big-endian).
    // On little-endian hosts (everything we ship to) we need the
    // byte swap to read them as host uint16_t. Without this 16-bit
    // PNGs come out as colored noise.
    if (bit_depth == 16) {
#if defined(_WIN32) || (defined(__BYTE_ORDER__) && \
                        __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
        png_set_swap(png);
#endif
    }

    png_read_update_info(png, info_png);

    const std::size_t rowBytes = png_get_rowbytes(png, info_png);
    pixel_data.assign(static_cast<std::size_t>(height) * rowBytes, 0);

    std::vector<png_bytep> rows(height);
    for (int y = 0; y < height; ++y) {
        rows[y] = pixel_data.data() + static_cast<std::size_t>(y) * rowBytes;
    }
    png_read_image(png, rows.data());

    mode = (bit_depth > 8) ? PipelineMode::HIGH_RES : PipelineMode::NORMAL;

    png_destroy_read_struct(&png, &info_png, nullptr);
    std::fclose(fp);
    return true;
}

} // namespace PNGLoader

// ---------------------------------------------------------------------------
// PNGImageLoader (IImageLoader implementation)
// ---------------------------------------------------------------------------

std::shared_ptr<PixelData> PNGImageLoader::loadFrame(
    const std::string &path,
    const std::string & /*layer*/,
    PipelineMode pipeline_mode)
{
    auto pd = std::make_shared<PixelData>();
    PipelineMode detected = pipeline_mode;
    if (!PNGLoader::load(path, pd->pixels, pd->width, pd->height, detected)) {
        return nullptr;
    }
    pd->pipeline_mode = detected;
    pd->setFormat(detected == PipelineMode::NORMAL
                  ? PixelFormat::RGBA8
                  : PixelFormat::RGBA16);
    return pd;
}

std::shared_ptr<PixelData> PNGImageLoader::loadThumbnail(
    const std::string &path, int max_size)
{
    // PNG can't decode partial scanline ranges, so we still read
    // the whole image. The savings come from a tighter destination
    // buffer + skip-downsample (no resampling filter — a simple
    // pixel pick). Matches the old app's approach in
    // image_loaders.cpp:765 line range.
    std::FILE *fp =
#ifdef _WIN32
        std::fopen(path.c_str(), "rb");
#else
        std::fopen(path.c_str(), "rb");
#endif
    if (!fp) return nullptr;

    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING,
                                             nullptr, nullptr, nullptr);
    if (!png) { std::fclose(fp); return nullptr; }

    png_infop info_png = png_create_info_struct(png);
    if (!info_png) {
        png_destroy_read_struct(&png, nullptr, nullptr);
        std::fclose(fp);
        return nullptr;
    }

    if (setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png, &info_png, nullptr);
        std::fclose(fp);
        return nullptr;
    }

    png_init_io(png, fp);
    png_read_info(png, info_png);

    const int full_w = static_cast<int>(png_get_image_width(png, info_png));
    const int full_h = static_cast<int>(png_get_image_height(png, info_png));
    const int bit_depth = png_get_bit_depth(png, info_png);

    const int max_dim     = std::max(full_w, full_h);
    const int skip_factor = std::max(1, max_dim / std::max(1, max_size));
    const int thumb_w     = full_w / skip_factor;
    const int thumb_h     = full_h / skip_factor;

    // Match full-loader transformations exactly, otherwise the
    // thumbnail's color/channel layout can diverge from the full.
    png_set_expand(png);
    png_set_palette_to_rgb(png);
    png_set_tRNS_to_alpha(png);
    png_set_gray_to_rgb(png);

    const int channels = png_get_channels(png, info_png);
    if (channels == 3 || (channels == 1 && bit_depth == 8)) {
        png_set_add_alpha(png, (bit_depth == 16) ? 0xFFFF : 0xFF,
                          PNG_FILLER_AFTER);
    }

    if (bit_depth == 16) {
#if defined(_WIN32) || (defined(__BYTE_ORDER__) && \
                        __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
        png_set_swap(png);
#endif
    }

    png_read_update_info(png, info_png);

    const std::size_t rowBytes = png_get_rowbytes(png, info_png);
    std::vector<std::uint8_t> full(
        static_cast<std::size_t>(full_h) * rowBytes);
    std::vector<png_bytep> rows(full_h);
    for (int y = 0; y < full_h; ++y) {
        rows[y] = full.data() + static_cast<std::size_t>(y) * rowBytes;
    }
    png_read_image(png, rows.data());

    png_destroy_read_struct(&png, &info_png, nullptr);
    std::fclose(fp);

    auto pd = std::make_shared<PixelData>();
    pd->width = thumb_w;
    pd->height = thumb_h;
    pd->pipeline_mode = PipelineMode::NORMAL;
    pd->setFormat(PixelFormat::RGBA8);
    pd->pixels.resize(static_cast<std::size_t>(thumb_w) * thumb_h * 4);

    if (bit_depth == 8) {
        for (int y = 0; y < thumb_h; ++y) {
            const std::uint8_t *src_row =
                full.data() +
                static_cast<std::size_t>(y * skip_factor) * rowBytes;
            std::uint8_t *dst_row =
                pd->pixels.data() +
                static_cast<std::size_t>(y) * thumb_w * 4;
            for (int x = 0; x < thumb_w; ++x) {
                const int sx = x * skip_factor;
                dst_row[x * 4 + 0] = src_row[sx * 4 + 0];
                dst_row[x * 4 + 1] = src_row[sx * 4 + 1];
                dst_row[x * 4 + 2] = src_row[sx * 4 + 2];
                dst_row[x * 4 + 3] = src_row[sx * 4 + 3];
            }
        }
    } else {
        // 16-bit source — right-shift to 8-bit. Lossy but cheap;
        // thumbnails don't need full bit depth.
        for (int y = 0; y < thumb_h; ++y) {
            const std::uint16_t *src_row = reinterpret_cast<const std::uint16_t *>(
                full.data() +
                static_cast<std::size_t>(y * skip_factor) * rowBytes);
            std::uint8_t *dst_row =
                pd->pixels.data() +
                static_cast<std::size_t>(y) * thumb_w * 4;
            for (int x = 0; x < thumb_w; ++x) {
                const int sx = x * skip_factor;
                dst_row[x * 4 + 0] = static_cast<std::uint8_t>(src_row[sx * 4 + 0] >> 8);
                dst_row[x * 4 + 1] = static_cast<std::uint8_t>(src_row[sx * 4 + 1] >> 8);
                dst_row[x * 4 + 2] = static_cast<std::uint8_t>(src_row[sx * 4 + 2] >> 8);
                dst_row[x * 4 + 3] = static_cast<std::uint8_t>(src_row[sx * 4 + 3] >> 8);
            }
        }
    }
    return pd;
}

bool PNGImageLoader::getDimensions(const std::string &path,
                                   int &width, int &height)
{
    int channels = 0, bit_depth = 0;
    return PNGLoader::getInfo(path, width, height, channels, bit_depth);
}

} // namespace qcv
