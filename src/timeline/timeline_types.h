#pragma once

#include <string>
#include <vector>

namespace qcview {

// Timeline source mode - determines editing restrictions and UI behavior
enum class TimelineSourceMode {
    IMAGE_SEQUENCE,    // Single image sequence (locked video track, editable audio)
    VIDEO_FILE,        // Single video file (locked video track, audio track if present)
    AUDIO_FILE,        // Single audio file (locked audio track only, no video)
    DUAL_VIEW,         // Side-by-side comparison (LEFT/RIGHT video tracks)
    PLAYLIST,          // Unified playlist - single track, ripple editing, native decoders
};

// Represents a single clip segment on a timeline track
// Named OTIOClip to avoid collision with qcview::TimelineClip in media_item.h
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
    std::string aaf_mob_id;       // AAF MobID for MXF matching (Avid media)

    // Source media metadata (populated when linked/probed)
    double source_fps = 0.0;      // Native frame rate of source media
    int source_width = 0;         // Source dimensions
    int source_height = 0;
    double source_duration = 0.0; // Full duration of source media (for trim limits)
    bool has_audio = false;       // True if source media contains audio track

    // Image sequence metadata (for IMAGE_SEQUENCE/EXR_SEQUENCE clips)
    bool is_sequence = false;          // True if this is an image sequence
    std::string sequence_directory;    // Base directory for sequence files
    std::string sequence_pattern;      // Printf pattern (e.g., "shot_%04d.exr")
    int sequence_start_frame = 1;      // First frame number in sequence
    int sequence_end_frame = 1;        // Last frame number in sequence
    std::string sequence_exr_layer;    // EXR layer name (for multi-layer EXR)

    // Clip-level audio control
    bool audio_muted = false;     // Per-clip audio mute (speaker icon on clip)

    // Nested timeline support (for AAF/XML nested sequences, compound clips)
    bool is_nested = false;                    // True if this clip is a nested composition
    std::string nested_timeline_json;          // Stored OTIO JSON for lazy parsing
    std::vector<struct OTIOTrack> nested_tracks; // Parsed tracks (populated on enter)
    double nested_fps = 0.0;                   // Frame rate of nested composition
    std::string nested_name;                   // Display name for breadcrumb navigation
    bool nested_loaded = false;                // True if nested_tracks has been parsed
};

// Represents a single video or audio track
struct OTIOTrack {
    std::string id;
    std::string name;
    std::vector<OTIOClip> clips;
    bool is_video = true;          // true=video, false=audio
    bool visible = true;           // Eye icon state (for video tracks - controls visibility)
    bool muted = false;            // Mute icon state (for audio tracks)
    bool audio_muted = false;      // Audio mute for video tracks (video still displays, audio silent)
    bool locked = false;           // Track locked - no editing allowed (for IMAGE_SEQUENCE mode)
    int z_index = 0;               // Stacking order (higher = on top)
};

} // namespace qcview
