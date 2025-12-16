#pragma once

#include <string>
#include <memory>
#include <vector>
#include <set>
#include <unordered_map>
#include <atomic>
#include <mutex>

#include "audio_decoder.h"

// Forward declare miniaudio types
struct ma_device;

namespace ump {

// Forward declarations
class TimelineFlattener;
class PlaybackTimer;
struct OTIOClip;

/**
 * Active audio source tracking for clip switching
 */
struct ActiveAudioSource {
    std::string clip_id;
    std::string source_path;
    double clip_start_time = 0.0;    // Timeline position where clip starts
    double clip_duration = 0.0;
    double source_in = 0.0;          // Trim in point
    double source_out = 0.0;         // Trim out point
    std::shared_ptr<AudioDecoder> decoder;
};

/**
 * AudioMixer - Multi-clip audio manager for OTIO timeline mode
 *
 * Manages multiple AudioDecoders and switches between them based on
 * timeline position, following top-layer priority (like video).
 *
 * Key features:
 * - Top-layer priority selection (matches TimelineFlattener::GetVisibleClipAtTime)
 * - Smooth clip switching with crossfade
 * - Pre-loading decoders for clips
 *
 * Usage:
 *   AudioMixer mixer;
 *   mixer.Initialize();
 *   mixer.SetFlattener(&timeline_flattener);
 *   mixer.SetTimer(&playback_timer);
 *   mixer.PreloadClips(all_clips);
 *
 *   // Each frame:
 *   mixer.Update();  // Checks for clip switches and sync
 */
class AudioMixer {
public:
    AudioMixer();
    ~AudioMixer();

    // Prevent copying
    AudioMixer(const AudioMixer&) = delete;
    AudioMixer& operator=(const AudioMixer&) = delete;

    //=========================================================================
    // Lifecycle
    //=========================================================================

    bool Initialize();
    void Shutdown();
    bool IsInitialized() const { return initialized_; }

    //=========================================================================
    // Timeline Integration
    //=========================================================================

    // Set timeline flattener for top-layer priority lookup
    void SetFlattener(TimelineFlattener* flattener);

    // Set playback timer for position tracking
    void SetTimer(PlaybackTimer* timer);

    // Pre-load audio decoders for linked clips
    void PreloadClips(const std::vector<OTIOClip>& clips);

    // Clear all loaded clips
    void ClearClips();

    //=========================================================================
    // Playback Control
    //=========================================================================

    void Play();
    void Pause();
    void Stop();
    bool IsPlaying() const { return is_playing_.load(); }

    //=========================================================================
    // Update (call every frame)
    //=========================================================================

    // Check for clip switches and sync
    void Update();

    // Seek to position (clears buffers and switches clips as needed)
    void Seek(double position);

    // Set timeline position directly (for use without PlaybackTimer)
    void SetTimelinePosition(double position) { current_position_ = position; }

    //=========================================================================
    // Volume Control
    //=========================================================================

    void SetVolume(float volume);
    float GetVolume() const { return volume_.load(); }
    void SetMuted(bool muted);
    bool IsMuted() const { return muted_.load(); }

    //=========================================================================
    // Current State
    //=========================================================================

    std::string GetCurrentClipId() const;
    std::string GetCurrentSourcePath() const;

private:
    //=========================================================================
    // miniaudio Callbacks
    //=========================================================================

    static void DataCallback(ma_device* device, void* output,
                            const void* input, unsigned int frame_count);

    void ProcessAudio(float* output, unsigned int frame_count);

    //=========================================================================
    // Clip Switching (Multi-track mixing)
    //=========================================================================

    // Get all audible clips at position (from all video and audio tracks)
    std::vector<const OTIOClip*> GetAllAudibleClipsAtTime(double timestamp);

    // Update active sources based on current clips
    void UpdateActiveSources(const std::vector<const OTIOClip*>& clips, double timeline_pos);

    // Get or create decoder for clip (keyed by clip_id to allow same source in multiple clips)
    std::shared_ptr<AudioDecoder> GetDecoderForClip(const std::string& source_path,
                                                     const std::string& clip_id);

    //=========================================================================
    // State
    //=========================================================================

    bool initialized_ = false;
    TimelineFlattener* flattener_ = nullptr;
    PlaybackTimer* timer_ = nullptr;

    // miniaudio device
    ma_device* device_ = nullptr;

    // Clip decoders (keyed by source path)
    std::unordered_map<std::string, std::shared_ptr<AudioDecoder>> decoders_;
    std::mutex decoders_mutex_;

    // Multiple active sources for multi-track mixing
    std::vector<ActiveAudioSource> active_sources_;
    std::mutex source_mutex_;

    // Playback state
    std::atomic<bool> is_playing_{false};
    std::atomic<float> volume_{1.0f};
    std::atomic<bool> muted_{false};
    std::atomic<double> current_position_{0.0};

    // Last known state for change detection
    std::set<std::string> last_clip_ids_;
    double last_position_ = 0.0;

    // Sync configuration
    double sync_check_interval_ms_ = 200.0;
    double sync_threshold_ms_ = 50.0;
    double last_sync_check_time_ = 0.0;
};

} // namespace ump
