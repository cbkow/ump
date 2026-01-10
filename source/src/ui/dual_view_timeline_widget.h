#pragma once

#include <functional>
#include <vector>
#include <imgui.h>
#include "../player/dual_view_types.h"

namespace ump {

//=============================================================================
// DualViewTimelineWidget
//
// Two-track timeline widget for dual view mode editing.
// Provides visual editing of Left/Right video clips with trim and slide support.
//=============================================================================

class DualViewTimelineWidget {
public:
    DualViewTimelineWidget();
    ~DualViewTimelineWidget() = default;

    //=========================================================================
    // Rendering
    //=========================================================================

    // Render the two-track timeline
    // Returns true if any edit occurred (caller should update VideoPlayer)
    bool Render(DualViewTimeline& timeline, double current_time,
                float available_width, ImFont* font_regular = nullptr);

    //=========================================================================
    // Callbacks
    //=========================================================================

    // Called when trim changes: (is_left_track, new_source_in, new_source_out)
    void SetOnTrimChanged(std::function<void(bool, double, double)> callback) {
        on_trim_changed_ = callback;
    }

    // Called when slide/position changes: (is_left_track, new_offset)
    void SetOnSlideChanged(std::function<void(bool, double)> callback) {
        on_slide_changed_ = callback;
    }

    // Called when edit starts (to pause playback): (is_starting)
    void SetOnEditStateChanged(std::function<void(bool)> callback) {
        on_edit_state_changed_ = callback;
    }

    //=========================================================================
    // Configuration
    //=========================================================================

    void SetPixelsPerSecond(float pps) { pixels_per_second_ = pps; }
    float GetPixelsPerSecond() const { return pixels_per_second_; }

    void SetSnapEnabled(bool enabled) { snap_enabled_ = enabled; }
    bool IsSnapEnabled() const { return snap_enabled_; }

    void SetSnapTolerance(float pixels) { snap_tolerance_ = pixels; }

    //=========================================================================
    // State Queries
    //=========================================================================

    bool IsEditing() const { return trim_state_.active || slide_state_.active; }
    bool IsTrimming() const { return trim_state_.active; }
    bool IsSliding() const { return slide_state_.active; }

private:
    //=========================================================================
    // Internal State
    //=========================================================================

    // Trim state
    struct TrimState {
        bool active = false;
        bool is_left_track = true;
        bool is_left_edge = true;
        double original_in = 0.0;
        double original_out = 0.0;
        double original_offset = 0.0;
    };
    TrimState trim_state_;

    // Slide state
    struct SlideState {
        bool active = false;
        bool is_left_track = true;
        double original_offset = 0.0;
        double drag_start_time = 0.0;
    };
    SlideState slide_state_;

    //=========================================================================
    // Layout Constants
    //=========================================================================

    static constexpr float TRACK_HEIGHT = 36.0f;
    static constexpr float TRACK_LABEL_WIDTH = 50.0f;
    static constexpr float TRACK_PADDING = 2.0f;
    static constexpr float EDGE_HANDLE_WIDTH = 8.0f;
    static constexpr float PLAYHEAD_WIDTH = 2.0f;

    //=========================================================================
    // Visual Settings
    //=========================================================================

    float pixels_per_second_ = 50.0f;
    bool snap_enabled_ = true;
    float snap_tolerance_ = 10.0f;  // pixels

    //=========================================================================
    // Callbacks
    //=========================================================================

    std::function<void(bool, double, double)> on_trim_changed_;
    std::function<void(bool, double)> on_slide_changed_;
    std::function<void(bool)> on_edit_state_changed_;

    //=========================================================================
    // Helper Methods
    //=========================================================================

    // Collect snap points from the timeline
    std::vector<double> CollectSnapPoints(const DualViewTimeline& timeline, double current_time);

    // Calculate snapped position if within tolerance
    double CalculateSnappedPosition(double pos, const std::vector<double>& snap_points, bool* snapped = nullptr);

    // Render a single track
    void RenderTrack(DualViewClip& clip, bool is_left_track, const DualViewTimeline& timeline,
                     double current_time, ImVec2 track_pos, float track_width,
                     ImDrawList* draw_list, ImFont* font_regular);

    // Handle mouse interaction for a clip
    void HandleClipInteraction(DualViewClip& clip, bool is_left_track, const DualViewTimeline& timeline,
                               ImVec2 clip_min, ImVec2 clip_max, float clip_x_offset);

    // Convert time to pixel position
    float TimeToPixel(double time) const {
        return static_cast<float>(time) * pixels_per_second_;
    }

    // Convert pixel to time
    double PixelToTime(float pixel) const {
        return static_cast<double>(pixel) / pixels_per_second_;
    }
};

} // namespace ump
