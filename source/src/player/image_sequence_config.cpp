#include "image_sequence_config.h"
#include "../utils/debug_utils.h"
#include <filesystem>
#include <regex>
#include <algorithm>
#include <set>
#include <sstream>
#include <iomanip>

namespace ump {

ImageSequenceConfig ImageSequencePatternConverter::ParseSequence(const std::vector<std::string>& sequence_files, double fps, PipelineMode pipeline_mode) {
    ImageSequenceConfig config;
    config.fps = fps;
    config.pipeline_mode = pipeline_mode;
    config.frame_count = static_cast<int>(sequence_files.size());
    config.duration = static_cast<double>(config.frame_count) / fps;

    // Store the actual file paths for native image loading
    config.sequence_files = sequence_files;

    if (sequence_files.empty()) {
        Debug::Log("ImageSequencePatternConverter: Empty sequence files");
        return config;
    }

    // Parse the first file to establish the pattern
    std::filesystem::path first_path(sequence_files[0]);
    config.directory = first_path.parent_path().string();
    config.extension = first_path.extension().string();

    std::string first_filename = first_path.stem().string();
    std::string base_name, separator;
    int first_frame_number, padding;

    if (!ParseFilename(first_filename, base_name, separator, first_frame_number, padding)) {
        Debug::Log("ImageSequencePatternConverter: Failed to parse first filename: " + first_filename);
        return config;
    }

    config.base_name = base_name;
    config.separator = separator;
    config.padding = padding;
    config.start_number = first_frame_number;

    // Parse the last file to get the end frame
    std::filesystem::path last_path(sequence_files.back());
    std::string last_filename = last_path.stem().string();
    std::string last_base, last_separator;
    int last_frame_number, last_padding;

    if (ParseFilename(last_filename, last_base, last_separator, last_frame_number, last_padding)) {
        config.end_number = last_frame_number;
    } else {
        config.end_number = first_frame_number;
        Debug::Log("ImageSequencePatternConverter: Could not parse last filename, using first frame number");
    }

    // Validate the sequence
    if (!ValidateSequence(sequence_files, config)) {
        Debug::Log("ImageSequencePatternConverter: Sequence validation failed");
        return config;
    }

    // Generate patterns
    config.ffmpeg_pattern = BuildFFmpegPattern(config);
    config.mpv_pattern = BuildMPVPattern(config);
    config.mf_url = BuildMFUrl(config);

    config.is_valid = true;
    Debug::Log("ImageSequencePatternConverter: Successfully parsed sequence");
    Debug::Log("  Base: " + config.base_name + ", Separator: '" + config.separator + "'");
    Debug::Log("  Padding: " + std::to_string(config.padding) + ", Range: " + std::to_string(config.start_number) + "-" + std::to_string(config.end_number));
    Debug::Log("  FFMPEG Pattern: " + config.ffmpeg_pattern);
    Debug::Log("  MPV Pattern: " + config.mf_url);

    return config;
}

std::string ImageSequencePatternConverter::BuildFFmpegPattern(const ImageSequenceConfig& config) {
    std::ostringstream pattern;

    // Normalize directory path for cross-platform compatibility
    std::string normalized_dir = NormalizePath(config.directory);

    pattern << normalized_dir << "/" << config.base_name;

    // Add separator if it exists
    if (!config.separator.empty()) {
        pattern << config.separator;
    }

    // Add FFMPEG %d pattern with appropriate padding
    if (config.padding == 1) {
        pattern << "%d";  // No padding for single digit
    } else {
        pattern << "%0" << config.padding << "d";
    }

    pattern << config.extension;

    return pattern.str();
}

std::string ImageSequencePatternConverter::BuildMPVPattern(const ImageSequenceConfig& config) {
    // MPV uses glob-style patterns: basename*extension
    std::string file_basename = config.base_name;

    // Remove trailing separator if it exists (MPV glob doesn't need it)
    if (!config.separator.empty()) {
        if (!file_basename.empty() && file_basename.back() == config.separator[0]) {
            file_basename.pop_back();
        }
    }

    return file_basename + "*" + config.extension;
}

std::string ImageSequencePatternConverter::BuildMFUrl(const ImageSequenceConfig& config) {
    std::string normalized_dir = NormalizePath(config.directory);
    // Replace backslashes with forward slashes for MPV compatibility
    std::replace(normalized_dir.begin(), normalized_dir.end(), '\\', '/');

    return "mf://" + normalized_dir + "/" + BuildMPVPattern(config);
}

bool ImageSequencePatternConverter::ValidateSequence(const std::vector<std::string>& sequence_files, ImageSequenceConfig& config) {
    if (sequence_files.empty()) return false;

    std::set<int> frame_numbers;
    std::string expected_base = config.base_name;
    std::string expected_separator = config.separator;
    int expected_padding = config.padding;
    std::string expected_extension = config.extension;

    for (const auto& file_path : sequence_files) {
        std::filesystem::path path(file_path);

        // Check extension consistency
        if (path.extension().string() != expected_extension) {
            Debug::Log("ImageSequencePatternConverter: Inconsistent extension in: " + file_path);
            return false;
        }

        std::string filename = path.stem().string();
        std::string base_name, separator;
        int frame_number, padding;

        if (!ParseFilename(filename, base_name, separator, frame_number, padding)) {
            Debug::Log("ImageSequencePatternConverter: Could not parse filename: " + filename);
            return false;
        }

        // Check pattern consistency
        if (base_name != expected_base || separator != expected_separator || padding != expected_padding) {
            Debug::Log("ImageSequencePatternConverter: Inconsistent pattern in: " + filename);
            Debug::Log("  Expected: " + expected_base + expected_separator + " (padding=" + std::to_string(expected_padding) + ")");
            Debug::Log("  Found: " + base_name + separator + " (padding=" + std::to_string(padding) + ")");
            return false;
        }

        frame_numbers.insert(frame_number);
    }

    // Detect gaps
    config.missing_frames = DetectGaps(sequence_files);
    if (!config.missing_frames.empty()) {
        Debug::Log("ImageSequencePatternConverter: Warning - " + std::to_string(config.missing_frames.size()) + " missing frames detected");
    }

    return true;
}

bool ImageSequencePatternConverter::ParseFilename(const std::string& filename, std::string& base_name,
                                                 std::string& separator, int& frame_number, int& padding) {
    // Use same regex patterns as ProjectManager for consistency
    std::regex pattern(R"(^(.+)([_\.\-])(\d+)$)");
    std::smatch match;

    if (std::regex_match(filename, match, pattern)) {
        // Pattern with separator found
        base_name = match[1].str();
        separator = match[2].str();
        std::string number_str = match[3].str();
        frame_number = std::stoi(number_str);
        padding = static_cast<int>(number_str.length());
        return true;
    }

    // Try pattern without separator (minimum 3 digits to avoid false positives)
    std::regex no_sep_pattern(R"(^(.+?)(\d{3,})$)");
    if (std::regex_match(filename, match, no_sep_pattern)) {
        base_name = match[1].str();
        separator = "";  // No separator
        std::string number_str = match[2].str();
        frame_number = std::stoi(number_str);
        padding = static_cast<int>(number_str.length());
        return true;
    }

    return false;
}

std::vector<int> ImageSequencePatternConverter::DetectGaps(const std::vector<std::string>& sequence_files) {
    std::set<int> frame_numbers;

    for (const auto& file_path : sequence_files) {
        int frame_number = ExtractFrameNumber(file_path);
        if (frame_number >= 0) {
            frame_numbers.insert(frame_number);
        }
    }

    if (frame_numbers.empty()) return {};

    std::vector<int> gaps;
    int start = *frame_numbers.begin();
    int end = *frame_numbers.rbegin();

    for (int i = start; i <= end; ++i) {
        if (frame_numbers.find(i) == frame_numbers.end()) {
            gaps.push_back(i);
        }
    }

    return gaps;
}

int ImageSequencePatternConverter::ExtractFrameNumber(const std::string& filename) {
    std::filesystem::path path(filename);
    std::string stem = path.stem().string();

    std::string base_name, separator;
    int frame_number, padding;

    if (ParseFilename(stem, base_name, separator, frame_number, padding)) {
        return frame_number;
    }

    return -1;  // Could not extract
}

int ImageSequencePatternConverter::GetPaddingFromFilename(const std::string& filename) {
    std::filesystem::path path(filename);
    std::string stem = path.stem().string();

    std::string base_name, separator;
    int frame_number, padding;

    if (ParseFilename(stem, base_name, separator, frame_number, padding)) {
        return padding;
    }

    return 0;  // Could not determine
}

std::string ImageSequencePatternConverter::BuildFFmpegCommand(const ImageSequenceConfig& config) {
    std::ostringstream cmd;

    cmd << "ffmpeg -framerate " << std::fixed << std::setprecision(3) << config.fps;
    cmd << " -start_number " << config.start_number;
    cmd << " -i \"" << config.ffmpeg_pattern << "\"";

    // Add color pipeline settings
    switch (config.pipeline_mode) {
        case PipelineMode::NORMAL:
            cmd << " -pix_fmt rgba";  // 8-bit RGBA
            break;
        case PipelineMode::HIGH_RES:
            cmd << " -pix_fmt rgba64le";  // 16-bit RGBA
            break;
        case PipelineMode::ULTRA_HIGH_RES:
        case PipelineMode::HDR_RES:
            cmd << " -pix_fmt rgbaf32le";  // 32-bit float RGBA
            break;
        default:
            cmd << " -pix_fmt rgba";  // Default to 8-bit
            break;
    }

    cmd << " -f rawvideo -";  // Output to stdout

    return cmd.str();
}

std::string ImageSequencePatternConverter::NormalizePath(const std::string& path) {
    std::string normalized = path;

    // Replace backslashes with forward slashes for consistency
    std::replace(normalized.begin(), normalized.end(), '\\', '/');

    // Remove trailing slash if present
    if (!normalized.empty() && normalized.back() == '/') {
        normalized.pop_back();
    }

    return normalized;
}

} // namespace ump