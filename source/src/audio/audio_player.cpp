// Define miniaudio implementation in this file
#define MINIAUDIO_IMPLEMENTATION
#include "../../external/miniaudio/miniaudio.h"

#include "audio_player.h"
#include "../player/playback_timer.h"
#include "../utils/debug_utils.h"

#include <cstring>
#include <cmath>
#include <chrono>

namespace ump {

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

    Debug::Log("AudioPlayer: Initializing miniaudio...");

    // Allocate device
    device_ = new ma_device();

    // Configure device
    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = ma_format_f32;
    config.playback.channels = 2;
    config.sampleRate = 48000;
    config.dataCallback = DataCallback;
    config.pUserData = this;

    // Initialize device
    ma_result result = ma_device_init(nullptr, &config, device_);
    if (result != MA_SUCCESS) {
        Debug::Log("AudioPlayer: Failed to initialize miniaudio device, error: " + std::to_string(result));
        delete device_;
        device_ = nullptr;
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
        ma_device_uninit(device_);
        delete device_;
        device_ = nullptr;
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
    ma_result result = ma_device_start(device_);
    if (result != MA_SUCCESS) {
        Debug::Log("AudioPlayer: Failed to start device, error: " + std::to_string(result));
        return;
    }

    is_playing_ = true;

    // Record time for sync checking
    last_sync_check_time_ = std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

void AudioPlayer::Pause() {
    if (!initialized_ || !device_ || !is_playing_) return;

    Debug::Log("AudioPlayer: Pause");

    ma_device_stop(device_);
    is_playing_ = false;
}

void AudioPlayer::Stop() {
    if (!initialized_ || !device_) return;

    Debug::Log("AudioPlayer: Stop");

    ma_device_stop(device_);
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

    if (drift_ms > sync_threshold_ms_) {
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
// miniaudio Callbacks
//=============================================================================

void AudioPlayer::DataCallback(ma_device* device, void* output,
                               const void* /*input*/, unsigned int frame_count) {
    AudioPlayer* player = static_cast<AudioPlayer*>(device->pUserData);
    if (player) {
        player->ProcessAudio(static_cast<float*>(output), frame_count);
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

    // Apply volume
    float vol = volume_.load();
    if (vol < 1.0f) {
        size_t sample_count = frame_count * 2;  // Stereo
        for (size_t i = 0; i < sample_count; ++i) {
            output[i] *= vol;
        }
    }

    // Update position estimate (rough, for display purposes)
    // The actual sync is done in Update() using the timer
    if (frames_read > 0) {
        double seconds_played = static_cast<double>(frames_read) / 48000.0;
        current_position_ = current_position_.load() + seconds_played;
    }
}

} // namespace ump
