#pragma once

#include <string>
#include <vector>
#include <memory>
#include <map>
#include <set>
#include <functional>
#include "../player/video_player.h"
#include "timeline_selection.h"
#include "timeline_types.h"
#include "../project/media_item.h"

// OTIO library support - disabled until library is installed
// When OTIO is available, define USE_OPENTIMELINEIO in CMakeLists.txt
#ifdef USE_OPENTIMELINEIO
#include <opentimelineio/timeline.h>
namespace otio = opentimelineio::OPENTIMELINEIO_VERSION;
#endif

namespace ump {

// Forward declarations
class TimelinePlaybackController;
class ProjectManager;

// Flattening engine - computes visible clip at any timestamp
// Thread-safe: can be accessed from main thread and audio background thread
class TimelineFlattener {
public:
    TimelineFlattener() = default;

    // Set track data and visibility
    void SetTracks(const std::vector<OTIOTrack>& tracks);
    void SetTrackVisibility(const std::string& track_id, bool visible);
    void SetTrackMute(const std::string& track_id, bool muted);

    // Enable visibility override mode (for dual view - persists across SetTracks)
    // When enabled, uses visibility_overrides_ instead of track.visible
    void EnableVisibilityOverrides(bool enable);
    void SetVisibilityOverride(const std::string& track_id, bool visible);

    // Get flattened result at specific time
    std::string GetVisibleClipPathAtTime(double timestamp);
    const OTIOClip* GetVisibleClipAtTime(double timestamp);

    // Get clip from a specific track at given time (for dual view mode)
    // Returns nullptr if track not found or no clip at that time
    const OTIOClip* GetClipFromTrackAtTime(const std::string& track_id, double timestamp);

    // Get audible clip (top-layer video clip where track is not audio_muted)
    const OTIOClip* GetAudibleClipAtTime(double timestamp);

    // Get all enabled audio tracks (for mixing)
    std::vector<std::string> GetAudibleClipPathsAtTime(double timestamp);

    // Get ALL audible clips at a given time for multi-track audio mixing
    // Returns clips from: video tracks (where !audio_muted) AND audio tracks (where !muted)
    // Each clip has timing info needed for proper audio sync
    // NOTE: Returns COPIES of clips (not pointers) for thread safety - the flattener's
    // internal tracks can be replaced at any time by the UI thread, so returning pointers
    // would cause use-after-free crashes.
    std::vector<OTIOClip> GetAllAudibleClipsAtTime(double timestamp);

    // Access to tracks for lookahead queries (returns a copy for thread safety)
    std::vector<OTIOTrack> GetTracks() const;

    // Clear cache (call when visibility changes)
    void InvalidateCache();

private:
    std::vector<OTIOTrack> tracks_;
    std::map<double, std::string> cache_visible_clips_;  // timestamp → file path
    mutable std::mutex tracks_mutex_;  // Protects tracks_ for thread safety

    // Visibility override mode (for dual view - persists across SetTracks)
    bool use_visibility_overrides_ = false;
    std::map<std::string, bool> visibility_overrides_;  // track_id → visible

    // Helper: check if track is visible (considers override mode)
    bool IsTrackVisible(const OTIOTrack& track) const;

    // Helper: find clip in track at given time (must be called with lock held)
    const OTIOClip* FindClipInTrack(const OTIOTrack& track, double timestamp);

    // Helper: flatten into nested clip to find visible media at given timeline time
    const OTIOClip* GetVisibleClipInNest(const OTIOClip* nest_clip, double timeline_timestamp);
};

// Main timeline viewer UI component
class TimelineView {
public:
    TimelineView(::VideoPlayer* player);
    ~TimelineView();

    // UI rendering
    void Render(bool* show_timeline_panel);

    // Timeline file import (creates mock data when OTIO not available)
    bool LoadOTIOFile(const std::string& file_path);
    bool LoadEDLFile(const std::string& file_path);

    // Auto-mute video clips with embedded audio on video tracks
    // Called after import since imported timelines likely have their own audio tracks
    void AutoMuteVideoClipsWithAudio();

    // Nested timeline navigation (for AAF/XML compositions)
    bool EnterNestedClip(const std::string& clip_id);
    bool ExitNestedTimeline();
    bool IsViewingNestedTimeline() const;
    int GetNestedDepth() const;
    std::vector<std::string> GetBreadcrumbPath() const;

    // Initialize empty scratch timeline (no source file)
    void InitializeForScratch(const std::string& name, double duration, double fps,
                              int width, int height);

    // Image sequence as timeline (unified view for sequences)
    bool LoadImageSequenceAsTimeline(MediaItem* item);

    // Video file as timeline (unified view for solo videos)
    bool LoadVideoFileAsTimeline(MediaItem* item);

    // Audio file as timeline (unified view for solo audio, A1 track only)
    bool LoadAudioFileAsTimeline(MediaItem* item);

    // Playlist as timeline (unified single-track playlist with ripple editing)
    bool LoadPlaylistAsTimeline(MediaItem* item);

    // Set project manager for resolving media IDs in playlist mode
    void SetProjectManager(ProjectManager* manager) { project_manager_ = manager; }

    // Dual view timeline (LEFT/RIGHT video tracks for comparison)
    void InitializeForDualView(const std::string& name, double fps);
    bool LoadMediaToLeftTrack(MediaItem* item);
    bool LoadMediaToRightTrack(MediaItem* item);
    bool IsDualViewMode() const { return source_mode_ == TimelineSourceMode::DUAL_VIEW; }
    OTIOTrack* GetLeftTrack();
    OTIOTrack* GetRightTrack();

    // Source mode - determines editing restrictions
    TimelineSourceMode GetSourceMode() const { return source_mode_; }
    void SetSourceMode(TimelineSourceMode mode) { source_mode_ = mode; }  // Direct setter for restoration
    MediaItem* GetSourceMediaItem();  // Refreshes pointer from project_manager if ID is set (prevents stale pointer)
    void ResetSourceMode();  // Reset to VIDEO_FILE mode (default)

    // Track locking queries (based on source mode)
    bool IsVideoTrackLocked() const;
    bool CanAddVideoClips() const;
    bool CanRemoveVideoClips() const;
    bool CanAddAudioClips() const;
    bool CanEditClip(const OTIOClip& clip, const OTIOTrack& track) const;

    // Timeline info
    std::string GetTimelineName() const { return timeline_name_; }
    double GetDuration() const { return timeline_duration_; }
    double GetFrameRate() const { return frame_rate_; }
    void SetFrameRate(double fps);
    int GetVideoTrackCount() const;
    int GetAudioTrackCount() const;
    std::string GetSourceFilePath() const { return source_file_path_; }
    std::string GetSourceDirectory() const;

    // Canvas dimensions (output resolution for multi-resolution support)
    void SetCanvasDimensions(int width, int height) { canvas_width_ = width; canvas_height_ = height; }
    int GetCanvasWidth() const { return canvas_width_; }
    int GetCanvasHeight() const { return canvas_height_; }

    // Access to tracks for external rendering
    std::vector<OTIOTrack>& GetTracks() { return tracks_; }
    const std::vector<OTIOTrack>& GetTracks() const { return tracks_; }

    // Set tracks from cached edits (restores timeline state when re-entering)
    // Optionally restores frame_rate, timeline_name, and source_file_path for full state restoration
    void SetTracks(const std::vector<OTIOTrack>& tracks,
                   double frame_rate = 0.0,
                   const std::string& timeline_name = "",
                   const std::string& source_file_path = "");

    // Track management (Resolve-style: right-click to add/delete)
    void AddVideoTrack(int insert_index = -1);  // -1 = add at top
    void AddAudioTrack(int insert_index = -1);  // -1 = add at bottom
    bool DeleteTrack(int track_index);
    bool CanDeleteTrack(int track_index) const;

    // Selection access
    TimelineSelection& GetSelection() { return selection_; }
    const TimelineSelection& GetSelection() const { return selection_; }

    // Clip query helpers
    OTIOClip* FindClipById(const std::string& clip_id);
    OTIOClip* FindClipById(const std::string& clip_id, int* out_track_index);
    std::vector<OTIOClip*> GetClipsAtTime(double time);
    int GetTrackIndexForClip(const std::string& clip_id) const;

    // Update timeline duration after edits
    void RecalculateDuration();

    // Sync flattener with current tracks and invalidate cache
    // Call this after any edit operation (move, trim, cut, delete, etc.)
    void SyncFlattenerAndInvalidate();

    // Lightweight sync for cut operations - updates flattener tracks but
    // does NOT invalidate cache since cutting doesn't change frame mappings
    void SyncFlattenerOnly();

    //=========================================================================
    // Edit Safety (Thread-Safe Track Modifications)
    //=========================================================================

    // Call BeginEdit() before any operation that modifies tracks_ (add/move/delete clips)
    // This pauses playback to prevent race conditions with background threads
    // Returns true if playback was paused (caller should call EndEdit when done)
    bool BeginEdit();

    // Call EndEdit() after track modifications are complete
    // If resume=true and playback was active before BeginEdit(), resumes playback
    void EndEdit(bool resume = true);

    // Check if currently in edit mode (playback paused for editing)
    bool IsEditing() const { return edit_mode_active_; }

    // Access to flattener for timeline cache
    TimelineFlattener& GetFlattener() { return flattener_; }
    const TimelineFlattener& GetFlattener() const { return flattener_; }

    // Playback controller for timeline mode
    // Call InitializePlayback() after loading EDL/OTIO and linking media
    bool InitializePlayback();
    void ShutdownPlayback();
    TimelinePlaybackController* GetPlaybackController() const { return playback_controller_.get(); }
    bool HasPlaybackController() const { return playback_controller_ != nullptr || external_playback_controller_ != nullptr; }

    // External playback controller (for scratch timelines where controller is managed externally)
    // When set, SyncFlattenerAndInvalidate() will use this instead of the internal controller
    void SetExternalPlaybackController(TimelinePlaybackController* controller);
    TimelinePlaybackController* GetEffectivePlaybackController() const;

    // Playback integration
    void SetCurrentTime(double timestamp);  // Called by main player
    double GetCurrentTime() const { return current_time_; }

    // Timeline In/Out points and loop mode (separate from solo video mode)
    void SetTimelineInPoint(double time);
    void SetTimelineOutPoint(double time);
    double GetTimelineInPoint() const { return timeline_in_point_; }
    double GetTimelineOutPoint() const { return timeline_out_point_; }
    void ClearTimelineInOutPoints();
    bool HasTimelineInPoint() const { return timeline_in_point_ >= 0; }
    bool HasTimelineOutPoint() const { return timeline_out_point_ >= 0; }
    bool HasTimelineInOutPoints() const { return timeline_in_point_ >= 0 && timeline_out_point_ >= 0; }
    void SetTimelineLooping(bool enabled);
    bool IsTimelineLooping() const { return timeline_loop_enabled_; }

    // Zoom/Pan control for external UI (sliders)
    float GetZoomLevel() const { return zoom_level_; }
    void SetZoomLevel(float zoom);
    void SetZoomLevelAroundTime(float zoom, double time);  // Zoom keeping time position stable
    void FitZoomToWidth(float visible_width);  // Auto-fit zoom so timeline fills visible width
    void SetInitialZoomForDuration();  // Set reasonable initial zoom based on timeline_duration_
    void UpdateVisibleWidth(float width);  // Update for zoom limits and proportional resize
    void RequestFitZoomOnNextRender() { fit_zoom_pending_ = true; }  // Defer fit to next render
    bool HasPendingFitZoom() const { return fit_zoom_pending_; }
    void ClearPendingFitZoom() { fit_zoom_pending_ = false; }

    // Pipeline mode - set before InitializePlayback() to ensure correct mode from start
    void SetPendingPipelineMode(PipelineMode mode) { pending_pipeline_mode_ = mode; }
    PipelineMode GetPendingPipelineMode() const { return pending_pipeline_mode_; }

    // Video codec - set before InitializePlayback() for MF HDR eligibility check
    void SetPendingVideoCodec(const std::string& codec) { pending_video_codec_ = codec; }
    const std::string& GetPendingVideoCodec() const { return pending_video_codec_; }

    // Timecode start frame - set before InitializePlayback() for MF HDR frame offset
    void SetPendingTimecodeStartFrame(int frame) { pending_timecode_start_frame_ = frame; }
    int GetPendingTimecodeStartFrame() const { return pending_timecode_start_frame_; }

    float GetScrollOffset() const { return scroll_offset_x_; }
    void SetScrollOffset(float offset);
    float GetMaxScrollOffset() const;

    // Force cache refresh (failsafe for corrupted state)
    void ForceRefreshCache();

    // Export flattened timeline
    void ExportFlattenedEDL(const std::string& output_path);
    void ExportFlattenedOTIO(const std::string& output_path);

    // Drag-drop callback for PLAYLIST mode - called when media items are dropped on timeline
    using DropMediaCallback = std::function<void(const std::vector<std::string>& media_ids)>;
    void SetDropMediaCallback(DropMediaCallback callback) { drop_media_callback_ = callback; }

private:
    // UI rendering helpers
    void RenderToolbar();
    void RenderTrackList();
    void RenderTrackHeader(OTIOTrack& track, int track_index);
    void RenderTrackClips(const OTIOTrack& track, float track_height);
    void RenderTimelineRuler();
    void RenderPlayhead();
    void RenderCacheBar();
    void RenderClipTooltip(const OTIOClip& clip);
    void RenderViewportMinimap();  // Shows pan/zoom viewport indicator

    // Track interaction
    void HandleTrackVisibilityToggle(const std::string& track_id);
    void HandleTrackMuteToggle(const std::string& track_id);
    void HandleTrackSolo(const std::string& track_id);
    void HandleClipClick(const OTIOClip& clip);
    void HandleTimelineSeek(double timestamp);

    // Flattening
    void UpdateFlattenedPlayback();
    void LoadFlattenedClipForPlayback(const OTIOClip& clip);

    // Timeline parsing helpers
    bool ParseOTIOTimeline(const std::string& file_path);
    void CreateMockTimeline();  // For testing without OTIO

#ifdef USE_OPENTIMELINEIO
    void ExtractTracksFromOTIO(otio::Timeline* timeline);
    OTIOClip ConvertOTIOClip(otio::Clip* otio_clip, double global_offset);
    // Recursively parse nested stack into nested_tracks and link media
    void ParseNestedStack(OTIOClip& nest_clip, otio::Stack* nested_stack, const std::string& source_dir);
#endif

    // Helper to find track by ID
    OTIOTrack* GetTrackById(const std::string& track_id);

    // Data members
    ::VideoPlayer* video_player_;
    TimelineFlattener flattener_;
    std::unique_ptr<TimelinePlaybackController> playback_controller_;
    TimelinePlaybackController* external_playback_controller_ = nullptr;  // Non-owning, for scratch timelines
    TimelineSelection selection_;
    std::unique_ptr<class NestedTimelineManager> nested_manager_;  // For AAF/XML nested compositions

    std::vector<OTIOTrack> tracks_;
    std::string timeline_name_;
    std::string source_file_path_;      // Path to the EDL/OTIO/XML source file
    double timeline_duration_ = 0.0;
    double frame_rate_ = 24.0;
    double current_time_ = 0.0;
    int canvas_width_ = 1920;     // Output canvas width (default HD)
    int canvas_height_ = 1080;    // Output canvas height (default HD)

    // Source mode - for unified view handling (image sequences, videos, etc.)
    TimelineSourceMode source_mode_ = TimelineSourceMode::VIDEO_FILE;
    MediaItem* source_media_item_ = nullptr;  // Non-owning pointer to source MediaItem (for IMAGE_SEQUENCE mode)
    std::string source_media_item_id_;  // ID for safe re-lookup (prevents stale pointer after media_pool reallocation)

    // UI state
    enum class FlattenMode {
        AUTO_PAINTER_ORDER,     // Standard top-to-bottom
        MANUAL_PRIORITY,        // User-defined priority
        SINGLE_TRACK_PREVIEW    // Show only selected track
    };

    FlattenMode flatten_mode_ = FlattenMode::AUTO_PAINTER_ORDER;
    std::string selected_track_id_;  // For single-track preview
    std::vector<std::string> manual_priority_order_;  // For manual mode

    // Visual settings
    float zoom_level_ = 50.0f;        // Pixels per second (matches OTIOTimeline::DEFAULT_PIXELS_PER_SECOND)
    float scroll_offset_x_ = 0.0f;    // Horizontal scroll
    float track_height_ = 60.0f;      // Height per track in pixels

    bool show_waveforms_ = false;     // Future: audio waveform display
    bool show_thumbnails_ = true;     // Clip thumbnail previews
    bool fit_zoom_pending_ = false;   // Request zoom fit on next render (deferred)
    float last_visible_width_ = 0.0f; // Cached visible width for zoom limit calculations
    PipelineMode pending_pipeline_mode_ = PipelineMode::NORMAL;  // Pipeline mode for next InitializePlayback
    std::string pending_video_codec_;  // Video codec for MF HDR eligibility check
    int pending_timecode_start_frame_ = 0;  // Embedded timecode start frame for MF HDR

    // Timeline In/Out points and loop mode (separate from solo video MediaItem)
    double timeline_in_point_ = -1.0;   // In point in seconds (-1 = not set)
    double timeline_out_point_ = -1.0;  // Out point in seconds (-1 = not set)
    bool timeline_loop_enabled_ = true; // Loop playback within In/Out region

    // Cached ruler position for cache bar rendering
    ImVec2 ruler_screen_pos_ = ImVec2(0, 0);
    float ruler_width_ = 0.0f;

    // Interaction state
    bool is_scrubbing_ = false;
    bool is_dragging_clip_ = false;
    std::string hovered_clip_id_;

    // PLAYLIST mode editing state
    enum class PlaylistEditOp {
        NONE,
        TRIM_START,    // Dragging left edge to adjust source_in
        TRIM_END,      // Dragging right edge to adjust source_out
        SLIP,          // Shift+drag to slip source content within fixed duration
        MOVE           // Drag to reorder (ripple)
    };
    PlaylistEditOp playlist_edit_op_ = PlaylistEditOp::NONE;
    std::string playlist_edit_clip_id_;       // Clip being edited
    int playlist_edit_clip_index_ = -1;       // Index in tracks_[0].clips
    float playlist_edit_start_mouse_x_ = 0.0f; // Mouse X when edit started
    double playlist_edit_original_in_ = 0.0;   // Original source_in (for trim/slip)
    double playlist_edit_original_out_ = 0.0;  // Original source_out (for trim/slip)
    double playlist_edit_original_start_ = 0.0; // Original timeline start (for move)

    // Pending edit state - require drag threshold before starting actual edit
    bool playlist_pending_edit_ = false;         // True when clicked but not yet dragging
    PlaylistEditOp playlist_pending_op_ = PlaylistEditOp::NONE;  // What operation to start
    static constexpr float kDragThreshold = 10.0f; // Pixels of movement before edit starts

    // Edit mode state (for thread-safe track modifications)
    bool edit_mode_active_ = false;      // True while editing tracks
    bool was_playing_before_edit_ = false; // Restore playback after edit

    // Viewport minimap interaction
    bool is_dragging_viewport_ = false;
    float viewport_drag_start_offset_ = 0.0f;  // Scroll offset when drag started
    float viewport_drag_start_mouse_x_ = 0.0f; // Mouse X when drag started

    // Colors (use existing theme)
    ImVec4 color_video_clip_ = ImVec4(0.3f, 0.5f, 0.8f, 1.0f);
    ImVec4 color_audio_clip_ = ImVec4(0.5f, 0.8f, 0.3f, 1.0f);
    ImVec4 color_gap_ = ImVec4(0.2f, 0.2f, 0.2f, 0.3f);
    ImVec4 color_transition_ = ImVec4(0.9f, 0.6f, 0.2f, 0.8f);

    // Drag-drop callback for PLAYLIST mode
    DropMediaCallback drop_media_callback_;

    // Project manager for resolving media IDs
    ProjectManager* project_manager_ = nullptr;

    // PLAYLIST mode editing helpers
    void UpdatePlaylistEdit();       // Called each frame during active edit
    void CommitPlaylistEdit();       // Finalize edit and sync
    void CancelPlaylistEdit();       // Cancel and revert
    void RippleRecalculatePositions(int start_index);  // Adjust downstream clip positions
    void SyncPlaylistToMediaItem();  // Save edits back to source MediaItem

public:
    // Delete clip from playlist (removes from both timeline and source MediaItem)
    bool DeleteClipFromPlaylist(const std::string& clip_id);
};

} // namespace ump
