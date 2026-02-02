#include "audio_mixer.h"
#include "wasapi_audio_device.h"

#include "../timeline/timeline_view.h"
#include "../timeline/timeline_types.h"
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

    Debug::Log("AudioMixer: Initializing WASAPI...");

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
        Debug::Log("AudioMixer: Failed to initialize WASAPI device");
        device_.reset();
        return false;
    }

    initialized_ = true;

    // Start background preparation thread
    StartPreparationThread();

    Debug::Log("AudioMixer: Initialized successfully");
    return true;
}

void AudioMixer::Shutdown() {
    if (!initialized_) return;

    Debug::Log("AudioMixer: Shutting down...");

    // Stop background preparation thread first
    StopPreparationThread();

    Stop();
    ClearClips();

    if (device_) {
        device_->Shutdown();
        device_.reset();
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

    // IMPORTANT: Don't hold decoders_mutex_ during FFmpeg I/O!
    // The background audio thread needs this lock for clip change detection.
    // Holding it during file I/O (which can take 100s of ms) causes audio glitches.

    // Helper to detect image sequences (no audio)
    auto isImageSequence = [](const std::string& path) -> bool {
        if (path.empty()) return false;
        // Check for frame pattern indicators
        if (path.find("%04d") != std::string::npos ||
            path.find("%05d") != std::string::npos ||
            path.find("%d") != std::string::npos) {
            return true;
        }
        // Check extension
        std::string ext = path;
        size_t dot = ext.rfind('.');
        if (dot != std::string::npos) {
            ext = ext.substr(dot);
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (ext == ".exr" || ext == ".tif" || ext == ".tiff" ||
                ext == ".png" || ext == ".jpg" || ext == ".jpeg" ||
                ext == ".dpx" || ext == ".tga") {
                return true;
            }
        }
        return false;
    };

    // Step 1: Collect clips that need loading (brief lock)
    struct ClipToLoad {
        std::string clip_id;
        std::string media_path;
        std::string cache_key;
    };
    std::vector<ClipToLoad> clips_to_load;

    {
        std::lock_guard<std::mutex> lock(decoders_mutex_);
        for (const auto& clip : clips) {
            if (clip.is_gap) continue;

            std::string media_path = clip.linked_path;
            if (media_path.empty()) media_path = clip.file_path;
            if (media_path.empty()) continue;

            // Skip image sequences - they don't have audio
            if (isImageSequence(media_path)) continue;

            std::string cache_key = clip.id + "|" + media_path;

            // Skip if already loaded
            if (decoders_.find(cache_key) != decoders_.end()) continue;

            clips_to_load.push_back({clip.id, media_path, cache_key});
        }
    }
    // Lock released - background thread can now work

    // Step 2: Open decoders WITHOUT holding the lock (FFmpeg I/O)
    struct LoadedDecoder {
        std::string cache_key;
        std::string clip_id;
        std::shared_ptr<AudioDecoder> decoder;
    };
    std::vector<LoadedDecoder> loaded;

    for (const auto& info : clips_to_load) {
        auto decoder = std::make_shared<AudioDecoder>();
        if (decoder->Open(info.media_path)) {
            decoder->Start();
            loaded.push_back({info.cache_key, info.clip_id, decoder});
            Debug::Log("AudioMixer: Preloaded audio for clip " + info.clip_id + " -> " + info.media_path);
        } else {
            Debug::Log("AudioMixer: No audio in " + info.media_path);
        }
    }

    // Step 3: Add to cache (brief lock)
    if (!loaded.empty()) {
        std::lock_guard<std::mutex> lock(decoders_mutex_);
        for (auto& item : loaded) {
            // Double-check not added by background thread while we were loading
            if (decoders_.find(item.cache_key) == decoders_.end()) {
                decoders_[item.cache_key] = item.decoder;
            }
        }
    }

    Debug::Log("AudioMixer: Preloaded " + std::to_string(loaded.size()) + " new decoders");
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

    device_->Start();

    is_playing_ = true;

    last_sync_check_time_ = std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

void AudioMixer::Pause() {
    if (!initialized_ || !device_ || !is_playing_) return;

    Debug::Log("AudioMixer: Pause");

    device_->Stop();
    is_playing_ = false;
}

void AudioMixer::Stop() {
    if (!initialized_ || !device_) return;

    Debug::Log("AudioMixer: Stop");

    device_->Stop();
    is_playing_ = false;

    // Reset position
    current_position_ = 0.0;
    last_clip_ids_.clear();

    // Clear all active sources
    {
        std::lock_guard<std::mutex> lock(source_mutex_);
        active_sources_.clear();
    }

    // Clear warming decoders
    {
        std::lock_guard<std::mutex> lock(warming_mutex_);
        warming_decoders_.clear();
    }
}

//=============================================================================
// Update
//=============================================================================

void AudioMixer::Update() {
    // MINIMAL MAIN THREAD WORK: Just update position from timer
    // All clip detection, source switching, and sync correction happens on background thread
    // This prevents audio operations from blocking video rendering

    if (timer_) {
        current_position_ = timer_->GetPosition();
    }
    // If no timer, position is set via SetTimelinePosition() which is already atomic
}

void AudioMixer::Seek(double position) {
    // MINIMAL MAIN THREAD WORK: Just update position atomically
    // The background thread will detect the position jump (>0.5s delta) and handle:
    // - Clearing warming decoders
    // - Checking for clip changes
    // - Seeking active decoders to new position
    // This prevents audio seek operations from blocking video/UI during scrubbing

    current_position_ = position;

    // Clear warming decoders immediately (they're pre-filled for wrong positions)
    // This is quick - just clearing a map, no FFmpeg calls
    {
        std::lock_guard<std::mutex> lock(warming_mutex_);
        warming_decoders_.clear();
    }

    // Wake up background thread to process the seek faster than the normal 50ms poll
    preparation_cv_.notify_one();
}

//=============================================================================
// Multi-track Audio Mixing
//=============================================================================

std::vector<const OTIOClip*> AudioMixer::GetAllAudibleClipsAtTime(double timestamp) {
    if (!flattener_) return {};
    return flattener_->GetAllAudibleClipsAtTime(timestamp);
}

std::vector<const OTIOClip*> AudioMixer::GetUpcomingClips(double current_pos, double lookahead_seconds) {
    if (!flattener_) return {};

    std::vector<const OTIOClip*> upcoming;
    double window_end = current_pos + lookahead_seconds;

    // Iterate through all tracks to find clips starting within the lookahead window
    const auto& tracks = flattener_->GetTracks();
    for (const auto& track : tracks) {
        // Check track audibility (same logic as GetAllAudibleClipsAtTime)
        bool include_track = false;
        if (track.is_video) {
            include_track = track.visible && !track.audio_muted;
        } else {
            include_track = !track.muted;
        }
        if (!include_track) continue;

        for (const auto& clip : track.clips) {
            if (clip.is_gap) continue;
            if (clip.audio_muted) continue;

            // Check if clip starts within the lookahead window (but not already playing)
            // We want clips that will START soon, not clips already active
            if (clip.start_time > current_pos && clip.start_time <= window_end) {
                // Must have a valid media path
                if (!clip.linked_path.empty() || !clip.file_path.empty()) {
                    upcoming.push_back(&clip);
                }
            }
        }
    }

    return upcoming;
}

void AudioMixer::PrewarmUpcomingClips(double current_pos) {
    if (!flattener_ || !is_playing_) return;

    // Get clips that will start within the lookahead window
    auto upcoming = GetUpcomingClips(current_pos, lookahead_seconds_);

    if (upcoming.empty()) return;

    std::lock_guard<std::mutex> lock(warming_mutex_);

    for (const auto* clip : upcoming) {
        // Skip if already warming
        if (warming_decoders_.find(clip->id) != warming_decoders_.end()) {
            continue;
        }

        // Skip if already active (shouldn't happen but be safe)
        {
            std::lock_guard<std::mutex> source_lock(source_mutex_);
            bool already_active = false;
            for (const auto& source : active_sources_) {
                if (source.clip_id == clip->id) {
                    already_active = true;
                    break;
                }
            }
            if (already_active) continue;
        }

        // Get media path
        std::string media_path = clip->linked_path;
        if (media_path.empty()) {
            media_path = clip->file_path;
        }
        if (media_path.empty()) continue;

        // Create or get decoder for this clip
        auto decoder = GetDecoderForClip(media_path, clip->id);
        if (!decoder) continue;

        // Calculate where to seek in the source media
        // When clip starts, we want to be at source_in
        double source_pos = clip->source_in;

        // Seek decoder to the start position so it can pre-fill its buffer
        decoder->Seek(source_pos);

        // Track as warming
        WarmingDecoder warming;
        warming.decoder = decoder;
        warming.source_position = source_pos;
        warming.source_path = media_path;
        warming.clip_start_time = clip->start_time;
        warming_decoders_[clip->id] = warming;

        Debug::Log("AudioMixer: Pre-warming decoder for upcoming clip '" + clip->name +
                   "' (starts at " + std::to_string(clip->start_time) +
                   "s, source_in=" + std::to_string(source_pos) + "s)");
    }

    // Clean up warming decoders for clips that have passed or are now active
    std::vector<std::string> to_remove;
    for (const auto& [clip_id, warming] : warming_decoders_) {
        // If clip has passed (its start time is behind playhead), remove from warming
        if (warming.clip_start_time < current_pos - 0.5) {
            to_remove.push_back(clip_id);
        }
    }
    for (const auto& id : to_remove) {
        warming_decoders_.erase(id);
    }
}

void AudioMixer::UpdateActiveSources(const std::vector<const OTIOClip*>& clips, double timeline_pos) {
    // IMPORTANT: Do all expensive FFmpeg operations OUTSIDE the lock to avoid
    // blocking the real-time audio callback. The audio thread needs source_mutex_
    // to read audio data - if we hold it during seeks, we cause hitches.
    //
    // Strategy: Keep existing sources playing until new sources are ready,
    // then atomically swap. This prevents audio dropout during transitions.

    // Step 1: Copy existing source info for reuse lookup (brief lock, no move)
    std::unordered_map<std::string, std::shared_ptr<AudioDecoder>> existing_decoders;
    {
        std::lock_guard<std::mutex> lock(source_mutex_);
        for (const auto& source : active_sources_) {
            if (source.decoder) {
                existing_decoders[source.clip_id] = source.decoder;
            }
        }
    }
    // NOTE: active_sources_ is NOT cleared - audio callback keeps playing old sources

    if (clips.empty()) {
        // Clear sources atomically
        std::lock_guard<std::mutex> lock(source_mutex_);
        active_sources_.clear();
        // NOTE: Debug logging disabled in hot path - causes hitches due to synchronous I/O
        // Debug::Log("AudioMixer: No audible clips at " + std::to_string(timeline_pos));
        return;
    }

    // Step 2: Build new sources list WITHOUT holding the lock
    // This is where expensive FFmpeg operations happen (decoder creation, seeks)
    std::vector<ActiveAudioSource> new_sources;

    for (const auto* clip : clips) {
        if (!clip || clip->is_gap) continue;

        // Get the media path - prefer linked_path, fall back to file_path
        std::string media_path = clip->linked_path;
        if (media_path.empty()) {
            media_path = clip->file_path;
        }

        // Skip if no valid path
        if (media_path.empty()) {
            // Debug::Log("AudioMixer: Skipping clip '" + clip->name + "' - no media path");
            continue;
        }

        // Check if we already have a decoder for this clip
        std::shared_ptr<AudioDecoder> decoder;
        bool decoder_reused = false;
        bool decoder_prewarmed = false;

        // Priority 1: Reuse existing active decoder
        auto it = existing_decoders.find(clip->id);
        if (it != existing_decoders.end() && it->second) {
            // Reuse existing decoder - it's already playing at the right position
            decoder = it->second;
            decoder_reused = true;
        }

        // Priority 2: Use pre-warmed decoder (buffer already filling)
        if (!decoder) {
            std::lock_guard<std::mutex> warm_lock(warming_mutex_);
            auto warm_it = warming_decoders_.find(clip->id);
            if (warm_it != warming_decoders_.end() && warm_it->second.decoder) {
                decoder = warm_it->second.decoder;
                decoder_prewarmed = true;
                // Remove from warming list since it's now active
                warming_decoders_.erase(warm_it);
                // Debug::Log("AudioMixer: Using pre-warmed decoder for '" + clip->name + "'");
            }
        }

        // Priority 3: Create new decoder (fallback)
        if (!decoder) {
            // NOTE: This may involve FFmpeg operations but GetDecoderForClip has its own lock
            decoder = GetDecoderForClip(media_path, clip->id);
        }
        if (!decoder) {
            // Debug::Log("AudioMixer: No audio in " + media_path);
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
                // Debug::Log("AudioMixer: Normalizing invalid source_in (" +
                //            std::to_string(src_in) + "s) for " + clip->name +
                //            " - media duration is only " + std::to_string(media_duration) + "s");
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

        // Determine if we need to seek
        // - Reused decoders: already at right position, no seek needed
        // - Pre-warmed decoders: already seeked to source_in, buffer filling - no seek needed
        //   (the buffer has the right data, seeking would flush it!)
        // - New decoders: need to seek to current position
        if (decoder_reused) {
            // Debug::Log("AudioMixer: Reusing active source '" + clip->name + "' (no seek)");
        } else if (decoder_prewarmed) {
            // Pre-warmed decoder was seeked to source_in and has been filling its buffer
            // Don't seek again - the buffer already has the audio we need
            // The read position will naturally advance as we consume audio
            // Debug::Log("AudioMixer: Using pre-warmed source '" + clip->name +
            //            "' (buffer ready, no seek)");
        } else {
            // Brand new decoder - seek to current position
            double source_pos = source.source_in + (timeline_pos - clip->start_time);
            source_pos = std::max(source.source_in, std::min(source_pos, source.source_out));
            decoder->Seek(source_pos);
            // Debug::Log("AudioMixer: Added NEW source '" + clip->name + "'"
            //            " clip_start=" + std::to_string(clip->start_time) +
            //            " duration=" + std::to_string(clip->duration) +
            //            " source_in=" + std::to_string(src_in) +
            //            " source_out=" + std::to_string(src_out) +
            //            " media_dur=" + std::to_string(media_duration) +
            //            " seek_to=" + std::to_string(source_pos) +
            //            " path=" + media_path);
        }

        new_sources.push_back(std::move(source));
    }

    // Step 3: Swap in new sources (brief lock)
    {
        std::lock_guard<std::mutex> lock(source_mutex_);
        active_sources_ = std::move(new_sources);
    }

    // NOTE: Debug logging disabled in hot path - synchronous I/O causes hitches
    // Debug::Log("AudioMixer: Now mixing " + std::to_string(active_sources_.size()) +
    //            " audio sources at timeline " + std::to_string(timeline_pos) + "s");
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
// WASAPI Callbacks
//=============================================================================

void AudioMixer::DataCallback(void* /*device*/, float* output,
                              uint32_t frame_count, void* userData) {
    AudioMixer* mixer = static_cast<AudioMixer*>(userData);
    if (mixer) {
        mixer->ProcessAudio(output, frame_count);
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

    // Apply volume and soft limiting
    float vol = volume_.load();

    // Soft limiter using fast tanh approximation
    // Starts compressing around 0.8, approaches ±1.0 asymptotically
    // Much smoother than hard clipping when multiple tracks sum loud
    auto softLimit = [](float x) -> float {
        // Fast tanh approximation: x / (1 + |x|) scaled for audio
        // Threshold at ~0.8 where limiting kicks in
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

    for (unsigned int i = 0; i < frame_count * 2; ++i) {
        output[i] = softLimit(output[i] * vol);
    }
}

//=============================================================================
// Background Preparation Thread
//=============================================================================

void AudioMixer::StartPreparationThread() {
    if (preparation_thread_running_) return;

    Debug::Log("AudioMixer: Starting background preparation thread");
    preparation_thread_running_ = true;
    preparation_thread_ = std::thread(&AudioMixer::PreparationThreadFunc, this);
}

void AudioMixer::StopPreparationThread() {
    if (!preparation_thread_running_) return;

    Debug::Log("AudioMixer: Stopping background preparation thread");

    // Signal thread to stop
    preparation_thread_running_ = false;
    preparation_cv_.notify_all();

    // Wait for thread to finish
    if (preparation_thread_.joinable()) {
        preparation_thread_.join();
    }

    Debug::Log("AudioMixer: Background preparation thread stopped");
}

void AudioMixer::PreparationThreadFunc() {
    Debug::Log("AudioMixer: Background preparation thread started");

    while (preparation_thread_running_) {
        // Wait for work or timeout (check every 50ms for lookahead updates)
        std::unique_lock<std::mutex> lock(preparation_mutex_);
        preparation_cv_.wait_for(lock, std::chrono::milliseconds(50), [this] {
            return !preparation_thread_running_;
        });
        lock.unlock();

        if (!preparation_thread_running_) break;

        // Skip if not playing or no flattener
        if (!is_playing_ || !flattener_) continue;

        double current_pos = current_position_.load();

        // Get upcoming clips within lookahead window
        auto upcoming = GetUpcomingClips(current_pos, lookahead_seconds_);

        for (const auto* clip : upcoming) {
            if (!preparation_thread_running_) break;

            std::string media_path = clip->linked_path.empty() ? clip->file_path : clip->linked_path;
            if (media_path.empty()) continue;

            std::string cache_key = clip->id + "|" + media_path;

            // Check if already warming (already being prepared)
            {
                std::lock_guard<std::mutex> wlock(warming_mutex_);
                if (warming_decoders_.find(clip->id) != warming_decoders_.end()) {
                    continue;  // Already warming
                }
            }

            // Try to get decoder from cache first (from PreloadClips), or create new
            std::shared_ptr<AudioDecoder> decoder;
            bool from_cache = false;
            {
                std::lock_guard<std::mutex> dlock(decoders_mutex_);
                auto it = decoders_.find(cache_key);
                if (it != decoders_.end()) {
                    decoder = it->second;
                    from_cache = true;
                }
            }

            // Create new decoder if not in cache
            if (!decoder) {
                decoder = std::make_shared<AudioDecoder>();
                if (!decoder->Open(media_path)) {
                    // No audio in this file - that's OK
                    continue;
                }
                decoder->Start();

                // Add to main cache for future use
                {
                    std::lock_guard<std::mutex> dlock(decoders_mutex_);
                    decoders_[cache_key] = decoder;
                }
            }

            // Seek to the source in point (where playback will start when clip becomes active)
            // This is the key operation - happens on background thread, not main thread
            double source_position = clip->source_in;
            decoder->Seek(source_position);

            // Add to warming decoders so UpdateActiveSources knows it's ready
            {
                std::lock_guard<std::mutex> wlock(warming_mutex_);
                WarmingDecoder wd;
                wd.decoder = decoder;
                wd.source_position = source_position;
                wd.source_path = media_path;
                wd.clip_start_time = clip->start_time;
                warming_decoders_[clip->id] = wd;
            }
        }

        // Also preload any pending clips that were queued from PreloadClips calls
        std::vector<PendingClipInfo> clips_to_load;
        {
            std::lock_guard<std::mutex> plock(pending_mutex_);
            clips_to_load = std::move(pending_clips_);
            pending_clips_.clear();
        }

        for (const auto& clip_info : clips_to_load) {
            if (!preparation_thread_running_) break;

            std::string cache_key = clip_info.clip_id + "|" + clip_info.source_path;

            // Check if already loaded
            {
                std::lock_guard<std::mutex> dlock(decoders_mutex_);
                if (decoders_.find(cache_key) != decoders_.end()) {
                    continue;
                }
            }

            Debug::Log("AudioMixer[BG]: Loading pending clip " + clip_info.clip_id);

            auto decoder = std::make_shared<AudioDecoder>();
            if (!decoder->Open(clip_info.source_path)) {
                Debug::Log("AudioMixer[BG]: No audio in " + clip_info.source_path);
                continue;
            }

            decoder->Start();

            // Add directly to main decoder cache
            {
                std::lock_guard<std::mutex> dlock(decoders_mutex_);
                decoders_[cache_key] = decoder;
            }

            Debug::Log("AudioMixer[BG]: Loaded decoder for " + clip_info.clip_id);
        }

        //=====================================================================
        // CLIP CHANGE DETECTION (moved from main thread Update())
        // Check for clip changes and update active sources
        //=====================================================================

        double now = std::chrono::duration<double>(
            std::chrono::steady_clock::now().time_since_epoch()).count();

        // Check for clip changes every iteration (we're already at 50ms intervals)
        double pos_delta = std::abs(current_pos - last_position_);

        // Always check on big jumps, otherwise periodic
        bool should_check_clips = (pos_delta > 0.5) ||
                                  ((now - last_clip_check_time_) * 1000.0 >= 50.0);

        if (should_check_clips && flattener_) {
            last_clip_check_time_ = now;

            // Get ALL audible clips at current position (multi-track mixing)
            auto clips = GetAllAudibleClipsAtTime(current_pos);

            // Build set of current clip IDs
            std::set<std::string> current_clip_ids;
            for (const auto* clip : clips) {
                if (clip) current_clip_ids.insert(clip->id);
            }

            // Check if clips changed
            if (current_clip_ids != last_clip_ids_) {
                UpdateActiveSources(clips, current_pos);
                last_clip_ids_ = current_clip_ids;
            }
            // Big position jump but same clips - seek all decoders immediately
            // This handles scrubbing within a single clip
            else if (pos_delta > 0.5) {
                std::vector<std::pair<std::shared_ptr<AudioDecoder>, double>> seeks;
                {
                    std::lock_guard<std::mutex> lock(source_mutex_);
                    for (auto& source : active_sources_) {
                        if (!source.decoder) continue;
                        double source_pos = source.source_in + (current_pos - source.clip_start_time);
                        source_pos = std::max(source.source_in, std::min(source_pos, source.source_out));
                        seeks.push_back({source.decoder, source_pos});
                    }
                }
                // Seek outside lock
                for (auto& s : seeks) {
                    s.first->Seek(s.second);
                }
            }
        }

        //=====================================================================
        // SYNC CORRECTION (moved from main thread Update())
        // Check for audio drift and apply corrections
        //=====================================================================

        double elapsed_ms = (now - last_sync_check_time_) * 1000.0;
        if (elapsed_ms >= sync_check_interval_ms_) {
            last_sync_check_time_ = now;

            // Collect sync corrections needed (brief lock)
            struct SyncCorrection {
                std::shared_ptr<AudioDecoder> decoder;
                double target_pos;
            };
            std::vector<SyncCorrection> corrections;

            {
                std::lock_guard<std::mutex> lock(source_mutex_);
                for (auto& source : active_sources_) {
                    if (!source.decoder) continue;

                    // Calculate expected source position
                    double expected_source = source.source_in +
                        (current_pos - source.clip_start_time);

                    // Clamp to valid range
                    expected_source = std::max(source.source_in,
                        std::min(expected_source, source.source_out));

                    double actual_pos = source.decoder->GetReadPosition();
                    double drift_ms = std::abs(actual_pos - expected_source) * 1000.0;

                    if (drift_ms > sync_threshold_ms_) {
                        corrections.push_back({source.decoder, expected_source});
                    }
                }
            }

            // Apply sync corrections OUTSIDE the lock (FFmpeg seeks)
            for (auto& corr : corrections) {
                corr.decoder->Seek(corr.target_pos);
            }
        }

        last_position_ = current_pos;
    }

    Debug::Log("AudioMixer: Background preparation thread exiting");
}

} // namespace ump
