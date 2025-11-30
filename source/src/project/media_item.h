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

        // For image sequence items
        std::string sequence_pattern;  // e.g., "shot_%04d.exr"
        std::string ffmpeg_pattern;    // Full path pattern for FFmpeg cache e.g., "/path/shot_%04d.exr"
        int frame_count = 0;          // Number of frames in sequence
        int start_frame = 1;          // First frame number
        int end_frame = 1;            // Last frame number
        double frame_rate = 24.0;     // Frame rate for sequence
        PipelineMode pipeline_mode = PipelineMode::NORMAL;  // Auto-detected bit depth/precision

        // Cached dimensions from first frame (for instant loading without I/O)
        int sequence_width = 0;       // For IMAGE_SEQUENCE and EXR_SEQUENCE types
        int sequence_height = 0;      // Avoids async MPV discovery and file I/O on load

        // EXR-specific fields
        std::string exr_layer;        // Selected EXR layer (e.g., "beauty", "diffuse")
        std::string exr_layer_display;// Display name for EXR layer

        // In/Out points for range-constrained playback and transcode
        double in_point = -1.0;       // In point timestamp in seconds (-1 = not set)
        double out_point = -1.0;      // Out point timestamp in seconds (-1 = not set)

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
        };
        std::vector<CachedClipLink> clip_links;  // Saved media links for clips

        // Cached track metadata (for persistent track structure in timelines)
        struct CachedTrackMetadata {
            std::string id;
            std::string name;
            bool is_video = true;
            bool visible = true;
            bool muted = false;
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