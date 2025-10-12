#pragma once
#include <string>
#include <vector>
#include <algorithm>

namespace ump {
    enum class MediaType {
        VIDEO,
        AUDIO,
        IMAGE,
        IMAGE_SEQUENCE,
        EXR_SEQUENCE,
        SEQUENCE
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

        // EXR-specific fields
        std::string exr_layer;        // Selected EXR layer (e.g., "beauty", "diffuse")
        std::string exr_layer_display;// Display name for EXR layer
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