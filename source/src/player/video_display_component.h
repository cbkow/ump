#pragma once

// Fix Windows min/max macro conflicts with OpenEXR
#ifdef WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#undef min
#undef max
#endif

#include <glad/gl.h>
#include <imgui.h>

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#ifdef _WIN32
#include <d3d11_1.h>
#include <wrl/client.h>
#endif

#include "pipeline_mode.h"
#include "dual_view_types.h"
#include "../overlay/safety_overlay_system.h"
#include "../overlay/svg_overlay_renderer.h"

// Include actual headers to avoid forward declaration conflicts
#include "direct_exr_cache.h"

// Forward declarations for MPV types
struct mpv_handle;
struct mpv_render_context;

// Forward declarations
class OCIOPipeline;

#ifdef _WIN32
namespace ump {
    class D3D11OCIORenderer;
}
#endif

namespace ump {
    class ThumbnailCache;
    class TimelinePlaybackController;
    class AudioPlayer;
    struct ThumbnailConfig;
}

/**
 * VideoDisplayComponent - Minimal video display layer
 *
 * This class provides:
 * - OpenGL texture management for video display
 * - OCIO color pipeline integration
 * - Safety/SVG overlay rendering
 * - Timeline frame injection (receives frames from TimelinePlaybackController)
 * - EXR sequence cache integration
 * - Screenshot capture
 * - Passthrough playback controls
 *
 * It does NOT:
 * - Load media files (that's TimelinePlaybackController's job)
 * - Manage MPV or any decoder
 * - Handle comparison/dual view modes
 * - Parse playlists or EDL files
 */
class VideoDisplayComponent {
public:
    VideoDisplayComponent();
    ~VideoDisplayComponent();

    //=========================================================================
    // Core Lifecycle
    //=========================================================================

    bool Initialize();
    void Cleanup();

    //=========================================================================
    // Texture Management
    //=========================================================================

    GLuint GetCurrentVideoTexture() const { return video_texture_; }
    GLuint GetColorTexture() const { return color_texture_; }  // For HDR output
    bool HasValidTexture() const { return has_video_ && video_texture_ != 0; }
    int GetVideoWidth() const { return video_width_; }
    int GetVideoHeight() const { return video_height_; }
    int GetColorTextureWidth() const { return color_texture_width_; }
    int GetColorTextureHeight() const { return color_texture_height_; }

    // Cross-platform texture ID for ImGui
    // Returns D3D11 SRV on Windows (full D3D11 mode), OpenGL texture ID otherwise
    ImTextureID GetDisplayTextureID() const {
#ifdef _WIN32
        if (use_d3d11_rendering_ && color_srv_d3d_) {
            return reinterpret_cast<ImTextureID>(color_srv_d3d_.Get());
        }
#endif
        return (ImTextureID)(intptr_t)color_texture_;
    }

#ifdef _WIN32
    //=========================================================================
    // D3D11 Rendering (Windows)
    //=========================================================================

    // Enable/disable full D3D11 rendering mode
    void SetD3D11RenderingMode(bool enabled);
    bool IsD3D11RenderingMode() const { return use_d3d11_rendering_; }

    // Get D3D11 resources
    ID3D11Texture2D* GetD3D11VideoTexture() const { return video_texture_d3d_.Get(); }
    ID3D11ShaderResourceView* GetD3D11VideoSRV() const { return video_srv_d3d_.Get(); }
    ID3D11Texture2D* GetD3D11ColorTexture() const { return color_texture_d3d_.Get(); }
    ID3D11ShaderResourceView* GetD3D11ColorSRV() const { return color_srv_d3d_.Get(); }
    ID3D11RenderTargetView* GetD3D11ColorRTV() const { return color_rtv_d3d_.Get(); }

    // Apply OCIO color pipeline using D3D11
    void ApplyColorPipelineD3D11();

    //=========================================================================
    // HDR Direct Rendering (for HDR/SDR layer separation)
    //=========================================================================

    // Render video directly to D3D11 render target (bypasses ImGui)
    // Used for HDR layer separation where video needs pure HDR passthrough
    void RenderVideoToHDRTarget(ID3D11RenderTargetView* rtv,
                                int x, int y, int width, int height);
#endif

    //=========================================================================
    // Rendering
    //=========================================================================

    void ProcessPendingTextureUploads();  // Call BEFORE ImGui::NewFrame()
    void RenderVideoFrame();
    void UpdateVideoTexture();
    void RenderPlaceholder();

    //=========================================================================
    // OCIO Color Pipeline
    //=========================================================================

    void SetColorPipeline(std::unique_ptr<OCIOPipeline> pipeline);
    void ClearColorPipeline();
    bool HasColorPipeline() const;
    bool HasActiveColorTransform() const;
    GLuint CreateColorCorrectedTexture(GLuint input_texture_id, int tex_width, int tex_height,
                                       int output_width, int output_height);
    void ForceFrameRefresh();

    //=========================================================================
    // Overlay Rendering
    //=========================================================================

    void RenderSVGOverlays(ImDrawList* draw_list, ImVec2 video_pos, ImVec2 video_size,
                          float opacity = 0.7f, ImU32 color = IM_COL32(255, 255, 255, 255),
                          float line_width = 2.0f);
    SVGOverlayRenderer* GetSVGRenderer() const { return svg_overlay_renderer_.get(); }
    bool IsSafetyOverlaysEnabled() const { return svg_overlays_enabled_; }
    void EnableSafetyOverlays(bool enabled) { svg_overlays_enabled_ = enabled; }
    void SetSafetyOverlaySettings(const SafetyGuideSettings& settings);
    SafetyGuideSettings& GetSafetyOverlaySettings() { return safety_settings_; }
    const SafetyGuideSettings& GetSafetyOverlaySettings() const { return safety_settings_; }

    //=========================================================================
    // EXR/Image Sequence Cache
    //=========================================================================

    bool IsInEXRMode() const { return is_exr_mode_; }
    void InitializeEXRCache(const std::vector<std::string>& sequence_files,
                           const std::string& layer_name, double fps,
                           double initial_position = -1.0);
    ump::DirectEXRCache* GetEXRCache() const { return exr_cache_.get(); }
    const std::vector<std::string>& GetEXRSequenceFiles() const { return exr_sequence_files_; }
    double GetEXRFrameRate() const { return exr_frame_rate_; }
    int GetEXRSequenceStartFrame() const { return exr_sequence_start_frame_; }
    void ClearEXRCache();
    void SetEXRCacheConfig(const ump::DirectEXRCacheConfig& config);
    std::vector<ump::CacheSegment> GetEXRCacheSegments() const;
    bool HasEXRCache() const;

    // Thumbnail cache access
    ump::ThumbnailCache* GetThumbnailCache() const { return thumbnail_cache_.get(); }
    GLuint GetThumbnailForFrame(int frame, bool allow_fallback = false, int* out_actual_frame = nullptr);
    bool HasThumbnailCache() const;
    void ClearThumbnailCache();

    //=========================================================================
    // Timeline Integration
    //=========================================================================

    void SetTimelineMode(bool enabled, ump::TimelinePlaybackController* controller = nullptr);
    bool IsInTimelineMode() const { return is_timeline_mode_; }
    void InjectCurrentTimelineFrame();
    void SetContentDimensions(int width, int height);

    //=========================================================================
    // Playback Control (Passthrough to TimelinePlaybackController)
    //=========================================================================

    void Play();
    void Pause();
    void Seek(double position);
    void StepFrame(int direction);
    void GoToStart();
    void GoToEnd();
    bool IsPlaying() const;

    //=========================================================================
    // Media Info (Cached Values)
    //=========================================================================

    double GetPosition() const;
    double GetDuration() const;
    double GetFrameRate() const;
    int GetTotalFrames() const;
    int GetCurrentFrame() const;
    std::string GetFilePath() const;
    bool HasVideo() const { return has_video_; }

    // Loop control
    void SetLoop(bool enabled);
    bool IsLooping() const { return loop_enabled_; }

    //=========================================================================
    // Screenshot Capture
    //=========================================================================

    bool CaptureScreenshotToClipboard();
    bool CaptureScreenshotToDesktop(const std::string& filename = "");
    bool CaptureScreenshotToPath(const std::string& directory_path, const std::string& filename);

    //=========================================================================
    // Pipeline Mode
    //=========================================================================

    void SetPipelineMode(PipelineMode mode);
    PipelineMode GetPipelineMode() const { return current_pipeline_mode_; }
    const PipelineConfig& GetCurrentPipelineConfig() const;

    // Apply MPV configuration for pipeline mode (tone-mapping, fbo-format, etc.)
    void ApplyPipelineModeConfig(PipelineMode mode);


    //=========================================================================
    // Callbacks
    //=========================================================================

    void SetDimensionChangeCallback(std::function<void(int, int)> callback) {
        dimension_change_callback_ = callback;
    }

    //=========================================================================
    // MPV Video Playback (Direct GPU rendering - no CPU roundtrip)
    //=========================================================================

    // MPV lifecycle
    bool InitializeMPV();
    void CleanupMPV();
    bool LoadVideoFile(const std::string& path);
    void UnloadVideoFile();

    // MPV rendering (called from UpdateVideoTexture)
    void RenderMPVFrame();      // Render MPV to mpv_fbo, blit to video_texture
    void ProcessMPVEvents();    // Non-blocking event poll

    // MPV playback control
    void MPVPlay();
    void MPVPause();
    void MPVSeek(double position);
    void MPVStepFrame(int direction);
    void MPVSetLoop(bool enabled);
    void MPVSetSpeed(double speed);  // 1.0 = normal, 2.0 = 2x, 0.5 = half speed
    void MPVSetVolume(double volume);  // 0.0 to 1.0
    void MPVSetMute(bool muted);
    bool IsMPVLoaded() const { return mpv_file_loaded_; }

    // MPV state queries
    double GetMPVPosition() const;
    double GetMPVDuration() const;
    double GetMPVSpeed() const;
    double GetMPVVolume() const { return mpv_volume_; }
    bool IsMPVMuted() const { return mpv_muted_; }
    bool IsMPVPlaying() const { return mpv_is_playing_; }
    bool IsMPVLooping() const { return mpv_loop_enabled_; }

    // Async scrubbing - playhead moves instantly, decode catches up
    void MPVStartScrub();
    void MPVUpdateScrub(double position);  // Call as user drags - instant playhead
    void MPVEndScrub();                     // Triggers final exact seek
    double GetMPVScrubTarget() const { return mpv_scrub_target_.load(); }
    bool IsMPVScrubbing() const { return mpv_is_scrubbing_.load(); }

    // AB-loop for in/out point playback
    void MPVSetABLoop(double in_point, double out_point);  // Set loop region (-1 to clear)
    void MPVClearABLoop();                                  // Clear AB-loop points
    bool HasMPVABLoop() const { return mpv_ab_loop_active_; }

    // Async fast seek - decoupled like scrubbing
    void MPVStartFastSeek(bool forward);    // Start FF/RW
    void MPVUpdateFastSeek();               // Call from render loop - updates position
    void MPVStopFastSeek();                 // Stop and resume normal playback
    bool IsMPVFastSeeking() const { return mpv_fast_seeking_; }
    bool IsMPVFastForward() const { return mpv_fast_forward_; }
    double GetMPVFastSeekSpeed() const { return mpv_fast_seek_speed_; }

    // Check if new frame is ready (for async rendering)
    bool HasMPVFrameReady() const { return mpv_frame_ready_.load(); }
    void ClearMPVFrameReady() { mpv_frame_ready_.store(false); }

    // EOF callback - called when MPV reaches end of file (non-looping mode)
    // Use this to trigger playlist transitions
    using MPVEOFCallback = std::function<void()>;
    void SetMPVEOFCallback(MPVEOFCallback callback) { mpv_eof_callback_ = callback; }

    //=========================================================================
    // Backward Compatibility Stubs (MPV-specific methods now deprecated)
    // These methods exist to maintain compilation compatibility during migration
    //=========================================================================

    // MPV-specific initialization - no-op in display component
    void SetupPropertyObservation() {}
    void UpdateFromMPVEvents() {}

    // Cache settings - now handled by TimelinePlaybackController
    void SetCacheSettings(int, int) {}
    void SetCacheSettings(const std::string&, int, int, bool) {}
    void SetCacheClearCallback(std::function<void()>) {}

    // State management
    void ResetState();

    // Fast seek methods - passthrough to TimelinePlaybackController
    void UpdateFastSeek();
    void UpdateFastSeek(double) { UpdateFastSeek(); }  // Legacy overload
    void StartRewind();
    void StartFastForward();
    void StopFastSeek();
    void SetScrubMode(bool enabled);  // Enable for responsive mouse scrubbing

    // Volume control - now handled by AudioMixer
    void SetVolume(double) {}
    ump::AudioPlayer* GetDualViewAudio() { return nullptr; }

    // File loading - now handled via timeline
    bool LoadFile(const std::string&, bool = false, double = -1.0) { return false; }
    bool LoadFileTrimmed(const std::string&, double, double) { return false; }

    // EXR methods
    std::string GetEXRLayerName() const { return exr_layer_name_; }
    size_t ClearEXRDiskCache();
    // EXRCacheStats maps to DirectEXRCache::Stats for compatibility
    struct EXRCacheStats {
        int total_frames = 0;
        int cached_frames = 0;
        int pending_requests = 0;
        size_t cache_bytes = 0;
    };
    EXRCacheStats GetEXRCacheStats() const;

    // Image sequence methods
    std::string GetImageSequenceFormat() const { return is_exr_mode_ ? "exr" : ""; }
    bool SupportsPipelineMode(PipelineMode) const { return true; }
    bool IsImageSequence() const { return is_exr_mode_; }
    int GetImageSequenceStartFrame() const { return exr_sequence_start_frame_; }

    // Comparison mode - deleted, always disabled
    bool IsComparisonModeEnabled() const { return false; }
    ComparisonMode GetComparisonMode() const { return ComparisonMode::DISABLED; }
    bool IsLavfiMode() const { return false; }

    // Audio methods - now handled via AudioMixer
    bool IsAudioOnly() const { return false; }
    bool LoadSequenceAudio(const std::string&) { return false; }
    void LoadSequenceAudio(const std::string&, double) {}
    void UnloadSequenceAudio() {}

    // Dual view methods - deprecated, always return disabled/empty
    void UpdateDualViewClipFromPrimary() {}
    void UpdateDualViewClipFromSecondary() {}
    void UpdateDualViewTimer() {}
    ump::DualViewTimeline& GetDualViewTimeline();
    double GetVirtualTimelinePosition() const { return GetPosition(); }
    double GetVirtualTimelineDuration() const { return GetDuration(); }
    bool IsDualViewTimerActive() const { return false; }
    // Stub timer struct for compatibility
    struct DualViewTimerStub {
        bool IsLooping() const { return false; }
        void SetLooping(bool) {}
    };
    DualViewTimerStub* GetDualViewTimer() { return nullptr; }
    void RevertToEditMode() {}
    void SetPrimaryTrimPoints(double, double) {}
    void SetSecondaryTrimPoints(double, double) {}
    bool IsLavfiMode(ComparisonMode) const { return false; }
    void OnVirtualTimelineDurationChanged() {}

    // Thumbnail methods
    std::pair<int, int> GetThumbnailSize() const { return {192, 108}; }
    void GetThumbnailSize(int* w, int* h, int* = nullptr) const { if (w) *w = 192; if (h) *h = 108; }
    bool GetThumbnailSize(int, int& w, int& h) const { w = 192; h = 108; return true; }

    // Timecode formatting
    static std::string FormatTimecode(double seconds, double fps);

    // Dual view seeking - no-op
    void SeekDualView(double) {}

    // Additional stubs for compatibility
    bool HasAudio() const { return false; }
    void SetLoopMode(bool enabled) { loop_enabled_ = enabled; }
    void Stop() { Pause(); }
    template<typename T> void SetPlaylistPositionCallback(T) {}
    template<typename T> void SetMetadataCallback(T) {}
    void SetMFFrameRate(double) {}
    void SetImageSequenceFrameRate(double) {}
    void SetImageSequenceFrameRate(double, bool) {}
    void LoadPlaylist(const std::string&) {}
    void OnPlaylistItemChanged(int) {}
    void OnPlaylistItemChanged(const std::string&) {}

    // Dual view loading
    void LoadPrimaryVideoInDualView(const std::string&) {}

    // EXR cache control
    void SetEXRCacheEnabled(bool) {}
    void SetEXRPosition(double) {}

    // Texture reference clearing
    void ClearVideoTextureReference() {}

    // Fast seek methods - query passthrough to TimelinePlaybackController
    bool IsFastSeeking() const;
    bool IsFastForward() const;
    double GetFastSeekSpeed() const;
    bool IsReadyToRender() const { return has_video_; }

private:
    //=========================================================================
    // MPV Instance (Three-FBO Architecture)
    //=========================================================================

    mpv_handle* mpv_ = nullptr;
    mpv_render_context* mpv_gl_ = nullptr;

    // Separate MPV FBO (MPV renders here, then blit to video_texture_)
    GLuint mpv_fbo_ = 0;
    GLuint mpv_texture_ = 0;

    // MPV state
    std::string current_video_path_;
    bool mpv_file_loaded_ = false;
    double mpv_cached_position_ = 0.0;
    double mpv_cached_duration_ = 0.0;
    double mpv_speed_ = 1.0;
    double mpv_volume_ = 1.0;  // 0.0 to 1.0
    bool mpv_muted_ = false;
    bool mpv_loop_enabled_ = false;

    // Async rendering - MPV signals when new frame is ready
    std::atomic<bool> mpv_frame_ready_{false};
    std::atomic<bool> mpv_redraw_needed_{false};

    // Scrub position tracking - decouples playhead from decode
    std::atomic<double> mpv_scrub_target_{0.0};  // Where user is scrubbing to (instant)
    std::atomic<bool> mpv_is_scrubbing_{false};  // True during active scrub
    bool mpv_is_playing_ = false;

    // AB-loop state (for in/out point playback)
    bool mpv_ab_loop_active_ = false;
    double mpv_ab_loop_a_ = -1.0;  // In point (-1 = not set)
    double mpv_ab_loop_b_ = -1.0;  // Out point (-1 = not set)

    // Fast seek state - decoupled like scrubbing
    bool mpv_fast_seeking_ = false;
    bool mpv_fast_forward_ = true;   // true = forward, false = rewind
    double mpv_fast_seek_speed_ = 2.0;
    double mpv_fast_seek_position_ = 0.0;
    std::chrono::steady_clock::time_point mpv_fast_seek_start_time_;
    std::chrono::steady_clock::time_point mpv_fast_seek_last_update_;

    // EOF callback - called when MPV reaches end of file (for playlist transitions)
    MPVEOFCallback mpv_eof_callback_;

    //=========================================================================
    // OpenGL Resources
    //=========================================================================

    GLuint video_texture_ = 0;
    GLuint fbo_ = 0;

    // Color processing resources
    GLuint color_fbo_ = 0;
    GLuint color_texture_ = 0;
    int color_texture_width_ = 0;
    int color_texture_height_ = 0;
    GLuint quad_vao_ = 0;
    GLuint quad_vbo_ = 0;

    // Transition placeholder texture
    GLuint transition_placeholder_texture_ = 0;
    int transition_placeholder_width_ = 64;
    int transition_placeholder_height_ = 64;

    // Gap placeholder texture (for timeline gaps/cache misses)
    GLuint gap_placeholder_texture_ = 0;
    int gap_placeholder_width_ = 0;
    int gap_placeholder_height_ = 0;

    //=========================================================================
    // Video Properties
    //=========================================================================

    int video_width_ = 0;
    int video_height_ = 0;
    double cached_position_ = 0.0;
    double cached_duration_ = 0.0;
    double cached_fps_ = 24.0;

    // Content dimensions (independent of video dimensions for overlay modes)
    int content_width_ = 0;
    int content_height_ = 0;
    bool use_content_dimensions_ = false;

    //=========================================================================
    // State
    //=========================================================================

    bool has_video_ = false;
    bool loop_enabled_ = true;

    //=========================================================================
    // OCIO Pipeline
    //=========================================================================

    std::unique_ptr<OCIOPipeline> color_pipeline_;

    //=========================================================================
    // Pipeline Mode
    //=========================================================================

    PipelineMode current_pipeline_mode_ = PipelineMode::NORMAL;
    GLenum current_internal_format_ = GL_RGBA8;

    //=========================================================================
    // Safety/SVG Overlays
    //=========================================================================

    std::unique_ptr<SVGOverlayRenderer> svg_overlay_renderer_;
    bool svg_overlays_enabled_ = false;
    SafetyGuideSettings safety_settings_;

    //=========================================================================
    // EXR Cache
    //=========================================================================

    bool is_exr_mode_ = false;
    std::string exr_layer_name_;
    double exr_frame_rate_ = 24.0;
    int exr_sequence_start_frame_ = 0;
    std::vector<std::string> exr_sequence_files_;
    std::shared_ptr<ump::DirectEXRCache> exr_cache_;
    std::unique_ptr<ump::ThumbnailCache> thumbnail_cache_;

    //=========================================================================
    // Timeline Mode
    //=========================================================================

    bool is_timeline_mode_ = false;
    ump::TimelinePlaybackController* timeline_controller_ = nullptr;
    GLuint timeline_texture_ = 0;
    int timeline_texture_width_ = 0;
    int timeline_texture_height_ = 0;
    int last_timeline_frame_ = -1;

    //=========================================================================
    // Callbacks
    //=========================================================================

    std::function<void(int, int)> dimension_change_callback_;

    //=========================================================================
    // Private Methods
    //=========================================================================

    void CreateVideoTextures(int width, int height);
    void CreateVideoTexturesForMode(int width, int height, PipelineMode mode);
    void CreateColorProcessingResourcesForMode(int width, int height, PipelineMode mode);
    void CreateMPVTextures(int width, int height);
    void CleanupMPVTextures();
    void CreateTransitionPlaceholder();
    void ClearVideoTextureToBackground();
    void ClearColorTextureToBackground();
    void SetupColorProcessingResources();
    void ApplyColorPipeline();
    void RenderVideoTexture();

#ifdef _WIN32
    //=========================================================================
    // D3D11 Resources (Windows)
    //=========================================================================

    bool use_d3d11_rendering_ = false;

    // D3D11 video texture (input)
    Microsoft::WRL::ComPtr<ID3D11Texture2D> video_texture_d3d_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> video_srv_d3d_;

    // D3D11 SRV from timeline cache (not owned - points to cache's texture)
    ID3D11ShaderResourceView* timeline_srv_d3d_ = nullptr;

    // D3D11 color texture (OCIO output)
    Microsoft::WRL::ComPtr<ID3D11Texture2D> color_texture_d3d_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> color_srv_d3d_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> color_rtv_d3d_;

    // D3D11 OCIO renderer
    std::unique_ptr<ump::D3D11OCIORenderer> d3d11_ocio_renderer_;

    // D3D11 helper methods
    void CreateD3D11VideoTextures(int width, int height);
    void CreateD3D11ColorTextures(int width, int height);
    void CleanupD3D11Resources();
#endif
};
