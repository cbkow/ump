#include "video_metadata.h"
#include "../utils/debug_utils.h"
#include <algorithm>
#include <regex>

// Detect bit depth from pixel format string
int VideoMetadata::DetectBitDepth() const {
    if (pixel_format.empty()) return 8;

    // Look for bit depth indicators in pixel format
    // Examples: yuv420p10le, yuva444p12le, rgb48le, etc.
    std::regex bit_depth_regex(R"((\d+)(?:le|be)?)");
    std::smatch matches;

    if (std::regex_search(pixel_format, matches, bit_depth_regex)) {
        int detected = std::stoi(matches[1].str());
        // Common video bit depths
        if (detected == 8 || detected == 10 || detected == 12 || detected == 16) {
            return detected;
        }
        // Handle special cases like rgb48 (48/3 = 16 bits per channel)
        if (detected == 24) return 8;   // rgb24 = 8 bits per channel
        if (detected == 48) return 16;  // rgb48 = 16 bits per channel
    }

    // Fallback: check for known high bit depth formats
    std::string format_lower = pixel_format;
    std::transform(format_lower.begin(), format_lower.end(), format_lower.begin(), ::tolower);

    if (format_lower.find("p10") != std::string::npos) return 10;
    if (format_lower.find("p12") != std::string::npos) return 12;
    if (format_lower.find("p16") != std::string::npos) return 16;

    return 8; // Default to 8-bit
}

// Detect alpha channel from pixel format
bool VideoMetadata::DetectHasAlpha() const {
    if (pixel_format.empty()) return false;

    std::string format_lower = pixel_format;
    std::transform(format_lower.begin(), format_lower.end(), format_lower.begin(), ::tolower);

    // Look for alpha indicators
    return (format_lower.find("yuva") != std::string::npos ||
            format_lower.find("rgba") != std::string::npos ||
            format_lower.find("argb") != std::string::npos ||
            format_lower.find("bgra") != std::string::npos);
}

// Detect HDR content based on multiple factors
bool VideoMetadata::DetectHDRContent() const {
    // HDR indicators:
    // 1. High bit depth (10+ bits) combined with HDR color spaces/transfers
    // 2. HDR transfer functions (PQ, HLG)
    // 3. Wide color gamut (BT.2020)

    bool has_high_bit_depth = (bit_depth >= 10);

    // Check transfer curve for HDR
    bool has_hdr_transfer = false;
    if (!color_transfer.empty()) {
        std::string transfer_lower = color_transfer;
        std::transform(transfer_lower.begin(), transfer_lower.end(), transfer_lower.begin(), ::tolower);

        has_hdr_transfer = (transfer_lower.find("pq") != std::string::npos ||
                           transfer_lower.find("smpte2084") != std::string::npos ||
                           transfer_lower.find("hlg") != std::string::npos ||
                           transfer_lower.find("arib-std-b67") != std::string::npos);
    }

    // Check color space for wide gamut
    bool has_wide_gamut = false;
    if (!colorspace.empty()) {
        std::string colorspace_lower = colorspace;
        std::transform(colorspace_lower.begin(), colorspace_lower.end(), colorspace_lower.begin(), ::tolower);

        has_wide_gamut = (colorspace_lower.find("bt2020") != std::string::npos ||
                          colorspace_lower.find("2020") != std::string::npos);
    }

    // HDR if we have HDR transfer curve OR (high bit depth + wide gamut)
    return has_hdr_transfer || (has_high_bit_depth && has_wide_gamut);
}

// Detect range type from various metadata sources
std::string VideoMetadata::DetectRangeType() const {
    // This would typically be populated during metadata extraction
    // from MPV or FFmpeg properties, but we can make educated guesses

    if (!range_type.empty() && range_type != "unknown") {
        return range_type;
    }

    // Most video content uses limited range
    // Most computer graphics use full range
    if (!pixel_format.empty()) {
        std::string format_lower = pixel_format;
        std::transform(format_lower.begin(), format_lower.end(), format_lower.begin(), ::tolower);

        // JPEG formats typically use full range
        if (format_lower.find("yuvj") != std::string::npos) {
            return "full";
        }

        // RGB formats typically use full range
        if (format_lower.find("rgb") != std::string::npos) {
            return "full";
        }
    }

    // Default assumption for YUV video content
    return "limited";
}

// NEW: Detect 4444 formats for conditional color matrix application
bool VideoMetadata::Is4444Format() const {
    if (pixel_format.empty()) {
        Debug::Log("Is4444Format: pixel_format is empty");
        return false;
    }

    std::string format_lower = pixel_format;
    std::transform(format_lower.begin(), format_lower.end(), format_lower.begin(), ::tolower);

    //Debug::Log("Is4444Format: Checking pixel format '" + pixel_format + "' (lowercase: '" + format_lower + "')");

    // Detect 4444 formats specifically - these are the formats that benefit from color matrix correction
    bool is_4444 = (format_lower.find("4444") != std::string::npos ||        // yuv444p, yuva444p variants
                    format_lower.find("prores4444") != std::string::npos ||  // ProRes 4444/4444XQ
                    format_lower.find("rgba") != std::string::npos ||        // RGBA formats
                    format_lower.find("argb") != std::string::npos ||        // ARGB formats
                    format_lower.find("bgra") != std::string::npos ||        // BGRA formats
                    format_lower.find("abgr") != std::string::npos);         // ABGR formats

    // Additional check for high-quality YUV 444 variants (with and without alpha)
    if (!is_4444 && format_lower.find("444") != std::string::npos) {
        // Only apply to true 444 formats, not 442 or 440
        is_4444 = (format_lower.find("yuv444") != std::string::npos ||   // yuv444p, yuv444p10le, etc.
                   format_lower.find("yuva444") != std::string::npos);   // yuva444p, yuva444p12le, etc.
    }

    //Debug::Log("Is4444Format: Result = " + std::string(is_4444 ? "TRUE (4444 format)" : "FALSE (standard format)"));
    return is_4444;
}

// NEW: Detect 422/420 formats for appropriate color matrix handling
bool VideoMetadata::Is422Or420Format() const {
    if (pixel_format.empty()) {
        Debug::Log("Is422Or420Format: pixel_format is empty");
        return false;
    }

    std::string format_lower = pixel_format;
    std::transform(format_lower.begin(), format_lower.end(), format_lower.begin(), ::tolower);

    Debug::Log("Is422Or420Format: Checking pixel format '" + pixel_format + "' (lowercase: '" + format_lower + "')");

    // Detect 422/420 formats specifically - these might benefit from range/light color matrix
    bool is_422_420 = (format_lower.find("420") != std::string::npos ||    // yuv420p, yuv420p10le, etc.
                       format_lower.find("422") != std::string::npos ||    // yuv422p, yuv422p10le, etc.
                       format_lower.find("nv12") != std::string::npos ||   // NV12 (420)
                       format_lower.find("nv21") != std::string::npos ||   // NV21 (420)
                       format_lower.find("yv12") != std::string::npos);    // YV12 (420)

    Debug::Log("Is422Or420Format: Result = " + std::string(is_422_420 ? "TRUE (422/420 format)" : "FALSE (not 422/420)"));
    return is_422_420;
}

// NEW: Detect 4:1:1 formats
bool VideoMetadata::Is411Format() const {
    if (pixel_format.empty()) {
        Debug::Log("Is411Format: pixel_format is empty");
        return false;
    }

    std::string format_lower = pixel_format;
    std::transform(format_lower.begin(), format_lower.end(), format_lower.begin(), ::tolower);

    Debug::Log("Is411Format: Checking pixel format '" + pixel_format + "' (lowercase: '" + format_lower + "')");

    // Detect 4:1:1 formats - horizontal chroma subsampling by 4
    // Common in DV NTSC (yuv411p)
    bool is_411 = (format_lower.find("411") != std::string::npos ||
                   format_lower.find("yuv411p") != std::string::npos);

    Debug::Log("Is411Format: Result = " + std::string(is_411 ? "TRUE (4:1:1 format)" : "FALSE (not 4:1:1)"));
    return is_411;
}

// NEW: Detect 4:2:1 formats (rare)
bool VideoMetadata::Is421Format() const {
    if (pixel_format.empty()) {
        Debug::Log("Is421Format: pixel_format is empty");
        return false;
    }

    std::string format_lower = pixel_format;
    std::transform(format_lower.begin(), format_lower.end(), format_lower.begin(), ::tolower);

    Debug::Log("Is421Format: Checking pixel format '" + pixel_format + "' (lowercase: '" + format_lower + "')");

    // Detect 4:2:1 formats - rare, but exists in some DVCPro variants
    // Less common than 411 but worth detecting
    bool is_421 = (format_lower.find("421") != std::string::npos);

    Debug::Log("Is421Format: Result = " + std::string(is_421 ? "TRUE (4:2:1 format)" : "FALSE (not 4:2:1)"));
    return is_421;
}

// NEW: Detect and cache NCLC (nclx) color tag triplet
void VideoMetadata::DetectAndCacheNCLC() {
    // Reset values
    nclc_primaries = 0;
    nclc_transfer = 0;
    nclc_matrix = 0;
    nclc_tag = "";

    // Map color_primaries to NCLC ColorPrimaries index
    std::string primaries_lower = color_primaries;
    std::transform(primaries_lower.begin(), primaries_lower.end(), primaries_lower.begin(), ::tolower);

    if (primaries_lower.find("bt709") != std::string::npos ||
        primaries_lower.find("bt.709") != std::string::npos ||
        primaries_lower.find("rec709") != std::string::npos ||
        primaries_lower.find("rec.709") != std::string::npos) {
        nclc_primaries = 1;  // BT.709
    } else if (primaries_lower.find("bt470m") != std::string::npos) {
        nclc_primaries = 4;  // BT.470M
    } else if (primaries_lower.find("bt470bg") != std::string::npos) {
        nclc_primaries = 5;  // BT.470BG
    } else if (primaries_lower.find("smpte170m") != std::string::npos ||
               primaries_lower.find("smpte 170m") != std::string::npos) {
        nclc_primaries = 6;  // SMPTE 170M (NTSC)
    } else if (primaries_lower.find("smpte240m") != std::string::npos) {
        nclc_primaries = 7;  // SMPTE 240M
    } else if (primaries_lower.find("film") != std::string::npos) {
        nclc_primaries = 8;  // Generic film
    } else if (primaries_lower.find("bt2020") != std::string::npos ||
               primaries_lower.find("bt.2020") != std::string::npos) {
        nclc_primaries = 9;  // BT.2020
    } else if (primaries_lower.find("p3") != std::string::npos ||
               primaries_lower.find("dci-p3") != std::string::npos ||
               primaries_lower.find("display-p3") != std::string::npos) {
        nclc_primaries = 12; // P3 D65 (Display P3) - commonly used
    }

    // Map color_transfer to NCLC TransferCharacteristics index
    std::string transfer_lower = color_transfer;
    std::transform(transfer_lower.begin(), transfer_lower.end(), transfer_lower.begin(), ::tolower);

    if (transfer_lower.find("bt709") != std::string::npos ||
        transfer_lower.find("bt.709") != std::string::npos ||
        transfer_lower.find("bt1886") != std::string::npos ||
        transfer_lower.find("bt.1886") != std::string::npos) {
        nclc_transfer = 1;  // BT.709 / BT.1886 (both use same NCLC code)
    } else if (transfer_lower.find("unknown") != std::string::npos ||
               transfer_lower.find("unspecified") != std::string::npos) {
        nclc_transfer = 2;  // Unspecified
    } else if (transfer_lower.find("gamma22") != std::string::npos) {
        nclc_transfer = 4;  // Gamma 2.2
    } else if (transfer_lower.find("gamma28") != std::string::npos) {
        nclc_transfer = 5;  // Gamma 2.8
    } else if (transfer_lower.find("smpte170m") != std::string::npos) {
        nclc_transfer = 6;  // SMPTE 170M
    } else if (transfer_lower.find("smpte240m") != std::string::npos) {
        nclc_transfer = 7;  // SMPTE 240M
    } else if (transfer_lower.find("linear") != std::string::npos) {
        nclc_transfer = 8;  // Linear
    } else if (transfer_lower.find("log100") != std::string::npos) {
        nclc_transfer = 9;  // Log 100:1
    } else if (transfer_lower.find("log316") != std::string::npos) {
        nclc_transfer = 10; // Log 316.22:1
    } else if (transfer_lower.find("iec61966") != std::string::npos ||
               transfer_lower.find("srgb") != std::string::npos) {
        nclc_transfer = 13; // sRGB (IEC 61966-2-1)
    } else if (transfer_lower.find("bt2020") != std::string::npos &&
               transfer_lower.find("10") != std::string::npos) {
        nclc_transfer = 14; // BT.2020 10-bit
    } else if (transfer_lower.find("bt2020") != std::string::npos &&
               transfer_lower.find("12") != std::string::npos) {
        nclc_transfer = 15; // BT.2020 12-bit
    } else if (transfer_lower.find("pq") != std::string::npos ||
               transfer_lower.find("smpte2084") != std::string::npos ||
               transfer_lower.find("smpte 2084") != std::string::npos) {
        nclc_transfer = 16; // SMPTE ST 2084 (PQ)
    } else if (transfer_lower.find("hlg") != std::string::npos ||
               transfer_lower.find("arib-std-b67") != std::string::npos) {
        nclc_transfer = 18; // ARIB STD-B67 (HLG)
    }

    // Map colorspace (matrix coefficients) to NCLC MatrixCoefficients index
    std::string matrix_lower = colorspace;
    std::transform(matrix_lower.begin(), matrix_lower.end(), matrix_lower.begin(), ::tolower);

    if (matrix_lower.find("bt709") != std::string::npos ||
        matrix_lower.find("bt.709") != std::string::npos) {
        nclc_matrix = 1;  // BT.709
    } else if (matrix_lower.find("unknown") != std::string::npos ||
               matrix_lower.find("unspecified") != std::string::npos) {
        nclc_matrix = 2;  // Unspecified
    } else if (matrix_lower.find("fcc") != std::string::npos) {
        nclc_matrix = 4;  // FCC
    } else if (matrix_lower.find("bt470bg") != std::string::npos) {
        nclc_matrix = 5;  // BT.470BG
    } else if (matrix_lower.find("smpte170m") != std::string::npos ||
               matrix_lower.find("bt601") != std::string::npos) {
        nclc_matrix = 6;  // SMPTE 170M / BT.601
    } else if (matrix_lower.find("smpte240m") != std::string::npos) {
        nclc_matrix = 7;  // SMPTE 240M
    } else if (matrix_lower.find("ycgco") != std::string::npos) {
        nclc_matrix = 8;  // YCgCo
    } else if (matrix_lower.find("bt2020") != std::string::npos &&
               matrix_lower.find("ncl") != std::string::npos) {
        nclc_matrix = 9;  // BT.2020 non-constant luminance
    } else if (matrix_lower.find("bt2020") != std::string::npos &&
               matrix_lower.find("cl") != std::string::npos) {
        nclc_matrix = 10; // BT.2020 constant luminance
    } else if (matrix_lower.find("rgb") != std::string::npos ||
               matrix_lower.find("identity") != std::string::npos ||
               matrix_lower.find("gbr") != std::string::npos) {
        nclc_matrix = 0;  // Identity (RGB)
    }

    // Construct the NCLC tag string
    if (nclc_primaries > 0 || nclc_transfer > 0 || nclc_matrix >= 0) {
        nclc_tag = std::to_string(nclc_primaries) + "-" +
                   std::to_string(nclc_transfer) + "-" +
                   std::to_string(nclc_matrix);

        Debug::Log("DetectAndCacheNCLC: Detected NCLC tag: " + nclc_tag +
                   " (P:" + color_primaries + ", T:" + color_transfer + ", M:" + colorspace + ")");
    } else {
        nclc_tag = "Unknown";
        Debug::Log("DetectAndCacheNCLC: Could not determine NCLC tag from metadata");
    }
}