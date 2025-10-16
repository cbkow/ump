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

    // NEW: NCLC (nclx) color tag components for QuickTime
    // Format: ColorPrimaries-TransferCharacteristics-MatrixCoefficients
    // Example: 1-1-1 (BT.709), 1-2-1 (BT.709 primaries, unspecified transfer, BT.709 matrix)
    int nclc_primaries = 0;        // ColorPrimaries index
    int nclc_transfer = 0;         // TransferCharacteristics index
    int nclc_matrix = 0;           // MatrixCoefficients index
    std::string nclc_tag;          // Cached string representation "1-1-1", "1-2-1", etc.

    // Audio properties
    std::string audio_codec;
    int audio_sample_rate = 0;
    int audio_channels = 0;

    // Cache optimization properties
    int bit_depth = 8;                    // Detected from pixel_format (8, 10, 12, 16)
    bool has_alpha = false;               // yuva vs yuv formats
    bool is_hdr_content = false;          // Derived from transfer + bit_depth + colorspace
    std::string range_type;               // "limited", "full", or "unknown"

    // NEW: Chroma subsampling detection (cached for later use)
    bool is_411_format = false;           // 4:1:1 chroma subsampling
    bool is_421_format = false;           // 4:2:1 chroma subsampling (rare, some DVCPro)

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
    bool Is411Format() const;       // NEW: 411 format detection (4:1:1)
    bool Is421Format() const;       // NEW: 421 format detection (4:2:1)

    // NEW: NCLC tag detection from color properties
    void DetectAndCacheNCLC();      // Detects NCLC triplet from color_primaries, color_transfer, colorspace

    // Utility method to populate from file path
    void PopulateBasicFileInfo(const std::string& path) {
        file_path = path;
        if (fs::exists(path)) {
            file_name = fs::path(path).filename().string();
            file_size = fs::file_size(path);
        }
    }
};
