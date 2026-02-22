#pragma once

#include <atomic>
#include <vector>
#include <cstdint>

// Forward declarations - avoid including FFmpeg headers here
struct SwrContext;

namespace qcview {

/**
 * AudioRateCorrector - Smooth drift correction via sample rate adjustment
 *
 * Uses libswresample to slightly speed up or slow down audio playback
 * to correct for A/V drift without audible seeks.
 *
 * Correction tiers:
 *   < 10ms drift: No correction (imperceptible)
 *   10-50ms drift: Sample rate micro-adjustment (±2-3%)
 *   > 50ms drift: Should use seek instead (handled by AudioMixer)
 */
class AudioRateCorrector {
public:
    AudioRateCorrector();
    ~AudioRateCorrector();

    // Prevent copying
    AudioRateCorrector(const AudioRateCorrector&) = delete;
    AudioRateCorrector& operator=(const AudioRateCorrector&) = delete;

    /**
     * Initialize the rate corrector
     * @param sample_rate Base sample rate (e.g., 48000)
     * @param channels Number of audio channels (e.g., 2)
     */
    void Initialize(int sample_rate, int channels);

    /**
     * Shutdown and free resources
     */
    void Shutdown();

    /**
     * Check if initialized
     */
    bool IsInitialized() const { return initialized_; }

    /**
     * Update drift measurement and calculate correction factor
     * @param drift_seconds Current audio drift (positive = audio behind video)
     * @return true if correction is active (10-50ms drift), false if no correction needed
     */
    bool UpdateDrift(double drift_seconds);

    /**
     * Get current correction factor (e.g., 1.02 = 2% speedup)
     */
    double GetCorrectionFactor() const { return correction_factor_.load(); }

    /**
     * Process audio samples with rate correction
     * @param input Input samples (interleaved float)
     * @param output Output buffer (may be different size than input!)
     * @param input_frames Number of input frames
     * @param max_output_frames Maximum output frames buffer can hold
     * @return Actual number of output frames produced
     */
    int Process(const float* input, float* output, int input_frames, int max_output_frames);

    /**
     * Check if correction is currently active
     */
    bool IsActive() const { return is_active_.load(); }

    /**
     * Reset correction state (call after seek)
     */
    void Reset();

    /**
     * Get current drift in milliseconds (for debugging)
     */
    double GetCurrentDriftMs() const { return current_drift_ms_.load(); }

private:
    void RecreateResampler();

    SwrContext* swr_ctx_ = nullptr;
    int sample_rate_ = 48000;
    int channels_ = 2;
    bool initialized_ = false;

    std::atomic<double> correction_factor_{1.0};
    std::atomic<bool> is_active_{false};
    std::atomic<double> current_drift_ms_{0.0};

    // Smoothing parameters
    double smoothing_factor_ = 0.1;        // How fast to adjust (0.1 = 10% per update)
    double target_correction_ = 1.0;       // Target correction factor
    double max_correction_ = 1.03;         // Maximum 3% speedup
    double min_correction_ = 0.97;         // Maximum 3% slowdown
    double drift_threshold_ms_ = 10.0;     // Don't correct below this
    double drift_max_ms_ = 50.0;           // Seek instead above this

    // Resampler state tracking
    double last_applied_factor_ = 1.0;     // Last factor applied to resampler
};

} // namespace qcview
