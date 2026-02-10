#include "audio_time_stretch.h"
#include "../utils/debug_utils.h"

#ifdef UMP_HAS_SOUNDTOUCH
#include <SoundTouch.h>
#endif

#include <cmath>
#include <algorithm>
#include <cstring>

namespace ump {

AudioTimeStretch::AudioTimeStretch() = default;

AudioTimeStretch::~AudioTimeStretch() {
    Shutdown();
}

bool AudioTimeStretch::Initialize(int sample_rate, int channels) {
    if (initialized_) {
        Shutdown();
    }

#ifdef UMP_HAS_SOUNDTOUCH
    st_ = new soundtouch::SoundTouch();

    st_->setSampleRate(sample_rate);
    st_->setChannels(channels);

    // Quality settings optimized for real-time playback
    // Use QUICKSEEK for lower latency (trades some quality for speed)
    st_->setSetting(SETTING_USE_QUICKSEEK, 1);

    // Sequence/overlap parameters for time-stretch quality
    // These are tuned for music/speech - good for general video audio
    st_->setSetting(SETTING_SEQUENCE_MS, 40);     // Processing sequence length
    st_->setSetting(SETTING_SEEKWINDOW_MS, 15);   // Seek window length
    st_->setSetting(SETTING_OVERLAP_MS, 8);       // Overlap length

    // Start at normal tempo
    st_->setTempo(1.0);
    st_->setPitch(1.0);  // Keep pitch constant (that's the whole point!)

    sample_rate_ = sample_rate;
    channels_ = channels;

    // Allocate process buffer - enough for typical audio callback + expansion room
    // When slowing down, we may need more output space
    process_buffer_.resize(8192 * channels);

    initialized_ = true;
    current_tempo_ = 1.0;
    pending_tempo_ = 1.0;

    Debug::Log("AudioTimeStretch: Initialized with SoundTouch @ " +
               std::to_string(sample_rate) + "Hz, " +
               std::to_string(channels) + " channels");

    return true;
#else
    // SoundTouch not available - tempo control disabled
    sample_rate_ = sample_rate;
    channels_ = channels;
    initialized_ = false;

    Debug::Log("AudioTimeStretch: SoundTouch not available - tempo control disabled");
    return false;
#endif
}

void AudioTimeStretch::Shutdown() {
#ifdef UMP_HAS_SOUNDTOUCH
    if (st_) {
        delete st_;
        st_ = nullptr;
    }
#endif
    initialized_ = false;
    current_tempo_ = 1.0;
    pending_tempo_ = 1.0;
    process_buffer_.clear();
}

void AudioTimeStretch::SetTempo(double tempo) {
    // Clamp to SoundTouch's valid range
    tempo = std::max(0.5, std::min(tempo, 2.0));
    pending_tempo_ = tempo;
}

void AudioTimeStretch::Reset() {
#ifdef UMP_HAS_SOUNDTOUCH
    if (st_) {
        st_->clear();
    }
#endif
    current_tempo_ = 1.0;
    pending_tempo_ = 1.0;
}

void AudioTimeStretch::Flush() {
#ifdef UMP_HAS_SOUNDTOUCH
    if (st_) {
        st_->flush();
    }
#endif
}

int AudioTimeStretch::GetLatencySamples() const {
#ifdef UMP_HAS_SOUNDTOUCH
    if (st_) {
        // SoundTouch doesn't expose latency directly, but it's roughly:
        // sequence_ms * sample_rate / 1000
        // With SEQUENCE_MS=40, that's about 40ms * 48000 / 1000 = 1920 samples
        return static_cast<int>(0.040 * sample_rate_);
    }
#endif
    return 0;
}

int AudioTimeStretch::Process(const float* input, float* output,
                               int input_frames, int max_output_frames) {
#ifdef UMP_HAS_SOUNDTOUCH
    if (!st_ || !initialized_) {
        // Fallback: direct copy (passthrough)
        int copy_frames = std::min(input_frames, max_output_frames);
        std::memcpy(output, input, copy_frames * channels_ * sizeof(float));
        return copy_frames;
    }

    // Smooth tempo transition to avoid audio pops/clicks
    // Use faster smoothing (30%) since throttle changes can be frequent
    double target = pending_tempo_.load();
    double current = current_tempo_.load();
    if (std::abs(target - current) > 0.001) {
        // Interpolate toward target (30% per callback = ~3 callbacks to converge)
        current += (target - current) * 0.3;

        // Clamp to valid range
        current = std::max(0.5, std::min(current, 2.0));

        current_tempo_ = current;
        st_->setTempo(static_cast<float>(current));
    }

    // If tempo is effectively 1.0, bypass SoundTouch for efficiency
    if (std::abs(current - 1.0) < 0.001) {
        int copy_frames = std::min(input_frames, max_output_frames);
        std::memcpy(output, input, copy_frames * channels_ * sizeof(float));
        return copy_frames;
    }

    // Feed input samples to SoundTouch
    st_->putSamples(input, input_frames);

    // Receive time-stretched output
    // SoundTouch may return fewer or more samples than input depending on tempo
    int received = st_->receiveSamples(output, max_output_frames);

    return received;
#else
    // SoundTouch not available - direct passthrough
    int copy_frames = std::min(input_frames, max_output_frames);
    std::memcpy(output, input, copy_frames * channels_ * sizeof(float));
    return copy_frames;
#endif
}

} // namespace ump
