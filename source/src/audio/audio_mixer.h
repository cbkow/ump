#pragma once

#include <string>
#include <memory>
#include <vector>
#include <set>
#include <unordered_map>
#include <atomic>
#include <mutex>
#include <thread>
#include <condition_variable>

#include "audio_decoder.h"
#include "audio_time_stretch.h"
// AudioRateCorrector removed - caused crackling artifacts, and mpv-style approach
// is to keep audio pristine and adjust video timing for sync instead

// Forward declare WASAPI device
namespace ump { class WasapiAudioDevice; }

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

    // Force refresh of active sources (call after clip edits like trim/slip)
    // This clears cached clip state and forces re-evaluation on next update
    void ForceRefresh();

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
    // Audio Sync Compensation (Phase 0)
    //=========================================================================

    /**
     * Set display latency compensation from preset or manual value.
     * @param latency_seconds Display latency in seconds (e.g., 0.033 for 60Hz)
     */
    void SetDisplayLatency(double latency_seconds);
    double GetDisplayLatency() const { return display_latency_.load(); }

    /**
     * Set video pipeline latency (D3D11 decoder buffering, GPU upload, etc.)
     * @param latency_seconds Pipeline latency in seconds (e.g., 0.028 for D3D11)
     */
    void SetPipelineLatency(double latency_seconds);
    double GetPipelineLatency() const { return pipeline_latency_.load(); }

    /**
     * Set fine-tune offset for manual A/V sync adjustment.
     * @param offset_seconds Offset in seconds (positive = delay audio)
     */
    void SetFineTuneOffset(double offset_seconds);
    double GetFineTuneOffset() const { return fine_tune_offset_.load(); }

    /**
     * Get the effective total offset being applied.
     * = display_latency + pipeline_latency + fine_tune - wasapi_buffer_latency
     */
    double GetEffectiveOffset() const;

    /**
     * Get current WASAPI buffer latency (automatic, from IAudioClock).
     */
    double GetWasapiLatency() const;

    //=========================================================================
    // Playback Tempo Control (Pitch-Preserving Time Stretch)
    //=========================================================================

    /**
     * Set playback tempo for adaptive speed control.
     * Uses SoundTouch for pitch-preserving time stretch.
     *
     * @param tempo Speed factor: 0.5 = half speed, 1.0 = normal, 2.0 = double
     * Values are clamped to 0.5-2.0 range.
     *
     * When tempo < 1.0, audio slows without pitch change (not chipmunk/slow-mo).
     * When tempo = 1.0, time stretch is bypassed for efficiency.
     *
     * If SoundTouch is not available, this has no effect and the caller
     * should fall back to pausing audio during throttle.
     */
    void SetPlaybackTempo(double tempo);

    /**
     * Get current playback tempo
     */
    double GetPlaybackTempo() const { return playback_tempo_.load(); }

    /**
     * Check if time stretching is available (SoundTouch compiled in)
     */
    bool IsTimeStretchAvailable() const;

    //=========================================================================
    // Current State
    //=========================================================================

    std::string GetCurrentClipId() const;
    std::string GetCurrentSourcePath() const;

private:
    //=========================================================================
    // WASAPI Callbacks
    //=========================================================================

    static void DataCallback(void* device, float* output,
                            uint32_t frame_count, void* userData);

    void ProcessAudio(float* output, unsigned int frame_count);

    //=========================================================================
    // Clip Switching (Multi-track mixing)
    //=========================================================================

    // Get all audible clips at position (from all video and audio tracks)
    // Returns COPIES of clips for thread safety (flattener may replace tracks at any time)
    std::vector<OTIOClip> GetAllAudibleClipsAtTime(double timestamp);

    // Get clips that will become audible within the lookahead window
    // Returns COPIES of clips for thread safety
    std::vector<OTIOClip> GetUpcomingClips(double current_pos, double lookahead_seconds);

    // Update active sources based on current clips
    void UpdateActiveSources(const std::vector<OTIOClip>& clips, double timeline_pos);

    // Pre-warm decoders for upcoming clips (seek and start filling buffer)
    // NOTE: Now runs on background thread - do not call from main thread
    void PrewarmUpcomingClips(double current_pos);

    // Get or create decoder for clip (keyed by clip_id to allow same source in multiple clips)
    std::shared_ptr<AudioDecoder> GetDecoderForClip(const std::string& source_path,
                                                     const std::string& clip_id);

    // Generate a signature for a clip that includes trim points
    // This allows detecting when a clip is slipped/trimmed (same ID, different content)
    static std::string GetClipSignature(const OTIOClip& clip);

    //=========================================================================
    // Background Preparation Thread
    //=========================================================================

    // Background thread that pre-loads and pre-seeks decoders for upcoming clips
    // All expensive FFmpeg operations (Open, Seek) happen on this thread
    void PreparationThreadFunc();

    // Start/stop the background preparation thread
    void StartPreparationThread();
    void StopPreparationThread();

    //=========================================================================
    // State
    //=========================================================================

    bool initialized_ = false;
    TimelineFlattener* flattener_ = nullptr;
    PlaybackTimer* timer_ = nullptr;

    // WASAPI audio device
    std::unique_ptr<WasapiAudioDevice> device_;

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
    // We track "clip signatures" not just IDs - signature includes trim points
    // so we detect when a clip is slipped/trimmed (same ID but different content)
    std::set<std::string> last_clip_signatures_;
    double last_position_ = 0.0;

    // Flag to force refresh on next background thread iteration
    std::atomic<bool> force_refresh_{false};

    // Sync configuration (Phase 1: Tighter Sync Loop)
    double sync_check_interval_ms_ = 50.0;   // Was 200ms, now 50ms
    double sync_threshold_ms_ = 20.0;        // Was 50ms, now 20ms
    double last_sync_check_time_ = 0.0;

    //=========================================================================
    // Audio Sync Compensation
    //=========================================================================

    // Display/pipeline latency compensation
    std::atomic<double> display_latency_{0.012};     // 12ms - tuned after removing swr
    std::atomic<double> pipeline_latency_{0.0};      // Video pipeline latency (D3D11: ~28ms)
    std::atomic<double> fine_tune_offset_{0.0};      // User fine-tune (±50ms range)

    // Mixing buffer (holds mixed audio before time stretch)
    std::vector<float> mix_buffer_;

    // Time stretch for adaptive speed control (pitch-preserving tempo change)
    AudioTimeStretch time_stretch_;
    std::vector<float> time_stretch_buffer_;         // Buffer for time-stretched output
    std::atomic<double> playback_tempo_{1.0};        // Current tempo (0.5-2.0)

    // Phase 3: Position tracking
    std::atomic<uint64_t> samples_output_{0};        // Total samples output to WASAPI
    std::atomic<double> playback_start_position_{0.0}; // Timeline position when playback started

    // Audio lookahead - pre-warm decoders for upcoming clips
    double lookahead_seconds_ = 2.0;  // How far ahead to pre-warm
    double last_prewarm_time_ = 0.0;
    double prewarm_interval_ms_ = 100.0;  // Check for upcoming clips every 100ms

    // Clip change detection - rate limited to reduce main thread work
    double last_clip_check_time_ = 0.0;

    // Warming decoders - being pre-filled before they become active
    // Key: clip_id, Value: {decoder, target_source_position}
    struct WarmingDecoder {
        std::shared_ptr<AudioDecoder> decoder;
        double source_position;  // Where it was seeked to
        std::string source_path;
        double clip_start_time;  // Timeline position where clip starts
    };
    std::unordered_map<std::string, WarmingDecoder> warming_decoders_;
    std::mutex warming_mutex_;

    //=========================================================================
    // Background Preparation Thread State
    //=========================================================================

    std::thread preparation_thread_;
    std::atomic<bool> preparation_thread_running_{false};
    std::condition_variable preparation_cv_;
    std::mutex preparation_mutex_;

    // Pending clips to prepare (written by main thread, read by background thread)
    struct PendingClipInfo {
        std::string clip_id;
        std::string source_path;
        double clip_start_time;
        double source_in;  // Where to seek in the source
    };
    std::vector<PendingClipInfo> pending_clips_;
    std::mutex pending_mutex_;
};

} // namespace ump
