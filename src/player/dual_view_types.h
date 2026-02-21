#pragma once

#include <string>

namespace ump {

//=============================================================================
// DualViewClip - Single video clip in dual view mode
//=============================================================================

struct DualViewClip {
    std::string source_path;
    double source_in = 0.0;        // Trim in point (seconds)
    double source_out = 0.0;       // Trim out point (seconds)
    double duration = 0.0;         // Effective duration after trim (source_out - source_in)
    double position_offset = 0.0;  // Horizontal offset for slide adjustment (seconds)

    // Metadata (populated on load)
    double source_duration = 0.0;  // Original source duration
    int width = 0;
    int height = 0;
    double fps = 0.0;

    // Check if clip is loaded
    bool IsLoaded() const { return !source_path.empty() && source_duration > 0.0; }

    // Get effective duration after trim
    double GetEffectiveDuration() const {
        if (source_out > source_in) {
            return source_out - source_in;
        }
        return source_duration;
    }

    //=========================================================================
    // Virtual Timeline Methods
    //=========================================================================

    // Get where this clip starts on the virtual timeline
    double GetTimelineStart() const {
        return position_offset;
    }

    // Get where this clip ends on the virtual timeline
    double GetTimelineEnd() const {
        return position_offset + GetEffectiveDuration();
    }

    // Check if a timeline position is within this clip's range
    bool ContainsPosition(double timeline_pos) const {
        return timeline_pos >= GetTimelineStart() &&
               timeline_pos < GetTimelineEnd();
    }

    // Convert a virtual timeline position to source position
    // Returns: source time within clip, or special values for gaps:
    //   - Returns -1.0 if timeline_pos is before clip starts (gap before)
    //   - Returns source_duration + 1.0 if timeline_pos is after clip ends (gap after)
    double TimelineToSource(double timeline_pos) const {
        if (timeline_pos < GetTimelineStart()) {
            return -1.0;  // Gap before clip
        }
        if (timeline_pos >= GetTimelineEnd()) {
            return source_duration + 1.0;  // Gap after clip
        }
        // Within clip range - calculate source position
        return source_in + (timeline_pos - position_offset);
    }

    // Reset to defaults
    void Reset() {
        source_path.clear();
        source_in = 0.0;
        source_out = 0.0;
        duration = 0.0;
        position_offset = 0.0;
        source_duration = 0.0;
        width = 0;
        height = 0;
        fps = 0.0;
    }
};

//=============================================================================
// DualViewTimeline - Two-layer timeline for dual view mode
//=============================================================================

struct DualViewTimeline {
    DualViewClip left;
    DualViewClip right;
    double timeline_duration = 0.0;  // Max of both clips' effective durations

    // Update timeline duration based on clips
    void UpdateDuration() {
        timeline_duration = GetVirtualDuration();
    }

    // Get the virtual timeline duration
    // In comparison mode, the left/primary video is LOCKED - it defines the timeline.
    // The right video can be trimmed/offset but the timeline is clamped to left's duration.
    // This ensures the decoder can handle seeking (no virtual extension past source duration).
    double GetVirtualDuration() const {
        // Left clip defines the timeline - it's locked with no trim/offset
        if (left.IsLoaded()) {
            return left.source_duration;  // Full source duration (left is never trimmed)
        }
        // Fallback if only right is loaded
        if (right.IsLoaded()) {
            return right.GetTimelineEnd();
        }
        return 0.0;
    }

    // Get the earliest start (usually 0, but supports negative offsets if needed)
    double GetVirtualStart() const {
        double left_start = left.IsLoaded() ? left.GetTimelineStart() : 0.0;
        double right_start = right.IsLoaded() ? right.GetTimelineStart() : 0.0;
        return (left_start < right_start) ? left_start : right_start;
    }

    // Check if either clip is loaded
    bool HasAnyClip() const { return left.IsLoaded() || right.IsLoaded(); }

    // Check if both clips are loaded
    bool HasBothClips() const { return left.IsLoaded() && right.IsLoaded(); }

    // Reset both clips
    void Reset() {
        left.Reset();
        right.Reset();
        timeline_duration = 0.0;
    }
};

} // namespace ump
