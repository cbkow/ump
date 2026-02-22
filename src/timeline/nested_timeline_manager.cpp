#include "nested_timeline_manager.h"
#include "../utils/debug_utils.h"

namespace qcview {

bool NestedTimelineManager::EnterNest(OTIOClip& clip,
                                       const std::vector<OTIOTrack>& current_tracks,
                                       const std::string& timeline_name,
                                       double fps,
                                       double playhead,
                                       float zoom,
                                       float scroll) {
    if (!clip.is_nested) {
        Debug::Log("NestedTimelineManager: Cannot enter non-nested clip: " + clip.name);
        return false;
    }

    // Save current context to stack
    TimelineContext context;
    context.tracks = current_tracks;
    context.name = timeline_name;
    context.fps = fps;
    context.playhead_position = playhead;
    context.zoom_level = zoom;
    context.scroll_offset = scroll;
    context.parent_clip_id = current_nested_clip_id_;  // Track nesting chain

    // Calculate duration from tracks
    double max_end = 0.0;
    for (const auto& track : current_tracks) {
        for (const auto& c : track.clips) {
            double end = c.start_time + c.duration;
            if (end > max_end) max_end = end;
        }
    }
    context.duration = max_end;

    context_stack_.push_back(std::move(context));

    // Update current state
    current_nested_clip_id_ = clip.id;
    current_timeline_name_ = clip.nested_name.empty() ? clip.name : clip.nested_name;

    Debug::Log("NestedTimelineManager: Entered nested timeline '" + current_timeline_name_ +
               "' (depth: " + std::to_string(GetDepth()) + ")");

    return true;
}

bool NestedTimelineManager::ExitNest(std::vector<OTIOTrack>& out_tracks,
                                      std::string& out_name,
                                      double& out_fps,
                                      double& out_playhead,
                                      float& out_zoom,
                                      float& out_scroll) {
    if (context_stack_.empty()) {
        Debug::Log("NestedTimelineManager: Already at root, cannot exit");
        return false;
    }

    // Pop and restore the parent context
    TimelineContext& parent = context_stack_.back();

    out_tracks = std::move(parent.tracks);
    out_name = parent.name;
    out_fps = parent.fps;
    out_playhead = parent.playhead_position;
    out_zoom = parent.zoom_level;
    out_scroll = parent.scroll_offset;

    // Update current state
    current_nested_clip_id_ = parent.parent_clip_id;
    current_timeline_name_ = parent.name;

    context_stack_.pop_back();

    Debug::Log("NestedTimelineManager: Exited to '" + out_name +
               "' (depth: " + std::to_string(GetDepth()) + ")");

    return true;
}

void NestedTimelineManager::ExitToRoot() {
    if (context_stack_.empty()) return;

    Debug::Log("NestedTimelineManager: Exiting to root from depth " +
               std::to_string(GetDepth()));

    context_stack_.clear();
    current_nested_clip_id_.clear();
    current_timeline_name_.clear();
}

std::vector<std::string> NestedTimelineManager::GetBreadcrumbPath() const {
    std::vector<std::string> path;

    // Add all parent names from the stack
    for (const auto& ctx : context_stack_) {
        path.push_back(ctx.name);
    }

    // Add current timeline name if we're in a nested timeline
    if (!current_timeline_name_.empty() && IsNested()) {
        path.push_back(current_timeline_name_);
    }

    return path;
}

const TimelineContext* NestedTimelineManager::GetRootContext() const {
    if (context_stack_.empty()) return nullptr;
    return &context_stack_.front();
}

const TimelineContext* NestedTimelineManager::GetContextAt(int depth) const {
    if (depth < 0 || depth >= static_cast<int>(context_stack_.size())) {
        return nullptr;
    }
    return &context_stack_[depth];
}

std::string NestedTimelineManager::GetCurrentNestedClipId() const {
    return current_nested_clip_id_;
}

} // namespace qcview
