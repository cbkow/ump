#pragma once

// Fix Windows min/max macro conflicts with OpenEXR
#ifdef WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#undef min
#undef max
#endif

#include <mpv/client.h>
#include <mpv/render_gl.h>
#include <glad/gl.h>

#include <chrono>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "../metadata/video_metadata.h"
#include "../utils/gpu_scheduler.h"
#include "../color/ocio_pipeline.h"
#include "../overlay/safety_overlay_system.h"
#include "../overlay/svg_overlay_renderer.h"
// Note: DummyVideoGenerator removed - image sequences and timeline now use PlaybackTimer-based virtual timeline
#include "direct_exr_cache.h"           // Direct EXR cache (100% OpenEXR)
#include "dual_view_types.h"            // DualViewTimeline for dual view editing
#include "playback_timer.h"             // Manual playback timer for virtual timelines

namespace ump {
    struct Sequence;
    // DirectEXRCacheConfig defined in direct_exr_cache.h
    // ThumbnailConfig and ThumbnailCache defined in thumbnail_cache.h
    struct ThumbnailConfig;
    class ThumbnailCache;
    class ComparisonVideoPlayer;
    class TranscodeManager;  // NEW: For difference mode transcoding
    class DifferenceCache;   // NEW: For difference mode caching
    class TimelinePlaybackController;  // Timeline mode playback
    class AudioPlayer;       // Independent audio playback for dual view mode
}

#include "pipeline_mode.h"

// Comparison mode for dual video review
enum class ComparisonMode {
    DISABLED,                   // Single video (current behavior)
    SIDE_BY_SIDE,               // Split viewport 50/50 (scaled down videos) - LEGACY, kept for setup UI
    SPLIT_SCREEN,               // Full-screen split with draggable divider - LEGACY, kept for setup UI
    DIFFERENCE_VIEW,            // OpenGL shader composite showing pixel differences - LEGACY
    SPLIT_HORIZONTAL,           // Lavfi: horizontal stack (side-by-side)
    SPLIT_VERTICAL,             // Lavfi: vertical stack (top/bottom)
    SPLIT_5050_HORIZONTAL,      // Lavfi: 50/50 split with adjustable divider
    DIFFERENCE_BLEND            // Lavfi: real-time difference blend
};

// Gap state for virtual timeline playback
enum class ClipGapState {
    PLAYING,        // Within clip range, normal playback
    GAP_BEFORE,     // Timeline position is before clip starts
    GAP_AFTER       // Timeline position is after clip ends
};

// Global pipeline configurations
extern const std::map<PipelineMode, PipelineConfig> PIPELINE_CONFIGS;

// Helper functions
const char* PipelineModeToString(PipelineMode mode);
PipelineMode StringToPipelineMode(const std::string& mode_str);
size_t CalculateCacheMemoryUsage(int width, int height, PipelineMode mode, size_t frame_count);

// Global configuration accessor (updated for new cache)
ump::DirectEXRCacheConfig GetCurrentEXRCacheConfig();
ump::ThumbnailConfig GetCurrentThumbnailConfig();

class VideoPlayer {
public:
    // Constructor/Destructor
    VideoPlayer();
    ~VideoPlayer();

    // Core lifecycle
    bool Initialize();
    void Cleanup();

    // Playback control
    void LoadFile(const std::string& path);
    bool LoadFileTrimmed(const std::string& path, double in_point, double out_point);
    void LoadPlaylist(const std::string& edl_content);
    void LoadSequence(const ump::Sequence& sequence);
    void InitializeForEmptySequence(double default_duration = 60.0);
    void OnPlaylistItemChanged(const std::string& new_file_path);  // Handle playlist item changes without full reload

    // Image sequence support
    void SetMFFrameRate(double fps);
    void SetImageSequenceFrameRate(double fps, int start_frame = 1);  // Store FPS and start frame for image sequences

    // EXR custom data feeding (legacy - kept for potential future use)
    bool FeedEXRFrame(const void* rgba_data, int width, int height, double timestamp);
    bool CopyTextureForPlayback(GLuint source_texture, int width, int height);
    bool ProcessAndFeedEXRFrame(int frame_index);

    // Image sequence loading (uses PlaybackTimer virtual timeline, no dummy videos)
    // cached_duration: If > 0, use this duration instead of calculating from file count (for reliable reload)
    // initial_playhead: If >= 0, start cache from this position instead of beginning
    bool LoadEXRSequenceWithDummy(const std::vector<std::string>& sequence_files, const std::string& layer_name, double fps,
                                 int cached_width = 0, int cached_height = 0, double cached_duration = 0.0,
                                 double initial_playhead = 0.0);

    // Universal image sequence loading (TIFF/PNG/JPEG with DirectEXRCache)
    // cached_duration: If > 0, use this duration instead of calculating from file count (for reliable reload)
    // initial_playhead: If >= 0, start cache from this position instead of beginning
    bool LoadImageSequenceWithCache(const std::vector<std::string>& sequence_files, double fps, PipelineMode pipeline_mode,
                                   int cached_width = 0, int cached_height = 0, double cached_duration = 0.0,
                                   double initial_playhead = 0.0);

    // Image sequence timer initialization (virtual timeline mode)
    bool InitializeImageSequenceTimer(double duration, double fps);

    // EXR frame synchronization helpers
    int CalculateCurrentEXRFrameIndex() const;
    void InjectCurrentEXRFrame();
    void RenderEXRFrameOverlay(int frame_index);
    void TriggerEXRFrameCaching(); // Cache current frame after EXR injection

    // Timeline mode frame injection (matches EXR pattern for smooth playback)
    void SetTimelineMode(bool enabled, ump::TimelinePlaybackController* controller = nullptr);
    bool IsInTimelineMode() const { return is_timeline_mode_; }
    void InjectCurrentTimelineFrame();

    // Content dimensions (for overlay modes - EXR/image sequences/timeline)
    // Used with PlaybackTimer virtual timeline for correct content dimensions
    void SetContentDimensions(int width, int height);

    void Play();
    void Pause();
    void Stop();

    // Seeking and navigation
    void Seek(double position);
    void SeekToFrame(int frame_number);
    void StepFrame(int direction);
    void GoToStart();
    void GoToEnd();

    // Fast seeking
    void StartFastForward();
    void StartRewind();
    void StopFastSeek();
    void UpdateFastSeek();
    bool IsFastSeeking() const { return is_fast_seeking; }
    bool IsFastForward() const { return fast_forward; }
    int GetFastSeekSpeed() const { return fast_seek_speed; }

    // Audio control
    void SetVolume(int vol);        // Legacy int version
    void SetVolume(float volume);   // New float version
    float GetVolume() const;

    // Loop control
    void SetLoop(bool enabled);
    void SetLoopMode(bool is_playlist_mode);
    bool IsLooping() const { return loop_enabled; }

    // Rendering
    void RenderVideoFrame();
    void RenderControls();

    // State queries
    bool IsPlaying() const { return is_playing; }
    bool HasVideo() const { return mpv != nullptr && cached_duration > 0; }
    bool HasAudio() const;
    bool HasValidTexture() const { return has_video && video_texture != 0; }

    // Image sequence queries
    bool IsImageSequence() const;
    int GetImageSequenceStartFrame() const;
    bool IsAudioOnly() const { return HasAudio() && !has_video; }
    bool IsReadyToRender() const;
    bool IsRenderInfrastructureReady() const;

    // Video properties
    double GetPosition() const;
    double GetDuration() const;
    int GetVideoWidth() const { return video_width; }
    int GetVideoHeight() const { return video_height; }
    double GetFrameRate() const;
    int GetTotalFrames() const;
    int GetCurrentFrame() const;

    // File information
    std::string GetFilePath() const { return current_file_path; }
    std::string GetVideoCodec() const;
    std::string GetPixelFormat() const;
    double GetVideoBitrate() const;
    int64_t GetFileSize() const;
    double ProbeFileDuration(const std::string& file_path);

    // Color information
    std::string GetColorspace() const;
    std::string GetColorPrimaries() const;
    std::string GetColorTrc() const;
    std::string GetColorRange() const;  // NEW: Color range extraction

    // Audio information
    std::string GetAudioCodec() const;
    int GetSampleRate() const;
    int GetAudioChannels() const;
    double GetAudioBitrate() const;

    // Metadata (kept for legacy compatibility, but prefer FFmpegMetadataExtractor for new code)
    VideoMetadata ExtractMetadata() const;  // Legacy: queries MPV (prefer FFmpegMetadataExtractor)
    VideoMetadata ExtractEXRMetadata(const std::vector<std::string>& sequence_files,
                                    const std::string& layer_name,
                                    double fps) const;  // EXR-specific metadata extraction
    std::string GetMetadata(const std::string& key) const;

    // Utility functions
    std::string FormatTimecode(double seconds, double fps = 23.976) const;

    // Event handling and callbacks
    void SetPlaylistPositionCallback(std::function<void()> callback) {
        playlist_position_callback = callback;
    }
    void SetEXRCachingCallback(std::function<bool(VideoPlayer*)> callback) {
        exr_caching_callback = callback;
    }
    void SetCacheClearCallback(std::function<void()> callback) {
        cache_clear_callback = callback;
    }
    void SetDimensionChangeCallback(std::function<void(int, int)> callback) {
        dimension_change_callback = callback;
    }
    void SetMetadataCallback(std::function<VideoMetadata(const std::string&)> callback) {
        metadata_callback = callback;
    }
    void SetupPropertyObservation();
    void UpdateFromMPVEvents();
    void Reset() {
        Stop();
        has_video = false;
        cached_duration = 0.0;
        cached_position = 0.0;
    }

    void UpdateVideoTexture(); 
    void DebugTextureInfo();

    // Direct MPV access (use with caution)
    mpv_handle* GetMPVHandle() const { return mpv; }

    // GPU Cache integration
    GLuint GetCurrentVideoTexture() const { return video_texture; }

    // NEW: Cache system accessors
    bool IsInEXRMode() const { return is_exr_mode; }
    std::string GetImageSequenceFormat() const { return image_sequence_format; }
    const std::vector<std::string>& GetEXRSequenceFiles() const { return exr_sequence_files; }
    const std::string& GetEXRLayerName() const { return exr_layer_name; }
    double GetEXRFrameRate() const { return exr_frame_rate; }
    int GetEXRSequenceStartFrame() const { return exr_sequence_start_frame; }
    bool IsCurrentFrameReady() const { return HasValidTexture(); }
    ump::DirectEXRCache* GetEXRCache() const { return exr_cache_.get(); }
    void SetEXRPosition(double timestamp) { cached_position = timestamp; }  // For timeline scrubbing
    // Removed: EnableOpportunisticCaching() (using only spiral background caching)

    // OCIO pipeline
    void SetColorPipeline(std::unique_ptr<OCIOPipeline> pipeline);
    void ClearColorPipeline();
    bool HasColorPipeline() const { return color_pipeline && color_pipeline->IsValid(); }
    void ForceFrameRefresh(); // Force re-render current frame with current color pipeline

    // Render any texture with OCIO color transformation to current framebuffer
    void RenderTextureWithOCIO(GLuint texture_id, int tex_width, int tex_height,
                               int viewport_x, int viewport_y, int viewport_width, int viewport_height);

    // Create color-corrected texture from input texture using OCIO pipeline
    // Returns 0 if OCIO pipeline not available or if creation fails
    GLuint CreateColorCorrectedTexture(GLuint input_texture_id, int tex_width, int tex_height,
                                       int output_width, int output_height);

    // Pipeline Mode System
    void SetPipelineMode(PipelineMode mode);
    PipelineMode GetPipelineMode() const { return current_pipeline_mode; }
    const PipelineConfig& GetCurrentPipelineConfig() const;
    bool SupportsPipelineMode(PipelineMode mode) const;

    // Pipeline-aware texture creation (replaces hardcoded RGBA8)
    void CreateVideoTexturesForMode(int width, int height, PipelineMode mode);
    void CreateColorProcessingResourcesForMode(int width, int height, PipelineMode mode);

    // Cache size recommendations based on current mode
    size_t GetRecommendedCacheSize() const;
    size_t GetMaxCacheSize() const;

    // Safety overlay system
    void InitializeSafetyOverlays();
    void SetSafetyOverlaySettings(const SafetyGuideSettings& settings);
    SafetyGuideSettings GetSafetyOverlaySettings() const;
    void EnableSafetyOverlays(bool enabled);
    bool IsSafetyOverlaysEnabled() const;
    bool HasSafetyOverlays() const;

    // SVG overlay rendering (renders directly over video using ImGui)
    void RenderSVGOverlays(ImDrawList* draw_list, ImVec2 video_pos, ImVec2 video_size,
                          float opacity = 0.7f, ImU32 color = IM_COL32(255, 255, 255, 255),
                          float line_width = 2.0f);

    // Get SVG overlay renderer for UI access
    SVGOverlayRenderer* GetSVGRenderer() const { return svg_overlay_renderer.get(); }


    // EXR Cache functionality (NEW: Using DirectEXRCache)
    // initial_position: If >= 0, start caching from this position; if < 0, use current playhead
    void InitializeEXRCache(const std::vector<std::string>& sequence_files,
                           const std::string& layer_name, double fps,
                           double initial_position = -1.0);
    void SetEXRCacheWindow(double seconds);
    void SetEXRCacheConfig(const ump::DirectEXRCacheConfig& config);
    void SetEXRCacheEnabled(bool enabled);
    void ClearEXRCache();
    void ClearVideoTextureReference();  // Clear before cache reinitialization to prevent dangling refs
    ump::DirectEXRCache::CacheStats GetEXRCacheStats() const;
    bool HasEXRCache() const;
    std::vector<ump::CacheSegment> GetEXRCacheSegments() const;

    // Thumbnail Cache (for timeline scrubbing)
    GLuint GetThumbnailForFrame(int frame, bool allow_fallback = false, int* out_actual_frame = nullptr);  // Get thumbnail for specific frame (0 if not available)
    bool GetThumbnailSize(int frame, int& width, int& height) const;  // Get actual dimensions of cached thumbnail
    bool HasThumbnailCache() const;
    void ClearThumbnailCache();
    ump::ThumbnailCache* GetThumbnailCache() const { return thumbnail_cache_.get(); }

    // Disk cache settings (for EXRTranscoder)
    void SetCacheSettings(const std::string& custom_path, int retention_days,
                         int transcode_max_gb, bool clear_on_exit);

    // Clear EXR disk caches (dummies + transcodes)
    // Returns total bytes deleted
    size_t ClearEXRDiskCache();

    // OIIO removed - EXR-only with DirectEXRCache

    // Dual Video Review / Comparison Mode
    void EnableComparisonMode(bool enabled);
    bool IsComparisonModeEnabled() const;
    ump::ComparisonVideoPlayer* GetComparisonPlayer() const { return comparison_video_.get(); }
    void LoadComparisonVideo(const std::string& path);
    void LoadPrimaryVideoInDualView(const std::string& path);  // Load primary video while staying in dual view
    void UnloadComparisonVideo();
    bool HasComparisonVideo() const;
    void SetComparisonMode(ComparisonMode mode);
    ComparisonMode GetComparisonMode() const;
    std::string GetComparisonVideoPath() const;
    std::string GetPendingComparisonDrop();  // Returns and clears pending media ID
    void ClearPendingComparisonDrop();
    std::string GetPendingViewportDrop();  // Returns and clears pending media ID for main viewport
    void ClearPendingViewportDrop();

    // Lavfi-based comparison mode (NEW)
    void LoadLavfiComparison(const std::string& primary_path, const std::string& secondary_path,
                            ComparisonMode mode, int viewport_width, int viewport_height);
    void UpdateLavfiFilter(ComparisonMode mode, int viewport_width, int viewport_height);
    void TransitionToLavfiMode(ComparisonMode lavfi_mode, int viewport_width, int viewport_height,
                              const std::string& primary_override = "", const std::string& secondary_override = "",
                              bool use_secondary_as_reference = false);  // Transition from setup to lavfi
    void SetPrimaryTrimPoints(double start, double duration);  // Per-comparison trim for primary video
    void SetSecondaryTrimPoints(double start, double duration);  // Per-comparison trim for secondary video
    void ClearPrimaryTrimPoints();
    void ClearSecondaryTrimPoints();
    bool IsLavfiMode(ComparisonMode mode) const;
    std::string GetSecondaryVideoPath() const { return lavfi_secondary_path_; }
    std::string GetCurrentLavfiFilter() const { return current_lavfi_filter_; }  // For debugging
    void ExitLavfiMode();  // Clean exit from lavfi mode (recreates MPV)

    // Dual view mode memory (for auto-revert from lavfi)
    ComparisonMode GetLastEditMode() const { return last_edit_mode_; }
    void RevertToEditMode();  // Revert from lavfi to last edit mode (side-by-side or split)

    // Dual view timeline data
    ump::DualViewTimeline& GetDualViewTimeline() { return dual_view_timeline_; }
    const ump::DualViewTimeline& GetDualViewTimeline() const { return dual_view_timeline_; }
    void UpdateDualViewClipFromPrimary();    // Sync left clip from primary video
    void UpdateDualViewClipFromSecondary();  // Sync right clip from secondary video

    // Calculate secondary video position accounting for position offset (slip)
    // Returns the source position for secondary video given primary timeline position
    double CalculateSecondaryPosition(double timeline_position) const;

    // Calculate primary video position accounting for position offset
    // Returns the source position for primary video given virtual timeline position
    double CalculatePrimaryPosition(double timeline_position) const;

    // Get gap state for primary (left) clip at given timeline position
    ClipGapState GetPrimaryGapState(double timeline_position) const;

    // Get gap state for secondary (right) clip at given timeline position
    ClipGapState GetSecondaryGapState(double timeline_position) const;

    // Get the virtual timeline duration (max of both clips' ends)
    double GetVirtualTimelineDuration() const;

    // Seek both videos based on virtual timeline position (handles offsets)
    void SeekDualView(double timeline_position);

    // Get current virtual timeline position (from timer if active, else calculated)
    double GetVirtualTimelinePosition() const;

    // Initialize dual view mode with manual timer for playback control
    bool InitializeDualViewPlayback();

    // Check if dual view timer is active
    bool IsDualViewTimerActive() const { return dual_view_timer_ != nullptr; }

    // Called when virtual timeline duration changes (clip trim/slide)
    void OnVirtualTimelineDurationChanged();

    // Update dual view timer (call every frame when in dual view mode)
    void UpdateDualViewTimer();

    // Get the dual view timer (for external control if needed)
    ump::PlaybackTimer* GetDualViewTimer() const { return dual_view_timer_.get(); }

    // Get the dual view audio player (for volume control, etc.)
    ump::AudioPlayer* GetDualViewAudio() const { return dual_view_audio_.get(); }

    // Screenshot functionality - captures final rendered frame with all FBO processing
    bool CaptureScreenshotToClipboard();
    bool CaptureScreenshotToDesktop(const std::string& filename = "");
    bool CaptureScreenshotToPath(const std::string& directory_path, const std::string& filename);

private:
    // MPV core
    mpv_handle* mpv;
    mpv_render_context* mpv_gl;

    // OpenGL resources
    GLuint video_texture;
    GLuint fbo;

    // NEW: Separate MPV rendering resources to break pipeline stalls
    GLuint mpv_fbo = 0;
    GLuint mpv_texture = 0;

    CooperativeGPUScheduler video_gpu_scheduler;

    // Color processing
    GLuint color_fbo = 0;
    GLuint color_texture = 0;
    int color_texture_width_ = 0;
    int color_texture_height_ = 0;
    GLuint quad_vao = 0;
    GLuint quad_vbo = 0;

    // Video properties
    int video_width;
    int video_height;

    // Content dimensions (independent of dummy video for overlay modes)
    // Set from MediaItem cache (EXR/image sequences) or timeline controller
    int content_width_ = 0;
    int content_height_ = 0;
    bool use_content_dimensions_ = false;  // True when in overlay mode

    double cached_position = 0.0;
    double cached_duration = 0.0;
    double cached_fps = 23.976;
    double position;
    double duration;

    // Playback state
    bool is_playing;
    bool has_video;
    bool is_slider_dragging = false;
    int volume;

    // Loop control
    bool loop_enabled = true;
    bool is_playlist_loop_mode = false;

    // Fast seeking state
    bool is_fast_seeking = false;
    bool fast_forward = false;  // true = FF, false = RW
    std::chrono::steady_clock::time_point fast_seek_start;
    int fast_seek_speed = 1;    // Multiplier for seek speed

    // Playlist management
    std::function<void()> playlist_position_callback;
    int last_known_playlist_pos = -1;

    // EXR caching callback
    std::function<bool(VideoPlayer*)> exr_caching_callback;

    // Cache clear callback (for evicting video cache when switching to EXR, or vice versa)
    std::function<void()> cache_clear_callback;

    // Dimension change callback (for notifying UI of video size changes)
    std::function<void(int, int)> dimension_change_callback;

    // Metadata callback (for retrieving cached FFmpeg metadata)
    std::function<VideoMetadata(const std::string&)> metadata_callback;

    // Cached data
    std::unique_ptr<VideoMetadata> cached_metadata;

    // Configuration methods
    void ConfigureBasicOptions();
    void ConfigureVideoOptions(bool lavfi_mode = false);
    void ConfigureAudioOptions();
    void ConfigureSeekingOptions();
    void ConfigureCacheOptions(bool lavfi_mode = false);
    void ConfigureHardwareDecoding(bool lavfi_mode = false);
    bool SetupOpenGL();
    void CreateVideoTextures(int width, int height);
    void CreateTransitionPlaceholder();
    void ClearVideoTextureToBackground();  // Clear video FBO to background color (for transitions)
    void ClearColorTextureToBackground();  // Clear color FBO to background color (for transitions)

    // Playback mode configuration
    void ConfigureForSingleFile();
    void ConfigureForPlaylist();
    void ApplyRenderOptimizations();

    // Rendering helpers
    void RenderVideoTexture();
    void RenderPlaceholder();
    void UpdateProperties();  // Updates both static and dynamic properties (call once on load)
    void UpdatePlaybackState();  // Updates only dynamic properties (position, pause) - call every frame

    // Event handling
    void HandleMPVEvent(mpv_event* event);
    void HandlePropertyChange(const std::string& prop_name, mpv_event_property* prop);

    // File loading helpers
    void ResetState();
    void WaitForFileLoad(bool is_audio_file = false);
    void FinalizeLoad();
    std::vector<std::string> ParseEDLContent(const std::string& edl_content);
    void LoadPlaylistFiles(const std::vector<std::string>& file_paths);

    static void* GetProcAddress(void* ctx, const char* name);

    // OCIO pipeline
    std::unique_ptr<OCIOPipeline> color_pipeline;

    void SetupColorProcessingResources();
    void ApplyColorPipeline();

    // Pipeline Mode System
    PipelineMode current_pipeline_mode = PipelineMode::NORMAL;
    GLenum current_internal_format = GL_RGBA8;  // Cached format to avoid map lookups every frame
    void ApplyPipelineModeConfig(PipelineMode mode);
    bool SetupOpenGLForMode(PipelineMode mode);

    // Safety overlay system
    std::unique_ptr<SafetyOverlaySystem> safety_overlay_system;

    // SVG overlay renderer for simple guide overlays
    std::unique_ptr<SVGOverlayRenderer> svg_overlay_renderer;
    bool svg_overlays_enabled = false;

    // Removed: opportunistic_caching_enabled (using only spiral background caching)

    // EXR sequence handling
    bool is_exr_mode = false;
    std::string exr_sequence_path;
    std::string exr_layer_name;
    std::string image_sequence_format;  // "EXR", "PNG", "JPEG", "TIFF", etc.

    // Current file path for cached metadata lookups
    std::string current_file_path;
    double exr_frame_rate = 24.0;
    int exr_current_frame = 0;
    int exr_frame_count = 0;
    int exr_sequence_start_frame = 0;  // First frame number in EXR sequence files

    // Image sequence frame rate storage
    double image_sequence_frame_rate = 24.0;
    bool is_image_sequence = false;
    int image_sequence_start_frame = 1;
    std::vector<std::string> exr_sequence_files;

    // Timeline mode handling (matches EXR pattern)
    bool is_timeline_mode_ = false;
    ump::TimelinePlaybackController* timeline_controller_ = nullptr;
    GLuint timeline_texture_ = 0;       // Current timeline frame texture
    int timeline_texture_width_ = 0;
    int timeline_texture_height_ = 0;
    int last_timeline_frame_ = -1;      // For change detection
    GLuint gap_placeholder_texture_ = 0; // Black texture for timeline gaps/cache misses
    int gap_placeholder_width_ = 0;      // Track dimensions to avoid constant resize
    int gap_placeholder_height_ = 0;

    // Transition placeholder texture (shown during media switches to prevent font cache flicker)
    GLuint transition_placeholder_texture_ = 0;
    int transition_placeholder_width_ = 64;
    int transition_placeholder_height_ = 64;

    // Image sequences removed - will be re-added with different libraries later

    // EXR texture management
    GLuint exr_texture = 0;
    int exr_texture_width = 0;
    int exr_texture_height = 0;

    // EXR Background Cache (NEW: DirectEXRCache)
    std::shared_ptr<ump::DirectEXRCache> exr_cache_;

    // Thumbnail Cache (for timeline scrubbing)
    std::unique_ptr<ump::ThumbnailCache> thumbnail_cache_;

    // Dual Video Review / Comparison Mode
    std::unique_ptr<ump::ComparisonVideoPlayer> comparison_video_;
    ComparisonMode comparison_mode_ = ComparisonMode::DISABLED;
    ComparisonMode last_edit_mode_ = ComparisonMode::SIDE_BY_SIDE;  // Last non-lavfi mode for auto-revert
    bool comparison_mode_enabled_ = false;
    ump::DualViewTimeline dual_view_timeline_;  // Timeline data for dual view editing
    std::unique_ptr<ump::PlaybackTimer> dual_view_timer_;  // Manual timer for virtual timeline playback
    std::unique_ptr<ump::AudioPlayer> dual_view_audio_;    // Independent audio for dual view mode (left video)
    std::unique_ptr<ump::PlaybackTimer> image_sequence_timer_;  // Timer for image sequence playback (no dummy video)
    std::string comparison_drop_pending_id_;  // Pending media ID from drag-drop (comparison video)
    std::string viewport_drop_pending_id_;  // Pending media ID from drag-drop (main viewport)
    std::string original_video_path_before_difference_;  // Stores original video to restore when exiting difference mode
    float split_screen_position_ = 0.5f;  // Split position (0.0 = left, 1.0 = right, 0.5 = center)
    bool is_dragging_split_ = false;  // True when user is dragging the split divider
    double last_seek_time_ = 0.0;  // For debounced seek sync
    bool was_playing_before_seek_ = false;  // Restore play state after seek
    double last_position_ = 0.0;  // For loop detection
    double loop_sync_time_ = 0.0;  // Timer for loop sync delay
    bool loop_sync_pending_ = false;  // Waiting for loop sync to settle

    // Lavfi-based comparison mode state (NEW)
    std::string lavfi_primary_path_;          // Primary video path for lavfi mode
    std::string lavfi_secondary_path_;        // Secondary video path (loaded via --external-file)
    double primary_trim_start_ = -1.0;        // Per-comparison trim for primary (-1 = no trim)
    double primary_trim_duration_ = -1.0;     // Per-comparison trim duration
    double secondary_trim_start_ = -1.0;      // Per-comparison trim for secondary
    double secondary_trim_duration_ = -1.0;   // Per-comparison trim duration
    int primary_video_width_ = 0;             // Cached dimensions for filter generation
    int primary_video_height_ = 0;
    int secondary_video_width_ = 0;
    int secondary_video_height_ = 0;
    double lavfi_secondary_fps_ = 0.0;        // Cached secondary FPS for lavfi mode
    double lavfi_secondary_duration_ = 0.0;   // Cached secondary duration for lavfi mode
    bool primary_has_audio_ = true;           // Whether primary video has audio track
    std::string current_lavfi_filter_;        // Current lavfi filter string

    // Difference compositor (old FBO-based - kept for fallback)
    GLuint difference_shader_program_ = 0;
    GLuint difference_fbo_ = 0;
    GLuint difference_texture_ = 0;
    int difference_texture_width_ = 0;
    int difference_texture_height_ = 0;
    float difference_amplification_ = 5.0f;  // Amplify subtle differences

    // NEW: Disk-based transcoding for frame-accurate difference mode
    std::unique_ptr<ump::TranscodeManager> transcode_manager_;
    std::unique_ptr<ump::DifferenceCache> difference_cache_;
    bool transcoding_in_progress_ = false;
    void OnTranscodeComplete();
    void StartTranscoding();

    // Comparison rendering helpers
    void RenderSideBySide();
    void RenderSplitScreen();
    void RenderDifference();
    void SetupDifferenceCompositor();
    void CleanupDifferenceCompositor();
    GLuint CompileDifferenceShader();
    ImVec2 CalculateFitSize(int source_w, int source_h, float max_w, float max_h) const;
    void RenderDropTargetPlaceholder(float width, float height);
    void RenderPrimaryDropTargetPlaceholder(float width, float height);
    GLuint GetDisplayTexture() const;
    void RenderMPVToCurrentFBO(mpv_render_context* ctx, int width, int height);  // Get current display texture (with OCIO/overlays applied)

    // OIIO removed - EXR-only support

};