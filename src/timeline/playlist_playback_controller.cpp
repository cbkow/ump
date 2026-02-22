#include "playlist_playback_controller.h"
#include "playlist_timeline.h"
#include "playlist_clip.h"
#include "timeline_cache.h"
#include "timeline_view.h"  // For TimelineFlattener
#include "timeline_types.h"
#include "../player/video_display_component.h"
#include "../player/playback_timer.h"
#include "../audio/audio_mixer.h"
#include "../utils/debug_utils.h"

#include <algorithm>
#include <cmath>

namespace qcview {

PlaylistPlaybackController::PlaylistPlaybackController() = default;

PlaylistPlaybackController::~PlaylistPlaybackController() {
    Shutdown();
}

//=============================================================================
// Initialization
//=============================================================================

bool PlaylistPlaybackController::Initialize(PlaylistTimeline* timeline, VideoDisplayComponent* video_player) {
    if (initialized_) {
        Debug::Log("[PlaylistPlaybackController] Already initialized");
        return true;
    }

    if (!timeline) {
        Debug::Log("[PlaylistPlaybackController] Error: timeline is null");
        return false;
    }

    timeline_ = timeline;
    video_player_ = video_player;

    // Get timeline properties
    fps_ = timeline_->GetFrameRate();
    timeline_duration_ = timeline_->GetDuration();
    width_ = timeline_->GetCanvasWidth();
    height_ = timeline_->GetCanvasHeight();

    // Create playback timer
    timer_ = std::make_unique<PlaybackTimer>();
    timer_->SetDuration(timeline_duration_);
    timer_->SetFrameRate(fps_);

    // Setup end callback
    timer_->SetOnEnd([this]() {
        if (playlist_ended_callback_) {
            playlist_ended_callback_();
        }
    });

    // Create audio mixer
    audio_mixer_ = std::make_unique<AudioMixer>();
    if (!audio_mixer_->Initialize()) {
        Debug::Log("[PlaylistPlaybackController] Warning: Audio mixer initialization failed");
    }
    audio_mixer_->SetTimer(timer_.get());

    // Create flattener for the playlist
    flattener_ = std::make_unique<TimelineFlattener>();

    Debug::Log("[PlaylistPlaybackController] Initialized - duration: " + std::to_string(timeline_duration_) +
               "s, fps: " + std::to_string(fps_) + ", " + std::to_string(width_) + "x" + std::to_string(height_));

    initialized_ = true;
    return true;
}

bool PlaylistPlaybackController::InitializeCache() {
    if (!initialized_ || !timeline_ || timeline_->IsEmpty()) {
        Debug::Log("[PlaylistPlaybackController] Cannot initialize cache - not ready");
        return false;
    }

    // Convert playlist to OTIO track format
    OTIOTrack track = timeline_->ToOTIOTrack();
    std::vector<OTIOTrack> tracks = { track };

    // Update flattener
    flattener_->SetTracks(tracks);

    // Create timeline cache
    // NOTE: Single-decoder mode is now handled inside TimelineCache when
    // source_mode == PLAYLIST and config.use_single_decoder is true.
    cache_ = std::make_unique<TimelineCache>();

    TimelineCacheConfig cache_config;
    cache_config.readAheadFrames = config_.readAheadFrames;
    cache_config.readBehindFrames = config_.readBehindFrames;
    cache_config.io_threads = config_.io_threads;
    cache_config.fps = fps_;
    cache_config.pipeline_mode = config_.pipeline_mode;
    // Enable single-decoder mode in TimelineCache (default is true for stability)
    cache_config.use_single_decoder = true;
    cache_->SetConfig(cache_config);

    // Initialize cache with PLAYLIST source mode
    cache_->Initialize(tracks, flattener_.get(), fps_, TimelineSourceMode::PLAYLIST);

    // Set canvas dimensions for consistent output
    cache_->SetCanvasDimensions(width_, height_);

    // Register sequence metadata for any image sequence clips
    // This enables DirectEXRCache for playlist items that are image sequences
    for (const auto& clip : track.clips) {
        if (clip.is_sequence && clip.is_linked && !clip.linked_path.empty()) {
            SequenceMetadata seq_meta;
            seq_meta.directory = clip.sequence_directory;
            seq_meta.pattern = clip.sequence_pattern;
            seq_meta.start_frame = clip.sequence_start_frame;
            seq_meta.end_frame = clip.sequence_end_frame;
            seq_meta.exr_layer = clip.sequence_exr_layer;
            // Detect pipeline mode from extension
            std::string pat = clip.sequence_pattern;
            std::transform(pat.begin(), pat.end(), pat.begin(), ::tolower);
            seq_meta.pipeline_mode = (pat.find(".exr") != std::string::npos)
                ? PipelineMode::ULTRA_HIGH_RES : PipelineMode::NORMAL;
            seq_meta.valid = true;
            cache_->RegisterSequenceMetadata(clip.linked_path, seq_meta);
            Debug::Log("[PlaylistPlaybackController] Registered sequence metadata for " + clip.linked_path);
        }
    }

    // Trigger DirectEXRCache enable for image sequence content
    cache_->NotifyTracksEdited();

    // Preload audio clips
    audio_mixer_->PreloadClips(track.clips);

    Debug::Log("[PlaylistPlaybackController] Cache initialized for " + std::to_string(track.clips.size()) + " clips");

    return true;
}

void PlaylistPlaybackController::Shutdown() {
    if (!initialized_) {
        return;
    }

    Debug::Log("[PlaylistPlaybackController] Shutting down");

    // Stop playback
    if (timer_ && timer_->IsPlaying()) {
        timer_->Pause();
    }

    // Shutdown components
    if (audio_mixer_) {
        audio_mixer_->Shutdown();
    }
    if (cache_) {
        cache_->Shutdown();
    }

    // Reset state
    timer_.reset();
    audio_mixer_.reset();
    cache_.reset();
    flattener_.reset();

    current_clip_ = nullptr;
    prewarmed_clip_ = nullptr;
    timeline_ = nullptr;
    video_player_ = nullptr;

    initialized_ = false;
}

//=============================================================================
// Playback State
//=============================================================================

bool PlaylistPlaybackController::IsPlaying() const {
    return timer_ && timer_->IsPlaying();
}

double PlaylistPlaybackController::GetPosition() const {
    return timer_ ? timer_->GetPosition() : 0.0;
}

double PlaylistPlaybackController::GetDuration() const {
    return timeline_duration_;
}

int PlaylistPlaybackController::GetCurrentFrame() const {
    return timer_ ? timer_->GetCurrentFrame() : 0;
}

//=============================================================================
// Transport Controls
//=============================================================================

void PlaylistPlaybackController::Play() {
    if (!initialized_ || !timer_) return;

    // Enable buffer wait
    waiting_for_buffer_ = true;
    waiting_start_time_ = std::chrono::steady_clock::now();

    // Update cache playhead and start playback
    if (cache_) {
        cache_->UpdatePlayhead(timer_->GetCurrentFrame(), true);
    }

    timer_->Play();
    is_playing_.store(true);

    if (audio_mixer_) {
        audio_mixer_->Play();
    }

    Debug::Log("[PlaylistPlaybackController] Play at " + std::to_string(timer_->GetPosition()) + "s");
}

void PlaylistPlaybackController::Pause() {
    if (!initialized_ || !timer_) return;

    timer_->Pause();
    is_playing_.store(false);
    waiting_for_buffer_ = false;

    if (audio_mixer_) {
        audio_mixer_->Pause();
    }

    Debug::Log("[PlaylistPlaybackController] Paused at " + std::to_string(timer_->GetPosition()) + "s");
}

void PlaylistPlaybackController::TogglePlayPause() {
    if (IsPlaying()) {
        Pause();
    } else {
        Play();
    }
}

void PlaylistPlaybackController::Seek(double position) {
    if (!initialized_ || !timer_) return;

    position = std::clamp(position, 0.0, timeline_duration_);
    timer_->Seek(position);

    if (cache_) {
        int frame = static_cast<int>(position * fps_);
        cache_->UpdatePlayhead(frame, timer_->IsPlaying());
    }

    // Check for clip change
    CheckClipBoundary();

    Debug::Log("[PlaylistPlaybackController] Seek to " + std::to_string(position) + "s");
}

void PlaylistPlaybackController::SeekRelative(double delta) {
    Seek(GetPosition() + delta);
}

void PlaylistPlaybackController::StepForward(int frames) {
    if (!initialized_ || !timer_) return;

    timer_->StepForward(frames);

    if (cache_) {
        cache_->UpdatePlayhead(timer_->GetCurrentFrame(), false);
    }

    CheckClipBoundary();
}

void PlaylistPlaybackController::StepBackward(int frames) {
    if (!initialized_ || !timer_) return;

    timer_->StepBackward(frames);

    if (cache_) {
        cache_->UpdatePlayhead(timer_->GetCurrentFrame(), false);
    }

    CheckClipBoundary();
}

void PlaylistPlaybackController::GoToStart() {
    Seek(0.0);
}

void PlaylistPlaybackController::GoToEnd() {
    Seek(timeline_duration_ - (1.0 / fps_));
}

//=============================================================================
// Loop Control
//=============================================================================

void PlaylistPlaybackController::SetLooping(bool enabled) {
    if (cache_) {
        cache_->SetLooping(enabled);
    }
}

bool PlaylistPlaybackController::IsLooping() const {
    return cache_ ? cache_->IsLooping() : false;
}

//=============================================================================
// Update Loop
//=============================================================================

GLuint PlaylistPlaybackController::Update(int& width, int& height) {
    if (!initialized_ || !cache_) {
        width = height = 0;
        return 0;
    }

    // Update timer
    UpdateTimer();

    // Get current frame
    int frame = timer_->GetCurrentFrame();
    current_frame_.store(frame);

    // Check for clip transitions
    CheckClipBoundary();

    // Use TimelineCache (handles single-decoder mode internally)
    cache_->UpdatePlayhead(frame, timer_->IsPlaying());

    // Pre-warm upcoming clips
    PrewarmNextClip();

    // Get frame from cache
    bool got_exact = false;
    GLuint texture = cache_->GetFrame(frame, width, height, &got_exact);

    // Handle buffer wait
    if (waiting_for_buffer_ && timer_->IsPlaying()) {
        auto elapsed = std::chrono::steady_clock::now() - waiting_start_time_;
        int elapsed_ms = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());

        if (got_exact || elapsed_ms >= kMaxWaitMs) {
            waiting_for_buffer_ = false;
        }
    }

    // Store current texture
    current_texture_ = texture;
    current_texture_width_ = width;
    current_texture_height_ = height;

    return texture;
}

void PlaylistPlaybackController::ProcessPendingUploads() {
    if (cache_) {
        cache_->ProcessPendingUploads();
    }
}

void PlaylistPlaybackController::UpdateTimer() {
    if (!timer_ || !timer_->IsPlaying()) {
        return;
    }

    // Frame-locked timing
    auto now = std::chrono::steady_clock::now();

    if (!timer_initialized_) {
        last_timer_update_ = now;
        accumulated_time_ = 0.0;
        timer_initialized_ = true;
        return;
    }

    auto elapsed = std::chrono::duration<double>(now - last_timer_update_);
    last_timer_update_ = now;

    // Don't advance if waiting for buffer
    if (waiting_for_buffer_) {
        return;
    }

    // Accumulate time and advance by frames
    accumulated_time_ += elapsed.count();
    double frame_duration = 1.0 / fps_;

    while (accumulated_time_ >= frame_duration) {
        timer_->AdvanceByFrame();
        accumulated_time_ -= frame_duration;

        // Check for loop/end
        if (timer_->IsAtEnd()) {
            if (IsLooping()) {
                timer_->SeekToStart();
                waiting_for_buffer_ = true;
                waiting_start_time_ = std::chrono::steady_clock::now();
            } else {
                Pause();
                if (playlist_ended_callback_) {
                    playlist_ended_callback_();
                }
            }
            break;
        }
    }
}

#ifdef _WIN32
void PlaylistPlaybackController::SetD3D11RenderingMode(bool enabled) {
    use_d3d11_rendering_ = enabled;
    if (cache_) {
        cache_->SetD3D11RenderingMode(enabled);
    }
}

ID3D11ShaderResourceView* PlaylistPlaybackController::UpdateD3D11(int& width, int& height) {
    if (!initialized_ || !cache_) {
        width = height = 0;
        return nullptr;
    }

    // Update timer
    UpdateTimer();

    int frame = timer_->GetCurrentFrame();
    current_frame_.store(frame);

    // Check for clip transitions
    CheckClipBoundary();

    // Use TimelineCache (handles single-decoder mode internally)
    cache_->UpdatePlayhead(frame, timer_->IsPlaying());

    // Pre-warm upcoming clips
    PrewarmNextClip();

    bool got_exact = false;
    ID3D11ShaderResourceView* srv = cache_->GetFrameD3D11(frame, width, height, &got_exact);

    // Handle buffer wait
    if (waiting_for_buffer_ && timer_->IsPlaying()) {
        auto elapsed = std::chrono::steady_clock::now() - waiting_start_time_;
        int elapsed_ms = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());

        if (got_exact || elapsed_ms >= kMaxWaitMs) {
            waiting_for_buffer_ = false;
        }
    }

    current_srv_d3d_ = srv;
    current_texture_width_ = width;
    current_texture_height_ = height;

    return srv;
}
#endif

//=============================================================================
// Configuration
//=============================================================================

void PlaylistPlaybackController::SetConfig(const PlaylistPlaybackConfig& config) {
    config_ = config;

    if (cache_) {
        TimelineCacheConfig cache_config;
        cache_config.readAheadFrames = config_.readAheadFrames;
        cache_config.readBehindFrames = config_.readBehindFrames;
        cache_config.io_threads = config_.io_threads;
        cache_config.fps = fps_;
        cache_config.pipeline_mode = config_.pipeline_mode;
        cache_->SetConfig(cache_config);
    }
}

//=============================================================================
// Volume/Mute Control
//=============================================================================

void PlaylistPlaybackController::SetVolume(double volume) {
    volume_ = std::clamp(volume, 0.0, 1.0);
    if (audio_mixer_) {
        audio_mixer_->SetVolume(volume_);
    }
}

void PlaylistPlaybackController::SetMuted(bool muted) {
    muted_ = muted;
    if (audio_mixer_) {
        audio_mixer_->SetMuted(muted_);
    }
}

//=============================================================================
// Clip Information
//=============================================================================

std::string PlaylistPlaybackController::GetCurrentClipName() const {
    if (current_clip_ && !current_clip_->is_gap) {
        return current_clip_->name;
    }
    return "";
}

std::string PlaylistPlaybackController::GetCurrentSourcePath() const {
    if (current_clip_ && !current_clip_->is_gap) {
        return current_clip_->source_path;
    }
    return "";
}

PipelineMode PlaylistPlaybackController::GetCurrentPipelineMode() const {
    if (cache_) {
        return cache_->GetClipPipelineMode(timer_->GetCurrentFrame());
    }
    return PipelineMode::NORMAL;
}

//=============================================================================
// Scrub Mode
//=============================================================================

void PlaylistPlaybackController::SetScrubMode(bool enabled) {
    is_scrubbing_ = enabled;
    if (cache_) {
        cache_->SetAggressiveScrubMode(enabled);
    }
}

//=============================================================================
// Fast Seek
//=============================================================================

void PlaylistPlaybackController::StartRewind() {
    if (!initialized_) return;

    is_fast_seeking_ = true;
    fast_forward_ = false;
    fast_seek_speed_ = 2.0;
    fast_seek_start_time_ = std::chrono::steady_clock::now();
    last_fast_seek_update_ = fast_seek_start_time_;

    if (cache_) {
        cache_->SetShuttleMode(true, -1);
    }
}

void PlaylistPlaybackController::StartFastForward() {
    if (!initialized_) return;

    is_fast_seeking_ = true;
    fast_forward_ = true;
    fast_seek_speed_ = 2.0;
    fast_seek_start_time_ = std::chrono::steady_clock::now();
    last_fast_seek_update_ = fast_seek_start_time_;

    if (cache_) {
        cache_->SetShuttleMode(true, 1);
    }
}

void PlaylistPlaybackController::StopFastSeek() {
    if (!is_fast_seeking_) return;

    is_fast_seeking_ = false;

    if (cache_) {
        cache_->ExitShuttleMode();
    }
}

void PlaylistPlaybackController::UpdateFastSeek() {
    if (!is_fast_seeking_ || !timer_) return;

    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration<double>(now - last_fast_seek_update_).count();
    last_fast_seek_update_ = now;

    // Accelerate over time
    auto total_elapsed = std::chrono::duration<double>(now - fast_seek_start_time_).count();
    fast_seek_speed_ = std::min(32.0, 2.0 * std::pow(2.0, total_elapsed));

    // Calculate new position
    double delta = elapsed * fast_seek_speed_ * (fast_forward_ ? 1.0 : -1.0);
    double new_pos = std::clamp(GetPosition() + delta, 0.0, timeline_duration_);

    timer_->Seek(new_pos);

    if (cache_) {
        int frame = static_cast<int>(new_pos * fps_);
        cache_->UpdatePlayhead(frame, false);
    }
}

//=============================================================================
// Notify of Timeline Edits
//=============================================================================

void PlaylistPlaybackController::NotifyTracksEdited() {
    if (!initialized_ || !timeline_) return;

    // Update duration
    timeline_duration_ = timeline_->GetDuration();
    if (timer_) {
        timer_->SetDuration(timeline_duration_);
    }

    // Update canvas dimensions
    width_ = timeline_->GetCanvasWidth();
    height_ = timeline_->GetCanvasHeight();

    // Rebuild flattener with new track data
    if (flattener_) {
        OTIOTrack track = timeline_->ToOTIOTrack();
        std::vector<OTIOTrack> tracks = { track };
        flattener_->SetTracks(tracks);
    }

    // Notify cache
    if (cache_) {
        cache_->NotifyTracksEdited();
        cache_->SetCanvasDimensions(width_, height_);
    }

    Debug::Log("[PlaylistPlaybackController] Tracks edited - duration: " + std::to_string(timeline_duration_) + "s");
}

void PlaylistPlaybackController::UpdateDuration(double new_duration) {
    timeline_duration_ = new_duration;
    if (timer_) {
        timer_->SetDuration(new_duration);
    }
    if (cache_) {
        cache_->UpdateDuration(new_duration);
    }
}

//=============================================================================
// Internal Methods
//=============================================================================

void PlaylistPlaybackController::CheckClipBoundary() {
    if (!timeline_) return;

    double pos = GetPosition();
    PlaylistClip* clip_at_time = timeline_->GetClipAtTime(pos);

    // Skip gaps for clip tracking
    while (clip_at_time && clip_at_time->is_gap) {
        clip_at_time = timeline_->GetNextClip(clip_at_time->GetTimelineEnd());
    }

    if (clip_at_time != current_clip_) {
        PlaylistClip* old_clip = current_clip_;
        current_clip_ = clip_at_time;

        if (clip_transition_callback_) {
            clip_transition_callback_(old_clip, current_clip_);
        }

        if (current_clip_) {
            Debug::Log("[PlaylistPlaybackController] Transitioned to clip '" + current_clip_->name + "'");
        }
    }
}

void PlaylistPlaybackController::TransitionToClip(PlaylistClip* clip) {
    if (!clip || clip == current_clip_) return;

    Debug::Log("[PlaylistPlaybackController] Transitioning to clip '" + clip->name + "'");

    // The cache handles decoder management via its loader system
    // We just need to update playhead position

    PlaylistClip* old_clip = current_clip_;
    current_clip_ = clip;

    // Trigger buffer wait for smooth transition
    if (timer_ && timer_->IsPlaying()) {
        waiting_for_buffer_ = true;
        waiting_start_time_ = std::chrono::steady_clock::now();
    }

    if (clip_transition_callback_) {
        clip_transition_callback_(old_clip, current_clip_);
    }
}

void PlaylistPlaybackController::PrewarmNextClip() {
    // NOTE: Single-decoder mode is handled in TimelineCache now.
    // Pre-warming still works, but TimelineCache skips decoder creation
    // in single-decoder mode.
    if (!timeline_ || !cache_) return;

    double pos = GetPosition();
    double prewarm_time = pos + config_.prewarm_seconds;

    // Find clip that will be playing at prewarm_time
    PlaylistClip* next_clip = timeline_->GetClipAtTime(prewarm_time);

    // Skip gaps
    while (next_clip && next_clip->is_gap) {
        next_clip = timeline_->GetNextClip(next_clip->GetTimelineEnd());
    }

    // Only prewarm if it's a different clip than current
    if (next_clip && next_clip != current_clip_ && next_clip != prewarmed_clip_) {
        prewarmed_clip_ = next_clip;

        // The cache's read-ahead window should automatically pre-warm
        // by loading frames from the next clip. The frame requests
        // go to the flattener which identifies the correct source.

        Debug::Log("[PlaylistPlaybackController] Pre-warming clip '" + next_clip->name +
                   "' for boundary at " + std::to_string(next_clip->timeline_start) + "s");
    }
}

void PlaylistPlaybackController::LoadClipDecoder(PlaylistClip* clip, bool is_prewarm) {
    // The TimelineCache handles decoder creation via GetOrCreateLoader
    // This method exists for future expansion if needed
    (void)clip;
    (void)is_prewarm;
}

void PlaylistPlaybackController::UnloadClipDecoder(PlaylistClip* clip) {
    // The TimelineCache handles decoder cleanup
    (void)clip;
}

GLuint PlaylistPlaybackController::GetCurrentFrame(int& width, int& height) {
    if (!cache_) {
        width = height = 0;
        return 0;
    }

    int frame = current_frame_.load();
    return cache_->GetFrame(frame, width, height);
}

} // namespace qcview
