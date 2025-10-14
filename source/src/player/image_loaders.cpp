#include "image_loaders.h"
#include "video_player.h"  // For PipelineModeToString
#include "direct_exr_cache.h"  // For MemoryMappedIStream
#include "../utils/debug_utils.h"

#include <tiffio.h>
#include <png.h>
#include <jpeglib.h>

// OpenEXR headers for EXRImageLoader
#include <OpenEXR/ImfInputFile.h>
#include <OpenEXR/ImfChannelList.h>
#include <OpenEXR/ImfFrameBuffer.h>
#include <OpenEXR/ImfInputPart.h>
#include <OpenEXR/ImfMultiPartInputFile.h>
#include <Imath/ImathBox.h>
#include <Imath/half.h>

#include <algorithm>
#include <cstring>

namespace ump {

// ============================================================================
// Format Detection
// ============================================================================

ImageFormat DetectImageFormat(const std::string& path) {
    size_t dot_pos = path.find_last_of('.');
    if (dot_pos == std::string::npos) return ImageFormat::UNKNOWN;

    std::string ext = path.substr(dot_pos);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    if (ext == ".tif" || ext == ".tiff") return ImageFormat::TIFF;
    if (ext == ".png") return ImageFormat::PNG;
    if (ext == ".jpg" || ext == ".jpeg") return ImageFormat::JPEG;

    return ImageFormat::UNKNOWN;
}

bool GetImageInfo(const std::string& path, ImageInfo& info) {
    ImageFormat format = DetectImageFormat(path);

    switch (format) {
        case ImageFormat::TIFF:
            return TIFFLoader::GetInfo(path, info);
        case ImageFormat::PNG:
            return PNGLoader::GetInfo(path, info);
        case ImageFormat::JPEG:
            return JPEGLoader::GetInfo(path, info);
        default:
            return false;
    }
}

// ============================================================================
// TIFF Loader (libtiff)
// ============================================================================

namespace TIFFLoader {

bool GetInfo(const std::string& path, ImageInfo& info) {
#ifdef _WIN32
    std::wstring wpath(path.begin(), path.end());
    TIFF* tif = TIFFOpenW(wpath.c_str(), "r");
#else
    TIFF* tif = TIFFOpen(path.c_str(), "r");
#endif

    if (!tif) {
        Debug::Log("TIFFLoader::GetInfo: Failed to open " + path);
        return false;
    }

    uint32_t width, height;
    uint16_t bitDepth = 8, sampleFormat = SAMPLEFORMAT_UINT, samplesPerPixel = 3;

    TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &width);
    TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &height);
    TIFFGetFieldDefaulted(tif, TIFFTAG_BITSPERSAMPLE, &bitDepth);
    TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLEFORMAT, &sampleFormat);
    TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLESPERPIXEL, &samplesPerPixel);

    info.width = width;
    info.height = height;
    info.channels = samplesPerPixel;
    info.bit_depth = bitDepth;
    info.is_float = (sampleFormat == SAMPLEFORMAT_IEEEFP);

    // Auto-detect pipeline mode based on bit depth and format
    if (bitDepth == 8) {
        info.recommended_pipeline = PipelineMode::NORMAL;
    } else if (bitDepth == 16) {
        info.recommended_pipeline = info.is_float ?
            PipelineMode::ULTRA_HIGH_RES : PipelineMode::HIGH_RES;
    } else if (bitDepth == 32 && info.is_float) {
        info.recommended_pipeline = PipelineMode::ULTRA_HIGH_RES;
    } else {
        info.recommended_pipeline = PipelineMode::NORMAL;  // Fallback
    }

    TIFFClose(tif);

    Debug::Log("TIFFLoader::GetInfo: " + path + " - " +
               std::to_string(width) + "x" + std::to_string(height) +
               ", " + std::to_string(bitDepth) + "-bit, " +
               std::to_string(samplesPerPixel) + " channels, " +
               (info.is_float ? "float" : "int") + " -> " +
               PipelineModeToString(info.recommended_pipeline));

    return true;
}

bool Load(const std::string& path, std::vector<uint8_t>& pixel_data,
          int& width, int& height, PipelineMode& mode) {
    Debug::Log("TIFFLoader::Load: Attempting to load " + path);

#ifdef _WIN32
    std::wstring wpath(path.begin(), path.end());
    TIFF* tif = TIFFOpenW(wpath.c_str(), "r");
#else
    TIFF* tif = TIFFOpen(path.c_str(), "r");
#endif

    if (!tif) {
        Debug::Log("TIFFLoader::Load: Failed to open " + path);
        return false;
    }

    Debug::Log("TIFFLoader::Load: Successfully opened TIFF file");

    uint32_t tiffWidth, tiffHeight;
    uint16_t bitDepth = 8, sampleFormat = SAMPLEFORMAT_UINT, samplesPerPixel = 3;

    TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &tiffWidth);
    TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &tiffHeight);
    TIFFGetFieldDefaulted(tif, TIFFTAG_BITSPERSAMPLE, &bitDepth);
    TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLEFORMAT, &sampleFormat);
    TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLESPERPIXEL, &samplesPerPixel);

    uint16_t photometric = 0;
    uint16_t* extraSamples = nullptr;
    uint16_t extraSamplesCount = 0;
    uint16_t planarConfig = PLANARCONFIG_CONTIG;
    TIFFGetFieldDefaulted(tif, TIFFTAG_PHOTOMETRIC, &photometric);
    TIFFGetField(tif, TIFFTAG_EXTRASAMPLES, &extraSamplesCount, &extraSamples);
    TIFFGetFieldDefaulted(tif, TIFFTAG_PLANARCONFIG, &planarConfig);

    std::string extraSampleType = "none";
    if (extraSamplesCount > 0 && extraSamples) {
        switch (extraSamples[0]) {
            case EXTRASAMPLE_UNSPECIFIED: extraSampleType = "UNSPECIFIED"; break;
            case EXTRASAMPLE_ASSOCALPHA: extraSampleType = "ASSOCALPHA"; break;
            case EXTRASAMPLE_UNASSALPHA: extraSampleType = "UNASSALPHA"; break;
            default: extraSampleType = "UNKNOWN(" + std::to_string(extraSamples[0]) + ")"; break;
        }
    }

    Debug::Log("TIFFLoader::Load: TIFF format - " + std::to_string(tiffWidth) + "x" + std::to_string(tiffHeight) +
               ", bitDepth=" + std::to_string(bitDepth) +
               ", samplesPerPixel=" + std::to_string(samplesPerPixel) +
               ", photometric=" + std::to_string(photometric) +
               ", sampleFormat=" + std::to_string(sampleFormat) +
               ", planarConfig=" + std::to_string(planarConfig) +
               ", extraSamples=" + std::to_string(extraSamplesCount) +
               " type=" + extraSampleType);

    width = tiffWidth;
    height = tiffHeight;

    // Set pipeline mode based on bit depth
    if (bitDepth == 8) {
        mode = PipelineMode::NORMAL;
    } else if (bitDepth == 16) {
        mode = (sampleFormat == SAMPLEFORMAT_IEEEFP) ?
            PipelineMode::ULTRA_HIGH_RES : PipelineMode::HIGH_RES;
    } else if (bitDepth == 32 && sampleFormat == SAMPLEFORMAT_IEEEFP) {
        mode = PipelineMode::ULTRA_HIGH_RES;
    } else {
        mode = PipelineMode::NORMAL;  // Fallback
    }

    // Use TIFFReadRGBAImageOriented for automatic format conversion
    // This handles all TIFF formats and converts to ABGR uint32
    if (bitDepth == 8) {
        // 8-bit: Use RGBA conversion
        std::vector<uint32_t> temp_buffer(tiffWidth * tiffHeight);
        if (!TIFFReadRGBAImageOriented(tif, tiffWidth, tiffHeight, temp_buffer.data(), ORIENTATION_TOPLEFT, 0)) {
            Debug::Log("TIFFLoader::Load: Failed to read RGBA image data");
            TIFFClose(tif);
            return false;
        }

        // Convert ABGR (uint32) to RGBA (4x uint8)
        pixel_data.resize(tiffWidth * tiffHeight * 4);
        for (size_t i = 0; i < tiffWidth * tiffHeight; ++i) {
            uint32_t abgr = temp_buffer[i];
            pixel_data[i * 4 + 0] = TIFFGetR(abgr);  // R
            pixel_data[i * 4 + 1] = TIFFGetG(abgr);  // G
            pixel_data[i * 4 + 2] = TIFFGetB(abgr);  // B
            pixel_data[i * 4 + 3] = TIFFGetA(abgr);  // A
        }
    } else {
        // 16-bit: Read scanlines directly and convert to RGBA16
        size_t bytes_per_sample = (bitDepth + 7) / 8;
        size_t scanlineSize = TIFFScanlineSize(tif);
        std::vector<uint8_t> temp_scanline(scanlineSize);
        pixel_data.resize(tiffHeight * tiffWidth * 4 * bytes_per_sample);

        // Check TIFF byte order - TIFFIsByteSwapped() returns true if libtiff is already handling byte swapping
        bool isByteSwapped = TIFFIsByteSwapped(tif) != 0;
        if (bitDepth == 16) {
            Debug::Log("TIFFLoader::Load: TIFFIsByteSwapped=" + std::string(isByteSwapped ? "YES (libtiff auto-swapping)" : "NO"));
        }

        for (uint32_t row = 0; row < tiffHeight; row++) {
            if (TIFFReadScanline(tif, temp_scanline.data(), row) < 0) {
                Debug::Log("TIFFLoader::Load: Failed to read scanline " + std::to_string(row));
                TIFFClose(tif);
                return false;
            }

            // Note: libtiff automatically handles byte swapping when TIFFIsByteSwapped() is true
            // So we do NOT need to manually swap - the data is already in host byte order

            // Convert to RGBA (add alpha channel if needed)
            uint8_t* dest = pixel_data.data() + row * tiffWidth * 4 * bytes_per_sample;
            if (samplesPerPixel == 4) {
                // 4 channels - need to check if it's RGBA or needs reordering
                // TIFF with photometric=RGB and 4 channels is typically RGBA, but may need verification
                if (row == 0) {
                    Debug::Log("TIFFLoader::Load: 16-bit RGBA (4 channels) - checking channel order, scanlineSize=" + std::to_string(scanlineSize));
                }

                // For 16-bit, TIFF might store channels in different order
                // Try swapping to see if it's ABGR instead of RGBA
                if (bitDepth == 16) {
                    uint16_t* src = reinterpret_cast<uint16_t*>(temp_scanline.data());
                    uint16_t* dst = reinterpret_cast<uint16_t*>(dest);

                    if (row == 0) {
                        Debug::Log("TIFFLoader::Load: 16-bit RGBA direct copy (no channel swap)");
                        // Log first pixel values to diagnose channel order
                        if (tiffWidth > 0) {
                            Debug::Log("TIFFLoader::Load: First pixel TIFF values: R=" + std::to_string(src[0]) +
                                       " G=" + std::to_string(src[1]) +
                                       " B=" + std::to_string(src[2]) +
                                       " A=" + std::to_string(src[3]));
                        }
                    }

                    // Direct copy - assume TIFF is already in RGBA order
                    memcpy(dst, src, tiffWidth * 4 * bytes_per_sample);
                } else {
                    memcpy(dest, temp_scanline.data(), tiffWidth * 4 * bytes_per_sample);
                }
            } else if (samplesPerPixel == 3) {
                // RGB → RGBA, add alpha channel
                if (row == 0) {
                    Debug::Log("TIFFLoader::Load: 16-bit RGB (3 channels) - converting to RGBA, scanlineSize=" + std::to_string(scanlineSize));
                }
                if (bitDepth == 16) {
                    uint16_t* src = reinterpret_cast<uint16_t*>(temp_scanline.data());
                    uint16_t* dst = reinterpret_cast<uint16_t*>(dest);
                    for (uint32_t x = 0; x < tiffWidth; ++x) {
                        dst[x * 4 + 0] = src[x * 3 + 0];  // R
                        dst[x * 4 + 1] = src[x * 3 + 1];  // G
                        dst[x * 4 + 2] = src[x * 3 + 2];  // B
                        dst[x * 4 + 3] = 65535;           // A = max
                    }
                } else {
                    uint8_t* src = temp_scanline.data();
                    for (uint32_t x = 0; x < tiffWidth; ++x) {
                        dest[x * 4 + 0] = src[x * 3 + 0];  // R
                        dest[x * 4 + 1] = src[x * 3 + 1];  // G
                        dest[x * 4 + 2] = src[x * 3 + 2];  // B
                        dest[x * 4 + 3] = 255;             // A = max
                    }
                }
            } else {
                if (row == 0) {
                    Debug::Log("TIFFLoader::Load: WARNING - Unexpected samplesPerPixel=" + std::to_string(samplesPerPixel));
                }
            }
        }
    }

    TIFFClose(tif);

    size_t expected_size = tiffWidth * tiffHeight * 4 * ((bitDepth == 8) ? 1 : 2);
    Debug::Log("TIFFLoader::Load: Successfully loaded " + path + " -> " +
               PipelineModeToString(mode) + ", pixel_data size: " + std::to_string(pixel_data.size()) +
               " (expected: " + std::to_string(expected_size) + ")");

    return true;
}

} // namespace TIFFLoader

// ============================================================================
// PNG Loader (libpng)
// ============================================================================

namespace PNGLoader {

bool GetInfo(const std::string& path, ImageInfo& info) {
#ifdef _WIN32
    FILE* fp = _wfopen(std::wstring(path.begin(), path.end()).c_str(), L"rb");
#else
    FILE* fp = fopen(path.c_str(), "rb");
#endif

    if (!fp) {
        Debug::Log("PNGLoader::GetInfo: Failed to open " + path);
        return false;
    }

    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (!png) {
        fclose(fp);
        return false;
    }

    png_infop info_png = png_create_info_struct(png);
    if (!info_png) {
        png_destroy_read_struct(&png, nullptr, nullptr);
        fclose(fp);
        return false;
    }

    if (setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png, &info_png, nullptr);
        fclose(fp);
        return false;
    }

    png_init_io(png, fp);
    png_read_info(png, info_png);

    info.width = png_get_image_width(png, info_png);
    info.height = png_get_image_height(png, info_png);
    info.channels = png_get_channels(png, info_png);
    info.bit_depth = png_get_bit_depth(png, info_png);
    info.is_float = false;  // PNG doesn't support float

    // Auto-detect pipeline mode
    info.recommended_pipeline = (info.bit_depth > 8) ?
        PipelineMode::HIGH_RES : PipelineMode::NORMAL;

    png_destroy_read_struct(&png, &info_png, nullptr);
    fclose(fp);

    Debug::Log("PNGLoader::GetInfo: " + path + " - " +
               std::to_string(info.width) + "x" + std::to_string(info.height) +
               ", " + std::to_string(info.bit_depth) + "-bit, " +
               std::to_string(info.channels) + " channels -> " +
               PipelineModeToString(info.recommended_pipeline));

    return true;
}

bool Load(const std::string& path, std::vector<uint8_t>& pixel_data,
          int& width, int& height, PipelineMode& mode) {
#ifdef _WIN32
    FILE* fp = _wfopen(std::wstring(path.begin(), path.end()).c_str(), L"rb");
#else
    FILE* fp = fopen(path.c_str(), "rb");
#endif

    if (!fp) {
        Debug::Log("PNGLoader::Load: Failed to open " + path);
        return false;
    }

    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (!png) {
        fclose(fp);
        return false;
    }

    png_infop info_png = png_create_info_struct(png);
    if (!info_png) {
        png_destroy_read_struct(&png, nullptr, nullptr);
        fclose(fp);
        return false;
    }

    if (setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png, &info_png, nullptr);
        fclose(fp);
        return false;
    }

    png_init_io(png, fp);
    png_read_info(png, info_png);

    width = png_get_image_width(png, info_png);
    height = png_get_image_height(png, info_png);
    int channels = png_get_channels(png, info_png);
    int bitDepth = png_get_bit_depth(png, info_png);

    // Auto-expand formats and ensure RGBA output
    png_set_expand(png);              // Expand palette/grayscale
    png_set_palette_to_rgb(png);      // Palette → RGB
    png_set_tRNS_to_alpha(png);       // tRNS → alpha channel
    png_set_gray_to_rgb(png);         // Grayscale → RGB

    // Add alpha channel if missing (RGB → RGBA)
    if (channels == 3 || (channels == 1 && bitDepth == 8)) {
        png_set_add_alpha(png, (bitDepth == 16) ? 0xFFFF : 0xFF, PNG_FILLER_AFTER);
    }

    // Endian swap for 16-bit (PNG stores in network byte order = big-endian)
    // On little-endian systems (Windows, x86/x64), we need to swap bytes
    if (bitDepth == 16) {
#if defined(_WIN32) || defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
        png_set_swap(png);  // Swap bytes for little-endian systems
        Debug::Log("PNGLoader::Load: Applied png_set_swap() for little-endian 16-bit PNG");
#else
        Debug::Log("PNGLoader::Load: No byte swap needed for big-endian system");
#endif
    }

    // Update info after transformations
    png_read_update_info(png, info_png);

    // Log final format after transformations
    int final_channels = png_get_channels(png, info_png);
    int final_bit_depth = png_get_bit_depth(png, info_png);
    int color_type = png_get_color_type(png, info_png);
    Debug::Log("PNGLoader::Load: After transformations - channels=" + std::to_string(final_channels) +
               ", bit_depth=" + std::to_string(final_bit_depth) +
               ", color_type=" + std::to_string(color_type));

    // Allocate buffer for RGBA output
    size_t rowBytes = png_get_rowbytes(png, info_png);
    pixel_data.resize(height * rowBytes);

    // Read scanlines
    std::vector<png_bytep> rows(height);
    for (int y = 0; y < height; y++) {
        rows[y] = pixel_data.data() + y * rowBytes;
    }
    png_read_image(png, rows.data());

    // Log first non-zero pixel for channel order verification
    if (bitDepth == 16 && pixel_data.size() >= 8) {
        const uint16_t* pixels16 = reinterpret_cast<const uint16_t*>(pixel_data.data());
        bool found_nonzero = false;
        for (size_t i = 0; i < std::min(pixel_data.size() / 8, size_t(100)); i++) {
            if (pixels16[i*4] != 0 || pixels16[i*4+1] != 0 || pixels16[i*4+2] != 0) {
                Debug::Log("PNGLoader::Load: First non-zero pixel at offset " + std::to_string(i) +
                           ": R=" + std::to_string(pixels16[i*4]) +
                           " G=" + std::to_string(pixels16[i*4+1]) +
                           " B=" + std::to_string(pixels16[i*4+2]) +
                           " A=" + std::to_string(pixels16[i*4+3]));
                found_nonzero = true;
                break;
            }
        }
        if (!found_nonzero) {
            Debug::Log("PNGLoader::Load: First 100 pixels are all zeros");
        }
    }

    // Set pipeline mode
    mode = (bitDepth > 8) ? PipelineMode::HIGH_RES : PipelineMode::NORMAL;

    png_destroy_read_struct(&png, &info_png, nullptr);
    fclose(fp);

    size_t expected_size = width * height * 4 * ((bitDepth > 8) ? 2 : 1);
    Debug::Log("PNGLoader::Load: Successfully loaded " + path + " -> " +
               PipelineModeToString(mode) + ", pixel_data size: " + std::to_string(pixel_data.size()) +
               " (expected: " + std::to_string(expected_size) + ")");

    return true;
}

} // namespace PNGLoader

// ============================================================================
// JPEG Loader (libjpeg-turbo)
// ============================================================================

namespace JPEGLoader {

bool GetInfo(const std::string& path, ImageInfo& info) {
#ifdef _WIN32
    FILE* fp = _wfopen(std::wstring(path.begin(), path.end()).c_str(), L"rb");
#else
    FILE* fp = fopen(path.c_str(), "rb");
#endif

    if (!fp) {
        Debug::Log("JPEGLoader::GetInfo: Failed to open " + path);
        return false;
    }

    struct jpeg_decompress_struct cinfo;
    struct jpeg_error_mgr jerr;

    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_decompress(&cinfo);
    jpeg_stdio_src(&cinfo, fp);

    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        jpeg_destroy_decompress(&cinfo);
        fclose(fp);
        return false;
    }

    info.width = cinfo.image_width;
    info.height = cinfo.image_height;
    info.channels = cinfo.num_components;
    info.bit_depth = 8;  // JPEG is always 8-bit
    info.is_float = false;
    info.recommended_pipeline = PipelineMode::NORMAL;

    jpeg_destroy_decompress(&cinfo);
    fclose(fp);

    Debug::Log("JPEGLoader::GetInfo: " + path + " - " +
               std::to_string(info.width) + "x" + std::to_string(info.height) +
               ", 8-bit, " + std::to_string(info.channels) + " channels -> NORMAL");

    return true;
}

bool Load(const std::string& path, std::vector<uint8_t>& pixel_data,
          int& width, int& height, PipelineMode& mode) {
#ifdef _WIN32
    FILE* fp = _wfopen(std::wstring(path.begin(), path.end()).c_str(), L"rb");
#else
    FILE* fp = fopen(path.c_str(), "rb");
#endif

    if (!fp) {
        Debug::Log("JPEGLoader::Load: Failed to open " + path);
        return false;
    }

    struct jpeg_decompress_struct cinfo;
    struct jpeg_error_mgr jerr;

    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_decompress(&cinfo);
    jpeg_stdio_src(&cinfo, fp);

    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        jpeg_destroy_decompress(&cinfo);
        fclose(fp);
        return false;
    }

    jpeg_start_decompress(&cinfo);

    width = cinfo.output_width;
    height = cinfo.output_height;
    int channels = cinfo.output_components;

    Debug::Log("JPEGLoader::Load: Image dimensions " + std::to_string(width) + "x" + std::to_string(height) +
               ", channels=" + std::to_string(channels));

    // Read JPEG scanlines into temporary RGB buffer
    size_t rowStride = width * channels;
    std::vector<uint8_t> temp_rgb(height * rowStride);

    while (cinfo.output_scanline < cinfo.output_height) {
        uint8_t* rowPtr = temp_rgb.data() + cinfo.output_scanline * rowStride;
        jpeg_read_scanlines(&cinfo, &rowPtr, 1);
    }

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    fclose(fp);

    // Convert RGB to RGBA (add alpha channel for OpenGL compatibility)
    pixel_data.resize(width * height * 4);
    if (channels == 3) {
        // RGB → RGBA
        for (int i = 0; i < width * height; ++i) {
            pixel_data[i * 4 + 0] = temp_rgb[i * 3 + 0];  // R
            pixel_data[i * 4 + 1] = temp_rgb[i * 3 + 1];  // G
            pixel_data[i * 4 + 2] = temp_rgb[i * 3 + 2];  // B
            pixel_data[i * 4 + 3] = 255;                  // A = opaque
        }
        Debug::Log("JPEGLoader::Load: Converted RGB to RGBA (" + std::to_string(pixel_data.size()) + " bytes)");
    } else if (channels == 1) {
        // Grayscale → RGBA
        for (int i = 0; i < width * height; ++i) {
            uint8_t gray = temp_rgb[i];
            pixel_data[i * 4 + 0] = gray;  // R
            pixel_data[i * 4 + 1] = gray;  // G
            pixel_data[i * 4 + 2] = gray;  // B
            pixel_data[i * 4 + 3] = 255;   // A = opaque
        }
        Debug::Log("JPEGLoader::Load: Converted grayscale to RGBA (" + std::to_string(pixel_data.size()) + " bytes)");
    } else {
        Debug::Log("JPEGLoader::Load: WARNING - Unexpected channel count: " + std::to_string(channels));
        return false;
    }

    mode = PipelineMode::NORMAL;  // JPEG is always 8-bit

    Debug::Log("JPEGLoader::Load: Successfully loaded " + path + " -> NORMAL, RGBA output");

    return true;
}

} // namespace JPEGLoader

//=============================================================================
// IImageLoader Wrapper Implementations
//=============================================================================

// TIFF Image Loader (wraps TIFFLoader namespace)
std::shared_ptr<PixelData> TIFFImageLoader::LoadFrame(
    const std::string& path,
    const std::string& layer,  // Ignored
    PipelineMode pipeline_mode) {

    auto result = std::make_shared<PixelData>();

    // Use existing TIFFLoader::Load
    PipelineMode detected_mode = pipeline_mode;
    if (!TIFFLoader::Load(path, result->pixels, result->width, result->height, detected_mode)) {
        return nullptr;
    }

    result->gl_format = GL_RGBA;
    result->pipeline_mode = detected_mode;

    // Set GL type based on detected pipeline mode
    if (detected_mode == PipelineMode::NORMAL) {
        result->gl_type = GL_UNSIGNED_BYTE;  // RGBA8
    } else if (detected_mode == PipelineMode::HIGH_RES) {
        result->gl_type = GL_UNSIGNED_SHORT;  // RGBA16
    } else {
        result->gl_type = GL_HALF_FLOAT;  // RGBA16F
    }

    return result;
}

std::shared_ptr<PixelData> TIFFImageLoader::LoadThumbnail(const std::string& path, int max_size) {
    // FAST thumbnail loading for TIFF - skip every Nth scanline for speed
    // Trade quality for performance

#ifdef _WIN32
    std::wstring wpath(path.begin(), path.end());
    TIFF* tif = TIFFOpenW(wpath.c_str(), "r");
#else
    TIFF* tif = TIFFOpen(path.c_str(), "r");
#endif

    if (!tif) {
        Debug::Log("TIFFImageLoader::LoadThumbnail: Failed to open " + path);
        return nullptr;
    }

    uint32_t full_width, full_height;
    uint16_t bitDepth = 8, samplesPerPixel = 3;

    TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &full_width);
    TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &full_height);
    TIFFGetFieldDefaulted(tif, TIFFTAG_BITSPERSAMPLE, &bitDepth);
    TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLESPERPIXEL, &samplesPerPixel);

    // Calculate downsample factor - read every Nth row/column
    int max_dim = (std::max)(full_width, full_height);
    int skip_factor = (std::max)(1, max_dim / max_size);  // How many rows/cols to skip

    int thumb_width = full_width / skip_factor;
    int thumb_height = full_height / skip_factor;

    Debug::Log("TIFFImageLoader::LoadThumbnail: " + path +
               " (" + std::to_string(full_width) + "x" + std::to_string(full_height) + " → " +
               std::to_string(thumb_width) + "x" + std::to_string(thumb_height) +
               ", skip=" + std::to_string(skip_factor) + ", " + std::to_string(bitDepth) + "-bit)");

    auto result = std::make_shared<PixelData>();
    result->width = thumb_width;
    result->height = thumb_height;
    result->gl_format = GL_RGBA;
    result->gl_type = GL_UNSIGNED_BYTE;
    result->pipeline_mode = PipelineMode::NORMAL;

    // Use TIFFReadRGBAImageOriented for ALL bit depths (8-bit and 16-bit)
    // This ensures consistent channel ordering via TIFFGetR/G/B/A macros
    // which handle all TIFF photometric interpretations and channel arrangements

    std::vector<uint32_t> full_buffer(full_width * full_height);
    if (!TIFFReadRGBAImageOriented(tif, full_width, full_height, full_buffer.data(), ORIENTATION_TOPLEFT, 0)) {
        Debug::Log("TIFFImageLoader::LoadThumbnail: Failed to read RGBA image");
        TIFFClose(tif);
        return nullptr;
    }

    // Downsample by skipping pixels and convert to 8-bit RGBA
    result->pixels.resize(thumb_width * thumb_height * 4);
    for (int y = 0; y < thumb_height; y++) {
        for (int x = 0; x < thumb_width; x++) {
            uint32_t abgr = full_buffer[(y * skip_factor) * full_width + (x * skip_factor)];
            // TIFFGetR/G/B/A macros correctly extract channels regardless of TIFF format
            result->pixels[(y * thumb_width + x) * 4 + 0] = TIFFGetR(abgr);  // R
            result->pixels[(y * thumb_width + x) * 4 + 1] = TIFFGetG(abgr);  // G
            result->pixels[(y * thumb_width + x) * 4 + 2] = TIFFGetB(abgr);  // B
            result->pixels[(y * thumb_width + x) * 4 + 3] = TIFFGetA(abgr);  // A
        }
    }

    TIFFClose(tif);

    Debug::Log("TIFFImageLoader::LoadThumbnail: Success - " +
               std::to_string(thumb_width) + "x" + std::to_string(thumb_height));

    return result;
}

bool TIFFImageLoader::GetDimensions(const std::string& path, int& width, int& height) {
    ImageInfo info;
    if (TIFFLoader::GetInfo(path, info)) {
        width = info.width;
        height = info.height;
        return true;
    }
    return false;
}

// PNG Image Loader (wraps PNGLoader namespace)
std::shared_ptr<PixelData> PNGImageLoader::LoadFrame(
    const std::string& path,
    const std::string& layer,  // Ignored
    PipelineMode pipeline_mode) {

    auto result = std::make_shared<PixelData>();

    // Use existing PNGLoader::Load
    PipelineMode detected_mode = pipeline_mode;
    if (!PNGLoader::Load(path, result->pixels, result->width, result->height, detected_mode)) {
        return nullptr;
    }

    result->gl_format = GL_RGBA;
    result->pipeline_mode = detected_mode;

    // Set GL type based on detected pipeline mode
    if (detected_mode == PipelineMode::NORMAL) {
        result->gl_type = GL_UNSIGNED_BYTE;  // RGBA8
    } else {
        result->gl_type = GL_UNSIGNED_SHORT;  // RGBA16 (PNG can be 16-bit)
    }

    return result;
}

std::shared_ptr<PixelData> PNGImageLoader::LoadThumbnail(const std::string& path, int max_size) {
    // FAST thumbnail loading for PNG - read every Nth row and skip expensive transforms

#ifdef _WIN32
    FILE* fp = _wfopen(std::wstring(path.begin(), path.end()).c_str(), L"rb");
#else
    FILE* fp = fopen(path.c_str(), "rb");
#endif

    if (!fp) {
        Debug::Log("PNGImageLoader::LoadThumbnail: Failed to open " + path);
        return nullptr;
    }

    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (!png) {
        fclose(fp);
        return nullptr;
    }

    png_infop info_png = png_create_info_struct(png);
    if (!info_png) {
        png_destroy_read_struct(&png, nullptr, nullptr);
        fclose(fp);
        return nullptr;
    }

    if (setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png, &info_png, nullptr);
        fclose(fp);
        return nullptr;
    }

    png_init_io(png, fp);
    png_read_info(png, info_png);

    int full_width = png_get_image_width(png, info_png);
    int full_height = png_get_image_height(png, info_png);
    int bitDepth = png_get_bit_depth(png, info_png);

    // Calculate skip factor for downsampling
    int max_dim = (std::max)(full_width, full_height);
    int skip_factor = (std::max)(1, max_dim / max_size);

    int thumb_width = full_width / skip_factor;
    int thumb_height = full_height / skip_factor;

    Debug::Log("PNGImageLoader::LoadThumbnail: " + path +
               " (" + std::to_string(full_width) + "x" + std::to_string(full_height) + " → " +
               std::to_string(thumb_width) + "x" + std::to_string(thumb_height) +
               ", skip=" + std::to_string(skip_factor) + ", " + std::to_string(bitDepth) + "-bit)");

    // Match the full-size loader transformations EXACTLY
    png_set_expand(png);              // Expand palette/grayscale
    png_set_palette_to_rgb(png);      // Palette → RGB
    png_set_tRNS_to_alpha(png);       // tRNS → alpha channel
    png_set_gray_to_rgb(png);         // Grayscale → RGB

    // Add alpha channel if missing (RGB → RGBA)
    int channels = png_get_channels(png, info_png);
    if (channels == 3 || (channels == 1 && bitDepth == 8)) {
        png_set_add_alpha(png, (bitDepth == 16) ? 0xFFFF : 0xFF, PNG_FILLER_AFTER);
    }

    // Endian swap for 16-bit (PNG stores in network byte order = big-endian)
    if (bitDepth == 16) {
#if defined(_WIN32) || defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
        png_set_swap(png);  // Swap bytes for little-endian systems
#endif
    }

    png_read_update_info(png, info_png);

    // Read full image (we have to - PNG doesn't support partial decode)
    size_t rowBytes = png_get_rowbytes(png, info_png);
    std::vector<uint8_t> full_data(full_height * rowBytes);
    std::vector<png_bytep> rows(full_height);
    for (int y = 0; y < full_height; y++) {
        rows[y] = full_data.data() + y * rowBytes;
    }
    png_read_image(png, rows.data());

    png_destroy_read_struct(&png, &info_png, nullptr);
    fclose(fp);

    // Downsample to thumbnail size
    auto result = std::make_shared<PixelData>();
    result->width = thumb_width;
    result->height = thumb_height;
    result->pixels.resize(thumb_width * thumb_height * 4);
    result->gl_format = GL_RGBA;
    result->gl_type = GL_UNSIGNED_BYTE;
    result->pipeline_mode = PipelineMode::NORMAL;

    if (bitDepth == 8) {
        // 8-bit PNG - direct copy with skip
        for (int y = 0; y < thumb_height; y++) {
            uint8_t* src_row = full_data.data() + (y * skip_factor) * rowBytes;
            for (int x = 0; x < thumb_width; x++) {
                int src_x = x * skip_factor;
                result->pixels[(y * thumb_width + x) * 4 + 0] = src_row[src_x * 4 + 0];  // R
                result->pixels[(y * thumb_width + x) * 4 + 1] = src_row[src_x * 4 + 1];  // G
                result->pixels[(y * thumb_width + x) * 4 + 2] = src_row[src_x * 4 + 2];  // B
                result->pixels[(y * thumb_width + x) * 4 + 3] = src_row[src_x * 4 + 3];  // A
            }
        }
    } else {
        // 16-bit PNG - downsample and convert to 8-bit
        for (int y = 0; y < thumb_height; y++) {
            uint16_t* src_row = reinterpret_cast<uint16_t*>(full_data.data() + (y * skip_factor) * rowBytes);
            for (int x = 0; x < thumb_width; x++) {
                int src_x = x * skip_factor;
                result->pixels[(y * thumb_width + x) * 4 + 0] = src_row[src_x * 4 + 0] >> 8;  // R
                result->pixels[(y * thumb_width + x) * 4 + 1] = src_row[src_x * 4 + 1] >> 8;  // G
                result->pixels[(y * thumb_width + x) * 4 + 2] = src_row[src_x * 4 + 2] >> 8;  // B
                result->pixels[(y * thumb_width + x) * 4 + 3] = src_row[src_x * 4 + 3] >> 8;  // A
            }
        }
    }

    Debug::Log("PNGImageLoader::LoadThumbnail: Success - " +
               std::to_string(thumb_width) + "x" + std::to_string(thumb_height));

    return result;
}

bool PNGImageLoader::GetDimensions(const std::string& path, int& width, int& height) {
    ImageInfo info;
    if (PNGLoader::GetInfo(path, info)) {
        width = info.width;
        height = info.height;
        return true;
    }
    return false;
}

// JPEG Image Loader (wraps JPEGLoader namespace)
std::shared_ptr<PixelData> JPEGImageLoader::LoadFrame(
    const std::string& path,
    const std::string& layer,  // Ignored
    PipelineMode pipeline_mode) {

    auto result = std::make_shared<PixelData>();

    // Use existing JPEGLoader::Load
    PipelineMode detected_mode = pipeline_mode;
    if (!JPEGLoader::Load(path, result->pixels, result->width, result->height, detected_mode)) {
        return nullptr;
    }

    result->gl_format = GL_RGBA;
    result->gl_type = GL_UNSIGNED_BYTE;  // JPEG is always 8-bit
    result->pipeline_mode = PipelineMode::NORMAL;  // JPEG is always NORMAL mode

    return result;
}

std::shared_ptr<PixelData> JPEGImageLoader::LoadThumbnail(const std::string& path, int max_size) {
    // Optimized JPEG thumbnail loading using libjpeg's built-in DCT scaling
    // This is MUCH faster than loading full-res and downsampling with stb_image_resize

#ifdef _WIN32
    FILE* fp = _wfopen(std::wstring(path.begin(), path.end()).c_str(), L"rb");
#else
    FILE* fp = fopen(path.c_str(), "rb");
#endif

    if (!fp) {
        Debug::Log("JPEGImageLoader::LoadThumbnail: Failed to open " + path);
        return nullptr;
    }

    struct jpeg_decompress_struct cinfo;
    struct jpeg_error_mgr jerr;

    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_decompress(&cinfo);
    jpeg_stdio_src(&cinfo, fp);

    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        jpeg_destroy_decompress(&cinfo);
        fclose(fp);
        return nullptr;
    }

    // Calculate optimal scale_num/scale_denom for thumbnail
    // libjpeg supports 1/1, 1/2, 1/4, 1/8 scaling via DCT
    int full_width = cinfo.image_width;
    int full_height = cinfo.image_height;
    int max_dim = (std::max)(full_width, full_height);

    // Choose scale factor (1, 2, 4, or 8)
    int scale_denom = 1;
    if (max_dim / 8 >= max_size) {
        scale_denom = 8;
    } else if (max_dim / 4 >= max_size) {
        scale_denom = 4;
    } else if (max_dim / 2 >= max_size) {
        scale_denom = 2;
    }

    cinfo.scale_num = 1;
    cinfo.scale_denom = scale_denom;

    // Start decompression with scaled output
    jpeg_start_decompress(&cinfo);

    int width = cinfo.output_width;
    int height = cinfo.output_height;
    int channels = cinfo.output_components;

    Debug::Log("JPEGImageLoader::LoadThumbnail: Loading " + path +
               " at 1/" + std::to_string(scale_denom) + " scale (" +
               std::to_string(full_width) + "x" + std::to_string(full_height) + " → " +
               std::to_string(width) + "x" + std::to_string(height) + ")");

    // Read scaled JPEG scanlines
    size_t rowStride = width * channels;
    std::vector<uint8_t> temp_rgb(height * rowStride);

    while (cinfo.output_scanline < cinfo.output_height) {
        uint8_t* rowPtr = temp_rgb.data() + cinfo.output_scanline * rowStride;
        jpeg_read_scanlines(&cinfo, &rowPtr, 1);
    }

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    fclose(fp);

    // Convert to RGBA
    auto result = std::make_shared<PixelData>();
    result->width = width;
    result->height = height;
    result->pixels.resize(width * height * 4);
    result->gl_format = GL_RGBA;
    result->gl_type = GL_UNSIGNED_BYTE;
    result->pipeline_mode = PipelineMode::NORMAL;

    if (channels == 3) {
        // RGB → RGBA
        for (int i = 0; i < width * height; ++i) {
            result->pixels[i * 4 + 0] = temp_rgb[i * 3 + 0];  // R
            result->pixels[i * 4 + 1] = temp_rgb[i * 3 + 1];  // G
            result->pixels[i * 4 + 2] = temp_rgb[i * 3 + 2];  // B
            result->pixels[i * 4 + 3] = 255;                  // A
        }
    } else if (channels == 1) {
        // Grayscale → RGBA
        for (int i = 0; i < width * height; ++i) {
            uint8_t gray = temp_rgb[i];
            result->pixels[i * 4 + 0] = gray;
            result->pixels[i * 4 + 1] = gray;
            result->pixels[i * 4 + 2] = gray;
            result->pixels[i * 4 + 3] = 255;
        }
    }

    Debug::Log("JPEGImageLoader::LoadThumbnail: Successfully loaded thumbnail " +
               std::to_string(width) + "x" + std::to_string(height));

    return result;
}

bool JPEGImageLoader::GetDimensions(const std::string& path, int& width, int& height) {
    ImageInfo info;
    if (JPEGLoader::GetInfo(path, info)) {
        width = info.width;
        height = info.height;
        return true;
    }
    return false;
}

// EXR Image Loader (wraps DirectEXRCache EXR loading for universal pipeline)
std::shared_ptr<PixelData> EXRImageLoader::LoadFrame(
    const std::string& path,
    const std::string& layer,
    PipelineMode pipeline_mode) {

    // DirectEXRCache::LoadEXRPixels is private, so we inline the EXR loading here
    // This uses the same memory-mapped, optimized loading path

    try {
        auto stream = std::make_unique<MemoryMappedIStream>(path);
        Imf::MultiPartInputFile file(*stream);

        const Imf::Header& header = file.header(0);
        const Imath::Box2i displayWindow = header.displayWindow();
        const Imath::Box2i dataWindow = header.dataWindow();

        const bool fastPath = (displayWindow == dataWindow);

        int width = displayWindow.max.x - displayWindow.min.x + 1;
        int height = displayWindow.max.y - displayWindow.min.y + 1;

        // Allocate pixel buffer
        auto result = std::make_shared<PixelData>();
        result->width = width;
        result->height = height;
        result->gl_format = GL_RGBA;
        result->gl_type = GL_HALF_FLOAT;
        result->pipeline_mode = PipelineMode::HDR_RES;  // EXR is always HDR

        const size_t pixelCount = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
        result->pixels.resize(pixelCount * sizeof(Imath::half));
        Imath::half* pixel_half = reinterpret_cast<Imath::half*>(result->pixels.data());

        Imf::FrameBuffer frameBuffer;
        const Imf::ChannelList& channels = header.channels();

        // Find RGBA channels with layer prefix
        std::string layerPrefix = layer.empty() ? "" : (layer + ".");
        std::string channelR = layerPrefix + "R";
        std::string channelG = layerPrefix + "G";
        std::string channelB = layerPrefix + "B";
        std::string channelA = layerPrefix + "A";

        const Imf::Channel* chR = channels.findChannel(channelR.c_str());
        const Imf::Channel* chG = channels.findChannel(channelG.c_str());
        const Imf::Channel* chB = channels.findChannel(channelB.c_str());
        const Imf::Channel* chA = channels.findChannel(channelA.c_str());

        // Fallback to root-level channels
        if (!chR && !layer.empty()) {
            channelR = "R";
            channelG = "G";
            channelB = "B";
            channelA = "A";
            chR = channels.findChannel("R");
            chG = channels.findChannel("G");
            chB = channels.findChannel("B");
            chA = channels.findChannel("A");
        }

        if (!chR || !chG || !chB) {
            Debug::Log("EXRImageLoader::LoadFrame: Missing RGB channels for layer '" + layer + "'");
            return nullptr;
        }

        bool hasAlpha = (chA != nullptr);
        std::string fullChannelNames[4] = { channelR, channelG, channelB, channelA };
        int numChannels = hasAlpha ? 4 : 3;

        Imf::InputPart part(file, 0);

        if (fastPath) {
            // FAST PATH: Direct read
            const size_t channelByteCount = sizeof(Imath::half);
            const size_t cb = 4 * channelByteCount;
            const size_t scb = width * 4 * channelByteCount;

            for (int c = 0; c < numChannels; ++c) {
                frameBuffer.insert(
                    fullChannelNames[c].c_str(),
                    Imf::Slice(
                        Imf::HALF,
                        (char*)(pixel_half) + (c * channelByteCount),
                        cb, scb, 1, 1, 0.0f
                    )
                );
            }

            if (!hasAlpha) {
                for (int i = 0; i < width * height; ++i) {
                    pixel_half[i * 4 + 3] = 1.0f;
                }
            }

            part.setFrameBuffer(frameBuffer);
            part.readPixels(displayWindow.min.y, displayWindow.max.y);
        } else {
            // SLOW PATH: Handle mismatched windows
            const Imath::Box2i intersectedWindow = Imath::Box2i(
                Imath::V2i((std::max)(displayWindow.min.x, dataWindow.min.x),
                          (std::max)(displayWindow.min.y, dataWindow.min.y)),
                Imath::V2i((std::min)(displayWindow.max.x, dataWindow.max.x),
                          (std::min)(displayWindow.max.y, dataWindow.max.y))
            );

            const int dataWidth = dataWindow.max.x - dataWindow.min.x + 1;
            const size_t channelByteCount = sizeof(Imath::half);
            const size_t cb = 4 * channelByteCount;
            const size_t bufSize = dataWidth * cb;
            std::vector<char> buf(bufSize);

            for (int c = 0; c < numChannels; ++c) {
                frameBuffer.insert(
                    fullChannelNames[c].c_str(),
                    Imf::Slice(
                        Imf::HALF,
                        buf.data() - (dataWindow.min.x * cb) + (c * channelByteCount),
                        cb, 0, 1, 1, 0.0f
                    )
                );
            }

            part.setFrameBuffer(frameBuffer);

            const size_t scb = width * 4 * channelByteCount;
            for (int y = displayWindow.min.y; y <= displayWindow.max.y; ++y) {
                uint8_t* p = result->pixels.data() + ((y - displayWindow.min.y) * scb);
                uint8_t* end = p + scb;

                if (y >= intersectedWindow.min.y && y <= intersectedWindow.max.y) {
                    size_t size = (intersectedWindow.min.x - displayWindow.min.x) * cb;
                    std::memset(p, 0, size);
                    p += size;

                    size = (intersectedWindow.max.x - intersectedWindow.min.x + 1) * cb;
                    part.readPixels(y, y);
                    std::memcpy(p, buf.data() + (std::max)(displayWindow.min.x - dataWindow.min.x, 0) * cb, size);
                    p += size;
                }

                std::memset(p, 0, end - p);
            }

            if (!hasAlpha) {
                for (int i = 0; i < width * height; ++i) {
                    pixel_half[i * 4 + 3] = 1.0f;
                }
            }
        }

        return result;

    } catch (const std::exception& e) {
        Debug::Log("EXRImageLoader::LoadFrame: Exception - " + std::string(e.what()));
        return nullptr;
    }
}

std::shared_ptr<PixelData> EXRImageLoader::LoadThumbnail(const std::string& path, int max_size) {
    // FAST EXR thumbnail loading with compression-aware strategy:
    // - DWAB (256-line blocks): Load full image and downsample (same decompression cost)
    // - Other compressions: Read every Nth scanline (efficient for small blocks)

    try {
        auto stream = std::make_unique<MemoryMappedIStream>(path);
        Imf::MultiPartInputFile file(*stream);

        const Imf::Header& header = file.header(0);
        const Imath::Box2i displayWindow = header.displayWindow();
        const Imath::Box2i dataWindow = header.dataWindow();

        // Check compression type
        Imf::Compression compression = header.compression();
        bool is_tiled = header.hasTileDescription();

        int full_width = displayWindow.max.x - displayWindow.min.x + 1;
        int full_height = displayWindow.max.y - displayWindow.min.y + 1;

        // Calculate skip factor for downsampling
        int max_dim = (std::max)(full_width, full_height);
        int skip_factor = (std::max)(1, max_dim / max_size);

        int thumb_width = full_width / skip_factor;
        int thumb_height = full_height / skip_factor;

        // Compression name for logging
        const char* compression_name = "UNKNOWN";
        switch (compression) {
            case Imf::NO_COMPRESSION: compression_name = "NONE"; break;
            case Imf::RLE_COMPRESSION: compression_name = "RLE"; break;
            case Imf::ZIPS_COMPRESSION: compression_name = "ZIPS"; break;
            case Imf::ZIP_COMPRESSION: compression_name = "ZIP"; break;
            case Imf::PIZ_COMPRESSION: compression_name = "PIZ"; break;
            case Imf::PXR24_COMPRESSION: compression_name = "PXR24"; break;
            case Imf::B44_COMPRESSION: compression_name = "B44"; break;
            case Imf::B44A_COMPRESSION: compression_name = "B44A"; break;
            case Imf::DWAA_COMPRESSION: compression_name = "DWAA"; break;
            case Imf::DWAB_COMPRESSION: compression_name = "DWAB"; break;
            default: break;
        }

        Debug::Log("EXRImageLoader::LoadThumbnail: " + path +
                   " (" + std::to_string(full_width) + "x" + std::to_string(full_height) + " → " +
                   std::to_string(thumb_width) + "x" + std::to_string(thumb_height) +
                   ", skip=" + std::to_string(skip_factor) +
                   ", " + std::string(is_tiled ? "TILED" : "SCANLINE") +
                   ", compression=" + compression_name + ")");

        // NOTE: DWAB uses 256-scanline blocks, so scanline-skipping is less efficient
        // (decompresses full blocks but only uses some scanlines). However, loading full
        // images for DWAB causes heavy disk I/O contention with transcoded playback.
        // Scanline-skip is slower per-thumbnail but avoids competing with main playback.

        // Use scanline-skip strategy for all compressions
        auto result = std::make_shared<PixelData>();
        result->width = thumb_width;
        result->height = thumb_height;
        result->gl_format = GL_RGBA;
        result->gl_type = GL_HALF_FLOAT;  // Keep as half-float for HDR thumbnails
        result->pipeline_mode = PipelineMode::HDR_RES;
        result->pixels.resize(thumb_width * thumb_height * 4 * sizeof(Imath::half));

        Imath::half* thumb_pixels = reinterpret_cast<Imath::half*>(result->pixels.data());

        // Find RGBA channels with layer support
        const Imf::ChannelList& channels = header.channels();

        std::string layerPrefix = layer_name_.empty() ? "" : (layer_name_ + ".");
        std::string channelR = layerPrefix + "R";
        std::string channelG = layerPrefix + "G";
        std::string channelB = layerPrefix + "B";
        std::string channelA = layerPrefix + "A";

        const Imf::Channel* chR = channels.findChannel(channelR.c_str());
        const Imf::Channel* chG = channels.findChannel(channelG.c_str());
        const Imf::Channel* chB = channels.findChannel(channelB.c_str());
        const Imf::Channel* chA = channels.findChannel(channelA.c_str());

        // Fallback to root-level channels if layer not found
        if (!chR && !layer_name_.empty()) {
            Debug::Log("EXRImageLoader::LoadThumbnail: Layer '" + layer_name_ + "' not found, trying root channels");
            channelR = "R";
            channelG = "G";
            channelB = "B";
            channelA = "A";
            chR = channels.findChannel("R");
            chG = channels.findChannel("G");
            chB = channels.findChannel("B");
            chA = channels.findChannel("A");
        }

        if (!chR || !chG || !chB) {
            Debug::Log("EXRImageLoader::LoadThumbnail: Missing RGB channels for layer '" + layer_name_ + "'");
            return nullptr;
        }

        bool hasAlpha = (chA != nullptr);

        Imf::InputPart part(file, 0);

        // NOTE: Tiled EXRs could be optimized further by reading only needed tiles
        // For now, we use scanline-based reading which works for both formats
        // (OpenEXR automatically converts tiles to scanlines when needed)
        // TODO: Add tile-based optimization for tiled EXRs if performance is insufficient

        // Allocate scanline buffer for reading
        std::vector<Imath::half> scanline_buffer(full_width * 4);
        Imf::FrameBuffer frameBuffer;

        const size_t channelByteCount = sizeof(Imath::half);
        const size_t cb = 4 * channelByteCount;

        // Setup framebuffer for reading one scanline at a time
        frameBuffer.insert(channelR.c_str(), Imf::Slice(Imf::HALF, (char*)(scanline_buffer.data()) + 0 * channelByteCount, cb, 0, 1, 1, 0.0f));
        frameBuffer.insert(channelG.c_str(), Imf::Slice(Imf::HALF, (char*)(scanline_buffer.data()) + 1 * channelByteCount, cb, 0, 1, 1, 0.0f));
        frameBuffer.insert(channelB.c_str(), Imf::Slice(Imf::HALF, (char*)(scanline_buffer.data()) + 2 * channelByteCount, cb, 0, 1, 1, 0.0f));
        if (hasAlpha) {
            frameBuffer.insert(channelA.c_str(), Imf::Slice(Imf::HALF, (char*)(scanline_buffer.data()) + 3 * channelByteCount, cb, 0, 1, 1, 0.0f));
        }

        part.setFrameBuffer(frameBuffer);

        // Read every Nth scanline and downsample horizontally
        for (int thumb_y = 0; thumb_y < thumb_height; thumb_y++) {
            int source_y = displayWindow.min.y + (thumb_y * skip_factor);
            if (source_y > displayWindow.max.y) break;

            // Read one scanline
            part.readPixels(source_y, source_y);

            // Downsample horizontally - pick every Nth pixel
            for (int thumb_x = 0; thumb_x < thumb_width; thumb_x++) {
                int source_x = thumb_x * skip_factor;
                if (source_x >= full_width) break;

                int src_idx = source_x * 4;
                int dst_idx = (thumb_y * thumb_width + thumb_x) * 4;

                thumb_pixels[dst_idx + 0] = scanline_buffer[src_idx + 0];  // R
                thumb_pixels[dst_idx + 1] = scanline_buffer[src_idx + 1];  // G
                thumb_pixels[dst_idx + 2] = scanline_buffer[src_idx + 2];  // B
                thumb_pixels[dst_idx + 3] = hasAlpha ? scanline_buffer[src_idx + 3] : Imath::half(1.0f);  // A
            }
        }

        Debug::Log("EXRImageLoader::LoadThumbnail: Success (half-float HDR) - " +
                   std::to_string(thumb_width) + "x" + std::to_string(thumb_height));

        return result;

    } catch (const std::exception& e) {
        Debug::Log("EXRImageLoader::LoadThumbnail: Exception - " + std::string(e.what()));
        return nullptr;
    }
}

bool EXRImageLoader::GetDimensions(const std::string& path, int& width, int& height) {
    // Use DirectEXRCache's static method to get dimensions without full load
    return ump::DirectEXRCache::GetFrameDimensions(path, width, height);
}

//=============================================================================
// Video Image Loader (FFmpeg-based frame extraction for thumbnails)
//=============================================================================

// FFmpeg headers
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

VideoImageLoader::VideoImageLoader(const std::string& video_path, double fps, double duration)
    : video_path_(video_path)
    , fps_(fps)
    , duration_(duration)
    , width_(0)
    , height_(0) {

    Debug::Log("VideoImageLoader: Initializing for " + video_path_ +
               " (fps=" + std::to_string(fps_) + ", duration=" + std::to_string(duration_) + "s)");

    initialized_ = InitializeFFmpeg();

    if (!initialized_) {
        Debug::Log("VideoImageLoader: Failed to initialize FFmpeg for " + video_path_);
    } else {
        Debug::Log("VideoImageLoader: Successfully initialized " + std::to_string(width_) + "x" +
                   std::to_string(height_) + ", " + std::to_string(GetFrameCount()) + " frames");
    }
}

VideoImageLoader::~VideoImageLoader() {
    CleanupFFmpeg();
}

bool VideoImageLoader::InitializeFFmpeg() {
    std::lock_guard<std::mutex> lock(ffmpeg_mutex_);

    // Open video file
    format_context_ = avformat_alloc_context();
    if (avformat_open_input(&format_context_, video_path_.c_str(), nullptr, nullptr) < 0) {
        Debug::Log("VideoImageLoader: Failed to open video file: " + video_path_);
        return false;
    }

    if (avformat_find_stream_info(format_context_, nullptr) < 0) {
        Debug::Log("VideoImageLoader: Failed to find stream info");
        avformat_close_input(&format_context_);
        return false;
    }

    // Find video stream
    video_stream_index_ = av_find_best_stream(format_context_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (video_stream_index_ < 0) {
        Debug::Log("VideoImageLoader: No video stream found");
        avformat_close_input(&format_context_);
        return false;
    }

    AVStream* video_stream = format_context_->streams[video_stream_index_];

    // Get video dimensions
    width_ = video_stream->codecpar->width;
    height_ = video_stream->codecpar->height;

    // Setup decoder (software only for thread safety)
    const AVCodec* decoder = avcodec_find_decoder(video_stream->codecpar->codec_id);
    if (!decoder) {
        Debug::Log("VideoImageLoader: No suitable decoder found");
        avformat_close_input(&format_context_);
        return false;
    }

    codec_context_ = avcodec_alloc_context3(decoder);
    if (avcodec_parameters_to_context(codec_context_, video_stream->codecpar) < 0) {
        Debug::Log("VideoImageLoader: Failed to copy codec parameters");
        avformat_close_input(&format_context_);
        return false;
    }

    // Configure for single-threaded extraction (thread safety)
    codec_context_->thread_count = 1;
    codec_context_->thread_type = FF_THREAD_SLICE;

    if (avcodec_open2(codec_context_, decoder, nullptr) < 0) {
        Debug::Log("VideoImageLoader: Failed to open codec");
        avcodec_free_context(&codec_context_);
        avformat_close_input(&format_context_);
        return false;
    }

    return true;
}

void VideoImageLoader::CleanupFFmpeg() {
    std::lock_guard<std::mutex> lock(ffmpeg_mutex_);

    if (codec_context_) {
        avcodec_free_context(&codec_context_);
    }
    if (format_context_) {
        avformat_close_input(&format_context_);
    }

    initialized_ = false;
}

std::shared_ptr<PixelData> VideoImageLoader::LoadFrame(
    const std::string& path,
    const std::string& layer,
    PipelineMode pipeline_mode) {

    // Parse frame number from path (path is "0", "1", "2", etc.)
    int frame_number = 0;
    try {
        frame_number = std::stoi(path);
    } catch (...) {
        Debug::Log("VideoImageLoader::LoadFrame: Invalid frame number: " + path);
        return nullptr;
    }

    return ExtractFrame(frame_number, pipeline_mode, 0);
}

std::shared_ptr<PixelData> VideoImageLoader::LoadThumbnail(const std::string& path, int max_size) {
    // Parse frame number from path
    int frame_number = 0;
    try {
        frame_number = std::stoi(path);
    } catch (...) {
        Debug::Log("VideoImageLoader::LoadThumbnail: Invalid frame number: " + path);
        return nullptr;
    }

    Debug::Log("VideoImageLoader::LoadThumbnail: Extracting frame " + std::to_string(frame_number) +
               " at max_size=" + std::to_string(max_size));

    // Extract at thumbnail resolution
    return ExtractFrame(frame_number, PipelineMode::NORMAL, max_size);
}

std::shared_ptr<PixelData> VideoImageLoader::ExtractFrame(int frame_number, PipelineMode pipeline_mode, int max_size) {
    if (!initialized_) {
        Debug::Log("VideoImageLoader::ExtractFrame: Not initialized");
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(ffmpeg_mutex_);

    // Calculate timestamp
    double timestamp = frame_number / fps_;

    // Allocate frame for decoding
    AVFrame* frame = av_frame_alloc();
    if (!frame) {
        Debug::Log("VideoImageLoader::ExtractFrame: Failed to allocate frame");
        return nullptr;
    }

    // Seek and decode
    bool success = SeekAndDecodeFrame(timestamp, frame);
    if (!success) {
        Debug::Log("VideoImageLoader::ExtractFrame: Seek/decode failed for frame " + std::to_string(frame_number) +
                   " (timestamp=" + std::to_string(timestamp) + "s)");
        av_frame_free(&frame);
        return nullptr;
    }

    // Convert to pixel buffer
    auto result = std::make_shared<PixelData>();
    result->gl_format = GL_RGBA;
    result->gl_type = GL_UNSIGNED_BYTE;  // Thumbnails are always 8-bit for now
    result->pipeline_mode = PipelineMode::NORMAL;

    if (!ConvertFrameToPixels(frame, result->pixels, result->width, result->height, pipeline_mode, max_size)) {
        Debug::Log("VideoImageLoader::ExtractFrame: Failed to convert frame to pixels");
        av_frame_free(&frame);
        return nullptr;
    }

    Debug::Log("VideoImageLoader::ExtractFrame: Successfully extracted frame " + std::to_string(frame_number) +
               " -> " + std::to_string(result->width) + "x" + std::to_string(result->height) +
               ", " + std::to_string(result->pixels.size()) + " bytes");

    av_frame_free(&frame);
    return result;
}

bool VideoImageLoader::SeekAndDecodeFrame(double timestamp, AVFrame* output_frame) {
    AVStream* stream = format_context_->streams[video_stream_index_];

    // Convert timestamp to stream timebase
    int64_t target_pts = av_rescale_q(timestamp * AV_TIME_BASE, AV_TIME_BASE_Q, stream->time_base);

    // Seek to target timestamp
    if (av_seek_frame(format_context_, video_stream_index_, target_pts, AVSEEK_FLAG_BACKWARD) < 0) {
        Debug::Log("VideoImageLoader: Seek failed for timestamp " + std::to_string(timestamp));
        return false;
    }

    avcodec_flush_buffers(codec_context_);

    AVPacket* packet = av_packet_alloc();
    if (!packet) {
        return false;
    }

    bool found_frame = false;

    // Read packets until we find our target frame
    while (av_read_frame(format_context_, packet) >= 0) {
        if (packet->stream_index == video_stream_index_) {
            if (avcodec_send_packet(codec_context_, packet) >= 0) {
                while (avcodec_receive_frame(codec_context_, output_frame) >= 0) {
                    // Check if this is close to our target timestamp
                    double frame_timestamp = output_frame->pts * av_q2d(stream->time_base);
                    if (std::abs(frame_timestamp - timestamp) < (1.0 / fps_)) {
                        found_frame = true;
                        break;
                    }
                    av_frame_unref(output_frame);
                }
            }
            if (found_frame) break;
        }
        av_packet_unref(packet);
    }

    av_packet_free(&packet);
    return found_frame;
}

bool VideoImageLoader::ConvertFrameToPixels(AVFrame* frame, std::vector<uint8_t>& pixels,
                                            int& width, int& height, PipelineMode pipeline_mode, int max_size) {
    if (!frame || frame->width <= 0 || frame->height <= 0) {
        return false;
    }

    // Calculate output dimensions
    if (max_size > 0) {
        // Scale to fit within max_size
        int max_dim = (std::max)(frame->width, frame->height);
        if (max_dim > max_size) {
            float scale = static_cast<float>(max_size) / max_dim;
            width = static_cast<int>(frame->width * scale);
            height = static_cast<int>(frame->height * scale);
        } else {
            width = frame->width;
            height = frame->height;
        }
    } else {
        width = frame->width;
        height = frame->height;
    }

    // Target format (RGBA8 for thumbnails)
    AVPixelFormat target_format = AV_PIX_FMT_RGBA;
    size_t bytes_per_pixel = 4;

    // Allocate target frame
    AVFrame* target_frame = av_frame_alloc();
    target_frame->format = target_format;
    target_frame->width = width;
    target_frame->height = height;

    if (av_frame_get_buffer(target_frame, 32) < 0) {
        av_frame_free(&target_frame);
        return false;
    }

    // Setup software scaler
    SwsContext* sws_ctx = sws_getContext(
        frame->width, frame->height, (AVPixelFormat)frame->format,
        width, height, target_format,
        SWS_BILINEAR,  // Bilinear for decent quality
        nullptr, nullptr, nullptr);

    if (!sws_ctx) {
        av_frame_free(&target_frame);
        return false;
    }

    // Convert
    sws_scale(sws_ctx, frame->data, frame->linesize, 0, frame->height,
              target_frame->data, target_frame->linesize);

    // Copy to output buffer
    size_t data_size = width * height * bytes_per_pixel;
    pixels.resize(data_size);
    std::memcpy(pixels.data(), target_frame->data[0], data_size);

    sws_freeContext(sws_ctx);
    av_frame_free(&target_frame);

    return true;
}

bool VideoImageLoader::GetDimensions(const std::string& path, int& width, int& height) {
    // For videos, path is a frame number but dimensions are constant
    width = width_;
    height = height_;
    return initialized_;
}

} // namespace ump
