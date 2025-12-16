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

    int preloaded = 0;
    for (const auto& clip : clips) {
        // Skip gaps
        if (clip.is_gap) {
            continue;
        }

        // Get the media path - prefer linked_path, fall back to file_path
        std::string media_path = clip.linked_path;
        if (media_path.empty()) {
            media_path = clip.file_path;
        }

        // Skip if no valid path
        if (media_path.empty()) {
            continue;
        }

        // Key by clip_id to allow multiple clips from same source
        // (e.g., Premiere linked video+audio, or same clip used multiple times)
        std::string cache_key = clip.id + "|" + media_path;

        // Skip if already loaded
        if (decoders_.find(cache_key) != decoders_.end()) {
            continue;
        }

        // Create decoder for this specific clip instance
        auto decoder = std::make_shared<AudioDecoder>();
        if (decoder->Open(media_path)) {
            decoder->Start();
            decoders_[cache_key] = decoder;
            Debug::Log("AudioMixer: Preloaded audio for clip " + clip.id + " -> " + media_path);
            preloaded++;
        } else {
            Debug::Log("AudioMixer: No audio in " + media_path);
        }
    }

    Debug::Log("AudioMixer: Preloaded " + std::to_string(preloaded) + " new decoders, total: " +
               std::to_string(decoders_.size()));
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

    // Clear active sources
    {
        std::lock_guard<std::mutex> source_lock(source_mutex_);
        active_sources_.clear();
    }

    last_clip_ids_.clear();
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
    last_clip_ids_.clear();

    // Clear all active sources
    {
        std::lock_guard<std::mutex> lock(source_mutex_);
        active_sources_.clear();
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

    // Get ALL audible clips at current position (multi-track mixing)
    auto clips = GetAllAudibleClipsAtTime(timeline_pos);

    // Build set of current clip IDs
    std::set<std::string> current_clip_ids;
    for (const auto* clip : clips) {
        if (clip) current_clip_ids.insert(clip->id);
    }

    // Check if clips changed
    if (current_clip_ids != last_clip_ids_) {
        UpdateActiveSources(clips, timeline_pos);
        last_clip_ids_ = current_clip_ids;
    }

    // Check sync periodically for all active sources
    double now = std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    double elapsed_ms = (now - last_sync_check_time_) * 1000.0;
    if (elapsed_ms >= sync_check_interval_ms_) {
        last_sync_check_time_ = now;

        std::lock_guard<std::mutex> lock(source_mutex_);
        for (auto& source : active_sources_) {
            if (!source.decoder) continue;

            // Calculate expected source position
            double expected_source = source.source_in +
                (timeline_pos - source.clip_start_time);

            // Clamp to valid range
            expected_source = std::max(source.source_in,
                std::min(expected_source, source.source_out));

            double actual_pos = source.decoder->GetReadPosition();
            double drift_ms = std::abs(actual_pos - expected_source) * 1000.0;

            if (drift_ms > sync_threshold_ms_) {
                Debug::Log("AudioMixer: Sync correction for " + source.clip_id +
                           " - drift: " + std::to_string(drift_ms) + "ms");
                source.decoder->Seek(expected_source);
            }
        }
    }

    last_position_ = timeline_pos;
}

void AudioMixer::Seek(double position) {
    current_position_ = position;

    // Get all audible clips at seek position
    auto clips = GetAllAudibleClipsAtTime(position);

    // Build set of current clip IDs
    std::set<std::string> current_clip_ids;
    for (const auto* clip : clips) {
        if (clip) current_clip_ids.insert(clip->id);
    }

    // Update sources if clips changed
    if (current_clip_ids != last_clip_ids_) {
        UpdateActiveSources(clips, position);
        last_clip_ids_ = current_clip_ids;
    } else {
        // Same clips, just seek all decoders
        std::lock_guard<std::mutex> lock(source_mutex_);
        for (auto& source : active_sources_) {
            if (!source.decoder) continue;
            double source_pos = source.source_in + (position - source.clip_start_time);
            source_pos = std::max(source.source_in, std::min(source_pos, source.source_out));
            source.decoder->Seek(source_pos);
        }
    }
}

//=============================================================================
// Multi-track Audio Mixing
//=============================================================================

std::vector<const OTIOClip*> AudioMixer::GetAllAudibleClipsAtTime(double timestamp) {
    if (!flattener_) return {};
    return flattener_->GetAllAudibleClipsAtTime(timestamp);
}

void AudioMixer::UpdateActiveSources(const std::vector<const OTIOClip*>& clips, double timeline_pos) {
    std::lock_guard<std::mutex> lock(source_mutex_);

    // Build map of existing sources by clip ID for reuse
    std::unordered_map<std::string, ActiveAudioSource> existing_sources;
    for (auto& source : active_sources_) {
        existing_sources[source.clip_id] = std::move(source);
    }
    active_sources_.clear();

    if (clips.empty()) {
        Debug::Log("AudioMixer: No audible clips at " + std::to_string(timeline_pos));
        return;
    }

    // Setup new active sources
    for (const auto* clip : clips) {
        if (!clip || clip->is_gap) continue;

        // Get the media path - prefer linked_path, fall back to file_path
        std::string media_path = clip->linked_path;
        if (media_path.empty()) {
            media_path = clip->file_path;
        }

        // Skip if no valid path
        if (media_path.empty()) {
            Debug::Log("AudioMixer: Skipping clip '" + clip->name + "' - no media path");
            continue;
        }

        // Check if we already have this source
        auto it = existing_sources.find(clip->id);
        if (it != existing_sources.end() && it->second.decoder) {
            // Reuse existing source
            active_sources_.push_back(std::move(it->second));
            existing_sources.erase(it);
            continue;
        }

        // Create new source - use clip ID to ensure separate decoder per clip instance
        auto decoder = GetDecoderForClip(media_path, clip->id);
        if (!decoder) {
            Debug::Log("AudioMixer: No audio in " + media_path);
            continue;
        }

        ActiveAudioSource source;
        source.clip_id = clip->id;
        source.source_path = media_path;
        source.clip_start_time = clip->start_time;
        source.clip_duration = clip->duration;
        source.decoder = decoder;

        // Get actual media duration for validation
        double media_duration = decoder->GetDuration();

        // Validate source_in/source_out against actual media duration
        // AAF/OTIO imports often have timecode-based values (e.g., 3598s for 00:59:58:00)
        // that exceed the actual media length - we need to normalize these
        double src_in = clip->source_in;
        double src_out = clip->source_out > 0 ? clip->source_out : media_duration;

        if (media_duration > 0) {
            // If source_in exceeds media duration, it's likely a timecode offset
            // Reset to use the clip duration as a subclip from start of media
            if (src_in >= media_duration) {
                Debug::Log("AudioMixer: Normalizing invalid source_in (" +
                           std::to_string(src_in) + "s) for " + clip->name +
                           " - media duration is only " + std::to_string(media_duration) + "s");
                src_in = 0.0;
                src_out = std::min(clip->duration, media_duration);
            }
            // Also check if source_out is beyond media
            else if (src_out > media_duration) {
                src_out = media_duration;
            }
        }

        source.source_in = src_in;
        source.source_out = src_out;

        // Seek decoder to correct position within clip
        double source_pos = source.source_in + (timeline_pos - clip->start_time);
        source_pos = std::max(source.source_in, std::min(source_pos, source.source_out));
        decoder->Seek(source_pos);

        active_sources_.push_back(std::move(source));
        Debug::Log("AudioMixer: Added source '" + clip->name + "'"
                   " clip_start=" + std::to_string(clip->start_time) +
                   " duration=" + std::to_string(clip->duration) +
                   " source_in=" + std::to_string(src_in) +
                   " source_out=" + std::to_string(src_out) +
                   " media_dur=" + std::to_string(media_duration) +
                   " seek_to=" + std::to_string(source_pos) +
                   " path=" + media_path);
    }

    Debug::Log("AudioMixer: Now mixing " + std::to_string(active_sources_.size()) +
               " audio sources at timeline " + std::to_string(timeline_pos) + "s");
}

std::shared_ptr<AudioDecoder> AudioMixer::GetDecoderForClip(const std::string& source_path,
                                                              const std::string& clip_id) {
    std::lock_guard<std::mutex> lock(decoders_mutex_);

    // Key by clip_id, not source_path, to allow multiple clips from same source
    // (e.g., Premiere linked video+audio, or same clip used multiple times)
    std::string cache_key = clip_id + "|" + source_path;

    auto it = decoders_.find(cache_key);
    if (it != decoders_.end()) {
        return it->second;
    }

    // Create new decoder for this clip instance
    auto decoder = std::make_shared<AudioDecoder>();
    if (decoder->Open(source_path)) {
        decoder->Start();
        decoders_[cache_key] = decoder;
        Debug::Log("AudioMixer: Created decoder for clip " + clip_id + " -> " + source_path);
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
    if (active_sources_.empty()) return "";
    return active_sources_[0].clip_id;  // Return first active source
}

std::string AudioMixer::GetCurrentSourcePath() const {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(source_mutex_));
    if (active_sources_.empty()) return "";
    return active_sources_[0].source_path;  // Return first active source
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

    // Check if we have any active sources
    if (active_sources_.empty()) {
        std::memset(output, 0, frame_count * 2 * sizeof(float));
        return;
    }

    double timeline_pos = current_position_.load();

    // Initialize output buffer to zero
    std::memset(output, 0, frame_count * 2 * sizeof(float));

    // Temporary buffer for each source
    std::vector<float> source_buf(frame_count * 2);

    // Mix all active sources together
    int sources_mixed = 0;
    for (auto& source : active_sources_) {
        if (!source.decoder || !source.decoder->HasAudio()) continue;

        // Check if we're within the clip's time range
        double clip_end = source.clip_start_time + source.clip_duration;
        if (timeline_pos < source.clip_start_time || timeline_pos >= clip_end) {
            continue;
        }

        // Read audio from this source
        std::memset(source_buf.data(), 0, frame_count * 2 * sizeof(float));
        source.decoder->Read(source_buf.data(), frame_count);

        // Mix into output (additive mixing)
        for (unsigned int i = 0; i < frame_count * 2; ++i) {
            output[i] += source_buf[i];
        }
        sources_mixed++;
    }

    // Apply volume only - no artificial gain reduction
    // Professional mixing simply sums tracks; the user controls levels
    float vol = volume_.load();

    for (unsigned int i = 0; i < frame_count * 2; ++i) {
        float sample = output[i] * vol;

        // Hard clamp to prevent any clipping artifacts
        // No soft clipping - let the sum be what it is, just prevent overflow
        if (sample > 1.0f) sample = 1.0f;
        else if (sample < -1.0f) sample = -1.0f;

        output[i] = sample;
    }
}

} // namespace ump
