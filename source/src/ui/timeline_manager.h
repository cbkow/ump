#pragma once

#include <chrono>
#include <memory>
#include "../player/frame_cache.h"

class VideoPlayer;

// Forward declaration to avoid circular dependency
namespace ump {
    class ProjectManager;
}

class TimelineManager {
public:
    TimelineManager();
    ~TimelineManager();
    
    // Set project manager reference for cache access
    void SetProjectManager(ump::ProjectManager* project_manager);

    // Core update method - called every frame
    void Update(VideoPlayer* video_player);

    // Scrubbing interface
    void StartScrubbing(VideoPlayer* video_player);
    void UpdateScrubbing(double new_position, VideoPlayer* video_player = nullptr);
    void StopScrubbing(VideoPlayer* video_player);

    // UI state getters - always smooth and responsive
    double GetUIPosition() const { return ui_position; }
    double GetUIDuration() const { return ui_duration; }
    bool IsScrubbing() const { return is_scrubbing; }
    bool IsHoldingCachedFrame() const { return hold_cached_frame; }
    
    // Frame cache interface
    bool GetCachedFrameForScrubbing(double timestamp, GLuint& texture_id, int& width, int& height);
    void SetCacheConfig(const FrameCache::CacheConfig& config);
    void SetVideoFile(const std::string& video_path); // Notify cache of new video file
    void NotifyPlaybackState(bool is_playing); // Notify cache of playback state
    FrameCache::CacheStats GetCacheStats() const;
    std::vector<FrameCache::CacheSegment> GetCacheSegments() const;

private:
    // UI state (always responsive)
    double ui_position = 0.0;
    double ui_duration = 0.0;
    bool is_scrubbing = false;
    bool was_playing_before_scrub = false;
    bool hold_cached_frame = false;  // Keep showing cached frame during video seek transition
    double target_seek_position = -1.0;  // Target position for cached frame hold release
    int stable_frame_count = 0;  // Count of consecutive stable frames at target position
    double last_stable_position = -1.0;  // Last stable position for stability tracking
    bool restore_playback_after_seek = false;  // Restore playback when seek completes
    
    // MPV sync state  
    double mpv_position = 0.0;
    double pending_seek_position = -1.0;
    
    // Throttling timers
    std::chrono::steady_clock::time_point last_seek_time;
    std::chrono::steady_clock::time_point last_sync_time;
    
    // Throttling intervals
    static constexpr auto SEEK_THROTTLE_MS = std::chrono::milliseconds(16); // ~60fps max seeks
    static constexpr auto SYNC_THROTTLE_MS = std::chrono::milliseconds(100); // Sync UI to MPV
    
    // Project manager reference for cache access
    ump::ProjectManager* project_manager = nullptr;

    // Internal methods
    void SyncFromMPV(VideoPlayer* video_player);
    void ProcessPendingSeek(VideoPlayer* video_player);
    void HandleFastSeeking(VideoPlayer* video_player);
};