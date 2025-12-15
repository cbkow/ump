#pragma once

#include <string>
#include <memory>
#include <atomic>
#include <chrono>
#include <functional>

#include <glad/gl.h>

// Forward declaration - VideoPlayer is in global namespace
class VideoPlayer;

namespace ump {

// Forward declarations
class TimelineView;
class TimelineCache;
class AudioMixer;
class PlaybackTimer;

//=============================================================================
// Timeline Playback Controller Configuration
//=============================================================================

struct TimelinePlaybackConfig {
    double scratch_duration = 1.0;          // Start at 1 second - auto-extends as clips are added
    int readAheadFrames = 72;               // Frames to prefetch ahead (~3s @ 24fps)
    double readBehindSeconds = 0.5;         // Seconds to keep behind for backward scrub
    int io_threads = 8;                     // Background I/O threads
};

//=============================================================================
// Timeline Playback Controller
//
// Orchestrates timeline playback using:
// - PlaybackTimer for transport control (play/pause/seek) - virtual timeline mode
// - TimelineCache for actual frame data
//
// Usage:
//   1. Call InitializeForVirtualTimeline() when loading an EDL/OTIO
//   2. Call Update() each render frame to get the current texture
//   3. Call Shutdown() when unloading the timeline
//
//=============================================================================

class TimelinePlaybackController {
public:
    TimelinePlaybackController();
    ~TimelinePlaybackController();

    // Initialize with virtual timeline (PlaybackTimer-based, no dummy video)
    // canvas_width/height: Timeline output canvas dimensions (0 = auto-detect from clips)
    bool InitializeForVirtualTimeline(TimelineView* timeline_view,
                                       ::VideoPlayer* video_player,
                                       int canvas_width = 0,
                                       int canvas_height = 0);

    // Initialize virtual scratch timeline (no dummy video)
    bool InitializeForVirtualScratchTimeline(::VideoPlayer* video_player,
                                              int width, int height, double fps);

    // Initialize cache for scratch timeline (called when first clip is added)
    // This is separate from InitializeForVirtualScratchTimeline because the cache needs
    // access to the TimelineView's flattener, which requires clips to exist
    bool InitializeCacheForScratchTimeline(TimelineView* timeline_view);

    // Shutdown and clean up
    void Shutdown();

    // Update each render frame
    // Returns texture ID (0 if not ready)
    // Sets width/height to frame dimensions
    GLuint Update(int& width, int& height);

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
    bool IsPlaying() const;
    int GetCurrentFrame() const { return current_frame_.load(); }
    double GetDuration() const { return timeline_duration_; }
    double GetFPS() const { return fps_; }
    int GetWidth() const { return width_; }
    int GetHeight() const { return height_; }

    // Access to cache for statistics
    TimelineCache* GetCache() const { return cache_.get(); }

    // Access to audio mixer for volume control
    AudioMixer* GetAudioMixer() const { return audio_mixer_.get(); }

    // Check if using virtual timeline mode (always true now)
    bool IsVirtualTimelineMode() const { return use_virtual_timeline_; }

    // Access timer (for external position queries)
    PlaybackTimer* GetTimer() const { return timeline_timer_.get(); }

    // Transport controls (route to timer in virtual mode)
    void Play();
    void Pause();
    void TogglePlayPause();
    void Seek(double position);
    void SeekRelative(double delta);
    void StepForward(int frames = 1);
    void StepBackward(int frames = 1);
    void GoToStart();
    void GoToEnd();

    // Get current position in seconds
    double GetPosition() const;

    // Per-frame timer update (call from render loop)
    void UpdateTimer();

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
    // State
    bool initialized_ = false;
    TimelinePlaybackConfig config_;

    // External references (not owned)
    TimelineView* timeline_view_ = nullptr;
    ::VideoPlayer* video_player_ = nullptr;

    // Owned components
    std::unique_ptr<TimelineCache> cache_;
    std::unique_ptr<AudioMixer> audio_mixer_;
    std::unique_ptr<PlaybackTimer> timeline_timer_;  // Virtual timeline clock

    // Virtual timeline mode flag (always true now - dummy video mode removed)
    bool use_virtual_timeline_ = true;

    // Frame-locked timing: accumulate real time and advance by frames
    std::chrono::steady_clock::time_point last_timer_update_;
    double accumulated_time_ = 0.0;
    bool timer_initialized_ = false;

    // Wait-for-frame state: after Play() or Seek(), don't advance timer until
    // we confirm the decoder has the current frame ready. This prevents the
    // timer from running ahead of H.264 decoders that need keyframe catch-up.
    bool waiting_for_frame_ = false;

    // Timeline properties
    double timeline_duration_ = 0.0;
    double fps_ = 24.0;
    int width_ = 1920;
    int height_ = 1080;

    // Current playback state
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
};

} // namespace ump
