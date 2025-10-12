#include "media_background_extractor.h"
#include "../metadata/video_metadata.h"
#include <algorithm>

extern "C" {
#include <libswscale/swscale.h>
}

// ============================================================================
// ConversionStrategy Implementation - Conditional 4444 Color Matrix Support
// ============================================================================

ConversionStrategy ConversionStrategy::FromMetadata(const VideoMetadata& metadata) {
    ConversionStrategy strategy;

    // PERFORMANCE FIX: Lazy detection - only compute expensive properties when needed
    if (!metadata.cache_properties_detected) {
        // Use const_cast to cache results (this is the only place detection happens now)
        VideoMetadata& mutable_metadata = const_cast<VideoMetadata&>(metadata);
        mutable_metadata.bit_depth = metadata.DetectBitDepth();
        mutable_metadata.has_alpha = metadata.DetectHasAlpha();
        mutable_metadata.is_hdr_content = metadata.DetectHDRContent();
        mutable_metadata.range_type = metadata.DetectRangeType();
        mutable_metadata.cache_properties_detected = true;
    }

    // Store source bit depth for quality decisions
    strategy.source_bit_depth = metadata.bit_depth;

    // Colorspace mapping from MPV metadata to FFmpeg constants
    Debug::Log("ConversionStrategy: Input colorspace='" + metadata.colorspace + "', range='" + metadata.range_type + "'");

    if (metadata.colorspace == "bt709" || metadata.colorspace == "bt.709") {
        strategy.source_colorspace = 1;  // BT.709
        Debug::Log("ConversionStrategy: Matched BT.709 colorspace");
    } else if (metadata.colorspace == "bt601" || metadata.colorspace == "smpte170m") {
        strategy.source_colorspace = 5;  // BT.601
        Debug::Log("ConversionStrategy: Matched BT.601 colorspace");
    } else if (metadata.colorspace == "bt2020nc" || metadata.colorspace == "bt2020-ncl") {
        strategy.source_colorspace = 9;  // BT.2020
        Debug::Log("ConversionStrategy: Matched BT.2020 colorspace");
    } else {
        // Default based on resolution (HD+ likely BT.709, SD likely BT.601)
        strategy.source_colorspace = (metadata.width >= 1280 || metadata.height >= 720) ? 1 : 5;
        Debug::Log("ConversionStrategy: Unknown colorspace '" + metadata.colorspace + "', defaulting to " +
                  std::string(strategy.source_colorspace == 1 ? "BT.709" : "BT.601") +
                  " based on resolution " + std::to_string(metadata.width) + "x" + std::to_string(metadata.height));
    }

    // Range detection (0=limited 16-235, 1=full 0-255)
    strategy.source_range = (metadata.range_type == "full") ? 1 : 0;

    // Quality algorithm selection based on bit depth
    strategy.sws_algorithm = (metadata.bit_depth > 8) ? SWS_LANCZOS : SWS_FAST_BILINEAR;

    // HDR tone mapping decision (for future implementation)
    strategy.needs_tone_mapping = metadata.is_hdr_content;

    // NEW: Format-specific matrix mode selection
    if (metadata.Is4444Format()) {
        strategy.matrix_mode = ColorMatrixMode::FULL_MATRIX;
        Debug::Log("ConversionStrategy: 4444 format detected - using FULL_MATRIX mode");
    } else if (metadata.Is422Or420Format()) {
        strategy.matrix_mode = ColorMatrixMode::RANGE_ONLY;
        Debug::Log("ConversionStrategy: 422/420 format detected - using RANGE_ONLY mode");
    } else {
        strategy.matrix_mode = ColorMatrixMode::NONE;
        Debug::Log("ConversionStrategy: Unknown format - using NONE mode");
    }

    // Build debug info
    std::string colorspace_name = "unknown";
    if (strategy.source_colorspace == 1) colorspace_name = "BT.709";
    else if (strategy.source_colorspace == 5) colorspace_name = "BT.601";
    else if (strategy.source_colorspace == 9) colorspace_name = "BT.2020";

    std::string range_name = (strategy.source_range == 1) ? "full" : "limited";

    std::string mode_name;
    std::string format_type;
    switch (strategy.matrix_mode) {
        case ColorMatrixMode::FULL_MATRIX:
            mode_name = "Full Matrix";
            format_type = "4444";
            break;
        case ColorMatrixMode::RANGE_ONLY:
            mode_name = "Range Only";
            format_type = "422/420";
            break;
        case ColorMatrixMode::NONE:
        default:
            mode_name = "No Processing";
            format_type = "unknown";
            break;
    }

    strategy.debug_info = mode_name + ": " + colorspace_name + " " + range_name + " " +
                         std::to_string(metadata.bit_depth) + "-bit " + format_type;

    return strategy;
}

bool ConversionStrategy::ShouldApplyColorMatrix() const {
    // Apply any color matrix processing (either full matrix or range-only)
    return matrix_mode != ColorMatrixMode::NONE;
}

bool ConversionStrategy::ShouldApplyFullMatrix() const {
    // Apply full colorspace + range conversion (4444 formats only)
    return matrix_mode == ColorMatrixMode::FULL_MATRIX;
}

bool ConversionStrategy::ShouldApplyRangeOnly() const {
    // Apply range conversion only (422/420 formats)
    return matrix_mode == ColorMatrixMode::RANGE_ONLY;
}

std::string ConversionStrategy::GetDescription() const {
    return debug_info;
}