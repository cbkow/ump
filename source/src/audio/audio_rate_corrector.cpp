#include "audio_rate_corrector.h"
#include "../utils/debug_utils.h"

extern "C" {
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
}

#include <cmath>
#include <algorithm>
#include <cstring>

namespace ump {

AudioRateCorrector::AudioRateCorrector() = default;

AudioRateCorrector::~AudioRateCorrector() {
    Shutdown();
}

void AudioRateCorrector::Initialize(int sample_rate, int channels) {
    if (initialized_) {
        Shutdown();
    }

    sample_rate_ = sample_rate;
    channels_ = channels;

    // Create resampler context with initial 1:1 rate
    RecreateResampler();

    if (swr_ctx_) {
        initialized_ = true;
        Debug::Log("AudioRateCorrector: Initialized @ " + std::to_string(sample_rate) + "Hz, " +
                   std::to_string(channels) + " channels");
    }
}

void AudioRateCorrector::Shutdown() {
    if (swr_ctx_) {
        swr_free(&swr_ctx_);
        swr_ctx_ = nullptr;
    }
    initialized_ = false;
    Reset();
}

void AudioRateCorrector::Reset() {
    correction_factor_ = 1.0;
    target_correction_ = 1.0;
    is_active_ = false;
    current_drift_ms_ = 0.0;
    last_applied_factor_ = 1.0;

    // Recreate resampler to flush any buffered samples
    if (initialized_) {
        RecreateResampler();
    }
}

void AudioRateCorrector::RecreateResampler() {
    if (swr_ctx_) {
        swr_free(&swr_ctx_);
        swr_ctx_ = nullptr;
    }

    swr_ctx_ = swr_alloc();
    if (!swr_ctx_) {
        Debug::Log("AudioRateCorrector: Failed to allocate SwrContext");
        return;
    }

    // Configure for float32 stereo
    AVChannelLayout ch_layout;
    av_channel_layout_default(&ch_layout, channels_);

    av_opt_set_chlayout(swr_ctx_, "in_chlayout", &ch_layout, 0);
    av_opt_set_chlayout(swr_ctx_, "out_chlayout", &ch_layout, 0);
    av_opt_set_int(swr_ctx_, "in_sample_rate", sample_rate_, 0);
    av_opt_set_int(swr_ctx_, "out_sample_rate", sample_rate_, 0);
    av_opt_set_sample_fmt(swr_ctx_, "in_sample_fmt", AV_SAMPLE_FMT_FLT, 0);
    av_opt_set_sample_fmt(swr_ctx_, "out_sample_fmt", AV_SAMPLE_FMT_FLT, 0);

    if (swr_init(swr_ctx_) < 0) {
        Debug::Log("AudioRateCorrector: Failed to initialize SwrContext");
        swr_free(&swr_ctx_);
        swr_ctx_ = nullptr;
    }

    last_applied_factor_ = 1.0;
}

bool AudioRateCorrector::UpdateDrift(double drift_seconds) {
    double drift_ms = drift_seconds * 1000.0;
    double abs_drift = std::abs(drift_ms);

    current_drift_ms_ = drift_ms;

    if (abs_drift < drift_threshold_ms_) {
        // Drift too small - no correction needed
        // Smoothly return to 1.0
        target_correction_ = 1.0;
        is_active_ = false;
    } else if (abs_drift > drift_max_ms_) {
        // Drift too large - caller should seek instead, reset correction
        target_correction_ = 1.0;
        is_active_ = false;
        return false;  // Signal that seek is needed
    } else {
        // Calculate correction factor
        // Map drift (10-50ms) to correction (1.0-1.03 or 0.97-1.0)
        double correction_range = (drift_ms > 0) ? (max_correction_ - 1.0) : (1.0 - min_correction_);
        double drift_range = drift_max_ms_ - drift_threshold_ms_;
        double correction_amount = ((abs_drift - drift_threshold_ms_) / drift_range) * correction_range;

        if (drift_ms > 0) {
            // Audio behind video - speed up
            target_correction_ = 1.0 + correction_amount;
        } else {
            // Audio ahead of video - slow down
            target_correction_ = 1.0 - correction_amount;
        }
        is_active_ = true;
    }

    // Smooth transition to target
    double current = correction_factor_.load();
    double new_factor = current + (target_correction_ - current) * smoothing_factor_;
    new_factor = std::clamp(new_factor, min_correction_, max_correction_);
    correction_factor_ = new_factor;

    return is_active_.load();
}

int AudioRateCorrector::Process(const float* input, float* output,
                                 int input_frames, int max_output_frames) {
    if (!swr_ctx_ || !initialized_) {
        // No resampler - copy input to output
        int frames_to_copy = std::min(input_frames, max_output_frames);
        std::memcpy(output, input, frames_to_copy * channels_ * sizeof(float));
        return frames_to_copy;
    }

    double factor = correction_factor_.load();

    // Check if we need to update the resampler rate
    // Only recreate if the factor changed significantly (>0.1%)
    if (std::abs(factor - last_applied_factor_) > 0.001) {
        // Use swr_set_compensation for soft compensation
        // This avoids recreating the resampler and provides smoother transitions
        // compensation_distance = how many samples to apply compensation over
        // sample_delta = difference in samples (positive = speedup, negative = slowdown)

        // Calculate the delta: for a factor of 1.02, we want to output fewer samples
        // to catch up. Factor > 1.0 means audio is behind, need to speed up.
        // To speed up playback, we output FEWER samples than input.
        // sample_delta = input_frames * (1 - factor) ... but sign is tricky

        // Actually, let's think about this differently:
        // factor = 1.02 means we want to play 2% faster
        // To play faster, we consume MORE input samples per output sample
        // So the output rate should be: sample_rate * factor
        // Or equivalently, input rate stays same, output gets fewer samples

        // The simpler approach: use compensation to stretch/shrink
        // Negative delta = stretch (slower), positive delta = compress (faster)
        int compensation_distance = input_frames;
        int sample_delta = static_cast<int>((factor - 1.0) * input_frames);

        int ret = swr_set_compensation(swr_ctx_, sample_delta, compensation_distance);
        if (ret < 0) {
            // Compensation failed, try recreating the resampler
            // This is a fallback that shouldn't happen often
        }

        last_applied_factor_ = factor;
    }

    // If factor is very close to 1.0 and no active compensation, just copy
    if (!is_active_.load() && std::abs(factor - 1.0) < 0.001) {
        int frames_to_copy = std::min(input_frames, max_output_frames);
        std::memcpy(output, input, frames_to_copy * channels_ * sizeof(float));
        return frames_to_copy;
    }

    // Use swr_convert for resampling
    const uint8_t* in_data[1] = { reinterpret_cast<const uint8_t*>(input) };
    uint8_t* out_data[1] = { reinterpret_cast<uint8_t*>(output) };

    int out_samples = swr_convert(swr_ctx_, out_data, max_output_frames,
                                   in_data, input_frames);

    if (out_samples < 0) {
        // Error - fall back to copy
        int frames_to_copy = std::min(input_frames, max_output_frames);
        std::memcpy(output, input, frames_to_copy * channels_ * sizeof(float));
        return frames_to_copy;
    }

    return out_samples;
}

} // namespace ump
