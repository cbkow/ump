#pragma once

#include <string>
#include <set>
#include <vector>

namespace qcview {

// Selection state management for timeline clips
struct TimelineSelection {
    std::set<std::string> selected_clip_ids;
    int primary_track_index = -1;  // Track of the last-selected clip

    // Clear all selection
    void Clear() {
        selected_clip_ids.clear();
        primary_track_index = -1;
    }

    // Select a clip
    // add_to_selection: if true (Ctrl+click), add to existing selection
    //                   if false (normal click), replace selection
    void SelectClip(const std::string& clip_id, int track_index, bool add_to_selection = false) {
        if (!add_to_selection) {
            selected_clip_ids.clear();
        }
        selected_clip_ids.insert(clip_id);
        primary_track_index = track_index;
    }

    // Deselect a specific clip
    void DeselectClip(const std::string& clip_id) {
        selected_clip_ids.erase(clip_id);
        if (selected_clip_ids.empty()) {
            primary_track_index = -1;
        }
    }

    // Toggle selection state
    void ToggleClip(const std::string& clip_id, int track_index) {
        if (IsSelected(clip_id)) {
            DeselectClip(clip_id);
        } else {
            SelectClip(clip_id, track_index, true);
        }
    }

    // Check if a clip is selected
    bool IsSelected(const std::string& clip_id) const {
        return selected_clip_ids.find(clip_id) != selected_clip_ids.end();
    }

    // Check if there's any selection
    bool HasSelection() const {
        return !selected_clip_ids.empty();
    }

    // Get selection count
    size_t GetSelectionCount() const {
        return selected_clip_ids.size();
    }

    // Get first selected clip ID (for single-clip operations)
    std::string GetFirstSelectedClipId() const {
        if (selected_clip_ids.empty()) return "";
        return *selected_clip_ids.begin();
    }
};

// Drag state for clip operations (supports multi-clip dragging)
struct TimelineClipDragState {
    bool active = false;
    std::string clip_id;                    // Primary clip being dragged
    int original_track_index = -1;
    double original_start_time = 0.0;
    double drag_offset = 0.0;               // Offset from clip start to mouse position

    // Multi-clip support: all clips being moved together
    struct DraggedClipInfo {
        std::string clip_id;
        int track_index;
        double original_start_time;
        double duration;
        double offset_from_primary;         // Time offset relative to primary clip
    };
    std::vector<DraggedClipInfo> dragged_clips;  // All clips being moved (includes primary)
};

// Trim state for edge dragging
struct TimelineTrimState {
    bool active = false;
    std::string clip_id;
    int track_index = -1;
    bool is_left_edge = true;  // true = trimming start, false = trimming end
    double original_value = 0.0;  // Original start_time or end_time
    double original_duration = 0.0;
    double original_source_in = 0.0;
};

// Slip state for PLAYLIST mode (Shift+drag on clip body)
struct TimelineSlipState {
    bool active = false;
    std::string clip_id;
    int clip_index = -1;            // Index in tracks[0].clips
    double original_source_in = 0.0;
    double original_source_out = 0.0;
    double source_duration = 0.0;   // Total available source duration
    float start_mouse_x = 0.0f;     // Mouse X when slip started
};

// Reorder state for PLAYLIST mode (drag clip to new position)
struct TimelineReorderState {
    bool active = false;
    std::string clip_id;
    int source_clip_index = -1;     // Original index in tracks[0].clips
    int target_insert_index = -1;   // Where to insert (-1 = invalid, updated during drag)
    double clip_duration = 0.0;     // Duration of the clip being moved
    float start_mouse_x = 0.0f;     // Mouse X when drag started
    float ghost_x = 0.0f;           // Current ghost position (for rendering)
};

// Drop state for media items from project panel
struct TimelineMediaDropState {
    bool active = false;
    std::string media_id;           // ID of MediaItem being dragged
    int target_track_index = -1;    // Track mouse is over
    double drop_time = 0.0;         // Timeline position for drop
    double preview_duration = 0.0;  // Duration from MediaItem
    std::string preview_name;       // Name for ghost label
};

} // namespace qcview
