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

    Debug::Log("Is4444Format: Checking pixel format '" + pixel_format + "' (lowercase: '" + format_lower + "')");

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

    Debug::Log("Is4444Format: Result = " + std::string(is_4444 ? "TRUE (4444 format)" : "FALSE (standard format)"));
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