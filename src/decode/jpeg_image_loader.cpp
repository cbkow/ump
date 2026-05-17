#include "jpeg_image_loader.h"

#include <cstdio>
#include <jpeglib.h>
#include <vector>

namespace qcv {

namespace JPEGLoader {

namespace {

// Custom error manager — libjpeg's default jumps via setjmp on
// fatal errors, but its emit_message handler writes to stderr.
// We swallow message output here; the caller sees the error via
// the false return.
struct JpegErrorMgr {
    struct jpeg_error_mgr base;
};

void emitNoop(j_common_ptr /*cinfo*/, int /*msg_level*/) {}
void outputNoop(j_common_ptr /*cinfo*/) {}

} // namespace

bool getInfo(const std::string &path, int &width, int &height,
             int &channels)
{
    std::FILE *fp = std::fopen(path.c_str(), "rb");
    if (!fp) return false;

    JpegErrorMgr err{};
    err.base = *jpeg_std_error(&err.base);
    err.base.emit_message    = emitNoop;
    err.base.output_message  = outputNoop;

    jpeg_decompress_struct cinfo;
    cinfo.err = &err.base;
    jpeg_create_decompress(&cinfo);
    jpeg_stdio_src(&cinfo, fp);

    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        jpeg_destroy_decompress(&cinfo);
        std::fclose(fp);
        return false;
    }

    width    = static_cast<int>(cinfo.image_width);
    height   = static_cast<int>(cinfo.image_height);
    channels = cinfo.num_components;

    jpeg_destroy_decompress(&cinfo);
    std::fclose(fp);
    return true;
}

bool load(const std::string &path,
          std::vector<std::uint8_t> &pixel_data,
          int &width, int &height,
          PipelineMode &mode)
{
    std::FILE *fp = std::fopen(path.c_str(), "rb");
    if (!fp) return false;

    JpegErrorMgr err{};
    err.base = *jpeg_std_error(&err.base);
    err.base.emit_message   = emitNoop;
    err.base.output_message = outputNoop;

    jpeg_decompress_struct cinfo;
    cinfo.err = &err.base;
    jpeg_create_decompress(&cinfo);
    jpeg_stdio_src(&cinfo, fp);

    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        jpeg_destroy_decompress(&cinfo);
        std::fclose(fp);
        return false;
    }
    jpeg_start_decompress(&cinfo);

    width  = static_cast<int>(cinfo.output_width);
    height = static_cast<int>(cinfo.output_height);
    const int channels = cinfo.output_components;
    const std::size_t rowStride =
        static_cast<std::size_t>(width) * channels;

    std::vector<std::uint8_t> rgb(rowStride * height);
    while (cinfo.output_scanline < cinfo.output_height) {
        std::uint8_t *rowPtr =
            rgb.data() + static_cast<std::size_t>(cinfo.output_scanline) * rowStride;
        jpeg_read_scanlines(&cinfo, &rowPtr, 1);
    }
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    std::fclose(fp);

    pixel_data.assign(static_cast<std::size_t>(width) * height * 4, 0);

    if (channels == 3) {
        // RGB → RGBA. libjpeg-turbo emits 8-bit per channel, so a
        // simple per-pixel rewrite synthesizes the alpha.
        for (int i = 0; i < width * height; ++i) {
            pixel_data[i * 4 + 0] = rgb[i * 3 + 0];
            pixel_data[i * 4 + 1] = rgb[i * 3 + 1];
            pixel_data[i * 4 + 2] = rgb[i * 3 + 2];
            pixel_data[i * 4 + 3] = 255;
        }
    } else if (channels == 1) {
        // Grayscale → RGBA monochrome.
        for (int i = 0; i < width * height; ++i) {
            const std::uint8_t g = rgb[i];
            pixel_data[i * 4 + 0] = g;
            pixel_data[i * 4 + 1] = g;
            pixel_data[i * 4 + 2] = g;
            pixel_data[i * 4 + 3] = 255;
        }
    } else {
        // Old app accepts RGB and gray only; CMYK / Lab JPEGs are
        // rare enough that we bail rather than guess at colorspace.
        return false;
    }

    mode = PipelineMode::NORMAL; // JPEG is always 8-bit
    return true;
}

} // namespace JPEGLoader

// ---------------------------------------------------------------------------
// JPEGImageLoader (IImageLoader implementation)
// ---------------------------------------------------------------------------

std::shared_ptr<PixelData> JPEGImageLoader::loadFrame(
    const std::string &path,
    const std::string & /*layer*/,
    PipelineMode pipeline_mode)
{
    auto pd = std::make_shared<PixelData>();
    PipelineMode detected = pipeline_mode;
    if (!JPEGLoader::load(path, pd->pixels, pd->width, pd->height, detected)) {
        return nullptr;
    }
    pd->pipeline_mode = detected;
    pd->setFormat(PixelFormat::RGBA8);
    return pd;
}

bool JPEGImageLoader::getDimensions(const std::string &path,
                                    int &width, int &height)
{
    int channels = 0;
    return JPEGLoader::getInfo(path, width, height, channels);
}

} // namespace qcv
