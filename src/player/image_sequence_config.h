#pragma once

#include <string>
#include <vector>
#include "pipeline_mode.h"

namespace ump {

struct ImageSequenceConfig {
    // Pattern information
    std::string base_name;        // "sequence" from "sequence_0001.png"
    std::string separator;        // "_", "-", "." or "" (empty for no separator)
    int padding;                  // Number of digits (1-20)
    int start_number;            // First frame number
    int end_number;              // Last frame number
    std::string extension;       // ".png", ".exr", etc.
    std::string directory;       // Full directory path

    // Playback settings
    double fps;                  // User-selected frame rate
    PipelineMode pipeline_mode;  // Color depth setting

    // Generated patterns
    std::string ffmpeg_pattern;  // "/path/to/sequence_%04d.png" for background extraction
    std::string mpv_pattern;     // "sequence*png" for MPV playback
    std::string mf_url;          // "mf://path/sequence*png" for MPV loading

    // Frame information
    int frame_count;             // Total number of frames
    double duration;             // Calculated duration (frame_count / fps)

    // Validation
    bool is_valid = false;       // True if pattern was successfully parsed
    std::vector<int> missing_frames; // Frame numbers with gaps (for warning)

    // NEW: For native image sequence loading
    std::vector<std::string> sequence_files;  // Full paths to all files in sequence

    ImageSequenceConfig()
        : padding(4), start_number(1), end_number(1), fps(24.0),
          pipeline_mode(PipelineMode::NORMAL), frame_count(0), duration(0.0) {}
};

class ImageSequencePatternConverter {
public:
    // Parse sequence files and extract pattern information
    static ImageSequenceConfig ParseSequence(const std::vector<std::string>& sequence_files, double fps, PipelineMode pipeline_mode);

    // Build FFMPEG-compatible pattern from parsed info
    static std::string BuildFFmpegPattern(const ImageSequenceConfig& config);

    // Build MPV-compatible pattern from parsed info
    static std::string BuildMPVPattern(const ImageSequenceConfig& config);

    // Build full mf:// URL for MPV
    static std::string BuildMFUrl(const ImageSequenceConfig& config);

    // Validate sequence for gaps and consistency
    static bool ValidateSequence(const std::vector<std::string>& sequence_files, ImageSequenceConfig& config);

    // Extract frame number from filename
    static int ExtractFrameNumber(const std::string& filename);

    // Get padding (digit count) from filename
    static int GetPaddingFromFilename(const std::string& filename);

    // Generate FFMPEG command string for testing
    static std::string BuildFFmpegCommand(const ImageSequenceConfig& config);

private:
    // Parse individual filename using same regex logic as ProjectManager
    static bool ParseFilename(const std::string& filename, std::string& base_name,
                             std::string& separator, int& frame_number, int& padding);

    // Detect gaps in frame sequence
    static std::vector<int> DetectGaps(const std::vector<std::string>& sequence_files);

    // Cross-platform path handling
    static std::string NormalizePath(const std::string& path);
};

} // namespace ump