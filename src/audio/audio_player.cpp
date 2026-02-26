#include "audio_player.h"
#include "wasapi_audio_device.h"
#include "../player/playback_timer.h"
#include "../utils/debug_utils.h"

#include <cstring>
#include <cmath>
#include <chrono>

namespace qcview {

//=============================================================================
// Constructor / Destructor
//=============================================================================

AudioPlayer::AudioPlayer() = default;

AudioPlayer::~AudioPlayer() {
    Shutdown();
}

//=============================================================================
// Lifecycle
//=============================================================================

bool AudioPlayer::Initialize() {
    if (initialized_) {
        return true;
    }

    Debug::Log("AudioPlayer: Initializing WASAPI...");

    // Create WASAPI device
    device_ = std::make_unique<WasapiAudioDevice>();

    // Configure device
    WasapiDeviceConfig config;
    config.dataCallback = DataCallback;
    config.userData = this;
    config.sampleRate = 48000;
    config.channels = 2;
    config.bufferSizeMs = 10;

    // Initialize device
    if (!device_->Initialize(config)) {
        Debug::Log("AudioPlayer: Failed to initialize WASAPI device");
        device_.reset();
        return false;
    }

    initialized_ = true;
    Debug::Log("AudioPlayer: Initialized successfully");
    return true;
}

void AudioPlayer::Shutdown() {
    if (!initialized_) return;

    Debug::Log("AudioPlayer: Shutting down...");

    Stop();
    UnloadClip();

    if (device_) {
        device_->Shutdown();
        device_.reset();
    }

    initialized_ = false;
    Debug::Log("AudioPlayer: Shutdown complete");
}

bool AudioPlayer::LoadClip(const std::string& file_path, const AudioClipConfig& config) {
    if (!initialized_) {
        Debug::Log("AudioPlayer: Cannot load clip - not initialized");
        return false;
    }

    Debug::Log("AudioPlayer: Loading clip " + file_path);

    // Stop current playback
    Stop();

    // Unload previous clip
    UnloadClip();

    // Create new decoder
    decoder_ = std::make_unique<AudioDecoder>();

    if (!decoder_->Open(file_path)) {
        Debug::Log("AudioPlayer: Failed to open audio from " + file_path);
        decoder_.reset();
        return false;
    }

    // Set clip configuration
    clip_config_ = config;

    // Set source duration if not specified
    if (clip_config_.source_duration <= 0) {
        clip_config_.source_duration = decoder_->GetDuration();
    }

    // Validate trim points
    if (clip_config_.source_out < 0 || clip_config_.source_out > clip_config_.source_duration) {
        clip_config_.source_out = clip_config_.source_duration;
    }

    // Start decoder thread
    decoder_->Start();

    // Seek to initial position (source_in)
    decoder_->Seek(clip_config_.source_in);
    current_position_ = clip_config_.position_offset;

    Debug::Log("AudioPlayer: Clip loaded - duration: " + std::to_string(decoder_->GetDuration()) +
               "s, trim: " + std::to_string(clip_config_.source_in) + "-" +
               std::to_string(clip_config_.source_out) + ", offset: " +
               std::to_string(clip_config_.position_offset));

    return true;
}

void AudioPlayer::UnloadClip() {
    if (decoder_) {
        decoder_->Stop();
        decoder_->Close();
        decoder_.reset();
    }

    clip_config_ = AudioClipConfig{};
    current_position_ = 0.0;
}

bool AudioPlayer::HasClip() const {
    return decoder_ && decoder_->IsOpen();
}

bool AudioPlayer::HasAudio() const {
    return decoder_ && decoder_->HasAudio();
}

//=============================================================================
// Playback Control
//=============================================================================

void AudioPlayer::Play() {
    if (!initialized_ || !device_ || is_playing_) return;

    Debug::Log("AudioPlayer: Play");

    // Start the audio device
    device_->Start();

    is_playing_ = true;

    // Record time for sync checking
    last_sync_check_time_ = std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

void AudioPlayer::Pause() {
    if (!initialized_ || !device_ || !is_playing_) return;

    Debug::Log("AudioPlayer: Pause");

    device_->Stop();
    is_playing_ = false;
}

void AudioPlayer::Stop() {
    if (!initialized_ || !device_) return;

    Debug::Log("AudioPlayer: Stop");

    device_->Stop();
    is_playing_ = false;

    // Reset position to start
    current_position_ = clip_config_.position_offset;

    if (decoder_) {
        decoder_->Seek(clip_config_.source_in);
    }
}

//=============================================================================
// Timeline Sync
//=============================================================================

void AudioPlayer::SetTimer(PlaybackTimer* timer) {
    timer_ = timer;
}

void AudioPlayer::Update() {
    if (!timer_ || !is_playing_ || !decoder_) return;

    // Get current timeline position from timer
    double timeline_pos = timer_->GetPosition();
    current_position_ = timeline_pos;

    // Check if it's time for a sync check
    double now = std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    double elapsed_ms = (now - last_sync_check_time_) * 1000.0;
    if (elapsed_ms < sync_check_interval_ms_) {
        return;  // Not time yet
    }

    last_sync_check_time_ = now;

    // Check sync
    CheckAndCorrectSync();
}

void AudioPlayer::CheckAndCorrectSync() {
    if (!decoder_ || !timer_) return;

    double timeline_pos = timer_->GetPosition();

    // Check if we're in a gap
    if (IsInGap(timeline_pos)) {
        // In a gap - no sync correction needed
        return;
    }

    // Calculate expected source position
    double expected_source_pos = clip_config_.TimelineToSource(timeline_pos);
    if (expected_source_pos < 0) return;

    // Get current decode position
    double audio_pos = decoder_->GetReadPosition();

    // Calculate drift
    double drift_ms = std::abs(audio_pos - expected_source_pos) * 1000.0;

    if (drift_ms > sync_threshold_ms_ &&
        decoder_->GetBufferedDuration() > 0.1 &&
        decoder_->SecondsSinceLastSeek() > 0.3) {
        // Only seek if:
        // 1. Buffer has data (not still filling from a recent seek)
        // 2. Enough time has passed since last seek (cooldown)
        // Without cooldown, compressed codecs like AAC enter a
        // seek storm: seek → flush → refill → drift detected → seek
        Debug::Log("AudioPlayer: Sync correction - drift: " + std::to_string(drift_ms) +
                   "ms, seeking to " + std::to_string(expected_source_pos) + "s");
        decoder_->Seek(expected_source_pos);
    }
}

void AudioPlayer::Seek(double position) {
    if (!decoder_) return;

    current_position_ = position;

    double source_pos = clip_config_.TimelineToSource(position);
    if (source_pos >= 0) {
        decoder_->Seek(source_pos);
    }
}

double AudioPlayer::GetPosition() const {
    return current_position_.load();
}

bool AudioPlayer::IsInGap(double timeline_pos) const {
    return !clip_config_.IsInRange(timeline_pos);
}

//=============================================================================
// Volume Control
//=============================================================================

void AudioPlayer::SetVolume(float volume) {
    volume_ = std::max(0.0f, std::min(1.0f, volume));
}

void AudioPlayer::SetMuted(bool muted) {
    muted_ = muted;
}

//=============================================================================
// Clip Configuration
//=============================================================================

void AudioPlayer::SetClipConfig(const AudioClipConfig& config) {
    clip_config_ = config;

    // Re-seek if we have a decoder
    if (decoder_ && is_playing_) {
        double source_pos = clip_config_.TimelineToSource(current_position_);
        if (source_pos >= 0) {
            decoder_->Seek(source_pos);
        }
    }
}

//=============================================================================
// WASAPI Callbacks
//=============================================================================

void AudioPlayer::DataCallback(void* /*device*/, float* output,
                               uint32_t frame_count, void* userData) {
    AudioPlayer* player = static_cast<AudioPlayer*>(userData);
    if (player) {
        player->ProcessAudio(output, frame_count);
    } else {
        // No player - fill with silence
        std::memset(output, 0, frame_count * 2 * sizeof(float));
    }
}

void AudioPlayer::ProcessAudio(float* output, unsigned int frame_count) {
    diag_callback_count_++;

    // Check if we should output silence
    if (!is_playing_ || !decoder_ || muted_ || !decoder_->HasAudio()) {
        std::memset(output, 0, frame_count * 2 * sizeof(float));
        diag_silence_frames_ += frame_count;
        return;
    }

    // Check if we're in a gap (before clip starts or after clip ends)
    double timeline_pos = current_position_.load();
    if (IsInGap(timeline_pos)) {
        std::memset(output, 0, frame_count * 2 * sizeof(float));
        diag_silence_frames_ += frame_count;
        return;
    }

    // Read from decoder
    size_t frames_read = decoder_->Read(output, frame_count);
    diag_frames_read_ += frames_read;
    diag_frames_output_ += frame_count;

    // Apply volume with soft limiting
    float vol = volume_.load();

    // Soft limiter - same as AudioMixer for consistency
    auto softLimit = [](float x) -> float {
        constexpr float threshold = 0.8f;
        if (x > threshold) {
            float excess = x - threshold;
            return threshold + (1.0f - threshold) * (excess / (1.0f + excess));
        } else if (x < -threshold) {
            float excess = -x - threshold;
            return -threshold - (1.0f - threshold) * (excess / (1.0f + excess));
        }
        return x;
    };

    size_t sample_count = frame_count * 2;  // Stereo
    for (size_t i = 0; i < sample_count; ++i) {
        output[i] = softLimit(output[i] * vol);
    }

    // Update position estimate (rough, for display purposes)
    // The actual sync is done in Update() using the timer
    if (frames_read > 0) {
        double seconds_played = static_cast<double>(frames_read) / 48000.0;
        current_position_ = current_position_.load() + seconds_played;
    }
}

} // namespace qcview
