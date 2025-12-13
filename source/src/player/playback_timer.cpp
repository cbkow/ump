#include "playback_timer.h"
#include <algorithm>
#include <cmath>

namespace ump {

PlaybackTimer::PlaybackTimer() {
    last_update_time_ = Clock::now();
}

//=============================================================================
// Playback Control
//=============================================================================

void PlaybackTimer::Play() {
    if (!is_playing_) {
        is_playing_ = true;
        first_update_ = true;  // Reset timing on play
        last_update_time_ = Clock::now();
    }
}

void PlaybackTimer::Pause() {
    is_playing_ = false;
}

void PlaybackTimer::Stop() {
    is_playing_ = false;
    position_ = 0.0;
    NotifyPositionChanged();
}

void PlaybackTimer::TogglePlayPause() {
    if (is_playing_) {
        Pause();
    } else {
        Play();
    }
}

//=============================================================================
// Seeking
//=============================================================================

void PlaybackTimer::Seek(double position) {
    double old_position = position_;
    position_ = position;
    ClampPosition();

    if (position_ != old_position) {
        NotifyPositionChanged();
    }

    // Reset timing after seek
    first_update_ = true;
    last_update_time_ = Clock::now();
}

void PlaybackTimer::SeekRelative(double delta) {
    Seek(position_ + delta);
}

void PlaybackTimer::SeekToStart() {
    Seek(0.0);
}

void PlaybackTimer::SeekToEnd() {
    Seek(duration_);
}

void PlaybackTimer::StepForward(int frames) {
    if (frame_rate_ > 0) {
        double frame_duration = 1.0 / frame_rate_;
        Seek(position_ + (frames * frame_duration));
    }
}

void PlaybackTimer::StepBackward(int frames) {
    if (frame_rate_ > 0) {
        double frame_duration = 1.0 / frame_rate_;
        Seek(position_ - (frames * frame_duration));
    }
}

void PlaybackTimer::SyncToExternalPosition(double position) {
    // Update position without triggering callbacks
    // Used when an external source (like left MPV) is driving playback
    // and we just need to keep the timer in sync for UI display
    position_ = position;
    ClampPosition();

    // Reset timing to prevent jumps when Update() is next called
    first_update_ = true;
    last_update_time_ = Clock::now();
}

//=============================================================================
// Update
//=============================================================================

bool PlaybackTimer::Update() {
    if (!is_playing_ || duration_ <= 0.0) {
        return false;
    }

    TimePoint now = Clock::now();

    // On first update after play/seek, just record the time
    if (first_update_) {
        last_update_time_ = now;
        first_update_ = false;
        return false;
    }

    // Calculate elapsed time
    auto elapsed = std::chrono::duration<double>(now - last_update_time_);
    double delta = elapsed.count() * playback_speed_;
    last_update_time_ = now;

    // Skip tiny updates (less than 0.1ms)
    if (delta < 0.0001) {
        return false;
    }

    double old_position = position_;
    position_ += delta;

    // Handle end of timeline
    if (position_ >= duration_) {
        if (looping_) {
            // Loop back to start
            position_ = std::fmod(position_, duration_);
            if (on_loop_) {
                on_loop_();
            }
        } else {
            // Stop at end
            position_ = duration_;
            is_playing_ = false;
            if (on_end_) {
                on_end_();
            }
        }
    }

    // Handle negative position (shouldn't happen normally, but safety check)
    if (position_ < 0.0) {
        position_ = 0.0;
    }

    bool changed = (position_ != old_position);
    if (changed) {
        NotifyPositionChanged();
    }

    return changed;
}

//=============================================================================
// Configuration
//=============================================================================

void PlaybackTimer::SetDuration(double duration) {
    duration_ = std::max(0.0, duration);
    ClampPosition();
}

void PlaybackTimer::SetFrameRate(double fps) {
    frame_rate_ = std::max(0.001, fps);  // Prevent division by zero
}

void PlaybackTimer::SetLooping(bool enabled) {
    looping_ = enabled;
}

void PlaybackTimer::SetPlaybackSpeed(double speed) {
    playback_speed_ = std::max(0.0, speed);  // No negative speeds
}

//=============================================================================
// Frame-based Queries
//=============================================================================

int PlaybackTimer::GetCurrentFrame() const {
    if (frame_rate_ <= 0) return 0;
    return static_cast<int>(position_ * frame_rate_);
}

int PlaybackTimer::GetTotalFrames() const {
    if (frame_rate_ <= 0) return 0;
    return static_cast<int>(std::ceil(duration_ * frame_rate_));
}

//=============================================================================
// Internal Helpers
//=============================================================================

void PlaybackTimer::ClampPosition() {
    if (position_ < 0.0) {
        position_ = 0.0;
    }
    if (duration_ > 0.0 && position_ > duration_) {
        position_ = duration_;
    }
}

void PlaybackTimer::NotifyPositionChanged() {
    if (on_position_changed_) {
        on_position_changed_(position_);
    }
}

} // namespace ump
