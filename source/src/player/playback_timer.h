#pragma once

#include <chrono>
#include <functional>
#include <algorithm>  // for std::max in GetPosition()

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
    // NOTE: For frame-locked playback, use AdvanceByFrame() instead.
    bool Update();

    // Advance position by exactly one frame duration (frame-locked playback).
    // Use this instead of Update() when you want playback to be locked to
    // actual frame display rate rather than wall-clock time.
    // This prevents desync when decoder can't keep up with timer.
    void AdvanceByFrame();

    //=========================================================================
    // Configuration
    //=========================================================================

    void SetDuration(double duration);
    double GetDuration() const { return duration_; }

    void SetFrameRate(double fps);
    double GetFrameRate() const { return frame_rate_; }

    // DEPRECATED: Loop control moved to main.cpp using boundary system
    // These are kept as no-ops for compatibility
    void SetLooping(bool /*enabled*/) {}  // No-op
    bool IsLooping() const { return false; }  // Always false - main.cpp handles looping

    void SetPlaybackSpeed(double speed);  // 1.0 = normal, 2.0 = 2x, 0.5 = half speed
    double GetPlaybackSpeed() const { return playback_speed_; }

    //=========================================================================
    // State Queries
    //=========================================================================

    // GetPosition() always returns a value in [0, duration)
    // This prevents UI flicker when position briefly exceeds duration before loop wrap
    double GetPosition() const {
        if (duration_ <= 0.0) return 0.0;
        if (position_ >= duration_) return duration_ - (1.0 / std::max(frame_rate_, 1.0));  // Clamp to last frame
        if (position_ < 0.0) return 0.0;
        return position_;
    }
    bool IsPlaying() const { return is_playing_; }
    bool IsAtEnd() const { return position_ >= duration_; }
    bool IsAtStart() const { return position_ <= 0.0; }

    // Frame-based queries (requires fps to be set)
    int GetCurrentFrame() const;
    int GetTotalFrames() const;

    //=========================================================================
    // Callbacks
    //=========================================================================

    // DEPRECATED: Loop callback removed - loop control handled by main.cpp
    void SetOnLoop(std::function<void()> /*callback*/) {}  // No-op

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
    // DEPRECATED: looping_ removed - loop control handled by main.cpp

    // High-resolution timing
    using Clock = std::chrono::steady_clock;
    using TimePoint = std::chrono::time_point<Clock>;
    TimePoint last_update_time_;
    bool first_update_ = true;

    // Callbacks
    std::function<void()> on_end_;
    std::function<void(double)> on_position_changed_;

    // Internal helpers
    void ClampPosition();
    void NotifyPositionChanged();
};

} // namespace ump
