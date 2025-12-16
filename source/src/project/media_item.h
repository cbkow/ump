#pragma once
#include <string>
#include <vector>
#include <algorithm>
#include "../player/pipeline_mode.h"
#include "../timeline/timeline_types.h"

namespace ump {
    enum class MediaType {
        VIDEO,
        AUDIO,
        IMAGE,
        IMAGE_SEQUENCE,
        EXR_SEQUENCE,
        SEQUENCE,
        TIMELINE        // OTIO/EDL multi-track timeline
    };

    // Image sequence specific data (for IMAGE_SEQUENCE and EXR_SEQUENCE types)
    // This struct holds all state needed to properly reinitialize playback when switching media
    struct ImageSequenceData {
        // Core sequence info
        std::string pattern;           // e.g., "shot_%04d.exr"
        std::string ffmpeg_pattern;    // Full path pattern for FFmpeg
        std::string directory;         // Directory containing sequence files

        // Frame range
        int frame_count = 0;
        int start_frame = 1;
        int end_frame = 1;

        // Timing (critical for playback)
        double frame_rate = 24.0;
        double duration = 0.0;         // Calculated: frame_count / frame_rate

        // Dimensions (cached from first frame for instant loading)
        int width = 0;
        int height = 0;

        // Pipeline/format
        PipelineMode pipeline_mode = PipelineMode::NORMAL;
        std::string format;            // "EXR", "TIFF", "PNG", "JPEG", etc.

        // EXR-specific
        std::string layer;             // Selected EXR layer (e.g., "beauty", "diffuse")
        std::string layer_display;     // Display name for EXR layer

        // Helper to check if data is valid/populated
        bool IsValid() const {
            return frame_count > 0 && frame_rate > 0 && duration > 0;
        }

        // Calculate duration from frame count and rate
        void CalculateDuration() {
            if (frame_count > 0 && frame_rate > 0) {
                duration = static_cast<double>(frame_count) / frame_rate;
            }
        }
    };

    struct MediaItem {
        std::string id;
        std::string name;
        std::string path;
        MediaType type = MediaType::VIDEO;
        double duration = 0.0;
        std::string thumbnail_path;

        // For sequence items (EDL timelines)
        std::string sequence_id;
        int clip_count = 0;
        bool is_active = false;

        // Image sequence data (for IMAGE_SEQUENCE and EXR_SEQUENCE types)
        // This is the primary source of truth for sequence playback parameters
        ImageSequenceData image_seq;

        // LEGACY FIELDS - kept for backward compatibility with existing projects
        // New code should use image_seq.* instead
        std::string sequence_pattern;  // LEGACY: use image_seq.pattern
        std::string ffmpeg_pattern;    // LEGACY: use image_seq.ffmpeg_pattern
        int frame_count = 0;          // LEGACY: use image_seq.frame_count
        int start_frame = 1;          // LEGACY: use image_seq.start_frame
        int end_frame = 1;            // LEGACY: use image_seq.end_frame
        double frame_rate = 24.0;     // LEGACY: use image_seq.frame_rate
        PipelineMode pipeline_mode = PipelineMode::NORMAL;  // LEGACY: use image_seq.pipeline_mode

        // Cached dimensions from first frame (for instant loading without I/O)
        int sequence_width = 0;       // LEGACY: use image_seq.width
        int sequence_height = 0;      // LEGACY: use image_seq.height

        // EXR-specific fields
        std::string exr_layer;        // LEGACY: use image_seq.layer
        std::string exr_layer_display;// LEGACY: use image_seq.layer_display

        // In/Out points for range-constrained playback and transcode
        double in_point = -1.0;       // In point timestamp in seconds (-1 = not set)
        double out_point = -1.0;      // Out point timestamp in seconds (-1 = not set)

        // View state for restoring zoom/pan/playhead when switching media
        struct ViewState {
            float zoom_level = 1.0f;       // Standard timeline zoom (1.0 = fit to width)
            float scroll_offset = 0.0f;    // Standard timeline pan offset
            double playhead_position = 0.0; // Playhead time in seconds

            // OTIO timeline specific (only used for MediaType::TIMELINE)
            float timeline_zoom = 50.0f;   // Pixels per second (default: 50)
            float timeline_scroll = 0.0f;  // Horizontal scroll offset
            double timeline_playhead = 0.0; // OTIO timeline playhead position

            // Timeline In/Out points (per-timeline range markers)
            double timeline_in_point = -1.0;   // -1 = not set
            double timeline_out_point = -1.0;  // -1 = not set
        };
        ViewState view_state;

        // Timeline-specific fields (for MediaType::TIMELINE)
        std::string timeline_id;      // Reference to timeline data
        std::string timeline_format;  // "otio", "edl", "aaf", "xml"
        int video_track_count = 0;    // Number of video tracks
        int audio_track_count = 0;    // Number of audio tracks
        int timeline_width = 1920;    // Timeline resolution width (for dummy video recreation)
        int timeline_height = 1080;   // Timeline resolution height (for dummy video recreation)

        // Cached clip links (for persistent media linking in timelines)
        struct CachedClipLink {
            std::string clip_id;
            std::string linked_path;
            double source_fps = 0.0;
            int source_width = 0;
            int source_height = 0;
            double source_duration = 0.0;
            bool has_audio = false;
            bool audio_muted = false;  // Per-clip audio mute state
        };
        std::vector<CachedClipLink> clip_links;  // Saved media links for clips

        // Cached track metadata (for persistent track structure in timelines)
        struct CachedTrackMetadata {
            std::string id;
            std::string name;
            bool is_video = true;
            bool visible = true;
            bool muted = false;
            bool audio_muted = false;  // Audio mute for video tracks (video displays, audio silent)
            int z_index = 0;
        };
        std::vector<CachedTrackMetadata> track_metadata;  // Saved track structure

        // Cached edited timeline tracks (for persistent edits across timeline switching)
        // When user makes edits (move, trim, cut, delete), the full track structure is saved here
        // This allows switching between timelines without losing edits
        std::vector<OTIOTrack> cached_tracks;
        bool has_cached_edits = false;  // True if tracks have been edited from original EDL
    };

    struct ProjectBin {
        std::string name;
        std::vector<MediaItem> items;
        bool is_open = true;
    };

    // Timeline clip structure
    struct TimelineClip {
        std::string id;
        std::string media_id;  
        std::string name;
        std::string file_path;
        double start_time = 0.0;    
        double duration = 0.0;    
        double source_in = 0.0;   
        double source_out = 0.0;  
        std::string track_type; 
    };

    // Sequence structure
    struct Sequence {
        std::string id;
        std::string name;
        std::string base_name;
        std::vector<TimelineClip> clips;
        double duration = 0.0;
        double frame_rate = 24.0;

        std::vector<TimelineClip> GetAllClipsSorted() const {
            std::vector<TimelineClip> sorted_clips = clips;
            std::sort(sorted_clips.begin(), sorted_clips.end(),
                [](const TimelineClip& a, const TimelineClip& b) {
                    return a.start_time < b.start_time;
                });
            return sorted_clips;
        }

        void UpdateDuration() {
            double max_end_time = 0.0;
            for (const auto& clip : clips) {
                double end_time = clip.start_time + clip.duration;
                if (end_time > max_end_time) {
                    max_end_time = end_time;
                }
            }
            duration = max_end_time;
        }
    };
}