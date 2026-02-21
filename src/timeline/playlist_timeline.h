#pragma once

#include <string>
#include <vector>
#include <functional>
#include <memory>

#include "playlist_clip.h"

namespace ump {

// Forward declarations
struct MediaItem;
class ProjectManager;

//=============================================================================
// PlaylistTimeline - Unified single-track playlist with ripple editing
//
// Features:
// - Single media track (no V1/V2/A1/A2 multi-track complexity)
// - Clips cannot overlap (ripple editing enforced)
// - Fixed gaps between clips (default 10 frames)
// - Each clip references a MediaItem from the project
// - Native decoder per clip (D3D11 for video, native loaders for sequences)
//
// Ripple Editing Behavior:
// - Move clip: pushes downstream clips
// - Delete clip: ripple collapse (fills gap, shifts left)
// - Trim: adjusts gaps as needed
// - Cut: splits clip at playhead, inserts gap
// - Slip: changes source in/out without affecting timeline position
// - Reorder: drag to new position, ripple adjustment
//=============================================================================

class PlaylistTimeline {
public:
    PlaylistTimeline();
    ~PlaylistTimeline();

    //=========================================================================
    // Configuration
    //=========================================================================

    void SetFrameRate(double fps) { frame_rate_ = fps; }
    double GetFrameRate() const { return frame_rate_; }

    void SetDefaultGapFrames(int frames) { default_gap_frames_ = frames; }
    int GetDefaultGapFrames() const { return default_gap_frames_; }

    // Gap duration in seconds
    double GetGapDuration() const {
        return frame_rate_ > 0 ? default_gap_frames_ / frame_rate_ : 0.0;
    }

    //=========================================================================
    // Clip Access
    //=========================================================================

    const std::vector<PlaylistClip>& GetClips() const { return clips_; }
    int GetClipCount() const { return static_cast<int>(clips_.size()); }
    bool IsEmpty() const { return clips_.empty(); }

    // Get clip by index
    PlaylistClip* GetClip(int index);
    const PlaylistClip* GetClip(int index) const;

    // Get clip by ID
    PlaylistClip* GetClipById(const std::string& id);
    const PlaylistClip* GetClipById(const std::string& id) const;

    // Get clip at timeline time
    PlaylistClip* GetClipAtTime(double time);
    const PlaylistClip* GetClipAtTime(double time) const;

    // Get clip index at timeline time (-1 if in gap or out of bounds)
    int GetClipIndexAtTime(double time) const;

    // Get next clip after given time (nullptr if at end)
    PlaylistClip* GetNextClip(double time);
    const PlaylistClip* GetNextClip(double time) const;

    //=========================================================================
    // Timeline Properties
    //=========================================================================

    // Total duration including all clips and gaps
    double GetDuration() const;

    // Total duration in frames
    int GetTotalFrames() const;

    // Get canvas dimensions (max of all clips)
    int GetCanvasWidth() const { return canvas_width_; }
    int GetCanvasHeight() const { return canvas_height_; }

    //=========================================================================
    // Editing Operations (All use ripple behavior)
    //=========================================================================

    // Insert clip from MediaItem at index position
    // If index < 0 or >= clip count, appends to end
    // Returns index of inserted clip, or -1 on failure
    int InsertClip(int index, const MediaItem& item);

    // Insert clip from MediaItem after given time position
    // Returns index of inserted clip, or -1 on failure
    int InsertClipAfterTime(double time, const MediaItem& item);

    // Append clip to end of timeline
    int AppendClip(const MediaItem& item);

    // Delete clip at index (ripple collapse - downstream clips shift left)
    bool DeleteClip(int index);

    // Move clip from one index to another (ripple reorder)
    bool MoveClip(int from_index, int to_index);

    // Trim clip in/out points
    // Adjusts timeline positions of downstream clips as needed
    bool TrimClipStart(int index, double new_source_in);
    bool TrimClipEnd(int index, double new_source_out);

    // Slip edit - changes source in/out without affecting timeline position
    // Keeps the same timeline duration, shifts the source window
    bool SlipClip(int index, double offset);

    // Cut clip at time, creating two clips with gap between
    // Returns index of first clip (second clip is at index + 2)
    int CutClipAtTime(double time);

    // Replace all clips (for loading)
    void SetClips(const std::vector<PlaylistClip>& clips);

    // Clear all clips
    void Clear();

    //=========================================================================
    // Conversion to OTIOTrack format (for TimelineView compatibility)
    //=========================================================================

    // Convert playlist to single OTIO video track
    // This allows reusing the existing TimelineView rendering
    struct OTIOTrack ToOTIOTrack() const;

    //=========================================================================
    // Project Integration
    //=========================================================================

    void SetProjectManager(ProjectManager* manager) { project_manager_ = manager; }

    // Link clip to its MediaItem (populates cached metadata)
    bool LinkClipToMedia(PlaylistClip& clip);

    // Relink all clips (call after project load)
    void RelinkAllClips();

    //=========================================================================
    // Callbacks
    //=========================================================================

    using EditCallback = std::function<void()>;
    void SetEditCallback(EditCallback callback) { edit_callback_ = callback; }

private:
    //=========================================================================
    // Internal Helpers
    //=========================================================================

    // Recalculate timeline positions for all clips starting from index
    void RecalculatePositions(int start_index = 0);

    // Generate unique clip ID
    std::string GenerateClipId() const;

    // Update canvas dimensions from all clips
    void UpdateCanvasDimensions();

    // Notify edit callback
    void NotifyEdit();

    //=========================================================================
    // State
    //=========================================================================

    std::vector<PlaylistClip> clips_;
    double frame_rate_ = 24.0;
    int default_gap_frames_ = 10;  // Fixed gap between clips

    // Canvas dimensions (max of all clips)
    int canvas_width_ = 1920;
    int canvas_height_ = 1080;

    // Project integration
    ProjectManager* project_manager_ = nullptr;

    // Callbacks
    EditCallback edit_callback_;

    // ID generation counter
    mutable int next_clip_id_ = 1;
};

} // namespace ump
