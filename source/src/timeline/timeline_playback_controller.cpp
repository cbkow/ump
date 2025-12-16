#include "timeline_playback_controller.h"
#include "timeline_view.h"
#include "timeline_cache.h"
#include "../player/video_player.h"
#include "../player/playback_timer.h"
#include "../audio/audio_mixer.h"
#include "../utils/debug_utils.h"

#include <cmath>
#include <sstream>
#include <iomanip>

// Global timeline cache settings (defined in main.cpp)
extern int g_timeline_read_ahead_frames;
extern float g_timeline_read_behind_seconds;
extern int g_timeline_max_textures;
extern int g_timeline_io_threads;

namespace ump {

TimelinePlaybackController::TimelinePlaybackController() {
    config_.scratch_duration = 1.0;  // Start at 1 second - auto-extends as clips are added
    // Use global settings instead of hardcoded defaults
    config_.readAheadFrames = g_timeline_read_ahead_frames;
    config_.readBehindSeconds = g_timeline_read_behind_seconds;
    config_.io_threads = g_timeline_io_threads;
}

TimelinePlaybackController::~TimelinePlaybackController() {
    Shutdown();
}

// NOTE: Old dummy-based InitializeForTimeline and InitializeForScratchTimeline removed
// Use InitializeForVirtualTimeline and InitializeForVirtualScratchTimeline instead

bool TimelinePlaybackController::InitializeCacheForScratchTimeline(TimelineView* timeline_view) {
    if (!initialized_) {
        Debug::Log("TimelinePlaybackController::InitializeCacheForScratchTimeline: Controller not initialized");
        return false;
    }

    if (!timeline_view) {
        Debug::Log("TimelinePlaybackController::InitializeCacheForScratchTimeline: No timeline view provided");
        return false;
    }

    if (cache_) {
        // Cache already exists - tracks are updated via flattener reference
        // NotifyTracksEdited() will be called by the caller to refresh the cache
        Debug::Log("TimelinePlaybackController::InitializeCacheForScratchTimeline: Cache already exists");
        return true;
    }

    // Store reference to timeline view (was nullptr for scratch timelines)
    timeline_view_ = timeline_view;

    Debug::Log("TimelinePlaybackController::InitializeCacheForScratchTimeline: Creating cache for scratch timeline");

    // Initialize cache
    cache_ = std::make_unique<TimelineCache>();

    TimelineCacheConfig cache_config;
    cache_config.readAheadFrames = config_.readAheadFrames;
    cache_config.readBehindSeconds = config_.readBehindSeconds;
    cache_config.io_threads = config_.io_threads;
    cache_config.max_textures = g_timeline_max_textures;
    cache_config.fps = fps_;
    cache_config.use_shared_pool = true;

    cache_->SetConfig(cache_config);

    const auto& tracks = timeline_view->GetTracks();
    cache_->Initialize(tracks, &timeline_view->GetFlattener(), fps_);

    // Set gap texture and canvas dimensions to match timeline content
    // This prevents OpenGL corruption on gap transitions and flickering with mixed resolutions
    if (width_ > 0 && height_ > 0) {
        cache_->SetGapTextureDimensions(width_, height_);
        cache_->SetCanvasDimensions(width_, height_);
    }

    Debug::Log("TimelinePlaybackController::InitializeCacheForScratchTimeline: Cache initialized with " +
               std::to_string(tracks.size()) + " tracks");

    // Initialize audio mixer for scratch timeline (same as InitializeForTimeline)
    if (!audio_mixer_) {
        audio_mixer_ = std::make_unique<AudioMixer>();
        if (audio_mixer_->Initialize()) {
            audio_mixer_->SetFlattener(&timeline_view->GetFlattener());
            // Connect timer for sync (virtual timeline mode)
            if (timeline_timer_) {
                audio_mixer_->SetTimer(timeline_timer_.get());
            }

            // Collect all clips with media paths for preloading
            // Don't filter by is_linked - PreloadClips handles path resolution
            std::vector<OTIOClip> all_clips;
            for (const auto& track : tracks) {
                for (const auto& clip : track.clips) {
                    if (!clip.is_gap && (!clip.linked_path.empty() || !clip.file_path.empty())) {
                        all_clips.push_back(clip);
                    }
                }
            }
            audio_mixer_->PreloadClips(all_clips);
            Debug::Log("TimelinePlaybackController: Audio mixer initialized for scratch timeline with " +
                       std::to_string(all_clips.size()) + " clips");
        } else {
            Debug::Log("TimelinePlaybackController: Audio mixer init failed for scratch timeline");
            audio_mixer_.reset();
        }
    } else {
        // Audio mixer already exists - just update flattener and preload new clips
        audio_mixer_->SetFlattener(&timeline_view->GetFlattener());
        if (timeline_timer_) {
            audio_mixer_->SetTimer(timeline_timer_.get());
        }

        std::vector<OTIOClip> all_clips;
        for (const auto& track : tracks) {
            for (const auto& clip : track.clips) {
                if (!clip.is_gap && (!clip.linked_path.empty() || !clip.file_path.empty())) {
                    all_clips.push_back(clip);
                }
            }
        }
        audio_mixer_->PreloadClips(all_clips);
    }

    return true;
}

void TimelinePlaybackController::Shutdown() {
    if (!initialized_) return;

    Debug::Log("TimelinePlaybackController: Shutting down...");

    // Shutdown audio mixer
    if (audio_mixer_) {
        audio_mixer_->Shutdown();
        audio_mixer_.reset();
    }

    // Shutdown virtual timeline timer
    if (timeline_timer_) {
        timeline_timer_.reset();
    }

    // Shutdown cache
    if (cache_) {
        cache_->Shutdown();
        cache_.reset();
    }

    // Clear texture
    if (current_texture_ != 0) {
        glDeleteTextures(1, &current_texture_);
        current_texture_ = 0;
    }

    timeline_view_ = nullptr;
    video_player_ = nullptr;
    use_virtual_timeline_ = false;

    initialized_ = false;
    Debug::Log("TimelinePlaybackController: Shutdown complete");
}

GLuint TimelinePlaybackController::Update(int& width, int& height) {
    if (!initialized_) {
        width = 0;
        height = 0;
        return 0;
    }

    // Sync playhead from virtual timer (always use virtual timeline mode now)
    // This must happen even if cache isn't initialized yet (for scratch timelines)
    UpdateTimer();

    // If no cache yet (scratch timeline with no clips), return black
    if (!cache_) {
        width = 0;
        height = 0;
        return 0;
    }

    int frame = current_frame_.load();
    bool playing = is_playing_.load();

    // Update cache with current playhead
    cache_->UpdatePlayhead(frame, playing);

    // Try to get frame from cache
    int tex_width = 0, tex_height = 0;
    bool got_exact_frame = false;
    GLuint texture = cache_->GetFrame(frame, tex_width, tex_height, &got_exact_frame);

    // Debug: Log cache result during post-edit period
    if (awaiting_post_edit_frame_) {
        static int post_edit_update_counter = 0;
        if (++post_edit_update_counter % 30 == 1) {  // Log every ~0.5 seconds at 60fps
            Debug::Log("TimelinePlaybackController::Update: Post-edit, frame=" + std::to_string(frame) +
                       ", cache returned " + (texture != 0 ? "texture" : "0"));
        }
    }

    if (texture != 0) {
        // Cache hit - store as current frame for future miss handling
        current_texture_ = texture;
        current_texture_width_ = tex_width;
        current_texture_height_ = tex_height;

        // If we were waiting for post-edit frame, we got it - clear pending evict
        if (awaiting_post_edit_frame_) {
            awaiting_post_edit_frame_ = false;
            pending_evict_texture_ = 0;  // Don't delete - it's from cache pool
            pending_evict_width_ = 0;
            pending_evict_height_ = 0;
            Debug::Log("TimelinePlaybackController: Post-edit frame arrived, transition complete");
        }

        width = tex_width;
        height = tex_height;
        return texture;
    }

    // Cache miss - return previous frame to hold (smoother than black flash)
    if (current_texture_ != 0) {
        width = current_texture_width_;
        height = current_texture_height_;
        return current_texture_;
    }

    // Post-edit fallback: use the old texture until new frame arrives
    // This prevents black flashing during the cache reload window
    // Keep showing the old frame until we get a valid new one - no timeout
    // The cache's post-edit mechanism handles blocking stale decoder frames
    if (awaiting_post_edit_frame_ && pending_evict_texture_ != 0) {
        width = pending_evict_width_;
        height = pending_evict_height_;
        return pending_evict_texture_;
    }

    width = 0;
    height = 0;
    return 0;
}

void TimelinePlaybackController::NotifyTracksEdited() {
    // CRITICAL: Clear VideoPlayer's timeline texture reference BEFORE cache clears textures
    // Otherwise VideoPlayer may hold a dangling pointer to a deleted texture = crash
    if (video_player_) {
        video_player_->ClearVideoTextureReference();
    }

    // SMOOTH TRANSITION: Instead of immediately clearing current_texture_ (which causes black flash),
    // move it to pending_evict and set a flag. Update() will use pending_evict as fallback
    // until a new frame is loaded, then evict it.
    if (current_texture_ != 0) {
        pending_evict_texture_ = current_texture_;
        pending_evict_width_ = current_texture_width_;
        pending_evict_height_ = current_texture_height_;
        awaiting_post_edit_frame_ = true;
        post_edit_start_time_ = std::chrono::steady_clock::now();
    }

    // Clear current texture - we'll use pending_evict as fallback until new frame arrives
    current_texture_ = 0;
    current_texture_width_ = 0;
    current_texture_height_ = 0;

    // Notify the cache to clear and refresh (this queues old textures for deletion)
    if (cache_) {
        cache_->NotifyTracksEdited();
    }

    // Refresh audio mixer with any new clips
    if (audio_mixer_ && timeline_view_) {
        const auto& tracks = timeline_view_->GetTracks();
        std::vector<OTIOClip> all_clips;
        for (const auto& track : tracks) {
            for (const auto& clip : track.clips) {
                if (!clip.is_gap && (!clip.linked_path.empty() || !clip.file_path.empty())) {
                    all_clips.push_back(clip);
                }
            }
        }
        // PreloadClips skips already-loaded clips, so this is safe to call repeatedly
        audio_mixer_->PreloadClips(all_clips);
    }

    // Force viewport refresh by re-seeking to current position
    // This triggers MPV to request a new frame, which will come from our refreshed cache
    if (video_player_) {
        double current_pos = video_player_->GetPosition();
        video_player_->Seek(current_pos);
        Debug::Log("TimelinePlaybackController: Re-seek to " + std::to_string(current_pos) + "s to force refresh");
    }

    Debug::Log("TimelinePlaybackController: Tracks edited, awaiting new frame (holding old texture as fallback)");
}

void TimelinePlaybackController::UpdateDuration(double new_duration) {
    if (!initialized_) return;

    double old_duration = timeline_duration_;
    timeline_duration_ = new_duration;

    // Virtual timeline mode: just update timer duration (instant)
    if (timeline_timer_) {
        timeline_timer_->SetDuration(new_duration);
    }

    if (old_duration != new_duration) {
        Debug::Log("TimelinePlaybackController::UpdateDuration: " +
                   std::to_string(old_duration) + "s -> " + std::to_string(new_duration) + "s");
    }
}

// NOTE: RegenerateDummy and CheckAndExtendDummy removed - virtual timeline mode doesn't need dummy videos

void TimelinePlaybackController::ProcessPendingUploads() {
    if (cache_) {
        cache_->ProcessPendingUploads();
    }
}

void TimelinePlaybackController::SetConfig(const TimelinePlaybackConfig& config) {
    config_ = config;

    if (cache_) {
        TimelineCacheConfig cache_config;
        cache_config.readAheadFrames = config_.readAheadFrames;
        cache_config.readBehindSeconds = config_.readBehindSeconds;
        cache_config.io_threads = config_.io_threads;
        cache_config.fps = fps_;
        cache_config.use_shared_pool = true;
        cache_->SetConfig(cache_config);
    }
}

void TimelinePlaybackController::SetLooping(bool enabled) {
    if (cache_) {
        cache_->SetLooping(enabled);
    }
}

bool TimelinePlaybackController::IsLooping() const {
    if (cache_) {
        return cache_->IsLooping();
    }
    return false;
}

bool TimelinePlaybackController::IsPlaying() const {
    return is_playing_.load();
}

std::string TimelinePlaybackController::GetCurrentClipName() const {
    if (!cache_) return "";

    SourceCoords coords = cache_->GetSourceCoords(current_frame_.load());
    return coords.valid ? coords.clip_name : "";
}

std::string TimelinePlaybackController::GetCurrentSourcePath() const {
    if (!cache_) return "";

    SourceCoords coords = cache_->GetSourceCoords(current_frame_.load());
    return coords.valid ? coords.source_path : "";
}

// NOTE: SyncFromMPV and GenerateDummy removed - virtual timeline mode uses PlaybackTimer

//=============================================================================
// Virtual Timeline Mode Implementation
//=============================================================================

bool TimelinePlaybackController::InitializeForVirtualTimeline(
    TimelineView* timeline_view, ::VideoPlayer* video_player,
    int canvas_width, int canvas_height) {

    if (!timeline_view || !video_player) {
        Debug::Log("TimelinePlaybackController: Invalid parameters for virtual timeline");
        return false;
    }

    // Shutdown existing state
    if (initialized_) {
        Shutdown();
    }

    timeline_view_ = timeline_view;
    video_player_ = video_player;

    // Get timeline properties
    timeline_duration_ = timeline_view->GetDuration();
    fps_ = timeline_view->GetFrameRate();

    if (timeline_duration_ <= 0.0) {
        Debug::Log("TimelinePlaybackController: Invalid timeline duration for virtual mode");
        return false;
    }

    Debug::Log("TimelinePlaybackController: Initializing virtual timeline for " +
               std::to_string(timeline_duration_) + "s at " + std::to_string(fps_) + " fps");

    // Get tracks reference for use throughout initialization
    const auto& tracks = timeline_view->GetTracks();

    // Use provided canvas dimensions, or fall back to first linked clip, or default 1920x1080
    if (canvas_width > 0 && canvas_height > 0) {
        width_ = canvas_width;
        height_ = canvas_height;
        Debug::Log("TimelinePlaybackController: Using provided canvas dimensions: " +
                   std::to_string(width_) + "x" + std::to_string(height_));
    } else {
        // Fallback: determine dimensions from first linked clip
        width_ = 1920;
        height_ = 1080;

        for (const auto& track : tracks) {
            if (!track.is_video) continue;
            for (const auto& clip : track.clips) {
                if (clip.is_linked && !clip.linked_path.empty()) {
                    if (clip.source_width > 0 && clip.source_height > 0) {
                        width_ = clip.source_width;
                        height_ = clip.source_height;
                        Debug::Log("TimelinePlaybackController: Using first clip dimensions: " +
                                   std::to_string(width_) + "x" + std::to_string(height_));
                    }
                    break;
                }
            }
        }
    }

    // Set content dimensions in VideoPlayer
    video_player_->SetContentDimensions(width_, height_);

    // Create PlaybackTimer (the virtual timeline clock)
    timeline_timer_ = std::make_unique<PlaybackTimer>();
    timeline_timer_->SetDuration(timeline_duration_);
    timeline_timer_->SetFrameRate(fps_);
    timeline_timer_->SetLooping(true);
    timeline_timer_->Seek(0.0);

    // Setup position change callback - updates frame index and audio
    timeline_timer_->SetOnPositionChanged([this](double pos) {
        current_frame_ = static_cast<int>(std::round(pos * fps_));
        if (audio_mixer_) {
            audio_mixer_->SetTimelinePosition(pos);
        }
    });

    timeline_timer_->SetOnLoop([this]() {
        Debug::Log("TimelinePlaybackController: Virtual timeline looped");
    });

    timeline_timer_->SetOnEnd([this]() {
        Debug::Log("TimelinePlaybackController: Virtual timeline ended");
        is_playing_ = false;
        if (audio_mixer_) {
            audio_mixer_->Pause();
        }
    });

    // Initialize cache
    cache_ = std::make_unique<TimelineCache>();

    TimelineCacheConfig cache_config;
    cache_config.readAheadFrames = config_.readAheadFrames;
    cache_config.readBehindSeconds = config_.readBehindSeconds;
    cache_config.io_threads = config_.io_threads;
    cache_config.max_textures = g_timeline_max_textures;
    cache_config.fps = fps_;
    cache_config.use_shared_pool = true;

    cache_->SetConfig(cache_config);
    cache_->Initialize(tracks, &timeline_view_->GetFlattener(), fps_);

    if (width_ > 0 && height_ > 0) {
        cache_->SetGapTextureDimensions(width_, height_);
        cache_->SetCanvasDimensions(width_, height_);  // Ensure consistent output dimensions
    }

    // Initialize audio mixer
    audio_mixer_ = std::make_unique<AudioMixer>();
    if (audio_mixer_->Initialize()) {
        audio_mixer_->SetFlattener(&timeline_view_->GetFlattener());
        audio_mixer_->SetTimer(timeline_timer_.get());  // Direct timer access for sync

        // Collect all clips with media paths for preloading
        std::vector<OTIOClip> all_clips;
        for (const auto& track : tracks) {
            for (const auto& clip : track.clips) {
                if (!clip.is_gap && (!clip.linked_path.empty() || !clip.file_path.empty())) {
                    all_clips.push_back(clip);
                }
            }
        }
        audio_mixer_->PreloadClips(all_clips);
        Debug::Log("TimelinePlaybackController: Virtual timeline audio mixer initialized with " +
                   std::to_string(all_clips.size()) + " clips");
    } else {
        Debug::Log("TimelinePlaybackController: Virtual timeline audio mixer init failed");
        audio_mixer_.reset();
    }

    use_virtual_timeline_ = true;
    initialized_ = true;
    Debug::Log("TimelinePlaybackController: Virtual timeline initialized successfully");
    return true;
}

bool TimelinePlaybackController::InitializeForVirtualScratchTimeline(
    ::VideoPlayer* video_player, int width, int height, double fps) {

    if (!video_player) {
        Debug::Log("TimelinePlaybackController: Invalid video player for virtual scratch timeline");
        return false;
    }

    if (initialized_) {
        Shutdown();
    }

    timeline_view_ = nullptr;  // No timeline view yet
    video_player_ = video_player;

    width_ = width;
    height_ = height;
    fps_ = fps;
    timeline_duration_ = config_.scratch_duration;  // 10 minutes default

    Debug::Log("TimelinePlaybackController: Initializing virtual scratch timeline " +
               std::to_string(width_) + "x" + std::to_string(height_) +
               " @ " + std::to_string(fps_) + " fps, " +
               std::to_string(timeline_duration_) + "s duration");

    // Set content dimensions in VideoPlayer
    video_player_->SetContentDimensions(width_, height_);

    // Create PlaybackTimer
    timeline_timer_ = std::make_unique<PlaybackTimer>();
    timeline_timer_->SetDuration(timeline_duration_);
    timeline_timer_->SetFrameRate(fps_);
    timeline_timer_->SetLooping(true);
    timeline_timer_->Seek(0.0);

    // Setup callbacks
    timeline_timer_->SetOnPositionChanged([this](double pos) {
        current_frame_ = static_cast<int>(std::round(pos * fps_));
        if (audio_mixer_) {
            audio_mixer_->SetTimelinePosition(pos);
        }
    });

    // No cache yet - will be initialized when first clip is added
    use_virtual_timeline_ = true;
    initialized_ = true;
    Debug::Log("TimelinePlaybackController: Virtual scratch timeline initialized (cache will be created when clips are added)");
    return true;
}

void TimelinePlaybackController::UpdateTimer() {
    if (!timeline_timer_ || !use_virtual_timeline_) {
        return;
    }

    auto now = std::chrono::steady_clock::now();

    // Initialize timing on first call
    if (!timer_initialized_) {
        last_timer_update_ = now;
        accumulated_time_ = 0.0;
        timer_initialized_ = true;
    }

    // Simple wall-clock based timing: let PlaybackTimer handle elapsed time directly
    // Timer always advances at real-time speed - if frames aren't ready, we hold/skip
    // This prevents video from playing slower than audio
    if (timeline_timer_->IsPlaying()) {
        timeline_timer_->Update();  // Advances position based on wall clock
    }

    // Update current_frame_ from timer position
    // Use rounding (+ 0.5) to match decoder's PTS-based frame numbering
    current_frame_ = static_cast<int>(timeline_timer_->GetPosition() * fps_ + 0.5);

    // Process pending GPU uploads from I/O worker threads
    // This MUST happen on the GL thread to create textures
    if (cache_) {
        cache_->ProcessPendingUploads();
    }

    // Update audio mixer
    if (audio_mixer_) {
        audio_mixer_->Update();
    }
}

double TimelinePlaybackController::GetPosition() const {
    if (use_virtual_timeline_ && timeline_timer_) {
        return timeline_timer_->GetPosition();
    }
    // Fallback for dummy mode: calculate from frame
    return static_cast<double>(current_frame_.load()) / fps_;
}

void TimelinePlaybackController::Play() {
    if (use_virtual_timeline_ && timeline_timer_) {
        timeline_timer_->Play();
        is_playing_ = true;

        // Reset timing accumulator to start fresh
        accumulated_time_ = 0.0;
        last_timer_update_ = std::chrono::steady_clock::now();

        // Start audio immediately - both audio and video run at wall-clock speed
        if (audio_mixer_) {
            audio_mixer_->Play();
        }
    }
}

void TimelinePlaybackController::Pause() {
    if (use_virtual_timeline_ && timeline_timer_) {
        timeline_timer_->Pause();
        is_playing_ = false;
        if (audio_mixer_) {
            audio_mixer_->Pause();
        }
    }
}

void TimelinePlaybackController::TogglePlayPause() {
    if (is_playing_.load()) {
        Pause();
    } else {
        Play();
    }
}

void TimelinePlaybackController::Seek(double position) {
    if (use_virtual_timeline_ && timeline_timer_) {
        // Clamp position
        if (position < 0) position = 0;
        if (position > timeline_duration_) position = timeline_duration_;

        timeline_timer_->Seek(position);

        // Reset timing accumulator to prevent accumulated time from causing jumps
        accumulated_time_ = 0.0;
        last_timer_update_ = std::chrono::steady_clock::now();

        // Seek audio to new position (keeps playing if it was playing)
        if (audio_mixer_) {
            audio_mixer_->Seek(position);
        }

        // IMMEDIATELY update cache (don't wait for next Update() cycle)
        // This prevents seek-to-cache propagation delay that causes desync
        // Use rounding to match decoder's PTS-based frame numbering
        int target_frame = static_cast<int>(position * fps_ + 0.5);
        current_frame_ = target_frame;
        if (cache_) {
            cache_->UpdatePlayhead(target_frame, timeline_timer_->IsPlaying());
        }
    }
}

void TimelinePlaybackController::SeekRelative(double delta) {
    if (use_virtual_timeline_ && timeline_timer_) {
        double new_pos = timeline_timer_->GetPosition() + delta;
        Seek(new_pos);
    }
}

void TimelinePlaybackController::StepForward(int frames) {
    if (use_virtual_timeline_ && timeline_timer_) {
        timeline_timer_->StepForward(frames);
    }
}

void TimelinePlaybackController::StepBackward(int frames) {
    if (use_virtual_timeline_ && timeline_timer_) {
        timeline_timer_->StepBackward(frames);
    }
}

void TimelinePlaybackController::GoToStart() {
    Seek(0.0);
}

void TimelinePlaybackController::GoToEnd() {
    Seek(timeline_duration_);
}

} // namespace ump
