#pragma once

#include <chrono>
#include <functional>

namespace ump {

//=============================================================================
// PlaybackTimer
//
// A manual playback timer that provides frame-accurate timing independent of
// any video player. Can be used to drive virtual timelines, image sequences,
// or any playback that needs precise timing control.
//
// Usage:
//   PlaybackTimer timer;
//   timer.SetDuration(10.0);  // 10 second timeline
//   timer.Play();
//
//   // Each frame:
//   timer.Update();
//   double pos = timer.GetPosition();
//   // Use pos to seek/sync your media sources
//
//=============================================================================

class PlaybackTimer {
public:
    PlaybackTimer();
    ~PlaybackTimer() = default;

    //=========================================================================
    // Playback Control
    //=========================================================================

    void Play();
    void Pause();
    void Stop();  // Pause and seek to start
    void TogglePlayPause();

    //=========================================================================
    // Seeking
    //=========================================================================

    void Seek(double position);
    void SeekRelative(double delta);  // Seek forward/backward by delta seconds
    void SeekToStart();
    void SeekToEnd();

    // Sync to external position (e.g., from MPV playback) without triggering callbacks
    // Used when an external source (like left MPV) is driving playback and we just
    // need to keep the timer's position in sync for UI display purposes.
    void SyncToExternalPosition(double position);

    // Frame stepping (requires fps to be set)
    void StepForward(int frames = 1);
    void StepBackward(int frames = 1);

    //=========================================================================
    // Update (call every frame)
    //=========================================================================

    // Updates position based on elapsed time. Returns true if position changed.
    bool Update();

    //=========================================================================
    // Configuration
    //=========================================================================

    void SetDuration(double duration);
    double GetDuration() const { return duration_; }

    void SetFrameRate(double fps);
    double GetFrameRate() const { return frame_rate_; }

    void SetLooping(bool enabled);
    bool IsLooping() const { return looping_; }

    void SetPlaybackSpeed(double speed);  // 1.0 = normal, 2.0 = 2x, 0.5 = half speed
    double GetPlaybackSpeed() const { return playback_speed_; }

    //=========================================================================
    // State Queries
    //=========================================================================

    double GetPosition() const { return position_; }
    bool IsPlaying() const { return is_playing_; }
    bool IsAtEnd() const { return position_ >= duration_; }
    bool IsAtStart() const { return position_ <= 0.0; }

    // Frame-based queries (requires fps to be set)
    int GetCurrentFrame() const;
    int GetTotalFrames() const;

    //=========================================================================
    // Callbacks
    //=========================================================================

    // Called when playback loops back to start
    void SetOnLoop(std::function<void()> callback) { on_loop_ = callback; }

    // Called when playback reaches end (non-looping mode)
    void SetOnEnd(std::function<void()> callback) { on_end_ = callback; }

    // Called when position changes (seek or playback)
    void SetOnPositionChanged(std::function<void(double)> callback) { on_position_changed_ = callback; }

private:
    // Timing
    double position_ = 0.0;           // Current position in seconds
    double duration_ = 0.0;           // Total duration in seconds
    double frame_rate_ = 24.0;        // Frames per second (for frame stepping)
    double playback_speed_ = 1.0;     // Playback speed multiplier

    // State
    bool is_playing_ = false;
    bool looping_ = true;

    // High-resolution timing
    using Clock = std::chrono::steady_clock;
    using TimePoint = std::chrono::time_point<Clock>;
    TimePoint last_update_time_;
    bool first_update_ = true;

    // Callbacks
    std::function<void()> on_loop_;
    std::function<void()> on_end_;
    std::function<void(double)> on_position_changed_;

    // Internal helpers
    void ClampPosition();
    void NotifyPositionChanged();
};

} // namespace ump
