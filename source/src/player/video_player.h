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
#include "dummy_video_generator.h"
#include "direct_exr_cache.h"           // tlRender-style direct EXR cache (100% OpenEXR)

namespace ump {
    struct Sequence;
    // DirectEXRCacheConfig defined in direct_exr_cache.h
    // ThumbnailConfig and ThumbnailCache defined in thumbnail_cache.h
    struct ThumbnailConfig;
    class ThumbnailCache;
}

#include "pipeline_mode.h"

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
    void LoadPlaylist(const std::string& edl_content);
    void LoadSequence(const ump::Sequence& sequence);
    void InitializeForEmptySequence(double default_duration = 60.0);
    void OnPlaylistItemChanged(const std::string& new_file_path);  // Handle playlist item changes without full reload

    // Image sequence support
    void SetMFFrameRate(double fps);
    void SetImageSequenceFrameRate(double fps, int start_frame = 1);  // Store FPS and start frame for image sequences

    // EXR custom data feeding
    bool LoadEXRSequence(const std::string& sequence_path, const std::string& layer_name, double fps, const std::vector<std::string>& sequence_files);
    bool FeedEXRFrame(const void* rgba_data, int width, int height, double timestamp);
    bool CopyTextureForPlayback(GLuint source_texture, int width, int height);
    bool ProcessAndFeedEXRFrame(int frame_index);

    // Hybrid dummy video + OpenGL overlay approach
    bool LoadEXRSequenceWithDummy(const std::vector<std::string>& sequence_files, const std::string& layer_name, double fps);
    bool LoadEXRSequenceWithShader(const std::vector<std::string>& sequence_files, const std::string& layer_name, double fps);

    // NEW: Universal image sequence loading (TIFF/PNG/JPEG with DirectEXRCache)
    bool LoadImageSequenceWithCache(const std::vector<std::string>& sequence_files, double fps, PipelineMode pipeline_mode);

    bool TestDummyVideoGeneration(int width = 1920, int height = 1080, double fps = 24.0);

    // EXR frame synchronization helpers
    int CalculateCurrentEXRFrameIndex() const;
    void InjectCurrentEXRFrame();
    void RenderEXRFrameOverlay(int frame_index);
    void TriggerEXRFrameCaching(); // Cache current frame after EXR injection

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
    float GetAudioLevel() const;    // Get current audio level for visualization

    // Real-time audio data extraction
    void SetupAudioVisualization();
    void UpdateAudioData();

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

    // Metadata
    VideoMetadata ExtractMetadata() const;
    VideoMetadata ExtractMetadataFast() const;  // Optimized version for background processing
    VideoMetadata ExtractCriticalMetadata() const;  // NEW: Minimal extraction for cache initialization only (6 properties)
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
    void InitializeEXRCache(const std::vector<std::string>& sequence_files,
                           const std::string& layer_name, double fps);
    void SetEXRCacheWindow(double seconds);
    void SetEXRCacheConfig(const ump::DirectEXRCacheConfig& config);
    void SetEXRCacheEnabled(bool enabled);
    void ClearEXRCache();
    ump::DirectEXRCache::CacheStats GetEXRCacheStats() const;
    bool HasEXRCache() const;
    std::vector<ump::CacheSegment> GetEXRCacheSegments() const;

    // Thumbnail Cache (for timeline scrubbing)
    GLuint GetThumbnailForFrame(int frame, bool allow_fallback = false);  // Get thumbnail for specific frame (0 if not available)
    bool HasThumbnailCache() const;
    void ClearThumbnailCache();
    ump::ThumbnailCache* GetThumbnailCache() const { return thumbnail_cache_.get(); }

    // Disk cache settings (for DummyVideoGenerator and EXRTranscoder)
    void SetCacheSettings(const std::string& custom_path, int retention_days,
                         int dummy_max_gb, int transcode_max_gb, bool clear_on_exit);

    // Clear EXR disk caches (dummies + transcodes)
    // Returns total bytes deleted
    size_t ClearEXRDiskCache();

    // OIIO removed - EXR-only with DirectEXRCache

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
    GLuint quad_vao = 0;
    GLuint quad_vbo = 0;

    // Video properties
    int video_width;
    int video_height;
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

    // Cached data
    std::unique_ptr<VideoMetadata> cached_metadata;

    // Configuration methods
    void ConfigureBasicOptions();
    void ConfigureVideoOptions();
    void ConfigureAudioOptions();
    void ConfigureSeekingOptions();
    void ConfigureCacheOptions();
    void ConfigureHardwareDecoding();
    bool SetupOpenGL();
    void CreateVideoTextures(int width, int height);

    // Playback mode configuration
    void ConfigureForSingleFile();
    void ConfigureForPlaylist();
    void ApplyRenderOptimizations();

    // Rendering helpers
    void RenderVideoTexture();
    void RenderPlaceholder();
    void UpdateProperties();

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

    // Real-time audio data
    bool audio_visualization_enabled = false;
    float current_audio_level = 0.0f;
    std::chrono::steady_clock::time_point last_audio_update;

    // EXR sequence handling
    bool is_exr_mode = false;
    std::string exr_sequence_path;
    std::string exr_layer_name;
    double exr_frame_rate = 24.0;
    int exr_current_frame = 0;
    int exr_frame_count = 0;
    int exr_sequence_start_frame = 0;  // First frame number in EXR sequence files

    // Image sequence frame rate storage
    double image_sequence_frame_rate = 24.0;
    bool is_image_sequence = false;
    int image_sequence_start_frame = 1;
    std::vector<std::string> exr_sequence_files;

    // Image sequences removed - will be re-added with different libraries later

    // Dummy video generation for shader injection
    ump::DummyVideoGenerator dummy_generator;

    // EXR texture management
    GLuint exr_texture = 0;
    int exr_texture_width = 0;
    int exr_texture_height = 0;

    // EXR Background Cache (NEW: DirectEXRCache)
    std::shared_ptr<ump::DirectEXRCache> exr_cache_;

    // Thumbnail Cache (for timeline scrubbing)
    std::unique_ptr<ump::ThumbnailCache> thumbnail_cache_;

    // OIIO removed - EXR-only support

};