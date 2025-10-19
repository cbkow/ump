#pragma once

#include <string>
#include <vector>
#include <filesystem>

namespace fs = std::filesystem;

struct EXRChannelInfo {
    std::string name;           // e.g., "R", "G", "B", "A", "beauty.R"
    std::string pixel_type;     // "half", "float", "uint"
    int x_sampling = 1;
    int y_sampling = 1;
    bool linear = false;
};

struct EXRLayerInfo {
    std::string name;           // e.g., "beauty", "diffuse", "RGBA" (main layer)
    std::string display_name;   // Human-readable name
    int channel_count = 0;      // Number of channels in this layer
    std::string channel_types;  // e.g., "RGB", "RGBA", "RGBAZ"
    std::string pixel_type;     // "half", "float", "uint" (dominant type)
    bool has_alpha = false;
    bool is_main_layer = false; // True for default/main layer
};

struct EXRMetadata {
    // Basic file info
    std::string file_name;
    std::string file_path;
    int64_t file_size = 0;

    // Sequence properties
    int start_frame = 0;
    int end_frame = 0;
    int total_frames = 0;
    double frame_rate = 0.0;
    std::string sequence_pattern;   // e.g., "sequence.%04d.exr"

    // Image properties
    int width = 0;
    int height = 0;
    int display_width = 0;          // Display window dimensions
    int display_height = 0;
    int data_width = 0;             // Data window dimensions
    int data_height = 0;

    // EXR-specific properties
    std::string compression;        // "PIZ", "ZIP", "DWAA", "B44A", etc.
    std::string layer_name;         // Selected layer (e.g., "beauty", "diffuse")
    bool is_tiled = false;
    bool is_multi_part = false;
    int part_count = 0;

    // Channel information (individual channels - for internal use)
    std::vector<EXRChannelInfo> channels;
    int total_channels = 0;

    // Layer information (grouped channels - for display)
    std::vector<EXRLayerInfo> layers;
    int total_layers = 0;
    std::string layer_summary;      // e.g., "3 layers (beauty, diffuse, specular)"

    // Color space info (from EXR attributes if present)
    std::string colorspace;
    std::string chromaticities;

    // Pixel format analysis
    std::string pixel_format;       // "RGB half", "RGBA float", etc.
    int bit_depth = 16;             // Typically 16 (half) or 32 (float)

    bool is_loaded = false;

    // PERFORMANCE: Track if expensive detection methods have been run
    mutable bool extended_properties_detected = false;

    // Helper methods
    void DetectAndCacheExtendedProperties(); // Lazy extraction of compression, channels, etc.

    // Utility method to populate from first file in sequence
    void PopulateBasicFileInfo(const std::string& path) {
        file_path = path;
        if (fs::exists(path)) {
            file_name = fs::path(path).filename().string();
            file_size = fs::file_size(path);
        }
    }
};
