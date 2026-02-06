#include "timeline_playback_controller.h"
#include "timeline_view.h"
#include "timeline_cache.h"
#include "timeline_thumbnail_cache.h"
#include "../player/video_player.h"
#include "../player/playback_timer.h"
#include "../player/image_loaders.h"
#include "../audio/audio_mixer.h"
#include "../utils/debug_utils.h"

#ifdef _WIN32
#include "../player/d3d11va_video_decoder.h"
#include "../player/d3d11_video_decoder.h"
#include "../player/dual_view_pipeline.h"
#include "../gpu/d3d11_device_manager.h"
#include "../gpu/dual_view_layout.h"
#include "../audio/audio_player.h"
#endif

#include <cmath>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <filesystem>

// Global timeline cache settings (defined in main.cpp)
extern int g_timeline_read_ahead_frames;
extern int g_timeline_read_behind_frames;
extern int g_timeline_max_textures;
extern int g_timeline_io_threads;

namespace ump {

// Helper function to detect the correct pipeline mode for an image sequence
// EXR sequences always use ULTRA_HIGH_RES (half-float), others use GetImageInfo
static PipelineMode DetectSequencePipelineMode(const OTIOClip& clip) {
    // Check if this is an EXR sequence by examining the pattern extension
    std::string pattern_lower = clip.sequence_pattern;
    std::transform(pattern_lower.begin(), pattern_lower.end(), pattern_lower.begin(), ::tolower);

    if (pattern_lower.find(".exr") != std::string::npos) {
        // EXR sequences are always half-float - no user choice
        Debug::Log("DetectSequencePipelineMode: EXR sequence detected -> ULTRA_HIGH_RES");
        return PipelineMode::ULTRA_HIGH_RES;
    }

    // For non-EXR sequences, probe the first frame to determine bit depth
    // Build the path to the first frame using the pattern
    char frame_path[1024];
    std::filesystem::path dir(clip.sequence_directory);
    snprintf(frame_path, sizeof(frame_path), clip.sequence_pattern.c_str(), clip.sequence_start_frame);
    std::filesystem::path full_path = dir / frame_path;

    ImageInfo info;
    if (GetImageInfo(full_path.string(), info)) {
        Debug::Log("DetectSequencePipelineMode: " + full_path.string() + " -> " +
                   std::to_string(info.bit_depth) + "-bit " +
                   (info.is_float ? "float" : "int"));
        return info.recommended_pipeline;
    }

    // Fallback to NORMAL if we can't detect
    Debug::Log("DetectSequencePipelineMode: Could not detect format, defaulting to NORMAL");
    return PipelineMode::NORMAL;
}

TimelinePlaybackController::TimelinePlaybackController() {
    config_.scratch_duration = 1.0;  // Start at 1 second - auto-extends as clips are added
    // Use global settings instead of hardcoded defaults
    config_.readAheadFrames = g_timeline_read_ahead_frames;
    config_.readBehindFrames = g_timeline_read_behind_frames;
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
    cache_config.readBehindFrames = config_.readBehindFrames;
    cache_config.io_threads = config_.io_threads;
    cache_config.max_textures = g_timeline_max_textures;
    cache_config.fps = fps_;
    cache_config.use_shared_pool = true;
    cache_config.pipeline_mode = config_.pipeline_mode;  // Pass through video pipeline mode

    cache_->SetConfig(cache_config);

    const auto& tracks = timeline_view->GetTracks();
    cache_->Initialize(tracks, &timeline_view->GetFlattener(), fps_,
                       timeline_view->GetSourceMode());

    // Compute max source width for resolution-based buffer thresholds
    max_source_width_ = 0;
    for (const auto& track : tracks) {
        if (!track.is_video) continue;
        for (const auto& clip : track.clips) {
            if (clip.is_linked && clip.source_width > max_source_width_) {
                max_source_width_ = clip.source_width;
            }
        }
    }
    Debug::Log("TimelinePlaybackController: Scratch timeline max source width: " +
               std::to_string(max_source_width_));

    // Register sequence metadata for any image sequence clips
    // This must happen after cache is created but before it starts loading frames
    for (const auto& track : tracks) {
        for (const auto& clip : track.clips) {
            if (clip.is_sequence && clip.is_linked && !clip.linked_path.empty()) {
                SequenceMetadata seq_meta;
                seq_meta.directory = clip.sequence_directory;
                seq_meta.pattern = clip.sequence_pattern;
                seq_meta.start_frame = clip.sequence_start_frame;
                seq_meta.end_frame = clip.sequence_end_frame;
                seq_meta.exr_layer = clip.sequence_exr_layer;
                seq_meta.pipeline_mode = DetectSequencePipelineMode(clip);
                seq_meta.valid = true;
                cache_->RegisterSequenceMetadata(clip.linked_path, seq_meta);
            }
        }
    }

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

#ifdef _WIN32
    // Shutdown D3D11VA HDR decoder
    if (d3d11va_decoder_) {
        d3d11va_decoder_->Shutdown();
        d3d11va_decoder_.reset();
        use_d3d11va_hdr_ = false;
    }

    // Shutdown D3D11 GPU-native decoder
    if (d3d11_decoder_) {
        d3d11_decoder_->Shutdown();
        d3d11_decoder_.reset();
        use_d3d11_decoder_ = false;
    }
#endif

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

    // Shutdown dual view resources
    if (right_cache_) {
        right_cache_->Shutdown();
        right_cache_.reset();
    }
    left_flattener_.reset();
    right_flattener_.reset();
    dual_view_mode_ = false;

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

#ifdef _WIN32
    // D3D11VA HDR mode: FFmpeg D3D11VA decode with D3D11-to-GL interop
    if (use_d3d11va_hdr_ && d3d11va_decoder_) {
        // Sync playhead from virtual timer
        UpdateTimer();

        int frame = current_frame_.load();

        // Use OUR timeline duration/fps (from container metadata via FFmpeg)
        // NOT FFmpeg's frame count which may differ slightly
        // This ensures we loop based on playhead position, not decoder internal state
        double fps = fps_;
        int total_frames = static_cast<int>(timeline_duration_ * fps + 0.5);

        // Account for embedded timecode offset
        // If timecode starts at frame N (e.g., 00:00:00:01 = frame 1), decoder's frame indices
        // are offset by N from what the container reports. Subtract to get actual frame count.
        int timecode_offset = config_.timecode_start_frame;
        if (timecode_offset > 0) {
            total_frames -= timecode_offset;
            if (total_frames < 1) total_frames = 1;
        }

        // Use probed actual_last_frame if available (discovered during init)
        // This is the real last decodable frame before decoder hits EOS
        // Add small margin for decoder's read-ahead
        int actual_last = d3d11va_decoder_->GetActualLastFrame();
        int safe_last_frame;
        if (actual_last >= 0) {
            // Use probed value with small margin for read-ahead safety
            safe_last_frame = actual_last - kD3D11VASafetyMarginFrames;
        } else {
            // Fallback: use container frame count with margin
            safe_last_frame = total_frames - kD3D11VASafetyMarginFrames - 1;
        }
        if (safe_last_frame < 0) safe_last_frame = 0;

        // Determine loop/playback boundaries from our timeline, not decoder
        int boundary_start = 0;
        int boundary_end = safe_last_frame;

        // Use In/Out points if set
        if (timeline_view_ && timeline_view_->HasTimelineInOutPoints()) {
            boundary_start = static_cast<int>(timeline_view_->GetTimelineInPoint() * fps + 0.5);
            boundary_end = static_cast<int>(timeline_view_->GetTimelineOutPoint() * fps + 0.5);
        }

        // Clamp boundary_end to safe last frame
        if (boundary_end > safe_last_frame) {
            boundary_end = safe_last_frame;
        }

        // Clamp frame to valid range
        if (frame > boundary_end) {
            frame = boundary_end;
        }
        if (frame < boundary_start) {
            frame = boundary_start;
        }

        // If waiting for buffer (after seek/loop), return last texture
        // UpdateTimer will handle decoding and resuming
        if (waiting_for_frame_) {
            if (current_texture_ != 0) {
                width = current_texture_width_;
                height = current_texture_height_;
                return current_texture_;
            }
            width = 0;
            height = 0;
            return 0;
        }

        // Loop/end handling for D3D11VA HDR mode
        // Triggered by OUR playhead position, not decoder's internal state
        // Strategy: Pause cleanly on last frame, then restart fresh from frame 0
        if (is_playing_.load() && frame >= boundary_end) {
            bool is_looping = timeline_view_ && timeline_view_->IsTimelineLooping();

            Debug::Log("D3D11VA_HDR LOOP: frame=" + std::to_string(frame) +
                       " >= boundary_end=" + std::to_string(boundary_end) +
                       ", total_frames=" + std::to_string(total_frames) +
                       " (tc_offset=" + std::to_string(timecode_offset) + ")" +
                       ", actual_last=" + std::to_string(actual_last) +
                       ", safe_last=" + std::to_string(safe_last_frame) +
                       ", looping=" + std::to_string(is_looping));

            // Step 1: Pause everything first (stops decoder's background read-ahead)
            if (timeline_timer_) {
                timeline_timer_->Pause();
            }
            is_playing_ = false;  // Mark as not playing during transition

            if (is_looping) {
                // Step 2: Set up loop restart state
                // Don't seek decoder yet - let it fully stop first
                // The seek will happen in UpdateTimer's buffer wait handling
                double start_time = static_cast<double>(boundary_start) / fps;
                if (timeline_timer_) {
                    timeline_timer_->Seek(start_time);
                }
                current_frame_ = boundary_start;

                Debug::Log("D3D11VA_HDR LOOP: Seeking to boundary_start=" + std::to_string(boundary_start) +
                           " (time=" + std::to_string(start_time) + "s)");

                // Step 3: Enter buffer wait with seek flag
                // Decoder will be seeked to boundary_start before resuming
                waiting_for_frame_ = true;
                d3d11va_needs_seek_ = true;  // Signal that decoder needs to seek before decoding
                waiting_start_time_ = std::chrono::steady_clock::now();
                is_playing_ = true;  // Mark as playing so buffer wait will resume

                // Return cached texture (last successfully decoded frame) while transitioning
                width = current_texture_width_;
                height = current_texture_height_;
                return current_texture_;
            } else {
                // Not looping - stay paused on last frame
                // Don't request any more frames from decoder
                width = current_texture_width_;
                height = current_texture_height_;
                return current_texture_;
            }
        }

        // Get frame from D3D11VA decoder as GL texture
        GLuint texture = d3d11va_decoder_->GetFrameAsGLTexture(frame);

        if (texture != 0) {
            current_texture_ = texture;
            current_texture_width_ = d3d11va_decoder_->GetWidth();
            current_texture_height_ = d3d11va_decoder_->GetHeight();

            width = current_texture_width_;
            height = current_texture_height_;
            return texture;
        }

        // Fallback to previous frame on decode failure
        if (current_texture_ != 0) {
            width = current_texture_width_;
            height = current_texture_height_;
            return current_texture_;
        }

        width = 0;
        height = 0;
        return 0;
    }

    // D3D11 GPU-native mode: New D3D11VideoDecoder (supports HW + SW decode)
    if (use_d3d11_decoder_ && d3d11_decoder_) {
        // Sync playhead from virtual timer
        UpdateTimer();

        int frame = current_frame_.load();
        double fps = fps_;
        int total_frames = static_cast<int>(timeline_duration_ * fps + 0.5);

        // Determine loop/playback boundaries
        int boundary_start = 0;
        int boundary_end = total_frames - 1;

        // Use In/Out points if set
        if (timeline_view_ && timeline_view_->HasTimelineInOutPoints()) {
            boundary_start = static_cast<int>(timeline_view_->GetTimelineInPoint() * fps + 0.5);
            boundary_end = static_cast<int>(timeline_view_->GetTimelineOutPoint() * fps + 0.5);
        }

        // Clamp frame to valid range
        if (frame > boundary_end) {
            frame = boundary_end;
        }
        if (frame < boundary_start) {
            frame = boundary_start;
        }

        // If waiting for buffer (after seek/loop), return last texture
        if (waiting_for_frame_) {
            if (current_texture_ != 0) {
                width = current_texture_width_;
                height = current_texture_height_;
                return current_texture_;
            }
            width = 0;
            height = 0;
            return 0;
        }

        // Loop/end handling
        if (is_playing_.load() && frame >= boundary_end) {
            bool is_looping = timeline_view_ && timeline_view_->IsTimelineLooping();

            Debug::Log("D3D11_DECODER LOOP: frame=" + std::to_string(frame) +
                       " >= boundary_end=" + std::to_string(boundary_end) +
                       ", looping=" + std::to_string(is_looping));

            // Pause timer
            if (timeline_timer_) {
                timeline_timer_->Pause();
            }
            is_playing_ = false;

            if (is_looping) {
                // Seek to start
                double start_time = static_cast<double>(boundary_start) / fps;
                if (timeline_timer_) {
                    timeline_timer_->Seek(start_time);
                }
                current_frame_ = boundary_start;

                // Seek decoder
                d3d11_decoder_->UpdatePlayhead(boundary_start, SeekQuality::NORMAL, true);

                // Enter buffer wait then resume
                waiting_for_frame_ = true;
                waiting_start_time_ = std::chrono::steady_clock::now();
                is_playing_ = true;

                // Return cached texture while transitioning
                width = current_texture_width_;
                height = current_texture_height_;
                return current_texture_;
            } else {
                // Not looping - stay paused on last frame
                width = current_texture_width_;
                height = current_texture_height_;
                return current_texture_;
            }
        }

        // Get frame from D3D11 decoder as GL texture
        GLuint texture = d3d11_decoder_->GetFrameAsGLTexture(frame);

        if (texture != 0) {
            current_texture_ = texture;
            current_texture_width_ = d3d11_decoder_->GetWidth();
            current_texture_height_ = d3d11_decoder_->GetHeight();

            width = current_texture_width_;
            height = current_texture_height_;
            return texture;
        }

        // Fallback to previous frame on decode failure
        if (current_texture_ != 0) {
            width = current_texture_width_;
            height = current_texture_height_;
            return current_texture_;
        }

        width = 0;
        height = 0;
        return 0;
    }
#endif

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

#ifdef _WIN32
void TimelinePlaybackController::SetD3D11RenderingMode(bool enabled) {
    if (use_d3d11_rendering_ == enabled) return;

    use_d3d11_rendering_ = enabled;

    // Forward to cache
    if (cache_) {
        cache_->SetD3D11RenderingMode(enabled);
    }
    if (right_cache_) {
        right_cache_->SetD3D11RenderingMode(enabled);
    }

    // Clear current textures
    current_texture_ = 0;
    current_srv_d3d_ = nullptr;
}

ID3D11ShaderResourceView* TimelinePlaybackController::UpdateD3D11(int& width, int& height) {
    if (!initialized_ || !use_d3d11_rendering_) {
        width = 0;
        height = 0;
        return nullptr;
    }

    // Sync playhead from virtual timer
    UpdateTimer();

    // If no cache yet, return null
    if (!cache_) {
        width = 0;
        height = 0;
        return nullptr;
    }

    int frame = current_frame_.load();
    bool playing = is_playing_.load();

    // Update cache with current playhead
    cache_->UpdatePlayhead(frame, playing);

    // Try to get frame from cache (D3D11 path)
    int tex_width = 0, tex_height = 0;
    bool got_exact_frame = false;
    ID3D11ShaderResourceView* srv = cache_->GetFrameD3D11(frame, tex_width, tex_height, &got_exact_frame);

    if (srv != nullptr) {
        // Cache hit - store as current frame for future miss handling
        current_srv_d3d_ = srv;
        current_texture_width_ = tex_width;
        current_texture_height_ = tex_height;

        // If we were waiting for post-edit frame, we got it
        if (awaiting_post_edit_frame_) {
            awaiting_post_edit_frame_ = false;
            pending_evict_texture_ = 0;
            pending_evict_width_ = 0;
            pending_evict_height_ = 0;
        }

        width = tex_width;
        height = tex_height;
        return srv;
    }

    // Cache miss - return previous frame to hold
    if (current_srv_d3d_ != nullptr) {
        width = current_texture_width_;
        height = current_texture_height_;
        return current_srv_d3d_;
    }

    width = 0;
    height = 0;
    return nullptr;
}
#endif

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

    // Register sequence metadata for any new image sequence clips
    // This must happen before audio mixer preloads to ensure cache knows about sequences
    if (cache_ && timeline_view_) {
        const auto& tracks = timeline_view_->GetTracks();
        for (const auto& track : tracks) {
            for (const auto& clip : track.clips) {
                if (clip.is_sequence && clip.is_linked && !clip.linked_path.empty()) {
                    SequenceMetadata seq_meta;
                    seq_meta.directory = clip.sequence_directory;
                    seq_meta.pattern = clip.sequence_pattern;
                    seq_meta.start_frame = clip.sequence_start_frame;
                    seq_meta.end_frame = clip.sequence_end_frame;
                    seq_meta.exr_layer = clip.sequence_exr_layer;
                    seq_meta.pipeline_mode = DetectSequencePipelineMode(clip);
                    seq_meta.valid = true;
                    cache_->RegisterSequenceMetadata(clip.linked_path, seq_meta);
                }
            }
        }
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

        // Force refresh to handle trim/slip edits where clip IDs don't change
        // but source_in/out values do
        audio_mixer_->ForceRefresh();
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
    // Also process right cache for dual view mode
    if (right_cache_) {
        right_cache_->ProcessPendingUploads();
    }
}

void TimelinePlaybackController::SetConfig(const TimelinePlaybackConfig& config) {
    config_ = config;

    if (cache_) {
        TimelineCacheConfig cache_config;
        cache_config.readAheadFrames = config_.readAheadFrames;
        cache_config.readBehindFrames = config_.readBehindFrames;
        cache_config.io_threads = config_.io_threads;
        cache_config.fps = fps_;
        cache_config.use_shared_pool = true;
        cache_config.pipeline_mode = config_.pipeline_mode;  // Pass through video pipeline mode
        cache_->SetConfig(cache_config);
    }
}

void TimelinePlaybackController::SetLooping(bool enabled) {
    // Update timer for consistent state
    if (timeline_timer_) {
        timeline_timer_->SetLooping(enabled);
    }
}

bool TimelinePlaybackController::IsLooping() const {
    // Timer is the source of truth for loop state
    if (timeline_timer_) {
        return timeline_timer_->IsLooping();
    }
    return false;
}

//=============================================================================
// Volume/Mute Control - Routes to AudioMixer
//=============================================================================

void TimelinePlaybackController::SetVolume(double volume) {
    // Clamp to valid range
    if (volume < 0.0) volume = 0.0;
    if (volume > 1.0) volume = 1.0;

    volume_ = volume;

    // Route to audio mixer
    if (audio_mixer_) {
        audio_mixer_->SetVolume(static_cast<float>(volume));
    }
}

void TimelinePlaybackController::SetMuted(bool muted) {
    muted_ = muted;

    // Route to audio mixer
    if (audio_mixer_) {
        audio_mixer_->SetMuted(muted);
    }
}

double TimelinePlaybackController::GetVolume() const {
    return volume_;
}

bool TimelinePlaybackController::IsMuted() const {
    return muted_;
}

bool TimelinePlaybackController::IsPlaying() const {
    return is_playing_.load();
}

bool TimelinePlaybackController::IsWaitingForBuffer() const {
    return waiting_for_frame_;
}

bool TimelinePlaybackController::IsActuallyPlaying() const {
    // True only if playing AND not waiting for buffer to fill
    // Used by UI to show correct play/pause state
    return is_playing_.load() && !waiting_for_frame_;
}

//=============================================================================
// Buffer-Wait Control
//=============================================================================

void TimelinePlaybackController::SetBufferWaitEnabled(bool enabled) {
    buffer_wait_enabled_ = enabled;
    Debug::Log("TimelinePlaybackController: Buffer-wait " +
               std::string(enabled ? "enabled" : "disabled"));
}

bool TimelinePlaybackController::ShouldApplyBufferWait() const {
    // Buffer wait only applies to image/EXR content at the current playhead
    // Video uses D3D11 decoders which handle their own buffering
    // This is clip-aware: only waits within image clips, not during video clips

    // Setting must be enabled
    if (!buffer_wait_enabled_) return false;

    // D3D11VA HDR mode - decoder handles buffering
    if (use_d3d11va_hdr_ && d3d11va_decoder_) return false;

    // D3D11 GPU-native mode - decoder handles buffering
    if (use_d3d11_decoder_ && d3d11_decoder_) return false;

#ifdef _WIN32
    // Unified D3D11 composite pipeline - decoders handle buffering
    if (use_unified_composite_ && dual_view_pipeline_) return false;
#endif

    // Must have cache
    if (!cache_) return false;

    // Clip-aware check: only buffer wait if current frame is image/EXR content
    // This allows playlists with mixed video+image content to only buffer wait
    // during image clips, not during video clips
    int current_frame = current_frame_.load();
    if (!cache_->IsFrameImageContent(current_frame)) return false;

    return true;  // Current frame is image/EXR - buffer wait applies
}

void TimelinePlaybackController::SetBufferWaitPercent(int /*percent*/) {
    // No-op: buffer wait percent is now hardcoded to 90%
    // This method is kept for API compatibility but does nothing
}

void TimelinePlaybackController::SetVideoBufferFrames(int frames) {
    video_buffer_frames_ = std::clamp(frames, 2, 48);
    Debug::Log("TimelinePlaybackController: Video buffer frames set to " +
               std::to_string(video_buffer_frames_));
}

int TimelinePlaybackController::GetEffectiveBufferWaitPercent() const {
    return 90;  // Hardcoded to 90% - simplifies interaction with adaptive speed
}

bool TimelinePlaybackController::IsSequentialBufferReady() const {
    if (!cache_) return true;  // No cache = ready (nothing to wait for)

    // Buffer wait only applies to image/EXR content at the current frame
    // Video uses D3D11 decoders which handle their own buffering
    int current_frame = current_frame_.load();
    if (!cache_->IsFrameImageContent(current_frame)) {
        return true;  // Video clip = always ready (D3D11 handles buffering)
    }

#ifdef _WIN32
    // Unified D3D11 composite pipeline: decoders have their own buffering
    // Skip buffer checks - similar to VIDEO_FILE mode with MPV
    if (use_unified_composite_ && dual_view_pipeline_) {
        return true;
    }
#endif

    // Image sequence / mixed timeline: check sequential buffer fill
    // Get priority-sorted frames from engine: [current, ahead1, ahead2, ..., behind1, ...]
    std::vector<int> priority_frames = cache_->GetPriorityFrameWindow();
    if (priority_frames.empty()) return true;

    // Image/EXR mode: use configured % of readahead
    int read_ahead = cache_->GetReadAheadFrames();
    int effective_percent = GetEffectiveBufferWaitPercent();
    int needed = std::max(1, (read_ahead * effective_percent) / 100);

    // The first (1 + needed) frames in priority order are: current, ahead1, ahead2, ...
    // Check if these are ALL ready (sequential from playhead)
    int frames_to_check = std::min(needed + 1, static_cast<int>(priority_frames.size()));

    for (int i = 0; i < frames_to_check; i++) {
        if (!cache_->HasFrameReady(priority_frames[i])) {
            return false;  // Sequential chain broken
        }
    }

    return true;  // All immediate frames ready
}

void TimelinePlaybackController::GetBufferFillStatus(int& filled, int& needed) const {
    filled = 0;
    needed = 1;

    if (!cache_) return;

    // Buffer status only applies to image/EXR content at the current frame
    // For video clips, report 100% full since D3D11 handles buffering
    int current_frame = current_frame_.load();
    if (!cache_->IsFrameImageContent(current_frame)) {
        filled = 1;
        needed = 1;
        return;
    }

#ifdef _WIN32
    // Unified D3D11 composite pipeline: decoders have their own buffering
    if (use_unified_composite_ && dual_view_pipeline_) {
        filled = 1;
        needed = 1;
        return;
    }
#endif

    // Image sequence / mixed timeline: check buffer fill status
    std::vector<int> priority_frames = cache_->GetPriorityFrameWindow();
    if (priority_frames.empty()) return;

    // Image/EXR mode: use configured % of readahead
    int read_ahead = cache_->GetReadAheadFrames();
    int effective_percent = GetEffectiveBufferWaitPercent();
    needed = std::max(1, (read_ahead * effective_percent) / 100);

    int frames_to_check = std::min(needed + 1, static_cast<int>(priority_frames.size()));

    // Count sequential ready frames from start
    for (int i = 0; i < frames_to_check; i++) {
        if (cache_->HasFrameReady(priority_frames[i])) {
            filled++;
        } else {
            break;  // Stop at first gap (sequential check)
        }
    }
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

PipelineMode TimelinePlaybackController::GetCurrentPipelineMode() const {
#ifdef _WIN32
    // For D3D11 decoder modes, get mode directly from decoder (cache is null)
    if (use_d3d11_decoder_ && d3d11_decoder_) {
        return d3d11_decoder_->GetPipelineMode();
    }
    if (use_d3d11va_hdr_ && d3d11va_decoder_) {
        // D3D11VA HDR mode is always ULTRA_HIGH_RES
        return PipelineMode::ULTRA_HIGH_RES;
    }
#endif

    if (!cache_) return config_.pipeline_mode;

    // For VIDEO_FILE and PLAYLIST modes, return the global video pipeline mode (user-selectable)
    // For other modes (IMAGE_SEQUENCE, etc.), return the per-clip pipeline mode (determined by content)
    if (timeline_view_) {
        TimelineSourceMode mode = timeline_view_->GetSourceMode();
        if (mode == TimelineSourceMode::VIDEO_FILE || mode == TimelineSourceMode::PLAYLIST) {
            return cache_->GetPipelineMode();  // User-selected video pipeline mode
        }
    }

    return cache_->GetClipPipelineMode(current_frame_.load());
}

std::string TimelinePlaybackController::GetTimelineName() const {
    if (!timeline_view_) return "";
    return timeline_view_->GetTimelineName();
}

std::string TimelinePlaybackController::GetSourceFilePath() const {
    if (!timeline_view_) return "";
    return timeline_view_->GetSourceFilePath();
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

    // Scan all clips to find:
    // 1. Canvas dimensions (from first linked clip if not provided)
    // 2. Max source width (for resolution-based buffer thresholds)
    max_source_width_ = 0;
    bool found_first_clip = false;

    for (const auto& track : tracks) {
        if (!track.is_video) continue;
        for (const auto& clip : track.clips) {
            if (clip.is_linked && !clip.linked_path.empty() && clip.source_width > 0) {
                // Track max source width across ALL clips
                if (clip.source_width > max_source_width_) {
                    max_source_width_ = clip.source_width;
                }

                // Use first clip for canvas dimensions (if not provided)
                if (!found_first_clip && clip.source_height > 0) {
                    found_first_clip = true;
                    if (canvas_width <= 0 || canvas_height <= 0) {
                        width_ = clip.source_width;
                        height_ = clip.source_height;
                    }
                }
            }
        }
    }

    // Use provided canvas dimensions, or fall back to first clip, or default 1920x1080
    if (canvas_width > 0 && canvas_height > 0) {
        width_ = canvas_width;
        height_ = canvas_height;
    } else if (!found_first_clip) {
        width_ = 1920;
        height_ = 1080;
    }

    Debug::Log("TimelinePlaybackController: Canvas " + std::to_string(width_) + "x" + std::to_string(height_) +
               ", max source width: " + std::to_string(max_source_width_));

    // Set content dimensions in VideoPlayer
    video_player_->SetContentDimensions(width_, height_);

    // Create PlaybackTimer (the virtual timeline clock)
    timeline_timer_ = std::make_unique<PlaybackTimer>();
    timeline_timer_->SetDuration(timeline_duration_);
    timeline_timer_->SetFrameRate(fps_);
    // SetLooping removed - loop control handled by main.cpp boundary system
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

    // Check if this is VIDEO_FILE mode - extract media file path first
    // Then decide which decoder to use based on pipeline mode
    std::string media_file_path;
    bool is_video_file_mode = (timeline_view_->GetSourceMode() == TimelineSourceMode::VIDEO_FILE);

    if (is_video_file_mode) {
        // First try to find a video clip
        for (const auto& track : tracks) {
            if (!track.is_video) continue;
            for (const auto& clip : track.clips) {
                if (clip.is_linked && !clip.linked_path.empty()) {
                    media_file_path = clip.linked_path;
                    break;
                }
            }
            if (!media_file_path.empty()) break;
        }

        // If no video clip found, check audio tracks (audio-only file)
        if (media_file_path.empty()) {
            for (const auto& track : tracks) {
                if (track.is_video) continue;  // Skip video tracks
                for (const auto& clip : track.clips) {
                    if (clip.is_linked && !clip.linked_path.empty()) {
                        media_file_path = clip.linked_path;
                        Debug::Log("TimelinePlaybackController: Found audio-only file: " + media_file_path);
                        break;
                    }
                }
                if (!media_file_path.empty()) break;
            }
        }
    }

#ifdef _WIN32
    // D3D11VA_HDR pipeline mode: use FFmpeg + D3D11VA hardware decode
    // Key: Our D3D11 device is passed TO FFmpeg for texture compatibility
    bool use_d3d11va_hdr = (config_.pipeline_mode == PipelineMode::MF_HDR);

    if (use_d3d11va_hdr && is_video_file_mode && !media_file_path.empty()) {
        Debug::Log("TimelinePlaybackController: D3D11VA HDR mode - using FFmpeg + D3D11VA for: " + media_file_path);

        // Ensure D3D11 device manager is initialized
        auto& device_mgr = D3D11DeviceManager::Instance();
        if (!device_mgr.IsInitialized()) {
            device_mgr.Initialize(nullptr);  // Initialize without HWND - will use default adapter
        }
        auto* device = device_mgr.GetDevice();
        if (device) {
            d3d11va_decoder_ = std::make_unique<D3D11VAVideoDecoder>();
            if (d3d11va_decoder_->Initialize(device, media_file_path)) {
                use_d3d11va_hdr_ = true;

                // Update dimensions from D3D11VA decoder
                if (d3d11va_decoder_->GetWidth() > 0 && d3d11va_decoder_->GetHeight() > 0) {
                    width_ = d3d11va_decoder_->GetWidth();
                    height_ = d3d11va_decoder_->GetHeight();
                    video_player_->SetContentDimensions(width_, height_);
                }

                // Update FPS if detected
                if (d3d11va_decoder_->GetFPS() > 0) {
                    fps_ = d3d11va_decoder_->GetFPS();
                    timeline_timer_->SetFrameRate(fps_);
                }

                // Clear the timer's on_end callback - D3D11VA handles loop/end internally
                timeline_timer_->SetOnEnd(nullptr);

                Debug::Log("TimelinePlaybackController: D3D11VA HDR mode active - " +
                           std::to_string(width_) + "x" + std::to_string(height_) +
                           " @ " + std::to_string(fps_) + " fps" +
                           (d3d11va_decoder_->IsHDRContent() ? " [HDR]" : " [SDR]") +
                           (d3d11va_decoder_->Is10BitOutput() ? " [10-bit]" : " [8-bit]"));

                // Log timecode offset if present
                if (config_.timecode_start_frame > 0) {
                    Debug::Log("TimelinePlaybackController: Timecode offset = " +
                               std::to_string(config_.timecode_start_frame) + " frames");
                }
            } else {
                Debug::Log("TimelinePlaybackController: WARNING - D3D11VA init failed");
                d3d11va_decoder_.reset();
                use_d3d11va_hdr_ = false;
            }
        } else {
            Debug::Log("TimelinePlaybackController: WARNING - No D3D11 device for D3D11VA");
            use_d3d11va_hdr_ = false;
        }
    }

    // D3D11VideoDecoder mode: use FFmpeg + D3D11 for direct GPU textures
    // Always use D3D11 decoder for video files (MPV removed)
    if (!use_d3d11va_hdr_ && is_video_file_mode && !media_file_path.empty()) {
        Debug::Log("TimelinePlaybackController: D3D11 GPU-native mode for: " + media_file_path);

        // Ensure D3D11 device manager is initialized
        auto& device_mgr = D3D11DeviceManager::Instance();
        if (!device_mgr.IsInitialized()) {
            device_mgr.Initialize(nullptr);
        }

        d3d11_decoder_ = std::make_unique<D3D11VideoDecoder>();
        d3d11_decoder_->SetVideoPath(media_file_path);
        d3d11_decoder_->SetPipelineMode(config_.pipeline_mode);

        if (d3d11_decoder_->Initialize()) {
            use_d3d11_decoder_ = true;

            // Update dimensions from decoder
            if (d3d11_decoder_->GetWidth() > 0 && d3d11_decoder_->GetHeight() > 0) {
                width_ = d3d11_decoder_->GetWidth();
                height_ = d3d11_decoder_->GetHeight();
                video_player_->SetContentDimensions(width_, height_);
            }

            // Update FPS
            if (d3d11_decoder_->GetFPS() > 0) {
                fps_ = d3d11_decoder_->GetFPS();
                timeline_timer_->SetFrameRate(fps_);
            }

            // Update duration
            if (d3d11_decoder_->GetDuration() > 0) {
                timeline_duration_ = d3d11_decoder_->GetDuration();
                timeline_timer_->SetDuration(timeline_duration_);
            }

            Debug::Log("TimelinePlaybackController: D3D11 GPU-native mode active - " +
                       std::to_string(width_) + "x" + std::to_string(height_) +
                       " @ " + std::to_string(fps_) + " fps" +
                       (d3d11_decoder_->IsHDRContent() ? " [HDR]" : " [SDR]") +
                       (d3d11_decoder_->Is10BitOutput() ? " [10-bit]" : " [8-bit]") +
                       " [" + std::string(d3d11_decoder_->IsHardwareAccelerated() ? "HW" : "SW") + " decode]");
        } else {
            Debug::Log("TimelinePlaybackController: WARNING - D3D11VideoDecoder init failed");
            d3d11_decoder_.reset();
            use_d3d11_decoder_ = false;
        }
    }
#endif

    // Initialize cache (skip for D3D11 decoder modes - no decoder needed)
#ifdef _WIN32
    bool skip_cache = use_d3d11va_hdr_ || use_d3d11_decoder_;
#else
    bool skip_cache = false;
#endif
    if (!skip_cache) {
        // For PLAYLIST and DUAL_VIEW modes, ensure D3D11DeviceManager is initialized
        // BEFORE creating the TimelineCache. The cache's background thread will try
        // to create D3D11VideoDecoder for VIDEO clips, which requires the device.
#ifdef _WIN32
        TimelineSourceMode source_mode = timeline_view_->GetSourceMode();
        if (source_mode == TimelineSourceMode::PLAYLIST ||
            source_mode == TimelineSourceMode::DUAL_VIEW) {
            auto& device_mgr = D3D11DeviceManager::Instance();
            if (!device_mgr.IsInitialized()) {
                Debug::Log("TimelinePlaybackController: Pre-initializing D3D11DeviceManager for " +
                           std::string(source_mode == TimelineSourceMode::PLAYLIST ? "PLAYLIST" : "DUAL_VIEW") + " mode");
                device_mgr.Initialize(nullptr);
            }
        }
#endif
        cache_ = std::make_unique<TimelineCache>();

        TimelineCacheConfig cache_config;
        cache_config.readAheadFrames = config_.readAheadFrames;
        cache_config.readBehindFrames = config_.readBehindFrames;
        cache_config.io_threads = config_.io_threads;
        cache_config.max_textures = g_timeline_max_textures;
        cache_config.fps = fps_;
        cache_config.use_shared_pool = true;
        cache_config.pipeline_mode = config_.pipeline_mode;

        cache_->SetConfig(cache_config);
        cache_->Initialize(tracks, &timeline_view_->GetFlattener(), fps_,
                           timeline_view_->GetSourceMode());
    }

    // Register sequence metadata for any image sequence clips (only if cache exists)
    // This must happen after cache is created but before it starts loading frames
    if (cache_) {
        for (const auto& track : tracks) {
            for (const auto& clip : track.clips) {
                if (clip.is_sequence && clip.is_linked && !clip.linked_path.empty()) {
                    SequenceMetadata seq_meta;
                    seq_meta.directory = clip.sequence_directory;
                    seq_meta.pattern = clip.sequence_pattern;
                    seq_meta.start_frame = clip.sequence_start_frame;
                    seq_meta.end_frame = clip.sequence_end_frame;
                    seq_meta.exr_layer = clip.sequence_exr_layer;
                    seq_meta.pipeline_mode = DetectSequencePipelineMode(clip);
                    seq_meta.valid = true;
                    cache_->RegisterSequenceMetadata(clip.linked_path, seq_meta);
                    Debug::Log("TimelinePlaybackController: Registered sequence metadata for " + clip.linked_path);
                }
            }
        }

        if (width_ > 0 && height_ > 0) {
            cache_->SetGapTextureDimensions(width_, height_);
            cache_->SetCanvasDimensions(width_, height_);  // Ensure consistent output dimensions
        }
    }

    // Initialize audio mixer (always needed now - MPV removed)
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
    // SetLooping removed - loop control handled by main.cpp boundary system
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

    // Check for pending FPS update when paused (safe to update without mid-playback glitches)
    if (!is_playing_.load() && !fps_update_applied_) {
        ApplyPendingFPSUpdate();
    }

    // NOTE: ProcessPendingUploads now called from ProcessPendingTextureUploads()
    // BEFORE ImGui::NewFrame() to avoid GL state corruption during render

    // Wait-for-frame logic: when Play() is called, we don't start the timer
    // until the sequential buffer is ready. This prevents stuttering,
    // especially when crossing edit boundaries where multiple clips need to be buffered.
    // Buffer wait only applies to image/EXR content - video uses D3D11 decoders with internal buffering.
    if (waiting_for_frame_ && is_playing_.load()) {
        // Safety check: if buffer wait no longer applies (setting changed, content type changed),
        // exit wait state immediately and start playback
        if (!ShouldApplyBufferWait()) {
            waiting_for_frame_ = false;
            accumulated_time_ = 0.0;
            last_timer_update_ = now;
            timeline_timer_->Play();
            if (audio_mixer_) {
                audio_mixer_->Play();
            }
            return;
        }

#ifdef _WIN32
        // D3D11VA HDR mode: seek (if needed for loop restart) and try to decode
        // Note: This path is only reached if ShouldApplyBufferWait() returned true,
        // which shouldn't happen for D3D11VA HDR mode, but keep as safety fallback.
        if (use_d3d11va_hdr_ && d3d11va_decoder_) {
            int target_frame = current_frame_.load();

            // Seek decoder if this is a loop restart (from end -> start)
            // SeekToFrame is safe when paused - no background read-ahead
            if (d3d11va_needs_seek_) {
                Debug::Log("D3D11VA_HDR BUFFER_WAIT: Seeking decoder to frame " + std::to_string(target_frame));
                d3d11va_decoder_->SeekToFrame(target_frame);
                d3d11va_needs_seek_ = false;  // Only seek once
            }

            // Now try to decode the frame
            GLuint texture = d3d11va_decoder_->GetFrameAsGLTexture(target_frame);

            if (texture != 0) {
                // Frame ready - resume playback
                Debug::Log("D3D11VA_HDR BUFFER_WAIT: Frame " + std::to_string(target_frame) + " ready, resuming playback");
                current_texture_ = texture;
                current_texture_width_ = d3d11va_decoder_->GetWidth();
                current_texture_height_ = d3d11va_decoder_->GetHeight();

                waiting_for_frame_ = false;
                accumulated_time_ = 0.0;
                last_timer_update_ = now;
                timeline_timer_->Play();
            }
            // If not ready, keep waiting (timer stays paused)
            return;
        }
#endif

        // Use engine-aware sequential buffer check
        // Needs 50% of readahead filled SEQUENTIALLY from playhead
        bool buffer_ready = IsSequentialBufferReady();

        // Check for timeout - don't wait forever if cache can't fill
        auto wait_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - waiting_start_time_).count();
        bool timed_out = wait_elapsed > kMaxWaitMs;

        // Also check if at least current frame is ready (minimum requirement)
        bool current_ready = cache_ && cache_->HasFrameReady(current_frame_.load());

        if (buffer_ready || (timed_out && current_ready)) {
            // Sequential buffer ready OR timed out with at least current frame
            waiting_for_frame_ = false;

            // Clear DirectEXRCache buffer wait flag (IMAGE_SEQUENCE mode)
            if (cache_ && cache_->IsDirectEXRCacheMode()) {
                cache_->ClearBufferWait();
            }

            // DEBUG: Log the exact state when transitioning from wait to play
            int frame_at_start = current_frame_.load();
            double timer_pos_at_start = timeline_timer_->GetPosition();
            Debug::Log("TimelinePlaybackController: [BUFFER WAIT -> PLAY] frame=" +
                       std::to_string(frame_at_start) +
                       " timer_pos=" + std::to_string(timer_pos_at_start) + "s" +
                       " wait_elapsed=" + std::to_string(wait_elapsed) + "ms" +
                       " buffer_ready=" + std::to_string(buffer_ready));

            // Reset debug counter to log first N updates after play starts
            debug_post_play_updates_ = 0;

            if (timed_out && !buffer_ready) {
                int filled, needed;
                GetBufferFillStatus(filled, needed);
                Debug::Log("TimelinePlaybackController: Buffer wait timed out after " +
                           std::to_string(wait_elapsed) + "ms, starting with " +
                           std::to_string(filled) + "/" + std::to_string(needed) + " sequential frames");
            }

            // Reset timing accumulator to start fresh
            accumulated_time_ = 0.0;
            last_timer_update_ = now;

            // Start timer and audio
            timeline_timer_->Play();
            if (audio_mixer_) {
                audio_mixer_->Play();
                audio_paused_for_throttle_ = false;  // Clear throttle pause flag
            }
        }
        // If not ready yet, just keep waiting (don't advance timer)
        return;
    }

    // Adaptive throttle: check buffer health and adjust playback speed
    // Only runs during actual playback (not buffer-wait or paused)
    if (is_playing_.load() && throttle_enabled_) {
        // For IMAGE_SEQUENCE with DirectEXRCache: use its speed factor directly
        // DirectEXRCache has proven rate-based adaptive speed control
        if (cache_ && cache_->IsDirectEXRCacheMode()) {
            // Check if DirectEXRCache is triggering buffer wait (< 6 frames ahead)
            if (cache_->NeedsBufferWait() && !waiting_for_frame_) {
                // Buffer critically low - pause playback until buffer refills
                waiting_for_frame_ = true;
                waiting_start_time_ = std::chrono::steady_clock::now();
                throttle_state_ = ThrottleState::BUFFER_PAUSE;
                if (timeline_timer_) {
                    timeline_timer_->Pause();
                }
                if (audio_mixer_ && !audio_paused_for_throttle_) {
                    audio_mixer_->Pause();
                    audio_paused_for_throttle_ = true;
                }
                Debug::Log("TimelinePlaybackController: DirectEXRCache triggered buffer wait");
                return;  // Wait for buffer to refill
            }

            double speed = cache_->GetPlaybackSpeedFactor();
            if (timeline_timer_) {
                timeline_timer_->SetPlaybackSpeed(speed);
            }
            current_speed_factor_ = speed;

            // Wire to audio: use pitch-preserving time stretch instead of pausing
            // SoundTouch can handle 0.5x-2.0x range; below 0.5x pause audio
            if (audio_mixer_) {
                if (speed >= 0.5) {
                    // Use time stretch for tempo control (pitch preserved)
                    audio_mixer_->SetPlaybackTempo(speed);
                    if (audio_paused_for_throttle_) {
                        audio_mixer_->Play();
                        audio_paused_for_throttle_ = false;
                    }
                } else {
                    // Below 0.5x is too extreme for time stretch - pause audio
                    if (!audio_paused_for_throttle_) {
                        audio_mixer_->Pause();
                        audio_paused_for_throttle_ = true;
                    }
                }
            }

            // Map speed to throttle state for UI display
            if (speed >= 1.0) {
                throttle_state_ = ThrottleState::FULL;
            } else if (speed >= 0.75) {
                throttle_state_ = ThrottleState::SLIGHT;
            } else if (speed >= 0.5) {
                throttle_state_ = ThrottleState::MODERATE;
            } else if (speed >= 0.33) {
                throttle_state_ = ThrottleState::SIGNIFICANT;
            } else if (speed >= 0.25) {
                throttle_state_ = ThrottleState::HEAVY;
            } else {
                throttle_state_ = ThrottleState::SEVERE;
            }

        } else {
            // Normal path: use UpdateThrottleState for other modes
            UpdateThrottleState();
        }
    }

    // Simple wall-clock based timing: let PlaybackTimer handle elapsed time directly
    // Timer always advances at real-time speed (or throttled speed) - if frames aren't ready, we hold/skip
    // This prevents video from playing slower than audio
    if (timeline_timer_->IsPlaying()) {
        timeline_timer_->Update();  // Advances position based on wall clock
    }

    // Update current_frame_ from timer position
    // Use rounding (+ 0.5) to match decoder's PTS-based frame numbering
    int old_frame = current_frame_.load();
    double timer_pos = timeline_timer_->GetPosition();
    current_frame_ = static_cast<int>(timer_pos * fps_ + 0.5);
    int new_frame = current_frame_.load();

    // DEBUG: Log first N updates after play starts to diagnose frame jumps
    if (debug_post_play_updates_ < kDebugPostPlayLogCount && is_playing_.load()) {
        debug_post_play_updates_++;
        Debug::Log("TimelinePlaybackController: [POST-PLAY UPDATE " +
                   std::to_string(debug_post_play_updates_) + "/" +
                   std::to_string(kDebugPostPlayLogCount) + "] old=" +
                   std::to_string(old_frame) + " new=" + std::to_string(new_frame) +
                   " timer_pos=" + std::to_string(timer_pos) + "s");
    }

    if (new_frame - old_frame > 5) {
        Debug::Log("TimelinePlaybackController: [FRAME JUMP] " + std::to_string(old_frame) +
                   " -> " + std::to_string(new_frame) + " (+" + std::to_string(new_frame - old_frame) +
                   " frames, timer_pos=" + std::to_string(timer_pos) + "s)");
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
    // Fallback: calculate from frame
    return static_cast<double>(current_frame_.load()) / fps_;
}

void TimelinePlaybackController::ApplyPendingFPSUpdate() {
    if (!cache_ || fps_update_applied_) return;

    if (cache_->HasPendingFPSUpdate()) {
        double new_fps = cache_->GetDetectedFPS();

        Debug::Log("TimelinePlaybackController: Applying FPS update from " +
                   std::to_string(fps_) + " to " + std::to_string(new_fps));

        fps_ = new_fps;

        // Update PlaybackTimer
        if (timeline_timer_) {
            timeline_timer_->SetFrameRate(new_fps);
        }

        // Update cache (clears cache and recalculates frame counts)
        cache_->SetFPS(new_fps);

        // Update timeline view (for UI display)
        if (timeline_view_) {
            timeline_view_->SetFrameRate(new_fps);
        }
    }

    fps_update_applied_ = true;
}

void TimelinePlaybackController::Play() {
    // Notify thumbnail cache to reduce workers during playback
    if (thumbnail_cache_) {
        thumbnail_cache_->NotifyPlaybackState(true);
    }

    if (use_virtual_timeline_ && timeline_timer_) {
        is_playing_ = true;

        // Buffer wait only applies to image/EXR content
        // Video and D3D11 pipelines handle their own buffering
        if (ShouldApplyBufferWait()) {
            // Enter wait-for-frame state - don't actually start timer/audio until
            // the cache has the current frame ready. This prevents stuttering at start.
            waiting_for_frame_ = true;
            waiting_start_time_ = std::chrono::steady_clock::now();

            // Notify cache that we're playing so decoder starts buffering
            if (cache_) {
                cache_->UpdatePlayhead(current_frame_.load(), true);
            }
            // Timer and audio will be started in UpdateTimer() once frame is ready
        } else {
            // No buffer wait - start playback immediately
            waiting_for_frame_ = false;
            accumulated_time_ = 0.0;
            last_timer_update_ = std::chrono::steady_clock::now();
            timer_initialized_ = true;
            timeline_timer_->Play();
            if (audio_mixer_) {
                audio_mixer_->Play();
            }
            if (cache_) {
                cache_->UpdatePlayhead(current_frame_.load(), true);
            }
        }
    }
}

void TimelinePlaybackController::ForcePlay() {
    if (use_virtual_timeline_ && timeline_timer_) {
        // Skip buffer wait and start immediately
        is_playing_ = true;
        waiting_for_frame_ = false;

        // Set force-play cooldown to prevent immediate BUFFER_PAUSE re-trigger
        force_play_active_ = true;
        force_play_time_ = std::chrono::steady_clock::now();

        // Reset timing accumulator
        accumulated_time_ = 0.0;
        last_timer_update_ = std::chrono::steady_clock::now();
        timer_initialized_ = true;

        // Start timer and audio immediately
        timeline_timer_->Play();
        if (audio_mixer_) {
            audio_mixer_->Play();
        }

        // Notify cache that we're playing
        if (cache_) {
            cache_->UpdatePlayhead(current_frame_.load(), true);
        }

        Debug::Log("TimelinePlaybackController: ForcePlay - skipped buffer wait, cooldown active for " +
                   std::to_string(kForcePlayCooldownMs) + "ms");
    }
}

void TimelinePlaybackController::Pause() {
    // Notify thumbnail cache to resume all workers
    if (thumbnail_cache_) {
        thumbnail_cache_->NotifyPlaybackState(false);
    }

#ifdef _WIN32
    // D3D11VA HDR mode: pause timer and clear pending states
    if (use_d3d11va_hdr_ && d3d11va_decoder_ && timeline_timer_) {
        timeline_timer_->Pause();
        is_playing_ = false;
        waiting_for_frame_ = false;
        d3d11va_needs_seek_ = false;  // Cancel any pending loop seek
        if (audio_mixer_) {
            audio_mixer_->Pause();
        }
        return;
    }

    // D3D11 GPU-native mode: pause timer, audio, and clear pending states
    if (use_d3d11_decoder_ && d3d11_decoder_ && timeline_timer_) {
        timeline_timer_->Pause();
        is_playing_ = false;
        waiting_for_frame_ = false;
        if (audio_mixer_) {
            audio_mixer_->Pause();
        }
        return;
    }
#endif

    if (use_virtual_timeline_ && timeline_timer_) {
        timeline_timer_->Pause();
        is_playing_ = false;
        waiting_for_frame_ = false;  // Cancel waiting if paused
        d3d11va_needs_seek_ = false;  // Reset seek flag
        force_play_active_ = false;  // Clear ForcePlay cooldown on pause
        ResetThrottle();  // Reset to full speed on pause
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

void TimelinePlaybackController::TriggerLoopBufferWait() {
    // Called when playback loops back to boundary start
    // Pauses the timer briefly to let cache fill the loop-start frames
    // Buffer wait only applies to image/EXR content
    if (!is_playing_.load() || !timeline_timer_) return;

    // Buffer wait only applies to image/EXR content - skip for video
    if (!ShouldApplyBufferWait()) {
        // Just notify cache and continue without pausing
        if (cache_) {
            cache_->UpdatePlayhead(current_frame_.load(), true);
        }
        if (dual_view_mode_ && right_cache_) {
            right_cache_->UpdatePlayhead(current_frame_.load(), true);
        }
        return;
    }

    // Pause timer and audio while we wait
    timeline_timer_->Pause();
    if (audio_mixer_) {
        audio_mixer_->Pause();
    }

    // Enter buffer-wait state (UpdateTimer will resume when ready)
    waiting_for_frame_ = true;
    waiting_start_time_ = std::chrono::steady_clock::now();

    // Notify cache of new position so it prioritizes these frames
    if (cache_) {
        cache_->UpdatePlayhead(current_frame_.load(), true);
    }
    if (dual_view_mode_ && right_cache_) {
        right_cache_->UpdatePlayhead(current_frame_.load(), true);
    }

    Debug::Log("TimelinePlaybackController: Loop buffer-wait triggered at frame " +
               std::to_string(current_frame_.load()));
}

void TimelinePlaybackController::Seek(double position) {
#ifdef _WIN32
    // D3D11VA HDR mode: seek decoder and timer directly (no buffer wait)
    if (use_d3d11va_hdr_ && d3d11va_decoder_ && timeline_timer_) {
        // Clamp position to valid range
        if (position < 0) position = 0;

        // Calculate safe end boundary using probed actual_last_frame
        int total_frames = static_cast<int>(timeline_duration_ * fps_ + 0.5);

        // Account for embedded timecode offset
        int timecode_offset = config_.timecode_start_frame;
        if (timecode_offset > 0) {
            total_frames -= timecode_offset;
            if (total_frames < 1) total_frames = 1;
        }

        int actual_last = d3d11va_decoder_->GetActualLastFrame();
        int safe_last_frame;
        if (actual_last >= 0) {
            safe_last_frame = actual_last - kD3D11VASafetyMarginFrames;
        } else {
            safe_last_frame = total_frames - kD3D11VASafetyMarginFrames - 1;
        }
        if (safe_last_frame < 0) safe_last_frame = 0;
        double safe_end_position = static_cast<double>(safe_last_frame) / fps_;

        if (position > safe_end_position) position = safe_end_position;

        // Seek timer
        timeline_timer_->Seek(position);

        // Seek D3D11VA decoder
        int target_frame = static_cast<int>(position * fps_ + 0.5);
        d3d11va_decoder_->SeekToFrame(target_frame);
        current_frame_ = target_frame;

        // Reset timing
        accumulated_time_ = 0.0;
        last_timer_update_ = std::chrono::steady_clock::now();
        return;
    }

    // D3D11 GPU-native mode: seek decoder and timer
    if (use_d3d11_decoder_ && d3d11_decoder_ && timeline_timer_) {
        // Clamp position to valid range
        if (position < 0) position = 0;
        if (position > timeline_duration_) position = timeline_duration_;

        // Seek timer
        timeline_timer_->Seek(position);

        // Seek decoder
        int target_frame = static_cast<int>(position * fps_ + 0.5);
        d3d11_decoder_->UpdatePlayhead(target_frame, SeekQuality::NORMAL, true);
        current_frame_ = target_frame;

        // Sync audio
        if (audio_mixer_) {
            audio_mixer_->SetTimelinePosition(position);
        }

        // Reset timing
        accumulated_time_ = 0.0;
        last_timer_update_ = std::chrono::steady_clock::now();
        return;
    }
#endif

    if (use_virtual_timeline_ && timeline_timer_) {
        // Clamp position
        if (position < 0) position = 0;
        if (position > timeline_duration_) position = timeline_duration_;

        // Check if we're currently playing (need to enter buffer wait after seek)
        bool was_playing = is_playing_.load() && !waiting_for_frame_;

        timeline_timer_->Seek(position);

        // Reset timing accumulator to prevent accumulated time from causing jumps
        accumulated_time_ = 0.0;
        last_timer_update_ = std::chrono::steady_clock::now();

        // Reset throttle on seek - give cache a fresh start
        ResetThrottle();

        // Seek audio to new position
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
        // Also update right cache in dual view mode
        if (dual_view_mode_ && right_cache_) {
            right_cache_->UpdatePlayhead(target_frame, timeline_timer_->IsPlaying());
        }

        // If playing, enter buffer wait to ensure frames are ready at new position
        // Buffer wait only applies to image/EXR content - video uses D3D11 decoders
        if (was_playing && ShouldApplyBufferWait()) {
            waiting_for_frame_ = true;
            waiting_start_time_ = std::chrono::steady_clock::now();

            // Pause timer/audio until buffer ready (UpdateTimer will resume)
            timeline_timer_->Pause();
            if (audio_mixer_) {
                audio_mixer_->Pause();
            }

            Debug::Log("TimelinePlaybackController: Seek while playing - entering buffer wait at frame " +
                       std::to_string(target_frame));
        }
    }
}

void TimelinePlaybackController::SeekRelative(double delta) {
#ifdef _WIN32
    // D3D11VA HDR mode: use timer position and route through protected Seek()
    if (use_d3d11va_hdr_ && d3d11va_decoder_ && timeline_timer_) {
        double new_pos = timeline_timer_->GetPosition() + delta;
        Seek(new_pos);
        return;
    }
#endif

    if (use_virtual_timeline_ && timeline_timer_) {
        double new_pos = timeline_timer_->GetPosition() + delta;
        Seek(new_pos);
    }
}

void TimelinePlaybackController::StepForward(int frames) {
#ifdef _WIN32
    // D3D11VA HDR mode: step with boundary clamping
    if (use_d3d11va_hdr_ && d3d11va_decoder_ && timeline_timer_) {
        // Calculate safe boundary using probed actual_last_frame
        int total_frames = static_cast<int>(timeline_duration_ * fps_ + 0.5);

        // Account for embedded timecode offset
        int timecode_offset = config_.timecode_start_frame;
        if (timecode_offset > 0) {
            total_frames -= timecode_offset;
            if (total_frames < 1) total_frames = 1;
        }

        int actual_last = d3d11va_decoder_->GetActualLastFrame();
        int safe_last_frame;
        if (actual_last >= 0) {
            safe_last_frame = actual_last - kD3D11VASafetyMarginFrames;
        } else {
            safe_last_frame = total_frames - kD3D11VASafetyMarginFrames - 1;
        }
        if (safe_last_frame < 0) safe_last_frame = 0;

        int current = current_frame_.load();
        int target = current + frames;
        if (target > safe_last_frame) target = safe_last_frame;

        // Seek to target frame
        double position = static_cast<double>(target) / fps_;
        Seek(position);
        return;
    }

    // D3D11 GPU-native mode: step with boundary clamping
    if (use_d3d11_decoder_ && d3d11_decoder_ && timeline_timer_) {
        int total_frames = static_cast<int>(timeline_duration_ * fps_ + 0.5);
        int safe_last_frame = total_frames - 1;
        if (safe_last_frame < 0) safe_last_frame = 0;

        int current = current_frame_.load();
        int target = current + frames;
        if (target > safe_last_frame) target = safe_last_frame;

        // Seek to target frame
        double position = static_cast<double>(target) / fps_;
        Seek(position);
        return;
    }
#endif

    if (use_virtual_timeline_ && timeline_timer_) {
        timeline_timer_->StepForward(frames);
    }
}

void TimelinePlaybackController::StepBackward(int frames) {
#ifdef _WIN32
    // D3D11VA HDR mode: step with boundary clamping
    if (use_d3d11va_hdr_ && d3d11va_decoder_ && timeline_timer_) {
        int current = current_frame_.load();
        int target = current - frames;
        if (target < 0) target = 0;

        // Seek to target frame
        double position = static_cast<double>(target) / fps_;
        Seek(position);
        return;
    }

    // D3D11 GPU-native mode: step with boundary clamping
    if (use_d3d11_decoder_ && d3d11_decoder_ && timeline_timer_) {
        int current = current_frame_.load();
        int target = current - frames;
        if (target < 0) target = 0;

        // Seek to target frame
        double position = static_cast<double>(target) / fps_;
        Seek(position);
        return;
    }
#endif

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

//=============================================================================
// Fast Seek Implementation
//=============================================================================

void TimelinePlaybackController::StartRewind() {
    if (!initialized_) return;

    // Pause normal playback if active
    if (is_playing_.load()) {
        Pause();
    }

    is_fast_seeking_ = true;
    fast_forward_ = false;
    fast_seek_speed_ = kFastSeekInitialSpeed;
    fast_seek_start_time_ = std::chrono::steady_clock::now();
    last_fast_seek_update_ = fast_seek_start_time_;

    // Route to appropriate scrub mode based on timeline source mode
    if (cache_) {
        TimelineSourceMode mode = timeline_view_ ? timeline_view_->GetSourceMode()
                                                  : TimelineSourceMode::VIDEO_FILE;

        if (mode == TimelineSourceMode::DUAL_VIEW) {
            // Use aggressive scrub mode for dual view
            cache_->SetAggressiveScrubMode(true);
            if (right_cache_) {
                right_cache_->SetAggressiveScrubMode(true);
            }
        } else {
            // Use normal shuttle mode for VIDEO_FILE/IMAGE_SEQUENCE
            cache_->SetShuttleMode(true, -1);  // -1 = backward
        }
    }

    Debug::Log("TimelinePlaybackController: Started rewind at " + std::to_string(fast_seek_speed_) + "x");
}

void TimelinePlaybackController::StartFastForward() {
    if (!initialized_) return;

    // Pause normal playback if active
    if (is_playing_.load()) {
        Pause();
    }

    is_fast_seeking_ = true;
    fast_forward_ = true;
    fast_seek_speed_ = kFastSeekInitialSpeed;
    fast_seek_start_time_ = std::chrono::steady_clock::now();
    last_fast_seek_update_ = fast_seek_start_time_;

    // Route to appropriate scrub mode based on timeline source mode
    if (cache_) {
        TimelineSourceMode mode = timeline_view_ ? timeline_view_->GetSourceMode()
                                                  : TimelineSourceMode::VIDEO_FILE;

        if (mode == TimelineSourceMode::DUAL_VIEW) {
            // Use aggressive scrub mode for dual view
            cache_->SetAggressiveScrubMode(true);
            if (right_cache_) {
                right_cache_->SetAggressiveScrubMode(true);
            }
        } else {
            // Use normal shuttle mode for VIDEO_FILE/IMAGE_SEQUENCE
            cache_->SetShuttleMode(true, 1);  // 1 = forward
        }
    }

    Debug::Log("TimelinePlaybackController: Started fast forward at " + std::to_string(fast_seek_speed_) + "x");
}

void TimelinePlaybackController::StopFastSeek() {
    if (!is_fast_seeking_) return;

    is_fast_seeking_ = false;
    fast_seek_speed_ = 1.0;

    // Disable scrub/shuttle mode based on timeline source mode
    if (cache_) {
        TimelineSourceMode mode = timeline_view_ ? timeline_view_->GetSourceMode()
                                                  : TimelineSourceMode::VIDEO_FILE;

        if (mode == TimelineSourceMode::DUAL_VIEW) {
            // Disable aggressive scrub mode for dual view
            cache_->SetAggressiveScrubMode(false);
            if (right_cache_) {
                right_cache_->SetAggressiveScrubMode(false);
            }
        } else {
            // Disable normal shuttle mode for VIDEO_FILE/IMAGE_SEQUENCE
            cache_->SetShuttleMode(false, 0);
        }
    }

    Debug::Log("TimelinePlaybackController: Stopped fast seek");
}

void TimelinePlaybackController::SetScrubMode(bool enabled) {
    if (is_scrubbing_ == enabled) return;

    is_scrubbing_ = enabled;

    // Route to appropriate scrub mode based on timeline source mode
    if (cache_) {
        TimelineSourceMode mode = timeline_view_ ? timeline_view_->GetSourceMode()
                                                  : TimelineSourceMode::VIDEO_FILE;

        if (mode == TimelineSourceMode::DUAL_VIEW || mode == TimelineSourceMode::PLAYLIST) {
            // Use aggressive scrub mode for dual view and playlist
            cache_->SetAggressiveScrubMode(enabled);
            if (right_cache_) {
                right_cache_->SetAggressiveScrubMode(enabled);
            }
        } else {
            // Use normal shuttle mode for VIDEO_FILE/IMAGE_SEQUENCE
            cache_->SetShuttleMode(enabled, 0);  // 0 = no direction preference during scrub
        }
    }

    Debug::Log("TimelinePlaybackController: Scrub mode " + std::string(enabled ? "ENABLED" : "DISABLED"));
}

void TimelinePlaybackController::UpdateFastSeek() {
    if (!initialized_ || !is_fast_seeking_) return;

    auto now = std::chrono::steady_clock::now();

    // Calculate time elapsed since last update
    double delta_seconds = std::chrono::duration<double>(now - last_fast_seek_update_).count();
    last_fast_seek_update_ = now;

    // Calculate time elapsed since fast seek started (for speed ramping)
    double elapsed_since_start = std::chrono::duration<double>(now - fast_seek_start_time_).count();

    // Accelerate speed over time: speed = initial * 2^(elapsed_time)
    // This doubles the speed every second
    fast_seek_speed_ = kFastSeekInitialSpeed * std::pow(kFastSeekAcceleration, elapsed_since_start);
    if (fast_seek_speed_ > kFastSeekMaxSpeed) {
        fast_seek_speed_ = kFastSeekMaxSpeed;
    }

#ifdef _WIN32
    // D3D11VA HDR mode: use protected Seek() which handles safe boundaries
    if (use_d3d11va_hdr_ && d3d11va_decoder_) {
        // Calculate position delta based on current speed
        double position_delta = delta_seconds * fast_seek_speed_;
        if (!fast_forward_) {
            position_delta = -position_delta;  // Rewind
        }

        double current_pos = GetPosition();
        double new_pos = current_pos + position_delta;

        // Seek() will handle safe boundary clamping for D3D11VA HDR
        Seek(new_pos);
        return;
    }
#endif

    // Calculate position delta based on current speed
    double position_delta = delta_seconds * fast_seek_speed_;
    if (!fast_forward_) {
        position_delta = -position_delta;  // Rewind
    }

    // Get current position and apply delta
    double current_pos = GetPosition();
    double new_pos = current_pos + position_delta;

    // Clamp to valid range
    if (new_pos < 0.0) {
        new_pos = 0.0;
        // Optionally stop at boundary
    }
    if (new_pos > timeline_duration_) {
        new_pos = timeline_duration_;
        // Optionally stop at boundary
    }

    // Seek to new position
    if (timeline_timer_) {
        timeline_timer_->Seek(new_pos);

        // Update frame immediately
        current_frame_ = static_cast<int>(new_pos * fps_ + 0.5);

        // Update cache playhead (not playing, just seeking)
        if (cache_) {
            cache_->UpdatePlayhead(current_frame_.load(), false);
        }
    }
}

//=============================================================================
// Adaptive Throttle Implementation
//=============================================================================

void TimelinePlaybackController::SetThrottleEnabled(bool enabled) {
    throttle_enabled_ = enabled;
    if (!enabled) {
        // When disabled, reset to full speed
        ResetThrottle();
    }
}

void TimelinePlaybackController::ResetThrottle() {
    throttle_state_ = ThrottleState::FULL;
    current_speed_factor_ = 1.0;
    was_healthy_ = true;
    if (timeline_timer_) {
        timeline_timer_->SetPlaybackSpeed(1.0);
    }
    // Reset audio tempo back to normal (1.0x)
    if (audio_mixer_) {
        audio_mixer_->SetPlaybackTempo(1.0);
    }
    // Clear audio throttle state (audio will be resumed by Play() call if needed)
    audio_paused_for_throttle_ = false;

    // Reset DirectEXRCache adaptive speed (IMAGE_SEQUENCE mode)
    if (cache_) {
        cache_->ResetPlaybackSpeed();
    }
}

double TimelinePlaybackController::GetSpeedForState(ThrottleState state) {
    switch (state) {
        case ThrottleState::FULL:        return 1.0;
        case ThrottleState::SLIGHT:      return 0.75;
        case ThrottleState::MODERATE:    return 0.5;
        case ThrottleState::SIGNIFICANT: return 0.33;
        case ThrottleState::HEAVY:       return 0.25;
        case ThrottleState::SEVERE:      return 0.125;
        case ThrottleState::CRAWL:       return 0.17;  // ~4fps at 24fps base
        case ThrottleState::BUFFER_PAUSE: return 0.0;  // Triggers pause
    }
    return 1.0;
}

void TimelinePlaybackController::UpdateThrottleState() {
    if (!cache_ || !timeline_timer_) return;

    // Adaptive throttle only applies to image/EXR content at the current frame
    // Video uses D3D11 decoders which handle their own buffering
    // Use ShouldApplyBufferWait() which is clip-aware for playlists
    if (!ShouldApplyBufferWait()) {
        if (throttle_state_ != ThrottleState::FULL) {
            throttle_state_ = ThrottleState::FULL;
            current_speed_factor_ = 1.0;
            timeline_timer_->SetPlaybackSpeed(1.0);
        }
        return;
    }

#ifdef _WIN32
    // Unified D3D11 composite pipeline: decoders handle their own buffering
    // No throttling needed - similar to VIDEO_FILE mode with MPV
    if (use_unified_composite_ && dual_view_pipeline_) {
        if (throttle_state_ != ThrottleState::FULL) {
            throttle_state_ = ThrottleState::FULL;
            current_speed_factor_ = 1.0;
            timeline_timer_->SetPlaybackSpeed(1.0);
        }
        return;
    }
#endif

    // Use engine-aware priority frames for throttle calculation
    // These are circle-aware - no boundary hacks needed
    std::vector<int> priority_frames = cache_->GetPriorityFrameWindow();

    if (priority_frames.empty()) {
        return;
    }

    // Use 50% of readahead as our target window (matches buffer-wait default)
    int read_ahead = cache_->GetReadAheadFrames();

    //=========================================================================
    // End-of-Timeline Bypass: If we're within one window of the end/out-point
    // and have all remaining frames cached, skip throttling - we're set to finish
    //=========================================================================
    int current_frame = current_frame_.load();
    int boundary_end = cache_->GetBoundaryEnd();
    int frames_to_end = boundary_end - current_frame;

    // Only applies when approaching end (not after wrap)
    if (frames_to_end > 0 && frames_to_end <= read_ahead) {
        // Count cached frames from current to end
        int cached_to_end = 0;
        for (int f = current_frame; f <= boundary_end; f++) {
            if (cache_->HasFrameReady(f)) {
                cached_to_end++;
            }
        }

        // Require 90% of remaining frames cached before bypassing throttle
        double fill_to_end = static_cast<double>(cached_to_end) / frames_to_end;
        if (fill_to_end >= 0.90) {
            // We have enough cached to finish at full speed - no throttle needed
            if (throttle_state_ != ThrottleState::FULL) {
                throttle_state_ = ThrottleState::FULL;
                current_speed_factor_ = 1.0;
                timeline_timer_->SetPlaybackSpeed(1.0);
                Debug::Log("TimelinePlaybackController: End-of-timeline bypass - " +
                           std::to_string(cached_to_end) + "/" + std::to_string(frames_to_end) +
                           " frames to end (" + std::to_string(static_cast<int>(fill_to_end * 100)) + "%)");
            }
            return;
        }
    }

    int target_frames = std::max(1, read_ahead / 2);
    int frames_to_check = std::min(target_frames + 1, static_cast<int>(priority_frames.size()));

    // Count SEQUENTIAL ready frames from playhead (circle-aware via engine)
    // This is the key difference from before - we stop at first gap
    int sequential_ready = 0;
    for (int i = 0; i < frames_to_check; i++) {
        if (cache_->HasFrameReady(priority_frames[i])) {
            sequential_ready++;
        } else {
            break;  // Stop at first gap - sequential check
        }
    }

    // Calculate fill percentage based on sequential frames
    double fill_percent = (frames_to_check > 0)
        ? static_cast<double>(sequential_ready) / frames_to_check
        : 0.0;

    // Map to throttle state with more granular tiers
    // 50% sequential = full speed, scaling down from there
    ThrottleState new_state;
    if (fill_percent >= 0.50)      new_state = ThrottleState::FULL;         // 50%+ = 100% speed
    else if (fill_percent >= 0.40) new_state = ThrottleState::SLIGHT;       // 40-50% = 75% speed
    else if (fill_percent >= 0.30) new_state = ThrottleState::MODERATE;     // 30-40% = 50% speed
    else if (fill_percent >= 0.20) new_state = ThrottleState::SIGNIFICANT;  // 20-30% = 33% speed
    else if (fill_percent >= 0.15) new_state = ThrottleState::HEAVY;        // 15-20% = 25% speed
    else if (fill_percent >= 0.10) new_state = ThrottleState::SEVERE;       // 10-15% = 12.5% speed
    else if (fill_percent >= 0.05) new_state = ThrottleState::CRAWL;        // 5-10% = ~4fps
    else                           new_state = ThrottleState::BUFFER_PAUSE; // <5% = pause & wait

    // Handle BUFFER_PAUSE - trigger waiting state instead of just slowing
    if (new_state == ThrottleState::BUFFER_PAUSE) {
        // Check ForcePlay cooldown - don't immediately re-trigger BUFFER_PAUSE after user forced play
        auto now = std::chrono::steady_clock::now();
        if (force_play_active_) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - force_play_time_).count();
            if (elapsed < kForcePlayCooldownMs) {
                // Still in cooldown - use CRAWL speed instead of BUFFER_PAUSE
                new_state = ThrottleState::CRAWL;
                // Fall through to normal throttle handling
            } else {
                // Cooldown expired
                force_play_active_ = false;
            }
        }

        // Only trigger BUFFER_PAUSE if not in cooldown
        if (new_state == ThrottleState::BUFFER_PAUSE) {
            // Critical buffer state - pause and wait for buffer to refill
            if (!waiting_for_frame_) {
                waiting_for_frame_ = true;
                waiting_start_time_ = now;
                timeline_timer_->Pause();
                if (audio_mixer_ && !audio_paused_for_throttle_) {
                    audio_mixer_->Pause();
                    audio_paused_for_throttle_ = true;
                }
                Debug::Log("TimelinePlaybackController: Buffer critical (" +
                           std::to_string(sequential_ready) + "/" + std::to_string(frames_to_check) +
                           " sequential) - triggering buffer pause");
            }
            // Don't update throttle state further - UpdateTimer will handle resume
            return;
        }
    }

    // Debounce both directions to prevent oscillation
    // Enum ordering: FULL=0 (fastest) ... BUFFER_PAUSE=7 (slowest)
    // So new_state < throttle_state_ means speeding UP, new_state > means slowing DOWN
    auto now = std::chrono::steady_clock::now();
    static constexpr int kSlowdownDebounceMs = 250;  // Faster response when slowing down

    if (new_state > throttle_state_) {
        // Slowing down (new_state has higher enum value = slower)
        if (was_healthy_) {
            last_healthy_time_ = now;
            was_healthy_ = false;
        }
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_healthy_time_).count();
        if (elapsed >= kSlowdownDebounceMs) {
            // Step down one level at a time for smooth degradation
            int current_level = static_cast<int>(throttle_state_);
            int max_level = static_cast<int>(ThrottleState::CRAWL);  // Don't step into BUFFER_PAUSE via this path
            if (current_level < max_level) {
                throttle_state_ = static_cast<ThrottleState>(current_level + 1);
                last_healthy_time_ = now;  // Reset for next step
                Debug::Log("TimelinePlaybackController: Throttle down to " +
                           std::to_string(static_cast<int>(GetSpeedForState(throttle_state_) * 100)) +
                           "% (" + std::to_string(sequential_ready) + "/" +
                           std::to_string(frames_to_check) + " sequential)");
            }
        }
    } else if (new_state < throttle_state_) {
        // Speeding up (new_state has lower enum value = faster) - longer debounce
        if (!was_healthy_) {
            last_healthy_time_ = now;
            was_healthy_ = true;
        }
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_healthy_time_).count();
        if (elapsed >= kThrottleDebounceMs) {
            // Step up one level at a time for smooth recovery
            int current_level = static_cast<int>(throttle_state_);
            if (current_level > 0) {
                throttle_state_ = static_cast<ThrottleState>(current_level - 1);
                was_healthy_ = false;  // Reset debounce for next step
                Debug::Log("TimelinePlaybackController: Throttle up to " +
                           std::to_string(static_cast<int>(GetSpeedForState(throttle_state_) * 100)) + "%");
            }
        }
    } else {
        // At target state - reset debounce tracking
        was_healthy_ = (new_state == ThrottleState::FULL);
    }

    // Apply speed factor to timer and handle audio sync
    double new_speed = GetSpeedForState(throttle_state_);
    if (new_speed != current_speed_factor_) {
        current_speed_factor_ = new_speed;
        timeline_timer_->SetPlaybackSpeed(current_speed_factor_);

        // Handle audio: use pitch-preserving time stretch for tempo control
        // SoundTouch can handle 0.5x-2.0x range; AudioRateCorrector only ±3%
        // Use time stretch for speed >= 0.5, pause audio below 0.5 (too extreme)
        if (audio_mixer_) {
            if (new_speed >= 0.5) {
                // Use time stretch for smooth tempo control (pitch preserved)
                audio_mixer_->SetPlaybackTempo(new_speed);
                if (audio_paused_for_throttle_) {
                    // Resume audio if it was paused from extreme throttle
                    audio_mixer_->Play();
                    audio_paused_for_throttle_ = false;
                    Debug::Log("TimelinePlaybackController: Audio resumed with tempo " +
                               std::to_string(static_cast<int>(new_speed * 100)) + "%");
                }
            } else {
                // Below 0.5x is too extreme for SoundTouch - pause audio
                if (!audio_paused_for_throttle_) {
                    audio_mixer_->Pause();
                    audio_paused_for_throttle_ = true;
                    Debug::Log("TimelinePlaybackController: Audio paused - throttle too extreme (" +
                               std::to_string(static_cast<int>(new_speed * 100)) + "% speed)");
                }
            }
        }
    }
}

//=============================================================================
// Dual View Mode Implementation
//=============================================================================

bool TimelinePlaybackController::InitializeForDualView(TimelineView* timeline_view, ::VideoPlayer* video_player) {
    if (!timeline_view || !video_player) {
        Debug::Log("TimelinePlaybackController: Invalid parameters for dual view");
        return false;
    }

    if (!timeline_view->IsDualViewMode()) {
        Debug::Log("TimelinePlaybackController: TimelineView is not in dual view mode");
        return false;
    }

#ifdef _WIN32
    // Initialize D3D11DeviceManager BEFORE creating caches
    // This is required for the unified composite pipeline (D3D11VideoDecoder)
    auto& device_mgr = D3D11DeviceManager::Instance();
    if (!device_mgr.IsInitialized()) {
        Debug::Log("TimelinePlaybackController: Initializing D3D11DeviceManager for DUAL_VIEW...");
        if (!device_mgr.Initialize(nullptr)) {
            Debug::Log("TimelinePlaybackController: D3D11DeviceManager init failed - D3D11 decoders unavailable");
        } else {
            Debug::Log("TimelinePlaybackController: D3D11DeviceManager initialized successfully");
        }
    }
#endif

    // Shutdown existing state
    if (initialized_) {
        Shutdown();
    }

    timeline_view_ = timeline_view;
    video_player_ = video_player;
    dual_view_mode_ = true;

    // Get timeline properties
    timeline_duration_ = timeline_view->GetDuration();
    fps_ = timeline_view->GetFrameRate();

    if (timeline_duration_ <= 0.0) {
        // For dual view, duration may be 0 if no clips are loaded yet
        timeline_duration_ = 1.0;  // Default to 1 second
    }

    Debug::Log("TimelinePlaybackController: Initializing dual view for " +
               std::to_string(timeline_duration_) + "s at " + std::to_string(fps_) + " fps");

    // Use canvas dimensions from TimelineView
    width_ = timeline_view->GetCanvasWidth();
    height_ = timeline_view->GetCanvasHeight();
    if (width_ <= 0 || height_ <= 0) {
        width_ = 1920;
        height_ = 1080;
    }

    // Set content dimensions in VideoPlayer
    video_player_->SetContentDimensions(width_, height_);

    // Create PlaybackTimer (shared between both caches)
    timeline_timer_ = std::make_unique<PlaybackTimer>();
    timeline_timer_->SetDuration(timeline_duration_);
    timeline_timer_->SetFrameRate(fps_);
    // SetLooping removed - loop control handled by main.cpp boundary system
    timeline_timer_->Seek(0.0);

    // Setup position change callback
    timeline_timer_->SetOnPositionChanged([this](double pos) {
        current_frame_ = static_cast<int>(std::round(pos * fps_));
    });

    // Create separate flatteners for LEFT and RIGHT tracks
    // Each flattener only "sees" its track using visibility overrides
    // Overrides persist across SetTracks calls, preventing race conditions
    const auto& tracks = timeline_view->GetTracks();

    left_flattener_ = std::make_unique<TimelineFlattener>();
    right_flattener_ = std::make_unique<TimelineFlattener>();

    // Set up visibility overrides BEFORE SetTracks to avoid race conditions
    // LEFT flattener: only sees "left" track
    left_flattener_->EnableVisibilityOverrides(true);
    left_flattener_->SetVisibilityOverride("left", true);
    left_flattener_->SetVisibilityOverride("right", false);

    // RIGHT flattener: only sees "right" track
    right_flattener_->EnableVisibilityOverrides(true);
    right_flattener_->SetVisibilityOverride("left", false);
    right_flattener_->SetVisibilityOverride("right", true);

    // Now set tracks - visibility overrides are already in place
    left_flattener_->SetTracks(tracks);
    right_flattener_->SetTracks(tracks);

    // Debug: Log track info
    Debug::Log("TimelinePlaybackController: Configured dual flatteners - " + std::to_string(tracks.size()) + " tracks:");
    for (const auto& track : tracks) {
        Debug::Log("  Track id='" + track.id + "' name='" + track.name + "' clips=" +
                   std::to_string(track.clips.size()) + " visible=" + std::to_string(track.visible));
        for (size_t i = 0; i < track.clips.size() && i < 3; i++) {
            const auto& clip = track.clips[i];
            Debug::Log("    Clip[" + std::to_string(i) + "]: '" + clip.name + "' start=" +
                       std::to_string(clip.start_time) + " dur=" + std::to_string(clip.duration) +
                       " linked=" + std::to_string(clip.is_linked) + " gap=" + std::to_string(clip.is_gap));
        }
    }

    // Create LEFT cache (reuse main cache_)
    cache_ = std::make_unique<TimelineCache>();

    TimelineCacheConfig cache_config;
    cache_config.readAheadFrames = config_.readAheadFrames;
    cache_config.readBehindFrames = config_.readBehindFrames;
    cache_config.io_threads = config_.io_threads;  // Full threads for each cache (was /2)
    cache_config.max_textures = g_timeline_max_textures;  // Full textures for each cache (was /2)
    cache_config.fps = fps_;
    cache_config.pipeline_mode = config_.pipeline_mode;  // Use project pipeline mode for D3D11 decoders
    // Note: use_shared_pool = false for dual view because both caches may reference
    // the same source frames (same file, different slips), causing key collisions.
    cache_config.use_shared_pool = false;

    cache_->SetConfig(cache_config);
    cache_->Initialize(tracks, left_flattener_.get(), fps_,
                       TimelineSourceMode::DUAL_VIEW);
    cache_->SetGapTextureDimensions(width_, height_);
    cache_->SetCanvasDimensions(width_, height_);

    // Create RIGHT cache
    right_cache_ = std::make_unique<TimelineCache>();
    right_cache_->SetConfig(cache_config);
    right_cache_->Initialize(tracks, right_flattener_.get(), fps_,
                             TimelineSourceMode::DUAL_VIEW);
    right_cache_->SetGapTextureDimensions(width_, height_);
    right_cache_->SetCanvasDimensions(width_, height_);

    // Register sequence metadata for both caches
    for (const auto& track : tracks) {
        for (const auto& clip : track.clips) {
            if (clip.is_sequence && clip.is_linked && !clip.linked_path.empty()) {
                SequenceMetadata seq_meta;
                seq_meta.directory = clip.sequence_directory;
                seq_meta.pattern = clip.sequence_pattern;
                seq_meta.start_frame = clip.sequence_start_frame;
                seq_meta.end_frame = clip.sequence_end_frame;
                seq_meta.exr_layer = clip.sequence_exr_layer;
                seq_meta.pipeline_mode = DetectSequencePipelineMode(clip);
                seq_meta.valid = true;
                cache_->RegisterSequenceMetadata(clip.linked_path, seq_meta);
                right_cache_->RegisterSequenceMetadata(clip.linked_path, seq_meta);
            }
        }
    }

    initialized_ = true;
    use_virtual_timeline_ = true;

    // Trigger initial prefetch for frame 0 on both caches
    if (cache_) {
        cache_->UpdatePlayhead(0, false);  // Not playing, just warm up frame 0
    }
    if (right_cache_) {
        right_cache_->UpdatePlayhead(0, false);
    }

    // Initialize audio mixer for dual view mode
    // Use main timeline flattener so we get audio from both tracks
    audio_mixer_ = std::make_unique<AudioMixer>();
    if (audio_mixer_->Initialize()) {
        audio_mixer_->SetFlattener(&timeline_view->GetFlattener());
        audio_mixer_->SetTimer(timeline_timer_.get());

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
        Debug::Log("TimelinePlaybackController: Audio mixer initialized for dual view with " +
                   std::to_string(all_clips.size()) + " clips");
    } else {
        Debug::Log("TimelinePlaybackController: Audio mixer init failed for dual view");
        audio_mixer_.reset();
    }

#ifdef _WIN32
    // Initialize unified composite pipeline for D3D11 dual view
    // This uses a single D3D11->GL interop instead of two competing ones,
    // solving the buffer exhaustion issue (25% frame loss per loop)
    InitializeUnifiedDualViewPipeline();
#endif

    Debug::Log("TimelinePlaybackController: Dual view initialized successfully");
    return true;
}

#ifdef _WIN32
void TimelinePlaybackController::InitializeUnifiedDualViewPipeline() {
    // Already initialized?
    if (dual_view_pipeline_ && dual_view_pipeline_->IsInitialized()) {
        return;
    }

    // Already determined that unified pipeline isn't possible?
    if (unified_not_possible_) {
        return;
    }

    // Check if either cache has image sequence content
    // Image sequences use GL-based loading, not D3D11, so we can't use the unified pipeline
    bool left_has_sequences = cache_ && cache_->HasImageSequenceContent();
    bool right_has_sequences = right_cache_ && right_cache_->HasImageSequenceContent();

    if (left_has_sequences || right_has_sequences) {
        unified_not_possible_ = true;
        Debug::Log("TimelinePlaybackController: Unified pipeline disabled - " +
                   std::string(left_has_sequences ? "LEFT" : "RIGHT") +
                   " track has image sequence content (using GL fallback)");
        return;
    }

    // Check if both caches have D3D11 decoders (VIDEO_FILE mode)
    D3D11VideoDecoder* left_decoder = cache_ ? cache_->GetD3D11Decoder() : nullptr;
    D3D11VideoDecoder* right_decoder = right_cache_ ? right_cache_->GetD3D11Decoder() : nullptr;

    // Only use unified pipeline if BOTH sources have D3D11 decoders
    // EXR/image sequences bypass D3D11 entirely, so we fall back to legacy mode
    if (!left_decoder || !right_decoder) {
        // Decoders not available yet - will retry on next UpdateDualView
        // (Decoders are created on-demand when first frame is requested)
        return;
    }

    // Get dimensions from decoders
    int left_w = left_decoder->GetWidth();
    int left_h = left_decoder->GetHeight();
    int right_w = right_decoder->GetWidth();
    int right_h = right_decoder->GetHeight();

    if (left_w <= 0 || left_h <= 0 || right_w <= 0 || right_h <= 0) {
        // Decoders not fully initialized yet - will retry later
        return;
    }

    // Get D3D11 device
    auto& device_manager = D3D11DeviceManager::Instance();
    ID3D11Device* device = device_manager.GetDevice();
    if (!device) {
        Debug::Log("TimelinePlaybackController: No D3D11 device available");
        return;
    }

    // Create unified pipeline
    dual_view_pipeline_ = std::make_unique<DualViewPipeline>();
    if (!dual_view_pipeline_->Initialize(device, left_w, left_h, right_w, right_h)) {
        Debug::Log("TimelinePlaybackController: Failed to initialize unified pipeline");
        dual_view_pipeline_.reset();
        return;
    }

    // Set decoder references
    dual_view_pipeline_->SetDecoders(left_decoder, right_decoder);

    use_unified_composite_ = true;
    Debug::Log("TimelinePlaybackController: Unified composite pipeline initialized (" +
               std::to_string(left_w) + "x" + std::to_string(left_h) + " + " +
               std::to_string(right_w) + "x" + std::to_string(right_h) + ")");
}
#endif

void TimelinePlaybackController::SyncDualFlatteners() {
    if (!dual_view_mode_ || !timeline_view_) return;

    const auto& tracks = timeline_view_->GetTracks();
    double duration = timeline_view_->GetDuration();

    // Reset unified_not_possible_ flag so we re-evaluate on next UpdateDualView
    // (In case user replaced image sequence with video or vice versa)
    unified_not_possible_ = false;

    // Update both flatteners with current tracks
    // Visibility overrides persist across SetTracks calls, so we don't need to reset them
    if (left_flattener_) {
        left_flattener_->SetTracks(tracks);
    }

    if (right_flattener_) {
        right_flattener_->SetTracks(tracks);
    }

    // Register sequence metadata for any image sequence clips
    // This is critical for dual view: InitializeForDualView runs with empty tracks,
    // so we must re-register when media is loaded via LoadMediaToLeftTrack/LoadMediaToRightTrack
    for (const auto& track : tracks) {
        for (const auto& clip : track.clips) {
            if (clip.is_sequence && clip.is_linked && !clip.linked_path.empty()) {
                SequenceMetadata seq_meta;
                seq_meta.directory = clip.sequence_directory;
                seq_meta.pattern = clip.sequence_pattern;
                seq_meta.start_frame = clip.sequence_start_frame;
                seq_meta.end_frame = clip.sequence_end_frame;
                seq_meta.exr_layer = clip.sequence_exr_layer;
                seq_meta.pipeline_mode = DetectSequencePipelineMode(clip);
                seq_meta.valid = true;
                if (cache_) {
                    cache_->RegisterSequenceMetadata(clip.linked_path, seq_meta);
                }
                if (right_cache_) {
                    right_cache_->RegisterSequenceMetadata(clip.linked_path, seq_meta);
                }
            }
        }
    }

    // Update duration for timer and both caches
    timeline_duration_ = duration;
    if (timeline_timer_) {
        timeline_timer_->SetDuration(duration);
    }
    if (cache_) {
        cache_->UpdateDuration(duration);
        cache_->NotifyTracksEdited();
    }
    if (right_cache_) {
        right_cache_->UpdateDuration(duration);
        right_cache_->NotifyTracksEdited();
    }

#ifdef _WIN32
    // Check if content type changed - if we now have image sequences, shutdown the D3D11 pipeline
    bool left_has_sequences = cache_ && cache_->HasImageSequenceContent();
    bool right_has_sequences = right_cache_ && right_cache_->HasImageSequenceContent();

    if ((left_has_sequences || right_has_sequences) && use_unified_composite_) {
        // Content changed from video-only to include image sequences
        // Shutdown the D3D11 pipeline and fall back to GL mode
        Debug::Log("TimelinePlaybackController: Content changed to include image sequences - disabling unified pipeline");
        if (dual_view_pipeline_) {
            dual_view_pipeline_->Shutdown();
            dual_view_pipeline_.reset();
        }
        use_unified_composite_ = false;
        unified_not_possible_ = true;
    }
    // When content changes, the old decoders are for the OLD media and the new decoders
    // haven't been created yet. Clear decoder references and let UpdateDualView re-establish
    // them once new decoders are available via InitializeUnifiedDualViewPipeline.
    else if (dual_view_pipeline_ && dual_view_pipeline_->IsInitialized()) {
        // Clear decoder references - they're stale after media change
        // The pipeline will be re-initialized with correct decoders on next UpdateDualView
        dual_view_pipeline_->SetDecoders(nullptr, nullptr);
        dual_view_pipeline_->InvalidateLayout();

        // Shutdown and reset the pipeline so it gets re-initialized with new content
        // This is safer than keeping stale pointers when resolution may have changed
        dual_view_pipeline_->Shutdown();
        dual_view_pipeline_.reset();
        use_unified_composite_ = false;  // Will be re-initialized in UpdateDualView

        Debug::Log("TimelinePlaybackController: Pipeline reset for media change, will reinitialize");
    }
#endif

    // Refresh audio mixer with any new clips (mirrors NotifyTracksEdited behavior)
    if (audio_mixer_) {
        std::vector<OTIOClip> all_clips;
        for (const auto& track : tracks) {
            for (const auto& clip : track.clips) {
                if (!clip.is_gap && (!clip.linked_path.empty() || !clip.file_path.empty())) {
                    all_clips.push_back(clip);
                }
            }
        }
        audio_mixer_->PreloadClips(all_clips);

        // Force refresh to handle trim/slip edits where clip IDs don't change
        audio_mixer_->ForceRefresh();
    }

    Debug::Log("TimelinePlaybackController: Synced dual flatteners - " +
               std::to_string(tracks.size()) + " tracks, duration=" +
               std::to_string(duration) + "s");
}

TimelinePlaybackController::DualViewTextures TimelinePlaybackController::UpdateDualView() {
    DualViewTextures result;

    if (!initialized_ || !dual_view_mode_) {
        return result;
    }

    // Update timer
    UpdateTimer();

    int frame = current_frame_.load();
    bool is_playing = is_playing_.load();

#ifdef _WIN32
    // Try deferred pipeline initialization if not yet done
    // (Decoders are created on-demand, so we retry until they're available)
    // Skip if unified_not_possible_ is set (e.g., one side has image sequences)
    if (!use_unified_composite_ && !dual_view_pipeline_ && !unified_not_possible_) {
        InitializeUnifiedDualViewPipeline();
    }

    // Use unified composite pipeline if available (single D3D11 interop)
    if (use_unified_composite_ && dual_view_pipeline_) {
        dual_view_pipeline_->SetPlaybackMode(is_playing);

        // Update caches for decode prefetching
        if (cache_) {
            cache_->UpdatePlayhead(frame, is_playing);
        }
        if (right_cache_) {
            right_cache_->UpdatePlayhead(frame, is_playing);
        }

        // Map timeline frame to source frames for each cache
        // This applies source_in offset from trim/slip edits
        // Use -1 to indicate gap (no valid source at this position)
        int left_source_frame = -1;
        int right_source_frame = -1;
        bool left_is_gap = true;
        bool right_is_gap = true;

        if (cache_) {
            SourceCoords left_coords = cache_->GetSourceCoords(frame);
            if (left_coords.valid) {
                left_source_frame = left_coords.source_frame;
                left_is_gap = false;
            }
        }
        if (right_cache_) {
            SourceCoords right_coords = right_cache_->GetSourceCoords(frame);
            if (right_coords.valid) {
                right_source_frame = right_coords.source_frame;
                right_is_gap = false;
            }
        }

        // Get composite texture from unified pipeline
        // Pipeline handles gaps by using transparent texture when source_frame is -1
        GLuint composite = dual_view_pipeline_->UpdateFrame(
            left_is_gap ? -1 : left_source_frame,
            right_is_gap ? -1 : right_source_frame);

        if (composite != 0) {
            result.is_unified = true;
            result.composite_texture = composite;

            const auto& layout = dual_view_pipeline_->GetLayout();
            result.composite_width = layout.composite_width;
            result.composite_height = layout.composite_height;

            // Copy UV coordinates from layout
            result.left_uv_min_x = layout.left_uv.u_min;
            result.left_uv_max_x = layout.left_uv.u_max;
            result.left_uv_min_y = layout.left_uv.v_min;
            result.left_uv_max_y = layout.left_uv.v_max;

            result.right_uv_min_x = layout.right_uv.u_min;
            result.right_uv_max_x = layout.right_uv.u_max;
            result.right_uv_min_y = layout.right_uv.v_min;
            result.right_uv_max_y = layout.right_uv.v_max;

            // Also set individual widths/heights for aspect calculations
            result.left_width = layout.left_source_width;
            result.left_height = layout.left_source_height;
            result.right_width = layout.right_source_width;
            result.right_height = layout.right_source_height;
        }

        return result;
    }
#endif

    // NOTE: ProcessPendingUploads now called from ProcessPendingTextureUploads()
    // BEFORE ImGui::NewFrame() to avoid GL state corruption during render
    if (cache_) {
        cache_->UpdatePlayhead(frame, is_playing);
    }
    if (right_cache_) {
        right_cache_->UpdatePlayhead(frame, is_playing);
    }

    // Get frames from both caches (legacy path - two separate interops)
    int left_w = 0, left_h = 0;
    int right_w = 0, right_h = 0;

    if (cache_) {
        result.left_texture = cache_->GetFrame(frame, left_w, left_h);
        result.left_width = left_w;
        result.left_height = left_h;
    }

    if (right_cache_) {
        result.right_texture = right_cache_->GetFrame(frame, right_w, right_h);
        result.right_width = right_w;
        result.right_height = right_h;
    }

    return result;
}

//=============================================================================
// AB-Loop for In/Out Point Playback
// Note: AB-loop was only used by MPV. Now handled via in/out points in timeline.
//=============================================================================

void TimelinePlaybackController::SetABLoop(double /*in_point*/, double /*out_point*/) {
    // No-op: AB-loop was MPV-specific feature, now handled via timeline in/out points
}

void TimelinePlaybackController::ClearABLoop() {
    // No-op: AB-loop was MPV-specific feature
}

bool TimelinePlaybackController::HasABLoop() const {
    // AB-loop was MPV-specific feature
    return false;
}

} // namespace ump
