#pragma once

#include <string>
#include <sstream>
#include <cmath>

// Forward declaration (ComparisonMode is in global namespace)
enum class ComparisonMode;

namespace ump {

/**
 * @brief Generates lavfi filter graphs for dual video comparison modes
 *
 * Supports horizontal/vertical stacking, 50/50 splits, and difference blending.
 * Handles independent trim points and optimal scaling for each video.
 */
class LavfiFilterGenerator {
public:
    /**
     * @brief Input video configuration
     */
    struct VideoInput {
        std::string stream_id;      // "vid1" or "vid2"
        double trim_start = -1.0;   // Start time in seconds (-1 = no trim)
        double trim_duration = -1.0; // Duration in seconds (-1 = no trim)
        int source_width = 0;       // Original video width
        int source_height = 0;      // Original video height
        bool has_audio = true;      // Whether this video has an audio track
    };

    /**
     * @brief Filter generation configuration
     */
    struct FilterConfig {
        ::ComparisonMode mode;  // ComparisonMode is in global namespace
        int viewport_width = 1920;
        int viewport_height = 1080;
        float split_position = 0.5f;  // For adjustable splits (0.0-1.0)
        bool maintain_aspect = true;
    };

    /**
     * @brief Generate complete lavfi-complex filter string
     *
     * @param primary Primary video input configuration
     * @param secondary Secondary video input configuration
     * @param config Filter configuration (mode, viewport size, etc.)
     * @return Complete lavfi filter graph string
     */
    static std::string GenerateLavfiFilter(
        const VideoInput& primary,
        const VideoInput& secondary,
        const FilterConfig& config
    );

    /**
     * @brief Calculate optimal scale dimensions for a video in split mode
     *
     * @param source_width Original video width
     * @param source_height Original video height
     * @param viewport_width Viewport width
     * @param viewport_height Viewport height
     * @param mode Comparison mode (affects scaling strategy)
     * @param out_width Output scaled width
     * @param out_height Output scaled height
     */
    static void CalculateOptimalScale(
        int source_width, int source_height,
        int viewport_width, int viewport_height,
        ::ComparisonMode mode,  // ComparisonMode is in global namespace
        int& out_width, int& out_height
    );

private:
    /**
     * @brief Generate trim filter for a video stream
     * @return Filter string like "trim=start=2,setpts=PTS-STARTPTS"
     */
    static std::string GenerateTrimFilter(const VideoInput& input);

    /**
     * @brief Generate audio trim filter for an audio stream
     * @return Filter string like "atrim=start=2:end=5,asetpts=PTS-STARTPTS"
     */
    static std::string GenerateAudioTrimFilter(const VideoInput& input);

    /**
     * @brief Generate scale filter for a video stream
     * @param scaler_flags FFmpeg scaler flags (fast_bilinear, area, bicubic, lanczos)
     * @return Filter string like "scale=1920:1080:flags=fast_bilinear" or "null" if no scaling needed
     */
    static std::string GenerateScaleFilter(
        const VideoInput& input,
        int target_width,
        int target_height,
        bool maintain_aspect,
        const std::string& scaler_flags = "fast_bilinear"
    );

    /**
     * @brief Generate crop filter
     * @return Filter string like "crop=960:1080:0:0"
     */
    static std::string GenerateCropFilter(
        int width, int height,
        int x, int y
    );

    /**
     * @brief Generate horizontal stack filter
     */
    static std::string GenerateHStackFilter(const FilterConfig& config);

    /**
     * @brief Generate vertical stack filter
     */
    static std::string GenerateVStackFilter(const FilterConfig& config);

    /**
     * @brief Generate 50/50 split horizontal filter with adjustable split point
     */
    static std::string Generate5050SplitFilter(
        const VideoInput& primary,
        const VideoInput& secondary,
        const FilterConfig& config
    );

    /**
     * @brief Generate difference blend filter
     */
    static std::string GenerateDifferenceFilter(const FilterConfig& config);

    /**
     * @brief Escape special characters for lavfi filter strings
     */
    static std::string EscapeFilterString(const std::string& input);
};

} // namespace ump
