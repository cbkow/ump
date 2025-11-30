#include "timeline_playback_controller.h"
#include "timeline_view.h"
#include "timeline_cache.h"
#include "../player/video_player.h"
#include "../player/dummy_video_generator.h"
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
    config_.scratch_duration = 600.0;  // 10 minutes default
    config_.min_extension_margin = 60.0;
    // Use global settings instead of hardcoded defaults
    config_.readAheadFrames = g_timeline_read_ahead_frames;
    config_.readBehindSeconds = g_timeline_read_behind_seconds;
    config_.io_threads = g_timeline_io_threads;
}

TimelinePlaybackController::~TimelinePlaybackController() {
    Shutdown();
}

bool TimelinePlaybackController::InitializeForTimeline(TimelineView* timeline_view,
                                                        ::VideoPlayer* video_player,
                                                        DummyVideoGenerator* dummy_generator) {
    if (!timeline_view || !video_player || !dummy_generator) {
        Debug::Log("TimelinePlaybackController: Invalid parameters");
        return false;
    }

    // Shutdown existing state
    if (initialized_) {
        Shutdown();
    }

    timeline_view_ = timeline_view;
    video_player_ = video_player;
    dummy_generator_ = dummy_generator;

    // Get timeline properties
    timeline_duration_ = timeline_view->GetDuration();
    fps_ = timeline_view->GetFrameRate();

    if (timeline_duration_ <= 0.0) {
        Debug::Log("TimelinePlaybackController: Invalid timeline duration");
        return false;
    }

    Debug::Log("TimelinePlaybackController: Initializing for " +
               std::to_string(timeline_duration_) + "s timeline at " +
               std::to_string(fps_) + " fps");

    // Determine dimensions from first linked clip
    width_ = 1920;
    height_ = 1080;

    const auto& tracks = timeline_view->GetTracks();
    for (const auto& track : tracks) {
        if (!track.is_video) continue;
        for (const auto& clip : track.clips) {
            if (clip.is_linked && !clip.linked_path.empty()) {
                // TODO: Get dimensions from first linked clip
                // For now, use default
                Debug::Log("TimelinePlaybackController: First linked clip: " + clip.linked_path);
                break;
            }
        }
    }

    // Set content dimensions in VideoPlayer (enables 1x1 dummy video optimization)
    // VideoPlayer will use these for texture creation instead of querying MPV
    video_player_->SetContentDimensions(width_, height_);

    // Generate dummy video matching timeline duration
    dummy_duration_ = timeline_duration_;
    dummy_path_ = GenerateDummy(width_, height_, fps_, dummy_duration_);
    if (dummy_path_.empty()) {
        Debug::Log("TimelinePlaybackController: Failed to generate dummy video");
        return false;
    }

    Debug::Log("TimelinePlaybackController: Dummy video: " + dummy_path_);

    // Load dummy into MPV
    video_player_->LoadFile(dummy_path_);
    // Note: LoadFile returns void, assume success if no exception

    // Pause immediately (timeline starts paused)
    video_player_->Pause();

    // Initialize cache
    cache_ = std::make_unique<TimelineCache>();

    TimelineCacheConfig cache_config;
    cache_config.readAheadFrames = config_.readAheadFrames;
    cache_config.readBehindSeconds = config_.readBehindSeconds;
    cache_config.io_threads = config_.io_threads;
    cache_config.max_textures = g_timeline_max_textures;  // Use global setting
    cache_config.fps = fps_;
    cache_config.use_shared_pool = true;

    cache_->SetConfig(cache_config);
    cache_->Initialize(tracks, &timeline_view_->GetFlattener(), fps_);

    initialized_ = true;
    Debug::Log("TimelinePlaybackController: Initialized successfully");
    return true;
}

bool TimelinePlaybackController::InitializeForScratchTimeline(::VideoPlayer* video_player,
                                                               DummyVideoGenerator* dummy_generator,
                                                               int width, int height, double fps) {
    if (!video_player || !dummy_generator) {
        Debug::Log("TimelinePlaybackController: Invalid parameters");
        return false;
    }

    if (initialized_) {
        Shutdown();
    }

    timeline_view_ = nullptr;  // No timeline view for scratch
    video_player_ = video_player;
    dummy_generator_ = dummy_generator;

    width_ = width;
    height_ = height;
    fps_ = fps;
    timeline_duration_ = config_.scratch_duration;
    dummy_duration_ = config_.scratch_duration;

    Debug::Log("TimelinePlaybackController: Initializing scratch timeline " +
               std::to_string(width_) + "x" + std::to_string(height_) +
               " @ " + std::to_string(fps_) + " fps, " +
               std::to_string(dummy_duration_) + "s duration");

    // Set content dimensions in VideoPlayer (enables 1x1 dummy video optimization)
    video_player_->SetContentDimensions(width_, height_);

    // Generate long-duration dummy
    dummy_path_ = GenerateDummy(width_, height_, fps_, dummy_duration_);
    if (dummy_path_.empty()) {
        Debug::Log("TimelinePlaybackController: Failed to generate scratch dummy");
        return false;
    }

    // Load dummy into MPV
    video_player_->LoadFile(dummy_path_);
    // Note: LoadFile returns void, assume success if no exception

    video_player_->Pause();

    // No cache for scratch timeline until clips are added
    // Cache will be initialized by InitializeCacheForScratchTimeline() when first clip is added
    initialized_ = true;
    Debug::Log("TimelinePlaybackController: Scratch timeline initialized (cache will be created when clips are added)");
    return true;
}

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

    Debug::Log("TimelinePlaybackController::InitializeCacheForScratchTimeline: Cache initialized with " +
               std::to_string(tracks.size()) + " tracks");
    return true;
}

void TimelinePlaybackController::Shutdown() {
    if (!initialized_) return;

    Debug::Log("TimelinePlaybackController: Shutting down...");

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
    dummy_generator_ = nullptr;
    dummy_path_.clear();

    initialized_ = false;
    Debug::Log("TimelinePlaybackController: Shutdown complete");
}

void TimelinePlaybackController::ReloadDummy() {
    if (!initialized_ || dummy_path_.empty() || !video_player_) {
        Debug::Log("TimelinePlaybackController::ReloadDummy: Cannot reload - not initialized or no dummy path");
        return;
    }
    Debug::Log("TimelinePlaybackController::ReloadDummy: Reloading dummy video: " + dummy_path_);
    video_player_->LoadFile(dummy_path_);
}

GLuint TimelinePlaybackController::Update(int& width, int& height) {
    if (!initialized_ || !cache_) {
        width = 0;
        height = 0;
        return 0;
    }

    // Sync playhead from MPV
    SyncFromMPV();

    int frame = current_frame_.load();
    bool playing = is_playing_.load();

    // Update cache with current playhead
    cache_->UpdatePlayhead(frame, playing);

    // Try to get frame from cache
    int tex_width = 0, tex_height = 0;
    GLuint texture = cache_->GetFrame(frame, tex_width, tex_height);

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

    // Notify the cache to clear and refresh
    if (cache_) {
        cache_->NotifyTracksEdited();
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

    // Only regenerate dummy if timeline grew BEYOND what the dummy can handle.
    // If timeline shrinks, the dummy can stay longer - it doesn't hurt.
    // This avoids the heavy LoadFile() call on most edits.
    if (new_duration > dummy_duration_) {
        RegenerateDummy(new_duration);
    }

    if (old_duration != new_duration) {
        Debug::Log("TimelinePlaybackController::UpdateDuration: " +
                   std::to_string(old_duration) + "s -> " + std::to_string(new_duration) + "s" +
                   " (dummy=" + std::to_string(dummy_duration_) + "s)");
    }
}

bool TimelinePlaybackController::RegenerateDummy(double new_duration) {
    if (!initialized_ || !dummy_generator_ || !video_player_) return false;

    // Add margin of a few frames to avoid regenerating on every tiny extension
    // But keep it small for frame accuracy
    double frame_time = 1.0 / fps_;
    double target_duration = new_duration + (frame_time * 10);  // 10 frames margin

    Debug::Log("TimelinePlaybackController: Regenerating dummy for " +
               std::to_string(new_duration) + "s timeline...");

    std::string new_dummy = GenerateDummy(width_, height_, fps_, target_duration);

    if (new_dummy.empty()) {
        Debug::Log("TimelinePlaybackController: Failed to regenerate dummy");
        return false;
    }

    // Use lightweight swap - doesn't clear caches or reset state
    video_player_->SwapTimelineDummy(new_dummy);

    dummy_path_ = new_dummy;
    dummy_duration_ = target_duration;
    timeline_duration_ = new_duration;

    Debug::Log("TimelinePlaybackController: Dummy regenerated to " +
               std::to_string(target_duration) + "s");
    return true;
}

bool TimelinePlaybackController::CheckAndExtendDummy(double new_duration) {
    // Legacy method - just call RegenerateDummy
    return RegenerateDummy(new_duration);
}

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

void TimelinePlaybackController::SyncFromMPV() {
    if (!video_player_) return;

    // Get position from MPV and convert to frame
    // Use rounding (like EXR cache) for frame-accurate calculation
    double pos = video_player_->GetPosition();
    int frame = static_cast<int>(std::round(pos * fps_));

    // Clamp to valid range
    if (frame < 0) frame = 0;

    bool playing = video_player_->IsPlaying();

    current_frame_ = frame;
    is_playing_ = playing;
}

std::string TimelinePlaybackController::GenerateDummy(int width, int height,
                                                       double fps, double duration) {
    if (!dummy_generator_) return "";

    return dummy_generator_->GetDummyFor(width, height, fps, duration);
}

} // namespace ump
