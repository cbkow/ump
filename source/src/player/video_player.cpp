#include "video_player.h"
#include "comparison_video_player.h"
#include "lavfi_filter_generator.h"
#include "../transcode/transcode_manager.h"
#include "difference_cache.h"
#include "../project/media_item.h"
#include "../timeline/timeline_playback_controller.h"
#include "../timeline/timeline_cache.h"
#include "../utils/debug_utils.h"
#include "../utils/gpu_scheduler.h"
// Note: dummy_video_generator.h removed - now using PlaybackTimer virtual timeline
#include "exr_transcoder.h"
#include "direct_exr_cache.h"
#include "image_loaders.h"  // For TIFF/PNG/JPEG loaders
#include "thumbnail_cache.h"
#include "media_background_extractor.h"  // For ConversionStrategy
#include "../metadata/video_metadata.h"  // For VideoMetadata
#include "../metadata/ffmpeg_metadata_extractor.h"  // For FFmpeg-based metadata extraction
#include "../project/project_manager.h"  // For IsInterFrameCodec helper
#include "../audio/audio_player.h"  // For independent audio playback in dual view mode

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>

// External global cache path (user-configurable)
extern std::string g_custom_cache_path;
#include <sstream>
#include <thread>
#include <vector>
#include <regex>
#include <filesystem>

#include <GLFW/glfw3.h>
#include <imgui.h>

// External font from main.cpp
extern ImFont* font_mono;

// External functions from main.cpp
extern ImVec4 GetWindowsAccentColor();

// Local helper to create muted dark variant of a color
static ImVec4 MutedDarkLocal(const ImVec4& color) {
    // Apply brightness (0.7f) and saturation (0.4f)
    ImVec4 result = color;
    result.x *= 0.7f;
    result.y *= 0.7f;
    result.z *= 0.7f;

    // Desaturate
    float gray = result.x * 0.299f + result.y * 0.587f + result.z * 0.114f;
    result.x = gray + (result.x - gray) * 0.4f;
    result.y = gray + (result.y - gray) * 0.4f;
    result.z = gray + (result.z - gray) * 0.4f;

    return result;
}

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#endif

// Include STB image write for PNG output
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../../external/glfw/deps/stb_image_write.h"

// ============================================================================
// Pipeline Mode System Implementation
// ============================================================================

const std::map<PipelineMode, PipelineConfig> PIPELINE_CONFIGS = {
    {PipelineMode::NORMAL, {
        PipelineMode::NORMAL,
        GL_RGBA8, GL_UNSIGNED_BYTE,
        false, false, 4,
        "Normal (8-bit) - Best Performance",
        4096,   // 4GB recommended
        16384   // 16GB max
    }},
    {PipelineMode::HIGH_RES, {
        PipelineMode::HIGH_RES,
        GL_RGBA16, GL_UNSIGNED_SHORT,
        false, false, 8,
        "High-Res (12-bit/16-bit) - Enhanced Precision",
        2048,   // 2GB recommended (double memory usage)
        8192    // 8GB max
    }},
    {PipelineMode::ULTRA_HIGH_RES, {
        PipelineMode::ULTRA_HIGH_RES,
        GL_RGBA16F, GL_HALF_FLOAT,
        true, false, 8,
        "Ultra-High-Res (Float) - Maximum OCIO Flexibility",
        2048,   // 2GB recommended (double memory usage)
        8192    // 8GB max
    }},
    {PipelineMode::HDR_RES, {
        PipelineMode::HDR_RES,
        GL_RGBA16F, GL_HALF_FLOAT,
        false, true, 8,
        "HDR-Res (Half-Float) - HDR10/HEVC HDR Video",
        2048,   // 2GB recommended (double memory usage)
        8192    // 8GB max
    }}
};

const char* PipelineModeToString(PipelineMode mode) {
    switch (mode) {
        case PipelineMode::NORMAL: return "Normal";
        case PipelineMode::HIGH_RES: return "High-Res";
        case PipelineMode::ULTRA_HIGH_RES: return "Ultra-High-Res";
        case PipelineMode::HDR_RES: return "HDR-Res";
        default: return "Unknown";
    }
}

PipelineMode StringToPipelineMode(const std::string& mode_str) {
    if (mode_str == "Normal") return PipelineMode::NORMAL;
    if (mode_str == "High-Res") return PipelineMode::HIGH_RES;
    if (mode_str == "Ultra-High-Res") return PipelineMode::ULTRA_HIGH_RES;
    if (mode_str == "HDR-Res") return PipelineMode::HDR_RES;
    return PipelineMode::NORMAL; // Default fallback
}

size_t CalculateCacheMemoryUsage(int width, int height, PipelineMode mode, size_t frame_count) {
    auto it = PIPELINE_CONFIGS.find(mode);
    if (it == PIPELINE_CONFIGS.end()) return 0;

    size_t bytes_per_frame = width * height * it->second.bytes_per_pixel;
    return bytes_per_frame * frame_count;
}

VideoPlayer::VideoPlayer()
    : mpv(nullptr), mpv_gl(nullptr),
    video_texture(0), fbo(0),
    video_width(0), video_height(0),
    is_playing(false), has_video(false),
    position(0.0), duration(0.0), volume(100) {

    // Always initialize SVG renderer so dropdown is available
    svg_overlay_renderer = std::make_unique<SVGOverlayRenderer>();
    Debug::Log("SVG overlay renderer initialized in constructor");

    // 🔧 CRITICAL: Pre-create DirectEXRCache so I/O threads are always running
    // This eliminates thread startup delay when loading EXR sequences
    exr_cache_ = std::make_shared<ump::DirectEXRCache>();
    Debug::Log("DirectEXRCache pre-created (threads will start on first Initialize)");
}

VideoPlayer::~VideoPlayer() {
    Cleanup();
}

void* VideoPlayer::GetProcAddress(void* ctx, const char* name) {
    return (void*)glfwGetProcAddress(name);
}

bool VideoPlayer::Initialize() {
    mpv = mpv_create();
    if (!mpv) {
        std::cerr << "Failed to create MPV instance" << std::endl;
        return false;
    }

    unsigned long version = mpv_client_api_version();
    std::cout << "MPV client API version: " << std::hex << version << std::dec << std::endl;

    ConfigureBasicOptions();
    ConfigureVideoOptions();
    ConfigureAudioOptions();
    ConfigureSeekingOptions();
    ConfigureCacheOptions();
    ConfigureHardwareDecoding();

    if (mpv_initialize(mpv) < 0) {
        std::cerr << "Failed to initialize MPV" << std::endl;
        return false;
    }

    std::cout << "MPV initialized successfully!" << std::endl;

    if (!SetupOpenGL()) {
        return false;
    }

    // Create transition placeholder texture (prevents font cache flicker during media switches)
    CreateTransitionPlaceholder();

    // Create default passthrough color pipeline (provides stable buffering during transitions)
    // The color pipeline copies video_texture -> color_texture, which provides frame stability
    // during media transitions. Without this, video_texture is displayed directly and can
    // become invalid momentarily, causing font cache flickering.
    auto passthrough = std::make_unique<OCIOPipeline>();
    if (passthrough->CreatePassthroughPipeline()) {
        color_pipeline = std::move(passthrough);
        Debug::Log("VideoPlayer: Default passthrough color pipeline created");

        // Create quad VAO/VBO for color pipeline rendering (only once)
        // This prevents issues if ApplyColorPipeline is called before SetupColorProcessingResources
        if (quad_vao == 0) {
            float quad_vertices[] = {
                // positions   // texCoords
                -1.0f,  1.0f,  0.0f, 1.0f,  // top-left
                -1.0f, -1.0f,  0.0f, 0.0f,  // bottom-left
                 1.0f, -1.0f,  1.0f, 0.0f,  // bottom-right
                -1.0f,  1.0f,  0.0f, 1.0f,  // top-left
                 1.0f, -1.0f,  1.0f, 0.0f,  // bottom-right
                 1.0f,  1.0f,  1.0f, 1.0f   // top-right
            };

            glGenVertexArrays(1, &quad_vao);
            glGenBuffers(1, &quad_vbo);

            glBindVertexArray(quad_vao);
            glBindBuffer(GL_ARRAY_BUFFER, quad_vbo);
            glBufferData(GL_ARRAY_BUFFER, sizeof(quad_vertices), quad_vertices, GL_STATIC_DRAW);

            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
            glEnableVertexAttribArray(1);
            glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));

            glBindVertexArray(0);
            Debug::Log("VideoPlayer: Created quad VAO/VBO for color pipeline");
        }

        // Create initial color processing resources at placeholder dimensions
        // This ensures color_fbo and color_texture exist before first video loads
        CreateColorProcessingResourcesForMode(transition_placeholder_width_, transition_placeholder_height_, current_pipeline_mode);
        Debug::Log("VideoPlayer: Created initial color resources at " +
                   std::to_string(transition_placeholder_width_) + "x" + std::to_string(transition_placeholder_height_));
    } else {
        Debug::Log("VideoPlayer: WARNING - Failed to create default passthrough pipeline");
    }

    ApplyRenderOptimizations();
    mpv_request_event(mpv, MPV_EVENT_FILE_LOADED, 1);
    return true;
}

void VideoPlayer::ConfigureBasicOptions() {

    mpv_set_option_string(mpv, "load-scripts", "no");
    mpv_set_option_string(mpv, "osc", "no");
    mpv_set_option_string(mpv, "ytdl", "no");
    mpv_set_option_string(mpv, "load-auto-profiles", "no");

    mpv_set_option_string(mpv, "terminal", "no");
    mpv_set_option_string(mpv, "msg-level", "no");
    mpv_set_option_string(mpv, "idle", "yes");
    mpv_set_option_string(mpv, "pause", "yes");
    mpv_set_option_string(mpv, "keep-open", "no");
    mpv_set_option_string(mpv, "keep-open-pause", "no");

    mpv_set_option_string(mpv, "input-default-bindings", "no");
    mpv_set_option_string(mpv, "cursor-autohide", "no");
    mpv_set_option_string(mpv, "force-window", "no");

    // Disable OSD (seek bar, messages, etc.)
    mpv_set_option_string(mpv, "osd-level", "0");
    mpv_set_option_string(mpv, "osd-bar", "no");

    // Visual settings
    mpv_set_option_string(mpv, "alpha", "blend");
    mpv_set_option_string(mpv, "background", "none");
    mpv_set_option_string(mpv, "background-color", "#202020/1.0");
    mpv_set_option_string(mpv, "blend-subtitles", "yes");
}

void VideoPlayer::ConfigureVideoOptions(bool lavfi_mode) {
    // Video output and rendering
    mpv_set_option_string(mpv, "vo", "libmpv");
    mpv_set_option_string(mpv, "video-unscaled", "no");
    mpv_set_option_string(mpv, "keepaspect", "yes");

    // Video sync - optimize based on mode
    if (lavfi_mode) {
        // Lavfi uses CPU-based filtering, so avoid expensive interpolation
        // Use audio sync for lower CPU usage and better performance
        mpv_set_option_string(mpv, "video-sync", "audio");
        Debug::Log("ConfigureVideoOptions: Using video-sync=audio for lavfi mode (performance optimization)");

        // Add video decode queue buffering for smooth dual-stream playback
        mpv_set_option_string(mpv, "vd-queue-enable", "yes");
        mpv_set_option_string(mpv, "vd-queue-max-bytes", "512MiB");
        mpv_set_option_string(mpv, "vd-queue-max-samples", "50");
        Debug::Log("ConfigureVideoOptions: Enabled vd-queue buffering (512MiB) for lavfi mode");
    } else {
        // Normal mode: use display-resample for smooth playback
        mpv_set_option_string(mpv, "video-sync", "display-resample");
    }

    // OpenGL settings
    mpv_set_option_string(mpv, "opengl-rectangle-textures", "yes");
    mpv_set_option_string(mpv, "opengl-pbo", "yes");
    mpv_set_option_string(mpv, "opengl-hwdec-interop", "auto");
    mpv_set_option_string(mpv, "gpu-shader-cache", "yes");

    // Color and tone mapping
    mpv_set_option_string(mpv, "tone-mapping", "off");

    // Screenshot settings
    //mpv_set_option_string(mpv, "screenshot-high-bit-depth", "yes");
    //mpv_set_option_string(mpv, "screenshot-jpeg-quality", "75");
}

void VideoPlayer::ConfigureAudioOptions() {
    mpv_set_option_string(mpv, "volume", "80");
    mpv_set_option_string(mpv, "ad-lavc-threads", "2");
}

void VideoPlayer::ConfigureSeekingOptions() {
    // High-precision seeking
    mpv_set_option_string(mpv, "hr-seek", "yes");
    mpv_set_option_string(mpv, "hr-seek-framedrop", "no");
    //mpv_set_option_string(mpv, "hr-seek-demuxer-offset", "1.0");

    // Demuxer optimizations for seeking
   /* mpv_set_option_string(mpv, "demuxer-seekable-cache", "yes");
    mpv_set_option_string(mpv, "demuxer-donation", "2.0");
    mpv_set_option_string(mpv, "demuxer-thread", "yes");
    mpv_set_option_string(mpv, "demuxer-lavf-probe-info", "nostreams");
    mpv_set_option_string(mpv, "demuxer-lavf-analyzeduration", "10M");
    mpv_set_option_string(mpv, "demuxer-lavf-probesize", "200M");
    mpv_set_option_string(mpv, "demuxer-lavf-o", "fflags=+fastseek");
    mpv_set_option_string(mpv, "index", "default");*/
}

void VideoPlayer::ConfigureCacheOptions(bool lavfi_mode) {
    // Cache settings for smooth playback
    mpv_set_option_string(mpv, "cache", "yes");

    if (lavfi_mode) {
        // Lavfi mode: Aggressive buffering for dual streams
        mpv_set_option_string(mpv, "cache-secs", "30");
        mpv_set_option_string(mpv, "demuxer-max-bytes", "1GiB");
        mpv_set_option_string(mpv, "demuxer-readahead-secs", "10");
        mpv_set_option_string(mpv, "demuxer-max-back-bytes", "500MiB");
        mpv_set_option_string(mpv, "stream-buffer-size", "128MiB");
        mpv_set_option_string(mpv, "demuxer-thread", "yes");

        // Cache pause controls for stutter-free startup
        mpv_set_option_string(mpv, "cache-pause-wait", "2");
        mpv_set_option_string(mpv, "cache-pause-fill", "80");

        Debug::Log("ConfigureCacheOptions: Lavfi mode - 1GiB demuxer buffer, 10sec readahead");
    } else {
        // Normal mode: existing settings
        mpv_set_option_string(mpv, "cache-secs", "600");
        mpv_set_option_string(mpv, "stream-buffer-size", "64MiB");
    }

    mpv_set_option_string(mpv, "cache-pause-restart", "yes");
    mpv_set_option_string(mpv, "cache-pause-initial", "yes");
    mpv_set_option_string(mpv, "cache-pause", "no");
    mpv_set_option_string(mpv, "cache-pause-below", "1");
    mpv_set_option_string(mpv, "network-timeout", "60");
    mpv_set_option_string(mpv, "video-latency-hacks", "yes");
}

void VideoPlayer::ConfigureHardwareDecoding(bool lavfi_mode) {
    // Threading
    if (lavfi_mode) {
        // Limit threads for lavfi to reduce context switching overhead
        mpv_set_option_string(mpv, "vd-lavc-threads", "8");
        Debug::Log("ConfigureHardwareDecoding: Using 8 decoder threads for lavfi mode");
    } else {
        mpv_set_option_string(mpv, "vd-lavc-threads", "0");  // Auto for normal mode
    }

    mpv_set_option_string(mpv, "vd-lavc-dr", "yes");

    // Platform-specific hardware decoding
#ifdef _WIN32
    mpv_set_option_string(mpv, "gpu-api", "d3d11");
    mpv_set_option_string(mpv, "gpu-context", "d3d11");

    if (lavfi_mode) {
        // Use hwdec-copy for lavfi - allows CPU filter access to decoded frames
        mpv_set_option_string(mpv, "hwdec", "d3d11va-copy");
        Debug::Log("ConfigureHardwareDecoding: Using d3d11va-copy for lavfi filter access");
    } else {
        mpv_set_option_string(mpv, "hwdec", "d3d11va");
    }
#else
    mpv_set_option_string(mpv, "gpu-api", "opengl");
    mpv_set_option_string(mpv, "hwdec", lavfi_mode ? "auto-copy" : "auto");
#endif

    mpv_set_option_string(mpv, "hwdec-preload", "auto");
    mpv_set_option_string(mpv, "prefetch-playlist", "yes");
}

bool VideoPlayer::SetupOpenGL() {
    // Setup MPV OpenGL rendering context only
    // Texture creation will happen when we have valid video dimensions
    mpv_opengl_init_params gl_init_params = {
        GetProcAddress,
        nullptr,
    };

    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_API_TYPE, const_cast<char*>(MPV_RENDER_API_TYPE_OPENGL)},
        {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl_init_params},
        {MPV_RENDER_PARAM_INVALID, nullptr}
    };

    if (mpv_render_context_create(&mpv_gl, mpv, params) < 0) {
        std::cerr << "Failed to create MPV render context" << std::endl;
        return false;
    }

    std::cout << "MPV render context created!" << std::endl;
    return true;
}

void VideoPlayer::ApplyRenderOptimizations() {
    mpv_set_option_string(mpv, "opengl-pbo", "yes");
    mpv_set_option_string(mpv, "opengl-hwdec-interop", "auto");
}

void VideoPlayer::CreateVideoTextures(int width, int height) {
    // Delegate to pipeline-aware version using current pipeline mode
    CreateVideoTexturesForMode(width, height, current_pipeline_mode);
}

void VideoPlayer::Cleanup() {
    Debug::Log("VideoPlayer::Cleanup: Starting cleanup...");

    // Reset content dimensions (overlay mode state)
    content_width_ = 0;
    content_height_ = 0;
    use_content_dimensions_ = false;

    // Free MPV render context first (may have background rendering threads)
    Debug::Log("VideoPlayer::Cleanup: Freeing MPV render context...");
    if (mpv_gl) {
        mpv_render_context_free(mpv_gl);
        mpv_gl = nullptr;
        Debug::Log("VideoPlayer::Cleanup: MPV render context freed");
    } else {
        Debug::Log("VideoPlayer::Cleanup: No MPV render context to free");
    }

    // Terminate and destroy MPV handle (stops all MPV threads)
    Debug::Log("VideoPlayer::Cleanup: Terminating MPV...");
    if (mpv) {
        mpv_terminate_destroy(mpv);
        mpv = nullptr;
        Debug::Log("VideoPlayer::Cleanup: MPV terminated and destroyed");
    } else {
        Debug::Log("VideoPlayer::Cleanup: No MPV handle to destroy");
    }

    // Delete OpenGL textures
    Debug::Log("VideoPlayer::Cleanup: Deleting OpenGL textures...");
    if (video_texture && video_texture != transition_placeholder_texture_) {
        glDeleteTextures(1, &video_texture);
        video_texture = 0;
        Debug::Log("VideoPlayer::Cleanup: Video texture deleted");
    }

    // Clean up transition placeholder texture
    if (transition_placeholder_texture_) {
        glDeleteTextures(1, &transition_placeholder_texture_);
        transition_placeholder_texture_ = 0;
        Debug::Log("VideoPlayer::Cleanup: Transition placeholder texture deleted");
    }

    // Clean up DirectEXRCache (background spiral caching threads)
    Debug::Log("VideoPlayer::Cleanup: Shutting down DirectEXRCache...");
    if (exr_cache_) {
        exr_cache_->Shutdown();  // This stops background threads
        exr_cache_.reset();
        Debug::Log("VideoPlayer::Cleanup: DirectEXRCache shutdown complete");
    } else {
        Debug::Log("VideoPlayer::Cleanup: No EXR cache to shut down");
    }

    // Clean up EXR texture (note: this might be the same as video_texture in EXR mode)
    if (exr_texture && exr_texture != video_texture) {
        glDeleteTextures(1, &exr_texture);
        exr_texture = 0;
        Debug::Log("VideoPlayer::Cleanup: EXR texture deleted");
    }

    // Delete framebuffers and other GL resources
    Debug::Log("VideoPlayer::Cleanup: Deleting framebuffers and GL resources...");
    if (fbo) {
        glDeleteFramebuffers(1, &fbo);
        fbo = 0;
    }

    if (mpv_texture) {
        glDeleteTextures(1, &mpv_texture);
        mpv_texture = 0;
    }

    if (mpv_fbo) {
        glDeleteFramebuffers(1, &mpv_fbo);
        mpv_fbo = 0;
    }

    if (color_texture) {
        glDeleteTextures(1, &color_texture);
        color_texture = 0;
    }

    if (color_fbo) {
        glDeleteFramebuffers(1, &color_fbo);
        color_fbo = 0;
    }

    if (quad_vao) {
        glDeleteVertexArrays(1, &quad_vao);
        quad_vao = 0;
    }

    if (quad_vbo) {
        glDeleteBuffers(1, &quad_vbo);
        quad_vbo = 0;
    }
    Debug::Log("VideoPlayer::Cleanup: All GL resources deleted");

    // Cleanup thumbnail cache (background worker thread)
    Debug::Log("VideoPlayer::Cleanup: Cleaning up thumbnail cache...");
    if (thumbnail_cache_) {
        thumbnail_cache_.reset();  // Destructor stops worker thread
        Debug::Log("VideoPlayer::Cleanup: Thumbnail cache destroyed");
    } else {
        Debug::Log("VideoPlayer::Cleanup: No thumbnail cache to clean up");
    }

    // Cleanup safety overlay system
    Debug::Log("VideoPlayer::Cleanup: Cleaning up safety overlay system...");
    if (safety_overlay_system) {
        safety_overlay_system->Cleanup();
        safety_overlay_system.reset();
        Debug::Log("VideoPlayer::Cleanup: Safety overlay system cleaned up");
    } else {
        Debug::Log("VideoPlayer::Cleanup: No safety overlay system to clean up");
    }

    Debug::Log("VideoPlayer::Cleanup: Cleanup complete");
}

void VideoPlayer::ConfigureForSingleFile() {
    //Debug::Log("=== ConfigureForSingleFile START ===");

    if (!mpv) {
     /*   Debug::Log("No MPV instance available");*/
        return;
    }

    mpv_set_property_string(mpv, "keep-open", "always");
    mpv_set_property_string(mpv, "keep-open-pause", "no");
    mpv_set_property_string(mpv, "gapless-audio", "no");
    mpv_set_property_string(mpv, "prefetch-playlist", "no");

    is_playlist_loop_mode = false;  // Single file mode
    SetLoop(loop_enabled);  // Apply current loop setting
    //Debug::Log("Configured MPV for single file mode");
    //Debug::Log("=== ConfigureForSingleFile COMPLETE ===");
}

void VideoPlayer::ConfigureForPlaylist() {
    //Debug::Log("=== ConfigureForPlaylist START ===");

    if (!mpv) {
        //Debug::Log("No MPV instance available");
        return;
    }

    mpv_set_property_string(mpv, "keep-open", "yes");
    mpv_set_property_string(mpv, "keep-open-pause", "yes");
    mpv_set_property_string(mpv, "loop-playlist", "no");
    mpv_set_property_string(mpv, "loop-file", "no");
    mpv_set_property_string(mpv, "gapless-audio", "yes");
    mpv_set_property_string(mpv, "prefetch-playlist", "yes");

    is_playlist_loop_mode = true;  // Playlist mode
    SetLoop(loop_enabled);  // Apply current loop setting
    //Debug::Log("Configured MPV for playlist mode");
    //Debug::Log("=== ConfigureForPlaylist COMPLETE ===");
}

void VideoPlayer::LoadFile(const std::string& path) {
    if (path.empty() || !mpv) return;

    std::cout << "Loading file: " << path << std::endl;

    // Exit lavfi mode if currently active (lavfi options are initialization-only)
    if (IsLavfiMode(comparison_mode_)) {
        Debug::Log("LoadFile: Exiting lavfi mode before loading new file");
        ExitLavfiMode();
    }

    // Enhanced logging for media transitions
    if (is_exr_mode) {
        Debug::Log("Transitioning from EXR/image sequence to regular video: " + path);
    } else if (has_video) {
        Debug::Log("Switching from video to video: " + path);
    }

    // Clear current_file_path FIRST to prevent UpdateProperties from accessing stale metadata
    // during cache shutdown (which can trigger callbacks)
    current_file_path.clear();

    // Clear EXR mode flag BEFORE cache shutdown to prevent InjectCurrentEXRFrame() from running
    // during cache teardown (prevents division by zero in EXR frame calculations)
    bool was_exr_mode = is_exr_mode;
    is_exr_mode = false;

    // Clear content dimensions (overlay mode) - new media will set its own if needed
    content_width_ = 0;
    content_height_ = 0;
    use_content_dimensions_ = false;

    // === CLEAR ALL CACHES BEFORE LOADING NEW MEDIA ===
    // This ensures clean transitions between any media types

    // FIRST: Switch video_texture to placeholder BEFORE destroying any cache textures
    // This prevents displaying deleted textures during the transition window
    video_texture = transition_placeholder_texture_;
    video_width = transition_placeholder_width_;
    video_height = transition_placeholder_height_;
    has_video = true;
    exr_texture = 0;

    // Clear color texture to background to prevent showing stale frames
    ClearColorTextureToBackground();

    // Clear video cache (FrameCache)
    if (cache_clear_callback) {
        Debug::Log("LoadFile: Clearing video cache before loading new media");
        cache_clear_callback();
    }

    // Clear EXR cache (DirectEXRCache)
    if (exr_cache_) {
        Debug::Log("LoadFile: Clearing EXR/image sequence cache before loading new media");
        exr_cache_->Shutdown();
        // Process ALL queued texture deletions BEFORE destroying the cache object
        // Otherwise texturesToDelete_ queue is lost and GL textures leak
        // ProcessReadyTextures() only deletes 20 per call, so loop until empty
        while (exr_cache_->HasPendingTextureDeletions()) {
            exr_cache_->ProcessReadyTextures();
        }
        exr_cache_.reset();
    }

    // Clear thumbnail cache
    if (thumbnail_cache_) {
        Debug::Log("LoadFile: Clearing thumbnail cache before loading new media");
        thumbnail_cache_->ClearCache();
        thumbnail_cache_.reset();
    }

    ConfigureForSingleFile();

    if (has_video) {
        Stop();
    }

    ResetState();

    // Store current file path AFTER state reset (prevents race conditions in UpdateProperties)
    current_file_path = path;

    // Reset image sequence flags when loading a new file
    is_image_sequence = false;
    image_sequence_frame_rate = 24.0;

    // Detect if this is an audio file for special handling (shorter timeout)
    bool is_audio_file = false;
    size_t dot_pos = path.find_last_of('.');
    if (dot_pos != std::string::npos) {
        std::string ext = path.substr(dot_pos);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        is_audio_file = (ext == ".wav" || ext == ".mp3" || ext == ".aac" ||
                        ext == ".flac" || ext == ".ogg" || ext == ".wma" || ext == ".m4a");
    }

    Debug::Log("LoadFile: Loading " + std::string(is_audio_file ? "AUDIO" : "VIDEO") +
               " file: " + path);
    Debug::Log("LoadFile: Path length: " + std::to_string(path.length()) + " bytes");
    Debug::Log("LoadFile: Path (raw): " + path);
    Debug::Log("LoadFile: Sending 'loadfile' command to MPV...");

    const char* cmd[] = { "loadfile", path.c_str(), nullptr };
    if (mpv_command(mpv, cmd) < 0) {
        Debug::Log("LoadFile: ERROR - Failed to send loadfile command");
        return;
    }

    Debug::Log("LoadFile: MPV loadfile command sent successfully");

    WaitForFileLoad(is_audio_file);  // Pass audio flag for shorter timeout
    FinalizeLoad();

    // NEW: Always load in paused state (deliberate autoplay control)
    // Autoplay decision happens later in OnVideoLoaded() based on media type
    Pause();
    Debug::Log("LoadFile: Media loaded in paused state (deliberate autoplay control)");

    // Additional verification for post-EXR transitions
    if (!has_video) {
        Debug::Log("WARNING: Video failed to load after EXR transition");
    } else {
        Debug::Log("Successfully loaded regular video after EXR transition");
    }

    // Initialize ThumbnailCache for regular video files (not audio-only files)
    // Skip dummy videos (timeline mode uses TimelineCache instead)
    // Reuse is_audio_file from above (already detected)
    bool is_dummy_video = (path.find("dummy_") != std::string::npos);
    if (has_video && !is_audio_file && !is_dummy_video) {
        ump::ThumbnailConfig thumb_config = GetCurrentThumbnailConfig();
        if (thumb_config.enabled) {
            // Get all video properties from cached FFmpeg metadata (instant, no MPV queries)
            VideoMetadata metadata;
            if (metadata_callback) {
                metadata = metadata_callback(path);
            }

            double fps;
            double duration;
            int frame_count;

            if (metadata.is_loaded && metadata.frame_rate > 0 && metadata.total_frames > 0) {
                // Use cached metadata values
                fps = metadata.frame_rate;
                frame_count = metadata.total_frames;
                duration = frame_count / fps;
                Debug::Log("VideoPlayer: Using cached FFmpeg metadata for thumbnail cache (fps=" +
                           std::to_string(fps) + ", frames=" + std::to_string(frame_count) + ")");
            } else {
                // Fallback: Query MPV only if cached metadata not available
                Debug::Log("VideoPlayer: WARNING - No cached metadata, falling back to MPV queries");
                fps = GetFrameRate();
                duration = GetDuration();
                frame_count = GetTotalFrames();
            }

            Debug::Log("VideoPlayer: Creating ThumbnailCache for video (fps=" + std::to_string(fps) +
                       ", duration=" + std::to_string(duration) + "s, frames=" + std::to_string(frame_count) + ")");

            // Create VideoImageLoader
            auto video_loader = std::make_unique<ump::VideoImageLoader>(path, fps, duration);

            if (metadata.is_loaded) {
                auto strategy = ConversionStrategy::FromMetadata(metadata);
                video_loader->SetConversionStrategy(strategy);
                Debug::Log("VideoPlayer: Thumbnail loader using cached FFmpeg metadata: " + strategy.GetDescription());
            } else {
                // Fallback: query MPV only if cached metadata not available
                Debug::Log("VideoPlayer: WARNING - No cached metadata, falling back to MPV query");
                metadata = ExtractMetadata();
                if (metadata.is_loaded) {
                    auto strategy = ConversionStrategy::FromMetadata(metadata);
                    video_loader->SetConversionStrategy(strategy);
                    Debug::Log("VideoPlayer: Thumbnail loader using MPV metadata (fallback): " + strategy.GetDescription());
                } else {
                    Debug::Log("VideoPlayer: Thumbnail loader - no metadata available for conversion strategy");
                }
            }

            // Create synthetic frame list ("0", "1", "2", etc.")
            std::vector<std::string> frame_list;
            frame_list.reserve(frame_count);
            for (int i = 0; i < frame_count; ++i) {
                frame_list.push_back(std::to_string(i));
            }

            // Create ThumbnailCache
            thumbnail_cache_ = std::make_unique<ump::ThumbnailCache>(
                std::move(frame_list),
                std::move(video_loader),
                thumb_config
            );

            Debug::Log("VideoPlayer: ThumbnailCache initialized for video, " +
                       std::to_string(thumb_config.width) + "x" + std::to_string(thumb_config.height) +
                       ", cache size: " + std::to_string(thumb_config.cache_size));

            // Prefetch strategic frames for timeline preview
            thumbnail_cache_->PrefetchStrategicFrames(frame_count);
        } else {
            Debug::Log("VideoPlayer: ThumbnailCache disabled by configuration");
        }
    } else if (is_audio_file) {
        Debug::Log("VideoPlayer: Skipping ThumbnailCache for audio file (no video frames)");
    } else if (is_dummy_video) {
        Debug::Log("VideoPlayer: Skipping ThumbnailCache for dummy video (timeline uses TimelineCache)");
    }
}

bool VideoPlayer::LoadFileTrimmed(const std::string& path, double in_point, double out_point) {
    if (path.empty() || !mpv) {
        Debug::Log("LoadFileTrimmed: ERROR - Empty path or null MPV instance");
        return false;
    }

    // Validate in/out points
    if (in_point < 0 || out_point <= in_point) {
        Debug::Log("LoadFileTrimmed: ERROR - Invalid in/out points (in=" +
                   std::to_string(in_point) + ", out=" + std::to_string(out_point) + ")");
        Debug::Log("LoadFileTrimmed: Falling back to normal LoadFile");
        LoadFile(path);
        return false;
    }

    double length = out_point - in_point;

    // Create EDL path: edl://[file],start=[in],length=[length]
    std::ostringstream edl;
    edl << "edl://" << path << ",start=" << in_point << ",length=" << length;
    std::string edl_path = edl.str();

    Debug::Log("LoadFileTrimmed: Loading trimmed video via EDL");
    Debug::Log("  Original path: " + path);
    Debug::Log("  In point: " + std::to_string(in_point) + "s");
    Debug::Log("  Out point: " + std::to_string(out_point) + "s");
    Debug::Log("  Duration: " + std::to_string(length) + "s");
    Debug::Log("  EDL path: " + edl_path);

    // Use LoadFile with the EDL path
    LoadFile(edl_path);

    Debug::Log("LoadFileTrimmed: Trimmed video loaded successfully");
    return true;
}

void VideoPlayer::SetMFFrameRate(double fps) {
    if (!mpv) return;

    std::string fps_str = std::to_string(fps);
    int result = mpv_set_option_string(mpv, "mf-fps", fps_str.c_str());
    if (result < 0) {
        std::cout << "Failed to set mf-fps to " << fps << std::endl;
    } else {
        std::cout << "Set mf-fps to " << fps << std::endl;
    }
}

void VideoPlayer::SetImageSequenceFrameRate(double fps, int start_frame) {
    image_sequence_frame_rate = fps;
    image_sequence_start_frame = start_frame;
    is_image_sequence = true;
    Debug::Log("VideoPlayer: Stored image sequence frame rate: " + std::to_string(fps) + ", start frame: " + std::to_string(start_frame));
}

void VideoPlayer::LoadPlaylist(const std::string& edl_content) {
    //Debug::Log("=== VideoPlayer::LoadPlaylist START ===");

    if (edl_content.empty()) {
        //Debug::Log("Empty EDL content, nothing to load");
        return;
    }

    // Exit lavfi mode if currently active (lavfi options are initialization-only)
    if (IsLavfiMode(comparison_mode_)) {
        Debug::Log("LoadPlaylist: Exiting lavfi mode before loading playlist");
        ExitLavfiMode();
    }

    ConfigureForPlaylist();

    std::vector<std::string> file_paths = ParseEDLContent(edl_content);
    if (file_paths.empty()) {
        //Debug::Log("No valid file paths found in EDL");
        return;
    }

    LoadPlaylistFiles(file_paths);
    //Debug::Log("=== VideoPlayer::LoadPlaylist COMPLETE ===");
}

void VideoPlayer::LoadSequence(const ump::Sequence& sequence) {
    std::string edl;
    auto sorted_clips = sequence.GetAllClipsSorted();

    for (const auto& clip : sorted_clips) {
        edl += clip.file_path + "\n";
    }

    LoadPlaylist(edl);
}

void VideoPlayer::OnPlaylistItemChanged(const std::string& new_file_path) {
    if (new_file_path.empty()) return;

    Debug::Log("OnPlaylistItemChanged: Handling playlist switch to: " + new_file_path);

    // Detect if this is an audio file
    bool is_audio_file = false;
    size_t dot_pos = new_file_path.find_last_of('.');
    if (dot_pos != std::string::npos) {
        std::string ext = new_file_path.substr(dot_pos);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        is_audio_file = (ext == ".wav" || ext == ".mp3" || ext == ".aac" ||
                        ext == ".flac" || ext == ".ogg" || ext == ".wma" || ext == ".m4a");
    }

    Debug::Log("OnPlaylistItemChanged: File type: " + std::string(is_audio_file ? "AUDIO" : "VIDEO"));

    // Clear old thumbnail cache (always - either we're switching from video, or it shouldn't exist for audio)
    if (thumbnail_cache_) {
        Debug::Log("OnPlaylistItemChanged: Clearing old thumbnail cache");
        thumbnail_cache_->ClearCache();
        thumbnail_cache_.reset();
    }

    // Defer thumbnail cache creation to background to avoid blocking viewport
    // This allows MPV to render the first frame immediately during playlist switches
    if (!is_audio_file) {
        Debug::Log("OnPlaylistItemChanged: Deferring thumbnail cache creation to background");

        // Defer thumbnail creation by 250ms - gives MPV time to load properties
        // without blocking the viewport from updating
        std::thread([this, new_file_path]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));

            // Update properties to get new file duration and dimensions
            UpdateProperties();

            double duration = cached_duration;
            double fps = GetFrameRate();

            if (fps <= 0) fps = 24.0;  // Default fallback
            int frame_count = static_cast<int>(duration * fps);

            Debug::Log("OnPlaylistItemChanged: Creating thumbnail cache (fps=" + std::to_string(fps) +
                       ", duration=" + std::to_string(duration) + "s, frames=" + std::to_string(frame_count) + ")");

            ump::ThumbnailConfig thumb_config = GetCurrentThumbnailConfig();
            if (thumb_config.enabled && duration > 0) {
                // Create VideoImageLoader for the new file
                auto video_loader = std::make_unique<ump::VideoImageLoader>(new_file_path, fps, duration);

                // Set conversion strategy for color matrix support (ProRes 4444/422, etc.)
                VideoMetadata metadata = ExtractMetadata();
                if (metadata.is_loaded) {
                    auto strategy = ConversionStrategy::FromMetadata(metadata);
                    video_loader->SetConversionStrategy(strategy);
                    Debug::Log("VideoPlayer: Thumbnail loader (playlist) - conversion strategy set: " + strategy.GetDescription());
                } else {
                    Debug::Log("VideoPlayer: Thumbnail loader (playlist) - no metadata available for conversion strategy");
                }

                // Create synthetic frame list
                std::vector<std::string> frame_list;
                frame_list.reserve(frame_count);
                for (int i = 0; i < frame_count; ++i) {
                    frame_list.push_back(std::to_string(i));
                }

                // Create ThumbnailCache (GL operations must be done on main thread via callback or deferred)
                // For now, we'll create it here - if GL context issues arise, we'll need to post to main thread
                thumbnail_cache_ = std::make_unique<ump::ThumbnailCache>(
                    std::move(frame_list),
                    std::move(video_loader),
                    thumb_config
                );

                Debug::Log("OnPlaylistItemChanged: ThumbnailCache created, " +
                           std::to_string(thumb_config.width) + "x" + std::to_string(thumb_config.height));

                // Prefetch strategic frames
                thumbnail_cache_->PrefetchStrategicFrames(frame_count);
            } else {
                Debug::Log("OnPlaylistItemChanged: ThumbnailCache disabled or no duration");
            }
        }).detach();
    } else {
        Debug::Log("OnPlaylistItemChanged: Skipping thumbnail cache for audio file");
        // Reset has_video for audio-only files
        has_video = false;
    }

    Debug::Log("OnPlaylistItemChanged: Playlist item change complete");
}

// ============================================================================
// Playback control methods
// ============================================================================

void VideoPlayer::Play() {
    // Debug: Log ALL play calls
    Debug::Log("VideoPlayer::Play() called - is_playing=" + std::to_string(is_playing) +
               ", timeline_mode=" + std::to_string(is_timeline_mode_) +
               ", virtual=" + std::to_string(timeline_controller_ ? timeline_controller_->IsVirtualTimelineMode() : false) +
               ", exr_mode=" + std::to_string(is_exr_mode) +
               ", img_seq_timer=" + std::to_string(image_sequence_timer_ != nullptr));

    // Image sequence virtual timeline mode
    if (is_exr_mode && image_sequence_timer_) {
        image_sequence_timer_->Play();
        is_playing = true;
        if (exr_cache_) {
            exr_cache_->UpdatePlaybackState(true);
        }
        return;
    }

    // Virtual timeline mode - route to timeline controller
    if (is_timeline_mode_ && timeline_controller_ &&
        timeline_controller_->IsVirtualTimelineMode()) {
        timeline_controller_->Play();
        is_playing = true;
        return;
    }

    // When dual view timer is active, control timer instead of MPV
    if (dual_view_timer_) {
        dual_view_timer_->Play();
        is_playing = true;

        // Keep both MPV instances paused - timer drives position via seeks
        // This ensures proper sync and respects trim boundaries
        if (comparison_video_ && comparison_video_->HasVideo()) {
            comparison_video_->SyncPlaybackState(false);
        }

        // Start audio playback
        if (dual_view_audio_) {
            dual_view_audio_->Play();
        }
        return;
    }

    mpv_set_property_string(mpv, "pause", "no");
    is_playing = true;

    // Update DirectEXRCache playback state
    if (exr_cache_) {
        exr_cache_->UpdatePlaybackState(true);
    }

    // Sync comparison video playback state
    if (comparison_video_ && comparison_video_->HasVideo()) {
        comparison_video_->SyncPlaybackState(true);
    }
}

void VideoPlayer::Pause() {
    // Debug: Log ALL pause calls with extra info
    Debug::Log("VideoPlayer::Pause() called - is_playing=" + std::to_string(is_playing) +
               ", timeline_mode=" + std::to_string(is_timeline_mode_) +
               ", virtual=" + std::to_string(timeline_controller_ ? timeline_controller_->IsVirtualTimelineMode() : false));

    // Image sequence virtual timeline mode
    if (is_exr_mode && image_sequence_timer_) {
        image_sequence_timer_->Pause();
        is_playing = false;
        if (exr_cache_) {
            exr_cache_->UpdatePlaybackState(false);
        }
        return;
    }

    // Virtual timeline mode - route to timeline controller
    if (is_timeline_mode_ && timeline_controller_ &&
        timeline_controller_->IsVirtualTimelineMode()) {
        timeline_controller_->Pause();
        is_playing = false;
        return;
    }

    // When dual view timer is active, control timer instead of MPV
    if (dual_view_timer_) {
        dual_view_timer_->Pause();
        is_playing = false;

        // Pause both videos
        mpv_set_property_string(mpv, "pause", "yes");
        if (comparison_video_ && comparison_video_->HasVideo()) {
            comparison_video_->SyncPlaybackState(false);
        }

        // Pause audio playback
        if (dual_view_audio_) {
            dual_view_audio_->Pause();
        }
        return;
    }

    mpv_set_property_string(mpv, "pause", "yes");
    is_playing = false;

    // Update DirectEXRCache playback state
    if (exr_cache_) {
        exr_cache_->UpdatePlaybackState(false);
    }

    // Sync comparison video playback state
    if (comparison_video_ && comparison_video_->HasVideo()) {
        comparison_video_->SyncPlaybackState(false);
    }
}

void VideoPlayer::Stop() {
    const char* cmd[] = { "stop", nullptr };
    mpv_command(mpv, cmd);
    is_playing = false;
    has_video = false;
    position = 0.0;

    // Update DirectEXRCache playback state
    if (exr_cache_) {
        exr_cache_->UpdatePlaybackState(false);
    }
}

void VideoPlayer::Seek(double pos) {
    // Image sequence virtual timeline mode
    if (is_exr_mode && image_sequence_timer_) {
        double timer_duration = image_sequence_timer_->GetDuration();
        if (pos < 0) pos = 0.0;
        if (pos > timer_duration) pos = timer_duration;
        image_sequence_timer_->Seek(pos);
        cached_position = pos;
        return;
    }

    if (!mpv) return;

    // Virtual timeline mode - route to timeline controller
    if (is_timeline_mode_ && timeline_controller_ &&
        timeline_controller_->IsVirtualTimelineMode()) {
        timeline_controller_->Seek(pos);
        return;
    }

    // When dual view timer is active, use SeekDualView instead
    if (dual_view_timer_) {
        SeekDualView(pos);
        return;
    }

    // Determine max duration: use virtual timeline duration in lavfi mode,
    // otherwise use cached_duration
    double max_duration = cached_duration;
    if (!current_lavfi_filter_.empty()) {
        double virtual_duration = GetVirtualTimelineDuration();
        if (virtual_duration > 0) {
            max_duration = virtual_duration;
        }
    }

    if (pos < 0) pos = 0.0;
    if (pos > max_duration) pos = max_duration;

    std::string pos_str = std::to_string(pos);
    const char* cmd[] = { "seek", pos_str.c_str(), "absolute", "exact", nullptr };
    mpv_command_async(mpv, 0, cmd);

    std::cout << "Seeking to: " << pos << " (exact mode)" << std::endl;

    // Trigger debounced sync for comparison video
    if (comparison_video_) {
        was_playing_before_seek_ = is_playing;
        last_seek_time_ = glfwGetTime();
    }
}

void VideoPlayer::StepFrame(int direction) {
    // Image sequence virtual timeline mode
    if (is_exr_mode && image_sequence_timer_) {
        if (direction > 0) {
            image_sequence_timer_->StepForward(1);
        } else {
            image_sequence_timer_->StepBackward(1);
        }
        cached_position = image_sequence_timer_->GetPosition();
        return;
    }

    // Virtual timeline mode - route to timeline controller
    if (is_timeline_mode_ && timeline_controller_ &&
        timeline_controller_->IsVirtualTimelineMode()) {
        if (direction > 0) {
            timeline_controller_->StepForward(1);
        } else {
            timeline_controller_->StepBackward(1);
        }
        return;
    }

    // When dual view timer is active, use timer stepping
    if (dual_view_timer_) {
        if (direction > 0) {
            dual_view_timer_->StepForward(1);
        } else {
            dual_view_timer_->StepBackward(1);
        }
        return;
    }

    const char* cmd = direction > 0 ? "frame-step" : "frame-back-step";
    const char* cmd_array[] = { cmd, nullptr };
    mpv_command(mpv, cmd_array);

    // Trigger debounced sync for comparison video
    if (comparison_video_) {
        was_playing_before_seek_ = is_playing;
        last_seek_time_ = glfwGetTime();
    }
}

void VideoPlayer::GoToStart() {
    // Image sequence virtual timeline mode
    if (is_exr_mode && image_sequence_timer_) {
        image_sequence_timer_->Seek(0.0);
        cached_position = 0.0;
        return;
    }

    // Virtual timeline mode - route to timeline controller
    if (is_timeline_mode_ && timeline_controller_ &&
        timeline_controller_->IsVirtualTimelineMode()) {
        timeline_controller_->GoToStart();
        return;
    }

    // When dual view timer is active, seek to virtual timeline start
    if (dual_view_timer_) {
        SeekDualView(0.0);
        return;
    }
    Seek(0.0);
}

void VideoPlayer::GoToEnd() {
    // Image sequence virtual timeline mode
    if (is_exr_mode && image_sequence_timer_) {
        double end_pos = image_sequence_timer_->GetDuration();
        // Seek to last frame (one frame before end)
        double fps = exr_frame_rate > 0 ? exr_frame_rate : 24.0;
        end_pos = std::max(0.0, end_pos - (1.0 / fps));
        image_sequence_timer_->Seek(end_pos);
        cached_position = end_pos;
        return;
    }

    // Virtual timeline mode - route to timeline controller
    if (is_timeline_mode_ && timeline_controller_ &&
        timeline_controller_->IsVirtualTimelineMode()) {
        timeline_controller_->GoToEnd();
        return;
    }

    // When dual view timer is active, seek to virtual timeline end
    if (dual_view_timer_) {
        double virtual_duration = GetVirtualTimelineDuration();
        if (virtual_duration > 0) {
            SeekDualView(virtual_duration);
        }
        return;
    }

    // In lavfi mode, use virtual timeline duration
    double effective_duration = cached_duration;
    if (!current_lavfi_filter_.empty()) {
        double virtual_duration = GetVirtualTimelineDuration();
        if (virtual_duration > 0) {
            effective_duration = virtual_duration;
        }
    }

    if (effective_duration > 0) {
        // Seek to the last valid frame (total_frames - 1)
        int total_frames = GetTotalFrames();
        double fps = GetFrameRate();
        if (total_frames > 0 && fps > 0) {
            // Calculate last frame based on effective duration
            int last_frame = static_cast<int>(effective_duration * fps) - 1;
            if (last_frame > 0) {
                double last_frame_time = last_frame / fps;
                Seek(last_frame_time);
            } else {
                Seek(effective_duration - 0.1);
            }
        } else {
            // Fallback if frames/fps not available
            Seek(effective_duration - 0.1);
        }
    }
}

// ============================================================================
// Fast seeking methods
// ============================================================================

void VideoPlayer::StartFastForward() {
    is_fast_seeking = true;
    fast_forward = true;
    fast_seek_speed = 1;
    fast_seek_start = std::chrono::steady_clock::now();
}

void VideoPlayer::StartRewind() {
    is_fast_seeking = true;
    fast_forward = false;
    fast_seek_speed = 1;
    fast_seek_start = std::chrono::steady_clock::now();
}

void VideoPlayer::StopFastSeek() {
    is_fast_seeking = false;
    fast_seek_speed = 1;
}

void VideoPlayer::UpdateFastSeek() {
    if (!is_fast_seeking) return;

    // Check if we're in image sequence virtual timeline mode
    bool is_image_seq_virtual = is_exr_mode && image_sequence_timer_;

    // Check if we're in virtual timeline mode (OTIO timeline with PlaybackTimer)
    bool is_virtual_timeline = is_timeline_mode_ && timeline_controller_ &&
                               timeline_controller_->IsVirtualTimelineMode();

    // When dual view timer is active, lavfi mode, or virtual timeline mode
    bool use_virtual_timeline = dual_view_timer_ || !current_lavfi_filter_.empty() || is_virtual_timeline || is_image_seq_virtual;

    // Get current position from appropriate source
    double current_pos;
    if (is_image_seq_virtual) {
        current_pos = image_sequence_timer_->GetPosition();
    } else if (is_virtual_timeline) {
        current_pos = timeline_controller_->GetPosition();
    } else if (dual_view_timer_) {
        current_pos = GetVirtualTimelinePosition();
    } else {
        current_pos = cached_position;
    }

    double max_duration = cached_duration;
    if (is_image_seq_virtual) {
        // Image sequence mode - get duration from timer
        max_duration = image_sequence_timer_->GetDuration();
    } else if (is_virtual_timeline) {
        // OTIO timeline mode - get duration from timeline controller
        max_duration = timeline_controller_->GetDuration();
    } else if (use_virtual_timeline) {
        // Dual view or lavfi mode
        double virtual_duration = GetVirtualTimelineDuration();
        if (virtual_duration > 0) {
            max_duration = virtual_duration;
        }
    }

    // Use slower base seek for virtual timeline modes (0.033s = ~1 frame at 30fps)
    // Normal mode uses faster seeking (0.1s base)
    bool use_slow_seek = dual_view_timer_ || is_virtual_timeline || is_image_seq_virtual;
    double base_seek = use_slow_seek ? 0.033 : 0.1;
    double seek_amount = base_seek * fast_seek_speed;
    if (!fast_forward) seek_amount = -seek_amount;

    double new_pos = current_pos + seek_amount;
    if (new_pos < 0) new_pos = 0;
    if (new_pos > max_duration) new_pos = max_duration;

    // Debug logging for FF/RW diagnosis
    static int ff_log_counter = 0;
    if (++ff_log_counter % 30 == 0) {  // Log every 30 frames to avoid spam
        Debug::Log("FastSeek: is_virtual=" + std::to_string(is_virtual_timeline) +
                   ", cur=" + std::to_string(current_pos) +
                   ", new=" + std::to_string(new_pos) +
                   ", dur=" + std::to_string(max_duration) +
                   ", speed=" + std::to_string(fast_seek_speed) +
                   ", ff=" + std::to_string(fast_forward));
    }

    // Route seek to appropriate handler
    if (is_image_seq_virtual) {
        // Image sequence mode - seek through timer
        image_sequence_timer_->Seek(new_pos);
        cached_position = new_pos;
    } else if (is_virtual_timeline) {
        // Virtual timeline mode - seek through timeline controller
        timeline_controller_->Seek(new_pos);
    } else if (dual_view_timer_) {
        SeekDualView(new_pos);
    } else {
        Seek(new_pos);
    }

    // Gradually increase speed (slower ramp-up for virtual timeline modes)
    static int frame_counter = 0;
    frame_counter++;
    int ramp_frames = use_slow_seek ? 90 : 60;  // Slower ramp for virtual timelines
    int max_speed = use_slow_seek ? 6 : 8;      // Lower max speed for virtual timelines
    if (frame_counter > ramp_frames && fast_seek_speed < max_speed) {
        fast_seek_speed++;
        frame_counter = 0;
    }
}

// ============================================================================
// Shuttle control methods (JKL professional shuttle system)
// ============================================================================

// ============================================================================
// Volume control methods
// ============================================================================

void VideoPlayer::SetVolume(int vol) {
    volume = vol;
    int64_t v = vol;
    mpv_set_property(mpv, "volume", MPV_FORMAT_INT64, &v);
}

void VideoPlayer::SetVolume(float volume) {
    if (!mpv) return;

    if (volume < 0.0f) volume = 0.0f;
    if (volume > 1.0f) volume = 1.0f;

    double mpv_volume = volume * 100.0;
    mpv_set_property(mpv, "volume", MPV_FORMAT_DOUBLE, &mpv_volume);
    this->volume = static_cast<int>(mpv_volume);
}

float VideoPlayer::GetVolume() const {
    if (!mpv) return 1.0f;

    double vol = 0.0;
    if (mpv_get_property(mpv, "volume", MPV_FORMAT_DOUBLE, &vol) == 0) {
        return static_cast<float>(vol / 100.0f);
    }
    return 1.0f;
}


// ============================================================================
// Loop control methods
// ============================================================================

void VideoPlayer::SetLoop(bool enabled) {
    loop_enabled = enabled;

    // Sync looping state to image sequence timer
    if (image_sequence_timer_) {
        image_sequence_timer_->SetLooping(enabled);
    }

    if (enabled) {
        if (is_playlist_loop_mode) {
            mpv_set_property_string(mpv, "loop-playlist", "inf");
            mpv_set_property_string(mpv, "loop-file", "no");
            //Debug::Log("Enabled playlist loop mode");
        }
        else {
            mpv_set_property_string(mpv, "loop-file", "inf");
            mpv_set_property_string(mpv, "loop-playlist", "no");
            //Debug::Log("Enabled single file loop mode");
        }
    }
    else {
        mpv_set_property_string(mpv, "loop-file", "no");
        mpv_set_property_string(mpv, "loop-playlist", "no");
        //Debug::Log("Disabled looping");
    }

    // Sync looping state to EXR cache for wrap-around caching
    if (exr_cache_) {
        exr_cache_->SetLooping(enabled);
    }

    // Sync looping state to comparison video
    if (comparison_video_ && comparison_video_->HasVideo()) {
        comparison_video_->SetLoop(enabled);
    }
}

void VideoPlayer::SetLoopMode(bool is_playlist_mode) {
    is_playlist_loop_mode = is_playlist_mode;
    if (loop_enabled) {
        SetLoop(true);
    }
}
// ============================================================================
// Properties + event handling
// ============================================================================

void VideoPlayer::SetupPropertyObservation() {
    if (!mpv) return;

    mpv_observe_property(mpv, 1, "time-pos", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv, 2, "duration", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv, 3, "pause", MPV_FORMAT_FLAG);
    mpv_observe_property(mpv, 0, "playlist-pos", MPV_FORMAT_INT64);
}

void VideoPlayer::UpdateFromMPVEvents() {
    if (!mpv) return;

    // In virtual timeline mode, MPV is not used - skip event processing entirely
    // This prevents MPV pause/position events from interfering with PlaybackTimer control
    if (is_timeline_mode_ && timeline_controller_ && timeline_controller_->IsVirtualTimelineMode()) {
        // Drain events without processing to prevent queue buildup
        while (mpv_wait_event(mpv, 0.0)->event_id != MPV_EVENT_NONE) {}
        return;
    }

    while (true) {
        mpv_event* event = mpv_wait_event(mpv, 0.0);
        if (event->event_id == MPV_EVENT_NONE) break;

        HandleMPVEvent(event);
    }
}

void VideoPlayer::HandleMPVEvent(mpv_event* event) {
    switch (event->event_id) {
    case MPV_EVENT_PROPERTY_CHANGE: {
        mpv_event_property* prop = (mpv_event_property*)event->data;
        if (!prop) break;

        std::string prop_name = prop->name ? prop->name : "";
        HandlePropertyChange(prop_name, prop);
        break;
    }
    default:
        break;
    }
}

void VideoPlayer::HandlePropertyChange(const std::string& prop_name, mpv_event_property* prop) {
    if (prop_name == "playlist-pos" && prop->format == MPV_FORMAT_INT64 && prop->data) {
        int new_playlist_pos = *((int64_t*)prop->data);
        if (new_playlist_pos != last_known_playlist_pos) {
            //Debug::Log("MPV playlist position changed from " +
            //    std::to_string(last_known_playlist_pos) +
            //    " to " + std::to_string(new_playlist_pos));

            last_known_playlist_pos = new_playlist_pos;

            if (playlist_position_callback) {
                playlist_position_callback();
            }
        }
    }
    else if (prop_name == "time-pos" && prop->format == MPV_FORMAT_DOUBLE && prop->data) {
        cached_position = *((double*)prop->data);
    }
    else if (prop_name == "duration" && prop->format == MPV_FORMAT_DOUBLE && prop->data) {
        cached_duration = *((double*)prop->data);

        // For image sequences, MPV reports duration as the timestamp of the LAST frame
        // We need to add one frame duration to get the actual end time of the video
        // Example: 90 frames at 24fps should be 3.75s, but MPV reports 89/24 = 3.708333s
        if (is_image_sequence || is_exr_mode) {
            double fps = GetFrameRate();
            if (fps > 0) {
                cached_duration += (1.0 / fps);
            }
        }
    }
    else if (prop_name == "pause" && prop->format == MPV_FORMAT_FLAG && prop->data) {
        // When dual view timer or virtual timeline is active, timer controls is_playing, not MPV events
        bool timer_controls_playback = dual_view_timer_ != nullptr ||
            (is_timeline_mode_ && timeline_controller_ && timeline_controller_->IsVirtualTimelineMode());
        if (!timer_controls_playback) {
            is_playing = !(*((int*)prop->data));
        }
    }
}

// ============================================================================
// Property getters
// ============================================================================

double VideoPlayer::GetPosition() const {
    // Image sequence virtual timeline mode
    if (is_exr_mode && image_sequence_timer_) {
        return image_sequence_timer_->GetPosition();
    }

    // Virtual timeline mode - get position from timeline controller
    if (is_timeline_mode_ && timeline_controller_ &&
        timeline_controller_->IsVirtualTimelineMode()) {
        return timeline_controller_->GetPosition();
    }

    // When dual view timer is active, return virtual timeline position
    if (dual_view_timer_) {
        return GetVirtualTimelinePosition();
    }
    return cached_position;
}

double VideoPlayer::GetDuration() const {
    // Image sequence virtual timeline mode
    if (is_exr_mode && image_sequence_timer_) {
        return image_sequence_timer_->GetDuration();
    }

    // Virtual timeline mode - get duration from timeline controller
    if (is_timeline_mode_ && timeline_controller_ &&
        timeline_controller_->IsVirtualTimelineMode()) {
        return timeline_controller_->GetDuration();
    }

    // When dual view timer is active or in lavfi mode, return virtual timeline duration
    if (dual_view_timer_ || !current_lavfi_filter_.empty()) {
        double virtual_duration = GetVirtualTimelineDuration();
        if (virtual_duration > 0) {
            return virtual_duration;
        }
    }
    return cached_duration;
}

double VideoPlayer::GetFrameRate() const {
    if (!mpv) return 23.976;

    double container_fps = 23.976;
    double estimated_fps = 23.976;

    bool has_container = mpv_get_property(mpv, "container-fps", MPV_FORMAT_DOUBLE, &container_fps) == 0;
    bool has_estimated = mpv_get_property(mpv, "estimated-vf-fps", MPV_FORMAT_DOUBLE, &estimated_fps) == 0;

    // For image sequences, prefer stored frame rate if available
    if (is_image_sequence && image_sequence_frame_rate > 0) {
        static bool logged_img_seq_fps = false;
        if (!logged_img_seq_fps) {
            //Debug::Log("VideoPlayer::GetFrameRate: Using stored image sequence frame rate: " + std::to_string(image_sequence_frame_rate));
            logged_img_seq_fps = true;
        }
        return image_sequence_frame_rate;
    }

    // For EXR sequences, prefer stored EXR frame rate if available
    if (exr_frame_rate > 0 && is_exr_mode) {
        //Debug::Log("VideoPlayer::GetFrameRate: Using stored EXR frame rate: " + std::to_string(exr_frame_rate));
        return exr_frame_rate;
    }

    double final_fps = 23.976;
    if (has_container && container_fps > 0) {
        final_fps = container_fps;
    } else if (has_estimated && estimated_fps > 0) {
        final_fps = estimated_fps;
    }

    // Debug log to trace FPS synchronization issues (only log when not image sequence to reduce spam)
    static bool logged_video_fps = false;
    if (!is_image_sequence && !logged_video_fps) {
      /*  Debug::Log("VideoPlayer::GetFrameRate: container=" + std::to_string(container_fps) +
                   ", estimated=" + std::to_string(estimated_fps) +
                   ", final=" + std::to_string(final_fps) +
                   ", exr_rate=" + std::to_string(exr_frame_rate) +
                   ", is_image_seq=" + (is_image_sequence ? "YES" : "NO") +
                   ", stored_img_fps=" + std::to_string(image_sequence_frame_rate));*/
        logged_video_fps = true;
    }

    return final_fps;
}

int VideoPlayer::GetTotalFrames() const {
    // For EXR/image sequence mode, use the timer duration (authoritative source)
    // This ensures correct frame count when switching between media items
    if (is_exr_mode && image_sequence_timer_) {
        double timer_duration = image_sequence_timer_->GetDuration();
        if (timer_duration > 0) {
            return static_cast<int>(std::round(timer_duration * GetFrameRate()));
        }
    }

    // For regular video, use cached_duration
    if (cached_duration <= 0) return 0;
    return static_cast<int>(std::round(cached_duration * GetFrameRate()));
}

int VideoPlayer::GetCurrentFrame() const {
    // For EXR/image sequence mode, use the timer position (authoritative source)
    if (is_exr_mode && image_sequence_timer_) {
        double timer_position = image_sequence_timer_->GetPosition();
        if (timer_position >= 0) {
            return static_cast<int>(std::round(timer_position * GetFrameRate()));
        }
    }

    // For regular video, use cached_position
    if (cached_position <= 0) return 0;
    return static_cast<int>(std::round(cached_position * GetFrameRate()));
}

bool VideoPlayer::IsImageSequence() const {
    return is_image_sequence;
}

int VideoPlayer::GetImageSequenceStartFrame() const {
    return image_sequence_start_frame;
}

void VideoPlayer::SeekToFrame(int frame_number) {
    if (frame_number < 0) frame_number = 0;

    double fps = GetFrameRate();
    int total_frames = GetTotalFrames();

    if (frame_number > total_frames) frame_number = total_frames;

    double position = frame_number / fps;
    Seek(position);
}

std::string VideoPlayer::FormatTimecode(double seconds, double fps) const {
    if (seconds < 0) seconds = 0;

    int hours = static_cast<int>(seconds / 3600);
    int minutes = static_cast<int>((seconds - hours * 3600) / 60);
    int secs = static_cast<int>(seconds) % 60;
    int frames = static_cast<int>((seconds - static_cast<int>(seconds)) * fps);

    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d:%02d", hours, minutes, secs, frames);
    return std::string(buffer);
}

// ============================================================================
// Rendering methods
// ============================================================================

void VideoPlayer::RenderVideoFrame() {
    UpdateProperties();

    // 🔧 CRITICAL: Process EXR textures EVERY frame (even in pause mode, even before has_video)
    // This ensures background-loaded pixels are converted to GL textures immediately
    if (exr_cache_) {
        exr_cache_->ProcessReadyTextures();
    }

    // 🔧 CRITICAL: Process timeline textures EVERY frame (like EXR above)
    // This ensures cache fills around playhead even when paused
    if (is_timeline_mode_ && timeline_controller_ && timeline_controller_->IsInitialized()) {
        // Only update cache if it exists (scratch timelines may not have a cache until clips are added)
        auto* cache = timeline_controller_->GetCache();
        if (cache) {
            // Update cache playhead from MPV position (enables pre-caching when paused)
            double pos = GetPosition();
            int frame = static_cast<int>(std::round(pos * timeline_controller_->GetFPS()));
            if (frame < 0) frame = 0;
            cache->UpdatePlayhead(frame, IsPlaying());
        }

        // Process pending GPU uploads from background I/O threads
        timeline_controller_->ProcessPendingUploads();
    }

    // NOTE: Timeline injection now handled inside UpdateVideoTexture() AFTER MPV render
    // This matches the EXR pattern exactly and prevents MPV from overwriting the injected frame

    // Always render comparison mode UI, even if no primary video is loaded
    if (comparison_mode_enabled_) {
        if (has_video && video_texture) {
            UpdateVideoTexture();
        }
        RenderVideoTexture();
    }
    else if (has_video && video_texture) {
        UpdateVideoTexture();
        RenderVideoTexture();
    }
    else {
        RenderPlaceholder();
    }
}

void VideoPlayer::RenderVideoTexture() {
    // Check for comparison mode rendering
    if (comparison_mode_enabled_) {
        // NEW: Lavfi modes render composited output directly to video_texture
        // They should use the standard single-video rendering path below
        if (IsLavfiMode(comparison_mode_)) {
            // Fall through to standard rendering - MPV already composited via lavfi
        } else {
            // Legacy dual-player comparison modes require manual compositing
            if (comparison_mode_ == ComparisonMode::DIFFERENCE_VIEW) {
                RenderDifference();
            } else if (comparison_mode_ == ComparisonMode::SPLIT_SCREEN) {
                RenderSplitScreen();
            } else {
                RenderSideBySide();
            }
            return;
        }
    }

    // Standard single video rendering (also used for lavfi composited output)
    float aspect_ratio = (float)video_width / (float)video_height;
    ImVec2 content_region = ImGui::GetContentRegionAvail();

    ImVec2 image_size;
    if (content_region.x / content_region.y > aspect_ratio) {
        image_size.y = content_region.y;
        image_size.x = content_region.y * aspect_ratio;
    }
    else {
        image_size.x = content_region.x;
        image_size.y = content_region.x / aspect_ratio;
    }

    // Center the image
    ImVec2 cursor_pos = ImGui::GetCursorPos();
    ImVec2 offset = ImVec2(
        (content_region.x - image_size.x) * 0.5f,
        (content_region.y - image_size.y) * 0.5f
    );
    ImGui::SetCursorPos(ImVec2(cursor_pos.x + offset.x, cursor_pos.y + offset.y));

    // Choose which texture to display (4-stage compositing pipeline)
    GLuint display_texture = video_texture;  // Default to video texture (Stage 1)

    // Stage 2: Use color-corrected texture if OCIO pipeline is active
    if (color_pipeline && color_pipeline->IsValid()) {
        // Make sure color_texture is a valid OpenGL texture
        if (color_texture > 0 && glIsTexture(color_texture)) {
            display_texture = color_texture;
            // Debug::Log("Using color-processed texture: " + std::to_string(color_texture));
        }
        else {
            // Color pipeline exists but texture not ready yet
            // Debug::Log("Color pipeline active but texture not ready, using video texture");
        }
    }

    // Stage 3: Use safety overlay texture if overlays are enabled and ready
    // DISABLED: Safety overlay texture selection disabled until SVG rendering implemented
    /*
    if (safety_overlay_system && safety_overlay_system->IsEnabled() && safety_overlay_system->IsReady()) {
        GLuint safety_texture = safety_overlay_system->GetOutputTexture();
        if (safety_texture > 0 && glIsTexture(safety_texture)) {
            display_texture = safety_texture;
            // Debug::Log("Using safety overlay texture: " + std::to_string(safety_texture));
        }
    }
    */

    // Safety check - make sure we have a valid texture to display
    // If display_texture is invalid, try video_texture (should be FBO-attached and cleared to background)
    // Last resort: use transition placeholder texture
    if (display_texture == 0 || !glIsTexture(display_texture)) {
        // Try video_texture first (FBO-attached, cleared to background color)
        if (video_texture != 0 && video_texture != display_texture && glIsTexture(video_texture)) {
            display_texture = video_texture;
        } else if (transition_placeholder_texture_ != 0 && glIsTexture(transition_placeholder_texture_)) {
            // Fallback to placeholder (not FBO-attached, but better than nothing)
            display_texture = transition_placeholder_texture_;
            // Recalculate image size for placeholder aspect ratio (1:1)
            if (content_region.x > content_region.y) {
                image_size.y = content_region.y;
                image_size.x = content_region.y;
            } else {
                image_size.x = content_region.x;
                image_size.y = content_region.x;
            }
            // Re-center with new size
            offset = ImVec2(
                (content_region.x - image_size.x) * 0.5f,
                (content_region.y - image_size.y) * 0.5f
            );
            ImGui::SetCursorPos(ImVec2(cursor_pos.x + offset.x, cursor_pos.y + offset.y));
        } else {
            // Last resort - no valid texture at all, just return
            return;
        }
    }

    // Save cursor position before rendering image for drop target
    ImVec2 image_start_pos = ImGui::GetCursorPos();
    ImVec2 image_screen_pos = ImGui::GetCursorScreenPos();

    // Display the texture
    ImGui::Image((void*)(intptr_t)display_texture, image_size);

    // Overrun mode overlay - shows when EXR cache can't keep up with playback
    if (exr_cache_ && exr_cache_->IsInOverrunMode() && font_mono) {
        ImDrawList* draw_list = ImGui::GetWindowDrawList();
        const char* overrun_text = "Not Realtime Playback: File too large";
        float font_size = 14.0f;

        // Calculate text size and position (bottom-left corner with padding)
        ImVec2 text_size = font_mono->CalcTextSizeA(font_size, FLT_MAX, 0.0f, overrun_text);
        float padding = 10.0f;
        ImVec2 text_pos(
            image_screen_pos.x + padding,
            image_screen_pos.y + image_size.y - text_size.y - padding
        );

        // Background rectangle for readability
        ImVec2 bg_min(text_pos.x - 4.0f, text_pos.y - 2.0f);
        ImVec2 bg_max(text_pos.x + text_size.x + 4.0f, text_pos.y + text_size.y + 2.0f);
        draw_list->AddRectFilled(bg_min, bg_max, IM_COL32(20, 20, 20, 200), 3.0f);

        // Draw text in system accent color
        ImVec4 accent = GetWindowsAccentColor();
        ImU32 text_color = IM_COL32(
            (int)(accent.x * 255),
            (int)(accent.y * 255),
            (int)(accent.z * 255),
            255
        );
        draw_list->AddText(font_mono, font_size, text_pos, text_color, overrun_text);
    }

    // Add drop target over the image
    ImGui::SetCursorPos(image_start_pos);
    ImGui::InvisibleButton("##ViewportDropTarget", image_size);

    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("MEDIA_ITEM")) {
            std::string media_id((const char*)payload->Data, payload->DataSize - 1);
            Debug::Log("Viewport drop received: " + media_id);
            viewport_drop_pending_id_ = media_id;
        }
        ImGui::EndDragDropTarget();
    }
}

void VideoPlayer::RenderPlaceholder() {
    ImVec2 content_region = ImGui::GetContentRegionAvail();
    ImVec2 center = ImVec2(content_region.x * 0.5f, content_region.y * 0.5f);

    /*ImGui::SetCursorPos(ImVec2(center.x - 100, center.y - 10));
    ImGui::TextDisabled("No video loaded");
    ImGui::SetCursorPos(ImVec2(center.x - 155, center.y + 20));
    ImGui::TextDisabled("Use File > Open Video (Ctrl + O)");*/
}

void VideoPlayer::RenderControls() {
    // Play/Pause button
    if (is_playing) {
        if (ImGui::Button("Pause")) {
            Pause();
        }
    }
    else {
        if (ImGui::Button("Play")) {
            Play();
        }
    }

    ImGui::SameLine();
    if (ImGui::Button("Stop")) {
        Stop();
    }

    // Frame stepping
    ImGui::SameLine();
    if (ImGui::Button("<")) {
        StepFrame(-1);
    }
    ImGui::SameLine();
    if (ImGui::Button(">")) {
        StepFrame(1);
    }

    // Volume control
    if (ImGui::SliderInt("Volume", &volume, 0, 100)) {
        SetVolume(volume);
    }

    // Load file button
    if (ImGui::Button("Load File...")) {
        LoadFile("test.mp4");
    }

    // EXR Cache Progress and Statistics (when in EXR mode)
    // Hide when in overrun mode - cache progress is not meaningful during frame-by-frame playback
    bool in_overrun = exr_cache_ && exr_cache_->IsInOverrunMode();
    if (is_exr_mode && HasEXRCache() && !in_overrun) {
        ImGui::Separator();
        ImGui::Text("EXR Cache Status:");

        auto cache_stats = GetEXRCacheStats();

        // Cache progress bar
        float cache_progress = (cache_stats.total_frames_in_sequence > 0) ?
            static_cast<float>(cache_stats.frames_cached) / static_cast<float>(cache_stats.total_frames_in_sequence) : 0.0f;

        ImGui::ProgressBar(cache_progress, ImVec2(-1.0f, 0.0f),
            (std::to_string(cache_stats.frames_cached) + "/" +
             std::to_string(cache_stats.total_frames_in_sequence) + " frames cached").c_str());

        // Cache statistics
        ImGui::Text("Hit Ratio: %.1f%% (%d hits, %d misses)",
                    cache_stats.hit_ratio * 100.0, cache_stats.cache_hits, cache_stats.cache_misses);

        ImGui::Text("Memory Usage: %zu MB", cache_stats.memory_usage_mb);

        if (cache_stats.background_thread_active) {
            ImGui::Text("Background Processing: Active (Avg load: %.1fms)",
                        cache_stats.average_load_time_ms);
        } else {
            ImGui::Text("Background Processing: Inactive");
        }
    }
}

void CheckGLError(const std::string& location) {
    GLenum err;
    while ((err = glGetError()) != GL_NO_ERROR) {
        std::string error;
        switch (err) {
        case GL_INVALID_ENUM: error = "INVALID_ENUM"; break;
        case GL_INVALID_VALUE: error = "INVALID_VALUE"; break;
        case GL_INVALID_OPERATION: error = "INVALID_OPERATION"; break;
        case GL_OUT_OF_MEMORY: error = "OUT_OF_MEMORY"; break;
        case GL_INVALID_FRAMEBUFFER_OPERATION: error = "INVALID_FRAMEBUFFER_OPERATION"; break;
        default: error = std::to_string(err); break;
        }
        Debug::Log("GL Error at " + location + ": " + error);
    }
}

void VideoPlayer::UpdateVideoTexture() {
    // 🔧 ProcessReadyTextures() now called in RenderVideoFrame() before this function

    if (!mpv_gl) {
        //Debug::Log("UpdateVideoTexture: No mpv_gl context");
        return;
    }

    // Don't update if we don't have valid dimensions
    if (video_width <= 0 || video_height <= 0) {
        //Debug::Log("UpdateVideoTexture: Invalid video dimensions");
        return;
    }

    video_gpu_scheduler.BeginFrame();

    // CRITICAL: For vo=libmpv, we must render continuously at display rate,
    // NOT gated on mpv_render_context_update(). This keeps the GPU pipeline warm
    // and prevents 4K playback stutter. The update() call is only used to
    // track if content changed, not to decide whether to render.
    mpv_render_context_update(mpv_gl);

    video_gpu_scheduler.CooperativeYield();

    // Make sure we have valid FBOs and textures (including new separate MPV resources)
    if (fbo == 0 || video_texture == 0 || mpv_fbo == 0 || mpv_texture == 0) {
        //Debug::Log("UpdateVideoTexture: FBO or texture resources not initialized!");
        return;
    }

    // Use cached pipeline format for MPV FBO (avoids expensive map lookup every frame)
    GLenum internal_format = current_internal_format;

    // NEW: MPV renders to separate FBO to break pipeline stalls
    mpv_opengl_fbo mpv_fbo_data = {
        static_cast<int>(mpv_fbo),  // Use separate MPV FBO
        video_width,
        video_height,
        static_cast<int>(internal_format)  // ← KEY: Tell MPV the target format!
    };

    int flip_y = 0;
    int block_for_target_time = 0;

    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_OPENGL_FBO, &mpv_fbo_data},
        {MPV_RENDER_PARAM_FLIP_Y, &flip_y},
        {MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME, &block_for_target_time},
        {MPV_RENDER_PARAM_INVALID, nullptr}
    };

    // Render to separate MPV FBO (no pipeline stall)
    mpv_render_context_render(mpv_gl, params);

    // NEW: Fast blit from MPV texture to main video texture (breaks dependency chain)
    glBindFramebuffer(GL_READ_FRAMEBUFFER, mpv_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
    glBlitFramebuffer(0, 0, video_width, video_height,
                      0, 0, video_width, video_height,
                      GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    video_gpu_scheduler.CooperativeYield();

    // 🔧 IMAGE SEQUENCE TIMER UPDATE: Advance timer if playing (virtual timeline mode)
    if (is_exr_mode && image_sequence_timer_) {
        bool in_overrun = exr_cache_ && exr_cache_->IsInOverrunMode();

        if (in_overrun) {
            // OVERRUN MODE: Pause timer - we'll step frame-by-frame in InjectCurrentEXRFrame()
            // This prevents the playhead from running ahead while we load frames synchronously
            if (image_sequence_timer_->IsPlaying()) {
                image_sequence_timer_->Pause();
                Debug::Log("VideoPlayer: Timer paused for overrun mode");
            }
        } else {
            // Normal mode or exiting overrun
            // Resume timer if we were in overrun mode (is_playing is true but timer paused)
            if (is_playing && !image_sequence_timer_->IsPlaying()) {
                image_sequence_timer_->Play();
                Debug::Log("VideoPlayer: Timer resumed after overrun mode");
            }
            if (image_sequence_timer_->IsPlaying()) {
                image_sequence_timer_->Update();
            }
        }
        cached_position = image_sequence_timer_->GetPosition();
    }

    // 🔧 EXR INJECTION POINT: Replace dummy video with current EXR frame
    if (is_exr_mode && !exr_sequence_files.empty()) {
        InjectCurrentEXRFrame();

        // 🔧 IMMEDIATE OVERRUN CHECK: Pause timer right after overrun is detected
        // This is critical because overrun mode is set INSIDE GetFrameOrLoad(),
        // which is called from InjectCurrentEXRFrame(). Without this check,
        // there's a one-frame delay before the timer is paused.
        if (image_sequence_timer_ && exr_cache_ && exr_cache_->IsInOverrunMode()) {
            if (image_sequence_timer_->IsPlaying()) {
                image_sequence_timer_->Pause();
                Debug::Log("VideoPlayer: Timer paused immediately after overrun detected");
            }
        }

        // 🔧 REMOVED: TriggerEXRFrameCaching() - FFmpeg cache system not used for EXR
        // DirectEXRCache handles all EXR caching with native OpenEXR + memory-mapping

        // 🔧 ProcessReadyTextures() now called unconditionally at start of UpdateVideoTexture()
    }

    // 🔧 TIMELINE INJECTION POINT: Replace dummy video with current timeline frame
    // This MUST happen AFTER the MPV render (like EXR) to prevent MPV from overwriting the frame
    if (is_timeline_mode_ && timeline_controller_) {
        static int timeline_inject_call_count = 0;
        timeline_inject_call_count++;
        /*if (timeline_inject_call_count <= 10 || timeline_inject_call_count % 300 == 0) {
            Debug::Log("UpdateVideoTexture: Calling InjectCurrentTimelineFrame (call #" +
                       std::to_string(timeline_inject_call_count) + ")");
        }*/
        InjectCurrentTimelineFrame();
    }

    // Apply color pipeline if active
    if (color_pipeline && color_pipeline->IsValid()) {
        // Only apply if we have valid resources
        if (color_fbo != 0 && color_texture != 0) {
            ApplyColorPipeline();
        }
        else {
            //Debug::Log("UpdateVideoTexture: Color resources not ready, initializing...");
            SetupColorProcessingResources();
        }
    }
}

void VideoPlayer::UpdateProperties() {
    if (!mpv) return;

    // OVERLAY MODES: Use cached content dimensions, ignore MPV's dummy video dimensions
    // This enables 1x1 dummies for EXR/image sequences and timeline mode
    if (use_content_dimensions_ && content_width_ > 0 && content_height_ > 0) {
        if (video_width != content_width_ || video_height != content_height_) {
            video_width = content_width_;
            video_height = content_height_;
            /*Debug::Log("Using cached content dimensions: " +
                       std::to_string(video_width) + "x" + std::to_string(video_height));*/
            CreateVideoTextures(video_width, video_height);

            // If color pipeline exists, also recreate color processing resources
            if (color_pipeline && color_pipeline->IsValid()) {
                SetupColorProcessingResources();
            }

            // Notify UI of dimension change
            if (dimension_change_callback) {
                dimension_change_callback(video_width, video_height);
            }
        }

        // Still need to update duration and FPS for timeline/sequence playback
        double dur = 0.0;
        if (mpv_get_property(mpv, "duration", MPV_FORMAT_DOUBLE, &dur) == 0 && dur > 0) {
            cached_duration = dur;
        }
        cached_fps = GetFrameRate();

        return;  // Skip MPV dimension queries entirely
    }

    // Use cached FFmpeg metadata for static properties (instant, no MPV queries)
    // Skip for EXR sequences - they use DirectEXRCache, not FFmpeg metadata
    // Skip for lavfi mode - dimensions come from MPV's composited output, not source files
    bool should_use_metadata = metadata_callback && !current_file_path.empty() &&
                                current_file_path.find("exr://") != 0 && !is_exr_mode &&
                                !IsLavfiMode(comparison_mode_);

    if (should_use_metadata) {
        VideoMetadata meta = metadata_callback(current_file_path);
        if (meta.is_loaded) {
            // Duration from metadata
            if (meta.total_frames > 0 && meta.frame_rate > 0) {
                cached_duration = meta.total_frames / meta.frame_rate;
            }
            // FPS from metadata
            cached_fps = meta.frame_rate;

            // Video dimensions from metadata
            int new_width = meta.width;
            int new_height = meta.height;

            // Check if video dimensions changed
            if (new_width != video_width || new_height != video_height) {
                if (new_width > 0 && new_height > 0) {
                    video_width = new_width;
                    video_height = new_height;
                    Debug::Log("Video dimensions updated from cached metadata: " +
                               std::to_string(video_width) + "x" + std::to_string(video_height));

                    // Recreate video textures with new dimensions
                    CreateVideoTextures(video_width, video_height);

                    // If color pipeline exists, also recreate color processing resources
                    if (color_pipeline && color_pipeline->IsValid()) {
                        SetupColorProcessingResources();
                    }

                    // If safety overlay system exists, update its dimensions
                    // DISABLED: Safety overlay dimension updates disabled until SVG rendering implemented
                    /*
                    if (safety_overlay_system && safety_overlay_system->IsReady()) {
                        safety_overlay_system->UpdateDimensions(video_width, video_height);
                    }
                    */

                    // Notify UI of dimension change
                    if (dimension_change_callback) {
                        dimension_change_callback(video_width, video_height);
                    }
                }
            }
        }
    } else {
        // Fallback: Query MPV only if cached metadata not available
        double dur = 0.0;
        if (mpv_get_property(mpv, "duration", MPV_FORMAT_DOUBLE, &dur) == 0 && dur > 0) {
            cached_duration = dur;
        }

        cached_fps = GetFrameRate();

        int64_t width = 0, height = 0;
        int new_width = video_width;
        int new_height = video_height;

        int width_result = mpv_get_property(mpv, "video-params/w", MPV_FORMAT_INT64, &width);
        int height_result = mpv_get_property(mpv, "video-params/h", MPV_FORMAT_INT64, &height);

        if (width_result == 0) {
            new_width = (int)width;
        }
        if (height_result == 0) {
            new_height = (int)height;
        }

        // TIMELINE MODE FALLBACK: If MPV isn't returning dimensions but we're in timeline mode,
        // use default 1920x1080 to ensure video_texture gets created
        // This MUST happen before the dimension change check
        if (new_width == 0 && new_height == 0 && is_timeline_mode_ && video_texture == 0) {
            new_width = 1920;
            new_height = 1080;
            Debug::Log("UpdateProperties: TIMELINE FALLBACK - MPV returned 0x0, using 1920x1080 for timeline mode");
        }

        // Debug: Log MPV dimension queries - always log when timeline mode is active and texture missing
        static int mpv_dim_log_count = 0;
        mpv_dim_log_count++;
        bool should_log = (mpv_dim_log_count <= 10 || mpv_dim_log_count % 300 == 0);
        // Always log when in timeline mode with missing texture
   /*     if (is_timeline_mode_ && video_texture == 0) {
            should_log = true;
        }
        if (should_log) {
            Debug::Log("UpdateProperties: MPV dims: " + std::to_string(width) + "x" + std::to_string(height) +
                       " (results: " + std::to_string(width_result) + "/" + std::to_string(height_result) + ")" +
                       ", video_texture=" + std::to_string(video_texture) +
                       ", current: " + std::to_string(video_width) + "x" + std::to_string(video_height) +
                       ", timeline_mode=" + (is_timeline_mode_ ? "true" : "false") +
                       ", new: " + std::to_string(new_width) + "x" + std::to_string(new_height));
        }*/

        // Check if video dimensions changed
        if (new_width != video_width || new_height != video_height) {
            if (new_width > 0 && new_height > 0) {
                video_width = new_width;
                video_height = new_height;
                Debug::Log("Video dimensions changed to: " + std::to_string(video_width) + "x" + std::to_string(video_height));

                // Recreate video textures with new dimensions
                CreateVideoTextures(video_width, video_height);

                // If color pipeline exists, also recreate color processing resources
                if (color_pipeline && color_pipeline->IsValid()) {
                    SetupColorProcessingResources();
                }

                // If safety overlay system exists, update its dimensions
                // DISABLED: Safety overlay dimension updates disabled until SVG rendering implemented
                /*
                if (safety_overlay_system && safety_overlay_system->IsReady()) {
                    safety_overlay_system->UpdateDimensions(video_width, video_height);
                }
                */

                // Notify UI of dimension change
                if (dimension_change_callback) {
                    dimension_change_callback(video_width, video_height);
                }
            }
        }
    }

    // 🔧 FINAL TIMELINE FALLBACK: Regardless of which branch above was taken,
    // if we're in timeline mode and video_texture is still 0, force create textures
    if (is_timeline_mode_ && video_texture == 0 && video_width == 0 && video_height == 0) {
        Debug::Log("UpdateProperties: TIMELINE FINAL FALLBACK - No dimensions from metadata or MPV, forcing 1920x1080");
        video_width = 1920;
        video_height = 1080;
        CreateVideoTextures(video_width, video_height);
       /* Debug::Log("UpdateProperties: Created video textures for timeline mode: " +
                   std::to_string(video_width) + "x" + std::to_string(video_height) +
                   ", video_texture=" + std::to_string(video_texture));*/
    }

    // Only query MPV for LIVE playback state (these must come from MPV)
    double pos = 0.0;
    if (mpv_get_property(mpv, "time-pos", MPV_FORMAT_DOUBLE, &pos) == 0) {
        cached_position = pos;
    }

    // When dual view timer is active, timer controls is_playing state, not MPV
    if (!dual_view_timer_) {
        int pause_state = 0;
        if (mpv_get_property(mpv, "pause", MPV_FORMAT_FLAG, &pause_state) == 0) {
            is_playing = !pause_state;
        }
    }

    // Event-driven sync for comparison video (debounced seek only)
    if (comparison_video_ && comparison_video_->HasVideo()) {
        // When using PlaybackTimer, the timer controls all timing and sync
        // Skip legacy position tracking/loop detection - timer handles it via callbacks
        if (dual_view_timer_) {
            // Timer mode: UpdateDualViewTimer() handles all syncing
            // Just process pending loop sync if needed
            if (loop_sync_pending_) {
                double now = glfwGetTime();
                if (now - loop_sync_time_ > 0.25) {  // 250ms settle time
                    dual_view_timer_->Play();
                    loop_sync_pending_ = false;
                }
            }
        } else {
            // Legacy mode (no timer): Use MPV-driven position tracking
            double effective_duration = GetVirtualTimelineDuration();
            if (effective_duration <= 0) {
                effective_duration = cached_duration;  // Fallback to primary duration
            }

            // Detect loop point (position jumped from near-end to near-start)
            bool loop_detected = false;
            if (loop_enabled && effective_duration > 0) {
                // Loop detected: was at >90% duration, now at <10% duration
                if (last_position_ > (effective_duration * 0.9) &&
                    cached_position < (effective_duration * 0.1)) {
                    loop_detected = true;
                }
            }

            if (loop_detected) {
                // Pause both videos at loop point
                if (is_playing) {
                    Pause();
                }

                // Sync comparison video to start position (accounting for offset)
                double secondary_pos = CalculateSecondaryPosition(0.0);
                comparison_video_->SyncToPosition(secondary_pos);

                // Start timer for delayed resume (allow seek to settle)
                loop_sync_pending_ = true;
                loop_sync_time_ = glfwGetTime();
            }

            // Check if loop sync delay has elapsed (250ms for larger videos)
            if (loop_sync_pending_) {
                double now = glfwGetTime();
                if (now - loop_sync_time_ > 0.25) {  // 250ms settle time
                    Play();
                    loop_sync_pending_ = false;
                }
            }

            // Immediate seek sync (no debounce - legacy view modes are for preview only)
            if (last_seek_time_ > 0.0) {
                double secondary_pos = CalculateSecondaryPosition(cached_position);
                comparison_video_->SyncToPosition(secondary_pos);
                if (was_playing_before_seek_) {
                    comparison_video_->SyncPlaybackState(true);
                }
                last_seek_time_ = 0.0;  // Reset
            }
        }

        // NOTE: UpdateVideoTexture() is called in RenderSideBySide(), not here
        // Play/Pause sync is handled in Play() and Pause() methods (truly event-driven)
    }

    // Track position for loop detection
    last_position_ = cached_position;
}

void VideoPlayer::UpdatePlaybackState() {
    if (!mpv) return;

    // Only update DYNAMIC playback state (called every frame from RenderVideoFrame)
    // Static properties (duration, fps, dimensions) are updated once in UpdateProperties()

    double pos = 0.0;
    if (mpv_get_property(mpv, "time-pos", MPV_FORMAT_DOUBLE, &pos) == 0) {
        cached_position = pos;
    }

    // When dual view timer is active, timer controls is_playing state, not MPV
    // MPV is always paused in timer mode - timer drives position via seeks
    if (!dual_view_timer_) {
        int pause_state = 0;
        if (mpv_get_property(mpv, "pause", MPV_FORMAT_FLAG, &pause_state) == 0) {
            is_playing = !pause_state;
        }
    }
    // Note: When dual_view_timer_ is active, is_playing is controlled by Play()/Pause() methods

    // Note: Comparison video sync is handled in UpdateProperties() via event-driven approach
    // (sync only on play/pause/seek events, not continuous polling)
}

void VideoPlayer::ResetState() {
    Debug::Log("ResetState: Starting (has_video=" +
               std::string(has_video ? "true" : "false") + ")");

    // Note: We keep has_video = true during transition so the render path
    // continues to use the transition placeholder texture instead of RenderPlaceholder()
    // which does nothing and causes font cache flicker
    cached_duration = 0.0;
    cached_position = 0.0;

    // Clear current file path to prevent stale metadata lookups during transitions
    current_file_path.clear();

    // === UNCONDITIONAL CACHE CLEANUP ===
    // Always clean up state, regardless of previous media type
    // This ensures consistent behavior for all transitions

    Debug::Log("ResetState: Cleaning up media state");

    // FIRST: Switch video_texture to placeholder BEFORE stopping MPV or deleting any textures
    // This prevents displaying deleted/invalid textures during the transition window
    video_texture = transition_placeholder_texture_;
    video_width = transition_placeholder_width_;
    video_height = transition_placeholder_height_;
    has_video = true;

    // Clear color texture to background to prevent showing stale frames
    ClearColorTextureToBackground();

    // Now safe to stop MPV
    if (mpv) {
        const char* cmd[] = { "stop", nullptr };
        mpv_command(mpv, cmd);
    }

    // Clean up EXR/image sequence state if active
    if (is_exr_mode) {
        Debug::Log("ResetState: Cleaning up EXR/image sequence state");

        // Stop and reset image sequence timer (virtual timeline mode)
        if (image_sequence_timer_) {
            image_sequence_timer_->Pause();
            image_sequence_timer_.reset();
            Debug::Log("ResetState: Image sequence timer cleaned up");
        }

        is_exr_mode = false;
        exr_sequence_files.clear();
        exr_layer_name.clear();
        image_sequence_format.clear();
        exr_current_frame = 0;
        exr_frame_count = 0;
        exr_frame_rate = 24.0;
        exr_sequence_start_frame = 0;

        // Clear EXR caching callback
        exr_caching_callback = nullptr;

        // Safe to delete exr_texture now since video_texture no longer points to it
        if (exr_texture != 0) {
            glDeleteTextures(1, &exr_texture);
        }
        exr_texture = 0;
        exr_texture_width = 0;
        exr_texture_height = 0;
    }

    // NOTE: We intentionally do NOT delete fbo here - it persists across transitions

    Debug::Log("ResetState: State reset complete");
}

void VideoPlayer::WaitForFileLoad(bool is_audio_file) {
    // Faster polling (50ms) for more responsive loading
    const int max_attempts = is_audio_file ? 60 : 200;  // 3s vs 10s with 50ms polls
    int attempts = 0;

    Debug::Log("WaitForFileLoad: Starting (audio=" + std::string(is_audio_file ? "true" : "false") +
               ", max_wait=" + std::to_string(max_attempts * 0.05) + "s)");

    while (attempts < max_attempts) {
        mpv_event* event = mpv_wait_event(mpv, 0.05);

        if (event->event_id == MPV_EVENT_FILE_LOADED) {
            Debug::Log("WaitForFileLoad: FILE_LOADED event received");
            break;
        }

        // Check for errors that indicate we should stop waiting
        if (event->event_id == MPV_EVENT_END_FILE) {
            mpv_event_end_file* end_file = (mpv_event_end_file*)event->data;
            if (end_file && end_file->error < 0) {
                Debug::Log("WaitForFileLoad: END_FILE error: " +
                          std::string(mpv_error_string(end_file->error)));
                break;
            }
        }

        attempts++;

        // For audio files, don't require duration to continue
        // Audio might initialize async and duration comes later
        double duration = 0.0;
        if (mpv_get_property(mpv, "duration", MPV_FORMAT_DOUBLE, &duration) == 0 && duration > 0) {
            Debug::Log("WaitForFileLoad: Duration available: " + std::to_string(duration) + "s");
            break;
        }

        // Log progress every second for troubleshooting (20 attempts = 1s at 50ms)
        if (attempts % 20 == 0 && attempts > 0) {
            Debug::Log("WaitForFileLoad: Still waiting... (" + std::to_string(attempts/20) + "s elapsed)");
        }
    }

    if (attempts >= max_attempts) {
        Debug::Log("WaitForFileLoad: TIMEOUT after " + std::to_string(attempts * 0.05) +
                   "s - proceeding anyway");
    } else {
        Debug::Log("WaitForFileLoad: Completed in " + std::to_string(attempts * 0.05) + "s");
    }
}

void VideoPlayer::FinalizeLoad() {
    Debug::Log("FinalizeLoad: Starting");
    UpdateProperties();

    // Special handling for EDL files (MPV's Edit Decision Lists)
    // EDL files may not report video dimensions immediately, but they're always video
    bool is_edl = (current_file_path.find("edl://") == 0);

    // Special handling for dummy videos (used by timeline playback)
    // Dummy videos may not report duration immediately due to async load
    bool is_dummy = (current_file_path.find("dummy_") != std::string::npos &&
                     current_file_path.find(".mp4") != std::string::npos);

    if (cached_duration > 0) {
        has_video = true;
        Debug::Log("FinalizeLoad: Media loaded successfully (duration=" +
                   std::to_string(cached_duration) + "s, has_video=true)");
    }
    else if (is_edl) {
        // EDL files are trimmed video segments - force has_video=true
        has_video = true;
        Debug::Log("FinalizeLoad: EDL file detected - forcing has_video=true (duration may not be available yet)");
    }
    else if (is_dummy) {
        // Dummy videos for timeline playback - force has_video=true
        has_video = true;
        Debug::Log("FinalizeLoad: Dummy video detected - forcing has_video=true (timeline playback mode)");
    }
    else {
        Debug::Log("FinalizeLoad: WARNING - No duration available (has_video=false)");
        has_video = false;
    }

    Debug::Log("FinalizeLoad: Complete");
}

std::vector<std::string> VideoPlayer::ParseEDLContent(const std::string& edl_content) {
    std::vector<std::string> file_paths;
    std::istringstream stream(edl_content);
    std::string line;

    while (std::getline(stream, line)) {
        if (!line.empty()) {
            file_paths.push_back(line);
            //Debug::Log("Added to playlist: " + line);
        }
    }

    return file_paths;
}

void VideoPlayer::LoadPlaylistFiles(const std::vector<std::string>& file_paths) {
    const char* clear_cmd[] = { "playlist-clear", nullptr };
    mpv_command(mpv, clear_cmd);

    //Debug::Log("Loading first file: " + file_paths[0]);
    ResetState();

    const char* cmd[] = { "loadfile", file_paths[0].c_str(), nullptr };
    if (mpv_command(mpv, cmd) < 0) {
        Debug::Log("Failed to send loadfile command");
        return;
    }

    for (size_t i = 1; i < file_paths.size(); i++) {
        //Debug::Log("Appending to playlist: " + file_paths[i]);
        const char* args[] = { "loadfile", file_paths[i].c_str(), "append", nullptr };
        int result = mpv_command(mpv, args);

        if (result < 0) {
            Debug::Log("Failed to append file (error: " + std::to_string(result) + ")");
        }
    }

    WaitForFileLoad();
    FinalizeLoad();
    //Debug::Log("Playlist loaded with " + std::to_string(file_paths.size()) + " files");
}

// ============================================================================
// Metadata and file information methods
// ============================================================================

// LEGACY: This method queries MPV for metadata (50-100ms blocking)
// PREFER: Use FFmpegMetadataExtractor before loading into MPV (much faster)
// This is kept ONLY as a fallback for edge cases where FFmpeg metadata is unavailable
VideoMetadata VideoPlayer::ExtractMetadata() const {
    Debug::Log("ExtractMetadata: WARNING - Using legacy MPV metadata query (slow, prefer FFmpeg)");

    VideoMetadata metadata;

    if (!mpv) {
        return metadata;
    }

    char* path_result = nullptr;
    if (mpv_get_property(mpv, "path", MPV_FORMAT_STRING, &path_result) == 0 && path_result) {
        metadata.PopulateBasicFileInfo(std::string(path_result));
        mpv_free(path_result);
    }

    // Populate video properties
    metadata.width = GetVideoWidth();
    metadata.height = GetVideoHeight();
    metadata.frame_rate = GetFrameRate();
    metadata.total_frames = GetTotalFrames();
    metadata.video_codec = GetVideoCodec();
    metadata.pixel_format = GetPixelFormat();
    metadata.colorspace = GetColorspace();
    metadata.color_primaries = GetColorPrimaries();
    metadata.color_transfer = GetColorTrc();

    // NEW: Add color range extraction
    metadata.range_type = GetColorRange();

    // NEW: Cache 4:1:1 and 4:2:1 format detection
    metadata.is_411_format = metadata.Is411Format();
    metadata.is_421_format = metadata.Is421Format();

    // NOTE: NCLC detection moved to lazy evaluation in DisplayColorPropertiesTable()

    // Populate audio properties
    metadata.audio_codec = GetAudioCodec();
    metadata.audio_sample_rate = GetSampleRate();
    metadata.audio_channels = GetAudioChannels();

    if (metadata.file_size == 0) {
        metadata.file_size = GetFileSize();
    }

    metadata.is_loaded = true;
    return metadata;
}

// REMOVED: ExtractMetadataFast() and ExtractCriticalMetadata()
// These methods queried MPV for metadata, forcing frame decode
// Now replaced by FFmpegMetadataExtractor which extracts metadata
// BEFORE loading into MPV (much faster, no frame decode required)
//
// For any legacy code that needs these:
// - Use FFmpegMetadataExtractor::Extract() instead (preferred)
// - Or use ExtractMetadata() below (legacy MPV query)

VideoMetadata VideoPlayer::ExtractEXRMetadata(const std::vector<std::string>& sequence_files,
                                             const std::string& layer_name,
                                             double fps) const {
    VideoMetadata metadata;

    if (sequence_files.empty()) {
        Debug::Log("ExtractEXRMetadata: Invalid parameters - empty sequence");
        return metadata;
    }

    // Get dimensions from first frame using DirectEXRCache
    int width, height;
    if (!ump::DirectEXRCache::GetFrameDimensions(sequence_files[0], width, height)) {
        //Debug::Log("ExtractEXRMetadata: Failed to get dimensions from first EXR file: " + sequence_files[0]);
        return metadata;
    }

    Debug::Log("ExtractEXRMetadata: Successfully extracted dimensions: " + std::to_string(width) + "x" + std::to_string(height));

    // Set video dimensions
    metadata.width = width;
    metadata.height = height;

    // EXR-specific properties
    metadata.pixel_format = "rgba16f";
    metadata.video_codec = "exr";
    metadata.bit_depth = 16;
    metadata.colorspace = "linear";  // EXR is typically linear
    metadata.color_primaries = "unknown";  // Could be extracted from EXR headers later
    metadata.color_transfer = "linear";

    // Sequence-specific metadata
    metadata.frame_rate = fps;
    metadata.total_frames = static_cast<int>(sequence_files.size());

    // File information from first frame
    metadata.PopulateBasicFileInfo(sequence_files[0]);

    // Override filename to show sequence info
    std::filesystem::path first_path(sequence_files[0]);
    std::string base_name = first_path.stem().string();

    // Extract sequence base name (remove frame number)
    std::regex pattern(R"(^(.+)([_\.\-])(\d+)$)");
    std::smatch match;
    if (std::regex_match(base_name, match, pattern)) {
        std::string sequence_base = match[1].str();
        metadata.file_name = sequence_base + "_[" + std::to_string(sequence_files.size()) + "_frames]" + first_path.extension().string();
    } else {
        metadata.file_name = base_name + "_sequence" + first_path.extension().string();
    }

    // Additional sequence info
    metadata.file_size = 0;  // Could sum all files, but might be expensive
    for (const auto& file : sequence_files) {
        try {
            metadata.file_size += std::filesystem::file_size(file);
        } catch (...) {
            // Skip files that can't be read
        }
    }

    metadata.is_loaded = true;
    Debug::Log("ExtractEXRMetadata: Successfully created metadata for EXR sequence: " + metadata.file_name);

    return metadata;
}

double VideoPlayer::ProbeFileDuration(const std::string& file_path) {
    // REFACTORED: Use FFmpeg instead of creating temporary MPV instance
    // This is ~250-500x faster (10ms vs up to 5000ms)
    return ump::FFmpegMetadataExtractor::ProbeDuration(file_path);
}

void VideoPlayer::InitializeForEmptySequence(double default_duration) {
    cached_duration = default_duration;
    cached_position = 0.0;
    is_playing = false;

    if (mpv) {
        const char* cmd[] = { "stop", nullptr };
        mpv_command(mpv, cmd);
    }
}

// Video codec and format methods
std::string VideoPlayer::GetVideoCodec() const {
    if (!mpv) return "Unknown";

    char* result = nullptr;
    if (mpv_get_property(mpv, "video-codec", MPV_FORMAT_STRING, &result) == 0 && result) {
        std::string codec(result);
        mpv_free(result);
        return codec;
    }
    return "Unknown";
}

std::string VideoPlayer::GetPixelFormat() const {
    if (!mpv) return "Unknown";

    char* result = nullptr;
    if (mpv_get_property(mpv, "video-params/pixelformat", MPV_FORMAT_STRING, &result) == 0 && result) {
        std::string format(result);
        mpv_free(result);
        return format;
    }
    return "Unknown";
}

double VideoPlayer::GetVideoBitrate() const {
    if (!mpv) return 0.0;

    double bitrate = 0.0;
    if (mpv_get_property(mpv, "video-bitrate", MPV_FORMAT_DOUBLE, &bitrate) == 0) {
        return bitrate / 1000.0;
    }

    if (mpv_get_property(mpv, "packet-video-bitrate", MPV_FORMAT_DOUBLE, &bitrate) == 0) {
        return bitrate / 1000.0;
    }

    return 0.0;
}

int64_t VideoPlayer::GetFileSize() const {
    if (!mpv) return 0;

    int64_t size = 0;
    mpv_get_property(mpv, "file-size", MPV_FORMAT_INT64, &size);
    return size;
}

std::string VideoPlayer::GetMetadata(const std::string& key) const {
    if (!mpv) return "Unknown";

    std::string property_path = "metadata/" + key;
    char* result = nullptr;
    if (mpv_get_property(mpv, property_path.c_str(), MPV_FORMAT_STRING, &result) == 0 && result) {
        std::string metadata(result);
        mpv_free(result);
        return metadata;
    }
    return "";
}

// Color information methods
std::string VideoPlayer::GetColorspace() const {
    if (!mpv) return "Unknown";

    char* result = nullptr;
    if (mpv_get_property(mpv, "video-params/colormatrix", MPV_FORMAT_STRING, &result) == 0 && result) {
        std::string colorspace(result);
        mpv_free(result);
        return colorspace;
    }
    return "Unknown";
}

std::string VideoPlayer::GetColorPrimaries() const {
    if (!mpv) return "Unknown";

    char* result = nullptr;
    if (mpv_get_property(mpv, "video-params/primaries", MPV_FORMAT_STRING, &result) == 0 && result) {
        std::string primaries(result);
        mpv_free(result);
        return primaries;
    }
    return "Unknown";
}

std::string VideoPlayer::GetColorTrc() const {
    if (!mpv) return "Unknown";

    char* result = nullptr;
    if (mpv_get_property(mpv, "video-params/gamma", MPV_FORMAT_STRING, &result) == 0 && result) {
        std::string trc(result);
        mpv_free(result);
        return trc;
    }
    return "Unknown";
}

std::string VideoPlayer::GetColorRange() const {
    if (!mpv) return "Unknown";

    char* result = nullptr;
    if (mpv_get_property(mpv, "video-params/colorrange", MPV_FORMAT_STRING, &result) == 0 && result) {
        std::string range(result);
        mpv_free(result);
        return range;
    }
    return "Unknown";
}

// Audio methods
bool VideoPlayer::HasAudio() const {
    if (!mpv) return false;

    int64_t track_count = 0;
    if (mpv_get_property(mpv, "track-list/count", MPV_FORMAT_INT64, &track_count) == 0) {
        for (int64_t i = 0; i < track_count; i++) {
            char property_path[64];
            snprintf(property_path, sizeof(property_path), "track-list/%lld/type", i);

            char* track_type = nullptr;
            if (mpv_get_property(mpv, property_path, MPV_FORMAT_STRING, &track_type) == 0 && track_type) {
                std::string type(track_type);
                mpv_free(track_type);
                if (type == "audio") {
                    return true;
                }
            }
        }
    }

    char* audio_codec = nullptr;
    if (mpv_get_property(mpv, "audio-codec-name", MPV_FORMAT_STRING, &audio_codec) == 0 && audio_codec) {
        bool has_audio = (strlen(audio_codec) > 0 && strcmp(audio_codec, "none") != 0);
        mpv_free(audio_codec);
        return has_audio;
    }

    return false;
}

bool VideoPlayer::IsReadyToRender() const {
    // Check all the same conditions as UpdateVideoTexture
    if (!mpv_gl) {
        return false;
    }
    
    if (!has_video || video_texture == 0) {
        return false;
    }
    
    if (video_width <= 0 || video_height <= 0) {
        return false;
    }
    
    if (fbo == 0) {
        return false;
    }
    
    // Check if MPV has a new frame ready to render
    int needs_render = mpv_render_context_update(mpv_gl);
    return needs_render > 0;
}

bool VideoPlayer::IsRenderInfrastructureReady() const {
    // Check if basic rendering infrastructure is ready (without requiring fresh MPV frame)
    if (!mpv_gl) {
        return false;
    }
    
    if (!has_video || video_texture == 0) {
        return false;
    }
    
    if (video_width <= 0 || video_height <= 0) {
        return false;
    }
    
    if (fbo == 0) {
        return false;
    }
    
    // Don't check for fresh MPV frame - just verify infrastructure is ready
    return true;
}

std::string VideoPlayer::GetAudioCodec() const {
    if (!mpv) return "None";

    char* result = nullptr;
    if (mpv_get_property(mpv, "audio-codec-name", MPV_FORMAT_STRING, &result) == 0 && result) {
        std::string codec(result);
        mpv_free(result);

        if (codec.empty() || codec == "none") {
            return "None";
        }
        return codec;
    }
    return "None";
}

int VideoPlayer::GetSampleRate() const {
    if (!mpv) return 0;

    int64_t rate = 0;
    if (mpv_get_property(mpv, "audio-params/samplerate", MPV_FORMAT_INT64, &rate) == 0) {
        return static_cast<int>(rate);
    }

    if (mpv_get_property(mpv, "audio/samplerate", MPV_FORMAT_INT64, &rate) == 0) {
        return static_cast<int>(rate);
    }

    return 0;
}

int VideoPlayer::GetAudioChannels() const {
    if (!mpv) return 0;

    int64_t channels = 0;
    if (mpv_get_property(mpv, "audio-params/channel-count", MPV_FORMAT_INT64, &channels) == 0) {
        return static_cast<int>(channels);
    }

    if (mpv_get_property(mpv, "audio/channels", MPV_FORMAT_INT64, &channels) == 0) {
        return static_cast<int>(channels);
    }

    return 0;
}

double VideoPlayer::GetAudioBitrate() const {
    if (!mpv) return 0.0;

    double bitrate = 0.0;

    if (mpv_get_property(mpv, "audio-bitrate", MPV_FORMAT_DOUBLE, &bitrate) == 0 && bitrate > 0) {
        return bitrate / 1000.0;
    }

    if (mpv_get_property(mpv, "packet-audio-bitrate", MPV_FORMAT_DOUBLE, &bitrate) == 0 && bitrate > 0) {
        return bitrate / 1000.0;
    }

    int64_t track_count = 0;
    if (mpv_get_property(mpv, "track-list/count", MPV_FORMAT_INT64, &track_count) == 0) {
        for (int64_t i = 0; i < track_count; i++) {
            char type_path[64], bitrate_path[64];
            snprintf(type_path, sizeof(type_path), "track-list/%lld/type", i);
            snprintf(bitrate_path, sizeof(bitrate_path), "track-list/%lld/demux-bitrate", i);

            char* track_type = nullptr;
            if (mpv_get_property(mpv, type_path, MPV_FORMAT_STRING, &track_type) == 0 && track_type) {
                std::string type(track_type);
                mpv_free(track_type);

                if (type == "audio") {
                    double track_bitrate = 0.0;
                    if (mpv_get_property(mpv, bitrate_path, MPV_FORMAT_DOUBLE, &track_bitrate) == 0 && track_bitrate > 0) {
                        return track_bitrate / 1000.0;
                    }
                }
            }
        }
    }

    return 0.0;
}

// ============================================================================
// OCIO pipeline
// ============================================================================

void VideoPlayer::SetupColorProcessingResources() {
    // Use content dimensions if available (overlay modes: EXR, image sequences, timelines)
    // Otherwise use video dimensions (regular video playback)
    int target_width = (use_content_dimensions_ && content_width_ > 0) ? content_width_ : video_width;
    int target_height = (use_content_dimensions_ && content_height_ > 0) ? content_height_ : video_height;

    if (target_width <= 0 || target_height <= 0) {
        Debug::Log("SetupColorProcessingResources: Invalid dimensions " + std::to_string(target_width) + "x" + std::to_string(target_height));
        return;
    }

    // Use pipeline-aware color processing resource creation
    CreateColorProcessingResourcesForMode(target_width, target_height, current_pipeline_mode);

    // Create fullscreen quad for processing (only if not already created)
    // Quad is reusable and doesn't depend on dimensions
    if (quad_vao == 0) {
        float quad_vertices[] = {
            // positions   // texCoords
            -1.0f,  1.0f,  0.0f, 1.0f,  // top-left
            -1.0f, -1.0f,  0.0f, 0.0f,  // bottom-left
             1.0f, -1.0f,  1.0f, 0.0f,  // bottom-right
            -1.0f,  1.0f,  0.0f, 1.0f,  // top-left
             1.0f, -1.0f,  1.0f, 0.0f,  // bottom-right
             1.0f,  1.0f,  1.0f, 1.0f   // top-right
        };

        glGenVertexArrays(1, &quad_vao);
        glGenBuffers(1, &quad_vbo);

        glBindVertexArray(quad_vao);
        glBindBuffer(GL_ARRAY_BUFFER, quad_vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(quad_vertices), quad_vertices, GL_STATIC_DRAW);

        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));

        glBindVertexArray(0);
        Debug::Log("SetupColorProcessingResources: Created quad VAO/VBO");
    }
}

void VideoPlayer::SetColorPipeline(std::unique_ptr<OCIOPipeline> pipeline) {
    // IMPORTANT: Clear any existing pipeline first to avoid GPU resource corruption
    if (color_pipeline) {
        //Debug::Log("Clearing existing color pipeline before setting new one");
        color_pipeline.reset();

        // Force OpenGL state cleanup - unbind any textures/programs that might conflict
        glUseProgram(0);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_3D, 0);  // Clear any LUT bindings
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, 0);

        //Debug::Log("OpenGL state cleaned up");
    }

    // Set the new pipeline
    color_pipeline = std::move(pipeline);

    if (color_pipeline && color_pipeline->IsValid()) {
        //Debug::Log("New color pipeline set successfully");

        // CRITICAL: Ensure color resources are initialized NOW
        // Always recreate color resources when setting new pipeline to ensure clean state
        if (has_video && video_width > 0 && video_height > 0) {
            //Debug::Log("Initializing color processing resources for new pipeline...");
            SetupColorProcessingResources();
        }
    } else {
        //Debug::Log("No valid color pipeline set - color processing disabled");
    }
}

void VideoPlayer::ClearColorPipeline() {
    // Instead of completely clearing the pipeline, replace it with a passthrough pipeline
    // This maintains the buffering behavior (video_texture -> color_texture) that prevents
    // font cache flickering during media transitions
    Debug::Log("ClearColorPipeline: Replacing with passthrough pipeline for transition stability");

    // Clean up OpenGL state first
    glUseProgram(0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_3D, 0);  // Clear any LUT bindings
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);

    // Create passthrough pipeline
    auto passthrough = std::make_unique<OCIOPipeline>();
    if (passthrough->CreatePassthroughPipeline()) {
        color_pipeline = std::move(passthrough);
        Debug::Log("ClearColorPipeline: Passthrough pipeline created successfully");

        // Initialize color processing resources if we have valid video dimensions
        if (has_video && video_width > 0 && video_height > 0) {
            SetupColorProcessingResources();
        }
    } else {
        Debug::Log("ClearColorPipeline: WARNING - Failed to create passthrough pipeline");
        color_pipeline.reset();
    }
}

void VideoPlayer::ForceFrameRefresh() {
    if (!mpv || !mpv_gl || !has_video) {
        return;
    }

    // Don't update if we don't have valid dimensions
    if (video_width <= 0 || video_height <= 0) {
        return;
    }

    // Make sure we have a valid FBO and texture
    if (fbo == 0 || video_texture == 0) {
        return;
    }

    // First check if MPV has a frame ready to render
    int needs_render = mpv_render_context_update(mpv_gl);

    if (needs_render <= 0) {
        // MPV doesn't think it needs to render - force it by seeking to current position
        // This is necessary when video is paused and we need to refresh with new color pipeline
        double current_pos = GetPosition();
        Debug::Log("MPV not ready to render, forcing seek to current position: " + std::to_string(current_pos));

        // Use MPV property to seek to current position (should be lighter than full seek)
        mpv_set_property(mpv, "time-pos", MPV_FORMAT_DOUBLE, &current_pos);

        // Process any pending MPV events
        UpdateFromMPVEvents();

        // Check again if we now have a frame to render
        needs_render = mpv_render_context_update(mpv_gl);
    }

    if (needs_render > 0) {
        // Force MPV to re-render the current frame with current pipeline format
        // Use cached pipeline format (avoids expensive map lookup every frame)
        GLenum internal_format = current_internal_format;

        mpv_opengl_fbo mpv_fbo = {
            static_cast<int>(fbo),
            video_width,
            video_height,
            static_cast<int>(internal_format)  // ← Tell MPV the target format!
        };

        int flip_y = 0;
        int block_for_target_time = 0;

        mpv_render_param params[] = {
            {MPV_RENDER_PARAM_OPENGL_FBO, &mpv_fbo},
            {MPV_RENDER_PARAM_FLIP_Y, &flip_y},
            {MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME, &block_for_target_time},
            {MPV_RENDER_PARAM_INVALID, nullptr}
        };

        // Force render current frame
        mpv_render_context_render(mpv_gl, params);

        // Apply color pipeline if active
        if (color_pipeline && color_pipeline->IsValid()) {
            // Only apply if we have valid resources
            if (color_fbo != 0 && color_texture != 0) {
                ApplyColorPipeline();
            }
            else {
                SetupColorProcessingResources();
            }
        }

        Debug::Log("Forced frame refresh for color pipeline change");
    } else {
        Debug::Log("Could not force MPV to render frame");
    }
}

// ============================================================================
// Safety Overlay System
// ============================================================================

void VideoPlayer::InitializeSafetyOverlays() {
    if (!safety_overlay_system) {
        safety_overlay_system = std::make_unique<SafetyOverlaySystem>();
    }

    if (has_video && video_width > 0 && video_height > 0) {
        if (!safety_overlay_system->Initialize(video_width, video_height)) {
            Debug::Log("Failed to initialize safety overlay system");
            safety_overlay_system.reset();
        } else {
            Debug::Log("Safety overlay system initialized successfully");
        }
    }
}

void VideoPlayer::SetSafetyOverlaySettings(const SafetyGuideSettings& settings) {
    if (!safety_overlay_system) {
        InitializeSafetyOverlays();
    }

    if (safety_overlay_system) {
        safety_overlay_system->SetOverlaySettings(settings);
    }
}

SafetyGuideSettings VideoPlayer::GetSafetyOverlaySettings() const {
    if (safety_overlay_system) {
        return safety_overlay_system->GetOverlaySettings();
    }
    return SafetyGuideSettings(); // Return default settings
}

void VideoPlayer::EnableSafetyOverlays(bool enabled) {
    svg_overlays_enabled = enabled;
    Debug::Log("Safety overlays " + std::string(enabled ? "enabled" : "disabled"));
}

bool VideoPlayer::IsSafetyOverlaysEnabled() const {
    return svg_overlays_enabled;
}

bool VideoPlayer::HasSafetyOverlays() const {
    return svg_overlay_renderer && svg_overlay_renderer->IsLoaded();
}

void VideoPlayer::RenderSVGOverlays(ImDrawList* draw_list, ImVec2 video_pos, ImVec2 video_size,
                                    float opacity, ImU32 color, float line_width) {
    static bool debug_logged = false;
    if (!debug_logged) {
        Debug::Log("RenderSVGOverlays called: enabled=" + std::string(svg_overlays_enabled ? "true" : "false") +
                   ", renderer=" + std::string(svg_overlay_renderer ? "exists" : "null") +
                   ", loaded=" + std::string(svg_overlay_renderer && svg_overlay_renderer->IsLoaded() ? "true" : "false"));
        debug_logged = true;
    }

    if (!svg_overlays_enabled || !svg_overlay_renderer || !svg_overlay_renderer->IsLoaded()) {
        return;
    }

    //Debug::Log("Calling svg_overlay_renderer->RenderOverlay");
    // Render the SVG overlay
    svg_overlay_renderer->RenderOverlay(draw_list, video_pos, video_size, opacity, color, line_width);
}

void VideoPlayer::RenderTextureWithOCIO(GLuint texture_id, int tex_width, int tex_height,
                                         int viewport_x, int viewport_y, int viewport_width, int viewport_height) {
    if (!color_pipeline || !color_pipeline->IsValid()) {
        // No OCIO pipeline - render texture directly without color correction
        // This fallback could be implemented if needed
        return;
    }

    if (quad_vao == 0) {
        // VAO not initialized - can't render
        return;
    }

    // Save current OpenGL state
    GLint current_fbo, current_program, current_viewport[4];
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &current_fbo);
    glGetIntegerv(GL_CURRENT_PROGRAM, &current_program);
    glGetIntegerv(GL_VIEWPORT, current_viewport);

    // Set viewport for rendering
    glViewport(viewport_x, viewport_y, viewport_width, viewport_height);

    // Use OCIO shader
    GLuint shader_program = color_pipeline->GetShaderProgram();
    glUseProgram(shader_program);

    // Check for OpenGL errors
    GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
        Debug::Log("OpenGL error after glUseProgram: " + std::to_string(error));
    }

    // Bind input texture (cached frame)
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture_id);

    // Bind all LUT textures if needed
    const auto& lut_ids = color_pipeline->GetLUTTextureIDs();
    const auto& lut_dims = color_pipeline->GetLUTTextureDimensions();
    if (!lut_ids.empty()) {
        for (size_t i = 0; i < lut_ids.size(); ++i) {
            int texture_unit = 1 + i; // Start from GL_TEXTURE1
            glActiveTexture(GL_TEXTURE0 + texture_unit);

            // Bind as 1D, 2D, or 3D based on dimension info
            if (i < lut_dims.size()) {
                if (lut_dims[i] == 1) {
                    glBindTexture(GL_TEXTURE_1D, lut_ids[i]);
                    Debug::Log("Bound 1D LUT texture " + std::to_string(lut_ids[i]) + " to texture unit " + std::to_string(texture_unit));
                } else if (lut_dims[i] == 2) {
                    glBindTexture(GL_TEXTURE_2D, lut_ids[i]);
                    Debug::Log("Bound 2D LUT texture " + std::to_string(lut_ids[i]) + " to texture unit " + std::to_string(texture_unit));
                } else {
                    glBindTexture(GL_TEXTURE_3D, lut_ids[i]);
                    Debug::Log("Bound 3D LUT texture " + std::to_string(lut_ids[i]) + " to texture unit " + std::to_string(texture_unit));
                }
            } else {
                glBindTexture(GL_TEXTURE_3D, lut_ids[i]);
                Debug::Log("Bound 3D LUT texture " + std::to_string(lut_ids[i]) + " to texture unit " + std::to_string(texture_unit));
            }
        }
    } else {
        Debug::Log("No LUT textures to bind");
    }

    // Set uniforms
    color_pipeline->UpdateUniforms(0, 1);

    // Apply debug mode (same as video pipeline for consistency)
    // 0=raw input, 1=OCIO processing, 2=UV coords, 3=dimmed input test
    static int debug_mode = 1;  // Normal OCIO processing

    // TODO: Add keyboard shortcut to cycle debug modes
    // For now, manually change this value and recompile to test different modes
    GLint debug_loc = glGetUniformLocation(shader_program, "debugMode");
    if (debug_loc >= 0) {
        glUniform1i(debug_loc, debug_mode);
        Debug::Log("Set debugMode to " + std::to_string(debug_mode) +
                   (debug_mode == 0 ? " (raw input)" :
                    debug_mode == 1 ? " (OCIO processing)" : " (UV coordinates)"));
    } else {
        Debug::Log("WARNING: debugMode uniform not found in shader!");
    }

    // Draw quad
    glBindVertexArray(quad_vao);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);

    // Restore OpenGL state
    glUseProgram(current_program);
    glViewport(current_viewport[0], current_viewport[1], current_viewport[2], current_viewport[3]);

    // Clean up texture bindings
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_3D, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
}

GLuint VideoPlayer::CreateColorCorrectedTexture(GLuint input_texture_id, int tex_width, int tex_height,
                                                int output_width, int output_height) {
    if (!color_pipeline || !color_pipeline->IsValid() || quad_vao == 0) {
        return 0; // No OCIO pipeline or VAO not available
    }

    // Create output texture
    GLuint output_texture = 0;
    glGenTextures(1, &output_texture);
    glBindTexture(GL_TEXTURE_2D, output_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, output_width, output_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // Create temporary framebuffer
    GLuint temp_fbo = 0;
    glGenFramebuffers(1, &temp_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, temp_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, output_texture, 0);

    // Check FBO completeness
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        glDeleteFramebuffers(1, &temp_fbo);
        glDeleteTextures(1, &output_texture);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return 0;
    }

    // Save current OpenGL state
    GLint current_fbo, current_program, current_viewport[4];
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &current_fbo);
    glGetIntegerv(GL_CURRENT_PROGRAM, &current_program);
    glGetIntegerv(GL_VIEWPORT, current_viewport);

    // Set up rendering to offscreen texture
    glViewport(0, 0, output_width, output_height);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    // Use OCIO shader
    GLuint shader_program = color_pipeline->GetShaderProgram();
    glUseProgram(shader_program);

    // Bind input texture
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, input_texture_id);

    // Bind all LUT textures if needed
    const auto& lut_ids = color_pipeline->GetLUTTextureIDs();
    const auto& lut_dims = color_pipeline->GetLUTTextureDimensions();
    if (!lut_ids.empty()) {
        for (size_t i = 0; i < lut_ids.size(); ++i) {
            int texture_unit = 1 + i; // Start from GL_TEXTURE1
            glActiveTexture(GL_TEXTURE0 + texture_unit);
            // Bind as 1D, 2D, or 3D based on dimension info
            if (i < lut_dims.size()) {
                if (lut_dims[i] == 1) {
                    glBindTexture(GL_TEXTURE_1D, lut_ids[i]);
                } else if (lut_dims[i] == 2) {
                    glBindTexture(GL_TEXTURE_2D, lut_ids[i]);
                } else {
                    glBindTexture(GL_TEXTURE_3D, lut_ids[i]);
                }
            } else {
                glBindTexture(GL_TEXTURE_3D, lut_ids[i]);
            }
        }
    }

    // Set uniforms
    color_pipeline->UpdateUniforms(0, 1);

    // Apply debug mode (same as video pipeline)
    static int debug_mode = 1;
    GLint debug_loc = glGetUniformLocation(shader_program, "debugMode");
    if (debug_loc >= 0) {
        glUniform1i(debug_loc, debug_mode);
    }

    // Render quad to offscreen texture
    glBindVertexArray(quad_vao);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);

    // Restore OpenGL state
    glUseProgram(current_program);
    glBindFramebuffer(GL_FRAMEBUFFER, current_fbo);
    glViewport(current_viewport[0], current_viewport[1], current_viewport[2], current_viewport[3]);

    // Clean up temporary framebuffer
    glDeleteFramebuffers(1, &temp_fbo);

    // Clean up texture bindings
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_3D, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);

    return output_texture;
}

void VideoPlayer::ApplyColorPipeline() {
    if (!color_pipeline || !color_pipeline->IsValid()) {
        //Debug::Log("ApplyColorPipeline: Invalid pipeline");
        return;
    }

    if (color_fbo == 0 || color_texture == 0) {
        //Debug::Log("ApplyColorPipeline: Resources not initialized");
        SetupColorProcessingResources();
        if (color_fbo == 0 || color_texture == 0) {
            //Debug::Log("ApplyColorPipeline: Failed to initialize resources");
            return;
        }
    }

    // Save state
    GLint viewport[4];
    glGetIntegerv(GL_VIEWPORT, viewport);
    GLint current_fbo;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &current_fbo);
    GLint current_program;
    glGetIntegerv(GL_CURRENT_PROGRAM, &current_program);
    GLint current_active_texture;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &current_active_texture);
    GLint current_vao;
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &current_vao);

    //Debug::Log("ApplyColorPipeline: Starting render");
    //Debug::Log("  Input texture: " + std::to_string(video_texture));
    //Debug::Log("  Output FBO: " + std::to_string(color_fbo));
    //Debug::Log("  Output texture: " + std::to_string(color_texture));

    // Determine target render dimensions
    int target_width = (use_content_dimensions_ && content_width_ > 0) ? content_width_ : video_width;
    int target_height = (use_content_dimensions_ && content_height_ > 0) ? content_height_ : video_height;

    if (target_width <= 0 || target_height <= 0) {
        Debug::Log("ApplyColorPipeline: Cannot render - no valid dimensions");
        return;
    }

    // Check if color resources need to be recreated due to dimension change
    // This handles transitions from timeline mode where dimensions were reset
    if (color_texture_width_ != target_width || color_texture_height_ != target_height) {
        Debug::Log("ApplyColorPipeline: Dimension mismatch detected (" +
                   std::to_string(color_texture_width_) + "x" + std::to_string(color_texture_height_) +
                   " vs " + std::to_string(target_width) + "x" + std::to_string(target_height) +
                   "), recreating color resources");
        CreateColorProcessingResourcesForMode(target_width, target_height, current_pipeline_mode);
    }

    // Bind color FBO
    glBindFramebuffer(GL_FRAMEBUFFER, color_fbo);
    glViewport(0, 0, color_texture_width_, color_texture_height_);

    // Clear to background color
    glClearColor(27.0f/255.0f, 27.0f/255.0f, 27.0f/255.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    // Use OCIO shader
    GLuint shader_program = color_pipeline->GetShaderProgram();
    glUseProgram(shader_program);
    //Debug::Log("  Shader program: " + std::to_string(shader_program));

    // Bind input texture
    glActiveTexture(GL_TEXTURE0);
    if (video_texture != 0) {
        glBindTexture(GL_TEXTURE_2D, video_texture);
    } else {
        // Bind a default/empty texture or skip binding to prevent invalid texture warnings
        glBindTexture(GL_TEXTURE_2D, 0);
        Debug::Log("WARNING: ApplyColorPipeline called with invalid video_texture, skipping");
        return;
    }

    // Bind all LUT textures if needed
    const auto& lut_ids = color_pipeline->GetLUTTextureIDs();
    const auto& lut_dims = color_pipeline->GetLUTTextureDimensions();
    if (!lut_ids.empty()) {
        for (size_t i = 0; i < lut_ids.size(); ++i) {
            int texture_unit = 1 + i; // Start from GL_TEXTURE1
            glActiveTexture(GL_TEXTURE0 + texture_unit);
            // Bind as 1D, 2D, or 3D based on dimension info
            if (i < lut_dims.size()) {
                if (lut_dims[i] == 1) {
                    glBindTexture(GL_TEXTURE_1D, lut_ids[i]);
                } else if (lut_dims[i] == 2) {
                    glBindTexture(GL_TEXTURE_2D, lut_ids[i]);
                } else {
                    glBindTexture(GL_TEXTURE_3D, lut_ids[i]);
                }
            } else {
                glBindTexture(GL_TEXTURE_3D, lut_ids[i]);
            }
           /* Debug::Log("  LUT texture bound: " + std::to_string(lut_ids[i]) + " to unit " + std::to_string(texture_unit));*/
        }
    }

    // Set uniforms
    color_pipeline->UpdateUniforms(0, 1);

    // Debug mode: 0=OCIO processing, 1=raw input, 2=UV coords
    static int debug_mode = 1;  // TEMP: Test ACES 2.0 input texture binding
    
    GLint debug_loc = glGetUniformLocation(shader_program, "debugMode");
    if (debug_loc >= 0) {
        glUniform1i(debug_loc, debug_mode);
       /* Debug::Log("Set debugMode to " + std::to_string(debug_mode) + 
                   (debug_mode == 0 ? " (OCIO processing active)" : 
                    debug_mode == 1 ? " (raw input texture)" : " (UV coordinates)"));*/
    }

    // Check VAO is valid
    if (!glIsVertexArray(quad_vao)) {
       /* Debug::Log("ERROR: quad_vao is not valid!");*/
        // Restore state on early exit
        glUseProgram(current_program);
        glBindFramebuffer(GL_FRAMEBUFFER, current_fbo);
        glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
        glActiveTexture(current_active_texture);
        glBindVertexArray(current_vao);
        return;
    }

    // Draw quad
    glBindVertexArray(quad_vao);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);

    // Check for errors
    GLenum err;
    while ((err = glGetError()) != GL_NO_ERROR) {
        std::string error_str;
        switch (err) {
        case GL_INVALID_ENUM: error_str = "INVALID_ENUM"; break;
        case GL_INVALID_VALUE: error_str = "INVALID_VALUE"; break;
        case GL_INVALID_OPERATION: error_str = "INVALID_OPERATION"; break;
        case GL_INVALID_FRAMEBUFFER_OPERATION: error_str = "INVALID_FRAMEBUFFER_OPERATION"; break;
        default: error_str = std::to_string(err); break;
        }
       /* Debug::Log("GL Error: " + error_str);*/
    }

    // Restore state completely
    glUseProgram(current_program);
    glBindFramebuffer(GL_FRAMEBUFFER, current_fbo);
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);

    // Restore vertex array binding
    glBindVertexArray(current_vao);

    // Clean up texture bindings and restore active texture
    // Unbind all texture units that we used (reuse lut_ids from earlier)
    if (!lut_ids.empty()) {
        for (size_t i = 0; i < lut_ids.size(); ++i) {
            int texture_unit = 1 + i;
            glActiveTexture(GL_TEXTURE0 + texture_unit);
            glBindTexture(GL_TEXTURE_3D, 0);
        }
    }

    // Unbind main texture and restore active texture unit
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(current_active_texture);

    /*Debug::Log("ApplyColorPipeline: Complete");*/
}

// ============================================================================
// Screenshot functionality
// ============================================================================

bool VideoPlayer::CaptureScreenshotToClipboard() {
    if (!HasValidTexture()) {
        Debug::Log("Screenshot failed: No valid video texture available");
        return false;
    }

    // Get the final rendered texture (with color correction and safety overlays)
    GLuint final_texture = video_texture;

    // Apply color correction if available
    if (HasColorPipeline()) {
        GLuint color_corrected = CreateColorCorrectedTexture(video_texture, video_width, video_height, video_width, video_height);
        if (color_corrected != 0) {
            final_texture = color_corrected;
        }
    }

    // Safety overlays are UI elements only - screenshots capture pure video + color correction

    // Read pixels from the final texture
    std::vector<unsigned char> pixels(video_width * video_height * 4);

    GLint current_fbo;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &current_fbo);

    // Create temporary FBO to read from texture
    GLuint temp_fbo;
    glGenFramebuffers(1, &temp_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, temp_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, final_texture, 0);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
        glReadPixels(0, 0, video_width, video_height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());

        // Copy to clipboard (Windows implementation)
        #ifdef _WIN32
        if (OpenClipboard(nullptr)) {
            EmptyClipboard();

            // Keep original RGBA for PNG
            std::vector<unsigned char> rgba_pixels = pixels;

            // Convert RGBA to BGRA for Windows bitmap formats
            for (size_t i = 0; i < pixels.size(); i += 4) {
                std::swap(pixels[i], pixels[i + 2]); // Swap R and B
            }

            // Format 1: CF_DIB (Device Independent Bitmap) - for Photoshop
            BITMAPINFOHEADER bi = {};
            bi.biSize = sizeof(BITMAPINFOHEADER);
            bi.biWidth = video_width;
            bi.biHeight = -video_height; // Negative for top-down bitmap
            bi.biPlanes = 1;
            bi.biBitCount = 32;
            bi.biCompression = BI_RGB;

            HGLOBAL hDIB = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPINFOHEADER) + pixels.size());
            if (hDIB) {
                unsigned char* pDIB = (unsigned char*)GlobalLock(hDIB);
                if (pDIB) {
                    memcpy(pDIB, &bi, sizeof(BITMAPINFOHEADER));
                    memcpy(pDIB + sizeof(BITMAPINFOHEADER), pixels.data(), pixels.size());
                    GlobalUnlock(hDIB);
                    SetClipboardData(CF_DIB, hDIB);
                }
            }

            // Format 2: PNG format for modern apps
            static UINT CF_PNG = RegisterClipboardFormatA("PNG");
            if (CF_PNG) {
                // Write PNG to memory buffer using stb_image_write
                auto png_write_func = [](void* context, void* data, int size) {
                    std::vector<unsigned char>* buffer = (std::vector<unsigned char>*)context;
                    unsigned char* bytes = (unsigned char*)data;
                    buffer->insert(buffer->end(), bytes, bytes + size);
                };

                std::vector<unsigned char> png_buffer;
                stbi_write_png_to_func(png_write_func, &png_buffer, video_width, video_height, 4, rgba_pixels.data(), video_width * 4);

                if (!png_buffer.empty()) {
                    HGLOBAL hPNG = GlobalAlloc(GMEM_MOVEABLE, png_buffer.size());
                    if (hPNG) {
                        unsigned char* pPNG = (unsigned char*)GlobalLock(hPNG);
                        if (pPNG) {
                            memcpy(pPNG, png_buffer.data(), png_buffer.size());
                            GlobalUnlock(hPNG);
                            SetClipboardData(CF_PNG, hPNG);
                        }
                    }
                }
            }

            CloseClipboard();
        }
        #endif

        Debug::Log("Screenshot captured to clipboard (" + std::to_string(video_width) + "x" + std::to_string(video_height) + ")");
    } else {
        Debug::Log("Screenshot failed: Could not create framebuffer for texture reading");
    }

    // Cleanup
    glBindFramebuffer(GL_FRAMEBUFFER, current_fbo);
    glDeleteFramebuffers(1, &temp_fbo);

    return true;
}

bool VideoPlayer::CaptureScreenshotToDesktop(const std::string& filename) {
    if (!HasValidTexture()) {
        Debug::Log("Screenshot failed: No valid video texture available");
        return false;
    }

    // Generate filename if not provided
    std::string output_filename = filename;
    if (output_filename.empty()) {
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        auto tm = *std::localtime(&time_t);

        char timestamp[64];
        std::strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", &tm);

        // Get filename from timeline name (if in timeline mode) or current file path
        std::string base_filename = "ump_Screenshot";
        if (is_timeline_mode_ && timeline_controller_) {
            // In timeline mode, use the timeline name instead of current clip
            std::string timeline_name = timeline_controller_->GetTimelineName();
            if (!timeline_name.empty()) {
                base_filename = timeline_name;
                Debug::Log("Screenshot: Using timeline name '" + base_filename + "'");
            } else {
                // Fallback to source file path stem if timeline name is empty
                std::string source_path = timeline_controller_->GetSourceFilePath();
                if (!source_path.empty()) {
                    std::filesystem::path p(source_path);
                    base_filename = p.stem().string();
                    Debug::Log("Screenshot: Using timeline source file '" + base_filename + "'");
                } else {
                    Debug::Log("Screenshot: Timeline mode but no timeline name, using fallback");
                }
            }
        } else if (mpv) {
            char* path_result = nullptr;
            if (mpv_get_property(mpv, "path", MPV_FORMAT_STRING, &path_result) == 0 && path_result) {
                std::string file_path = path_result;
                mpv_free(path_result);

                // Extract filename without extension
                std::filesystem::path p(file_path);
                base_filename = p.stem().string();
                Debug::Log("Screenshot: Using filename '" + base_filename + "' from current file");
            } else {
                Debug::Log("Screenshot: Could not get file path, using fallback");
            }
        }

        // Save to desktop
        #ifdef _WIN32
        char desktop_path[MAX_PATH];
        if (SHGetSpecialFolderPathA(nullptr, desktop_path, CSIDL_DESKTOP, FALSE)) {
            output_filename = std::string(desktop_path) + "\\" + base_filename + "_" + timestamp + ".png";
        } else {
            output_filename = base_filename + "_" + std::string(timestamp) + ".png";
        }
        #else
        output_filename = base_filename + "_" + std::string(timestamp) + ".png";
        #endif
    }

    // Get the final rendered texture (with color correction and safety overlays)
    GLuint final_texture = video_texture;

    // Apply color correction if available
    if (HasColorPipeline()) {
        GLuint color_corrected = CreateColorCorrectedTexture(video_texture, video_width, video_height, video_width, video_height);
        if (color_corrected != 0) {
            final_texture = color_corrected;
        }
    }

    // Safety overlays are UI elements only - screenshots capture pure video + color correction

    // Read pixels from the final texture
    std::vector<unsigned char> pixels(video_width * video_height * 4);

    GLint current_fbo;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &current_fbo);

    // Create temporary FBO to read from texture
    GLuint temp_fbo;
    glGenFramebuffers(1, &temp_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, temp_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, final_texture, 0);

    bool success = false;
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
        glReadPixels(0, 0, video_width, video_height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());

        // Save as PNG using stb_image_write (no flip needed - texture is already correct orientation)
        int result = stbi_write_png(output_filename.c_str(), video_width, video_height, 4,
                                   pixels.data(), video_width * 4);

        if (result) {
            Debug::Log("Screenshot saved to: " + output_filename + " (" + std::to_string(video_width) + "x" + std::to_string(video_height) + ")");
            success = true;
        } else {
            Debug::Log("Failed to save screenshot to: " + output_filename);
            success = false;
        }
    } else {
        Debug::Log("Screenshot failed: Could not create framebuffer for texture reading");
    }

    // Cleanup
    glBindFramebuffer(GL_FRAMEBUFFER, current_fbo);
    glDeleteFramebuffers(1, &temp_fbo);

    return success;
}

bool VideoPlayer::CaptureScreenshotToPath(const std::string& directory_path, const std::string& filename) {
    if (!HasValidTexture()) {
        Debug::Log("Screenshot failed: No valid video texture available");
        return false;
    }

    // Construct full output path
    std::string output_filename = directory_path;

    // Ensure directory path ends with separator
    #ifdef _WIN32
    if (!output_filename.empty() && output_filename.back() != '\\' && output_filename.back() != '/') {
        output_filename += "\\";
    }
    #else
    if (!output_filename.empty() && output_filename.back() != '/') {
        output_filename += "/";
    }
    #endif

    output_filename += filename;

    // Get the final rendered texture (with color correction and safety overlays)
    GLuint final_texture = video_texture;

    // Apply color correction if available
    if (HasColorPipeline()) {
        GLuint color_corrected = CreateColorCorrectedTexture(video_texture, video_width, video_height, video_width, video_height);
        if (color_corrected != 0) {
            final_texture = color_corrected;
        }
    }

    // Safety overlays are UI elements only - screenshots capture pure video + color correction

    // Read pixels from the final texture
    std::vector<unsigned char> pixels(video_width * video_height * 4);

    GLint current_fbo;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &current_fbo);

    // Create temporary FBO to read from texture
    GLuint temp_fbo;
    glGenFramebuffers(1, &temp_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, temp_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, final_texture, 0);

    bool success = false;
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
        glReadPixels(0, 0, video_width, video_height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());

        // Save as PNG using stb_image_write (no flip needed - texture is already correct orientation)
        int result = stbi_write_png(output_filename.c_str(), video_width, video_height, 4,
                                   pixels.data(), video_width * 4);

        if (result) {
            Debug::Log("Screenshot saved to: " + output_filename + " (" + std::to_string(video_width) + "x" + std::to_string(video_height) + ")");
            success = true;
        } else {
            Debug::Log("Failed to save screenshot to: " + output_filename);
            success = false;
        }
    } else {
        Debug::Log("Screenshot failed: Could not create framebuffer for texture reading");
    }

    // Cleanup
    glBindFramebuffer(GL_FRAMEBUFFER, current_fbo);
    glDeleteFramebuffers(1, &temp_fbo);

    return success;
}

bool VideoPlayer::FeedEXRFrame(const void* rgba_data, int width, int height, double timestamp) {
    if (!is_exr_mode || !rgba_data) {
        return false;
    }

    Debug::Log("Feeding EXR frame: " + std::to_string(width) + "x" + std::to_string(height) +
               " at timestamp " + std::to_string(timestamp));

    // Create or update EXR texture
    if (exr_texture == 0 || exr_texture_width != width || exr_texture_height != height) {
        // Delete old texture if it exists
        if (exr_texture != 0) {
            glDeleteTextures(1, &exr_texture);
        }

        // Create new texture
        glGenTextures(1, &exr_texture);
        glBindTexture(GL_TEXTURE_2D, exr_texture);

        // Set texture parameters
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        // Store dimensions
        exr_texture_width = width;
        exr_texture_height = height;
    } else {
        glBindTexture(GL_TEXTURE_2D, exr_texture);
    }

    // Upload float16 RGBA data to GPU
    // OpenGL supports GL_HALF_FLOAT for float16 data
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0,
                 GL_RGBA, GL_HALF_FLOAT, rgba_data);

    // Update video texture reference to point to our EXR texture
    video_texture = exr_texture;
    video_width = width;
    video_height = height;
    has_video = true;

    glBindTexture(GL_TEXTURE_2D, 0);

    Debug::Log("EXR frame uploaded to GPU texture: " + std::to_string(exr_texture));
    return true;
}

bool VideoPlayer::CopyTextureForPlayback(GLuint source_texture, int width, int height) {
    if (source_texture == 0) {
        Debug::Log("CopyTextureForPlayback: Invalid source texture");
        return false;
    }

    // Create or resize our playback texture if needed
    if (exr_texture == 0 || exr_texture_width != width || exr_texture_height != height) {
        // Delete old texture if it exists
        if (exr_texture != 0) {
            glDeleteTextures(1, &exr_texture);
        }

        // Create new texture
        glGenTextures(1, &exr_texture);
        glBindTexture(GL_TEXTURE_2D, exr_texture);

        // Set texture parameters
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        // Allocate texture storage
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA, GL_HALF_FLOAT, nullptr);

        exr_texture_width = width;
        exr_texture_height = height;
    }

    // Copy texture data using framebuffers (GPU-to-GPU copy)
    GLuint fbo_read, fbo_write;
    glGenFramebuffers(1, &fbo_read);
    glGenFramebuffers(1, &fbo_write);

    // Bind source texture to read framebuffer
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo_read);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, source_texture, 0);

    // Bind destination texture to write framebuffer
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo_write);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, exr_texture, 0);

    // Check framebuffer completeness
    if (glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE ||
        glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        Debug::Log("CopyTextureForPlayback: Framebuffer setup failed");
        glDeleteFramebuffers(1, &fbo_read);
        glDeleteFramebuffers(1, &fbo_write);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return false;
    }

    // Perform the copy
    glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);

    // Cleanup
    glDeleteFramebuffers(1, &fbo_read);
    glDeleteFramebuffers(1, &fbo_write);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // Update video texture reference
    video_texture = exr_texture;
    video_width = width;
    video_height = height;
    has_video = true;

    Debug::Log("Successfully copied texture from " + std::to_string(source_texture) +
              " to " + std::to_string(exr_texture) + " (" + std::to_string(width) + "x" + std::to_string(height) + ")");
    return true;
}

// DEPRECATED: Replaced by DirectEXRCache::GetFrameOrLoad
bool VideoPlayer::ProcessAndFeedEXRFrame(int frame_index) {
    Debug::Log("ProcessAndFeedEXRFrame: DEPRECATED - No longer used (DirectEXRCache handles everything)");
    return false;
}

// ============================================================================
// Shader Injection EXR Integration
// ============================================================================

// Helper function to extract start frame number from image sequence filenames
static int ExtractStartFrameFromSequence(const std::vector<std::string>& files) {
    if (files.empty()) return 0;

    std::filesystem::path first_file(files[0]);
    std::string filename = first_file.stem().string();

    // Try to extract frame number using regex pattern
    // Matches patterns like: name_1001, name.1001, name-1001, or name1001
    std::regex pattern(R"(^(.+?)([_\.\-])?(\d+)$)");
    std::smatch match;

    if (std::regex_match(filename, match, pattern)) {
        try {
            return std::stoi(match[3].str());
        } catch (...) {
            return 0; // Fallback to 0 on parse error
        }
    }

    return 0; // Fallback to 0 if no frame number found
}

// ============================================================================
// Image Sequence Timer Initialization (Virtual Timeline Mode)
// ============================================================================

bool VideoPlayer::InitializeImageSequenceTimer(double duration, double fps) {
    // Create timer for image sequence playback (replaces dummy video)
    image_sequence_timer_ = std::make_unique<ump::PlaybackTimer>();
    image_sequence_timer_->SetDuration(duration);
    image_sequence_timer_->SetFrameRate(fps);
    image_sequence_timer_->SetLooping(loop_enabled);
    image_sequence_timer_->Seek(0.0);

    // Loop callback
    image_sequence_timer_->SetOnLoop([this]() {
        Debug::Log("Image sequence looped");
    });

    // End callback (pause at end if not looping)
    image_sequence_timer_->SetOnEnd([this]() {
        Debug::Log("Image sequence reached end");
        is_playing = false;
    });

    cached_duration = duration;
    Debug::Log("Image sequence timer initialized: duration=" + std::to_string(duration) +
               "s, fps=" + std::to_string(fps) + ", loop=" + std::to_string(loop_enabled));
    return true;
}

bool VideoPlayer::LoadEXRSequenceWithDummy(const std::vector<std::string>& sequence_files,
                                           const std::string& layer_name,
                                           double fps,
                                           int cached_width,
                                           int cached_height,
                                           double cached_duration,
                                           double initial_playhead) {
    if (sequence_files.empty()) {
        Debug::Log("ERROR: Empty sequence files list");
        return false;
    }

    Debug::Log("Loading EXR sequence with hybrid dummy + OpenGL overlay approach");
    Debug::Log("Sequence: " + std::to_string(sequence_files.size()) + " files, layer: " + layer_name + ", fps: " + std::to_string(fps));

    // Extract start frame from sequence filenames
    int start_frame = ExtractStartFrameFromSequence(sequence_files);
    Debug::Log("EXR sequence start frame: " + std::to_string(start_frame));

    // MODIFIED: Use cached dimensions if available (instant - no I/O!)
    int width, height;

    if (cached_width > 0 && cached_height > 0) {
        // Use cached dimensions from MediaItem
        width = cached_width;
        height = cached_height;
        Debug::Log("Using cached EXR sequence dimensions: " + std::to_string(width) + "x" + std::to_string(height));
    } else {
        // Fallback: Get dimensions from first EXR frame
        if (!ump::DirectEXRCache::GetFrameDimensions(sequence_files[0], width, height)) {
            Debug::Log("ERROR: Could not get dimensions from first EXR file: " + sequence_files[0]);
            return false;
        }
        Debug::Log("Detected EXR sequence dimensions: " + std::to_string(width) + "x" + std::to_string(height));
    }

    // Store content dimensions for overlay mode (allows 1x1 dummies)
    SetContentDimensions(width, height);

    // === ENSURE VIDEO FBO RESOURCES EXIST ===
    // EXR sequences render via overlay mode, but UpdateVideoTexture requires fbo/mpv_fbo
    // to be valid. Create them at content dimensions if they don't exist.
    if (fbo == 0 || mpv_fbo == 0 || mpv_texture == 0) {
        Debug::Log("Creating video FBO resources for EXR sequence (first load case)");
        CreateVideoTextures(width, height);
    }

    // FIRST: Switch video_texture to placeholder BEFORE destroying any cache textures
    // This prevents displaying deleted textures during the transition window
    video_texture = transition_placeholder_texture_;
    // IMPORTANT: Use content dimensions for video_width/height, not placeholder dimensions
    // This ensures aspect ratio is correct during the transition (color_fbo is at content size)
    video_width = width;
    video_height = height;
    has_video = true;
    exr_texture = 0;

    // Clear color texture to background to prevent showing stale frames
    ClearColorTextureToBackground();

    // === EVICT VIDEO CACHE TO FREE RAM (cross-cache eviction) ===
    if (cache_clear_callback) {
        Debug::Log("Clearing video cache before loading EXR sequence (cross-cache eviction)");
        cache_clear_callback();
    }

    // === CLEAR THUMBNAIL CACHE (media switching) ===
    if (thumbnail_cache_) {
        Debug::Log("Clearing thumbnail cache before loading EXR sequence (media switch)");
        thumbnail_cache_->ClearCache();
        thumbnail_cache_.reset();  // Destroy old thumbnail cache
    }

    // Calculate actual sequence duration (prefer cached duration if provided)
    double duration;
    if (cached_duration > 0.0) {
        duration = cached_duration;
        Debug::Log("EXR sequence using cached duration: " + std::to_string(duration) + " seconds");
    } else {
        duration = static_cast<double>(sequence_files.size()) / fps;
        Debug::Log("EXR sequence calculated duration: " + std::to_string(duration) + " seconds (" + std::to_string(sequence_files.size()) + " frames)");
    }

    // Initialize virtual timeline (no dummy video needed)
    if (!InitializeImageSequenceTimer(duration, fps)) {
        Debug::Log("ERROR: Failed to initialize image sequence timer");
        return false;
    }

    // Set content dimensions for rendering
    SetContentDimensions(width, height);

    // Ensure FBO resources exist for rendering
    if (fbo == 0) {
        CreateVideoTextures(width, height);
    }

    Debug::Log("LoadEXRSequenceWithDummy: Using virtual timeline (no dummy video)");

    // Store sequence data for frame processing
    exr_sequence_files = sequence_files;
    exr_layer_name = layer_name;
    exr_frame_rate = fps;
    exr_frame_count = static_cast<int>(sequence_files.size());
    exr_sequence_start_frame = start_frame;
    is_exr_mode = true;
    image_sequence_format = "EXR";  // Store format type

    // Set pipeline mode for EXR sequences (Float16/half-precision)
    current_pipeline_mode = PipelineMode::ULTRA_HIGH_RES;

    Debug::Log("EXR sequence data stored: " + std::to_string(exr_frame_count) + " frames, start frame: " + std::to_string(start_frame));

    // NEW: Initialize EXR background cache (non-blocking)
    // Pass initial_playhead so cache starts from correct position when reloading
    Debug::Log("LoadEXRSequenceWithDummy: Initializing cache at playhead position " + std::to_string(initial_playhead) + "s");
    InitializeEXRCache(sequence_files, layer_name, fps, initial_playhead);

    // NEW: Initialize ThumbnailCache for EXR sequences
    ump::ThumbnailConfig thumb_config = GetCurrentThumbnailConfig();
    if (thumb_config.enabled) {
        Debug::Log("VideoPlayer: Creating ThumbnailCache for EXR sequence (layer: '" + layer_name + "')");

        auto exr_thumb_loader = std::make_unique<ump::EXRImageLoader>();
        exr_thumb_loader->SetLayer(layer_name);  // Set layer for multi-layer EXR support

        thumbnail_cache_ = std::make_unique<ump::ThumbnailCache>(
            sequence_files,
            std::move(exr_thumb_loader),
            thumb_config
        );
        Debug::Log("VideoPlayer: ThumbnailCache initialized for EXR, " +
                   std::to_string(thumb_config.width) + "x" + std::to_string(thumb_config.height) +
                   ", cache size: " + std::to_string(thumb_config.cache_size));

        // Prefetch strategic frames for timeline preview
        thumbnail_cache_->PrefetchStrategicFrames(static_cast<int>(sequence_files.size()));
    } else {
        Debug::Log("VideoPlayer: ThumbnailCache disabled by configuration");
    }

    // Metadata extraction deferred - will be extracted lazily when inspector is opened
    // This avoids blocking UI on EXR file I/O during load

    // Synchronously wait for initial frame to be loaded and uploaded
    // This ensures the EXR sequence displays immediately instead of showing placeholder
    // Use initial_playhead to determine which frame to wait for
    int initial_frame_index = static_cast<int>(initial_playhead * fps);
    if (initial_frame_index < 0) initial_frame_index = 0;
    if (initial_frame_index >= static_cast<int>(sequence_files.size())) {
        initial_frame_index = static_cast<int>(sequence_files.size()) - 1;
    }
    Debug::Log("Waiting for initial EXR frame " + std::to_string(initial_frame_index) + " to load (playhead " + std::to_string(initial_playhead) + "s)...");

    if (exr_cache_) {
        const int MAX_WAIT_ITERATIONS = 100;  // ~2 seconds max (20ms * 100)
        bool first_frame_ready = false;

        for (int i = 0; i < MAX_WAIT_ITERATIONS && !first_frame_ready; i++) {
            // Request frame at initial playhead position
            exr_cache_->RequestFrame(initial_frame_index);

            // Give background thread time to load
            std::this_thread::sleep_for(std::chrono::milliseconds(20));

            // Process any ready textures (uploads to GPU)
            exr_cache_->ProcessReadyTextures();

            // Check if frame is now available
            GLuint texture = 0;
            int tex_width = 0, tex_height = 0;
            texture = exr_cache_->GetTexture(initial_frame_index, tex_width, tex_height);

            if (texture != 0) {
                // Initial frame is ready - update video_texture
                video_texture = texture;
                video_width = tex_width;
                video_height = tex_height;
                exr_texture = texture;
                exr_texture_width = tex_width;
                exr_texture_height = tex_height;
                first_frame_ready = true;
                Debug::Log("Initial EXR frame " + std::to_string(initial_frame_index) + " ready after " + std::to_string((i + 1) * 20) + "ms (texture " +
                           std::to_string(texture) + ", " + std::to_string(tex_width) + "x" + std::to_string(tex_height) + ")");
            }
        }

        if (!first_frame_ready) {
            Debug::Log("WARNING: Initial EXR frame " + std::to_string(initial_frame_index) + " not ready after timeout - will show placeholder until cached");
        }
    }

    Debug::Log("EXR sequence loaded successfully with hybrid approach");
    return true;
}

// NEW: Universal image sequence loading (TIFF/PNG/JPEG with DirectEXRCache)
bool VideoPlayer::LoadImageSequenceWithCache(const std::vector<std::string>& sequence_files,
                                             double fps,
                                             PipelineMode pipeline_mode,
                                             int cached_width,
                                             int cached_height,
                                             double cached_duration,
                                             double initial_playhead) {
    if (sequence_files.empty()) {
        Debug::Log("ERROR: Empty sequence files list");
        return false;
    }

    Debug::Log("Loading image sequence with DirectEXRCache (universal loader)");
    Debug::Log("Sequence: " + std::to_string(sequence_files.size()) + " files, fps: " + std::to_string(fps) +
               ", pipeline: " + std::string(PipelineModeToString(pipeline_mode)));

    // === RESET STATE BEFORE LOADING NEW SEQUENCE (if needed) ===
    // Only reset if there's existing media loaded to avoid interfering with fresh initialization
    if (has_video || is_exr_mode) {
        Debug::Log("Resetting state (cleaning up previous media)");
        ResetState();
    }

    // Detect image format from first file extension
    std::filesystem::path first_file(sequence_files[0]);
    std::string ext = first_file.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    // Create appropriate image loader
    std::unique_ptr<ump::IImageLoader> loader;
    std::string format_name;

    if (ext == ".tiff" || ext == ".tif") {
        loader = std::make_unique<ump::TIFFImageLoader>();
        format_name = "TIFF";
    } else if (ext == ".png") {
        loader = std::make_unique<ump::PNGImageLoader>();
        format_name = "PNG";
    } else if (ext == ".jpg" || ext == ".jpeg") {
        loader = std::make_unique<ump::JPEGImageLoader>();
        format_name = "JPEG";
    } else {
        Debug::Log("ERROR: Unsupported image format: " + ext);
        return false;
    }

    Debug::Log("Created " + format_name + " loader for sequence");

    // MODIFIED: Use cached dimensions if available (instant - no I/O!)
    int width, height;

    if (cached_width > 0 && cached_height > 0) {
        // Use cached dimensions from MediaItem
        width = cached_width;
        height = cached_height;
        Debug::Log("Using cached sequence dimensions: " + std::to_string(width) + "x" + std::to_string(height));
    } else {
        // Fallback: Get dimensions from first file
        if (!loader->GetDimensions(sequence_files[0], width, height)) {
            Debug::Log("ERROR: Could not get dimensions from first file");
            return false;
        }
        Debug::Log("Detected image sequence dimensions: " + std::to_string(width) + "x" + std::to_string(height));
    }

    // Store content dimensions for overlay mode (allows 1x1 dummies)
    SetContentDimensions(width, height);

    // Extract start frame from sequence filenames
    int start_frame = ExtractStartFrameFromSequence(sequence_files);
    Debug::Log("Image sequence start frame: " + std::to_string(start_frame));

    // === ENSURE VIDEO FBO RESOURCES EXIST ===
    // Image sequences render via overlay mode, but UpdateVideoTexture requires fbo/mpv_fbo
    // to be valid. Create them at content dimensions if they don't exist.
    if (fbo == 0 || mpv_fbo == 0 || mpv_texture == 0) {
        Debug::Log("Creating video FBO resources for image sequence (first load case)");
        CreateVideoTextures(width, height);
    }

    // === CLEAR ALL CACHES BEFORE LOADING NEW IMAGE SEQUENCE ===
    // This ensures clean transitions when switching between image sequences

    // FIRST: Switch video_texture to placeholder BEFORE destroying any cache textures
    // This prevents displaying deleted textures during the transition window
    video_texture = transition_placeholder_texture_;
    // IMPORTANT: Use content dimensions for video_width/height, not placeholder dimensions
    // This ensures aspect ratio is correct during the transition (color_fbo is at content size)
    video_width = width;
    video_height = height;
    has_video = true;
    exr_texture = 0;

    // Clear color texture to background to prevent showing stale frames
    ClearColorTextureToBackground();

    // Clear video cache (FrameCache) to free RAM
    if (cache_clear_callback) {
        Debug::Log("Clearing video cache before loading image sequence (cross-cache eviction)");
        cache_clear_callback();
    }

    // Clear existing EXR/image sequence cache
    if (exr_cache_) {
        Debug::Log("Clearing existing EXR/image sequence cache before loading new sequence");
        exr_cache_->Shutdown();
        // Process ALL queued texture deletions BEFORE destroying the cache object
        while (exr_cache_->HasPendingTextureDeletions()) {
            exr_cache_->ProcessReadyTextures();
        }
        exr_cache_.reset();
    }

    // Clear thumbnail cache
    if (thumbnail_cache_) {
        Debug::Log("Clearing thumbnail cache before loading image sequence (media switch)");
        thumbnail_cache_->ClearCache();
        thumbnail_cache_.reset();  // Destroy old thumbnail cache
    }

    // Calculate actual sequence duration (prefer cached duration if provided)
    double sequence_duration;
    if (cached_duration > 0.0) {
        sequence_duration = cached_duration;
        Debug::Log("Image sequence using cached duration: " + std::to_string(sequence_duration) + "s");
    } else {
        sequence_duration = static_cast<double>(sequence_files.size()) / fps;
        Debug::Log("Image sequence calculated duration: " + std::to_string(sequence_duration) + "s (" + std::to_string(sequence_files.size()) + " frames)");
    }

    // Initialize virtual timeline (no dummy video needed)
    if (!InitializeImageSequenceTimer(sequence_duration, fps)) {
        Debug::Log("ERROR: Failed to initialize image sequence timer");
        return false;
    }

    // Set content dimensions for rendering
    SetContentDimensions(width, height);

    // Ensure FBO resources exist for rendering
    if (fbo == 0) {
        CreateVideoTextures(width, height);
    }

    Debug::Log("LoadImageSequenceWithCache: Using virtual timeline (no dummy video)");

    // Store sequence data for frame processing (reuse EXR infrastructure)
    exr_sequence_files = sequence_files;
    exr_layer_name = "";  // No layer concept for TIFF/PNG/JPEG
    exr_frame_rate = fps;
    exr_frame_count = static_cast<int>(sequence_files.size());
    exr_sequence_start_frame = start_frame;
    is_exr_mode = true;  // Reuse EXR mode flag for all image sequences
    image_sequence_format = format_name;  // Store detected format (PNG, JPEG, TIFF)

    Debug::Log("Image sequence data stored: " + std::to_string(exr_frame_count) + " frames, start frame: " + std::to_string(start_frame));

    // NEW: Initialize DirectEXRCache with universal loader
    if (!exr_cache_) {
        Debug::Log("VideoPlayer: Creating DirectEXRCache");
        exr_cache_ = std::make_shared<ump::DirectEXRCache>();
    }

    // Use new Initialize overload with IImageLoader
    // Pass initial_playhead so cache starts from correct position when reloading
    Debug::Log("LoadImageSequenceWithCache: Initializing cache at playhead position " + std::to_string(initial_playhead) + "s");
    if (exr_cache_->Initialize(std::move(loader), sequence_files, "", fps, pipeline_mode, start_frame, initial_playhead)) {
        // Update current pipeline mode to match what we just initialized
        current_pipeline_mode = pipeline_mode;

        // Cache the internal format to avoid map lookups every frame
        auto it = PIPELINE_CONFIGS.find(pipeline_mode);
        current_internal_format = (it != PIPELINE_CONFIGS.end()) ? it->second.internal_format : GL_RGBA8;

        Debug::Log("VideoPlayer: Pipeline mode set to " + std::string(PipelineModeToString(pipeline_mode)));

        // Apply current configuration
        ump::DirectEXRCacheConfig config = GetCurrentEXRCacheConfig();
        exr_cache_->SetConfig(config);
        Debug::Log("VideoPlayer: Applied cache config: " +
                   std::to_string(config.readAheadFrames) + " frames ahead, " +
                   std::to_string(config.readBehindSeconds) + "s behind");

        // Sync looping state for seamless wrap-around caching
        exr_cache_->SetLooping(loop_enabled);

        // Start background caching
        exr_cache_->StartBackgroundCaching();
        Debug::Log("VideoPlayer: DirectEXRCache initialized with " + format_name + " loader");
    } else {
        Debug::Log("VideoPlayer: ERROR - Failed to initialize DirectEXRCache");
        exr_cache_.reset();
        return false;
    }

    // NEW: Initialize ThumbnailCache with separate loader instance
    ump::ThumbnailConfig thumb_config = GetCurrentThumbnailConfig();
    if (thumb_config.enabled) {
        Debug::Log("VideoPlayer: Creating ThumbnailCache");

        // Create separate loader instance for thumbnails (format detection again)
        std::unique_ptr<ump::IImageLoader> thumb_loader;
        if (ext == ".tiff" || ext == ".tif") {
            thumb_loader = std::make_unique<ump::TIFFImageLoader>();
        } else if (ext == ".png") {
            thumb_loader = std::make_unique<ump::PNGImageLoader>();
        } else if (ext == ".jpg" || ext == ".jpeg") {
            thumb_loader = std::make_unique<ump::JPEGImageLoader>();
        }

        if (thumb_loader) {
            thumbnail_cache_ = std::make_unique<ump::ThumbnailCache>(
                sequence_files,
                std::move(thumb_loader),
                thumb_config
            );
            Debug::Log("VideoPlayer: ThumbnailCache initialized with " + format_name + " loader, " +
                       std::to_string(thumb_config.width) + "x" + std::to_string(thumb_config.height) +
                       ", cache size: " + std::to_string(thumb_config.cache_size));

            // Prefetch strategic frames for timeline preview
            thumbnail_cache_->PrefetchStrategicFrames(static_cast<int>(sequence_files.size()));
        }
    } else {
        Debug::Log("VideoPlayer: ThumbnailCache disabled by configuration");
        thumbnail_cache_.reset();
    }

    // Synchronously wait for initial frame to be loaded and uploaded
    // This ensures the image sequence displays immediately instead of showing placeholder
    // Use initial_playhead to determine which frame to wait for
    int initial_frame_index = static_cast<int>(initial_playhead * fps);
    if (initial_frame_index < 0) initial_frame_index = 0;
    if (initial_frame_index >= static_cast<int>(sequence_files.size())) {
        initial_frame_index = static_cast<int>(sequence_files.size()) - 1;
    }
    Debug::Log("Waiting for initial frame " + std::to_string(initial_frame_index) + " to load (playhead " + std::to_string(initial_playhead) + "s)...");

    const int MAX_WAIT_ITERATIONS = 100;  // ~2 seconds max (20ms * 100)
    bool first_frame_ready = false;

    for (int i = 0; i < MAX_WAIT_ITERATIONS && !first_frame_ready; i++) {
        // Request frame at initial playhead position
        exr_cache_->RequestFrame(initial_frame_index);

        // Give background thread time to load
        std::this_thread::sleep_for(std::chrono::milliseconds(20));

        // Process any ready textures (uploads to GPU)
        exr_cache_->ProcessReadyTextures();

        // Check if frame is now available
        GLuint texture = 0;
        int tex_width = 0, tex_height = 0;
        texture = exr_cache_->GetTexture(initial_frame_index, tex_width, tex_height);

        if (texture != 0) {
            // Initial frame is ready - update video_texture
            video_texture = texture;
            video_width = tex_width;
            video_height = tex_height;
            exr_texture = texture;
            exr_texture_width = tex_width;
            exr_texture_height = tex_height;
            first_frame_ready = true;
            Debug::Log("Initial frame " + std::to_string(initial_frame_index) + " ready after " + std::to_string((i + 1) * 20) + "ms (texture " +
                       std::to_string(texture) + ", " + std::to_string(tex_width) + "x" + std::to_string(tex_height) + ")");
        }
    }

    if (!first_frame_ready) {
        Debug::Log("WARNING: Initial frame " + std::to_string(initial_frame_index) + " not ready after timeout - will show placeholder until cached");
    }

    Debug::Log("Image sequence loaded successfully with DirectEXRCache");
    return true;
}

int VideoPlayer::CalculateCurrentEXRFrameIndex() const {
    if (!is_exr_mode || exr_sequence_files.empty()) {
        return 0;
    }

    double position = GetPosition();
    double fps = GetFrameRate();

    if (fps <= 0) {
        fps = 24.0; // Fallback FPS
    }

    // Use frame-accurate calculation instead of duration-based progress
    // This ensures each frame gets exactly the right amount of time
    int frame_index = static_cast<int>(std::round(position * fps));

    int sequence_size = static_cast<int>(exr_sequence_files.size());

    // Handle frame clamping for EXR sequences
    // CRITICAL: Always clamp to valid range [0, sequence_size-1]
    // Looping happens via MPV seek command, not frame index wrapping
    // This ensures the last frame stays visible during extended duration
    frame_index = std::clamp(frame_index, 0, sequence_size - 1);

    // Debug: Log timing info when frame changes significantly
    // Note: Static tracking per-sequence handled in InjectCurrentEXRFrame
    static int last_logged_frame = -1;
    static std::string last_sequence_for_log;
    std::string current_sequence = exr_sequence_files[0];

    if (current_sequence != last_sequence_for_log) {
        last_sequence_for_log = current_sequence;
        last_logged_frame = -1; // Reset on sequence change
    }

    if (abs(frame_index - last_logged_frame) > 0) {
       /* Debug::Log("EXR Frame Timing: pos=" + std::to_string(position) +
                   "s, fps=" + std::to_string(fps) +
                   ", calc_frame=" + std::to_string(frame_index) +
                   "/" + std::to_string(sequence_size) +
                   ", loop=" + (loop_enabled ? "ON" : "OFF"));*/
        last_logged_frame = frame_index;
    }

    return frame_index;
}

void VideoPlayer::InjectCurrentEXRFrame() {
    if (!is_exr_mode || exr_sequence_files.empty()) {
        return;
    }

    // Track sequence changes and reset static variables when switching sequences
    static std::string last_sequence_path;
    std::string current_sequence_path = exr_sequence_files[0]; // Use first file as identifier

    static auto last_log_time = std::chrono::steady_clock::now();
    static int last_injected_frame = -1;
    static int last_cache_update_frame = -1;
    static int last_miss_frame = -1;

    if (current_sequence_path != last_sequence_path) {
        // Sequence changed - reset all static tracking variables
        Debug::Log("EXR sequence changed from '" + last_sequence_path + "' to '" + current_sequence_path + "', resetting static tracking");
        last_sequence_path = current_sequence_path;
        last_log_time = std::chrono::steady_clock::now();
        last_injected_frame = -1;
        last_cache_update_frame = -1;
        last_miss_frame = -1;
    }

    // Calculate sequence info and current frame FIRST
    int sequence_size = static_cast<int>(exr_sequence_files.size());
    double sequence_duration = static_cast<double>(sequence_size) / exr_frame_rate;
    int target_frame = CalculateCurrentEXRFrameIndex();

    auto now = std::chrono::steady_clock::now();
    auto ms_since_last = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_log_time).count();

    // Log to see call frequency
    if (ms_since_last > 1000) {  // Every second
        /*Debug::Log("*** InjectCurrentEXRFrame called - target frame: " + std::to_string(target_frame) +
                   " (called every " + std::to_string(ms_since_last) + "ms)");*/
        last_log_time = now;
    }

    // SEAMLESS LOOPING: Now handled by PlaybackTimer callbacks (SetOnLoop, SetOnEnd)
    // - SetOnLoop: Called when timer wraps, logs event
    // - SetOnEnd: Called when reaching end without looping, pauses playback
    // DirectEXRCache wrap-around caching pre-loads frames 0-20 when approaching end (if looping enabled)
    // This allows smooth, hitch-free looping without interruption

    // NOTE: No MPV-based loop detection needed - image_sequence_timer_ handles all timing

    // Use DirectEXRCache with automatic load-on-miss
    if (exr_cache_) {
        GLuint cached_texture = 0;
        int cached_width = 0, cached_height = 0;

        // GetFrameOrLoad tries cache first, loads synchronously on miss
        if (exr_cache_->GetFrameOrLoad(target_frame, cached_texture, cached_width, cached_height)) {
            // OPTIMIZED: Use cached texture directly - no copy needed!
            // The cache owns the texture, we just reference it
            exr_texture = cached_texture;
            exr_texture_width = cached_width;
            exr_texture_height = cached_height;
            exr_current_frame = target_frame;

            // Update video texture reference for rendering
            video_texture = cached_texture;
            video_width = cached_width;
            video_height = cached_height;
            has_video = true;

            // CRITICAL: Update position to match displayed frame (for timeline sync in EXR mode)
            double frame_timestamp = target_frame / exr_frame_rate;
            cached_position = frame_timestamp;

            // OVERRUN MODE: Play one frame at a time at decode speed
            // Timer is paused, we step forward ONLY after displaying a NEW frame
            // This ensures we don't skip frames or step multiple times per frame
            bool already_updated_position = false;
            if (exr_cache_->IsInOverrunMode() && image_sequence_timer_) {
                // Throttle overrun playback to ~1fps minimum to avoid "catchup" bursts
                // when cached frames are available faster than decode speed
                static auto last_overrun_step = std::chrono::steady_clock::now();
                static constexpr auto MIN_OVERRUN_INTERVAL = std::chrono::milliseconds(500); // ~2fps max

                auto now = std::chrono::steady_clock::now();
                bool throttle_ok = (now - last_overrun_step) >= MIN_OVERRUN_INTERVAL;

                // Only step when displaying a NEW frame (not same frame repeated)
                if (target_frame != last_injected_frame && throttle_ok) {
                    last_injected_frame = target_frame;
                    last_overrun_step = now;

                    int total_frames = static_cast<int>(exr_sequence_files.size());
                    bool at_end = (target_frame >= total_frames - 1);

                    // Step forward after displaying this new frame
                    if (!at_end) {
                        image_sequence_timer_->StepForward(1);
                        cached_position = image_sequence_timer_->GetPosition();

                        // CRITICAL: Immediately request next frame so it starts loading NOW
                        // Without this, we wait until next render cycle to request it
                        exr_cache_->UpdateCurrentPosition(cached_position);
                        already_updated_position = true;
                    }
                }
            } else {
                // Normal mode - just track frame display
                if (target_frame != last_injected_frame) {
                    last_injected_frame = target_frame;
                }
            }

            // Update cache position for background processing (throttled - only on frame change)
            // Skip if we already updated in overrun mode stepping above
            if (!already_updated_position && target_frame != last_cache_update_frame) {
                exr_cache_->UpdateCurrentPosition(GetPosition());
                last_cache_update_frame = target_frame;
            }

            return;
        }
        // Frame not cached yet - background thread will load it
        // Update cache position to request this frame (only once per target frame)
        if (target_frame != last_miss_frame) {
            exr_cache_->UpdateCurrentPosition(GetPosition());
            last_miss_frame = target_frame;
        }

        // OVERRUN MODE: Display last good frame while waiting (keeps UI responsive)
        // GetFrameOrLoad sets cached_texture to last_good_texture on miss
        if (exr_cache_->IsInOverrunMode() && cached_texture != 0) {
            video_texture = cached_texture;
            video_width = cached_width;
            video_height = cached_height;
            has_video = true;
            // Don't step - we haven't displayed the NEW frame yet
        }
    }
}

void VideoPlayer::TriggerEXRFrameCaching() {
    // Only cache in EXR mode and if we have a caching callback
    if (!is_exr_mode || !exr_caching_callback) {
        return;
    }

    // Call the caching callback - this will trigger ProjectManager::VideoCache::TryOpportunisticCaching
    // but now it will cache the EXR frame data instead of dummy video data
    exr_caching_callback(this);
}

void VideoPlayer::RenderEXRFrameOverlay(int frame_index) {
    if (!is_exr_mode || exr_sequence_files.empty()) {
        return;
    }

    // Process and upload EXR frame if not already done
    if (frame_index != exr_current_frame) {
        if (!ProcessAndFeedEXRFrame(frame_index)) {
            return;
        }
        exr_current_frame = frame_index;
    }

    // The actual rendering happens through the existing texture pipeline
    // EXR texture is already bound to video_texture by ProcessAndFeedEXRFrame
    // MPV renders the dummy, then our main render loop shows the EXR texture
}

// ============================================================================
// Timeline Mode Frame Injection (matches EXR pattern for smooth playback)
// ============================================================================

void VideoPlayer::SetTimelineMode(bool enabled, ump::TimelinePlaybackController* controller) {
    is_timeline_mode_ = enabled;
    timeline_controller_ = controller;

    // Get timeline dimensions from controller if available
    int target_width = transition_placeholder_width_;
    int target_height = transition_placeholder_height_;
    if (enabled && controller && controller->IsInitialized()) {
        int timeline_width = controller->GetWidth();
        int timeline_height = controller->GetHeight();
        if (timeline_width > 0 && timeline_height > 0) {
            target_width = timeline_width;
            target_height = timeline_height;
            SetContentDimensions(timeline_width, timeline_height);
        }
    }

    // FIRST: Switch video_texture to placeholder BEFORE any cache textures are invalidated
    // This prevents displaying deleted textures during the transition window
    video_texture = transition_placeholder_texture_;
    // IMPORTANT: Use target dimensions for video_width/height, not placeholder dimensions
    // This ensures aspect ratio is correct during the transition (color_fbo is at target size)
    video_width = target_width;
    video_height = target_height;
    has_video = true;

    // Clear color texture to background to prevent showing stale frames
    ClearColorTextureToBackground();

    if (enabled) {
        // Timeline mode ENABLED
        // CRITICAL: Reset timeline texture tracking when switching timelines
        // The old timeline_texture_ may point to a deleted texture from the previous timeline's cache
        timeline_texture_ = 0;
        timeline_texture_width_ = 0;
        timeline_texture_height_ = 0;
        last_timeline_frame_ = -1;

        // === ENSURE VIDEO FBO RESOURCES EXIST ===
        // Timeline rendering via UpdateVideoTexture() requires fbo/mpv_fbo/mpv_texture to be valid.
        // Create them at target dimensions if they don't exist (first timeline load case).
        if (fbo == 0 || mpv_fbo == 0 || mpv_texture == 0) {
            Debug::Log("Creating video FBO resources for timeline mode (first load case)");
            CreateVideoTextures(target_width, target_height);
        }

        // ALWAYS recreate gap placeholder texture when entering timeline mode
        // This ensures it's fresh and valid, avoiding issues when switching timelines
        // Use content dimensions to prevent FBO resize when showing placeholder
        if (gap_placeholder_texture_ != 0) {
            glDeleteTextures(1, &gap_placeholder_texture_);
            gap_placeholder_texture_ = 0;
        }

        // Create placeholder at content dimensions (prevents FBO resize on gaps)
        int placeholder_w = target_width > 0 ? target_width : 1920;
        int placeholder_h = target_height > 0 ? target_height : 1080;

        std::vector<unsigned char> black_pixels(placeholder_w * placeholder_h * 4, 0);
        for (size_t i = 3; i < black_pixels.size(); i += 4) {
            black_pixels[i] = 255;  // Alpha = opaque
        }

        glGenTextures(1, &gap_placeholder_texture_);
        glBindTexture(GL_TEXTURE_2D, gap_placeholder_texture_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, placeholder_w, placeholder_h, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, black_pixels.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glBindTexture(GL_TEXTURE_2D, 0);

        gap_placeholder_width_ = placeholder_w;
        gap_placeholder_height_ = placeholder_h;
        Debug::Log("VideoPlayer: Created gap placeholder texture at " +
                   std::to_string(placeholder_w) + "x" + std::to_string(placeholder_h) +
                   " (ID: " + std::to_string(gap_placeholder_texture_) + ")");

        if (controller) {
            // CRITICAL: Shutdown EXR cache to free memory - timeline uses TimelineCache instead
            if (exr_cache_) {
                Debug::Log("VideoPlayer: Timeline mode - shutting down EXR cache");
                exr_cache_->Shutdown();
                exr_cache_.reset();
            }
            // Also clear thumbnail cache (not needed for timeline - uses TimelineCache)
            if (thumbnail_cache_) {
                Debug::Log("VideoPlayer: Timeline mode - clearing thumbnail cache");
                thumbnail_cache_->ClearCache();
                thumbnail_cache_.reset();
            }
            Debug::Log("VideoPlayer: Timeline mode ENABLED with controller - cleared EXR and thumbnail caches");
        } else {
            Debug::Log("VideoPlayer: Timeline mode ENABLED without controller (UI only)");
        }
    } else {
        Debug::Log("VideoPlayer: Timeline mode DISABLED");
        // Reset timeline texture tracking
        timeline_texture_ = 0;
        timeline_texture_width_ = 0;
        timeline_texture_height_ = 0;
        last_timeline_frame_ = -1;

        // Clean up gap placeholder texture
        if (gap_placeholder_texture_ != 0) {
            glDeleteTextures(1, &gap_placeholder_texture_);
            gap_placeholder_texture_ = 0;
            gap_placeholder_width_ = 0;
            gap_placeholder_height_ = 0;
            Debug::Log("VideoPlayer: Deleted gap placeholder texture");
        }

        // Clear content dimensions (overlay mode was used for timeline)
        // The next media load will set appropriate dimensions
        content_width_ = 0;
        content_height_ = 0;
        use_content_dimensions_ = false;
        Debug::Log("VideoPlayer: Cleared content dimensions (exiting timeline mode)");

        // CRITICAL: Reset color texture dimension tracking
        // This forces SetupColorProcessingResources to recreate at correct dimensions
        // Otherwise stale timeline dimensions cause render mismatches
        color_texture_width_ = 0;
        color_texture_height_ = 0;
        Debug::Log("VideoPlayer: Reset color texture dimension tracking");

        // CRITICAL: Force FBO recreation on next video load
        // Timeline mode may have created FBOs at timeline dimensions (e.g., 1920x1080)
        // The next video may have different dimensions and needs fresh FBOs
        // Delete existing FBOs so LoadFile/UpdateProperties will recreate them
        if (fbo != 0) {
            glDeleteFramebuffers(1, &fbo);
            fbo = 0;
        }
        if (mpv_fbo != 0) {
            glDeleteFramebuffers(1, &mpv_fbo);
            mpv_fbo = 0;
        }
        if (mpv_texture != 0) {
            glDeleteTextures(1, &mpv_texture);
            mpv_texture = 0;
        }
        // Don't delete video_texture if it's the transition placeholder
        if (video_texture != 0 && video_texture != transition_placeholder_texture_) {
            glDeleteTextures(1, &video_texture);
        }
        video_texture = transition_placeholder_texture_;
        video_width = transition_placeholder_width_;
        video_height = transition_placeholder_height_;
        Debug::Log("VideoPlayer: Cleared FBO resources (exiting timeline mode) - will be recreated on next load");
    }
}

void VideoPlayer::SetContentDimensions(int width, int height) {
    content_width_ = width;
    content_height_ = height;
    use_content_dimensions_ = (width > 0 && height > 0);

    if (use_content_dimensions_) {
        Debug::Log("VideoPlayer: Content dimensions set to " +
                   std::to_string(width) + "x" + std::to_string(height) +
                   " (overlay mode enabled)");

        // CRITICAL: Immediately recreate color processing resources at new dimensions
        // This prevents dimension mismatch artifacts during transitions (e.g., 1080p → UHD)
        // Without this, the old-size color_texture is used until UpdateProperties() runs
        if (color_pipeline && color_pipeline->IsValid()) {
            Debug::Log("VideoPlayer: Recreating color resources for new dimensions");
            CreateColorProcessingResourcesForMode(width, height, current_pipeline_mode);
            // NOTE: Don't clear the texture - let it show undefined content briefly
            // rather than flashing to background. The new frame will overwrite it.
        }
    } else {
        Debug::Log("VideoPlayer: Content dimensions cleared (overlay mode disabled)");
    }
}

void VideoPlayer::CreateTransitionPlaceholder() {
    // Delete existing if any
    if (transition_placeholder_texture_ != 0) {
        glDeleteTextures(1, &transition_placeholder_texture_);
        transition_placeholder_texture_ = 0;
    }

    // Create a small dark gray texture (64x64) matching background color
    const int size = 64;
    glGenTextures(1, &transition_placeholder_texture_);
    glBindTexture(GL_TEXTURE_2D, transition_placeholder_texture_);

    // Dark gray pixels matching background (#1b1b1b = RGB 27,27,27)
    std::vector<unsigned char> pixels(size * size * 4);
    for (size_t i = 0; i < pixels.size(); i += 4) {
        pixels[i] = 27;      // R
        pixels[i + 1] = 27;  // G
        pixels[i + 2] = 27;  // B
        pixels[i + 3] = 255; // A (opaque)
    }

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, size, size, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D, 0);

    transition_placeholder_width_ = size;
    transition_placeholder_height_ = size;

    Debug::Log("VideoPlayer: Created transition placeholder texture: " + std::to_string(transition_placeholder_texture_));
}

void VideoPlayer::ClearVideoTextureToBackground() {
    // Clear the video texture through the FBO pipeline to prevent font cache flicker
    // This keeps OpenGL state consistent by rendering through the same path as real frames

    if (fbo == 0 || video_texture == 0) {
        // No FBO yet - can't clear through pipeline
        return;
    }

    // Save current framebuffer binding
    GLint current_fbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &current_fbo);

    // Bind our video FBO and clear to background color (#1b1b1b)
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glClearColor(27.0f/255.0f, 27.0f/255.0f, 27.0f/255.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    // Restore previous framebuffer
    glBindFramebuffer(GL_FRAMEBUFFER, current_fbo);

    // Ensure has_video stays true so render path uses video_texture
    has_video = true;
}

void VideoPlayer::ClearColorTextureToBackground() {
    // Clear the color texture to background color during media transitions
    // This prevents showing stale frames at wrong dimensions when switching media

    if (color_fbo == 0 || color_texture == 0) {
        // No color FBO yet - nothing to clear
        return;
    }

    // Save current framebuffer binding
    GLint current_fbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &current_fbo);

    // Bind color FBO and clear to background color (#1b1b1b)
    glBindFramebuffer(GL_FRAMEBUFFER, color_fbo);
    glClearColor(27.0f/255.0f, 27.0f/255.0f, 27.0f/255.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    // Restore previous framebuffer
    glBindFramebuffer(GL_FRAMEBUFFER, current_fbo);

    Debug::Log("VideoPlayer: Cleared color texture to background");
}

void VideoPlayer::InjectCurrentTimelineFrame() {
    static int inject_call_count = 0;
    inject_call_count++;

    if (!is_timeline_mode_ || !timeline_controller_) {
       /* if (inject_call_count <= 5) {
            Debug::Log("InjectCurrentTimelineFrame: Not in timeline mode or no controller");
        }*/
        return;
    }

    if (!timeline_controller_->IsInitialized()) {
        /*if (inject_call_count <= 5) {
            Debug::Log("InjectCurrentTimelineFrame: Controller not initialized");
        }*/
        return;
    }

    // Process pending GPU uploads from background I/O threads (like EXR mode)
    timeline_controller_->ProcessPendingUploads();

    // NOTE: Gap detection removed - the timeline cache now returns a properly-sized
    // gap texture for gaps/unlinked clips. This prevents OpenGL corruption from
    // constant FBO resize when transitioning between clips and 1x1 gap textures.
    // The cache's gap texture is created at timeline content dimensions.

    // Get current frame from the playback controller
    // The controller syncs from MPV internally
    int frame_width = 0, frame_height = 0;
    GLuint frame_texture = timeline_controller_->Update(frame_width, frame_height);

    /*if (inject_call_count <= 10 || inject_call_count % 300 == 0) {
        Debug::Log("InjectCurrentTimelineFrame: Update returned texture=" +
                   std::to_string(frame_texture) + ", " +
                   std::to_string(frame_width) + "x" + std::to_string(frame_height));
    }*/

    if (frame_texture != 0) {
        // SUCCESS: Got a valid frame from timeline cache
        // Update our tracking variables
        timeline_texture_ = frame_texture;
        timeline_texture_width_ = frame_width;
        timeline_texture_height_ = frame_height;

        // CRITICAL: Replace video_texture with cached timeline frame (like EXR mode)
        // This integrates timeline frames into the existing render pipeline
        video_texture = frame_texture;
        video_width = frame_width;
        video_height = frame_height;
        has_video = true;

        // Update position tracking from MPV (for timeline sync)
        int current_frame = timeline_controller_->GetCurrentFrame();
        if (current_frame != last_timeline_frame_) {
            // Frame changed - could add debug logging here if needed
            last_timeline_frame_ = current_frame;
        }
    } else {
        // Cache miss - use previous frame or content dimensions placeholder
        // This should be rare now that the cache returns gap textures for gaps
        if (timeline_texture_ != 0) {
            // Keep previous timeline frame while loading
            video_texture = timeline_texture_;
            video_width = timeline_texture_width_;
            video_height = timeline_texture_height_;
            has_video = true;
        } else {
            // No previous frame - use content dimensions to prevent FBO resize
            // Use content dimensions if set, otherwise use standard HD
            int placeholder_w = (use_content_dimensions_ && content_width_ > 0) ? content_width_ : 1920;
            int placeholder_h = (use_content_dimensions_ && content_height_ > 0) ? content_height_ : 1080;

            // Check if we need to recreate the placeholder at new dimensions
            if (gap_placeholder_texture_ == 0 ||
                gap_placeholder_width_ != placeholder_w ||
                gap_placeholder_height_ != placeholder_h) {
                // Delete old texture if exists
                if (gap_placeholder_texture_ != 0) {
                    glDeleteTextures(1, &gap_placeholder_texture_);
                }

                // Create properly-sized black texture
                std::vector<unsigned char> black_pixels(placeholder_w * placeholder_h * 4, 0);
                for (size_t i = 3; i < black_pixels.size(); i += 4) {
                    black_pixels[i] = 255;  // Alpha = opaque
                }

                glGenTextures(1, &gap_placeholder_texture_);
                glBindTexture(GL_TEXTURE_2D, gap_placeholder_texture_);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, placeholder_w, placeholder_h, 0,
                             GL_RGBA, GL_UNSIGNED_BYTE, black_pixels.data());
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glBindTexture(GL_TEXTURE_2D, 0);

                gap_placeholder_width_ = placeholder_w;
                gap_placeholder_height_ = placeholder_h;
                Debug::Log("InjectCurrentTimelineFrame: Created gap placeholder at " +
                           std::to_string(placeholder_w) + "x" + std::to_string(placeholder_h));
            }

            video_texture = gap_placeholder_texture_;
            video_width = gap_placeholder_width_;
            video_height = gap_placeholder_height_;
            has_video = true;
        }
    }
}

// ============================================================================
// Pipeline Mode System Methods
// ============================================================================

void VideoPlayer::SetPipelineMode(PipelineMode mode) {
    if (mode == current_pipeline_mode) {
        return; // No change needed
    }

    Debug::Log("Switching pipeline mode from " + std::string(PipelineModeToString(current_pipeline_mode)) +
               " to " + std::string(PipelineModeToString(mode)));

    // Store current playback state
    double current_position = GetPosition();
    bool was_playing = IsPlaying();

    if (was_playing) {
        Pause(); // Pause during transition
    }

    // Update MPV configuration for new mode
    ApplyPipelineModeConfig(mode);

    // Recreate render context for new format requirements
    if (mpv_gl) {
        mpv_render_context_free(mpv_gl);
        mpv_gl = nullptr;
    }

    // Recreate OpenGL context with new pipeline settings
    current_pipeline_mode = mode;

    // Cache the internal format to avoid map lookups every frame
    auto it = PIPELINE_CONFIGS.find(mode);
    current_internal_format = (it != PIPELINE_CONFIGS.end()) ? it->second.internal_format : GL_RGBA8;

    if (!SetupOpenGLForMode(mode)) {
        Debug::Log("Failed to recreate OpenGL context for pipeline mode " + std::string(PipelineModeToString(mode)));
        return;
    }

    // Recreate video textures with new format
    if (video_width > 0 && video_height > 0) {
        CreateVideoTexturesForMode(video_width, video_height, mode);

        // Also recreate color processing resources if OCIO pipeline is active
        if (HasColorPipeline()) {
            CreateColorProcessingResourcesForMode(video_width, video_height, mode);
        }
    }

    // Resume playback if it was playing before
    if (was_playing) {
        Play();
    }

    Debug::Log("Pipeline mode switch completed successfully");
}

const PipelineConfig& VideoPlayer::GetCurrentPipelineConfig() const {
    auto it = PIPELINE_CONFIGS.find(current_pipeline_mode);
    if (it != PIPELINE_CONFIGS.end()) {
        return it->second;
    }
    return PIPELINE_CONFIGS.at(PipelineMode::NORMAL); // Fallback
}

bool VideoPlayer::SupportsPipelineMode(PipelineMode mode) const {
    // All modes are supported - this could be extended to check GPU capabilities
    return PIPELINE_CONFIGS.find(mode) != PIPELINE_CONFIGS.end();
}

void VideoPlayer::CreateVideoTexturesForMode(int width, int height, PipelineMode mode) {
    if (width <= 0 || height <= 0) {
        return;
    }

    auto it = PIPELINE_CONFIGS.find(mode);
    if (it == PIPELINE_CONFIGS.end()) {
        Debug::Log("CreateVideoTexturesForMode: Unknown pipeline mode");
        return;
    }

    const PipelineConfig& config = it->second;

    // Store old texture to delete AFTER new one is created
    GLuint old_video_texture = video_texture;
    GLuint old_fbo = fbo;
    GLuint old_mpv_texture = mpv_texture;
    GLuint old_mpv_fbo = mpv_fbo;

    // Clear existing texture through FBO during recreation to prevent flicker
    // This keeps OpenGL state consistent rather than swapping to a different texture
    ClearVideoTextureToBackground();

    // Create new OpenGL texture with pipeline-specific format
    GLuint new_texture = 0;
    glGenTextures(1, &new_texture);
    glBindTexture(GL_TEXTURE_2D, new_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, config.internal_format, width, height,
        0, GL_RGBA, config.data_type, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    // Create new FBO for final output (after color correction)
    GLuint new_fbo = 0;
    glGenFramebuffers(1, &new_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, new_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
        GL_TEXTURE_2D, new_texture, 0);

    // Check FBO completeness
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        Debug::Log("ERROR: Video FBO incomplete for " + std::string(PipelineModeToString(mode)) +
                   "! Status: " + std::to_string(status));
    }

    // Create separate MPV rendering texture and FBO to break pipeline stalls
    GLuint new_mpv_texture = 0;
    glGenTextures(1, &new_mpv_texture);
    glBindTexture(GL_TEXTURE_2D, new_mpv_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, config.internal_format, width, height,
        0, GL_RGBA, config.data_type, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    // Create MPV FBO
    GLuint new_mpv_fbo = 0;
    glGenFramebuffers(1, &new_mpv_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, new_mpv_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
        GL_TEXTURE_2D, new_mpv_texture, 0);

    // Check MPV FBO completeness
    GLenum mpv_status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (mpv_status != GL_FRAMEBUFFER_COMPLETE) {
        Debug::Log("ERROR: MPV FBO incomplete for " + std::string(PipelineModeToString(mode)) +
                   "! Status: " + std::to_string(mpv_status));
    } else {
        Debug::Log("Created video textures for " + std::string(PipelineModeToString(mode)) + ": " +
                   std::to_string(width) + "x" + std::to_string(height) + " (with separate MPV FBO)");
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // NOW assign new resources and clean up old ones
    // This order ensures video_texture is never 0 during the transition
    video_texture = new_texture;
    fbo = new_fbo;
    mpv_texture = new_mpv_texture;
    mpv_fbo = new_mpv_fbo;

    // Clean up old resources AFTER new ones are assigned
    if (old_video_texture && old_video_texture != transition_placeholder_texture_) {
        glDeleteTextures(1, &old_video_texture);
    }
    if (old_fbo) {
        glDeleteFramebuffers(1, &old_fbo);
    }
    if (old_mpv_texture) {
        glDeleteTextures(1, &old_mpv_texture);
    }
    if (old_mpv_fbo) {
        glDeleteFramebuffers(1, &old_mpv_fbo);
    }
}

void VideoPlayer::CreateColorProcessingResourcesForMode(int width, int height, PipelineMode mode) {
    if (width <= 0 || height <= 0) {
        return;
    }

    auto it = PIPELINE_CONFIGS.find(mode);
    if (it == PIPELINE_CONFIGS.end()) {
        return;
    }

    const PipelineConfig& config = it->second;

    // Clean up existing color processing resources
    if (color_texture) {
        glDeleteTextures(1, &color_texture);
        color_texture = 0;
        color_texture_width_ = 0;
        color_texture_height_ = 0;
    }
    if (color_fbo) {
        glDeleteFramebuffers(1, &color_fbo);
        color_fbo = 0;
    }

    // Create FBO for color processing
    glGenFramebuffers(1, &color_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, color_fbo);

    // Create color texture with pipeline-specific format
    glGenTextures(1, &color_texture);
    glBindTexture(GL_TEXTURE_2D, color_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, config.internal_format, width, height,
        0, GL_RGBA, config.data_type, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // Track color texture dimensions for viewport calculations
    color_texture_width_ = width;
    color_texture_height_ = height;

    // Attach to FBO
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
        GL_TEXTURE_2D, color_texture, 0);

    // Check FBO completeness
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        Debug::Log("ERROR: Color FBO incomplete for " + std::string(PipelineModeToString(mode)) +
                   "! Status: " + std::to_string(status));
        color_texture_width_ = 0;
        color_texture_height_ = 0;
    } else {
        Debug::Log("Created color processing resources for " + std::string(PipelineModeToString(mode)) + ": " +
                   std::to_string(width) + "x" + std::to_string(height));
        // NOTE: Don't clear the texture here - the passthrough pipeline will render the
        // placeholder/previous frame into it, providing a stable buffer during transitions.
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void VideoPlayer::ApplyPipelineModeConfig(PipelineMode mode) {
    auto it = PIPELINE_CONFIGS.find(mode);
    if (it == PIPELINE_CONFIGS.end()) {
        Debug::Log("ApplyPipelineModeConfig: Unknown pipeline mode");
        return;
    }

    const PipelineConfig& config = it->second;

    switch (mode) {
        case PipelineMode::NORMAL:
            mpv_set_option_string(mpv, "tone-mapping", "off");
            mpv_set_option_string(mpv, "opengl-fbo-format", "rgba8");
            // Reset linear processing settings that may have been set by float modes
            mpv_set_option_string(mpv, "target-trc", "auto");
            mpv_set_option_string(mpv, "target-prim", "auto");
            mpv_set_option_string(mpv, "linear-scaling", "no");
            mpv_set_option_string(mpv, "target-colorspace", "auto");
            Debug::Log("Applied NORMAL pipeline config - RGBA8 standard processing");
            break;

        case PipelineMode::HIGH_RES:
            mpv_set_option_string(mpv, "tone-mapping", "off");
            mpv_set_option_string(mpv, "opengl-fbo-format", "rgba16");
            // Reset linear processing settings that may have been set by float modes
            mpv_set_option_string(mpv, "target-trc", "auto");
            mpv_set_option_string(mpv, "target-prim", "auto");
            mpv_set_option_string(mpv, "linear-scaling", "no");
            mpv_set_option_string(mpv, "target-colorspace", "auto");
            Debug::Log("Applied HIGH_RES pipeline config - RGBA16 12-bit precision for OCIO");
            break;

        case PipelineMode::ULTRA_HIGH_RES:
            mpv_set_option_string(mpv, "tone-mapping", "linear");
            mpv_set_option_string(mpv, "target-trc", "linear");
            mpv_set_option_string(mpv, "linear-scaling", "yes");
            mpv_set_option_string(mpv, "opengl-fbo-format", "rgba16f");
            // No target-prim - preserve source primaries for OCIO flexibility
            Debug::Log("Applied ULTRA_HIGH_RES pipeline config - RGBA16F linear processing for maximum OCIO flexibility");
            break;

        case PipelineMode::HDR_RES:
            mpv_set_option_string(mpv, "tone-mapping", "linear");
            mpv_set_option_string(mpv, "target-trc", "linear");
            mpv_set_option_string(mpv, "target-prim", "rec2020");  // HDR display target
            mpv_set_option_string(mpv, "linear-scaling", "yes");
            mpv_set_option_string(mpv, "opengl-fbo-format", "rgba16f");
            mpv_set_option_string(mpv, "target-colorspace", "bt.2020");  // HDR colorspace
            mpv_set_option_string(mpv, "target-colorspace-hint", "yes");  // Signal HDR display capability
            Debug::Log("Applied HDR_RES pipeline config - RGBA16F linear processing with Rec.2020 targeting");
            break;
    }
}

size_t VideoPlayer::GetRecommendedCacheSize() const {
    auto it = PIPELINE_CONFIGS.find(current_pipeline_mode);
    if (it != PIPELINE_CONFIGS.end()) {
        return it->second.recommended_cache_mb;
    }
    return 4096; // Default 4GB
}

size_t VideoPlayer::GetMaxCacheSize() const {
    auto it = PIPELINE_CONFIGS.find(current_pipeline_mode);
    if (it != PIPELINE_CONFIGS.end()) {
        return it->second.max_cache_mb;
    }
    return 16384; // Default 16GB
}

bool VideoPlayer::SetupOpenGLForMode(PipelineMode mode) {
    auto it = PIPELINE_CONFIGS.find(mode);
    if (it == PIPELINE_CONFIGS.end()) {
        Debug::Log("SetupOpenGLForMode: Unknown pipeline mode");
        return false;
    }

    const PipelineConfig& config = it->second;

    // Setup MPV OpenGL rendering context with pipeline-specific format information
    mpv_opengl_init_params gl_init_params = {
        GetProcAddress,
        nullptr,
    };

    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_API_TYPE, const_cast<char*>(MPV_RENDER_API_TYPE_OPENGL)},
        {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl_init_params},
        {MPV_RENDER_PARAM_INVALID, nullptr}
    };

    if (mpv_render_context_create(&mpv_gl, mpv, params) < 0) {
        Debug::Log("Failed to create MPV render context for " + std::string(PipelineModeToString(mode)));
        return false;
    }

    Debug::Log("MPV render context created successfully for " + std::string(PipelineModeToString(mode)) +
               " mode with " + (config.internal_format == GL_RGBA8 ? "RGBA8" :
                               config.internal_format == GL_RGBA16 ? "RGBA16" : "RGBA16F") + " format");
    return true;
}

// EXR Cache Implementation (NEW: Using DirectEXRCache)

void VideoPlayer::InitializeEXRCache(const std::vector<std::string>& sequence_files,
                                     const std::string& layer_name, double fps,
                                     double initial_position) {
    Debug::Log("VideoPlayer::InitializeEXRCache - " + std::to_string(sequence_files.size()) +
               " files, layer: " + layer_name);

    // Cache created in constructor with threads always running
    // Just call Initialize to swap sequences (threads stay alive)
    if (!exr_cache_) {
        Debug::Log("VideoPlayer: ERROR - EXR cache should be pre-created in constructor!");
        exr_cache_ = std::make_shared<ump::DirectEXRCache>();
    }

    // Determine starting position for cache
    // If initial_position < 0, use current playhead position from timer
    double cache_start_position = initial_position;
    if (cache_start_position < 0.0) {
        if (image_sequence_timer_) {
            cache_start_position = image_sequence_timer_->GetPosition();
            Debug::Log("VideoPlayer::InitializeEXRCache - Using current playhead position: " +
                       std::to_string(cache_start_position) + "s");
        } else {
            cache_start_position = 0.0;
            Debug::Log("VideoPlayer::InitializeEXRCache - No timer, starting from 0");
        }
    }

    // Load new sequence (threads keep running, just swap data)
    // Create EXR loader for universal pipeline (ensures consistent path with other image loaders)
    auto exr_loader = std::make_unique<ump::EXRImageLoader>();
    if (exr_cache_->Initialize(std::move(exr_loader), sequence_files, layer_name, fps, PipelineMode::HDR_RES, exr_sequence_start_frame, cache_start_position)) {
        // Apply current configuration
        ump::DirectEXRCacheConfig config = GetCurrentEXRCacheConfig();
        exr_cache_->SetConfig(config);
        Debug::Log("VideoPlayer: Applied cache config: " +
                   std::to_string(config.readAheadFrames) + " frames ahead, " +
                   std::to_string(config.readBehindSeconds) + "s behind");

        // Sync looping state for seamless wrap-around caching
        exr_cache_->SetLooping(loop_enabled);

        // Start background caching
        exr_cache_->StartBackgroundCaching();
        Debug::Log("VideoPlayer: DirectEXRCache initialized and background caching started");
    } else {
        Debug::Log("VideoPlayer: ERROR - Failed to initialize DirectEXRCache");
        exr_cache_.reset();
    }
}

void VideoPlayer::SetEXRCacheWindow(double seconds) {
    if (exr_cache_) {
        exr_cache_->SetCacheWindow(seconds);
        Debug::Log("VideoPlayer: EXR cache window set to " + std::to_string(seconds) + " seconds");
    }
}

void VideoPlayer::SetEXRCacheConfig(const ump::DirectEXRCacheConfig& config) {
    if (exr_cache_) {
        exr_cache_->SetConfig(config);
        Debug::Log("VideoPlayer: DirectEXRCache configuration updated");
    }
}

void VideoPlayer::SetEXRCacheEnabled(bool enabled) {
    if (exr_cache_) {
        exr_cache_->SetCachingEnabled(enabled);
        Debug::Log("VideoPlayer: EXR cache " + std::string(enabled ? "enabled" : "disabled"));
    }
}

void VideoPlayer::ClearEXRCache() {
    if (exr_cache_) {
        exr_cache_->Shutdown();
        Debug::Log("VideoPlayer: EXR cache shut down (fully cleared and uninitialized)");
    }
}

void VideoPlayer::ClearVideoTextureReference() {
    // Clear the render texture reference to prevent dangling pointer during cache reinitialization
    // This should be called BEFORE cache reinitialization queues old textures for deletion
    // Otherwise the render loop may try to use a texture that's been deleted = GL corruption
    //
    // NOTE: In EXR mode, video_texture points to EXR cache textures (not FBO-attached)
    // so we can't use ClearVideoTextureToBackground() - must use placeholder fallback
    if (is_exr_mode && video_texture == exr_texture && exr_texture != 0) {
        Debug::Log("VideoPlayer: Clearing EXR texture reference before cache reinitialization");
        // Use placeholder as fallback since EXR textures aren't FBO-attached
        video_texture = transition_placeholder_texture_;
        video_width = transition_placeholder_width_;
        video_height = transition_placeholder_height_;
        exr_texture = 0;

        // Clear color texture to background to prevent showing stale frames
        ClearColorTextureToBackground();
    }

    // TIMELINE MODE: Clear timeline texture reference before cache clears textures
    // Without this, timeline_texture_ may point to a deleted texture after edit = crash
    if (is_timeline_mode_ && timeline_texture_ != 0) {
        Debug::Log("VideoPlayer: Clearing timeline texture reference before cache reinitialization");
        // Use gap placeholder as fallback - it has correct dimensions
        if (gap_placeholder_texture_ != 0) {
            video_texture = gap_placeholder_texture_;
            video_width = gap_placeholder_width_;
            video_height = gap_placeholder_height_;
        } else if (transition_placeholder_texture_ != 0) {
            video_texture = transition_placeholder_texture_;
            video_width = transition_placeholder_width_;
            video_height = transition_placeholder_height_;
        }
        timeline_texture_ = 0;
        timeline_texture_width_ = 0;
        timeline_texture_height_ = 0;

        // Clear color texture to background to prevent showing stale frames
        ClearColorTextureToBackground();
    }
}

bool VideoPlayer::HasEXRCache() const {
    return exr_cache_ && exr_cache_->IsInitialized();
}

ump::DirectEXRCache::CacheStats VideoPlayer::GetEXRCacheStats() const {
    if (exr_cache_) {
        return exr_cache_->GetStats();
    }
    return ump::DirectEXRCache::CacheStats{};
}

std::vector<ump::CacheSegment> VideoPlayer::GetEXRCacheSegments() const {
    if (exr_cache_ && exr_cache_->IsInitialized()) {
        return exr_cache_->GetCacheSegments();
    }
    return std::vector<ump::CacheSegment>();
}

void VideoPlayer::SetCacheSettings(const std::string& custom_path, int retention_days,
                                   int transcode_max_gb, bool clear_on_exit) {
    // Note: DummyVideoGenerator removed - no longer need dummy video cache settings

    Debug::Log("VideoPlayer: Disk cache settings updated - retention=" + std::to_string(retention_days) +
              " days, transcode limit=" + std::to_string(transcode_max_gb) +
              " GB, clear on exit=" + std::string(clear_on_exit ? "ON" : "OFF"));
}

size_t VideoPlayer::ClearEXRDiskCache() {
    size_t total_bytes = 0;

    // Note: DummyVideoGenerator removed - no longer create dummy video files

    // Clear EXR transcodes
    // NOTE: We create a temporary transcoder and configure it with the current cache settings
    static ump::EXRTranscoder transcoder;

    // Configure transcoder with current cache settings (custom path, retention, etc.)
    extern std::string g_custom_cache_path;
    extern int g_cache_retention_days;
    extern int g_transcode_cache_max_gb;
    extern bool g_clear_cache_on_exit;

    transcoder.SetCacheConfig(g_custom_cache_path, g_cache_retention_days,
                              g_transcode_cache_max_gb, g_clear_cache_on_exit);

    total_bytes += transcoder.ClearAllTranscodes();

    return total_bytes;
}

// ============================================================================
// Thumbnail Cache (for timeline scrubbing)
// ============================================================================

GLuint VideoPlayer::GetThumbnailForFrame(int frame, bool allow_fallback, int* out_actual_frame) {
    if (!thumbnail_cache_) {
        return 0;  // No thumbnail cache available
    }
    GLuint texture_id = thumbnail_cache_->GetThumbnail(frame, allow_fallback, out_actual_frame);

    static int log_counter = 0;
    if (log_counter++ % 100 == 0) {  // Log every 100th request to avoid spam
        Debug::Log("VideoPlayer::GetThumbnailForFrame: frame=" + std::to_string(frame) +
                   ", texture_id=" + std::to_string(texture_id) +
                   ", fallback=" + std::string(allow_fallback ? "true" : "false"));
    }

    return texture_id;
}

bool VideoPlayer::GetThumbnailSize(int frame, int& width, int& height) const {
    if (!thumbnail_cache_) {
        width = 0;
        height = 0;
        return false;
    }
    return thumbnail_cache_->GetCachedThumbnailSize(frame, width, height);
}

bool VideoPlayer::HasThumbnailCache() const {
    return thumbnail_cache_ != nullptr;
}

void VideoPlayer::ClearThumbnailCache() {
    if (thumbnail_cache_) {
        thumbnail_cache_->ClearCache();
        Debug::Log("VideoPlayer: Thumbnail cache cleared");
    }
}

// ============================================================================
// Dual Video Review / Comparison Mode Implementation
// ============================================================================

void VideoPlayer::EnableComparisonMode(bool enabled) {
    Debug::Log("VideoPlayer: EnableComparisonMode called with: " + std::string(enabled ? "true" : "false"));

    if (enabled == comparison_mode_enabled_) {
        return; // Already in desired state
    }

    comparison_mode_enabled_ = enabled;

    if (enabled) {
        // If we're currently showing an image sequence, clear it
        // Image sequences are not supported in comparison mode
        if (is_exr_mode) {
            Debug::Log("VideoPlayer: Clearing image sequence before entering comparison mode");
            ResetState();
        }

        // Initialize comparison video player
        if (!comparison_video_) {
            comparison_video_ = std::make_unique<ump::ComparisonVideoPlayer>();
            if (!comparison_video_->Initialize()) {
                Debug::Log("ERROR: Failed to initialize comparison video player");
                comparison_video_.reset();
                comparison_mode_enabled_ = false;
                return;
            }
        }

        // Default to side-by-side mode
        comparison_mode_ = ComparisonMode::SIDE_BY_SIDE;

        // Initialize left clip from primary video if already loaded
        if (HasVideo()) {
            UpdateDualViewClipFromPrimary();
        }

        Debug::Log("VideoPlayer: Comparison mode enabled (side-by-side)");
    } else {
        // Exit lavfi mode if currently active
        if (IsLavfiMode(comparison_mode_)) {
            Debug::Log("EnableComparisonMode(false): Exiting lavfi mode");
            ExitLavfiMode();
            return; // ExitLavfiMode already handles cleanup
        }

        // Restore original video if we were in difference mode
        if (comparison_mode_ == ComparisonMode::DIFFERENCE_VIEW && !original_video_path_before_difference_.empty() && is_exr_mode) {
            Debug::Log("VideoPlayer: Restoring original video after exiting difference mode: " + original_video_path_before_difference_);
            LoadFile(original_video_path_before_difference_);
            original_video_path_before_difference_.clear();
        }

        // Clean up dual view timer
        if (dual_view_timer_) {
            dual_view_timer_.reset();
            Debug::Log("VideoPlayer: Dual view timer destroyed");
        }

        // Clean up comparison video
        if (comparison_video_) {
            comparison_video_->Cleanup();
            comparison_video_.reset();
        }

        // Clean up difference compositor
        CleanupDifferenceCompositor();

        // Clear dual view timeline data
        dual_view_timeline_ = ump::DualViewTimeline();

        comparison_mode_ = ComparisonMode::DISABLED;

        Debug::Log("VideoPlayer: Comparison mode disabled");
    }
}

bool VideoPlayer::IsComparisonModeEnabled() const {
    return comparison_mode_enabled_;
}

void VideoPlayer::LoadComparisonVideo(const std::string& path) {
    if (!comparison_mode_enabled_) {
        Debug::Log("ERROR: Cannot load comparison video - comparison mode not enabled");
        return;
    }

    if (!comparison_video_) {
        Debug::Log("ERROR: Comparison video player not initialized");
        return;
    }

    Debug::Log("VideoPlayer: Loading comparison video: " + path);

    // Clear any previous trim points before loading new file
    ClearSecondaryTrimPoints();

    if (!comparison_video_->LoadFile(path)) {
        Debug::Log("ERROR: Failed to load comparison video: " + path);
        return;
    }

    Debug::Log("VideoPlayer: Comparison video loaded successfully");

    // Sync current settings to comparison video
    comparison_video_->SetLoop(loop_enabled);
    Debug::Log("VideoPlayer: Synced loop mode to comparison video: " + std::string(loop_enabled ? "enabled" : "disabled"));

    // Update dual view clip data and initialize timer if both clips loaded
    UpdateDualViewClipFromSecondary();
    if (dual_view_timeline_.HasAnyClip() && dual_view_timeline_.right.IsLoaded()) {
        InitializeDualViewPlayback();
    }
}

void VideoPlayer::LoadPrimaryVideoInDualView(const std::string& path) {
    if (!comparison_mode_enabled_) {
        Debug::Log("ERROR: Cannot load primary in dual view - comparison mode not enabled");
        // Fall back to regular load
        LoadFile(path);
        return;
    }

    Debug::Log("VideoPlayer: Loading primary video in dual view: " + path);

    // Stop current dual view playback
    if (dual_view_timer_) {
        dual_view_timer_.reset();
    }

    // Stop audio
    if (dual_view_audio_) {
        dual_view_audio_->Stop();
    }

    // Clear EXR mode flag BEFORE cache shutdown
    is_exr_mode = false;

    // Clear content dimensions (overlay mode) - new media will set its own if needed
    content_width_ = 0;
    content_height_ = 0;
    use_content_dimensions_ = false;

    // Switch video_texture to placeholder BEFORE destroying any cache textures
    video_texture = transition_placeholder_texture_;
    video_width = transition_placeholder_width_;
    video_height = transition_placeholder_height_;
    has_video = true;
    exr_texture = 0;

    // Clear color texture to background
    ClearColorTextureToBackground();

    // Clear caches
    if (cache_clear_callback) {
        Debug::Log("LoadPrimaryVideoInDualView: Clearing video cache");
        cache_clear_callback();
    }

    if (exr_cache_) {
        Debug::Log("LoadPrimaryVideoInDualView: Clearing EXR cache");
        exr_cache_->Shutdown();
        while (exr_cache_->HasPendingTextureDeletions()) {
            exr_cache_->ProcessReadyTextures();
        }
        exr_cache_.reset();
    }

    if (thumbnail_cache_) {
        Debug::Log("LoadPrimaryVideoInDualView: Clearing thumbnail cache");
        thumbnail_cache_->ClearCache();
        thumbnail_cache_.reset();
    }

    // Reset some state but preserve comparison mode
    current_file_path.clear();
    is_image_sequence = false;
    image_sequence_frame_rate = 24.0;

    ConfigureForSingleFile();

    if (has_video) {
        Stop();
    }

    // Store current file path
    current_file_path = path;

    Debug::Log("LoadPrimaryVideoInDualView: Sending loadfile command to MPV");
    const char* cmd[] = { "loadfile", path.c_str(), nullptr };
    if (mpv_command(mpv, cmd) < 0) {
        Debug::Log("LoadPrimaryVideoInDualView: ERROR - Failed to send loadfile command");
        return;
    }

    WaitForFileLoad(false);
    FinalizeLoad();

    // Load in paused state
    Pause();
    Debug::Log("LoadPrimaryVideoInDualView: Media loaded in paused state");
    Debug::Log("LoadPrimaryVideoInDualView: cached_duration=" + std::to_string(cached_duration) +
               ", cached_fps=" + std::to_string(cached_fps) +
               ", video_width=" + std::to_string(video_width) +
               ", video_height=" + std::to_string(video_height));

    // Update dual view clip data from newly loaded primary
    UpdateDualViewClipFromPrimary();

    Debug::Log("LoadPrimaryVideoInDualView: After UpdateDualViewClipFromPrimary - left.source_duration=" +
               std::to_string(dual_view_timeline_.left.source_duration) +
               ", left.duration=" + std::to_string(dual_view_timeline_.left.duration));

    // Re-initialize dual view playback if secondary is also loaded
    if (dual_view_timeline_.HasBothClips()) {
        InitializeDualViewPlayback();
    } else if (dual_view_timeline_.left.IsLoaded()) {
        // Only primary loaded - still initialize timer for single clip playback
        InitializeDualViewPlayback();
    }

    Debug::Log("VideoPlayer: Primary video loaded in dual view successfully");
}

void VideoPlayer::UnloadComparisonVideo() {
    // Clean up dual view timer
    if (dual_view_timer_) {
        dual_view_timer_.reset();
        Debug::Log("VideoPlayer: Dual view timer destroyed");
    }

    if (comparison_video_) {
        comparison_video_->Unload();
        Debug::Log("VideoPlayer: Comparison video unloaded");
    }

    // Clear dual view timeline data for right clip
    dual_view_timeline_.right = ump::DualViewClip();
}

bool VideoPlayer::HasComparisonVideo() const {
    return comparison_video_ && comparison_video_->HasVideo();
}

void VideoPlayer::SetComparisonMode(ComparisonMode mode) {
    if (!comparison_mode_enabled_) {
        Debug::Log("WARNING: Cannot set comparison mode - comparison not enabled");
        return;
    }

    ComparisonMode old_mode = comparison_mode_;
    comparison_mode_ = mode;

    // Debug: Log timer state and timeline values when switching modes
    Debug::Log("SetComparisonMode: Timer active=" + std::to_string(dual_view_timer_ != nullptr) +
               ", right.source_in=" + std::to_string(dual_view_timeline_.right.source_in) +
               ", right.source_out=" + std::to_string(dual_view_timeline_.right.source_out) +
               ", right.position_offset=" + std::to_string(dual_view_timeline_.right.position_offset));

    // Force re-sync when switching between non-lavfi modes to ensure trim is applied
    if (!IsLavfiMode(old_mode) && !IsLavfiMode(mode) && dual_view_timer_) {
        double current_pos = dual_view_timer_->GetPosition();
        Debug::Log("SetComparisonMode: Re-syncing videos to timeline position " + std::to_string(current_pos));
        SeekDualView(current_pos);
    }

    // Track non-lavfi edit modes for auto-revert
    if (mode == ComparisonMode::SIDE_BY_SIDE || mode == ComparisonMode::SPLIT_SCREEN) {
        last_edit_mode_ = mode;
        Debug::Log("VideoPlayer: Saved last edit mode: " +
                   std::string(mode == ComparisonMode::SIDE_BY_SIDE ? "Side-by-Side" : "Split Screen"));
    }

    const char* mode_name = "";
    switch (mode) {
        case ComparisonMode::SIDE_BY_SIDE: mode_name = "Side-by-Side"; break;
        case ComparisonMode::SPLIT_SCREEN: mode_name = "Split Screen"; break;
        case ComparisonMode::DIFFERENCE_VIEW: mode_name = "Difference"; break;
        case ComparisonMode::SPLIT_HORIZONTAL: mode_name = "Lavfi Horizontal"; break;
        case ComparisonMode::SPLIT_VERTICAL: mode_name = "Lavfi Vertical"; break;
        case ComparisonMode::SPLIT_5050_HORIZONTAL: mode_name = "Lavfi 50/50"; break;
        case ComparisonMode::DIFFERENCE_BLEND: mode_name = "Lavfi Difference"; break;
        default: mode_name = "Disabled"; break;
    }

    Debug::Log("VideoPlayer: Comparison mode changed to: " + std::string(mode_name));

    // Switching from difference back to side-by-side: restore original video
    if (old_mode == ComparisonMode::DIFFERENCE_VIEW && mode == ComparisonMode::SIDE_BY_SIDE) {
        if (!original_video_path_before_difference_.empty() && is_exr_mode) {
            Debug::Log("VideoPlayer: Restoring original video: " + original_video_path_before_difference_);
            LoadFile(original_video_path_before_difference_);
            // Don't clear the path - we can switch back to difference mode
        }
    }
}

ComparisonMode VideoPlayer::GetComparisonMode() const {
    return comparison_mode_;
}

std::string VideoPlayer::GetComparisonVideoPath() const {
    if (comparison_video_) {
        return comparison_video_->GetFilePath();
    }
    return "";
}

std::string VideoPlayer::GetPendingComparisonDrop() {
    std::string result = comparison_drop_pending_id_;
    comparison_drop_pending_id_.clear();
    return result;
}

void VideoPlayer::ClearPendingComparisonDrop() {
    comparison_drop_pending_id_.clear();
}

std::string VideoPlayer::GetPendingViewportDrop() {
    std::string result = viewport_drop_pending_id_;
    viewport_drop_pending_id_.clear();
    return result;
}

void VideoPlayer::ClearPendingViewportDrop() {
    viewport_drop_pending_id_.clear();
}

void VideoPlayer::RevertToEditMode() {
    if (!IsLavfiMode(comparison_mode_)) {
        Debug::Log("VideoPlayer::RevertToEditMode: Not in lavfi mode, nothing to do");
        return;
    }

    Debug::Log("VideoPlayer::RevertToEditMode: Reverting from lavfi to " +
               std::string(last_edit_mode_ == ComparisonMode::SIDE_BY_SIDE ? "Side-by-Side" : "Split Screen"));

    // Save the FULL dual view timeline state (paths, trims, offsets, everything)
    ump::DualViewTimeline saved_timeline = dual_view_timeline_;
    ComparisonMode target_mode = last_edit_mode_;

    Debug::Log("VideoPlayer::RevertToEditMode: Saved timeline state:");
    Debug::Log("  Left: " + saved_timeline.left.source_path +
               " in=" + std::to_string(saved_timeline.left.source_in) +
               " out=" + std::to_string(saved_timeline.left.source_out) +
               " offset=" + std::to_string(saved_timeline.left.position_offset));
    Debug::Log("  Right: " + saved_timeline.right.source_path +
               " in=" + std::to_string(saved_timeline.right.source_in) +
               " out=" + std::to_string(saved_timeline.right.source_out) +
               " offset=" + std::to_string(saved_timeline.right.position_offset));

    // Exit lavfi mode (this destroys MPV and comparison video)
    ExitLavfiMode();

    // Reload primary video
    if (!saved_timeline.left.source_path.empty()) {
        Debug::Log("VideoPlayer::RevertToEditMode: Reloading primary video");
        LoadFile(saved_timeline.left.source_path.c_str());
    }

    // Enable comparison mode and reload secondary video
    if (!saved_timeline.right.source_path.empty()) {
        Debug::Log("VideoPlayer::RevertToEditMode: Enabling comparison mode and loading secondary");
        EnableComparisonMode(true);
        LoadComparisonVideo(saved_timeline.right.source_path);
    }

    // Restore the full timeline state (trims, offsets, etc.)
    dual_view_timeline_ = saved_timeline;

    // Apply trim points to the video player's internal state
    if (saved_timeline.left.source_in > 0 || saved_timeline.left.source_out > 0) {
        SetPrimaryTrimPoints(saved_timeline.left.source_in,
                            saved_timeline.left.source_out - saved_timeline.left.source_in);
    }
    if (saved_timeline.right.source_in > 0 || saved_timeline.right.source_out > 0) {
        SetSecondaryTrimPoints(saved_timeline.right.source_in,
                              saved_timeline.right.source_out - saved_timeline.right.source_in);
    }

    // Set the target comparison mode
    SetComparisonMode(target_mode);

    Debug::Log("VideoPlayer::RevertToEditMode: Transition complete with restored timeline state");
}

void VideoPlayer::UpdateDualViewClipFromPrimary() {
    // Sync left clip from primary video metadata
    // LEFT CLIP IS LOCKED - always uses full source duration, no trim, no offset
    // This ensures the timeline duration equals the primary video's full duration,
    // allowing MPV to handle audio/seeking natively without virtual timeline extension.
    dual_view_timeline_.left.source_path = current_file_path;
    dual_view_timeline_.left.width = video_width;
    dual_view_timeline_.left.height = video_height;
    dual_view_timeline_.left.fps = cached_fps;
    dual_view_timeline_.left.source_duration = cached_duration;

    // LOCKED: Always use full source - no trim allowed on left clip
    dual_view_timeline_.left.source_in = 0.0;
    dual_view_timeline_.left.source_out = dual_view_timeline_.left.source_duration;
    dual_view_timeline_.left.duration = dual_view_timeline_.left.source_duration;
    dual_view_timeline_.left.position_offset = 0.0;  // LOCKED: No offset

    dual_view_timeline_.UpdateDuration();
}

void VideoPlayer::UpdateDualViewClipFromSecondary() {
    // Sync right clip from secondary video
    if (comparison_video_ && comparison_video_->HasVideo()) {
        // Check if this is a new file (path changed)
        std::string new_path = comparison_video_->GetFilePath();
        bool is_new_file = (dual_view_timeline_.right.source_path != new_path);

        if (is_new_file) {
            Debug::Log("UpdateDualViewClipFromSecondary: NEW FILE detected");
            Debug::Log("  Old path: " + dual_view_timeline_.right.source_path);
            Debug::Log("  New path: " + new_path);
        }

        dual_view_timeline_.right.source_path = new_path;
        dual_view_timeline_.right.width = comparison_video_->GetWidth();
        dual_view_timeline_.right.height = comparison_video_->GetHeight();

        // Use cached metadata from ComparisonVideoPlayer (probed on file load)
        dual_view_timeline_.right.fps = comparison_video_->GetFrameRate();
        double new_duration = comparison_video_->GetDuration();

        if (is_new_file) {
            Debug::Log("  New duration from comparison_video_: " + std::to_string(new_duration));
            Debug::Log("  Old source_duration: " + std::to_string(dual_view_timeline_.right.source_duration));
        }

        dual_view_timeline_.right.source_duration = new_duration;

        // Reset trim to full source when loading a new file, or if current trim is invalid
        if (is_new_file ||
            dual_view_timeline_.right.source_out <= 0 ||
            dual_view_timeline_.right.source_out > dual_view_timeline_.right.source_duration) {
            dual_view_timeline_.right.source_in = 0.0;
            dual_view_timeline_.right.source_out = dual_view_timeline_.right.source_duration;
            dual_view_timeline_.right.duration = dual_view_timeline_.right.source_duration;

            if (is_new_file) {
                Debug::Log("  Reset trim to full source: source_out=" + std::to_string(dual_view_timeline_.right.source_out) +
                           ", duration=" + std::to_string(dual_view_timeline_.right.duration));
            }
        }

        // Apply current trim points if they exist
        if (secondary_trim_start_ >= 0) {
            // Only log when trim override values actually differ from current (avoid spam)
            // Debug::Log("  Applying secondary trim override: start=" + std::to_string(secondary_trim_start_) +
            //            ", duration=" + std::to_string(secondary_trim_duration_));
            dual_view_timeline_.right.source_in = secondary_trim_start_;
            if (secondary_trim_duration_ > 0) {
                dual_view_timeline_.right.source_out = secondary_trim_start_ + secondary_trim_duration_;
            }
            dual_view_timeline_.right.duration = dual_view_timeline_.right.source_out - dual_view_timeline_.right.source_in;
        }

        // Log final values
        if (is_new_file) {
            Debug::Log("  FINAL right clip: source_duration=" + std::to_string(dual_view_timeline_.right.source_duration) +
                       ", source_in=" + std::to_string(dual_view_timeline_.right.source_in) +
                       ", source_out=" + std::to_string(dual_view_timeline_.right.source_out) +
                       ", duration=" + std::to_string(dual_view_timeline_.right.duration) +
                       ", GetEffectiveDuration()=" + std::to_string(dual_view_timeline_.right.GetEffectiveDuration()));
        }

        dual_view_timeline_.UpdateDuration();
    } else if (!lavfi_secondary_path_.empty()) {
        // In lavfi mode, use the stored secondary path and cached metadata
        dual_view_timeline_.right.source_path = lavfi_secondary_path_;
        dual_view_timeline_.right.width = secondary_video_width_;
        dual_view_timeline_.right.height = secondary_video_height_;
        // In lavfi mode, use stored secondary metadata if available, otherwise fall back to primary's
        dual_view_timeline_.right.fps = (lavfi_secondary_fps_ > 0) ? lavfi_secondary_fps_ : cached_fps;
        dual_view_timeline_.right.source_duration = (lavfi_secondary_duration_ > 0) ? lavfi_secondary_duration_ : cached_duration;

        // Initialize trim to full source if not already set
        if (dual_view_timeline_.right.source_out <= 0 ||
            dual_view_timeline_.right.source_out > dual_view_timeline_.right.source_duration) {
            dual_view_timeline_.right.source_in = 0.0;
            dual_view_timeline_.right.source_out = dual_view_timeline_.right.source_duration;
            dual_view_timeline_.right.duration = dual_view_timeline_.right.source_duration;
        }

        // Apply secondary trim if set
        if (secondary_trim_start_ >= 0) {
            dual_view_timeline_.right.source_in = secondary_trim_start_;
            if (secondary_trim_duration_ > 0) {
                dual_view_timeline_.right.source_out = secondary_trim_start_ + secondary_trim_duration_;
            }
            dual_view_timeline_.right.duration = dual_view_timeline_.right.source_out - dual_view_timeline_.right.source_in;
        }

        dual_view_timeline_.UpdateDuration();
        Debug::Log("VideoPlayer: Updated right clip from lavfi secondary - " + lavfi_secondary_path_);
    }
}

// ============================================================================
// Lavfi-based Comparison Mode (NEW)
// ============================================================================

bool VideoPlayer::IsLavfiMode(ComparisonMode mode) const {
    return mode == ComparisonMode::SPLIT_HORIZONTAL ||
           mode == ComparisonMode::SPLIT_VERTICAL ||
           mode == ComparisonMode::SPLIT_5050_HORIZONTAL ||
           mode == ComparisonMode::DIFFERENCE_BLEND;
}

void VideoPlayer::SetPrimaryTrimPoints(double start, double duration) {
    // LEFT/PRIMARY VIDEO IS LOCKED - trim is not allowed
    // This function is now a no-op. The left video always uses its full duration
    // to define the timeline, ensuring MPV can handle audio/seeking natively.
    Debug::Log("SetPrimaryTrimPoints IGNORED (left video is locked): start=" +
               std::to_string(start) + ", duration=" + std::to_string(duration));
    // Don't set primary_trim_start_ or primary_trim_duration_
}

void VideoPlayer::SetSecondaryTrimPoints(double start, double duration) {
    secondary_trim_start_ = start;
    secondary_trim_duration_ = duration;
    Debug::Log("Secondary trim set: start=" + std::to_string(start) + ", duration=" + std::to_string(duration));
}

double VideoPlayer::CalculateSecondaryPosition(double timeline_position) const {
    const auto& clip = dual_view_timeline_.right;

    // Use the new TimelineToSource method for proper gap handling
    double source_pos = clip.TimelineToSource(timeline_position);

    // Handle gaps - return held frame positions
    if (source_pos < 0) {
        // Gap before clip - hold at first frame (source_in or 0)
        return clip.source_in > 0 ? clip.source_in : 0.0;
    }
    if (source_pos > clip.source_duration) {
        // Gap after clip - hold at last frame (source_out or duration)
        return clip.source_out > 0 ? clip.source_out : clip.source_duration;
    }

    return source_pos;
}

double VideoPlayer::CalculatePrimaryPosition(double timeline_position) const {
    const auto& clip = dual_view_timeline_.left;

    // Use the new TimelineToSource method for proper gap handling
    double source_pos = clip.TimelineToSource(timeline_position);

    // Handle gaps - return held frame positions
    if (source_pos < 0) {
        // Gap before clip - hold at first frame (source_in or 0)
        return clip.source_in > 0 ? clip.source_in : 0.0;
    }
    if (source_pos > clip.source_duration) {
        // Gap after clip - hold at last frame (source_out or duration)
        return clip.source_out > 0 ? clip.source_out : clip.source_duration;
    }

    return source_pos;
}

ClipGapState VideoPlayer::GetPrimaryGapState(double timeline_position) const {
    const auto& clip = dual_view_timeline_.left;

    if (!clip.IsLoaded()) {
        return ClipGapState::GAP_BEFORE;  // No clip = all gap
    }

    if (timeline_position < clip.GetTimelineStart()) {
        return ClipGapState::GAP_BEFORE;
    }
    if (timeline_position >= clip.GetTimelineEnd()) {
        return ClipGapState::GAP_AFTER;
    }
    return ClipGapState::PLAYING;
}

ClipGapState VideoPlayer::GetSecondaryGapState(double timeline_position) const {
    const auto& clip = dual_view_timeline_.right;

    if (!clip.IsLoaded()) {
        return ClipGapState::GAP_BEFORE;  // No clip = all gap
    }

    if (timeline_position < clip.GetTimelineStart()) {
        return ClipGapState::GAP_BEFORE;
    }
    if (timeline_position >= clip.GetTimelineEnd()) {
        return ClipGapState::GAP_AFTER;
    }
    return ClipGapState::PLAYING;
}

double VideoPlayer::GetVirtualTimelineDuration() const {
    return dual_view_timeline_.GetVirtualDuration();
}

void VideoPlayer::SeekDualView(double timeline_position) {
    // Clamp to virtual timeline bounds
    double duration = GetVirtualTimelineDuration();
    if (duration <= 0.0) {
        return;  // No valid timeline
    }

    // Clamp position
    if (timeline_position < 0.0) timeline_position = 0.0;
    if (timeline_position > duration) timeline_position = duration;

    // Update timer position if active
    if (dual_view_timer_) {
        dual_view_timer_->Seek(timeline_position);
    }

    // Seek primary video to its calculated position (direct MPV command, not Seek())
    double primary_pos = CalculatePrimaryPosition(timeline_position);
    if (mpv) {
        std::string pos_str = std::to_string(primary_pos);
        const char* cmd[] = { "seek", pos_str.c_str(), "absolute", "exact", nullptr };
        mpv_command_async(mpv, 0, cmd);
    }

    // Sync secondary video if loaded
    if (comparison_video_ && comparison_video_->HasVideo()) {
        double secondary_pos = CalculateSecondaryPosition(timeline_position);
        comparison_video_->SyncToPosition(secondary_pos);
    }

    // Update the dual view timeline's internal duration tracker
    dual_view_timeline_.UpdateDuration();
}

double VideoPlayer::GetVirtualTimelinePosition() const {
    if (dual_view_timer_) {
        return dual_view_timer_->GetPosition();
    }
    // In lavfi mode, MPV's position IS the virtual timeline position
    // (because tpad filter already pads the start for offset)
    if (IsLavfiMode(comparison_mode_)) {
        return cached_position;
    }
    // Non-lavfi fallback: calculate from primary video position + offset
    return cached_position + dual_view_timeline_.left.position_offset;
}

bool VideoPlayer::InitializeDualViewPlayback() {
    Debug::Log("InitializeDualViewPlayback: Setting up PlaybackTimer for virtual timeline");

    // Ensure we have at least one clip loaded
    if (!dual_view_timeline_.HasAnyClip()) {
        Debug::Log("InitializeDualViewPlayback: No clips loaded");
        return false;
    }

    // Update timeline duration
    dual_view_timeline_.UpdateDuration();
    double virtual_duration = dual_view_timeline_.GetVirtualDuration();

    if (virtual_duration <= 0.0) {
        Debug::Log("InitializeDualViewPlayback: Invalid virtual duration");
        return false;
    }

    Debug::Log("InitializeDualViewPlayback: Virtual timeline duration = " + std::to_string(virtual_duration) + "s");

    // Get frame rate from clips
    double fps = 24.0;
    if (dual_view_timeline_.left.IsLoaded() && dual_view_timeline_.left.fps > 0) {
        fps = dual_view_timeline_.left.fps;
    } else if (dual_view_timeline_.right.IsLoaded() && dual_view_timeline_.right.fps > 0) {
        fps = dual_view_timeline_.right.fps;
    }

    // Create or reset the playback timer
    if (!dual_view_timer_) {
        dual_view_timer_ = std::make_unique<ump::PlaybackTimer>();
    }

    dual_view_timer_->SetDuration(virtual_duration);
    dual_view_timer_->SetFrameRate(fps);
    dual_view_timer_->SetLooping(loop_enabled);
    dual_view_timer_->Seek(0.0);

    // Pause both MPV instances - timer will drive position via seeks
    mpv_set_property_string(mpv, "pause", "yes");
    if (comparison_video_ && comparison_video_->HasVideo()) {
        comparison_video_->SyncPlaybackState(false);
    }

    // Set up callbacks
    dual_view_timer_->SetOnLoop([this]() {
        Debug::Log("DualView: Timeline looped");
        // Sync both videos to start position directly (not via SeekDualView to avoid recursion)
        double primary_pos = CalculatePrimaryPosition(0.0);
        double secondary_pos = CalculateSecondaryPosition(0.0);

        if (mpv) {
            std::string pos_str = std::to_string(primary_pos);
            const char* cmd[] = { "seek", pos_str.c_str(), "absolute", "exact", nullptr };
            mpv_command_async(mpv, 0, cmd);
        }
        if (comparison_video_ && comparison_video_->HasVideo()) {
            comparison_video_->SyncToPosition(secondary_pos);
        }
    });

    dual_view_timer_->SetOnEnd([this]() {
        Debug::Log("DualView: Timeline ended (non-looping)");
        is_playing = false;
    });

    dual_view_timer_->SetOnPositionChanged([this](double pos) {
        // Seek both videos to the new position (seek-based sync)
        double primary_pos = CalculatePrimaryPosition(pos);
        double secondary_pos = CalculateSecondaryPosition(pos);

        // Debug: Log every 30 callbacks (approx 1 per second at 30fps)
        static int timer_cb_counter = 0;
        if (timer_cb_counter++ % 30 == 0) {
            Debug::Log("TimerCallback: timeline=" + std::to_string(pos) +
                       ", primary=" + std::to_string(primary_pos) +
                       ", secondary=" + std::to_string(secondary_pos) +
                       ", mode=" + std::to_string(static_cast<int>(comparison_mode_)));
        }

        // Update cached position for UI
        cached_position = primary_pos;

        // Seek primary video
        if (mpv) {
            std::string pos_str = std::to_string(primary_pos);
            const char* cmd[] = { "seek", pos_str.c_str(), "absolute", "exact", nullptr };
            mpv_command_async(mpv, 0, cmd);
        }

        // Seek secondary video
        if (comparison_video_ && comparison_video_->HasVideo()) {
            comparison_video_->SyncToPosition(secondary_pos);
        }
    });

    // Initial sync to position 0
    double primary_pos = CalculatePrimaryPosition(0.0);
    double secondary_pos = CalculateSecondaryPosition(0.0);
    if (mpv) {
        std::string pos_str = std::to_string(primary_pos);
        const char* cmd[] = { "seek", pos_str.c_str(), "absolute", "exact", nullptr };
        mpv_command_async(mpv, 0, cmd);
    }
    if (comparison_video_ && comparison_video_->HasVideo()) {
        comparison_video_->SyncToPosition(secondary_pos);
    }

    is_playing = false;  // Start paused

    // Initialize audio player for left video
    if (!dual_view_audio_) {
        dual_view_audio_ = std::make_unique<ump::AudioPlayer>();
        if (!dual_view_audio_->Initialize()) {
            Debug::Log("InitializeDualViewPlayback: Failed to initialize audio player (continuing without audio)");
            dual_view_audio_.reset();
        }
    }

    // Load audio from left video if audio player initialized
    // Use the original source path (not lavfi path which is for video filters)
    std::string audio_source_path = dual_view_timeline_.left.source_path;

    if (dual_view_audio_ && dual_view_timeline_.left.IsLoaded() && !audio_source_path.empty()) {
        ump::AudioClipConfig audio_config;
        audio_config.source_in = dual_view_timeline_.left.source_in;
        audio_config.source_out = dual_view_timeline_.left.source_out;
        audio_config.position_offset = dual_view_timeline_.left.position_offset;
        audio_config.source_duration = dual_view_timeline_.left.source_duration;

        if (dual_view_audio_->LoadClip(audio_source_path, audio_config)) {
            dual_view_audio_->SetTimer(dual_view_timer_.get());
        }
    }

    Debug::Log("InitializeDualViewPlayback: Timer initialized with duration=" +
              std::to_string(virtual_duration) + "s, fps=" + std::to_string(fps));
    return true;
}

void VideoPlayer::UpdateDualViewTimer() {
    if (!dual_view_timer_) {
        return;
    }

    // Update the timer (advances position if playing)
    // The OnPositionChanged callback handles seeking both videos
    dual_view_timer_->Update();

    // Update audio sync
    if (dual_view_audio_) {
        dual_view_audio_->Update();
    }
}

void VideoPlayer::OnVirtualTimelineDurationChanged() {
    // Update timeline duration calculation
    dual_view_timeline_.UpdateDuration();
    double new_duration = dual_view_timeline_.GetVirtualDuration();

    if (new_duration <= 0.0) {
        return;  // No valid duration
    }

    // Update timer duration if timer is active
    if (dual_view_timer_) {
        double old_duration = dual_view_timer_->GetDuration();

        // Check if duration changed significantly (more than 0.1 seconds)
        if (std::abs(new_duration - old_duration) < 0.1) {
            return;  // No significant change
        }

        Debug::Log("OnVirtualTimelineDurationChanged: Duration changed from " +
                  std::to_string(old_duration) + "s to " + std::to_string(new_duration) + "s");

        // Store current position (clamped to new duration)
        double current_pos = dual_view_timer_->GetPosition();
        if (current_pos > new_duration) {
            current_pos = new_duration;
        }

        // Update timer with new duration
        dual_view_timer_->SetDuration(new_duration);

        // Seek to clamped position if it changed
        if (current_pos != dual_view_timer_->GetPosition()) {
            dual_view_timer_->Seek(current_pos);
        }

        Debug::Log("OnVirtualTimelineDurationChanged: Timer updated, position=" + std::to_string(current_pos) + "s");
    }
}

void VideoPlayer::ClearPrimaryTrimPoints() {
    primary_trim_start_ = -1.0;
    primary_trim_duration_ = -1.0;
    Debug::Log("Primary trim cleared");
}

void VideoPlayer::ClearSecondaryTrimPoints() {
    secondary_trim_start_ = -1.0;
    secondary_trim_duration_ = -1.0;
    Debug::Log("Secondary trim cleared");
}

void VideoPlayer::ExitLavfiMode() {
    Debug::Log("ExitLavfiMode: Cleaning up lavfi state and recreating MPV");

    //1. Clear lavfi state variables
    lavfi_primary_path_.clear();
    lavfi_secondary_path_.clear();
    current_lavfi_filter_.clear();

    // 2. Clear trim points
    ClearPrimaryTrimPoints();
    ClearSecondaryTrimPoints();

    // 3. Clear comparison mode state
    comparison_mode_ = ComparisonMode::DISABLED;
    comparison_mode_enabled_ = false;

    // 4. Unload legacy comparison player (if exists)
    if (comparison_video_) {
        comparison_video_->Cleanup();
        comparison_video_.reset();
    }

    // 4.5. Cleanup dual view audio player
    if (dual_view_audio_) {
        dual_view_audio_->Shutdown();
        dual_view_audio_.reset();
    }

    // 4.6. Cleanup dual view timer
    dual_view_timer_.reset();

    // 5. Destroy current MPV instance
    if (mpv_gl) {
        Debug::Log("ExitLavfiMode: Freeing MPV render context");
        mpv_render_context_free(mpv_gl);
        mpv_gl = nullptr;
    }

    if (mpv) {
        Debug::Log("ExitLavfiMode: Destroying MPV instance");
        mpv_terminate_destroy(mpv);
        mpv = nullptr;
    }

    // 6. Recreate clean MPV instance (without lavfi options)
    Debug::Log("ExitLavfiMode: Creating new MPV instance");
    mpv = mpv_create();
    if (!mpv) {
        Debug::Log("ERROR: Failed to create MPV instance");
        return;
    }

    // 7. Configure MPV with standard options (NO lavfi)
    ConfigureBasicOptions();
    ConfigureVideoOptions();
    ConfigureAudioOptions();
    ConfigureSeekingOptions();
    ConfigureCacheOptions();
    ConfigureHardwareDecoding();

    // 8. Initialize MPV
    Debug::Log("ExitLavfiMode: Initializing MPV");
    if (mpv_initialize(mpv) < 0) {
        Debug::Log("ERROR: Failed to initialize MPV");
        return;
    }

    // 9. Setup OpenGL
    if (!SetupOpenGL()) {
        Debug::Log("ERROR: Failed to setup OpenGL");
        return;
    }

    ApplyRenderOptimizations();
    mpv_request_event(mpv, MPV_EVENT_FILE_LOADED, 1);

    // 10. Setup property observation for transport controls
    SetupPropertyObservation();

    // 11. Reapply loop settings
    SetLoop(loop_enabled);

    Debug::Log("ExitLavfiMode: MPV recreated successfully without lavfi options");
}

void VideoPlayer::TransitionToLavfiMode(ComparisonMode lavfi_mode, int viewport_width, int viewport_height,
                                       const std::string& primary_override, const std::string& secondary_override,
                                       bool use_secondary_as_reference) {
    if (!IsLavfiMode(lavfi_mode)) {
        Debug::Log("ERROR: Target mode is not a lavfi mode");
        return;
    }

    // Get paths - use overrides if provided (for EDL unwrapping), otherwise use current paths
    std::string primary_path = primary_override.empty() ? GetFilePath() : primary_override;
    std::string secondary_path = secondary_override;

    if (secondary_path.empty()) {
        if (comparison_video_ && comparison_video_->HasVideo()) {
            secondary_path = comparison_video_->GetFilePath();
        } else {
            Debug::Log("ERROR: No comparison video loaded for transition");
            return;
        }
    }

    if (primary_path.empty()) {
        Debug::Log("ERROR: No primary video loaded for transition");
        return;
    }

    // If using secondary as reference, swap paths so secondary becomes "primary" in filter
    if (use_secondary_as_reference) {
        Debug::Log("Using secondary as reference - swapping primary and secondary");
        std::swap(primary_path, secondary_path);
    }

    // Extract original paths and trim info from EDL if needed
    std::string primary_original = primary_path;
    std::string secondary_original = secondary_path;
    double primary_trim_start = -1.0;
    double primary_trim_duration = -1.0;
    double secondary_trim_start = -1.0;
    double secondary_trim_duration = -1.0;

    // Parse EDL for primary: edl://path,start=X,length=Y
    if (primary_path.find("edl://") == 0) {
        Debug::Log("Primary is EDL, extracting original path and trim info");
        size_t comma_pos = primary_path.find(',');
        if (comma_pos != std::string::npos) {
            primary_original = primary_path.substr(6, comma_pos - 6);  // Skip "edl://"

            // Extract start and length from EDL
            size_t start_pos = primary_path.find("start=");
            size_t length_pos = primary_path.find("length=");
            if (start_pos != std::string::npos && length_pos != std::string::npos) {
                std::string start_str = primary_path.substr(start_pos + 6, length_pos - start_pos - 7);
                std::string length_str = primary_path.substr(length_pos + 7);
                primary_trim_start = std::stod(start_str);
                primary_trim_duration = std::stod(length_str);
                Debug::Log("  Extracted primary trim: start=" + std::to_string(primary_trim_start) +
                          ", duration=" + std::to_string(primary_trim_duration));
            }
            Debug::Log("  Primary original path: " + primary_original);
        }
    }

    // Parse EDL for secondary: edl://path,start=X,length=Y
    if (secondary_path.find("edl://") == 0) {
        Debug::Log("Secondary is EDL, extracting original path and trim info");
        size_t comma_pos = secondary_path.find(',');
        if (comma_pos != std::string::npos) {
            secondary_original = secondary_path.substr(6, comma_pos - 6);  // Skip "edl://"

            // Extract start and length from EDL
            size_t start_pos = secondary_path.find("start=");
            size_t length_pos = secondary_path.find("length=");
            if (start_pos != std::string::npos && length_pos != std::string::npos) {
                std::string start_str = secondary_path.substr(start_pos + 6, length_pos - start_pos - 7);
                std::string length_str = secondary_path.substr(length_pos + 7);
                secondary_trim_start = std::stod(start_str);
                secondary_trim_duration = std::stod(length_str);
                Debug::Log("  Extracted secondary trim: start=" + std::to_string(secondary_trim_start) +
                          ", duration=" + std::to_string(secondary_trim_duration));
            }
            Debug::Log("  Secondary original path: " + secondary_original);
        }
    }

    // Apply trim points (either from EDL or existing trim settings)
    // If using secondary as reference, trim values are already swapped with paths
    if (primary_trim_start >= 0.0) {
        SetPrimaryTrimPoints(primary_trim_start, primary_trim_duration);
        Debug::Log("Applied primary EDL trim to lavfi");
    }
    // Note: If no EDL trim and no existing trim, current trim settings are preserved

    if (secondary_trim_start >= 0.0) {
        SetSecondaryTrimPoints(secondary_trim_start, secondary_trim_duration);
        Debug::Log("Applied secondary EDL trim to lavfi");
    }
    // Note: If no EDL trim and no existing trim, current trim settings are preserved

    // If using secondary as reference, also swap the stored trim points
    if (use_secondary_as_reference && (primary_trim_start >= 0.0 || secondary_trim_start >= 0.0)) {
        Debug::Log("Swapping trim points to match swapped videos");
        std::swap(primary_trim_start_, secondary_trim_start_);
        std::swap(primary_trim_duration_, secondary_trim_duration_);
    }

    Debug::Log("Transitioning from setup mode to lavfi mode");
    Debug::Log("  Primary: " + primary_original);
    Debug::Log("  Secondary: " + secondary_original);
    if (primary_path != primary_original) {
        Debug::Log("  (Primary EDL unwrapped)");
    }
    if (secondary_path != secondary_original) {
        Debug::Log("  (Secondary EDL unwrapped)");
    }

    // Disable legacy comparison mode (will unload the ComparisonVideoPlayer)
    if (comparison_video_) {
        comparison_video_->Unload();
    }
    comparison_mode_enabled_ = false;

    // CRITICAL: Reset dual view timer and audio - lavfi mode uses native MPV playback, not timer-driven seeks
    if (dual_view_timer_) {
        Debug::Log("Resetting dual_view_timer_ for lavfi mode");
        dual_view_timer_.reset();
    }
    if (dual_view_audio_) {
        dual_view_audio_->Shutdown();
        dual_view_audio_.reset();
    }

    // Load with lavfi using original paths
    LoadLavfiComparison(primary_original, secondary_original, lavfi_mode, viewport_width, viewport_height);

    Debug::Log("Transition to lavfi mode complete");
}

void VideoPlayer::LoadLavfiComparison(const std::string& primary_path, const std::string& secondary_path,
                                     ComparisonMode mode, int viewport_width, int viewport_height) {
    if (!IsLavfiMode(mode)) {
        Debug::Log("ERROR: Mode is not a lavfi mode");
        return;
    }

    Debug::Log("LoadLavfiComparison: primary=" + primary_path + ", secondary=" + secondary_path);
    Debug::Log("Mode: " + std::to_string(static_cast<int>(mode)) + ", Viewport: " +
               std::to_string(viewport_width) + "x" + std::to_string(viewport_height));

    // Store paths for state
    lavfi_primary_path_ = primary_path;
    lavfi_secondary_path_ = secondary_path;

    // Extract video metadata for dimensions and audio info
    VideoMetadata primary_metadata = ump::FFmpegMetadataExtractor::Extract(primary_path);
    if (primary_metadata.width > 0 && primary_metadata.height > 0) {
        primary_video_width_ = primary_metadata.width;
        primary_video_height_ = primary_metadata.height;
        Debug::Log("Primary video dimensions: " + std::to_string(primary_video_width_) + "x" + std::to_string(primary_video_height_));
    } else {
        Debug::Log("WARNING: Could not extract primary video dimensions, using defaults");
        primary_video_width_ = 1920;
        primary_video_height_ = 1080;
    }

    // Check if primary has audio (needed for lavfi filter generation)
    primary_has_audio_ = (primary_metadata.audio_channels > 0 && !primary_metadata.audio_codec.empty());
    Debug::Log("Primary video has audio: " + std::string(primary_has_audio_ ? "yes" : "no"));

    VideoMetadata secondary_metadata = ump::FFmpegMetadataExtractor::Extract(secondary_path);
    if (secondary_metadata.width > 0 && secondary_metadata.height > 0) {
        secondary_video_width_ = secondary_metadata.width;
        secondary_video_height_ = secondary_metadata.height;
        Debug::Log("Secondary video dimensions: " + std::to_string(secondary_video_width_) + "x" + std::to_string(secondary_video_height_));
    } else {
        Debug::Log("WARNING: Could not extract secondary video dimensions, using defaults");
        secondary_video_width_ = 1920;
        secondary_video_height_ = 1080;
    }

    // Cache secondary FPS and duration for timeline calculations
    lavfi_secondary_fps_ = secondary_metadata.frame_rate;
    if (lavfi_secondary_fps_ <= 0) {
        lavfi_secondary_fps_ = 24.0;  // Default fallback
    }
    // Calculate duration from frame count and fps
    if (secondary_metadata.total_frames > 0 && lavfi_secondary_fps_ > 0) {
        lavfi_secondary_duration_ = static_cast<double>(secondary_metadata.total_frames) / lavfi_secondary_fps_;
    } else {
        lavfi_secondary_duration_ = 0.0;  // Unknown, will fall back to primary's duration
    }
    Debug::Log("Secondary video metadata: fps=" + std::to_string(lavfi_secondary_fps_) +
               ", duration=" + std::to_string(lavfi_secondary_duration_) + "s");

    // Generate lavfi filter
    UpdateLavfiFilter(mode, viewport_width, viewport_height);
    Debug::Log("Generated lavfi filter: " + current_lavfi_filter_);

    // === CRITICAL: RECREATE MPV WITH LAVFI OPTIONS ===
    // external-file and lavfi-complex are initialization-only options
    // They MUST be set BEFORE mpv_initialize()

    // 1. Destroy current MPV instance
    if (mpv_gl) {
        Debug::Log("LoadLavfiComparison: Freeing MPV render context");
        mpv_render_context_free(mpv_gl);
        mpv_gl = nullptr;
    }

    if (mpv) {
        Debug::Log("LoadLavfiComparison: Destroying MPV instance");
        mpv_terminate_destroy(mpv);
        mpv = nullptr;
    }

    // 2. Create new MPV instance
    Debug::Log("LoadLavfiComparison: Creating new MPV instance");
    mpv = mpv_create();
    if (!mpv) {
        Debug::Log("ERROR: Failed to create MPV instance");
        return;
    }

    // 3. Configure standard MPV options (with lavfi optimizations)
    ConfigureBasicOptions();
    ConfigureVideoOptions(true);  // true = lavfi mode (disables interpolation, adds vd-queue)
    ConfigureAudioOptions();
    ConfigureSeekingOptions();
    ConfigureCacheOptions(true);  // true = lavfi mode (aggressive buffering)
    ConfigureHardwareDecoding(true);  // true = lavfi mode (hwdec-copy, thread limit)

    // 4. SET LAVFI OPTIONS BEFORE INITIALIZE (This is the key!)
    // Primary is loaded as main file, secondary as external file
    // vid1 = primary (main file), vid2 = secondary (external file)
    Debug::Log("LoadLavfiComparison: Setting external-file: " + secondary_path);
    int ext_result = mpv_set_option_string(mpv, "external-file", secondary_path.c_str());
    if (ext_result < 0) {
        Debug::Log("WARNING: Failed to set external-file: " + std::string(mpv_error_string(ext_result)));
        // Try plural version (different MPV versions)
        ext_result = mpv_set_option_string(mpv, "external-files", secondary_path.c_str());
        if (ext_result < 0) {
            Debug::Log("ERROR: Failed to set external-files too: " + std::string(mpv_error_string(ext_result)));
        }
    }

    Debug::Log("LoadLavfiComparison: Setting lavfi-complex option (BEFORE init)");
    int lavfi_result = mpv_set_option_string(mpv, "lavfi-complex", current_lavfi_filter_.c_str());
    if (lavfi_result < 0) {
        Debug::Log("ERROR: Failed to set lavfi-complex: " + std::string(mpv_error_string(lavfi_result)));
        return;
    }

    // 5. Initialize MPV (lavfi options are now locked in)
    Debug::Log("LoadLavfiComparison: Initializing MPV with lavfi options");
    if (mpv_initialize(mpv) < 0) {
        Debug::Log("ERROR: Failed to initialize MPV");
        return;
    }

    // 6. Setup OpenGL
    if (!SetupOpenGL()) {
        Debug::Log("ERROR: Failed to setup OpenGL");
        return;
    }

    ApplyRenderOptimizations();
    mpv_request_event(mpv, MPV_EVENT_FILE_LOADED, 1);

    // Setup property observation for transport controls
    SetupPropertyObservation();

    // Reapply loop settings
    SetLoop(loop_enabled);

    // 7. Configure for single file mode (lavfi creates single composite output)
    ConfigureForSingleFile();

    // 8. Load PRIMARY file as main file
    // The lavfi filter uses vid1 (primary) and vid2 (secondary/external)
    Debug::Log("LoadLavfiComparison: Loading primary file");
    const char* cmd[] = {"loadfile", primary_path.c_str(), "replace", nullptr};
    int result = mpv_command(mpv, cmd);

    if (result < 0) {
        Debug::Log("ERROR: Failed to load lavfi comparison: " + std::string(mpv_error_string(result)));
        return;
    }

    // 9. Update mode state
    comparison_mode_ = mode;
    comparison_mode_enabled_ = true;

    // 10. Wait for file to load before seeking
    Debug::Log("LoadLavfiComparison: Waiting for file load...");
    WaitForFileLoad();

    // 11. Seek to start of timeline
    Debug::Log("LoadLavfiComparison: Seeking to start of timeline");
    Seek(0.0);

    Debug::Log("LoadLavfiComparison: Success! Both videos should now be composited");
}

void VideoPlayer::UpdateLavfiFilter(ComparisonMode mode, int viewport_width, int viewport_height) {
    if (!IsLavfiMode(mode)) {
        Debug::Log("ERROR: Cannot update lavfi filter for non-lavfi mode");
        return;
    }

    // Create filter configuration
    ump::LavfiFilterGenerator::FilterConfig config;
    config.mode = mode;
    config.viewport_width = viewport_width;
    config.viewport_height = viewport_height;
    config.split_position = split_screen_position_;  // Use current split position
    config.maintain_aspect = true;

    // Calculate total virtual timeline duration (both clips must be padded to this length)
    dual_view_timeline_.UpdateDuration();
    config.timeline_duration = dual_view_timeline_.GetVirtualDuration();
    Debug::Log("UpdateLavfiFilter: Virtual timeline duration = " + std::to_string(config.timeline_duration));

    // Create primary input configuration
    // LEFT/PRIMARY VIDEO IS LOCKED - no trim, no offset
    // It defines the timeline duration and plays with native MPV audio
    const auto& left_clip = dual_view_timeline_.left;
    ump::LavfiFilterGenerator::VideoInput primary_input;
    // vid1 = primary (main file loaded with loadfile)
    primary_input.stream_id = "vid1";
    primary_input.trim_start = -1.0;  // LOCKED: No trim (full video)
    primary_input.trim_duration = left_clip.source_duration;  // Full source duration
    primary_input.position_offset = 0.0;  // LOCKED: No offset
    primary_input.source_width = primary_video_width_;
    primary_input.source_height = primary_video_height_;
    primary_input.has_audio = primary_has_audio_;

    // Create secondary input configuration from dual_view_timeline_ for consistency
    const auto& right_clip = dual_view_timeline_.right;
    ump::LavfiFilterGenerator::VideoInput secondary_input;
    // vid2 = secondary (external file)
    secondary_input.stream_id = "vid2";
    secondary_input.trim_start = right_clip.source_in;  // Use source_in from timeline
    secondary_input.trim_duration = right_clip.GetEffectiveDuration();  // source_out - source_in
    secondary_input.position_offset = right_clip.position_offset;
    secondary_input.source_width = secondary_video_width_;
    secondary_input.source_height = secondary_video_height_;
    secondary_input.has_audio = false;  // Secondary video audio is never used in lavfi mode

    // Debug: Log the input values
    Debug::Log("UpdateLavfiFilter inputs:");
    Debug::Log("  Primary (LOCKED): full duration=" + std::to_string(primary_input.trim_duration) + "s");
    Debug::Log("  Secondary: source_in=" + std::to_string(right_clip.source_in) +
               ", source_out=" + std::to_string(right_clip.source_out) +
               ", duration=" + std::to_string(secondary_input.trim_duration) +
               ", offset=" + std::to_string(secondary_input.position_offset));

    // Generate filter string
    current_lavfi_filter_ = ump::LavfiFilterGenerator::GenerateLavfiFilter(
        primary_input, secondary_input, config
    );

    Debug::Log("Generated lavfi filter: " + current_lavfi_filter_);

    // If already in lavfi mode, recreate MPV with new filter
    if (mpv && !lavfi_primary_path_.empty() && !lavfi_secondary_path_.empty()) {
        Debug::Log("Recreating MPV with updated lavfi filter");

        // Store current state
        double current_position = GetPosition();
        bool was_playing = IsPlaying();

        // === RECREATE MPV WITH NEW FILTER ===
        // 1. Destroy current MPV instance
        if (mpv_gl) {
            mpv_render_context_free(mpv_gl);
            mpv_gl = nullptr;
        }
        if (mpv) {
            mpv_terminate_destroy(mpv);
            mpv = nullptr;
        }

        // 2. Create new MPV instance
        mpv = mpv_create();
        if (!mpv) {
            Debug::Log("ERROR: Failed to create MPV instance");
            return;
        }

        // 3. Configure standard MPV options (with lavfi optimizations)
        ConfigureBasicOptions();
        ConfigureVideoOptions(true);  // true = lavfi mode (disables interpolation, adds vd-queue)
        ConfigureAudioOptions();
        ConfigureSeekingOptions();
        ConfigureCacheOptions(true);  // true = lavfi mode (aggressive buffering)
        ConfigureHardwareDecoding(true);  // true = lavfi mode (hwdec-copy, thread limit)

        // 4. SET LAVFI OPTIONS BEFORE INITIALIZE (initialization-only options!)
        // Primary is loaded as main file, secondary as external file
        // vid1 = primary (main file), vid2 = secondary (external file)
        int ext_result = mpv_set_option_string(mpv, "external-file", lavfi_secondary_path_.c_str());
        if (ext_result < 0) {
            Debug::Log("Trying plural 'external-files' option");
            ext_result = mpv_set_option_string(mpv, "external-files", lavfi_secondary_path_.c_str());
        }

        int lavfi_result = mpv_set_option_string(mpv, "lavfi-complex", current_lavfi_filter_.c_str());

        Debug::Log("external-file result: " + std::to_string(ext_result));
        Debug::Log("lavfi-complex result: " + std::to_string(lavfi_result));

        // 5. Initialize MPV (lavfi options are now locked in)
        if (mpv_initialize(mpv) < 0) {
            Debug::Log("ERROR: Failed to initialize MPV");
            return;
        }

        // 6. Setup OpenGL
        SetupOpenGL();
        ApplyRenderOptimizations();
        mpv_request_event(mpv, MPV_EVENT_FILE_LOADED, 1);

        // Setup property observation for transport controls
        SetupPropertyObservation();

        // Reapply loop settings
        SetLoop(loop_enabled);

        // Configure for single file mode (lavfi creates single composite output)
        ConfigureForSingleFile();

        // 7. Load PRIMARY file as main file
        const char* cmd[] = {"loadfile", lavfi_primary_path_.c_str(), "replace", nullptr};
        int result = mpv_command(mpv, cmd);

        if (result < 0) {
            Debug::Log("ERROR: Failed to load file with updated filter: " + std::string(mpv_error_string(result)));
            return;
        }

        Debug::Log("MPV loadfile command succeeded");

        // 8. Restore state
        if (current_position > 0.0) {
            Seek(current_position);
        }
        if (was_playing) {
            Play();
        }

        Debug::Log("Filter updated and MPV recreated successfully");
    }
}

// ============================================================================
// Comparison Rendering Helper Methods
// ============================================================================

GLuint VideoPlayer::GetDisplayTexture() const {
    // Return the final composited texture (with OCIO/overlays applied)
    GLuint display_texture = video_texture;

    // Apply color correction if active
    if (color_pipeline && color_pipeline->IsValid() && color_texture > 0 && glIsTexture(color_texture)) {
        display_texture = color_texture;
    }

    // Could add safety overlays here if implemented
    // if (safety_overlay_system && safety_overlay_system->IsEnabled() && safety_overlay_system->GetOutputTexture()) {
    //     display_texture = safety_overlay_system->GetOutputTexture();
    // }

    return display_texture;
}

ImVec2 VideoPlayer::CalculateFitSize(int source_w, int source_h, float max_w, float max_h) const {
    if (source_w == 0 || source_h == 0) {
        return ImVec2(max_w, max_h);
    }

    float aspect_ratio = (float)source_w / (float)source_h;
    ImVec2 result;

    if (max_w / max_h > aspect_ratio) {
        // Limited by height
        result.y = max_h;
        result.x = max_h * aspect_ratio;
    } else {
        // Limited by width
        result.x = max_w;
        result.y = max_w / aspect_ratio;
    }

    return result;
}

void VideoPlayer::RenderSideBySide() {
    // Debug: Log every 60 frames
    static int sbs_debug_counter = 0;
    if (sbs_debug_counter++ % 60 == 0) {
        Debug::Log("RenderSideBySide: Timer active=" + std::to_string(dual_view_timer_ != nullptr) +
                   ", right.source_in=" + std::to_string(dual_view_timeline_.right.source_in) +
                   ", right.position_offset=" + std::to_string(dual_view_timeline_.right.position_offset));
    }

    ImVec2 content_region = ImGui::GetContentRegionAvail();
    float half_width = content_region.x * 0.5f;

    // Get draw list and viewport position for overlays
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImVec2 viewport_pos = ImGui::GetCursorScreenPos();
    ImVec2 cursor_pos = ImGui::GetCursorPos();

    // Left side: Primary video from main MPV context
    GLuint left_texture = GetDisplayTexture();
    int left_width = video_width;
    int left_height = video_height;

    if (left_texture > 0 && glIsTexture(left_texture)) {
        // Render left video
        ImVec2 left_size = CalculateFitSize(left_width, left_height, half_width, content_region.y);

        // Center both vertically and horizontally
        float left_offset_x = (half_width - left_size.x) * 0.5f;
        float left_offset_y = (content_region.y - left_size.y) * 0.5f;
        ImGui::SetCursorPos(ImVec2(cursor_pos.x + left_offset_x, cursor_pos.y + left_offset_y));

        ImGui::Image((void*)(intptr_t)left_texture, left_size);
    } else {
        // Show drop target placeholder on left side
        ImGui::SetCursorPos(cursor_pos);
        RenderPrimaryDropTargetPlaceholder(half_width, content_region.y);
    }

    // Right side: Secondary video or drop target
    ImGui::SameLine(0.0f, 0.0f); // No spacing, we'll position manually
    ImVec2 right_start_pos = ImGui::GetCursorScreenPos();

    if (comparison_video_ && comparison_video_->HasVideo()) {
        // CRITICAL: Update the comparison video texture BEFORE rendering
        //Debug::Log("RenderSideBySide: About to call UpdateVideoTexture()");
        comparison_video_->UpdateVideoTexture();

        GLuint comp_texture = comparison_video_->GetTexture();
        int comp_w = comparison_video_->GetWidth();
        int comp_h = comparison_video_->GetHeight();
        ImVec2 right_size = CalculateFitSize(comp_w, comp_h, half_width, content_region.y);

        // Debug logging (only log every 60 frames to avoid spam)
        static int debug_counter = 0;
        if (debug_counter++ % 60 == 0) {
            //Debug::Log("RenderSideBySide: comp_texture=" + std::to_string(comp_texture) +
            //           ", dimensions=" + std::to_string(comp_w) + "x" + std::to_string(comp_h) +
            //           ", right_size=" + std::to_string(right_size.x) + "x" + std::to_string(right_size.y) +
            //           ", glIsTexture=" + std::to_string(glIsTexture(comp_texture)));
        }

        // Center both vertically and horizontally
        float right_offset_x = half_width + (half_width - right_size.x) * 0.5f;
        float right_offset_y = (content_region.y - right_size.y) * 0.5f;
        ImGui::SetCursorPos(ImVec2(cursor_pos.x + right_offset_x, cursor_pos.y + right_offset_y));

        if (comp_texture > 0 && glIsTexture(comp_texture)) {
            ImGui::Image((void*)(intptr_t)comp_texture, right_size);
        } else {
            Debug::Log("ERROR: Comparison texture is invalid! texture_id=" + std::to_string(comp_texture));
        }
    } else {
        // Show drop target placeholder (centered in right half)
        float right_offset_x = half_width + (half_width - half_width) * 0.5f; // Center placeholder in right half
        ImGui::SetCursorPos(ImVec2(cursor_pos.x + half_width, cursor_pos.y));
        RenderDropTargetPlaceholder(half_width, content_region.y);

        // Add invisible drop target over entire right half (only when no video loaded)
        ImGui::SetCursorPos(ImVec2(cursor_pos.x + half_width, cursor_pos.y));
        ImGui::InvisibleButton("##ComparisonDropTarget", ImVec2(half_width, content_region.y));

        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("MEDIA_ITEM")) {
                std::string media_id((const char*)payload->Data, payload->DataSize - 1);
                Debug::Log("Comparison video drop received: " + media_id);
                comparison_drop_pending_id_ = media_id;
            }
            ImGui::EndDragDropTarget();
        }
    }

    // Add drop targets over both sides (allows changing videos after loading)
    // Left side drop target (primary video)
    ImGui::SetCursorPos(ImVec2(0, 0));
    ImGui::InvisibleButton("##PrimaryVideoDropTarget", ImVec2(half_width, content_region.y));
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("MEDIA_ITEM")) {
            std::string media_id((const char*)payload->Data, payload->DataSize - 1);
            Debug::Log("Primary video drop received: " + media_id);
            viewport_drop_pending_id_ = media_id;
        }
        ImGui::EndDragDropTarget();
    }

    // Right side drop target (comparison video)
    ImGui::SetCursorPos(ImVec2(half_width, 0));
    ImGui::InvisibleButton("##ComparisonVideoDropTarget", ImVec2(half_width, content_region.y));
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("MEDIA_ITEM")) {
            std::string media_id((const char*)payload->Data, payload->DataSize - 1);
            Debug::Log("Comparison video drop received: " + media_id);
            comparison_drop_pending_id_ = media_id;
        }
        ImGui::EndDragDropTarget();
    }

    // Draw gap overlays (black) when playhead is outside a clip's range on virtual timeline
    // This provides visual feedback that the clip has a gap at this position
    double timeline_pos = GetVirtualTimelinePosition();

    // Left video gap overlay
    ClipGapState left_gap = GetPrimaryGapState(timeline_pos);
    if (left_gap != ClipGapState::PLAYING && left_texture > 0) {
        // Calculate the video display area (same as above)
        ImVec2 left_size = CalculateFitSize(left_width, left_height, half_width, content_region.y);
        float left_offset_x = (half_width - left_size.x) * 0.5f;
        float left_offset_y = (content_region.y - left_size.y) * 0.5f;

        ImVec2 gap_min(viewport_pos.x + left_offset_x, viewport_pos.y + left_offset_y);
        ImVec2 gap_max(gap_min.x + left_size.x, gap_min.y + left_size.y);
        draw_list->AddRectFilled(gap_min, gap_max, IM_COL32(0, 0, 0, 255));

        // Optional: Add "GAP" text indicator
        const char* gap_text = (left_gap == ClipGapState::GAP_BEFORE) ? "BEFORE CLIP" : "AFTER CLIP";
        if (font_mono) {
            ImVec2 text_size = font_mono->CalcTextSizeA(12.0f, FLT_MAX, 0.0f, gap_text);
            ImVec2 text_pos((gap_min.x + gap_max.x - text_size.x) * 0.5f,
                           (gap_min.y + gap_max.y - text_size.y) * 0.5f);
            draw_list->AddText(font_mono, 12.0f, text_pos, IM_COL32(100, 100, 100, 255), gap_text);
        }
    }

    // Right video gap overlay
    ClipGapState right_gap = GetSecondaryGapState(timeline_pos);
    if (right_gap != ClipGapState::PLAYING && comparison_video_ && comparison_video_->HasVideo()) {
        int comp_w = comparison_video_->GetWidth();
        int comp_h = comparison_video_->GetHeight();
        ImVec2 right_size = CalculateFitSize(comp_w, comp_h, half_width, content_region.y);
        float right_offset_x = half_width + (half_width - right_size.x) * 0.5f;
        float right_offset_y = (content_region.y - right_size.y) * 0.5f;

        ImVec2 gap_min(viewport_pos.x + right_offset_x, viewport_pos.y + right_offset_y);
        ImVec2 gap_max(gap_min.x + right_size.x, gap_min.y + right_size.y);
        draw_list->AddRectFilled(gap_min, gap_max, IM_COL32(0, 0, 0, 255));

        // Optional: Add "GAP" text indicator
        const char* gap_text = (right_gap == ClipGapState::GAP_BEFORE) ? "BEFORE CLIP" : "AFTER CLIP";
        if (font_mono) {
            ImVec2 text_size = font_mono->CalcTextSizeA(12.0f, FLT_MAX, 0.0f, gap_text);
            ImVec2 text_pos((gap_min.x + gap_max.x - text_size.x) * 0.5f,
                           (gap_min.y + gap_max.y - text_size.y) * 0.5f);
            draw_list->AddText(font_mono, 12.0f, text_pos, IM_COL32(100, 100, 100, 255), gap_text);
        }
    }

    // Draw vertical divider line between left and right sides
    ImVec2 divider_top(viewport_pos.x + half_width, viewport_pos.y);
    ImVec2 divider_bottom(viewport_pos.x + half_width, viewport_pos.y + content_region.y);
    draw_list->AddLine(divider_top, divider_bottom, IM_COL32(60, 60, 60, 180), 1.0f);

    // Draw text overlays using monospace font
    if (font_mono) {
        float font_size = 14.0f;
        float small_font_size = 11.0f;

        // Left side - Main Control + media name
        {
            const char* left_label = "Main Control";
            ImVec2 left_text_size = font_mono->CalcTextSizeA(font_size, FLT_MAX, 0.0f, left_label);
            ImVec2 left_text_pos(viewport_pos.x + (half_width - left_text_size.x) * 0.5f, viewport_pos.y + 10.0f);

            // Get media filename
            std::string left_media_name;
            if (!current_file_path.empty()) {
                std::filesystem::path p(current_file_path);
                left_media_name = p.filename().string();
            }
            ImVec2 left_media_size = font_mono->CalcTextSizeA(small_font_size, FLT_MAX, 0.0f, left_media_name.c_str());

            // Background for both labels
            float combined_height = left_text_size.y + left_media_size.y + 4.0f;
            float max_width = (std::max)(left_text_size.x, left_media_size.x);
            draw_list->AddRectFilled(
                ImVec2(viewport_pos.x + (half_width - max_width) * 0.5f - 5, left_text_pos.y - 2),
                ImVec2(viewport_pos.x + (half_width + max_width) * 0.5f + 5, left_text_pos.y + combined_height + 2),
                IM_COL32(20, 20, 20, 180)
            );

            // Draw labels
            draw_list->AddText(font_mono, font_size, left_text_pos, IM_COL32(200, 200, 200, 255), left_label);
            ImVec2 left_media_pos(viewport_pos.x + (half_width - left_media_size.x) * 0.5f, left_text_pos.y + left_text_size.y + 2.0f);
            draw_list->AddText(font_mono, small_font_size, left_media_pos, IM_COL32(150, 150, 150, 255), left_media_name.c_str());
        }

        // Right side - Secondary Video + media name
        {
            const char* right_label = "Secondary Video";
            ImVec2 right_text_size = font_mono->CalcTextSizeA(font_size, FLT_MAX, 0.0f, right_label);
            ImVec2 right_text_pos(viewport_pos.x + half_width + (half_width - right_text_size.x) * 0.5f, viewport_pos.y + 10.0f);

            // Get media filename
            std::string right_media_name;
            if (comparison_video_ && comparison_video_->HasVideo()) {
                std::filesystem::path p(comparison_video_->GetFilePath());
                right_media_name = p.filename().string();
            }
            ImVec2 right_media_size = font_mono->CalcTextSizeA(small_font_size, FLT_MAX, 0.0f, right_media_name.c_str());

            // Background for both labels
            float combined_height = right_text_size.y + right_media_size.y + 4.0f;
            float max_width = (std::max)(right_text_size.x, right_media_size.x);
            draw_list->AddRectFilled(
                ImVec2(viewport_pos.x + half_width + (half_width - max_width) * 0.5f - 5, right_text_pos.y - 2),
                ImVec2(viewport_pos.x + half_width + (half_width + max_width) * 0.5f + 5, right_text_pos.y + combined_height + 2),
                IM_COL32(20, 20, 20, 180)
            );

            // Draw labels
            draw_list->AddText(font_mono, font_size, right_text_pos, IM_COL32(200, 200, 200, 255), right_label);
            ImVec2 right_media_pos(viewport_pos.x + half_width + (half_width - right_media_size.x) * 0.5f, right_text_pos.y + right_text_size.y + 2.0f);
            draw_list->AddText(font_mono, small_font_size, right_media_pos, IM_COL32(150, 150, 150, 255), right_media_name.c_str());
        }
    }

    // Floating dropdown overlay - top-left corner to select comparison mode
    const float dropdown_width = 180.0f;
    const float dropdown_height = 28.0f;
    const float dropdown_spacing = 4.0f;  // Space between dropdowns

    // View mode dropdown (legacy split view modes)
    ImGui::SetCursorScreenPos(ImVec2(viewport_pos.x + 10, viewport_pos.y + 10));
    ImGui::BeginChild("##ComparisonModeToggle", ImVec2(dropdown_width, dropdown_height), false,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground);

    std::string view_label = "View: Side-by-Side";
    if (comparison_mode_ == ComparisonMode::SPLIT_SCREEN) {
        view_label = "View: Split Screen";
    }

    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.15f, 0.15f, 0.15f, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.25f, 0.25f, 0.25f, 0.95f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.35f, 0.35f, 0.35f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));

    ImGui::SetNextItemWidth(dropdown_width);
    if (ImGui::BeginCombo("##ViewModeSelect", view_label.c_str(), ImGuiComboFlags_NoArrowButton)) {
        if (ImGui::Selectable("View Side-by-Side", comparison_mode_ == ComparisonMode::SIDE_BY_SIDE)) {
            SetComparisonMode(ComparisonMode::SIDE_BY_SIDE);
            Debug::Log("Switched to Side-by-Side view mode");
        }
        if (ImGui::Selectable("View Split Screen", comparison_mode_ == ComparisonMode::SPLIT_SCREEN)) {
            SetComparisonMode(ComparisonMode::SPLIT_SCREEN);
            Debug::Log("Switched to Split Screen view mode");
        }
        ImGui::EndCombo();
    }

    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar();
    ImGui::EndChild();

    // Lavfi Mode Dropdown (positioned close below view picker)
    if (left_texture > 0 && comparison_video_ && comparison_video_->HasVideo()) {
        const float lavfi_dropdown_y = viewport_pos.y + 10.0f + dropdown_height + dropdown_spacing;

        ImGui::SetCursorScreenPos(ImVec2(viewport_pos.x + 10.0f, lavfi_dropdown_y));
        ImGui::BeginChild("##LavfiModeDropdownSideBySide", ImVec2(dropdown_width, dropdown_height), false,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground);

        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.15f, 0.15f, 0.15f, 0.85f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.25f, 0.25f, 0.25f, 0.95f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.35f, 0.35f, 0.35f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));

        // Set popup height to fit all items without scrollbar
        ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(FLT_MAX, 270.0f));
        ImGui::SetNextItemWidth(dropdown_width);
        if (ImGui::BeginCombo("##LavfiModeSideBySide", "Play Modes...", ImGuiComboFlags_NoArrowButton | ImGuiComboFlags_HeightLarge)) {
            if (ImGui::Selectable("Side-by-Side A")) {
                Debug::Log("Lavfi: Side-by-Side A (primary as reference)");
                TransitionToLavfiMode(ComparisonMode::SPLIT_HORIZONTAL,
                                     static_cast<int>(content_region.x),
                                     static_cast<int>(content_region.y));
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Uses left video resolution as reference\n(scales right video to match)");
            }

            if (ImGui::Selectable("Side-by-Side B")) {
                Debug::Log("Lavfi: Side-by-Side B (secondary as reference)");
                TransitionToLavfiMode(ComparisonMode::SPLIT_HORIZONTAL,
                                     static_cast<int>(content_region.x),
                                     static_cast<int>(content_region.y),
                                     "", "", true);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Uses right video resolution as reference\n(scales left video to match)");
            }

            ImGui::Separator();

            if (ImGui::Selectable("Top-Bottom A")) {
                Debug::Log("Lavfi: Top-Bottom A (primary as reference)");
                TransitionToLavfiMode(ComparisonMode::SPLIT_VERTICAL,
                                     static_cast<int>(content_region.x),
                                     static_cast<int>(content_region.y));
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Uses top video resolution as reference\n(scales bottom video to match)");
            }

            if (ImGui::Selectable("Top-Bottom B")) {
                Debug::Log("Lavfi: Top-Bottom B (secondary as reference)");
                TransitionToLavfiMode(ComparisonMode::SPLIT_VERTICAL,
                                     static_cast<int>(content_region.x),
                                     static_cast<int>(content_region.y),
                                     "", "", true);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Uses bottom video resolution as reference\n(scales top video to match)");
            }

            ImGui::Separator();

            if (ImGui::Selectable("Split-Screen A")) {
                Debug::Log("Lavfi: Split-Screen A (50/50 split - primary on left)");
                TransitionToLavfiMode(ComparisonMode::SPLIT_5050_HORIZONTAL,
                                     static_cast<int>(content_region.x),
                                     static_cast<int>(content_region.y));
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("50/50 split with left video on left side");
            }

            if (ImGui::Selectable("Split-Screen B")) {
                Debug::Log("Lavfi: Split-Screen B (50/50 split - secondary on left)");
                TransitionToLavfiMode(ComparisonMode::SPLIT_5050_HORIZONTAL,
                                     static_cast<int>(content_region.x),
                                     static_cast<int>(content_region.y),
                                     "", "", true);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("50/50 split with right video on left side");
            }

            ImGui::Separator();

            if (ImGui::Selectable("Difference A")) {
                Debug::Log("Lavfi: Difference A (primary as base reference)");
                TransitionToLavfiMode(ComparisonMode::DIFFERENCE_BLEND,
                                     static_cast<int>(content_region.x),
                                     static_cast<int>(content_region.y));
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Pixel-level difference blend\nUses left video resolution as reference");
            }

            if (ImGui::Selectable("Difference B")) {
                Debug::Log("Lavfi: Difference B (secondary as base reference)");
                TransitionToLavfiMode(ComparisonMode::DIFFERENCE_BLEND,
                                     static_cast<int>(content_region.x),
                                     static_cast<int>(content_region.y),
                                     "", "", true);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Pixel-level difference blend\nUses right video resolution as reference");
            }

            ImGui::EndCombo();
        }

        ImGui::PopStyleColor(4);
        ImGui::PopStyleVar();
        ImGui::EndChild();
    }

    // Show transcoding progress underneath toggle button
    if (transcoding_in_progress_ && transcode_manager_) {
        float progress = transcode_manager_->GetProgress();

        ImGui::SetCursorScreenPos(ImVec2(viewport_pos.x + 10, viewport_pos.y + 56));

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 6));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.1f, 0.1f, 0.1f, 0.85f));

        ImGui::BeginChild("##TranscodeProgressSideBySide", ImVec2(280, 52), false,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground);

        // Use mono font and system accent color
        ImGui::PushFont(font_mono);
        ImGui::Text("Transcoding difference...");
        ImGui::PopFont();

        // Progress bar with system accent color
        ImVec4 accent = GetWindowsAccentColor();
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, accent);
        char progress_text[32];
        snprintf(progress_text, sizeof(progress_text), "%d%%", (int)(progress * 100));
        ImGui::ProgressBar(progress, ImVec2(-1, 0), progress_text);
        ImGui::PopStyleColor();

        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
    }

}

void VideoPlayer::RenderSplitScreen() {
    // Debug: Log every 60 frames
    static int split_debug_counter = 0;
    if (split_debug_counter++ % 60 == 0) {
        Debug::Log("RenderSplitScreen: Timer active=" + std::to_string(dual_view_timer_ != nullptr) +
                   ", right.source_in=" + std::to_string(dual_view_timeline_.right.source_in) +
                   ", right.position_offset=" + std::to_string(dual_view_timeline_.right.position_offset));
    }

    ImVec2 content_region = ImGui::GetContentRegionAvail();

    // Get draw list and viewport position for overlays
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImVec2 viewport_pos = ImGui::GetCursorScreenPos();

    // Calculate split position in pixels
    float split_x = content_region.x * split_screen_position_;

    // Update comparison video texture
    if (comparison_video_ && comparison_video_->HasVideo()) {
        comparison_video_->UpdateVideoTexture();
    }

    // Get textures
    GLuint primary_texture = GetDisplayTexture();
    GLuint comparison_texture = (comparison_video_ && comparison_video_->HasVideo())
        ? comparison_video_->GetTexture() : 0;

    // Calculate full-screen sizes for both videos
    float aspect_ratio = (float)video_width / (float)video_height;
    ImVec2 primary_size = CalculateFitSize(video_width, video_height, content_region.x, content_region.y);
    ImVec2 primary_pos = ImVec2(
        (content_region.x - primary_size.x) * 0.5f,
        (content_region.y - primary_size.y) * 0.5f
    );

    // Render left side (primary video or placeholder) - clipped to split position
    ImVec2 cursor_pos = ImGui::GetCursorPos();
    if (primary_texture > 0 && glIsTexture(primary_texture)) {
        // Set up clipping for left side
        ImGui::PushClipRect(
            viewport_pos,
            ImVec2(viewport_pos.x + split_x, viewport_pos.y + content_region.y),
            true
        );

        ImGui::SetCursorPos(ImVec2(cursor_pos.x + primary_pos.x, cursor_pos.y + primary_pos.y));
        ImGui::Image((void*)(intptr_t)primary_texture, primary_size);

        ImGui::PopClipRect();
    } else {
        // Show drop target placeholder on left side
        ImGui::PushClipRect(
            viewport_pos,
            ImVec2(viewport_pos.x + split_x, viewport_pos.y + content_region.y),
            true
        );

        ImGui::SetCursorPos(cursor_pos);
        RenderPrimaryDropTargetPlaceholder(split_x, content_region.y);

        ImGui::PopClipRect();
    }

    // Render right side (comparison video) - clipped from split position
    if (comparison_texture > 0 && glIsTexture(comparison_texture)) {
        int comp_w = comparison_video_->GetWidth();
        int comp_h = comparison_video_->GetHeight();
        ImVec2 comparison_size = CalculateFitSize(comp_w, comp_h, content_region.x, content_region.y);
        ImVec2 comparison_pos = ImVec2(
            (content_region.x - comparison_size.x) * 0.5f,
            (content_region.y - comparison_size.y) * 0.5f
        );

        // Set up clipping for right side
        ImGui::PushClipRect(
            ImVec2(viewport_pos.x + split_x, viewport_pos.y),
            ImVec2(viewport_pos.x + content_region.x, viewport_pos.y + content_region.y),
            true
        );

        ImGui::SetCursorPos(ImVec2(cursor_pos.x + comparison_pos.x, cursor_pos.y + comparison_pos.y));
        ImGui::Image((void*)(intptr_t)comparison_texture, comparison_size);

        ImGui::PopClipRect();
    } else if (!comparison_video_ || !comparison_video_->HasVideo()) {
        // Show drop target on right side
        ImGui::PushClipRect(
            ImVec2(viewport_pos.x + split_x, viewport_pos.y),
            ImVec2(viewport_pos.x + content_region.x, viewport_pos.y + content_region.y),
            true
        );

        ImGui::SetCursorPos(ImVec2(cursor_pos.x + split_x, cursor_pos.y));
        RenderDropTargetPlaceholder(content_region.x - split_x, content_region.y);

        // Add invisible drop target
        ImGui::SetCursorPos(ImVec2(cursor_pos.x + split_x, cursor_pos.y));
        ImGui::InvisibleButton("##ComparisonDropTarget", ImVec2(content_region.x - split_x, content_region.y));

        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("MEDIA_ITEM")) {
                std::string media_id((const char*)payload->Data, payload->DataSize - 1);
                Debug::Log("Comparison video drop received: " + media_id);
                comparison_drop_pending_id_ = media_id;
            }
            ImGui::EndDragDropTarget();
        }

        ImGui::PopClipRect();
    }

    // Add drop targets over both sides (allows changing videos after loading)
    // Left side drop target (primary video)
    ImGui::SetCursorPos(ImVec2(0, 0));
    ImGui::InvisibleButton("##PrimaryVideoDropTargetSplit", ImVec2(split_x, content_region.y));
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("MEDIA_ITEM")) {
            std::string media_id((const char*)payload->Data, payload->DataSize - 1);
            Debug::Log("Primary video drop received (split): " + media_id);
            viewport_drop_pending_id_ = media_id;
        }
        ImGui::EndDragDropTarget();
    }

    // Right side drop target (comparison video)
    ImGui::SetCursorPos(ImVec2(split_x, 0));
    ImGui::InvisibleButton("##ComparisonVideoDropTargetSplit", ImVec2(content_region.x - split_x, content_region.y));
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("MEDIA_ITEM")) {
            std::string media_id((const char*)payload->Data, payload->DataSize - 1);
            Debug::Log("Comparison video drop received (split): " + media_id);
            comparison_drop_pending_id_ = media_id;
        }
        ImGui::EndDragDropTarget();
    }

    // Draw gap overlays (black) when playhead is outside a clip's range on virtual timeline
    // This provides visual feedback that the clip has a gap at this position
    double timeline_pos = GetVirtualTimelinePosition();

    // Left video gap overlay (clipped to left side of split)
    ClipGapState left_gap = GetPrimaryGapState(timeline_pos);
    if (left_gap != ClipGapState::PLAYING && primary_texture > 0) {
        ImVec2 gap_min(viewport_pos.x + primary_pos.x, viewport_pos.y + primary_pos.y);
        ImVec2 gap_max(gap_min.x + primary_size.x, gap_min.y + primary_size.y);

        // Clip to left side of split
        draw_list->PushClipRect(viewport_pos, ImVec2(viewport_pos.x + split_x, viewport_pos.y + content_region.y), true);
        draw_list->AddRectFilled(gap_min, gap_max, IM_COL32(0, 0, 0, 255));

        const char* gap_text = (left_gap == ClipGapState::GAP_BEFORE) ? "BEFORE CLIP" : "AFTER CLIP";
        if (font_mono) {
            ImVec2 text_size = font_mono->CalcTextSizeA(12.0f, FLT_MAX, 0.0f, gap_text);
            ImVec2 text_pos((gap_min.x + gap_max.x - text_size.x) * 0.5f,
                           (gap_min.y + gap_max.y - text_size.y) * 0.5f);
            draw_list->AddText(font_mono, 12.0f, text_pos, IM_COL32(100, 100, 100, 255), gap_text);
        }
        draw_list->PopClipRect();
    }

    // Right video gap overlay (clipped to right side of split)
    ClipGapState right_gap = GetSecondaryGapState(timeline_pos);
    if (right_gap != ClipGapState::PLAYING && comparison_video_ && comparison_video_->HasVideo()) {
        int comp_w = comparison_video_->GetWidth();
        int comp_h = comparison_video_->GetHeight();
        ImVec2 comparison_size = CalculateFitSize(comp_w, comp_h, content_region.x, content_region.y);
        ImVec2 comparison_pos_calc = ImVec2(
            (content_region.x - comparison_size.x) * 0.5f,
            (content_region.y - comparison_size.y) * 0.5f
        );

        ImVec2 gap_min(viewport_pos.x + comparison_pos_calc.x, viewport_pos.y + comparison_pos_calc.y);
        ImVec2 gap_max(gap_min.x + comparison_size.x, gap_min.y + comparison_size.y);

        // Clip to right side of split
        draw_list->PushClipRect(ImVec2(viewport_pos.x + split_x, viewport_pos.y),
                                ImVec2(viewport_pos.x + content_region.x, viewport_pos.y + content_region.y), true);
        draw_list->AddRectFilled(gap_min, gap_max, IM_COL32(0, 0, 0, 255));

        const char* gap_text = (right_gap == ClipGapState::GAP_BEFORE) ? "BEFORE CLIP" : "AFTER CLIP";
        if (font_mono) {
            ImVec2 text_size = font_mono->CalcTextSizeA(12.0f, FLT_MAX, 0.0f, gap_text);
            ImVec2 text_pos((gap_min.x + gap_max.x - text_size.x) * 0.5f,
                           (gap_min.y + gap_max.y - text_size.y) * 0.5f);
            draw_list->AddText(font_mono, 12.0f, text_pos, IM_COL32(100, 100, 100, 255), gap_text);
        }
        draw_list->PopClipRect();
    }

    // Draw draggable divider line
    float divider_width = 8.0f;  // Wider hit area for dragging
    float divider_visual_width = 2.0f;
    ImVec2 divider_top(viewport_pos.x + split_x, viewport_pos.y);
    ImVec2 divider_bottom(viewport_pos.x + split_x, viewport_pos.y + content_region.y);

    // Check for divider hover and drag
    ImVec2 mouse_pos = ImGui::GetMousePos();
    bool is_hovering_divider = (mouse_pos.x >= viewport_pos.x + split_x - divider_width * 0.5f &&
                                mouse_pos.x <= viewport_pos.x + split_x + divider_width * 0.5f &&
                                mouse_pos.y >= viewport_pos.y &&
                                mouse_pos.y <= viewport_pos.y + content_region.y);

    // Handle dragging
    if (is_hovering_divider && ImGui::IsMouseClicked(0)) {
        is_dragging_split_ = true;
    }

    if (is_dragging_split_) {
        if (ImGui::IsMouseDown(0)) {
            // Update split position based on mouse
            float new_split = (mouse_pos.x - viewport_pos.x) / content_region.x;
            split_screen_position_ = (std::max)(0.1f, (std::min)(0.9f, new_split));  // Clamp between 10% and 90%
        } else {
            is_dragging_split_ = false;
        }
    }

    // Set cursor when hovering or dragging
    if (is_hovering_divider || is_dragging_split_) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    }

    // Draw divider (highlight when hovering/dragging)
    ImU32 divider_color = (is_hovering_divider || is_dragging_split_)
        ? IM_COL32(180, 180, 180, 255)
        : IM_COL32(80, 80, 80, 200);
    draw_list->AddRectFilled(
        ImVec2(viewport_pos.x + split_x - divider_visual_width * 0.5f, viewport_pos.y),
        ImVec2(viewport_pos.x + split_x + divider_visual_width * 0.5f, viewport_pos.y + content_region.y),
        divider_color
    );

    // Draw text labels
    if (font_mono) {
        float font_size = 14.0f;
        float small_font_size = 11.0f;

        // Left label
        if (split_x > 150.0f) {  // Only show if there's enough space
            const char* left_label = "Primary";
            ImVec2 left_text_size = font_mono->CalcTextSizeA(font_size, FLT_MAX, 0.0f, left_label);
            ImVec2 left_text_pos(viewport_pos.x + (split_x - left_text_size.x) * 0.5f, viewport_pos.y + 10.0f);

            std::string left_media_name;
            if (!current_file_path.empty()) {
                std::filesystem::path p(current_file_path);
                left_media_name = p.filename().string();
            }
            ImVec2 left_media_size = font_mono->CalcTextSizeA(small_font_size, FLT_MAX, 0.0f, left_media_name.c_str());

            float combined_height = left_text_size.y + left_media_size.y + 4.0f;
            float max_width = (std::max)(left_text_size.x, left_media_size.x);
            draw_list->AddRectFilled(
                ImVec2(viewport_pos.x + (split_x - max_width) * 0.5f - 5, left_text_pos.y - 2),
                ImVec2(viewport_pos.x + (split_x + max_width) * 0.5f + 5, left_text_pos.y + combined_height + 2),
                IM_COL32(20, 20, 20, 180)
            );

            draw_list->AddText(font_mono, font_size, left_text_pos, IM_COL32(200, 200, 200, 255), left_label);
            ImVec2 left_media_pos(viewport_pos.x + (split_x - left_media_size.x) * 0.5f, left_text_pos.y + left_text_size.y + 2.0f);
            draw_list->AddText(font_mono, small_font_size, left_media_pos, IM_COL32(150, 150, 150, 255), left_media_name.c_str());
        }

        // Right label
        float right_width = content_region.x - split_x;
        if (right_width > 150.0f && comparison_video_ && comparison_video_->HasVideo()) {  // Only show if there's enough space
            const char* right_label = "Comparison";
            ImVec2 right_text_size = font_mono->CalcTextSizeA(font_size, FLT_MAX, 0.0f, right_label);
            ImVec2 right_text_pos(viewport_pos.x + split_x + (right_width - right_text_size.x) * 0.5f, viewport_pos.y + 10.0f);

            std::string right_media_name;
            if (comparison_video_ && comparison_video_->HasVideo()) {
                std::filesystem::path p(comparison_video_->GetFilePath());
                right_media_name = p.filename().string();
            }
            ImVec2 right_media_size = font_mono->CalcTextSizeA(small_font_size, FLT_MAX, 0.0f, right_media_name.c_str());

            float combined_height = right_text_size.y + right_media_size.y + 4.0f;
            float max_width = (std::max)(right_text_size.x, right_media_size.x);
            draw_list->AddRectFilled(
                ImVec2(viewport_pos.x + split_x + (right_width - max_width) * 0.5f - 5, right_text_pos.y - 2),
                ImVec2(viewport_pos.x + split_x + (right_width + max_width) * 0.5f + 5, right_text_pos.y + combined_height + 2),
                IM_COL32(20, 20, 20, 180)
            );

            draw_list->AddText(font_mono, font_size, right_text_pos, IM_COL32(200, 200, 200, 255), right_label);
            ImVec2 right_media_pos(viewport_pos.x + split_x + (right_width - right_media_size.x) * 0.5f, right_text_pos.y + right_text_size.y + 2.0f);
            draw_list->AddText(font_mono, small_font_size, right_media_pos, IM_COL32(150, 150, 150, 255), right_media_name.c_str());
        }
    }

    // Floating dropdown overlay - top-left corner to select comparison mode
    const float dropdown_width = 180.0f;
    const float dropdown_height = 28.0f;
    const float dropdown_spacing = 4.0f;  // Space between dropdowns

    // View mode dropdown (legacy split view modes)
    ImGui::SetCursorScreenPos(ImVec2(viewport_pos.x + 10, viewport_pos.y + 10));
    ImGui::BeginChild("##ComparisonModeToggleSplit", ImVec2(dropdown_width, dropdown_height), false,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground);

    std::string view_label = "View: Split Screen";
    if (comparison_mode_ == ComparisonMode::SIDE_BY_SIDE) {
        view_label = "View: Side-by-Side";
    }

    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.15f, 0.15f, 0.15f, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.25f, 0.25f, 0.25f, 0.95f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.35f, 0.35f, 0.35f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));

    ImGui::SetNextItemWidth(dropdown_width);
    if (ImGui::BeginCombo("##ViewModeSelect", view_label.c_str(), ImGuiComboFlags_NoArrowButton)) {
        if (ImGui::Selectable("View Side-by-Side", comparison_mode_ == ComparisonMode::SIDE_BY_SIDE)) {
            SetComparisonMode(ComparisonMode::SIDE_BY_SIDE);
            Debug::Log("Switched to Side-by-Side view mode");
        }
        if (ImGui::Selectable("View Split Screen", comparison_mode_ == ComparisonMode::SPLIT_SCREEN)) {
            SetComparisonMode(ComparisonMode::SPLIT_SCREEN);
            Debug::Log("Switched to Split Screen view mode");
        }
        ImGui::EndCombo();
    }

    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar();
    ImGui::EndChild();

    // Lavfi Mode Dropdown (positioned close below view picker)
    if (primary_texture > 0 && comparison_texture > 0) {
        const float lavfi_dropdown_y = viewport_pos.y + 10.0f + dropdown_height + dropdown_spacing;

        ImGui::SetCursorScreenPos(ImVec2(viewport_pos.x + 10.0f, lavfi_dropdown_y));
        ImGui::BeginChild("##LavfiModeDropdown", ImVec2(dropdown_width, dropdown_height), false,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground);

        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.15f, 0.15f, 0.15f, 0.85f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.25f, 0.25f, 0.25f, 0.95f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.35f, 0.35f, 0.35f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));

        // Set popup height to fit all items without scrollbar
        ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(FLT_MAX, 270.0f));
        ImGui::SetNextItemWidth(dropdown_width);
        if (ImGui::BeginCombo("##LavfiMode", "Play Modes...", ImGuiComboFlags_NoArrowButton | ImGuiComboFlags_HeightLarge)) {
            if (ImGui::Selectable("Side-by-Side A")) {
                Debug::Log("Lavfi: Side-by-Side A (primary as reference)");
                TransitionToLavfiMode(ComparisonMode::SPLIT_HORIZONTAL,
                                     static_cast<int>(content_region.x),
                                     static_cast<int>(content_region.y));
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Uses left video resolution as reference\n(scales right video to match)");
            }

            if (ImGui::Selectable("Side-by-Side B")) {
                Debug::Log("Lavfi: Side-by-Side B (secondary as reference)");
                TransitionToLavfiMode(ComparisonMode::SPLIT_HORIZONTAL,
                                     static_cast<int>(content_region.x),
                                     static_cast<int>(content_region.y),
                                     "", "", true);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Uses right video resolution as reference\n(scales left video to match)");
            }

            ImGui::Separator();

            if (ImGui::Selectable("Top-Bottom A")) {
                Debug::Log("Lavfi: Top-Bottom A (primary as reference)");
                TransitionToLavfiMode(ComparisonMode::SPLIT_VERTICAL,
                                     static_cast<int>(content_region.x),
                                     static_cast<int>(content_region.y));
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Uses top video resolution as reference\n(scales bottom video to match)");
            }

            if (ImGui::Selectable("Top-Bottom B")) {
                Debug::Log("Lavfi: Top-Bottom B (secondary as reference)");
                TransitionToLavfiMode(ComparisonMode::SPLIT_VERTICAL,
                                     static_cast<int>(content_region.x),
                                     static_cast<int>(content_region.y),
                                     "", "", true);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Uses bottom video resolution as reference\n(scales top video to match)");
            }

            ImGui::Separator();

            if (ImGui::Selectable("Split-Screen A")) {
                Debug::Log("Lavfi: Split-Screen A (50/50 split - primary on left)");
                TransitionToLavfiMode(ComparisonMode::SPLIT_5050_HORIZONTAL,
                                     static_cast<int>(content_region.x),
                                     static_cast<int>(content_region.y));
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("50/50 split with left video on left side");
            }

            if (ImGui::Selectable("Split-Screen B")) {
                Debug::Log("Lavfi: Split-Screen B (50/50 split - secondary on left)");
                TransitionToLavfiMode(ComparisonMode::SPLIT_5050_HORIZONTAL,
                                     static_cast<int>(content_region.x),
                                     static_cast<int>(content_region.y),
                                     "", "", true);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("50/50 split with right video on left side");
            }

            ImGui::Separator();

            if (ImGui::Selectable("Difference A")) {
                Debug::Log("Lavfi: Difference A (primary as base reference)");
                TransitionToLavfiMode(ComparisonMode::DIFFERENCE_BLEND,
                                     static_cast<int>(content_region.x),
                                     static_cast<int>(content_region.y));
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Pixel-level difference blend\nUses left video resolution as reference");
            }

            if (ImGui::Selectable("Difference B")) {
                Debug::Log("Lavfi: Difference B (secondary as base reference)");
                TransitionToLavfiMode(ComparisonMode::DIFFERENCE_BLEND,
                                     static_cast<int>(content_region.x),
                                     static_cast<int>(content_region.y),
                                     "", "", true);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Pixel-level difference blend\nUses right video resolution as reference");
            }

            ImGui::EndCombo();
        }

        ImGui::PopStyleColor(4);
        ImGui::PopStyleVar();
        ImGui::EndChild();
    }
}

void VideoPlayer::RenderDifference() {
    if (!comparison_video_ || !comparison_video_->HasVideo()) {
        ImGui::Text("Load a comparison video to use difference mode");
        return;
    }

    // Capture viewport position at the very start
    ImVec2 viewport_pos = ImGui::GetCursorScreenPos();
    ImVec2 content_region = ImGui::GetContentRegionAvail();

    // Check if sequence is ready (loaded via EXR cache flow)
    if (!is_exr_mode) {
        // Show toggle button and message underneath
        goto render_toggle_button;
    }

    // Render the difference frame from EXR cache
    {
        int frame_index = CalculateCurrentEXRFrameIndex();
        InjectCurrentEXRFrame();

        // Display the difference texture (already loaded via EXR cache)
        GLuint diff_texture = GetDisplayTexture();
        if (diff_texture != 0) {
            int width = video_width;
            int height = video_height;
            ImVec2 image_size = CalculateFitSize(width, height, content_region.x, content_region.y);
            ImVec2 image_pos((content_region.x - image_size.x) / 2, (content_region.y - image_size.y) / 2);
            ImGui::SetCursorPos(image_pos);
            ImGui::Image((void*)(intptr_t)diff_texture, image_size);
        } else {
            ImGui::Text("Loading difference frame...");
        }

        // Draw "Difference Mode" label with both media names
        if (font_mono) {
            ImDrawList* draw_list = ImGui::GetWindowDrawList();
            float font_size = 14.0f;
            float small_font_size = 11.0f;

            // Main label
            const char* mode_label = "Difference Mode";
            ImVec2 mode_text_size = font_mono->CalcTextSizeA(font_size, FLT_MAX, 0.0f, mode_label);

            // Get both media filenames
            std::string primary_name;
            std::string comparison_name;
            if (!original_video_path_before_difference_.empty()) {
                std::filesystem::path p(original_video_path_before_difference_);
                primary_name = p.filename().string();
            } else if (!current_file_path.empty()) {
                std::filesystem::path p(current_file_path);
                primary_name = p.filename().string();
            }
            if (comparison_video_ && comparison_video_->HasVideo()) {
                std::filesystem::path p(comparison_video_->GetFilePath());
                comparison_name = p.filename().string();
            }

            ImVec2 primary_size = font_mono->CalcTextSizeA(small_font_size, FLT_MAX, 0.0f, primary_name.c_str());
            ImVec2 comparison_size = font_mono->CalcTextSizeA(small_font_size, FLT_MAX, 0.0f, comparison_name.c_str());

            // Calculate total height and max width
            float total_height = mode_text_size.y + primary_size.y + comparison_size.y + 6.0f; // 2px spacing between each
            float max_width = (std::max)({mode_text_size.x, primary_size.x, comparison_size.x});

            // Center horizontally at top
            ImVec2 label_pos(viewport_pos.x + (content_region.x - mode_text_size.x) * 0.5f, viewport_pos.y + 10.0f);

            // Background
            draw_list->AddRectFilled(
                ImVec2(viewport_pos.x + (content_region.x - max_width) * 0.5f - 5, label_pos.y - 2),
                ImVec2(viewport_pos.x + (content_region.x + max_width) * 0.5f + 5, label_pos.y + total_height + 2),
                IM_COL32(20, 20, 20, 180)
            );

            // Draw main label
            draw_list->AddText(font_mono, font_size, label_pos, IM_COL32(200, 200, 200, 255), mode_label);

            // Draw primary filename
            ImVec2 primary_pos(viewport_pos.x + (content_region.x - primary_size.x) * 0.5f, label_pos.y + mode_text_size.y + 2.0f);
            draw_list->AddText(font_mono, small_font_size, primary_pos, IM_COL32(150, 150, 150, 255), primary_name.c_str());

            // Draw comparison filename
            ImVec2 comparison_pos(viewport_pos.x + (content_region.x - comparison_size.x) * 0.5f, primary_pos.y + primary_size.y + 2.0f);
            draw_list->AddText(font_mono, small_font_size, comparison_pos, IM_COL32(150, 150, 150, 255), comparison_name.c_str());
        }
    }

    // Show toggle button and status (skip old compositor code entirely)
    goto render_toggle_button;

render_toggle_button:
    // Render mode selector and status UI
    {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 8));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.1f, 0.1f, 0.1f, 0.85f));

        ImGui::SetCursorScreenPos(ImVec2(viewport_pos.x + 10, viewport_pos.y + 10));
        ImGui::BeginChild("##ComparisonModeToggleDiff", ImVec2(150, 36), false,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground);

        // Determine current mode label
        const char* mode_label = "Difference";
        if (comparison_mode_ == ComparisonMode::SIDE_BY_SIDE) {
            mode_label = "Side-by-Side";
        } else if (comparison_mode_ == ComparisonMode::SPLIT_SCREEN) {
            mode_label = "Split Screen";
        }

        ImGui::SetNextItemWidth(134);
        if (ImGui::BeginCombo("##ModeSelect", mode_label, ImGuiComboFlags_NoArrowButton)) {
            if (ImGui::Selectable("Side-by-Side", comparison_mode_ == ComparisonMode::SIDE_BY_SIDE)) {
                SetComparisonMode(ComparisonMode::SIDE_BY_SIDE);
                Debug::Log("Switched to Side-by-Side mode");
            }
            if (ImGui::Selectable("Split Screen", comparison_mode_ == ComparisonMode::SPLIT_SCREEN)) {
                SetComparisonMode(ComparisonMode::SPLIT_SCREEN);
                Debug::Log("Switched to Split Screen mode");
            }

            // Difference mode - now enabled for all codecs
            // Note: Inter-frame codecs (H.264/H.265) may have slight frame misalignment
            if (ImGui::Selectable("Difference", comparison_mode_ == ComparisonMode::DIFFERENCE_VIEW)) {
                SetComparisonMode(ComparisonMode::DIFFERENCE_VIEW);
                Debug::Log("Switched to Difference mode");
            }
            ImGui::EndCombo();
        }

        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);

        // Show status underneath toggle button
        ImGui::SetCursorScreenPos(ImVec2(viewport_pos.x + 10, viewport_pos.y + 56));

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 6));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.1f, 0.1f, 0.1f, 0.85f));

        if (transcoding_in_progress_ && transcode_manager_) {
            // Show transcoding progress
            float progress = transcode_manager_->GetProgress();

            ImGui::BeginChild("##TranscodeProgress", ImVec2(280, 52), false,
                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground);

            ImGui::PushFont(font_mono);
            ImGui::Text("Transcoding difference...");
            ImGui::PopFont();

            ImVec4 accent = GetWindowsAccentColor();
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, accent);
            char progress_text[32];
            snprintf(progress_text, sizeof(progress_text), "%d%%", (int)(progress * 100));
            ImGui::ProgressBar(progress, ImVec2(-1, 0), progress_text);
            ImGui::PopStyleColor();

            ImGui::EndChild();
        } else if (!is_exr_mode && !transcoding_in_progress_) {
            // Show transcoding prompt
            ImGui::BeginChild("##TranscodePrompt", ImVec2(280, 82), false,
                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground);

            ImGui::PushFont(font_mono);
            ImGui::TextWrapped("Requires transcoding");
            ImGui::PopFont();

            if (ImGui::Button("Start Transcoding", ImVec2(264, 28))) {
                StartTranscoding();
            }

            ImGui::TextDisabled("Cached for future use");

            ImGui::EndChild();
        } else if (!is_exr_mode) {
            // Show initializing message
            ImGui::BeginChild("##InitializingPrompt", ImVec2(280, 40), false,
                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground);

            ImGui::PushFont(font_mono);
            ImGui::Text("Initializing...");
            ImGui::PopFont();

            ImGui::EndChild();
        }

        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
    }

    // Done - skip all the old compositor code below
    return;

    // ========================================================================
    // OLD COMPOSITOR CODE (UNREACHABLE - kept for reference/fallback)
    // ========================================================================
    // Update comparison video texture
    comparison_video_->UpdateVideoTexture();

    // Get both textures
    GLuint primary_texture = GetDisplayTexture();
    GLuint comparison_texture = comparison_video_->GetTexture();

    if (primary_texture == 0 || comparison_texture == 0) {
        // Already handled above
        return;
    }

    // Set up difference compositor
    SetupDifferenceCompositor();

    if (difference_shader_program_ == 0 || difference_texture_ == 0) {
        ImGui::Text("Failed to initialize difference compositor");
        return;
    }

    // Save OpenGL state
    GLint last_viewport[4];
    glGetIntegerv(GL_VIEWPORT, last_viewport);
    GLint last_fbo;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &last_fbo);

    // Render difference to FBO
    glBindFramebuffer(GL_FRAMEBUFFER, difference_fbo_);
    glViewport(0, 0, difference_texture_width_, difference_texture_height_);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    // Use difference shader
    glUseProgram(difference_shader_program_);

    // Bind textures
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, primary_texture);
    glUniform1i(glGetUniformLocation(difference_shader_program_, "tex1"), 0);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, comparison_texture);
    glUniform1i(glGetUniformLocation(difference_shader_program_, "tex2"), 1);

    glUniform1f(glGetUniformLocation(difference_shader_program_, "amplification"), difference_amplification_);

    // Render fullscreen quad
    float vertices[] = {
        // positions   // texCoords
        -1.0f,  1.0f,  0.0f, 1.0f,
        -1.0f, -1.0f,  0.0f, 0.0f,
         1.0f, -1.0f,  1.0f, 0.0f,
        -1.0f,  1.0f,  0.0f, 1.0f,
         1.0f, -1.0f,  1.0f, 0.0f,
         1.0f,  1.0f,  1.0f, 1.0f
    };

    GLuint vbo, vao;
    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glDeleteBuffers(1, &vbo);
    glDeleteVertexArrays(1, &vao);

    // Restore OpenGL state
    glBindFramebuffer(GL_FRAMEBUFFER, last_fbo);
    glViewport(last_viewport[0], last_viewport[1], last_viewport[2], last_viewport[3]);
    glUseProgram(0);

    // Display the difference texture
    ImVec2 image_size = CalculateFitSize(difference_texture_width_, difference_texture_height_, content_region.x, content_region.y);
    ImGui::SetCursorPos(ImVec2((content_region.x - image_size.x) * 0.5f, (content_region.y - image_size.y) * 0.5f));
    ImGui::Image((void*)(intptr_t)difference_texture_, image_size);

    // Floating dropdown overlay - top-left corner to select comparison mode

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 8));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.1f, 0.1f, 0.1f, 0.85f));

    ImGui::SetCursorScreenPos(ImVec2(viewport_pos.x + 10, viewport_pos.y + 10));
    ImGui::BeginChild("##ComparisonModeToggleDiff2", ImVec2(150, 36), false,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground);

    // Determine current mode label
    const char* mode_label2 = "Difference";
    if (comparison_mode_ == ComparisonMode::SIDE_BY_SIDE) {
        mode_label2 = "Side-by-Side";
    } else if (comparison_mode_ == ComparisonMode::SPLIT_SCREEN) {
        mode_label2 = "Split Screen";
    }

    ImGui::SetNextItemWidth(134);
    if (ImGui::BeginCombo("##ModeSelect2", mode_label2, ImGuiComboFlags_NoArrowButton)) {
        if (ImGui::Selectable("Side-by-Side##2", comparison_mode_ == ComparisonMode::SIDE_BY_SIDE)) {
            SetComparisonMode(ComparisonMode::SIDE_BY_SIDE);
            Debug::Log("Switched to Side-by-Side mode");
        }
        if (ImGui::Selectable("Split Screen##2", comparison_mode_ == ComparisonMode::SPLIT_SCREEN)) {
            SetComparisonMode(ComparisonMode::SPLIT_SCREEN);
            Debug::Log("Switched to Split Screen mode");
        }

        // Difference mode - now enabled for all codecs
        // Note: Inter-frame codecs (H.264/H.265) may have slight frame misalignment
        if (ImGui::Selectable("Difference##2", comparison_mode_ == ComparisonMode::DIFFERENCE_VIEW)) {
            SetComparisonMode(ComparisonMode::DIFFERENCE_VIEW);
            Debug::Log("Switched to Difference mode");
        }
        ImGui::EndCombo();
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);

    // Show status underneath toggle button
    ImGui::SetCursorScreenPos(ImVec2(viewport_pos.x + 10, viewport_pos.y + 56));

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 6));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.1f, 0.1f, 0.1f, 0.85f));

    if (transcoding_in_progress_ && transcode_manager_) {
        // Show transcoding progress
        float progress = transcode_manager_->GetProgress();

        ImGui::BeginChild("##TranscodeProgress", ImVec2(280, 52), false,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground);

        ImGui::PushFont(font_mono);
        ImGui::Text("Transcoding difference...");
        ImGui::PopFont();

        ImVec4 accent = GetWindowsAccentColor();
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, accent);
        char progress_text[32];
        snprintf(progress_text, sizeof(progress_text), "%d%%", (int)(progress * 100));
        ImGui::ProgressBar(progress, ImVec2(-1, 0), progress_text);
        ImGui::PopStyleColor();

        ImGui::EndChild();
    } else if (!is_exr_mode && !transcoding_in_progress_) {
        // Show transcoding prompt
        ImGui::BeginChild("##TranscodePrompt", ImVec2(280, 82), false,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground);

        ImGui::PushFont(font_mono);
        ImGui::TextWrapped("Requires transcoding");
        ImGui::PopFont();

        if (ImGui::Button("Start Transcoding", ImVec2(264, 28))) {
            StartTranscoding();
        }

        ImGui::TextDisabled("Cached for future use");

        ImGui::EndChild();
    } else if (!is_exr_mode) {
        // Show initializing message
        ImGui::BeginChild("##InitializingPrompt", ImVec2(280, 40), false,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground);

        ImGui::PushFont(font_mono);
        ImGui::Text("Initializing...");
        ImGui::PopFont();

        ImGui::EndChild();
    }

    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

void VideoPlayer::RenderDropTargetPlaceholder(float width, float height) {
    ImVec2 center(width * 0.5f, height * 0.5f);

    ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPos().x + center.x - 100, ImGui::GetCursorPos().y + center.y - 20));
    ImGui::TextDisabled("Drop video here");

    ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPos().x + center.x - 80, ImGui::GetCursorPos().y + center.y + 10));
    ImGui::TextDisabled("for comparison");
}

void VideoPlayer::RenderPrimaryDropTargetPlaceholder(float width, float height) {
    ImVec2 center(width * 0.5f, height * 0.5f);

    ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPos().x + center.x - 80, ImGui::GetCursorPos().y + center.y - 10));
    ImGui::TextDisabled("Drop video here");
}

void VideoPlayer::RenderMPVToCurrentFBO(mpv_render_context* ctx, int width, int height) {
    if (!ctx) return;

    // Get the currently bound FBO
    GLint current_fbo;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &current_fbo);

    // Setup MPV render params for current FBO
    mpv_opengl_fbo mpv_fbo;
    mpv_fbo.fbo = current_fbo;
    mpv_fbo.w = width;
    mpv_fbo.h = height;
    mpv_fbo.internal_format = 0;

    int flip_y = 1;
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_OPENGL_FBO, &mpv_fbo},
        {MPV_RENDER_PARAM_FLIP_Y, &flip_y},
        {MPV_RENDER_PARAM_INVALID, nullptr}
    };

    mpv_render_context_render(ctx, params);
}

// ============================================================================
// Difference Compositor Implementation
// ============================================================================

GLuint VideoPlayer::CompileDifferenceShader() {
    // Simple vertex shader - fullscreen quad
    const char* vertex_shader_src = R"(
        #version 330 core
        layout(location = 0) in vec2 aPos;
        layout(location = 1) in vec2 aTexCoord;
        out vec2 TexCoord;
        void main() {
            gl_Position = vec4(aPos, 0.0, 1.0);
            TexCoord = aTexCoord;
        }
    )";

    // Fragment shader - compute absolute difference
    const char* fragment_shader_src = R"(
        #version 330 core
        in vec2 TexCoord;
        out vec4 FragColor;
        uniform sampler2D tex1;  // Primary video
        uniform sampler2D tex2;  // Comparison video
        uniform float amplification;
        void main() {
            vec3 color1 = texture(tex1, TexCoord).rgb;
            vec3 color2 = texture(tex2, TexCoord).rgb;
            vec3 diff = abs(color1 - color2) * amplification;
            FragColor = vec4(diff, 1.0);
        }
    )";

    // Compile vertex shader
    GLuint vertex_shader = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vertex_shader, 1, &vertex_shader_src, nullptr);
    glCompileShader(vertex_shader);

    GLint success;
    glGetShaderiv(vertex_shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char info_log[512];
        glGetShaderInfoLog(vertex_shader, 512, nullptr, info_log);
        Debug::Log("ERROR: Vertex shader compilation failed: " + std::string(info_log));
        return 0;
    }

    // Compile fragment shader
    GLuint fragment_shader = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fragment_shader, 1, &fragment_shader_src, nullptr);
    glCompileShader(fragment_shader);

    glGetShaderiv(fragment_shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char info_log[512];
        glGetShaderInfoLog(fragment_shader, 512, nullptr, info_log);
        Debug::Log("ERROR: Fragment shader compilation failed: " + std::string(info_log));
        glDeleteShader(vertex_shader);
        return 0;
    }

    // Link shader program
    GLuint program = glCreateProgram();
    glAttachShader(program, vertex_shader);
    glAttachShader(program, fragment_shader);
    glLinkProgram(program);

    glGetProgramiv(program, GL_LINK_STATUS, &success);
    if (!success) {
        char info_log[512];
        glGetProgramInfoLog(program, 512, nullptr, info_log);
        Debug::Log("ERROR: Shader program linking failed: " + std::string(info_log));
        glDeleteShader(vertex_shader);
        glDeleteShader(fragment_shader);
        return 0;
    }

    glDeleteShader(vertex_shader);
    glDeleteShader(fragment_shader);

    Debug::Log("Difference shader compiled successfully");
    return program;
}

void VideoPlayer::SetupDifferenceCompositor() {
    if (difference_shader_program_ == 0) {
        difference_shader_program_ = CompileDifferenceShader();
    }

    // Create FBO if needed
    if (difference_fbo_ == 0) {
        glGenFramebuffers(1, &difference_fbo_);
    }

    // Create or resize output texture if needed
    int target_width = video_width;
    int target_height = video_height;

    if (difference_texture_ == 0 ||
        difference_texture_width_ != target_width ||
        difference_texture_height_ != target_height) {

        if (difference_texture_ != 0) {
            glDeleteTextures(1, &difference_texture_);
        }

        glGenTextures(1, &difference_texture_);
        glBindTexture(GL_TEXTURE_2D, difference_texture_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, target_width, target_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);

        difference_texture_width_ = target_width;
        difference_texture_height_ = target_height;

        // Attach texture to FBO
        glBindFramebuffer(GL_FRAMEBUFFER, difference_fbo_);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, difference_texture_, 0);

        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            Debug::Log("ERROR: Difference compositor FBO is not complete!");
        }

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        Debug::Log("Difference compositor set up: " + std::to_string(target_width) + "x" + std::to_string(target_height));
    }
}

void VideoPlayer::CleanupDifferenceCompositor() {
    if (difference_texture_ != 0) {
        glDeleteTextures(1, &difference_texture_);
        difference_texture_ = 0;
    }
    if (difference_fbo_ != 0) {
        glDeleteFramebuffers(1, &difference_fbo_);
        difference_fbo_ = 0;
    }
    if (difference_shader_program_ != 0) {
        glDeleteProgram(difference_shader_program_);
        difference_shader_program_ = 0;
    }
    difference_texture_width_ = 0;
    difference_texture_height_ = 0;
}

// ============================================================================
// NEW: Disk-Based Transcoding for Frame-Accurate Difference Mode
// ============================================================================

void VideoPlayer::StartTranscoding() {
    Debug::Log("VideoPlayer: Starting transcode for difference mode");

    if (!comparison_video_ || !comparison_video_->HasVideo()) {
        Debug::Log("ERROR: No comparison video loaded");
        return;
    }

    // Extract original file paths from EDL paths if needed
    // EDL format: edl://original_path,start=X,length=Y
    // FFmpeg doesn't understand EDL syntax, so we need to extract the original path
    // NOTE: This means we transcode the FULL videos, not just trimmed segments
    std::string primary_file_path = current_file_path;
    std::string comparison_file_path = comparison_video_->GetFilePath();

    // Check if primary is EDL and extract original path
    if (primary_file_path.find("edl://") == 0) {
        size_t comma_pos = primary_file_path.find(',');
        if (comma_pos != std::string::npos) {
            primary_file_path = primary_file_path.substr(6, comma_pos - 6);  // Skip "edl://" prefix
            Debug::Log("StartTranscoding: Extracted original primary path from EDL: " + primary_file_path);
        }
    }

    // Check if comparison is EDL and extract original path
    if (comparison_file_path.find("edl://") == 0) {
        size_t comma_pos = comparison_file_path.find(',');
        if (comma_pos != std::string::npos) {
            comparison_file_path = comparison_file_path.substr(6, comma_pos - 6);  // Skip "edl://" prefix
            Debug::Log("StartTranscoding: Extracted original comparison path from EDL: " + comparison_file_path);
        }
    }

    // TODO: Get metadata for both videos
    // For now, create minimal metadata from what we know
    VideoMetadata primary_metadata;
    primary_metadata.file_path = primary_file_path;  // Use extracted original path
    primary_metadata.width = GetVideoWidth();
    primary_metadata.height = GetVideoHeight();
    primary_metadata.frame_rate = GetFrameRate();
    primary_metadata.total_frames = static_cast<int>(GetDuration() * GetFrameRate());
    primary_metadata.colorspace = "bt709";  // Default
    primary_metadata.range_type = "limited";  // Default
    primary_metadata.bit_depth = 8;  // Default

    VideoMetadata comparison_metadata;
    comparison_metadata.file_path = comparison_file_path;  // Use extracted original path
    comparison_metadata.width = comparison_video_->GetWidth();
    comparison_metadata.height = comparison_video_->GetHeight();
    comparison_metadata.frame_rate = GetFrameRate();  // Use same as primary
    comparison_metadata.total_frames = static_cast<int>(GetDuration() * GetFrameRate());
    comparison_metadata.colorspace = "bt709";  // Default
    comparison_metadata.range_type = "limited";  // Default
    comparison_metadata.bit_depth = 8;  // Default

    // Create transcode manager
    transcode_manager_ = std::make_unique<ump::TranscodeManager>();
    transcode_manager_->SetCompletionCallback([this]() {
        OnTranscodeComplete();
    });

    // Start transcode (use user-configured cache directory)
    // Use extracted original paths (not EDL paths)
    transcoding_in_progress_ = true;
    bool success = transcode_manager_->StartTranscode(
        primary_file_path,        // Original path (EDL extracted if needed)
        primary_metadata,
        comparison_file_path,     // Original path (EDL extracted if needed)
        comparison_metadata,
        5.0f,                     // amplification
        g_custom_cache_path       // user-configured cache directory
    );

    if (!success) {
        Debug::Log("ERROR: Failed to start transcoding");
        transcoding_in_progress_ = false;
        transcode_manager_.reset();
    }
}

void VideoPlayer::OnTranscodeComplete() {
    Debug::Log("VideoPlayer: Transcode complete!");

    // Get the combined cache directory
    std::string combined_dir = transcode_manager_->GetCombinedCacheDirectory();
    double fps = GetFrameRate();
    int total_frames = static_cast<int>(GetDuration() * fps);

    Debug::Log("VideoPlayer: Combined cache directory: " + combined_dir);
    Debug::Log("VideoPlayer: Total frames: " + std::to_string(total_frames));
    Debug::Log("VideoPlayer: FPS: " + std::to_string(fps));

    // Verify the directory and files exist
    if (!std::filesystem::exists(combined_dir)) {
        Debug::Log("ERROR: Combined cache directory does not exist: " + combined_dir);
        transcoding_in_progress_ = false;
        return;
    }

    // Build list of PNG sequence files
    std::vector<std::string> sequence_files;
    for (int i = 0; i < total_frames; ++i) {
        std::stringstream ss;
        ss << combined_dir << "/frame_" << std::setfill('0') << std::setw(5) << i << ".png";
        sequence_files.push_back(ss.str());
    }

    // Verify first frame exists
    if (!sequence_files.empty() && !std::filesystem::exists(sequence_files[0])) {
        Debug::Log("ERROR: First frame does not exist: " + sequence_files[0]);
        transcoding_in_progress_ = false;
        return;
    }

    Debug::Log("VideoPlayer: Loading " + std::to_string(sequence_files.size()) + " PNG frames into DirectEXRCache...");
    Debug::Log("VideoPlayer: Current pipeline mode BEFORE load: " + std::string(PipelineModeToString(current_pipeline_mode)));

    // Store the original video path before loading difference sequence (so we can restore it later)
    if (!current_file_path.empty()) {
        original_video_path_before_difference_ = current_file_path;
        Debug::Log("VideoPlayer: Stored original video path: " + original_video_path_before_difference_);
    }

    // Load the PNG difference sequence using the universal image loader!
    // LoadImageSequenceWithCache properly supports PNGs and uses DirectEXRCache
    bool success = LoadImageSequenceWithCache(
        sequence_files,         // List of PNG files
        fps,                    // Frame rate
        PipelineMode::NORMAL,   // 8-bit RGBA (our difference PNGs are 8-bit RGB)
        0,                      // Auto-detect width from first file
        0                       // Auto-detect height from first file
    );

    transcoding_in_progress_ = false;

    Debug::Log("VideoPlayer: Current pipeline mode AFTER load: " + std::string(PipelineModeToString(current_pipeline_mode)));

    if (!success) {
        Debug::Log("ERROR: LoadImageSequenceWithCache failed for difference sequence");
    } else {
        Debug::Log("VideoPlayer: Difference sequence loaded successfully - using DirectEXRCache!");
    }
}

