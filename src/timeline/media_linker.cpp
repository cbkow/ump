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

ump::VideoProbeResult ump::MediaLinker::ProbeVideoFile(const std::string& path) {
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

    // Check for audio stream
    int audio_stream_idx = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    result.has_audio = (audio_stream_idx >= 0);

    avformat_close_input(&fmt_ctx);
    return result;
}

// Check if file is a video format (not image sequence)
bool ump::MediaLinker::IsVideoFile(const std::string& path) {
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

    // Log first 10 indexed files for debugging
    int logged = 0;
    for (const auto& [norm_name, full_path] : index) {
        if (logged++ < 10) {
            Debug::Log("MediaLinker: Indexed file: '" + norm_name + "' -> " + full_path);
        }
    }

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

// Helper to recursively count clips in tracks (including nested)
static void CountClipsRecursive(const std::vector<OTIOTrack>& tracks, int& total, int& already_linked) {
    for (const auto& track : tracks) {
        for (const auto& clip : track.clips) {
            if (clip.is_gap) continue;

            if (clip.is_nested && clip.nested_loaded && !clip.nested_tracks.empty()) {
                // Recurse into nested tracks
                CountClipsRecursive(clip.nested_tracks, total, already_linked);
            } else if (!clip.is_nested) {
                // Regular clip - count it
                total++;
                if (clip.is_linked) {
                    already_linked++;
                }
            }
        }
    }
}

LinkSummary MediaLinker::LinkMediaInDirectory(
    std::vector<OTIOTrack>& tracks,
    const std::string& search_directory,
    const LinkOptions& options
) {
    LinkSummary summary;

    // Count total clips (including nested, excluding already-linked from Python resolution)
    int already_linked = 0;
    CountClipsRecursive(tracks, summary.total_clips, already_linked);

    if (summary.total_clips == 0) {
        Debug::Log("MediaLinker: No clips to link");
        return summary;
    }

    Debug::Log("MediaLinker: " + std::to_string(already_linked) + " clips already linked (from Python resolution)");
    summary.linked_count = already_linked;  // Start with already-linked count

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

    // Helper lambda to link a single clip (returns true if newly linked)
    auto link_single_clip = [&](OTIOClip& clip) -> bool {
        // Skip already-linked clips (e.g., from Python resolution)
        if (clip.is_linked) {
            LinkResult result;
            result.clip_id = clip.id;
            result.clip_name = clip.name;
            result.matched_path = clip.linked_path;
            result.success = true;
            result.match_score = 1.0f;
            summary.results.push_back(result);
            return false;  // Not newly linked
        }

        LinkResult result;
        result.clip_id = clip.id;
        result.clip_name = clip.name;

        std::string matched_path;
        float score = 0.0f;

        // Strategy 1: Try direct file_path if it exists (for AAF with local media)
        if (!clip.file_path.empty()) {
            if (fs::exists(clip.file_path)) {
                matched_path = clip.file_path;
                score = 1.0f;
                Debug::Log("MediaLinker: Direct path match: " + clip.file_path);
            } else {
                std::string filename = fs::path(clip.file_path).filename().string();
                std::string norm_filename = NormalizeClipName(filename, options.fuzzy_match);
                auto it = file_index.find(norm_filename);
                if (it != file_index.end()) {
                    matched_path = it->second;
                    score = 1.0f;
                    Debug::Log("MediaLinker: Found by original filename '" + filename + "' -> " + matched_path);
                }
            }
        }

        // Strategy 2: Try AAF MobID matching (for Avid MXF files)
        if (matched_path.empty() && !clip.aaf_mob_id.empty()) {
            std::string mob_id = clip.aaf_mob_id;
            size_t last_dot = mob_id.rfind('.');
            if (last_dot != std::string::npos && last_dot > 0) {
                size_t second_last = mob_id.rfind('.', last_dot - 1);
                if (second_last != std::string::npos) {
                    std::string hex_segment = mob_id.substr(second_last + 1, last_dot - second_last - 1);
                    std::transform(hex_segment.begin(), hex_segment.end(), hex_segment.begin(), ::tolower);

                    for (const auto& [norm_filename, full_path] : file_index) {
                        if (norm_filename.find(hex_segment) != std::string::npos) {
                            matched_path = full_path;
                            score = 1.0f;
                            Debug::Log("MediaLinker: MobID match! '" + hex_segment + "' found in '" + norm_filename + "'");
                            break;
                        }
                    }
                }
            }
        }

        // Strategy 2b: For graphics/stills, try searching by clip name
        if (matched_path.empty() && !clip.name.empty()) {
            std::string search_name = NormalizeClipName(clip.name, true);
            for (const auto& [norm_filename, full_path] : file_index) {
                if (norm_filename.find(search_name) != std::string::npos) {
                    matched_path = full_path;
                    score = 0.85f;
                    Debug::Log("MediaLinker: Name-in-file match: '" + search_name + "' in '" + norm_filename + "'");
                    break;
                }
            }
        }

        // Strategy 3: Fall back to fuzzy name matching
        if (matched_path.empty()) {
            auto [name_match, name_score] = FindBestMatch(clip.name, file_index, options);
            matched_path = name_match;
            score = name_score;
        }

        if (!matched_path.empty() && score >= 0.7f) {
            clip.linked_path = matched_path;
            clip.is_linked = true;
            result.matched_path = matched_path;
            result.success = true;
            result.match_score = score;

            // Probe video files to get source metadata
            if (IsVideoFile(matched_path)) {
                VideoProbeResult probe = ProbeVideoFile(matched_path);
                if (probe.valid) {
                    clip.source_fps = probe.fps;
                    clip.source_width = probe.width;
                    clip.source_height = probe.height;
                    clip.source_duration = probe.duration;
                    clip.has_audio = probe.has_audio;
                }
            }

            summary.results.push_back(result);
            return true;  // Newly linked
        } else {
            result.success = false;
            summary.results.push_back(result);
            return false;  // Not linked
        }
    };

    // Recursive function to process clips in tracks (including nested)
    int processed = 0;
    std::function<void(std::vector<OTIOTrack>&)> process_tracks = [&](std::vector<OTIOTrack>& track_list) {
        for (auto& track : track_list) {
            for (auto& clip : track.clips) {
                if (clip.is_gap) continue;

                if (clip.is_nested && clip.nested_loaded && !clip.nested_tracks.empty()) {
                    // Recurse into nested tracks
                    process_tracks(clip.nested_tracks);
                } else if (!clip.is_nested) {
                    // Regular clip - try to link it
                    processed++;
                    if (progress_callback_) {
                        progress_callback_(processed, summary.total_clips, "Linking: " + clip.name);
                    }

                    if (link_single_clip(clip)) {
                        summary.linked_count++;
                    } else if (!clip.is_linked) {
                        summary.unlinked_count++;
                    }
                    // Note: already-linked clips were counted in summary.linked_count at start
                }
            }
        }
    };

    process_tracks(tracks);

    Debug::Log("MediaLinker: Linked " + std::to_string(summary.linked_count) + "/" +
               std::to_string(summary.total_clips) + " clips (including " +
               std::to_string(already_linked) + " pre-linked from Python resolution)");

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
            clip.has_audio = probe.has_audio;
            Debug::Log("MediaLinker: Probed video - " + std::to_string(probe.width) + "x" +
                       std::to_string(probe.height) + " @ " + std::to_string(probe.fps) + " fps, " +
                       std::to_string(probe.duration) + "s, has_audio=" + (probe.has_audio ? "yes" : "no"));
        }
    } else {
        // Check if this is an image file that might be part of a sequence
        std::string ext = fs::path(media_path).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

        static const std::set<std::string> image_extensions = {
            ".exr", ".dpx", ".tif", ".tiff", ".png", ".jpg", ".jpeg"
        };

        if (image_extensions.count(ext) > 0) {
            // Try to detect if this file is part of a sequence
            fs::path path(media_path);
            std::string filename = path.stem().string();
            fs::path dir = path.parent_path();

            // Look for frame number pattern at end of filename (e.g., "shot_0001" or "shot.0001")
            std::regex frame_pattern(R"(^(.+?)[._]?(\d{2,8})$)");
            std::smatch match;

            if (std::regex_match(filename, match, frame_pattern)) {
                std::string base_name = match[1].str();
                std::string frame_str = match[2].str();
                int padding = static_cast<int>(frame_str.length());
                int first_frame = std::stoi(frame_str);

                // Build pattern for sequence detection
                std::string pattern_base = base_name;
                if (filename[base_name.length()] == '.') {
                    pattern_base += ".";
                } else if (filename[base_name.length()] == '_') {
                    pattern_base += "_";
                }

                // Scan directory for sequence files
                int min_frame = first_frame;
                int max_frame = first_frame;
                int frame_count = 0;

                try {
                    for (const auto& entry : fs::directory_iterator(dir)) {
                        if (!entry.is_regular_file()) continue;
                        if (entry.path().extension() != path.extension()) continue;

                        std::string other_stem = entry.path().stem().string();
                        std::smatch other_match;
                        if (std::regex_match(other_stem, other_match, frame_pattern)) {
                            std::string other_base = other_match[1].str();
                            if (other_base == base_name) {
                                int frame_num = std::stoi(other_match[2].str());
                                min_frame = std::min(min_frame, frame_num);
                                max_frame = std::max(max_frame, frame_num);
                                frame_count++;
                            }
                        }
                    }
                } catch (...) {
                    // Ignore directory iteration errors
                }

                if (frame_count > 1) {
                    // This is a sequence!
                    clip.is_sequence = true;
                    clip.sequence_directory = dir.string();

                    // Build printf-style pattern
                    char pattern_buf[512];
                    snprintf(pattern_buf, sizeof(pattern_buf), "%s%%0%dd%s",
                             pattern_base.c_str(), padding, ext.c_str());
                    clip.sequence_pattern = pattern_buf;
                    clip.sequence_start_frame = min_frame;
                    clip.sequence_end_frame = max_frame;

                    // Set source metadata for timeline calculations
                    // Use a default fps (will be overridden by timeline fps if not set)
                    double assumed_fps = 24.0;  // Default assumption
                    clip.source_fps = assumed_fps;
                    clip.source_duration = static_cast<double>(frame_count) / assumed_fps;

                    Debug::Log("MediaLinker: Detected image sequence - " + clip.sequence_pattern +
                               " frames " + std::to_string(min_frame) + "-" + std::to_string(max_frame) +
                               " (" + std::to_string(frame_count) + " frames)");
                }
            }
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

    // Recursive helper to process clips including nested
    std::function<void(const std::vector<OTIOTrack>&)> process_tracks;
    process_tracks = [&](const std::vector<OTIOTrack>& track_list) {
        for (const auto& track : track_list) {
            for (const auto& clip : track.clips) {
                if (clip.is_gap) continue;

                if (clip.is_nested && clip.nested_loaded && !clip.nested_tracks.empty()) {
                    // Recurse into nested tracks
                    process_tracks(clip.nested_tracks);
                } else if (!clip.is_nested) {
                    // Regular clip - count it
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
        }
    };

    process_tracks(tracks);
    return summary;
}

} // namespace ump
