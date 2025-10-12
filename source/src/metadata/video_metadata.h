#pragma once

#include <string>
#include <memory>
#include <filesystem>

namespace fs = std::filesystem;

struct VideoMetadata {
    // Basic file info
    std::string file_name;
    std::string file_path;
    int64_t file_size = 0;

    // Timecode properties:
    bool has_embedded_timecode = false;
    double start_timecode = 0.0;
    std::string timecode_format;
    std::string source_format;
    bool timecode_checked = false;

    // Video properties  
    int width = 0;
    int height = 0;
    double frame_rate = 0.0;
    int total_frames = 0;
    std::string video_codec;
    std::string pixel_format;
    std::string colorspace;
    std::string color_primaries;
    std::string color_transfer;

    // Audio properties
    std::string audio_codec;
    int audio_sample_rate = 0;
    int audio_channels = 0;

    // Cache optimization properties
    int bit_depth = 8;                    // Detected from pixel_format (8, 10, 12, 16)
    bool has_alpha = false;               // yuva vs yuv formats
    bool is_hdr_content = false;          // Derived from transfer + bit_depth + colorspace
    std::string range_type;               // "limited", "full", or "unknown"

    bool is_loaded = false;

    // PERFORMANCE: Track if expensive detection methods have been run (lazy evaluation)
    mutable bool cache_properties_detected = false;

    // Cache-specific helper methods
    int DetectBitDepth() const;
    bool DetectHasAlpha() const;
    bool DetectHDRContent() const;
    std::string DetectRangeType() const;

    // NEW: Format detection for conditional color matrix application
    bool Is4444Format() const;
    bool Is422Or420Format() const;  // NEW: 422/420 format detection

    // Utility method to populate from file path
    void PopulateBasicFileInfo(const std::string& path) {
        file_path = path;
        if (fs::exists(path)) {
            file_name = fs::path(path).filename().string();
            file_size = fs::file_size(path);
        }
    }
};