#include "timeline_manager.h"
#include "../player/video_player.h"
#include "../project/project_manager.h"
#include "../utils/debug_utils.h"
#include <cmath>
#ifdef _WIN32
#include <Windows.h>
#endif

// Helper for timeline debug output
static void TimelineDebugLog(const std::string& message) {
    // Primary output: Visual Studio debug console (this is what you'll see)
    Debug::Log("TimelineManager: " + message);
    
#ifdef _WIN32
    // Also ensure direct Visual Studio output
    std::string formatted = "[Timeline] " + message + "\n";
    OutputDebugStringA(formatted.c_str());
#endif
    
    // Console output (won't be visible in VS but useful for command line)
    std::cout << "[Timeline] " << message << std::endl;
}

TimelineManager::TimelineManager() 
    : ui_position(0.0)
    , ui_duration(0.0)
    , is_scrubbing(false)
    , was_playing_before_scrub(false)
    , mpv_position(0.0)
    , pending_seek_position(-1.0)
    , last_seek_time(std::chrono::steady_clock::now())
    , last_sync_time(std::chrono::steady_clock::now())
    , project_manager(nullptr)
{
    // Test debug output immediately
    TimelineDebugLog("TimelineManager constructor called - testing debug output");
    
    // Cache access will now be routed through ProjectManager
    Debug::Log("TimelineManager: Initialized (cache access through ProjectManager)");
}

TimelineManager::~TimelineManager() {
}

void TimelineManager::SetProjectManager(ump::ProjectManager* pm) {
    project_manager = pm;
    TimelineDebugLog("ProjectManager reference set");
}

void TimelineManager::Update(VideoPlayer* video_player) {
    if (!video_player) return;

    auto now = std::chrono::steady_clock::now();

    // EXR MODE: Skip all MPV-related operations (no dummy video seeks/syncs)
    if (video_player->IsInEXRMode()) {
        // Update duration
        ui_duration = video_player->GetDuration();

        // Update position from VideoPlayer (always update, even when paused)
        // This ensures transport controls (step frame, go to start/end) update playhead visual
        if (!is_scrubbing) {
            ui_position = video_player->GetPosition();
        }

        return;
    }

    // VIDEO MODE: Normal MPV operations
    // Process pending seek operations (throttled)
    if (pending_seek_position >= 0.0) {
        ProcessPendingSeek(video_player);
    }

    // Handle fast seeking with cache integration
    if (video_player->IsFastSeeking() && !is_scrubbing) {
        // Fast seeking uses cache for smooth preview, similar to scrubbing
        HandleFastSeeking(video_player);
    }
    // Handle fast seeking stop - sync video back to cached position
    else if (!video_player->IsFastSeeking() && hold_cached_frame && target_seek_position < 0.0) {
        if (!video_player->IsPlaying()) {
            // Paused mode - seek and use full flicker protection for precise positioning
            video_player->Seek(ui_position);
            pending_seek_position = -1.0;
            last_seek_time = now;
            
            target_seek_position = ui_position;
            stable_frame_count = 0;
            // Debug::Log("FAST_SEEK_END: Paused mode - seeking to " + std::to_string(ui_position) + "s with full flicker protection");
        } else {
            // Playing mode - pause first, then seek when paused (like scrubbing)
            video_player->Pause();

            // Check if pause took effect, if not use pending seek
            if (!video_player->IsPlaying()) {
                // Pause successful - seek immediately
                video_player->Seek(ui_position);
                pending_seek_position = -1.0;
                last_seek_time = now;

                target_seek_position = ui_position;
                stable_frame_count = 0;

                // DON'T restore playback immediately - let flicker protection handle it
                // The SyncFromMPV logic will restore playback when video properly syncs
                restore_playback_after_seek = true;
                // Debug::Log("FAST_SEEK_END: Playing mode - paused, seeked, will restore playback when synced to " + std::to_string(ui_position) + "s");
            } else {
                // Pause didn't take effect immediately - use pending seek for next frame
                pending_seek_position = ui_position;
                target_seek_position = ui_position;
                stable_frame_count = 0;

                // Will restore playback in next Update() after seek completes
                video_player->Play();
                // Debug::Log("FAST_SEEK_END: Playing mode - pause pending, will seek next frame to " + std::to_string(ui_position) + "s");
            }
        }
    }
    // Sync UI to MPV periodically when not scrubbing or fast seeking (throttled)
    else if (!is_scrubbing) {
        if (now - last_sync_time >= SYNC_THROTTLE_MS) {
            SyncFromMPV(video_player);
            last_sync_time = now;
        } else {
            // Between sync updates, smoothly interpolate position for playback
            if (video_player->IsPlaying() && ui_duration > 0) {
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_sync_time);
                double interpolated_offset = elapsed.count() / 1000.0;
                ui_position = mpv_position + interpolated_offset;
                
                // Clamp to valid range
                if (ui_position > ui_duration) ui_position = ui_duration;
                if (ui_position < 0.0) ui_position = 0.0;
            }
        }
    }
}

void TimelineManager::StartScrubbing(VideoPlayer* video_player) {
    if (!video_player) return;

    is_scrubbing = true;
    was_playing_before_scrub = video_player->IsPlaying();
    
    // Reset stability tracking for clean state
    stable_frame_count = 0;
    last_stable_position = -1.0;
    
    if (was_playing_before_scrub) {
        video_player->Pause();
    }

    //Debug::Log("Timeline: Started scrubbing, was_playing=" + std::string(was_playing_before_scrub ? "true" : "false"));
}

void TimelineManager::UpdateScrubbing(double new_position, VideoPlayer* video_player) {
    if (!is_scrubbing) return;

    // Update UI position immediately for instant visual feedback
    ui_position = new_position;

    // Clamp to valid range
    if (ui_position < 0.0) ui_position = 0.0;
    if (ui_position > ui_duration) ui_position = ui_duration;

    // EXR MODE: Don't seek during scrub - too heavy!
    // Just update UI position, actual frame load happens on mouse release (StopScrubbing)
    if (video_player && video_player->IsInEXRMode()) {
        // Update UI only - no seeking, no cache updates
        pending_seek_position = -1.0; // No seek while dragging
        hold_cached_frame = false;
        return;
    }

    // VIDEO MODE: Normal scrubbing behavior with cache
    // Update frame cache scrub position for background caching
    if (project_manager) {
        FrameCache* cache = project_manager->GetCurrentVideoCache();
        if (cache) {
            cache->UpdateScrubPosition(ui_position, nullptr); // Will be set by video_player
        }

        // Check if we have a cached frame at this position
        GLuint dummy_texture_id;
        int dummy_width, dummy_height;
        bool cache_hit = project_manager->GetCachedFrame(ui_position, dummy_texture_id, dummy_width, dummy_height);

        if (cache_hit) {
            // Cache hit - no need to seek video, cached frame will be shown
            pending_seek_position = -1.0; // Cancel any pending video seek
            hold_cached_frame = true; // Keep showing this cached frame
            // Debug::Log("SCRUB: Using cached frame at " + std::to_string(ui_position) + "s");
        } else {
            // Cache miss - fallback to video seeking
            pending_seek_position = ui_position;
            hold_cached_frame = false; // Don't hold cache, use video
            // Debug::Log("SCRUB: Cache miss, seeking video to " + std::to_string(ui_position) + "s");
        }
    } else {
        // No cache available - always seek video
        pending_seek_position = ui_position;
    }
}

void TimelineManager::StopScrubbing(VideoPlayer* video_player) {
    if (!video_player || !is_scrubbing) return;

    is_scrubbing = false;

    // EXR MODE: Seek to final position now (on mouse release)
    if (video_player->IsInEXRMode()) {
        // CRITICAL: Update VideoPlayer's internal position (used by InjectEXRFrame)
        video_player->SetEXRPosition(ui_position);

        // ALSO seek dummy MPV video to keep it in sync
        video_player->Seek(ui_position);

        // Update cache position to trigger frame load
        video_player->GetEXRCache()->UpdateCurrentPosition(ui_position);

        // Don't pause/play during EXR scrubbing!
        // The cache can handle continuous playback - pausing causes:
        // 1. Async seek race condition (play before seek completes)
        // 2. Cache fill interruption
        // 3. Unnecessary state changes
        // Just let playback continue seamlessly through the seek

        // OLD CODE (removed):
        // if (was_playing_before_scrub) {
        //     video_player->Play();
        // }

        Debug::Log("SCRUB END (EXR): Seeking to " + std::to_string(ui_position) + "s, triggered cache update (no pause/play cycle)");
        return;
    }

    // VIDEO MODE: Normal seek behavior with caching
    // Cache-first approach for playback mode only to avoid flicker
    bool use_cached_frame = false;
    if (was_playing_before_scrub && project_manager) {
        GLuint dummy_texture_id;
        int dummy_width, dummy_height;
        bool cache_hit = project_manager->GetCachedFrame(ui_position, dummy_texture_id, dummy_width, dummy_height);

        if (cache_hit) {
            // Cache hit during playback mode - use cached frame and avoid video seek (like FF/RW)
            hold_cached_frame = true;
            pending_seek_position = -1.0; // Cancel any pending video seek
            target_seek_position = -1.0; // No transition needed - stay on cached frame
            stable_frame_count = 0;
            use_cached_frame = true;
            // Debug::Log("SCRUB END: Playback mode - using cached frame at " + std::to_string(ui_position) + "s, no video seek");
        }
    }

    if (!use_cached_frame) {
        // Non-playback mode or cache miss - seek video to final UI position (current working behavior)
        video_player->Seek(ui_position);
        pending_seek_position = -1.0;
        last_seek_time = std::chrono::steady_clock::now();

        // Wait for video to properly sync after seeking - this prevents flash frames
        if (hold_cached_frame) {
            target_seek_position = ui_position;
            stable_frame_count = 0;
            // Debug::Log("SCRUB END: Keeping cached frame until video syncs to position " + std::to_string(ui_position));
        }
        // Debug::Log("SCRUB END: Seeking video to final position " + std::to_string(ui_position) + "s");
    }

    // Restore playback state
    if (was_playing_before_scrub) {
        video_player->Play();
    }

    //Debug::Log("Timeline: Stopped scrubbing, restoring playback=" + std::string(was_playing_before_scrub ? "true" : "false"));
}

void TimelineManager::SyncFromMPV(VideoPlayer* video_player) {
    if (!video_player) return;

    auto now = std::chrono::steady_clock::now();

    // Update UI state from MPV
    double new_position = video_player->GetPosition();
    double new_duration = video_player->GetDuration();
    bool current_playing_state = video_player->IsPlaying();
    
    // Only update if values are valid
    if (new_duration > 0) {
        ui_duration = new_duration;
    }
    
    if (new_position >= 0) {
        mpv_position = new_position;
        
        // Don't update UI position while holding cached frame - keep showing the cached position
        if (!hold_cached_frame) {
            ui_position = new_position;
        }
        
        // If video position changed and matches our target, we can stop holding cached frame
        // OPTIMIZED: Only run stability logic when actually holding a cached frame
        if (hold_cached_frame && target_seek_position >= 0.0) {
            double position_diff = std::abs(new_position - target_seek_position);
            
            // Different logic for playing vs paused video with timeout fallback
            bool should_release = false;
            
            if (current_playing_state) {
                // During playback: more lenient - if we're near the target or past it
                if (new_position >= target_seek_position - 0.1) { // Within 100ms or past target
                    stable_frame_count++;
                    // Try for ready frame first, but fallback to basic texture immediately for seeking
                    if (stable_frame_count >= 0) { // Immediate transition for seeking
                        if (video_player->IsReadyToRender()) {
                            should_release = true;
                            // Debug::Log("TRANSITION: Playback active, video ready, releasing cached frame");
                        } else if (video_player->HasValidTexture()) {
                            should_release = true; // Immediate fallback for seeking
                            // Debug::Log("TRANSITION: Playback active, immediate fallback, releasing cached frame");
                        }
                    }
                } else {
                    stable_frame_count = 0;
                }
            } else {
                // During pause: precise positioning required
                if (position_diff < 0.03) { // Within 30ms when paused
                    stable_frame_count++;
                    // Try for ready frame first, but fallback to basic texture after timeout
                    if (stable_frame_count >= 2) {
                        if (video_player->IsReadyToRender()) {
                            should_release = true;
                            // Debug::Log("TRANSITION: Paused, video ready, releasing cached frame");
                        } else if (stable_frame_count >= 2 && video_player->HasValidTexture()) {
                            should_release = true; // Timeout fallback after 5 frames
                            // Debug::Log("TRANSITION: Paused, timeout fallback, releasing cached frame");
                        }
                    }
                } else {
                    stable_frame_count = 0;
                }
            }
            
            if (should_release) {
                hold_cached_frame = false;
                target_seek_position = -1.0;
                stable_frame_count = 0;
                last_stable_position = -1.0;
                ui_position = new_position;

                // Restore playback if we were waiting for seek to complete
                if (restore_playback_after_seek) {
                    video_player->Play();
                    restore_playback_after_seek = false;
                    // Debug::Log("TRANSITION: Seek completed, restoring playback");
                }
            }
        }
        
        // Update frame cache with current position for background caching
        if (project_manager) {
            FrameCache* cache = project_manager->GetCurrentVideoCache();
            if (cache) {
                cache->UpdateScrubPosition(new_position, video_player);
            }
            
            // Notify cache about playback state changes
            static bool last_playing_state = false;
            if (current_playing_state != last_playing_state) {
                if (cache) {
                    cache->NotifyPlaybackState(current_playing_state);
                }
                last_playing_state = current_playing_state;
            }
        }
    }
}

void TimelineManager::ProcessPendingSeek(VideoPlayer* video_player) {
    if (!video_player || pending_seek_position < 0.0) return;

    auto now = std::chrono::steady_clock::now();
    
    // Check if enough time has passed since last seek
    if (now - last_seek_time >= SEEK_THROTTLE_MS) {
        video_player->Seek(pending_seek_position);
        pending_seek_position = -1.0;
        last_seek_time = now;
    }
}

bool TimelineManager::GetCachedFrameForScrubbing(double timestamp, GLuint& texture_id, int& width, int& height) {
    if (!project_manager) return false;
    
    return project_manager->GetCachedFrame(timestamp, texture_id, width, height);
}

void TimelineManager::SetCacheConfig(const FrameCache::CacheConfig& config) {
    if (!project_manager) return;
    
    project_manager->SetCacheConfig(config);
    Debug::Log("TimelineManager: Cache configuration updated");
}

FrameCache::CacheStats TimelineManager::GetCacheStats() const {
    if (!project_manager) return FrameCache::CacheStats{};
    
    return project_manager->GetCacheStats();
}

void TimelineManager::SetVideoFile(const std::string& video_path) {
    Debug::Log("*** TimelineManager::SetVideoFile called with: " + video_path);
    
    // Reset cached frame state for new video to prevent seeking to old positions
    // This ensures new videos start from the beginning, not from cached frame positions
    hold_cached_frame = false;
    target_seek_position = -1.0;
    stable_frame_count = 0;
    last_stable_position = -1.0;
    ui_position = 0.0; // Reset to beginning of new video
    
    // Cache management is now handled by ProjectManager
    // ProjectManager will automatically create/manage caches for each video file
    if (project_manager && !video_path.empty()) {
        Debug::Log("*** NOTIFYING PROJECT MANAGER OF VIDEO CHANGE ***");
        TimelineDebugLog("Video file changed, ProjectManager will handle caching: " + video_path);
        
        // Cache will be automatically created by ProjectManager when needed
        // No need to create FrameCache here anymore
    } else {
        Debug::Log("*** No project manager reference, cache management disabled ***");
    }
}

void TimelineManager::NotifyPlaybackState(bool is_playing) {
    if (!project_manager) return;
    
    FrameCache* cache = project_manager->GetCurrentVideoCache();
    if (cache) {
        cache->NotifyPlaybackState(is_playing);
    }
}

std::vector<FrameCache::CacheSegment> TimelineManager::GetCacheSegments() const {
    if (!project_manager) return std::vector<FrameCache::CacheSegment>();

    return project_manager->GetCacheSegments();
}

void TimelineManager::HandleFastSeeking(VideoPlayer* video_player) {
    if (!video_player || !project_manager) return;
    
    // Calculate seek amount based on video player's fast seek parameters
    double seek_amount = 0.1 * video_player->GetFastSeekSpeed();
    if (!video_player->IsFastForward()) seek_amount = -seek_amount;
    
    // Calculate new position
    double new_position = ui_position + seek_amount;
    
    // Clamp to valid range
    if (new_position < 0.0) new_position = 0.0;
    if (new_position > ui_duration) new_position = ui_duration;
    
    // Update UI position for time progression
    ui_position = new_position;
    
    // Try to get cached frame at new position for smooth preview
    FrameCache* cache = project_manager->GetCurrentVideoCache();
    if (cache) {
        GLuint dummy_texture_id;
        int dummy_width, dummy_height;
        bool cache_hit = project_manager->GetCachedFrame(ui_position, dummy_texture_id, dummy_width, dummy_height);
        
        if (cache_hit) {
            // Cache hit - show cached frame and avoid video seeking to prevent conflicts
            hold_cached_frame = true;
            pending_seek_position = -1.0; // Cancel any pending video seek
            // Debug::Log("FAST_SEEK: Using cached frame at " + std::to_string(ui_position) + "s, skipping video seek");
            
            // Only update cache position when we have a cache hit
            cache->UpdateScrubPosition(ui_position, nullptr); // Don't pass video_player to avoid conflicts
        } else {
            // Cache miss - let video player handle seeking naturally by not holding cached frame
            hold_cached_frame = false;
            // Allow video seeking to proceed in main loop since we don't have cached frame
            // Debug::Log("FAST_SEEK: Cache miss, letting video handle seeking at " + std::to_string(ui_position) + "s");
        }
    } else {
        // No cache available - rely on video player
        hold_cached_frame = false;
    }
}