#pragma once

#include <string>
#include <memory>
#include <atomic>
#include <chrono>

#include <glad/gl.h>

// Forward declaration - VideoPlayer is in global namespace
class VideoPlayer;

namespace ump {

// Forward declarations
class TimelineView;
class TimelineCache;
class DummyVideoGenerator;

//=============================================================================
// Timeline Playback Controller Configuration
//=============================================================================

struct TimelinePlaybackConfig {
    double scratch_duration = 600.0;        // Default 10 minutes for scratch timelines
    double min_extension_margin = 60.0;     // Regenerate dummy if within this margin of end
    int readAheadFrames = 72;               // Frames to prefetch ahead (~3s @ 24fps)
    double readBehindSeconds = 0.5;         // Seconds to keep behind for backward scrub
    int io_threads = 8;                     // Background I/O threads
};

//=============================================================================
// Timeline Playback Controller
//
// Orchestrates timeline playback using:
// - Dummy video for MPV transport control (play/pause/seek)
// - TimelineCache for actual frame data
//
// Usage:
//   1. Call InitializeForTimeline() when loading an EDL/OTIO
//   2. Call Update() each render frame to get the current texture
//   3. Call Shutdown() when unloading the timeline
//
//=============================================================================

class TimelinePlaybackController {
public:
    TimelinePlaybackController();
    ~TimelinePlaybackController();

    // Initialize for a specific timeline
    // Creates dummy video and initializes cache
    // Returns true on success
    bool InitializeForTimeline(TimelineView* timeline_view,
                               ::VideoPlayer* video_player,
                               DummyVideoGenerator* dummy_generator);

    // Initialize for scratch timeline (no EDL loaded)
    // Creates long-duration dummy for manual timeline building
    bool InitializeForScratchTimeline(::VideoPlayer* video_player,
                                       DummyVideoGenerator* dummy_generator,
                                       int width, int height, double fps);

    // Initialize cache for scratch timeline (called when first clip is added)
    // This is separate from InitializeForScratchTimeline because the cache needs
    // access to the TimelineView's flattener, which requires clips to exist
    bool InitializeCacheForScratchTimeline(TimelineView* timeline_view);

    // Shutdown and clean up
    void Shutdown();

    // Update each render frame
    // Returns texture ID (0 if not ready)
    // Sets width/height to frame dimensions
    GLuint Update(int& width, int& height);

    // Regenerate dummy video to match timeline duration
    // Returns true if dummy was regenerated
    bool RegenerateDummy(double new_duration);

    // Legacy: Check and extend dummy if timeline duration changed
    bool CheckAndExtendDummy(double new_duration);

    // Process pending GPU uploads (call from GL thread)
    void ProcessPendingUploads();

    // Configuration
    void SetConfig(const TimelinePlaybackConfig& config);
    TimelinePlaybackConfig GetConfig() const { return config_; }

    // Loop control
    void SetLooping(bool enabled);
    bool IsLooping() const;

    // State queries
    bool IsInitialized() const { return initialized_; }
    void ReloadDummy();  // Reload dummy video into player (for mode switching)
    bool IsPlaying() const;
    int GetCurrentFrame() const { return current_frame_.load(); }
    double GetDuration() const { return timeline_duration_; }
    double GetDummyDuration() const { return dummy_duration_; }
    double GetFPS() const { return fps_; }
    int GetWidth() const { return width_; }
    int GetHeight() const { return height_; }

    // Access to cache for statistics
    TimelineCache* GetCache() const { return cache_.get(); }

    // Notify controller that timeline was edited
    // Clears stale current texture so we don't show old frames
    void NotifyTracksEdited();

    // Update timeline duration (call after edits that may change duration)
    // Automatically extends dummy video if needed
    void UpdateDuration(double new_duration);

    // Get current source clip info (for UI display)
    std::string GetCurrentClipName() const;
    std::string GetCurrentSourcePath() const;

private:
    // Sync playhead from MPV
    void SyncFromMPV();

    // Generate dummy video for timeline
    std::string GenerateDummy(int width, int height, double fps, double duration);

    // State
    bool initialized_ = false;
    TimelinePlaybackConfig config_;

    // External references (not owned)
    TimelineView* timeline_view_ = nullptr;
    ::VideoPlayer* video_player_ = nullptr;
    DummyVideoGenerator* dummy_generator_ = nullptr;

    // Owned components
    std::unique_ptr<TimelineCache> cache_;

    // Timeline properties
    double timeline_duration_ = 0.0;
    double dummy_duration_ = 0.0;
    double fps_ = 24.0;
    int width_ = 1920;
    int height_ = 1080;

    // Current playback state (synced from MPV)
    std::atomic<int> current_frame_{0};
    std::atomic<bool> is_playing_{false};

    // Current display texture
    GLuint current_texture_ = 0;
    int current_texture_width_ = 0;
    int current_texture_height_ = 0;

    // Post-edit state: Hold old texture briefly until new frame arrives
    // This prevents black flashing while new frame loads
    GLuint pending_evict_texture_ = 0;
    int pending_evict_width_ = 0;
    int pending_evict_height_ = 0;
    bool awaiting_post_edit_frame_ = false;
    std::chrono::steady_clock::time_point post_edit_start_time_;

    // Dummy video path
    std::string dummy_path_;
};

} // namespace ump
