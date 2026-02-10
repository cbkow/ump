#pragma once

#include <string>

namespace ump {

//=============================================================================
// PlaylistClip - Simplified clip structure for unified playlist timeline
//
// Each clip represents either:
// - A media clip referencing a MediaItem from the project
// - A gap (empty space between clips)
//
// Clips are ordered sequentially on a single track with no overlapping.
// Ripple editing is enforced - edits push downstream clips automatically.
//=============================================================================

struct PlaylistClip {
    std::string id;               // Unique clip identifier (UUID)
    std::string media_item_id;    // Reference to MediaItem by ID (empty for gaps)

    // Timeline position and duration
    double timeline_start = 0.0;  // Position on playlist timeline (seconds)
    double duration = 0.0;        // Duration on timeline after trim (seconds)

    // Source trim points (for media clips only)
    double source_in = 0.0;       // Trim in point in source media (seconds)
    double source_out = 0.0;      // Trim out point in source media (seconds)

    // Gap flag
    bool is_gap = false;          // True for empty space between clips

    // Display name (cached from MediaItem or "Gap")
    std::string name;

    // Source media metadata (cached from MediaItem when linked)
    std::string source_path;      // Full path to source media
    double source_fps = 0.0;      // Native frame rate of source media
    double source_duration = 0.0; // Full duration of source media (for trim limits)
    int source_width = 0;         // Source dimensions
    int source_height = 0;
    bool has_audio = false;       // True if source media contains audio

    // For image sequences
    bool is_sequence = false;     // True if this is an image sequence
    std::string sequence_directory;
    std::string sequence_pattern;
    int sequence_start_frame = 1;
    int sequence_end_frame = 1;
    std::string sequence_exr_layer;
    std::string sequence_audio_file;  // Companion audio for image sequence

    //=========================================================================
    // Helper Methods
    //=========================================================================

    // Get timeline end time
    double GetTimelineEnd() const {
        return timeline_start + duration;
    }

    // Get source frame for a given timeline frame
    int GetSourceFrame(double timeline_time, double timeline_fps) const {
        if (is_gap) return -1;

        // Calculate offset within clip
        double clip_offset = timeline_time - timeline_start;
        if (clip_offset < 0 || clip_offset >= duration) return -1;

        // Convert to source time
        double source_time = source_in + clip_offset;

        // Convert to frame number
        double fps = source_fps > 0 ? source_fps : timeline_fps;
        return static_cast<int>(source_time * fps);
    }

    // Check if timeline time falls within this clip
    bool ContainsTime(double timeline_time) const {
        return timeline_time >= timeline_start && timeline_time < GetTimelineEnd();
    }

    // Create a gap clip
    static PlaylistClip CreateGap(double start, double duration) {
        PlaylistClip gap;
        gap.id = "gap_" + std::to_string(static_cast<int64_t>(start * 1000));
        gap.timeline_start = start;
        gap.duration = duration;
        gap.is_gap = true;
        gap.name = "Gap";
        return gap;
    }
};

} // namespace ump
