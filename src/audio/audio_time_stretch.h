#pragma once

#include <atomic>
#include <vector>
#include <cstdint>

// Forward declare SoundTouch in its namespace
namespace soundtouch { class SoundTouch; }

namespace qcview {

/**
 * AudioTimeStretch - Pitch-preserving time stretch using SoundTouch
 *
 * Allows tempo changes (0.5x-2.0x) without pitch shift for adaptive
 * speed control during playback. When the cache falls behind, video
 * slows down and audio needs to slow proportionally without sounding
 * like a chipmunk/slow-mo effect.
 *
 * Integrates into the AudioMixer pipeline BEFORE rate correction:
 *   Decoders -> Mixing -> [TimeStretch] -> RateCorrection -> Volume -> WASAPI
 *
 * Uses SoundTouch library for PSOLA-based time stretching. If SoundTouch
 * is not available (QCVIEW_HAS_SOUNDTOUCH not defined), falls back to
 * passthrough mode (audio pauses during throttle instead).
 */
class AudioTimeStretch {
public:
    AudioTimeStretch();
    ~AudioTimeStretch();

    // Prevent copying
    AudioTimeStretch(const AudioTimeStretch&) = delete;
    AudioTimeStretch& operator=(const AudioTimeStretch&) = delete;

    /**
     * Initialize the time stretcher
     * @param sample_rate Audio sample rate (e.g., 48000)
     * @param channels Number of audio channels (e.g., 2 for stereo)
     * @return true if initialization succeeded
     */
    bool Initialize(int sample_rate, int channels);

    /**
     * Shutdown and free resources
     */
    void Shutdown();

    /**
     * Check if initialized
     */
    bool IsInitialized() const { return initialized_; }

    /**
     * Set tempo (playback speed without pitch change)
     * @param tempo Tempo multiplier: 0.5 = half speed, 1.0 = normal, 2.0 = double
     * Values are clamped to 0.5-2.0 range (SoundTouch limitation)
     */
    void SetTempo(double tempo);

    /**
     * Get current tempo setting
     */
    double GetTempo() const { return current_tempo_.load(); }

    /**
     * Check if time stretching is active (tempo != 1.0)
     */
    bool IsActive() const { return std::abs(current_tempo_.load() - 1.0) > 0.001; }

    /**
     * Process audio samples with time stretching
     *
     * SoundTouch is a non-real-time algorithm: output frame count may differ
     * from input. During slowdown, fewer output frames are produced per input.
     * The caller must handle variable output sizes.
     *
     * @param input Input samples (interleaved float, channels * input_frames)
     * @param output Output buffer (must be at least max_output_frames * channels)
     * @param input_frames Number of input frames
     * @param max_output_frames Maximum frames the output buffer can hold
     * @return Actual number of output frames produced
     */
    int Process(const float* input, float* output, int input_frames, int max_output_frames);

    /**
     * Reset internal state (call after seek)
     * Clears SoundTouch's internal buffers
     */
    void Reset();

    /**
     * Flush remaining samples from SoundTouch buffer
     * Call when stopping playback or before seek
     */
    void Flush();

    /**
     * Get approximate latency added by time stretching in samples
     * Useful for A/V sync compensation
     */
    int GetLatencySamples() const;

private:
    soundtouch::SoundTouch* st_ = nullptr;
    int sample_rate_ = 48000;
    int channels_ = 2;
    bool initialized_ = false;

    // Atomic for thread-safe tempo access (set from main thread, read from audio thread)
    std::atomic<double> current_tempo_{1.0};
    std::atomic<double> pending_tempo_{1.0};

    // Smoothing to avoid pops when tempo changes
    double smoothing_factor_ = 0.1;  // 10% per update toward target

    // Internal buffer for SoundTouch processing
    std::vector<float> process_buffer_;
};

} // namespace qcview
