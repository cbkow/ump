#pragma once

#include <string>
#include <memory>
#include <atomic>
#include <chrono>
#include <functional>

#include <glad/gl.h>

#ifdef _WIN32
#include <d3d11_1.h>
#endif

// Include VideoPlayer (alias to VideoDisplayComponent)
#include "../player/video_player.h"
#include "../player/pipeline_mode.h"

namespace qcview {

// Forward declaration for D3D11VA HDR decoder (old)
class D3D11VAVideoDecoder;
// Forward declaration for D3D11 GPU-native decoder (new)
class D3D11VideoDecoder;
// Forward declaration for unified dual view pipeline
class DualViewPipeline;

// Forward declarations
class TimelineView;
class TimelineCache;
class TimelineFlattener;
class AudioMixer;
class PlaybackTimer;
class TimelineThumbnailCache;

//=============================================================================
// Timeline Playback Controller Configuration
//=============================================================================

struct TimelinePlaybackConfig {
    double scratch_duration = 1.0;          // Start at 1 second - auto-extends as clips are added
    int readAheadFrames = 108;              // Frames to prefetch ahead (~4.5s @ 24fps)
    int readBehindFrames = 12;              // Frames to keep behind for backward scrub (~0.5s @ 24fps)
    int io_threads = 8;                     // Background I/O threads
    PipelineMode pipeline_mode = PipelineMode::NORMAL;  // Video pipeline mode (8-bit/16-bit)
    std::string video_codec;                // Video codec name (for MF HDR eligibility check)
    int timecode_start_frame = 0;           // Embedded timecode start frame (0 if starts at 00:00:00:00)
};

//=============================================================================
// Timeline Playback Controller
//
// Orchestrates timeline playback using:
// - PlaybackTimer for transport control (play/pause/seek) - virtual timeline mode
// - TimelineCache for actual frame data
//
// Usage:
//   1. Call InitializeForVirtualTimeline() when loading an EDL/OTIO
//   2. Call Update() each render frame to get the current texture
//   3. Call Shutdown() when unloading the timeline
//
//=============================================================================

class TimelinePlaybackController {
public:
    TimelinePlaybackController();
    ~TimelinePlaybackController();

    // Initialize with virtual timeline (PlaybackTimer-based, no dummy video)
    // canvas_width/height: Timeline output canvas dimensions (0 = auto-detect from clips)
    bool InitializeForVirtualTimeline(TimelineView* timeline_view,
                                       ::VideoPlayer* video_player,
                                       int canvas_width = 0,
                                       int canvas_height = 0);

    // Initialize virtual scratch timeline (no dummy video)
    bool InitializeForVirtualScratchTimeline(::VideoPlayer* video_player,
                                              int width, int height, double fps);

    // Initialize cache for scratch timeline (called when first clip is added)
    // This is separate from InitializeForVirtualScratchTimeline because the cache needs
    // access to the TimelineView's flattener, which requires clips to exist
    bool InitializeCacheForScratchTimeline(TimelineView* timeline_view);

    // Shutdown and clean up
    void Shutdown();

    // Update each render frame
    // Returns texture ID (0 if not ready)
    // Sets width/height to frame dimensions
    GLuint Update(int& width, int& height);

#ifdef _WIN32
    // D3D11 rendering mode
    void SetD3D11RenderingMode(bool enabled);
    bool IsD3D11RenderingMode() const { return use_d3d11_rendering_; }

    // Update for D3D11 mode - returns SRV instead of GL texture
    ID3D11ShaderResourceView* UpdateD3D11(int& width, int& height);
#endif

    // Process pending GPU uploads (call from GL thread)
    void ProcessPendingUploads();

    // Configuration
    void SetConfig(const TimelinePlaybackConfig& config);
    TimelinePlaybackConfig GetConfig() const { return config_; }

    // Set thumbnail cache reference (for playback state notifications)
    void SetThumbnailCache(TimelineThumbnailCache* cache) { thumbnail_cache_ = cache; }

    // Loop control
    void SetLooping(bool enabled);
    bool IsLooping() const;

    // State queries
    bool IsInitialized() const { return initialized_; }
    bool IsPlaying() const;
    bool IsActuallyPlaying() const;    // True only if actually playing (for UI state)
    int GetCurrentFrame() const { return current_frame_.load(); }
    double GetDuration() const { return timeline_duration_; }
    double GetFPS() const { return fps_; }
    int GetWidth() const { return width_; }
    int GetHeight() const { return height_; }

    // Playback stride (cache-side frame skip for heavy sequences)
    void SetPlaybackStride(int stride);
    int GetPlaybackStride() const;

    // Access to cache for statistics
    TimelineCache* GetCache() const { return cache_.get(); }

    // Apply pending FPS update from video decoder (called when paused)
    void ApplyPendingFPSUpdate();

    //=========================================================================
    // Dual View Mode - Side-by-side comparison (LEFT/RIGHT tracks)
    //=========================================================================

    // Initialize for dual view mode (creates two caches, one per track)
    bool InitializeForDualView(TimelineView* timeline_view, ::VideoPlayer* video_player);

    // Check if in dual view mode
    bool IsDualViewMode() const { return dual_view_mode_; }

    // Check if using unified composite pipeline (single interop)
    bool IsUnifiedCompositePipeline() const { return use_unified_composite_; }

    // Dual view update - returns textures for both LEFT and RIGHT tracks
    // Returns gap texture for sides with no clip at current time
    struct DualViewTextures {
        // Separate textures (legacy mode - two interops)
        GLuint left_texture = 0;
        int left_width = 0;
        int left_height = 0;
        GLuint right_texture = 0;
        int right_width = 0;
        int right_height = 0;

        // Unified composite texture (new mode - single interop)
        GLuint composite_texture = 0;
        int composite_width = 0;
        int composite_height = 0;
        bool is_unified = false;  // True if using unified pipeline

        // UV coordinates for sampling left/right from composite
        float left_uv_min_x = 0.0f, left_uv_max_x = 0.5f;
        float left_uv_min_y = 0.0f, left_uv_max_y = 1.0f;
        float right_uv_min_x = 0.5f, right_uv_max_x = 1.0f;
        float right_uv_min_y = 0.0f, right_uv_max_y = 1.0f;
    };
    DualViewTextures UpdateDualView();

    // Access to right cache for statistics
    TimelineCache* GetRightCache() const { return right_cache_.get(); }

    // Sync dual flatteners with updated tracks (call after edits in dual view mode)
    void SyncDualFlatteners();

    // Access to audio mixer for volume control
    AudioMixer* GetAudioMixer() const { return audio_mixer_.get(); }


#ifdef _WIN32
    // D3D11VA HDR mode: FFmpeg + D3D11VA hardware decode (Windows only) - OLD
    bool IsD3D11VAHDRMode() const { return use_d3d11va_hdr_; }

    // D3D11 GPU-native mode: New D3D11VideoDecoder for VIDEO_FILE + ULTRA_HIGH_RES
    bool IsD3D11DecoderMode() const { return use_d3d11_decoder_; }
#endif

    // Volume/Mute control
    void SetVolume(double volume);  // 0.0 to 1.0
    void SetMuted(bool muted);
    double GetVolume() const;
    bool IsMuted() const;

    // Check if using virtual timeline mode (always true now)
    bool IsVirtualTimelineMode() const { return use_virtual_timeline_; }

    // Access timeline metadata (for screenshot naming, etc.)
    std::string GetTimelineName() const;
    std::string GetSourceFilePath() const;

    // Access timer (for external position queries)
    PlaybackTimer* GetTimer() const { return timeline_timer_.get(); }

    // Transport controls (route to timer in virtual mode)
    void Play();
    void Pause();
    void TogglePlayPause();
    void Seek(double position);
    void SeekRelative(double delta);
    void StepForward(int frames = 1);
    void StepBackward(int frames = 1);
    void GoToStart();
    void GoToEnd();

    // Get current position in seconds
    double GetPosition() const;

    // Per-frame timer update (call from render loop)
    void UpdateTimer();

    //=========================================================================
    // Fast Seek (Rewind/Fast Forward)
    //=========================================================================

    void StartRewind();
    void StartFastForward();
    void StopFastSeek();
    void UpdateFastSeek();  // Call from render loop when fast seeking
    bool IsFastSeeking() const { return is_fast_seeking_; }
    bool IsFastForward() const { return fast_forward_; }
    double GetFastSeekSpeed() const { return fast_seek_speed_; }

    //=========================================================================
    // Scrub Mode (Mouse timeline scrubbing)
    // Enables immediate single-frame seeks in video decoder
    //=========================================================================

    void SetScrubMode(bool enabled);
    bool IsScrubMode() const { return is_scrubbing_; }

    // Notify controller that timeline was edited
    // Clears stale current texture so we don't show old frames
    void NotifyTracksEdited();

    // Update timeline duration (call after edits that may change duration)
    // Automatically extends dummy video if needed
    void UpdateDuration(double new_duration);

    // Get current source clip info (for UI display)
    std::string GetCurrentClipName() const;
    std::string GetCurrentSourcePath() const;

    // Get pipeline mode for current clip (for UI display)
    PipelineMode GetCurrentPipelineMode() const;

private:
    // State
    bool initialized_ = false;
    TimelinePlaybackConfig config_;

    // External references (not owned)
    TimelineView* timeline_view_ = nullptr;
    ::VideoPlayer* video_player_ = nullptr;
    TimelineThumbnailCache* thumbnail_cache_ = nullptr;  // For playback state notifications

    // Owned components
    std::unique_ptr<TimelineCache> cache_;
    std::unique_ptr<AudioMixer> audio_mixer_;
    std::unique_ptr<PlaybackTimer> timeline_timer_;  // Virtual timeline clock

    // Dual view mode components
    bool dual_view_mode_ = false;
    bool use_unified_composite_ = false;  // True to use DualViewPipeline (single interop)
    bool unified_not_possible_ = false;   // True if one side has image sequences (skip D3D11 pipeline)
    std::unique_ptr<TimelineCache> right_cache_;       // Cache for RIGHT track (dual view only)
    std::unique_ptr<TimelineFlattener> left_flattener_;  // Flattener for LEFT track only
    std::unique_ptr<TimelineFlattener> right_flattener_; // Flattener for RIGHT track only

#ifdef _WIN32
    // Unified dual view pipeline (single interop, D3D11 compositor)
    std::unique_ptr<DualViewPipeline> dual_view_pipeline_;

    // Initialize unified composite pipeline for dual view (called from InitializeForDualView)
    void InitializeUnifiedDualViewPipeline();
#endif

    // Virtual timeline mode flag (always true now - dummy video mode removed)
    bool use_virtual_timeline_ = true;


#ifdef _WIN32
    // D3D11VA HDR mode: Use FFmpeg + D3D11VA for HDR video decode (Windows only) - OLD
    // Our D3D11 device is passed to FFmpeg for texture compatibility
    bool use_d3d11va_hdr_ = false;
    std::unique_ptr<D3D11VAVideoDecoder> d3d11va_decoder_;

    // D3D11 GPU-native mode: New D3D11VideoDecoder for VIDEO_FILE + ULTRA_HIGH_RES
    // Supports both HW decode (H.264/HEVC) and SW decode (ProRes/DNxHD)
    bool use_d3d11_decoder_ = false;
    std::unique_ptr<D3D11VideoDecoder> d3d11_decoder_;
#endif

    // Volume/mute state
    double volume_ = 1.0;  // 0.0 to 1.0
    bool muted_ = false;

    // Frame-locked timing: accumulate real time and advance by frames
    std::chrono::steady_clock::time_point last_timer_update_;
    double accumulated_time_ = 0.0;
    bool timer_initialized_ = false;

    bool d3d11va_needs_seek_ = false;  // D3D11VA: need to seek before decoding (loop restart)

    // D3D11VA safety margin: frames to subtract from frame_count to avoid hitting EOS
    // Set to 0 - frame boundaries should be handled correctly.
    static constexpr int kD3D11VASafetyMarginFrames = 0;

    // Get max source width across all clips (computed at initialization)
    int GetMaxSourceWidth() const { return max_source_width_; }

private:
    // Timeline properties
    double timeline_duration_ = 0.0;
    double fps_ = 24.0;
    int width_ = 1920;
    int height_ = 1080;

    // FPS synchronization - tracks whether we've applied the detected media FPS
    bool fps_update_applied_ = false;

    // Max source width across all clips (for resolution-based buffer thresholds)
    // Computed once at initialization, not per-frame
    int max_source_width_ = 0;

    // Playback stride stored at controller level (survives cache state changes)
    int playback_stride_ = 1;

    // Current playback state
    std::atomic<int> current_frame_{0};
    std::atomic<bool> is_playing_{false};

    // Current display texture (GL)
    GLuint current_texture_ = 0;
    int current_texture_width_ = 0;
    int current_texture_height_ = 0;

#ifdef _WIN32
    // D3D11 rendering mode
    bool use_d3d11_rendering_ = false;

    // Current display texture (D3D11)
    ID3D11ShaderResourceView* current_srv_d3d_ = nullptr;
#endif

    //=========================================================================
    // Fast Seek State
    //=========================================================================

    bool is_fast_seeking_ = false;
    bool fast_forward_ = true;      // true = forward, false = rewind
    double fast_seek_speed_ = 1.0;  // Current speed multiplier (increases over time)
    std::chrono::steady_clock::time_point fast_seek_start_time_;
    std::chrono::steady_clock::time_point last_fast_seek_update_;

    // Fast seek speed configuration
    static constexpr double kFastSeekInitialSpeed = 2.0;   // Start at 2x
    static constexpr double kFastSeekMaxSpeed = 32.0;      // Cap at 32x
    static constexpr double kFastSeekAcceleration = 2.0;   // Double speed every second

    //=========================================================================
    // Scrub Mode State (Mouse scrubbing on timeline)
    //=========================================================================

    bool is_scrubbing_ = false;

    // Post-edit state: Hold old texture briefly until new frame arrives
    // This prevents black flashing while new frame loads
    GLuint pending_evict_texture_ = 0;
    int pending_evict_width_ = 0;
    int pending_evict_height_ = 0;
    bool awaiting_post_edit_frame_ = false;
    std::chrono::steady_clock::time_point post_edit_start_time_;

    // Debug: track first N updates after play starts to diagnose frame jumps
    int debug_post_play_updates_ = 0;
    static constexpr int kDebugPostPlayLogCount = 10;
};

} // namespace qcview
