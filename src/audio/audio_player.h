#pragma once

#include <string>
#include <memory>
#include <atomic>
#include <functional>

#include "audio_decoder.h"

// Forward declare WASAPI device
namespace qcview { class WasapiAudioDevice; }

namespace qcview {

// Forward declarations
class PlaybackTimer;

/**
 * Clip trim/offset configuration for audio sync
 */
struct AudioClipConfig {
    double source_in = 0.0;       // Trim in point (source media time)
    double source_out = -1.0;     // Trim out point (-1 = end of file)
    double position_offset = 0.0; // Timeline offset (when clip starts on timeline)
    double source_duration = 0.0; // Full source duration

    // Get effective duration after trim
    double GetEffectiveDuration() const {
        if (source_out > source_in) {
            return source_out - source_in;
        }
        return source_duration - source_in;
    }

    // Check if timeline position is within clip range
    bool IsInRange(double timeline_pos) const {
        double start = position_offset;
        double end = position_offset + GetEffectiveDuration();
        return timeline_pos >= start && timeline_pos < end;
    }

    // Convert timeline position to source position
    // Returns -1 if position is outside clip range (gap)
    double TimelineToSource(double timeline_pos) const {
        if (!IsInRange(timeline_pos)) {
            return -1.0;  // In a gap
        }
        return source_in + (timeline_pos - position_offset);
    }
};

/**
 * AudioPlayer - WASAPI-based audio output
 *
 * Plays audio from AudioDecoder, synced to external timeline (PlaybackTimer).
 * Handles trim points, position offsets, and gaps with silence.
 *
 * Usage:
 *   AudioPlayer player;
 *   player.Initialize();
 *   player.LoadClip("video.mp4", clip_config);
 *   player.SetTimer(playback_timer);
 *   player.Play();
 *
 *   // Each frame:
 *   player.Update();  // Check sync, correct drift
 */
class AudioPlayer {
public:
    AudioPlayer();
    ~AudioPlayer();

    // Prevent copying
    AudioPlayer(const AudioPlayer&) = delete;
    AudioPlayer& operator=(const AudioPlayer&) = delete;

    //=========================================================================
    // Lifecycle
    //=========================================================================

    // Initialize WASAPI audio engine
    bool Initialize();

    // Shutdown and release resources
    void Shutdown();

    // Load audio clip with trim configuration
    bool LoadClip(const std::string& file_path, const AudioClipConfig& config = {});

    // Unload current clip
    void UnloadClip();

    // Check state
    bool IsInitialized() const { return initialized_; }
    bool HasClip() const;
    bool HasAudio() const;

    //=========================================================================
    // Playback Control
    //=========================================================================

    void Play();
    void Pause();
    void Stop();
    bool IsPlaying() const { return is_playing_.load(); }

    //=========================================================================
    // Timeline Sync
    //=========================================================================

    // Set external timer for position sync
    // When set, audio position is driven by timer
    void SetTimer(PlaybackTimer* timer);

    // Called every frame to check sync and correct drift
    void Update();

    // Manual seek (when no timer is set)
    void Seek(double position);

    // Get current playback position (timeline time)
    double GetPosition() const;

    //=========================================================================
    // Volume Control
    //=========================================================================

    void SetVolume(float volume);  // 0.0 to 1.0
    float GetVolume() const { return volume_.load(); }

    void SetMuted(bool muted);
    bool IsMuted() const { return muted_.load(); }

    //=========================================================================
    // Clip Configuration
    //=========================================================================

    void SetClipConfig(const AudioClipConfig& config);
    const AudioClipConfig& GetClipConfig() const { return clip_config_; }

    //=========================================================================
    // Sync Configuration
    //=========================================================================

    void SetSyncCheckInterval(double interval_ms) { sync_check_interval_ms_ = interval_ms; }
    void SetSyncThreshold(double threshold_ms) { sync_threshold_ms_ = threshold_ms; }

private:
    //=========================================================================
    // WASAPI Callbacks
    //=========================================================================

    static void DataCallback(void* device, float* output,
                            uint32_t frame_count, void* userData);

    void ProcessAudio(float* output, unsigned int frame_count);

    //=========================================================================
    // Sync Logic
    //=========================================================================

    void CheckAndCorrectSync();
    bool IsInGap(double timeline_pos) const;

    //=========================================================================
    // State
    //=========================================================================

    bool initialized_ = false;
    AudioClipConfig clip_config_;

    // WASAPI audio device
    std::unique_ptr<WasapiAudioDevice> device_;

    // Audio decoder
    std::unique_ptr<AudioDecoder> decoder_;

    // Playback state
    std::atomic<bool> is_playing_{false};
    std::atomic<float> volume_{1.0f};
    std::atomic<bool> muted_{false};

    // Timeline sync
    PlaybackTimer* timer_ = nullptr;
    std::atomic<double> current_position_{0.0};
    double last_sync_check_time_ = 0.0;

    // Sync configuration
    double sync_check_interval_ms_ = 200.0;  // Check every 200ms
    double sync_threshold_ms_ = 50.0;        // Correct if drift > 50ms

public:
    // Diagnostic counters (for troubleshooting)
    std::atomic<uint64_t> diag_callback_count_{0};
    std::atomic<uint64_t> diag_frames_output_{0};
    std::atomic<uint64_t> diag_silence_frames_{0};
    std::atomic<uint64_t> diag_frames_read_{0};
};

} // namespace qcview
