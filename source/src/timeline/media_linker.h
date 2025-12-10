#pragma once

#include <string>
#include <vector>
#include <map>
#include <functional>
#include "timeline_view.h"

namespace ump {

// Result of probing a video file for metadata
struct VideoProbeResult {
    bool valid = false;
    double fps = 0.0;
    double duration = 0.0;
    int width = 0;
    int height = 0;
};

// Result of a link attempt for a single clip
struct LinkResult {
    std::string clip_id;
    std::string clip_name;
    std::string matched_path;     // Empty if no match found
    bool success = false;
    float match_score = 0.0f;     // 0.0 to 1.0, higher is better match
};

// Summary of a link operation
struct LinkSummary {
    int total_clips = 0;
    int linked_count = 0;
    int unlinked_count = 0;
    std::vector<LinkResult> results;
};

// Options for the linking operation
struct LinkOptions {
    bool recursive = true;              // Search subdirectories
    int max_depth = 6;                  // Maximum folder depth to search (0 = unlimited)
    bool fuzzy_match = true;            // Use fuzzy matching (ignore suffixes like .new.02)
    bool case_insensitive = true;       // Ignore case when matching
    std::vector<std::string> extensions = {  // File extensions to consider
        ".mov", ".mp4", ".mxf", ".avi", ".mkv",
        ".exr", ".dpx", ".tif", ".tiff", ".png", ".jpg", ".jpeg"
    };
};

class MediaLinker {
public:
    MediaLinker() = default;

    // Static utility methods for video file handling
    static bool IsVideoFile(const std::string& path);
    static VideoProbeResult ProbeVideoFile(const std::string& path);

    // Main linking function - searches directory and links clips
    LinkSummary LinkMediaInDirectory(
        std::vector<OTIOTrack>& tracks,
        const std::string& search_directory,
        const LinkOptions& options = LinkOptions()
    );

    // Link a single clip manually
    bool LinkClip(OTIOClip& clip, const std::string& media_path);

    // Unlink a clip
    void UnlinkClip(OTIOClip& clip);

    // Get all unlinked clips from tracks
    std::vector<OTIOClip*> GetUnlinkedClips(std::vector<OTIOTrack>& tracks);

    // Get link statistics
    LinkSummary GetLinkStatus(const std::vector<OTIOTrack>& tracks);

    // Progress callback for UI updates during search
    using ProgressCallback = std::function<void(int current, int total, const std::string& status)>;
    void SetProgressCallback(ProgressCallback callback) { progress_callback_ = callback; }

    // Normalize clip name for matching (remove suffixes, extensions, etc.)
    static std::string NormalizeClipName(const std::string& name, bool fuzzy);

    // Calculate match score between clip name and filename
    static float CalculateMatchScore(const std::string& clip_name, const std::string& filename, bool fuzzy);

private:
    ProgressCallback progress_callback_;

    // Build index of files in directory
    std::map<std::string, std::string> BuildFileIndex(
        const std::string& directory,
        const LinkOptions& options
    );

    // Find best match for a clip name in the file index
    std::pair<std::string, float> FindBestMatch(
        const std::string& clip_name,
        const std::map<std::string, std::string>& file_index,
        const LinkOptions& options
    );
};

} // namespace ump
