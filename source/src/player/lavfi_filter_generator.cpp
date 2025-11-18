#include "lavfi_filter_generator.h"
#include "video_player.h"
#include "../utils/debug_utils.h"
#include <algorithm>

namespace ump {

std::string LavfiFilterGenerator::GenerateLavfiFilter(
    const VideoInput& primary,
    const VideoInput& secondary,
    const FilterConfig& config
) {
    std::ostringstream filter;

    switch (config.mode) {
        case ComparisonMode::SPLIT_HORIZONTAL: {
            // Horizontal stack (side-by-side)
            // Strategy: Scale Video 2 to match Video 1's height (only one scale operation)

            // Primary video - pass through at native resolution
            filter << "[" << primary.stream_id << "]";
            std::string primary_trim = GenerateTrimFilter(primary);
            if (!primary_trim.empty()) {
                filter << primary_trim << ",";
            }
            filter << "null[v1];";  // Pass-through, no scaling

            // Secondary video - scale to match primary height
            filter << "[" << secondary.stream_id << "]";
            std::string secondary_trim = GenerateTrimFilter(secondary);
            if (!secondary_trim.empty()) {
                filter << secondary_trim << ",";
            }

            // Choose scaler based on scale direction (area for downscale, fast_bilinear for upscale)
            bool is_downscale = (secondary.source_height > primary.source_height);
            std::string scaler = is_downscale ? "area" : "fast_bilinear";

            filter << GenerateScaleFilter(secondary, -1, primary.source_height, true, scaler);
            filter << "[v2];";

            // Stack horizontally
            filter << "[v1][v2]hstack[vo]";
            break;
        }

        case ComparisonMode::SPLIT_VERTICAL: {
            // Vertical stack (top/bottom)
            // Strategy: Scale Video 2 to match Video 1's width (only one scale operation)

            // Primary video - pass through at native resolution
            filter << "[" << primary.stream_id << "]";
            std::string primary_trim = GenerateTrimFilter(primary);
            if (!primary_trim.empty()) {
                filter << primary_trim << ",";
            }
            filter << "null[v1];";  // Pass-through, no scaling

            // Secondary video - scale to match primary width
            filter << "[" << secondary.stream_id << "]";
            std::string secondary_trim = GenerateTrimFilter(secondary);
            if (!secondary_trim.empty()) {
                filter << secondary_trim << ",";
            }

            // Choose scaler based on scale direction
            bool is_downscale = (secondary.source_width > primary.source_width);
            std::string scaler = is_downscale ? "area" : "fast_bilinear";

            filter << GenerateScaleFilter(secondary, primary.source_width, -1, true, scaler);
            filter << "[v2];";

            // Stack vertically
            filter << "[v1][v2]vstack[vo]";
            break;
        }

        case ComparisonMode::SPLIT_5050_HORIZONTAL: {
            // 50/50 split with crop and horizontal stack
            // [vid1]trim,scale=W:H,crop=W*split:H:0:0[v1];[vid2]trim,scale=W:H,crop=W*(1-split):H:W*split:0[v2];[v1][v2]hstack[vo]
            filter << Generate5050SplitFilter(primary, secondary, config);
            break;
        }

        case ComparisonMode::DIFFERENCE_BLEND: {
            // Difference blend overlay
            // Strategy: Scale Video 2 to match Video 1's exact dimensions

            // Primary video - pass through at native resolution
            filter << "[" << primary.stream_id << "]";
            std::string primary_trim = GenerateTrimFilter(primary);
            if (!primary_trim.empty()) {
                filter << primary_trim << ",";
            }
            filter << "null[v1];";  // Pass-through, no scaling

            // Secondary video - scale to match primary exactly (for pixel-perfect blend)
            filter << "[" << secondary.stream_id << "]";
            std::string secondary_trim = GenerateTrimFilter(secondary);
            if (!secondary_trim.empty()) {
                filter << secondary_trim << ",";
            }

            // Choose scaler based on total pixel count (downscale vs upscale)
            bool is_downscale = (secondary.source_width * secondary.source_height >
                                 primary.source_width * primary.source_height);
            std::string scaler = is_downscale ? "area" : "fast_bilinear";

            filter << GenerateScaleFilter(secondary, primary.source_width, primary.source_height, false, scaler);
            filter << "[v2];";

            // Blend with difference mode
            filter << "[v1][v2]blend=all_mode=difference[vo]";
            break;
        }

        default:
            Debug::Log("ERROR: Unsupported comparison mode for lavfi filter generation");
            return "";
    }

    std::string filter_str = filter.str();
    Debug::Log("Generated lavfi filter: " + filter_str);
    return filter_str;
}

void LavfiFilterGenerator::CalculateOptimalScale(
    int source_width, int source_height,
    int viewport_width, int viewport_height,
    ::ComparisonMode mode,
    int& out_width, int& out_height
) {
    if (source_width <= 0 || source_height <= 0) {
        out_width = viewport_width;
        out_height = viewport_height;
        return;
    }

    float source_aspect = static_cast<float>(source_width) / static_cast<float>(source_height);
    float viewport_aspect = static_cast<float>(viewport_width) / static_cast<float>(viewport_height);

    // For horizontal stack, we want to match height and let width vary
    if (mode == ComparisonMode::SPLIT_HORIZONTAL) {
        out_height = viewport_height;
        out_width = static_cast<int>(out_height * source_aspect);
    }
    // For vertical stack, we want to match width and let height vary
    else if (mode == ComparisonMode::SPLIT_VERTICAL) {
        out_width = viewport_width;
        out_height = static_cast<int>(out_width / source_aspect);
    }
    // For 50/50 split and difference, scale to fill viewport
    else {
        if (viewport_aspect > source_aspect) {
            // Viewport is wider, match width
            out_width = viewport_width;
            out_height = static_cast<int>(out_width / source_aspect);
        } else {
            // Viewport is taller, match height
            out_height = viewport_height;
            out_width = static_cast<int>(out_height * source_aspect);
        }
    }
}

std::string LavfiFilterGenerator::GenerateTrimFilter(const VideoInput& input) {
    if (input.trim_start < 0.0) {
        return "";
    }

    std::ostringstream trim;

    if (input.trim_duration > 0.0) {
        // Both start and duration specified - use end time
        double end_time = input.trim_start + input.trim_duration;
        trim << "trim=start=" << input.trim_start << ":end=" << end_time << ",setpts=PTS-STARTPTS";
    } else {
        // Only start specified - trim from start to end of video
        trim << "trim=start=" << input.trim_start << ",setpts=PTS-STARTPTS";
    }

    return trim.str();
}

std::string LavfiFilterGenerator::GenerateScaleFilter(
    const VideoInput& input,
    int target_width,
    int target_height,
    bool maintain_aspect,
    const std::string& scaler_flags
) {
    // Check if scaling is actually needed
    if (target_width > 0 && target_height > 0) {
        if (target_width == input.source_width && target_height == input.source_height) {
            return "null";  // No scaling needed, pass-through filter
        }
    }

    std::ostringstream scale;
    scale << "scale=";

    if (target_width < 0 && target_height < 0) {
        // No scaling
        scale << input.source_width << ":" << input.source_height;
    } else if (target_width < 0) {
        // Maintain aspect, height fixed
        scale << "-1:" << target_height;
    } else if (target_height < 0) {
        // Maintain aspect, width fixed
        scale << target_width << ":-1";
    } else {
        // Both dimensions specified
        scale << target_width << ":" << target_height;
    }

    // Add scaler performance flags
    if (!scaler_flags.empty()) {
        scale << ":flags=" << scaler_flags;
    }

    return scale.str();
}

std::string LavfiFilterGenerator::GenerateCropFilter(
    int width, int height,
    int x, int y
) {
    std::ostringstream crop;
    crop << "crop=" << width << ":" << height << ":" << x << ":" << y;
    return crop.str();
}

std::string LavfiFilterGenerator::Generate5050SplitFilter(
    const VideoInput& primary,
    const VideoInput& secondary,
    const FilterConfig& config
) {
    std::ostringstream filter;

    int scale_w = config.viewport_width;
    int scale_h = config.viewport_height;

    // Calculate crop widths based on split position
    int left_crop_width = static_cast<int>(scale_w * config.split_position);
    int right_crop_width = scale_w - left_crop_width;
    int right_crop_x = left_crop_width;

    // Primary video (left side)
    filter << "[" << primary.stream_id << "]";
    std::string primary_trim = GenerateTrimFilter(primary);
    if (!primary_trim.empty()) {
        filter << primary_trim << ",";
    }
    filter << GenerateScaleFilter(primary, scale_w, scale_h, false) << ",";
    filter << GenerateCropFilter(left_crop_width, scale_h, 0, 0);
    filter << "[v1];";

    // Secondary video (right side)
    filter << "[" << secondary.stream_id << "]";
    std::string secondary_trim = GenerateTrimFilter(secondary);
    if (!secondary_trim.empty()) {
        filter << secondary_trim << ",";
    }
    filter << GenerateScaleFilter(secondary, scale_w, scale_h, false) << ",";
    filter << GenerateCropFilter(right_crop_width, scale_h, right_crop_x, 0);
    filter << "[v2];";

    // Stack horizontally
    filter << "[v1][v2]hstack[vo]";

    return filter.str();
}

std::string LavfiFilterGenerator::EscapeFilterString(const std::string& input) {
    std::string output;
    output.reserve(input.length());

    for (char c : input) {
        // Escape special characters for lavfi
        if (c == ':' || c == ',' || c == '[' || c == ']' || c == '\\') {
            output += '\\';
        }
        output += c;
    }

    return output;
}

} // namespace ump
