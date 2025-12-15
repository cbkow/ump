#include "audio_mixer.h"

// Include miniaudio (implementation already in audio_player.cpp)
#include "../../external/miniaudio/miniaudio.h"

#include "../timeline/timeline_view.h"
#include "../player/playback_timer.h"
#include "../utils/debug_utils.h"

#include <cstring>
#include <cmath>
#include <chrono>
#include <algorithm>

namespace ump {

//=============================================================================
// Constructor / Destructor
//=============================================================================

AudioMixer::AudioMixer() = default;

AudioMixer::~AudioMixer() {
    Shutdown();
}

//=============================================================================
// Lifecycle
//=============================================================================

bool AudioMixer::Initialize() {
    if (initialized_) {
        return true;
    }

    Debug::Log("AudioMixer: Initializing...");

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
        Debug::Log("AudioMixer: Failed to initialize miniaudio device, error: " + std::to_string(result));
        delete device_;
        device_ = nullptr;
        return false;
    }

    initialized_ = true;
    Debug::Log("AudioMixer: Initialized successfully");
    return true;
}

void AudioMixer::Shutdown() {
    if (!initialized_) return;

    Debug::Log("AudioMixer: Shutting down...");

    Stop();
    ClearClips();

    if (device_) {
        ma_device_uninit(device_);
        delete device_;
        device_ = nullptr;
    }

    flattener_ = nullptr;
    timer_ = nullptr;
    initialized_ = false;

    Debug::Log("AudioMixer: Shutdown complete");
}

//=============================================================================
// Timeline Integration
//=============================================================================

void AudioMixer::SetFlattener(TimelineFlattener* flattener) {
    flattener_ = flattener;
}

void AudioMixer::SetTimer(PlaybackTimer* timer) {
    timer_ = timer;
}

void AudioMixer::PreloadClips(const std::vector<OTIOClip>& clips) {
    Debug::Log("AudioMixer: Preloading audio for " + std::to_string(clips.size()) + " clips");

    std::lock_guard<std::mutex> lock(decoders_mutex_);

    for (const auto& clip : clips) {
        // Skip gaps and unlinked clips
        if (clip.is_gap || !clip.is_linked || clip.linked_path.empty()) {
            continue;
        }

        // Skip if already loaded
        if (decoders_.find(clip.linked_path) != decoders_.end()) {
            continue;
        }

        // Create decoder
        auto decoder = std::make_shared<AudioDecoder>();
        if (decoder->Open(clip.linked_path)) {
            decoder->Start();
            decoders_[clip.linked_path] = decoder;
            Debug::Log("AudioMixer: Preloaded audio for " + clip.linked_path);
        } else {
            Debug::Log("AudioMixer: No audio in " + clip.linked_path);
        }
    }

    Debug::Log("AudioMixer: Preloaded " + std::to_string(decoders_.size()) + " audio decoders");
}

void AudioMixer::ClearClips() {
    std::lock_guard<std::mutex> lock(decoders_mutex_);

    // Stop all decoders
    for (auto& pair : decoders_) {
        if (pair.second) {
            pair.second->Stop();
            pair.second->Close();
        }
    }
    decoders_.clear();

    // Clear current source
    {
        std::lock_guard<std::mutex> source_lock(source_mutex_);
        current_source_ = ActiveAudioSource{};
        outgoing_decoder_ = nullptr;
    }

    last_clip_id_.clear();
}

//=============================================================================
// Playback Control
//=============================================================================

void AudioMixer::Play() {
    if (!initialized_ || !device_ || is_playing_) return;

    Debug::Log("AudioMixer: Play");

    ma_result result = ma_device_start(device_);
    if (result != MA_SUCCESS) {
        Debug::Log("AudioMixer: Failed to start device, error: " + std::to_string(result));
        return;
    }

    is_playing_ = true;

    last_sync_check_time_ = std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

void AudioMixer::Pause() {
    if (!initialized_ || !device_ || !is_playing_) return;

    Debug::Log("AudioMixer: Pause");

    ma_device_stop(device_);
    is_playing_ = false;
}

void AudioMixer::Stop() {
    if (!initialized_ || !device_) return;

    Debug::Log("AudioMixer: Stop");

    ma_device_stop(device_);
    is_playing_ = false;

    // Reset position
    current_position_ = 0.0;
    last_clip_id_.clear();

    // Clear current source
    {
        std::lock_guard<std::mutex> lock(source_mutex_);
        current_source_ = ActiveAudioSource{};
        outgoing_decoder_ = nullptr;
    }
}

//=============================================================================
// Update
//=============================================================================

void AudioMixer::Update() {
    if (!flattener_) return;

    // Get timeline position from timer or use directly-set position
    double timeline_pos;
    if (timer_) {
        timeline_pos = timer_->GetPosition();
        current_position_ = timeline_pos;
    } else {
        timeline_pos = current_position_.load();
    }

    // Get top-layer clip at current position
    const OTIOClip* clip = GetTopLayerClipAtTime(timeline_pos);

    // Check if clip changed
    std::string clip_id = clip ? clip->id : "";
    if (clip_id != last_clip_id_) {
        SwitchToClip(clip, timeline_pos);
        last_clip_id_ = clip_id;
    }

    // Check sync periodically
    double now = std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    double elapsed_ms = (now - last_sync_check_time_) * 1000.0;
    if (elapsed_ms >= sync_check_interval_ms_) {
        last_sync_check_time_ = now;

        // Check sync for current decoder
        std::lock_guard<std::mutex> lock(source_mutex_);
        if (current_source_.decoder) {
            // Calculate expected source position
            double expected_source = current_source_.source_in +
                (timeline_pos - current_source_.clip_start_time);

            // Clamp to valid range
            expected_source = std::max(current_source_.source_in,
                std::min(expected_source, current_source_.source_out));

            double actual_pos = current_source_.decoder->GetReadPosition();
            double drift_ms = std::abs(actual_pos - expected_source) * 1000.0;

            if (drift_ms > sync_threshold_ms_) {
                Debug::Log("AudioMixer: Sync correction - drift: " + std::to_string(drift_ms) +
                           "ms, seeking to " + std::to_string(expected_source));
                current_source_.decoder->Seek(expected_source);
            }
        }
    }

    last_position_ = timeline_pos;
}

void AudioMixer::Seek(double position) {
    current_position_ = position;

    // Get clip at seek position and switch if needed
    const OTIOClip* clip = GetTopLayerClipAtTime(position);
    std::string clip_id = clip ? clip->id : "";

    if (clip_id != last_clip_id_) {
        SwitchToClip(clip, position);
        last_clip_id_ = clip_id;
    } else if (clip) {
        // Same clip, just seek the decoder
        std::lock_guard<std::mutex> lock(source_mutex_);
        if (current_source_.decoder) {
            double source_pos = current_source_.source_in +
                (position - current_source_.clip_start_time);
            source_pos = std::max(current_source_.source_in,
                std::min(source_pos, current_source_.source_out));
            current_source_.decoder->Seek(source_pos);
        }
    }
}

//=============================================================================
// Clip Switching
//=============================================================================

const OTIOClip* AudioMixer::GetTopLayerClipAtTime(double timestamp) {
    if (!flattener_) return nullptr;

    // Use GetAudibleClipAtTime which respects track audio_muted flag
    // This allows users to mute audio from specific video tracks
    return flattener_->GetAudibleClipAtTime(timestamp);
}

void AudioMixer::SwitchToClip(const OTIOClip* clip, double timeline_pos) {
    std::lock_guard<std::mutex> lock(source_mutex_);

    // Start crossfade from current decoder
    if (current_source_.decoder && crossfade_position_ >= 1.0f) {
        outgoing_decoder_ = current_source_.decoder;
        crossfade_position_ = 0.0f;
    }

    if (!clip || clip->is_gap || !clip->is_linked) {
        // No clip at this position - clear current source
        current_source_ = ActiveAudioSource{};
        Debug::Log("AudioMixer: Switched to gap/empty at " + std::to_string(timeline_pos));
        return;
    }

    // Get decoder for this clip
    auto decoder = GetDecoderForClip(clip->linked_path);
    if (!decoder) {
        current_source_ = ActiveAudioSource{};
        Debug::Log("AudioMixer: No audio decoder for " + clip->linked_path);
        return;
    }

    // Setup new source
    current_source_.clip_id = clip->id;
    current_source_.source_path = clip->linked_path;
    current_source_.clip_start_time = clip->start_time;
    current_source_.clip_duration = clip->duration;
    current_source_.source_in = clip->source_in;
    current_source_.source_out = clip->source_out > 0 ? clip->source_out : decoder->GetDuration();
    current_source_.decoder = decoder;

    // Seek decoder to correct position within clip
    double source_pos = current_source_.source_in + (timeline_pos - clip->start_time);
    source_pos = std::max(current_source_.source_in,
        std::min(source_pos, current_source_.source_out));
    decoder->Seek(source_pos);

    Debug::Log("AudioMixer: Switched to clip " + clip->id + " at " +
               std::to_string(source_pos) + "s (timeline " + std::to_string(timeline_pos) + "s)");
}

std::shared_ptr<AudioDecoder> AudioMixer::GetDecoderForClip(const std::string& source_path) {
    std::lock_guard<std::mutex> lock(decoders_mutex_);

    auto it = decoders_.find(source_path);
    if (it != decoders_.end()) {
        return it->second;
    }

    // Not preloaded - create on demand
    auto decoder = std::make_shared<AudioDecoder>();
    if (decoder->Open(source_path)) {
        decoder->Start();
        decoders_[source_path] = decoder;
        return decoder;
    }

    return nullptr;
}

//=============================================================================
// Volume Control
//=============================================================================

void AudioMixer::SetVolume(float volume) {
    volume_ = std::max(0.0f, std::min(1.0f, volume));
}

void AudioMixer::SetMuted(bool muted) {
    muted_ = muted;
}

//=============================================================================
// Current State
//=============================================================================

std::string AudioMixer::GetCurrentClipId() const {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(source_mutex_));
    return current_source_.clip_id;
}

std::string AudioMixer::GetCurrentSourcePath() const {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(source_mutex_));
    return current_source_.source_path;
}

//=============================================================================
// miniaudio Callbacks
//=============================================================================

void AudioMixer::DataCallback(ma_device* device, void* output,
                              const void* /*input*/, unsigned int frame_count) {
    AudioMixer* mixer = static_cast<AudioMixer*>(device->pUserData);
    if (mixer) {
        mixer->ProcessAudio(static_cast<float*>(output), frame_count);
    } else {
        std::memset(output, 0, frame_count * 2 * sizeof(float));
    }
}

void AudioMixer::ProcessAudio(float* output, unsigned int frame_count) {
    // Check if we should output silence
    if (!is_playing_ || muted_) {
        std::memset(output, 0, frame_count * 2 * sizeof(float));
        return;
    }

    std::lock_guard<std::mutex> lock(source_mutex_);

    // Check if we have a current decoder
    if (!current_source_.decoder || !current_source_.decoder->HasAudio()) {
        std::memset(output, 0, frame_count * 2 * sizeof(float));
        return;
    }

    // Check if we're past the clip's end
    double timeline_pos = current_position_.load();
    double clip_end = current_source_.clip_start_time + current_source_.clip_duration;
    if (timeline_pos >= clip_end) {
        std::memset(output, 0, frame_count * 2 * sizeof(float));
        return;
    }

    // Read from current decoder
    std::vector<float> current_buf(frame_count * 2);
    current_source_.decoder->Read(current_buf.data(), frame_count);

    // Apply crossfade if transitioning
    if (outgoing_decoder_ && crossfade_position_ < 1.0f) {
        std::vector<float> outgoing_buf(frame_count * 2);
        outgoing_decoder_->Read(outgoing_buf.data(), frame_count);

        // Linear crossfade
        float samples_per_fade = CROSSFADE_DURATION_SECONDS * 48000.0f;
        for (unsigned int i = 0; i < frame_count; ++i) {
            float fade_pos = crossfade_position_ + (static_cast<float>(i) / samples_per_fade);
            fade_pos = std::min(1.0f, fade_pos);

            float fade_in = fade_pos;
            float fade_out = 1.0f - fade_pos;

            output[i * 2] = current_buf[i * 2] * fade_in + outgoing_buf[i * 2] * fade_out;
            output[i * 2 + 1] = current_buf[i * 2 + 1] * fade_in + outgoing_buf[i * 2 + 1] * fade_out;
        }

        crossfade_position_ += static_cast<float>(frame_count) / samples_per_fade;
        if (crossfade_position_ >= 1.0f) {
            outgoing_decoder_ = nullptr;
        }
    } else {
        std::memcpy(output, current_buf.data(), frame_count * 2 * sizeof(float));
    }

    // Apply volume
    float vol = volume_.load();
    if (vol < 1.0f) {
        for (unsigned int i = 0; i < frame_count * 2; ++i) {
            output[i] *= vol;
        }
    }
}

} // namespace ump
