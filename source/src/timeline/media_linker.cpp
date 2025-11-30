#include "media_linker.h"
#include "../utils/debug_utils.h"
#include <filesystem>
#include <algorithm>
#include <regex>
#include <cctype>
#include <set>

// FFmpeg for video probing
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}

namespace fs = std::filesystem;

// ============================================================================
// Video Probing Helper
// ============================================================================

struct VideoProbeResult {
    bool valid = false;
    double fps = 0.0;
    double duration = 0.0;
    int width = 0;
    int height = 0;
};

static VideoProbeResult ProbeVideoFile(const std::string& path) {
    VideoProbeResult result;

    AVFormatContext* fmt_ctx = nullptr;
    if (avformat_open_input(&fmt_ctx, path.c_str(), nullptr, nullptr) < 0) {
        return result;
    }

    if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
        avformat_close_input(&fmt_ctx);
        return result;
    }

    // Find video stream
    int video_stream_idx = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (video_stream_idx >= 0) {
        AVStream* stream = fmt_ctx->streams[video_stream_idx];
        AVCodecParameters* codecpar = stream->codecpar;

        result.width = codecpar->width;
        result.height = codecpar->height;

        // Get frame rate
        if (stream->avg_frame_rate.den > 0 && stream->avg_frame_rate.num > 0) {
            result.fps = av_q2d(stream->avg_frame_rate);
        } else if (stream->r_frame_rate.den > 0 && stream->r_frame_rate.num > 0) {
            result.fps = av_q2d(stream->r_frame_rate);
        } else {
            result.fps = 24.0;  // Fallback
        }

        // Get duration
        if (fmt_ctx->duration > 0) {
            result.duration = static_cast<double>(fmt_ctx->duration) / AV_TIME_BASE;
        } else if (stream->duration > 0) {
            result.duration = av_q2d(stream->time_base) * stream->duration;
        }

        result.valid = true;
    }

    avformat_close_input(&fmt_ctx);
    return result;
}

// Check if file is a video format (not image sequence)
static bool IsVideoFile(const std::string& path) {
    std::string ext = fs::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    // Video container formats
    static const std::set<std::string> video_extensions = {
        ".mov", ".mp4", ".mxf", ".avi", ".mkv", ".m4v",
        ".webm", ".wmv", ".flv", ".mpg", ".mpeg", ".m2v"
    };

    return video_extensions.count(ext) > 0;
}

namespace ump {

// ============================================================================
// Static Helper Functions
// ============================================================================

std::string MediaLinker::NormalizeClipName(const std::string& name, bool fuzzy) {
    std::string normalized = name;

    // Convert to lowercase
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), ::tolower);

    // Remove file extension if present
    size_t dot_pos = normalized.rfind('.');
    if (dot_pos != std::string::npos) {
        std::string ext = normalized.substr(dot_pos);
        // Check if it looks like an extension (short, alphanumeric)
        if (ext.length() <= 5) {
            normalized = normalized.substr(0, dot_pos);
        }
    }

    if (fuzzy) {
        // Remove common suffixes like .new.01, .new.02, _v001, etc.
        // Pattern: .new.## or _new.## or .## at end
        std::regex suffix_pattern(R"([\._](new|v|ver|version)?[\._]?\d+$)", std::regex::icase);
        normalized = std::regex_replace(normalized, suffix_pattern, "");

        // Remove trailing underscores/dots
        while (!normalized.empty() && (normalized.back() == '_' || normalized.back() == '.')) {
            normalized.pop_back();
        }
    }

    return normalized;
}

float MediaLinker::CalculateMatchScore(const std::string& clip_name, const std::string& filename, bool fuzzy) {
    std::string norm_clip = NormalizeClipName(clip_name, fuzzy);
    std::string norm_file = NormalizeClipName(filename, fuzzy);

    if (norm_clip.empty() || norm_file.empty()) {
        return 0.0f;
    }

    // Exact match after normalization
    if (norm_clip == norm_file) {
        return 1.0f;
    }

    // Check if one contains the other
    if (norm_file.find(norm_clip) != std::string::npos) {
        // Clip name is substring of filename
        return 0.9f * (static_cast<float>(norm_clip.length()) / norm_file.length());
    }
    if (norm_clip.find(norm_file) != std::string::npos) {
        // Filename is substring of clip name
        return 0.9f * (static_cast<float>(norm_file.length()) / norm_clip.length());
    }

    // Calculate simple character overlap score
    int matches = 0;
    size_t min_len = std::min(norm_clip.length(), norm_file.length());
    for (size_t i = 0; i < min_len; i++) {
        if (norm_clip[i] == norm_file[i]) {
            matches++;
        }
    }

    if (min_len > 0) {
        float prefix_score = static_cast<float>(matches) / min_len;
        if (prefix_score > 0.8f) {
            return prefix_score * 0.7f;  // Good prefix match but not exact
        }
    }

    return 0.0f;
}

// ============================================================================
// File Index Building
// ============================================================================

std::map<std::string, std::string> MediaLinker::BuildFileIndex(
    const std::string& directory,
    const LinkOptions& options
) {
    std::map<std::string, std::string> index;

    try {
        auto process_file = [&](const fs::path& path) {
            if (fs::is_regular_file(path)) {
                std::string ext = path.extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

                // Check if extension is in our list
                bool valid_ext = options.extensions.empty();
                for (const auto& valid : options.extensions) {
                    if (ext == valid) {
                        valid_ext = true;
                        break;
                    }
                }

                if (valid_ext) {
                    std::string filename = path.filename().string();
                    std::string normalized = NormalizeClipName(filename, options.fuzzy_match);

                    // Store normalized name -> full path
                    // If duplicate normalized names, keep the first one found
                    if (index.find(normalized) == index.end()) {
                        index[normalized] = path.string();
                    }
                }
            }
        };

        if (options.recursive) {
            // Use recursive iterator with depth control
            fs::recursive_directory_iterator iter(directory, fs::directory_options::skip_permission_denied);
            fs::recursive_directory_iterator end;

            while (iter != end) {
                // Check depth limit (max_depth of 0 means unlimited)
                if (options.max_depth > 0 && iter.depth() >= options.max_depth) {
                    // Skip this directory's contents
                    iter.disable_recursion_pending();
                }

                process_file(iter->path());
                ++iter;
            }
        } else {
            for (const auto& entry : fs::directory_iterator(directory)) {
                process_file(entry.path());
            }
        }
    } catch (const std::exception& e) {
        Debug::Log("MediaLinker: Error scanning directory: " + std::string(e.what()));
    }

    Debug::Log("MediaLinker: Indexed " + std::to_string(index.size()) + " files in " + directory +
               " (max depth: " + std::to_string(options.max_depth) + ")");
    return index;
}

// ============================================================================
// Matching
// ============================================================================

std::pair<std::string, float> MediaLinker::FindBestMatch(
    const std::string& clip_name,
    const std::map<std::string, std::string>& file_index,
    const LinkOptions& options
) {
    std::string normalized_clip = NormalizeClipName(clip_name, options.fuzzy_match);

    // First try exact match on normalized name
    auto exact_it = file_index.find(normalized_clip);
    if (exact_it != file_index.end()) {
        return {exact_it->second, 1.0f};
    }

    // Try fuzzy matching
    std::string best_path;
    float best_score = 0.0f;

    for (const auto& [norm_filename, full_path] : file_index) {
        float score = CalculateMatchScore(clip_name, norm_filename, options.fuzzy_match);
        if (score > best_score && score >= 0.7f) {  // Minimum threshold
            best_score = score;
            best_path = full_path;
        }
    }

    return {best_path, best_score};
}

// ============================================================================
// Main Linking Functions
// ============================================================================

LinkSummary MediaLinker::LinkMediaInDirectory(
    std::vector<OTIOTrack>& tracks,
    const std::string& search_directory,
    const LinkOptions& options
) {
    LinkSummary summary;

    // Count total clips
    for (const auto& track : tracks) {
        for (const auto& clip : track.clips) {
            if (!clip.is_gap) {
                summary.total_clips++;
            }
        }
    }

    if (summary.total_clips == 0) {
        Debug::Log("MediaLinker: No clips to link");
        return summary;
    }

    Debug::Log("MediaLinker: Attempting to link " + std::to_string(summary.total_clips) +
               " clips from " + search_directory);

    // Build file index
    if (progress_callback_) {
        progress_callback_(0, summary.total_clips, "Scanning directory...");
    }

    auto file_index = BuildFileIndex(search_directory, options);

    if (file_index.empty()) {
        Debug::Log("MediaLinker: No media files found in directory");
        return summary;
    }

    // Link each clip
    int processed = 0;
    for (auto& track : tracks) {
        for (auto& clip : track.clips) {
            if (clip.is_gap) continue;

            processed++;
            if (progress_callback_) {
                progress_callback_(processed, summary.total_clips,
                                  "Linking: " + clip.name);
            }

            LinkResult result;
            result.clip_id = clip.id;
            result.clip_name = clip.name;

            // Try to find match
            auto [matched_path, score] = FindBestMatch(clip.name, file_index, options);

            if (!matched_path.empty() && score >= 0.7f) {
                clip.linked_path = matched_path;
                clip.is_linked = true;
                result.matched_path = matched_path;
                result.success = true;
                result.match_score = score;
                summary.linked_count++;

                // Probe video files to get source metadata
                if (IsVideoFile(matched_path)) {
                    VideoProbeResult probe = ProbeVideoFile(matched_path);
                    if (probe.valid) {
                        clip.source_fps = probe.fps;
                        clip.source_width = probe.width;
                        clip.source_height = probe.height;
                        clip.source_duration = probe.duration;
                    }
                }
            } else {
                clip.is_linked = false;
                clip.linked_path.clear();
                result.success = false;
                summary.unlinked_count++;
            }

            summary.results.push_back(result);
        }
    }

    Debug::Log("MediaLinker: Linked " + std::to_string(summary.linked_count) + "/" +
               std::to_string(summary.total_clips) + " clips");

    return summary;
}

bool MediaLinker::LinkClip(OTIOClip& clip, const std::string& media_path) {
    if (!fs::exists(media_path)) {
        Debug::Log("MediaLinker: File not found: " + media_path);
        return false;
    }

    clip.linked_path = media_path;
    clip.is_linked = true;

    // Probe video files to get source metadata
    if (IsVideoFile(media_path)) {
        VideoProbeResult probe = ProbeVideoFile(media_path);
        if (probe.valid) {
            clip.source_fps = probe.fps;
            clip.source_width = probe.width;
            clip.source_height = probe.height;
            clip.source_duration = probe.duration;
            Debug::Log("MediaLinker: Probed video - " + std::to_string(probe.width) + "x" +
                       std::to_string(probe.height) + " @ " + std::to_string(probe.fps) + " fps, " +
                       std::to_string(probe.duration) + "s");
        }
    }

    Debug::Log("MediaLinker: Linked clip '" + clip.name + "' to " + media_path);
    return true;
}

void MediaLinker::UnlinkClip(OTIOClip& clip) {
    clip.linked_path.clear();
    clip.is_linked = false;
}

std::vector<OTIOClip*> MediaLinker::GetUnlinkedClips(std::vector<OTIOTrack>& tracks) {
    std::vector<OTIOClip*> unlinked;
    for (auto& track : tracks) {
        for (auto& clip : track.clips) {
            if (!clip.is_gap && !clip.is_linked) {
                unlinked.push_back(&clip);
            }
        }
    }
    return unlinked;
}

LinkSummary MediaLinker::GetLinkStatus(const std::vector<OTIOTrack>& tracks) {
    LinkSummary summary;
    for (const auto& track : tracks) {
        for (const auto& clip : track.clips) {
            if (clip.is_gap) continue;

            summary.total_clips++;
            if (clip.is_linked) {
                summary.linked_count++;
            } else {
                summary.unlinked_count++;
            }

            LinkResult result;
            result.clip_id = clip.id;
            result.clip_name = clip.name;
            result.matched_path = clip.linked_path;
            result.success = clip.is_linked;
            summary.results.push_back(result);
        }
    }
    return summary;
}

} // namespace ump
