#pragma once

#include <string>
#include <vector>

namespace ump {

// Represents a single clip segment on a timeline track
// Named OTIOClip to avoid collision with ump::TimelineClip in media_item.h
struct OTIOClip {
    std::string id;
    std::string name;
    std::string file_path;        // Original path from EDL (may be just filename)
    double start_time = 0.0;      // Timeline position (global)
    double duration = 0.0;
    double source_in = 0.0;       // Trim: source media in-point
    double source_out = 0.0;      // Trim: source media out-point
    bool is_gap = false;          // True for empty space

    // Transition info (if clip has fade)
    bool has_fade_in = false;
    bool has_fade_out = false;
    double fade_in_duration = 0.0;
    double fade_out_duration = 0.0;

    // Media linking
    std::string linked_path;      // Resolved full path to media file
    bool is_linked = false;       // True if media file found and linked

    // Source media metadata (populated when linked/probed)
    double source_fps = 0.0;      // Native frame rate of source media
    int source_width = 0;         // Source dimensions
    int source_height = 0;
    double source_duration = 0.0; // Full duration of source media (for trim limits)
};

// Represents a single video or audio track
struct OTIOTrack {
    std::string id;
    std::string name;
    std::vector<OTIOClip> clips;
    bool is_video = true;          // true=video, false=audio
    bool visible = true;           // Eye icon state (for video)
    bool muted = false;            // Mute icon state (for audio)
    int z_index = 0;               // Stacking order (higher = on top)
};

} // namespace ump
