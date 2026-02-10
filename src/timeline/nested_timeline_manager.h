#pragma once

#include <vector>
#include <string>
#include "timeline_types.h"

namespace ump {

/**
 * TimelineContext - Stores state for a timeline level (root or nested)
 *
 * When entering a nested timeline, the current context is pushed onto a stack
 * so it can be restored when exiting.
 */
struct TimelineContext {
    std::vector<OTIOTrack> tracks;       // Tracks at this level
    std::string name;                    // Timeline/sequence name (for breadcrumb)
    double duration = 0.0;               // Total duration of this timeline
    double fps = 24.0;                   // Frame rate
    std::string parent_clip_id;          // ID of the clip that was entered (empty for root)
    double playhead_position = 0.0;      // Playhead position when we entered nested
    float zoom_level = 50.0f;            // Zoom level when we entered
    float scroll_offset = 0.0f;          // Scroll offset when we entered
};

/**
 * NestedTimelineManager - Manages navigation into/out of nested timeline compositions
 *
 * This class maintains a stack of timeline contexts, allowing users to:
 * - Enter a nested composition (double-click on a nested clip)
 * - Exit to parent timeline (breadcrumb click or back button)
 * - Navigate back to root timeline
 *
 * The stack stores the parent timeline's state so it can be restored exactly
 * when exiting the nested timeline.
 *
 * Usage:
 *   NestedTimelineManager nest_manager;
 *
 *   // Enter nested clip
 *   if (nest_manager.EnterNest(clip, current_tracks, name, fps, playhead)) {
 *       // Load nested tracks into timeline view
 *   }
 *
 *   // Exit to parent
 *   if (nest_manager.ExitNest(out_tracks, out_name, out_fps, out_playhead)) {
 *       // Restore parent tracks to timeline view
 *   }
 */
class NestedTimelineManager {
public:
    NestedTimelineManager() = default;
    ~NestedTimelineManager() = default;

    /**
     * Enter a nested timeline composition
     *
     * @param clip The nested clip to enter (must have is_nested=true)
     * @param current_tracks Current tracks (will be saved to stack)
     * @param timeline_name Current timeline name
     * @param fps Current frame rate
     * @param playhead Current playhead position
     * @param zoom Current zoom level
     * @param scroll Current scroll offset
     * @return true if entered successfully, false if clip is not nested
     */
    bool EnterNest(OTIOClip& clip,
                   const std::vector<OTIOTrack>& current_tracks,
                   const std::string& timeline_name,
                   double fps,
                   double playhead,
                   float zoom = 50.0f,
                   float scroll = 0.0f);

    /**
     * Exit current nested timeline and restore parent
     *
     * @param out_tracks Output: Tracks from parent timeline
     * @param out_name Output: Name of parent timeline
     * @param out_fps Output: Frame rate of parent timeline
     * @param out_playhead Output: Playhead position to restore
     * @param out_zoom Output: Zoom level to restore
     * @param out_scroll Output: Scroll offset to restore
     * @return true if exited successfully, false if already at root
     */
    bool ExitNest(std::vector<OTIOTrack>& out_tracks,
                  std::string& out_name,
                  double& out_fps,
                  double& out_playhead,
                  float& out_zoom,
                  float& out_scroll);

    /**
     * Exit all nested timelines and return to root
     */
    void ExitToRoot();

    /**
     * Get current nesting depth
     * @return 0 if at root, 1+ if in nested timeline(s)
     */
    int GetDepth() const { return static_cast<int>(context_stack_.size()); }

    /**
     * Check if currently viewing a nested timeline
     */
    bool IsNested() const { return !context_stack_.empty(); }

    /**
     * Get breadcrumb path for navigation UI
     * @return Vector of timeline names from root to current
     */
    std::vector<std::string> GetBreadcrumbPath() const;

    /**
     * Get the root timeline context (for full restoration)
     * @return Pointer to root context, or nullptr if stack is empty
     */
    const TimelineContext* GetRootContext() const;

    /**
     * Get context at specific depth (0 = first parent saved)
     */
    const TimelineContext* GetContextAt(int depth) const;

    /**
     * Get the clip ID of the currently entered nested clip
     * @return Clip ID or empty string if at root
     */
    std::string GetCurrentNestedClipId() const;

private:
    std::vector<TimelineContext> context_stack_;
    std::string current_nested_clip_id_;
    std::string current_timeline_name_;
};

} // namespace ump
