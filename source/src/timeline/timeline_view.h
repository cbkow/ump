#pragma once

#include <string>
#include <vector>
#include <memory>
#include <map>
#include <set>
#include "../player/video_player.h"
#include "timeline_selection.h"
#include "timeline_types.h"

// OTIO library support - disabled until library is installed
// When OTIO is available, define USE_OPENTIMELINEIO in CMakeLists.txt
#ifdef USE_OPENTIMELINEIO
#include <opentimelineio/timeline.h>
namespace otio = opentimelineio::OPENTIMELINEIO_VERSION;
#endif

namespace ump {

// Forward declarations
class TimelinePlaybackController;
class DummyVideoGenerator;

// Flattening engine - computes visible clip at any timestamp
class TimelineFlattener {
public:
    TimelineFlattener() = default;

    // Set track data and visibility
    void SetTracks(const std::vector<OTIOTrack>& tracks);
    void SetTrackVisibility(const std::string& track_id, bool visible);
    void SetTrackMute(const std::string& track_id, bool muted);

    // Get flattened result at specific time
    std::string GetVisibleClipPathAtTime(double timestamp);
    const OTIOClip* GetVisibleClipAtTime(double timestamp);

    // Get all enabled audio tracks (for mixing)
    std::vector<std::string> GetAudibleClipPathsAtTime(double timestamp);

    // Clear cache (call when visibility changes)
    void InvalidateCache();

private:
    std::vector<OTIOTrack> tracks_;
    std::map<double, std::string> cache_visible_clips_;  // timestamp → file path

    // Helper: find clip in track at given time
    const OTIOClip* FindClipInTrack(const OTIOTrack& track, double timestamp);
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
    bool LoadFCPXMLFile(const std::string& file_path);

    // Initialize empty scratch timeline (no source file)
    void InitializeForScratch(const std::string& name, double duration, double fps,
                              int width, int height);

    // Timeline info
    std::string GetTimelineName() const { return timeline_name_; }
    double GetDuration() const { return timeline_duration_; }
    double GetFrameRate() const { return frame_rate_; }
    int GetVideoTrackCount() const;
    int GetAudioTrackCount() const;
    std::string GetSourceFilePath() const { return source_file_path_; }
    std::string GetSourceDirectory() const;

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

    // Access to flattener for timeline cache
    TimelineFlattener& GetFlattener() { return flattener_; }
    const TimelineFlattener& GetFlattener() const { return flattener_; }

    // Playback controller for timeline mode
    // Call InitializePlayback() after loading EDL/OTIO and linking media
    bool InitializePlayback(DummyVideoGenerator* dummy_generator);
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
    void SetTimelineLooping(bool enabled) { timeline_loop_enabled_ = enabled; }
    bool IsTimelineLooping() const { return timeline_loop_enabled_; }

    // Zoom/Pan control for external UI (sliders)
    float GetZoomLevel() const { return zoom_level_; }
    void SetZoomLevel(float zoom);
    float GetScrollOffset() const { return scroll_offset_x_; }
    void SetScrollOffset(float offset);
    float GetMaxScrollOffset() const;

    // Force cache refresh (failsafe for corrupted state)
    void ForceRefreshCache();

    // Export flattened timeline
    void ExportFlattenedEDL(const std::string& output_path);
    void ExportFlattenedOTIO(const std::string& output_path);

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

    // Track interaction
    void HandleTrackVisibilityToggle(const std::string& track_id);
    void HandleTrackMuteToggle(const std::string& track_id);
    void HandleTrackSolo(const std::string& track_id);
    void HandleClipClick(const OTIOClip& clip);
    void HandleTimelineSeek(double timestamp);

    // Flattening
    void UpdateFlattenedPlayback();
    void LoadFlattenedClipIntoMPV(const OTIOClip& clip);

    // Timeline parsing helpers
    bool ParseOTIOTimeline(const std::string& file_path);
    void CreateMockTimeline();  // For testing without OTIO

#ifdef USE_OPENTIMELINEIO
    void ExtractTracksFromOTIO(otio::Timeline* timeline);
    OTIOClip ConvertOTIOClip(otio::Clip* otio_clip, double global_offset);
#endif

    // Helper to find track by ID
    OTIOTrack* GetTrackById(const std::string& track_id);

    // Data members
    ::VideoPlayer* video_player_;
    TimelineFlattener flattener_;
    std::unique_ptr<TimelinePlaybackController> playback_controller_;
    TimelinePlaybackController* external_playback_controller_ = nullptr;  // Non-owning, for scratch timelines
    TimelineSelection selection_;

    std::vector<OTIOTrack> tracks_;
    std::string timeline_name_;
    std::string source_file_path_;      // Path to the EDL/OTIO/XML source file
    double timeline_duration_ = 0.0;
    double frame_rate_ = 24.0;
    double current_time_ = 0.0;

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

    // Colors (use existing theme)
    ImVec4 color_video_clip_ = ImVec4(0.3f, 0.5f, 0.8f, 1.0f);
    ImVec4 color_audio_clip_ = ImVec4(0.5f, 0.8f, 0.3f, 1.0f);
    ImVec4 color_gap_ = ImVec4(0.2f, 0.2f, 0.2f, 0.3f);
    ImVec4 color_transition_ = ImVec4(0.9f, 0.6f, 0.2f, 0.8f);
};

} // namespace ump
