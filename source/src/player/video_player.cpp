#include "video_player.h"
#include "../project/media_item.h"
#include "../utils/debug_utils.h"
#include "../utils/gpu_scheduler.h"
#include "dummy_video_generator.h"
#include "exr_transcoder.h"
#include "direct_exr_cache.h"
#include "image_loaders.h"  // For TIFF/PNG/JPEG loaders
#include "thumbnail_cache.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>
#include <vector>
#include <regex>
#include <filesystem>

#include <GLFW/glfw3.h>
#include <imgui.h>

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
        true, true, 8,
        "HDR-Res (Half-Float) - HDR Display & OCIO",
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
    mpv_set_option_string(mpv, "keep-open", "always");
    mpv_set_option_string(mpv, "keep-open-pause", "no");

    mpv_set_option_string(mpv, "input-default-bindings", "no");
    mpv_set_option_string(mpv, "cursor-autohide", "no");
    mpv_set_option_string(mpv, "force-window", "no");

    // Visual settings
    mpv_set_option_string(mpv, "alpha", "blend");
    mpv_set_option_string(mpv, "background", "none");
    mpv_set_option_string(mpv, "background-color", "#202020/1.0");
    mpv_set_option_string(mpv, "blend-subtitles", "yes");
}

void VideoPlayer::ConfigureVideoOptions() {
    // Video output and rendering
    mpv_set_option_string(mpv, "vo", "libmpv");
    mpv_set_option_string(mpv, "video-unscaled", "no");
    mpv_set_option_string(mpv, "keepaspect", "yes");
    mpv_set_option_string(mpv, "video-sync", "display-resample");

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

void VideoPlayer::ConfigureCacheOptions() {
    // Cache settings for smooth playback
    mpv_set_option_string(mpv, "cache", "yes");
    mpv_set_option_string(mpv, "cache-secs", "600");
    mpv_set_option_string(mpv, "cache-pause-restart", "yes");
    mpv_set_option_string(mpv, "cache-pause-initial", "yes");
    mpv_set_option_string(mpv, "cache-pause", "no");
    mpv_set_option_string(mpv, "cache-pause-below", "1");

    // Demuxer cache settings
    //mpv_set_option_string(mpv, "demuxer-readahead-secs", "5");
    //mpv_set_option_string(mpv, "demuxer-max-bytes", "4GiB");
    //mpv_set_option_string(mpv, "demuxer-max-packets", "5000");

    // Buffer settings
    mpv_set_option_string(mpv, "stream-buffer-size", "64MiB");
    mpv_set_option_string(mpv, "network-timeout", "60");
    mpv_set_option_string(mpv, "video-latency-hacks", "yes");
}

void VideoPlayer::ConfigureHardwareDecoding() {
    // Threading
    mpv_set_option_string(mpv, "vd-lavc-threads", "0"); // Use all CPU cores
    mpv_set_option_string(mpv, "vd-lavc-dr", "yes");

    // Platform-specific hardware decoding
#ifdef _WIN32
    mpv_set_option_string(mpv, "gpu-api", "d3d11");
    mpv_set_option_string(mpv, "gpu-context", "d3d11");
    mpv_set_option_string(mpv, "hwdec", "d3d11va");
#else
    mpv_set_option_string(mpv, "gpu-api", "opengl");
    mpv_set_option_string(mpv, "hwdec", "auto");
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
    if (video_texture) {
        glDeleteTextures(1, &video_texture);
        video_texture = 0;
        Debug::Log("VideoPlayer::Cleanup: Video texture deleted");
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

    // Enhanced logging for media transitions
    if (is_exr_mode) {
        Debug::Log("Transitioning from EXR/image sequence to regular video: " + path);
    } else if (has_video) {
        Debug::Log("Switching from video to video: " + path);
    }

    // === CLEAR ALL CACHES BEFORE LOADING NEW MEDIA ===
    // This ensures clean transitions between any media types

    // Clear video cache (FrameCache)
    if (cache_clear_callback) {
        Debug::Log("LoadFile: Clearing video cache before loading new media");
        cache_clear_callback();
    }

    // Clear EXR cache (DirectEXRCache)
    if (exr_cache_) {
        Debug::Log("LoadFile: Clearing EXR/image sequence cache before loading new media");
        exr_cache_->Shutdown();
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

    // Reset image sequence flags when loading a new file
    is_image_sequence = false;
    image_sequence_frame_rate = 24.0;

    const char* cmd[] = { "loadfile", path.c_str(), nullptr };
    if (mpv_command(mpv, cmd) < 0) {
        std::cout << "Failed to send loadfile command" << std::endl;
        return;
    }

    WaitForFileLoad();
    FinalizeLoad();

    // Additional verification for post-EXR transitions
    if (!has_video) {
        Debug::Log("WARNING: Video failed to load after EXR transition");
    } else {
        Debug::Log("Successfully loaded regular video after EXR transition");
    }

    // Initialize ThumbnailCache for regular video files
    if (has_video) {
        ump::ThumbnailConfig thumb_config = GetCurrentThumbnailConfig();
        if (thumb_config.enabled) {
            double fps = GetFrameRate();
            double duration = GetDuration();

            // IMPORTANT: Use GetTotalFrames() to match timeline's frame count calculation
            // Timeline uses std::round(duration * fps), we must match exactly
            int frame_count = GetTotalFrames();

            Debug::Log("VideoPlayer: Creating ThumbnailCache for video (fps=" + std::to_string(fps) +
                       ", duration=" + std::to_string(duration) + "s, frames=" + std::to_string(frame_count) + ")");

            // Create VideoImageLoader
            auto video_loader = std::make_unique<ump::VideoImageLoader>(path, fps, duration);

            // Create synthetic frame list ("0", "1", "2", etc.)
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
    }
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
// ============================================================================
// Playback control methods
// ============================================================================

void VideoPlayer::Play() {
    mpv_set_property_string(mpv, "pause", "no");
    is_playing = true;

    // Update DirectEXRCache playback state
    if (exr_cache_) {
        exr_cache_->UpdatePlaybackState(true);
    }
}

void VideoPlayer::Pause() {
    mpv_set_property_string(mpv, "pause", "yes");
    is_playing = false;

    // Update DirectEXRCache playback state
    if (exr_cache_) {
        exr_cache_->UpdatePlaybackState(false);
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
    if (!mpv) return;

    if (pos < 0) pos = 0.0;
    if (pos > cached_duration) pos = cached_duration;

    std::string pos_str = std::to_string(pos);
    const char* cmd[] = { "seek", pos_str.c_str(), "absolute", "exact", nullptr };
    mpv_command_async(mpv, 0, cmd);

    std::cout << "Seeking to: " << pos << " (exact mode)" << std::endl;
}

void VideoPlayer::StepFrame(int direction) {
    const char* cmd = direction > 0 ? "frame-step" : "frame-back-step";
    const char* cmd_array[] = { cmd, nullptr };
    mpv_command(mpv, cmd_array);
}

void VideoPlayer::GoToStart() {
    Seek(0.0);
}

void VideoPlayer::GoToEnd() {
    if (cached_duration > 0) {
        Seek(cached_duration - 0.1);
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

    double seek_amount = 0.1 * fast_seek_speed;
    if (!fast_forward) seek_amount = -seek_amount;

    double new_pos = cached_position + seek_amount;
    if (new_pos < 0) new_pos = 0;
    if (new_pos > cached_duration) new_pos = cached_duration;

    Seek(new_pos);

    // Gradually increase speed
    static int frame_counter = 0;
    frame_counter++;
    if (frame_counter > 60 && fast_seek_speed < 8) {
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

void VideoPlayer::SetupAudioVisualization() {
    if (!mpv || audio_visualization_enabled) return;

    std::cout << "Setting up real-time audio visualization filter..." << std::endl;

    // Set up lavfi showvolume filter for real-time audio level detection
    // This creates a 1x1 pixel output that represents the current audio level
    const char* af_filter = "lavfi=[showvolume=rate=30:f=1:b=4:w=1:h=1:t=0]";

    if (mpv_set_property_string(mpv, "af", af_filter) == 0) {
        audio_visualization_enabled = true;
        std::cout << "Audio visualization filter enabled successfully" << std::endl;

        // Enable property change notifications for audio data
        mpv_observe_property(mpv, 0, "af-metadata", MPV_FORMAT_NODE);
    } else {
        std::cout << "Failed to enable audio visualization filter" << std::endl;
    }
}

void VideoPlayer::UpdateAudioData() {
    if (!mpv || !audio_visualization_enabled) return;

    // Update audio level from MPV events
    // This will be called from the main update loop
    auto now = std::chrono::steady_clock::now();

    // Limit update frequency to avoid overhead
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_audio_update).count() < 33) {
        return; // Update at most 30 FPS
    }
    last_audio_update = now;

    // Try multiple approaches to get real audio data

    // Method 1: Try to get volume level from show volume filter metadata
    mpv_node* af_metadata = nullptr;
    if (mpv_get_property(mpv, "af-metadata", MPV_FORMAT_NODE, &af_metadata) == 0 && af_metadata) {
        // Parse the metadata for volume information
        // This is complex as it requires parsing MPV's filter metadata structure
        // For now, we'll implement a simplified approach
        mpv_free_node_contents(af_metadata);
    }

    // Method 2: Try to get RMS audio level (if available)
    double rms_level = 0.0;
    if (mpv_get_property(mpv, "audio-out-detected-device", MPV_FORMAT_DOUBLE, &rms_level) == 0) {
        // This might not be the right property, but we're exploring available options
        current_audio_level = static_cast<float>(rms_level / 100.0f);
        return;
    }

    // Method 3: Fallback to volume-based estimation with real audio activity detection
    double volume = 0.0;
    int muted = 0;
    bool has_audio = false;

    if (mpv_get_property(mpv, "volume", MPV_FORMAT_DOUBLE, &volume) == 0) {
        has_audio = true;
    }
    mpv_get_property(mpv, "mute", MPV_FORMAT_FLAG, &muted);

    if (muted || !is_playing || !has_audio) {
        current_audio_level = 0.0f;
        return;
    }

    // For now, use a hybrid approach: volume setting + time-based variation
    // This will be replaced with real filter data once we parse the metadata properly
    float volume_base = static_cast<float>(volume / 100.0f);
    double pos = GetPosition();

    // Create more realistic audio patterns
    float audio_activity = 0.3f + 0.5f * abs(sin(pos * 8.0)) * abs(cos(pos * 3.0));
    current_audio_level = volume_base * audio_activity;

    // Clamp to valid range
    current_audio_level = (std::max)(0.0f, (std::min)(1.0f, current_audio_level));
}

float VideoPlayer::GetAudioLevel() const {
    // Return the cached audio level that's updated by UpdateAudioData()
    return current_audio_level;
}

// ============================================================================
// Loop control methods
// ============================================================================

void VideoPlayer::SetLoop(bool enabled) {
    loop_enabled = enabled;

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

    while (true) {
        mpv_event* event = mpv_wait_event(mpv, 0.0);
        if (event->event_id == MPV_EVENT_NONE) break;

        HandleMPVEvent(event);
    }

    // Update real-time audio data
    UpdateAudioData();
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
    }
    else if (prop_name == "pause" && prop->format == MPV_FORMAT_FLAG && prop->data) {
        is_playing = !(*((int*)prop->data));
    }
}

// ============================================================================
// Property getters
// ============================================================================

double VideoPlayer::GetPosition() const {
    return cached_position;
}

double VideoPlayer::GetDuration() const {
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
    if (cached_duration <= 0) return 0;
    return static_cast<int>(std::round(cached_duration * GetFrameRate()));
}

int VideoPlayer::GetCurrentFrame() const {
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

    if (has_video && video_texture) {
        UpdateVideoTexture();
        RenderVideoTexture();
    }
    else {
        RenderPlaceholder();
    }
}

void VideoPlayer::RenderVideoTexture() {
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
    if (display_texture == 0 || !glIsTexture(display_texture)) {
        //Debug::Log("ERROR: Invalid texture to display: " + std::to_string(display_texture));
        // Show black frame instead of error text
        return;
    }

    // Display the texture
    ImGui::Image((void*)(intptr_t)display_texture, image_size);
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

    // Test dummy video generation
    if (ImGui::Button("Test Dummy Generation")) {
        TestDummyVideoGeneration(1920, 1080, 24.0);
    }

    // EXR Cache Progress and Statistics (when in EXR mode)
    if (is_exr_mode && HasEXRCache()) {
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

    int needs_render = mpv_render_context_update(mpv_gl);

    // Check if we need to force render for color pipeline when paused
    // Only force render when NOT playing to avoid impacting playback performance
    bool force_render_for_color = (needs_render <= 0) && !is_playing &&
                                  color_pipeline && color_pipeline->IsValid() &&
                                  color_fbo != 0 && color_texture != 0;

    // Force render for EXR mode to ensure frame updates regardless of dummy video state
    bool force_render_for_exr = (needs_render <= 0) && is_exr_mode && !exr_sequence_files.empty();

    if (needs_render <= 0 && !force_render_for_color && !force_render_for_exr) {
        // No new frame to render and no color pipeline or EXR needing current frame
        return;
    }

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

    // 🔧 EXR INJECTION POINT: Replace dummy video with current EXR frame
    if (is_exr_mode && !exr_sequence_files.empty()) {
        InjectCurrentEXRFrame();

        // 🔧 REMOVED: TriggerEXRFrameCaching() - FFmpeg cache system not used for EXR
        // DirectEXRCache handles all EXR caching with native OpenEXR + memory-mapping

        // 🔧 ProcessReadyTextures() now called unconditionally at start of UpdateVideoTexture()
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

    double dur = 0.0;
    if (mpv_get_property(mpv, "duration", MPV_FORMAT_DOUBLE, &dur) == 0 && dur > 0) {
        cached_duration = dur;
    }

    double pos = 0.0;
    if (mpv_get_property(mpv, "time-pos", MPV_FORMAT_DOUBLE, &pos) == 0) {
        cached_position = pos;
    }

    cached_fps = GetFrameRate();

    int pause_state = 0;
    if (mpv_get_property(mpv, "pause", MPV_FORMAT_FLAG, &pause_state) == 0) {
        is_playing = !pause_state;
    }

    int64_t width = 0, height = 0;
    int new_width = video_width;
    int new_height = video_height;
    
    if (mpv_get_property(mpv, "video-params/w", MPV_FORMAT_INT64, &width) == 0) {
        new_width = (int)width;
    }
    if (mpv_get_property(mpv, "video-params/h", MPV_FORMAT_INT64, &height) == 0) {
        new_height = (int)height;
    }

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

void VideoPlayer::ResetState() {
    has_video = false;
    cached_duration = 0.0;
    cached_position = 0.0;

    // === UNCONDITIONAL CACHE CLEANUP ===
    // Always clean up state, regardless of previous media type
    // This ensures consistent behavior for all transitions

    Debug::Log("ResetState: Cleaning up media state");

    // First, ensure MPV is properly stopped
    if (mpv) {
        const char* cmd[] = { "stop", nullptr };
        mpv_command(mpv, cmd);
    }

    // Clean up EXR/image sequence state if active
    if (is_exr_mode) {
        Debug::Log("ResetState: Cleaning up EXR/image sequence state");

        is_exr_mode = false;
        exr_sequence_files.clear();
        exr_layer_name.clear();
        exr_current_frame = 0;
        exr_frame_count = 0;
        exr_frame_rate = 24.0;
        exr_sequence_start_frame = 0;

        // Clear EXR caching callback
        exr_caching_callback = nullptr;

        // Clear any cached EXR texture if it exists
        if (exr_texture != 0) {
            // If video_texture was pointing to the EXR texture, reset it
            if (video_texture == exr_texture) {
                video_texture = 0;
                Debug::Log("ResetState: Reset video_texture reference to EXR texture");
            }

            glDeleteTextures(1, &exr_texture);
            exr_texture = 0;
            exr_texture_width = 0;
            exr_texture_height = 0;
        }
    }

    // Reset video dimensions to force recreation of textures with new video
    video_width = 0;
    video_height = 0;

    // Clean up video textures to ensure fresh start
    if (video_texture != 0) {
        glDeleteTextures(1, &video_texture);
        video_texture = 0;
    }
    if (fbo != 0) {
        glDeleteFramebuffers(1, &fbo);
        fbo = 0;
    }

    Debug::Log("ResetState: State reset complete");
}

void VideoPlayer::WaitForFileLoad() {
    const int max_attempts = 100;
    int attempts = 0;

    while (attempts < max_attempts) {
        mpv_event* event = mpv_wait_event(mpv, 0.1);

        if (event->event_id == MPV_EVENT_FILE_LOADED) {
            std::cout << "File loaded event received" << std::endl;
            break;
        }

        attempts++;

        double duration = 0.0;
        if (mpv_get_property(mpv, "duration", MPV_FORMAT_DOUBLE, &duration) == 0 && duration > 0) {
            std::cout << "Duration became available: " << duration << std::endl;
            break;
        }
    }
}

void VideoPlayer::FinalizeLoad() {
    UpdateProperties();

    if (cached_duration > 0) {
        has_video = true;
        std::cout << "Successfully loaded video with duration: " << cached_duration << std::endl;
    }
    else {
        std::cout << "Warning: Video loaded but no duration available" << std::endl;
        has_video = false;
    }

    // Set up audio visualization for the loaded content
    SetupAudioVisualization();
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

VideoMetadata VideoPlayer::ExtractMetadata() const {
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

VideoMetadata VideoPlayer::ExtractMetadataFast() const {
    VideoMetadata metadata;

    if (!mpv) {
        return metadata;
    }

    // Single batch operation to get all properties at once
    char* path_result = nullptr;
    char* video_codec_result = nullptr;
    char* pixel_format_result = nullptr;
    char* colorspace_result = nullptr;
    char* primaries_result = nullptr;
    char* trc_result = nullptr;
    char* range_result = nullptr;  // NEW: Color range extraction
    char* audio_codec_result = nullptr;

    int64_t width = 0, height = 0, audio_channels = 0, sample_rate = 0;
    double frame_rate = 0.0;

    // Batch property extraction with error checking
    if (mpv_get_property(mpv, "path", MPV_FORMAT_STRING, &path_result) == 0 && path_result) {
        metadata.PopulateBasicFileInfo(std::string(path_result));
        mpv_free(path_result);
    }

    // Video properties - use cached values when possible
    metadata.width = GetVideoWidth();  // These are already cached
    metadata.height = GetVideoHeight();

    // Get frame rate with fallback
    if (mpv_get_property(mpv, "container-fps", MPV_FORMAT_DOUBLE, &frame_rate) != 0) {
        mpv_get_property(mpv, "estimated-vf-fps", MPV_FORMAT_DOUBLE, &frame_rate);
    }
    metadata.frame_rate = (frame_rate > 0) ? frame_rate : 23.976;
    metadata.total_frames = static_cast<int>(cached_duration * metadata.frame_rate);

    // Get codecs and formats (these are typically fast)
    if (mpv_get_property(mpv, "video-codec", MPV_FORMAT_STRING, &video_codec_result) == 0 && video_codec_result) {
        metadata.video_codec = std::string(video_codec_result);
        mpv_free(video_codec_result);
    }

    if (mpv_get_property(mpv, "video-params/pixelformat", MPV_FORMAT_STRING, &pixel_format_result) == 0 && pixel_format_result) {
        metadata.pixel_format = std::string(pixel_format_result);
        mpv_free(pixel_format_result);
    }

    // Color properties (skip if not essential)
    if (mpv_get_property(mpv, "video-params/colormatrix", MPV_FORMAT_STRING, &colorspace_result) == 0 && colorspace_result) {
        metadata.colorspace = std::string(colorspace_result);
        mpv_free(colorspace_result);
    }

    if (mpv_get_property(mpv, "video-params/primaries", MPV_FORMAT_STRING, &primaries_result) == 0 && primaries_result) {
        metadata.color_primaries = std::string(primaries_result);
        mpv_free(primaries_result);
    }

    if (mpv_get_property(mpv, "video-params/gamma", MPV_FORMAT_STRING, &trc_result) == 0 && trc_result) {
        metadata.color_transfer = std::string(trc_result);
        mpv_free(trc_result);
    }

    // NEW: Color range extraction (critical for proper color matrix application)
    if (mpv_get_property(mpv, "video-params/colorrange", MPV_FORMAT_STRING, &range_result) == 0 && range_result) {
        metadata.range_type = std::string(range_result);
        mpv_free(range_result);
    } else {
        // Fallback to "unknown" if MPV doesn't provide range info
        metadata.range_type = "unknown";
    }

    // Audio properties
    if (mpv_get_property(mpv, "audio-codec", MPV_FORMAT_STRING, &audio_codec_result) == 0 && audio_codec_result) {
        metadata.audio_codec = std::string(audio_codec_result);
        mpv_free(audio_codec_result);
    }

    if (mpv_get_property(mpv, "audio-params/samplerate", MPV_FORMAT_INT64, &sample_rate) == 0) {
        metadata.audio_sample_rate = static_cast<int>(sample_rate);
    }

    if (mpv_get_property(mpv, "audio-params/channel-count", MPV_FORMAT_INT64, &audio_channels) == 0) {
        metadata.audio_channels = static_cast<int>(audio_channels);
    }

    // File size (use existing filesystem info if available, otherwise query)
    if (metadata.file_size == 0) {
        metadata.file_size = GetFileSize();
    }

    // PERFORMANCE FIX: Cache-specific properties will be detected lazily when needed
    // This avoids expensive regex operations during synchronous metadata loading

    metadata.is_loaded = true;
    return metadata;
}

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
    if (file_path.empty()) return 0.0;

    mpv_handle* probe_mpv = mpv_create();
    if (!probe_mpv) return 0.0;

    // Configure for metadata probing only
    mpv_set_option_string(probe_mpv, "vo", "null");
    mpv_set_option_string(probe_mpv, "ao", "null");
    mpv_set_option_string(probe_mpv, "pause", "yes");
    mpv_set_option_string(probe_mpv, "idle", "yes");

    if (mpv_initialize(probe_mpv) < 0) {
        mpv_terminate_destroy(probe_mpv);
        return 0.0;
    }

    const char* cmd[] = { "loadfile", file_path.c_str(), nullptr };
    if (mpv_command(probe_mpv, cmd) < 0) {
        mpv_terminate_destroy(probe_mpv);
        return 0.0;
    }

    double duration = 0.0;
    int attempts = 0;
    while (attempts < 50) {
        if (mpv_get_property(probe_mpv, "duration", MPV_FORMAT_DOUBLE, &duration) == 0 && duration > 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        attempts++;
    }

    mpv_terminate_destroy(probe_mpv);

    std::cout << "Probed duration for " << file_path << ": " << duration << " seconds" << std::endl;
    return duration;
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
    if (video_width <= 0 || video_height <= 0) {
        Debug::Log("SetupColorProcessingResources: Invalid video dimensions " + std::to_string(video_width) + "x" + std::to_string(video_height));
        return;
    }

    // Use pipeline-aware color processing resource creation
    CreateColorProcessingResourcesForMode(video_width, video_height, current_pipeline_mode);

    // Create fullscreen quad for processing
    float quad_vertices[] = {
        // positions   // texCoords (CORRECTED)
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

    // Position attribute
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);

    // TexCoord attribute  
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));

    glBindVertexArray(0);

    //Debug::Log("Color processing resources initialized");
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
    if (color_pipeline) {
        //Debug::Log("Clearing color pipeline and cleaning up OpenGL state");
        color_pipeline.reset();
        
        // Clean up OpenGL state to prevent corruption
        glUseProgram(0);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_3D, 0);  // Clear any LUT bindings
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, 0);
        
        //Debug::Log("Color pipeline cleared and OpenGL state cleaned");
    } else {
        //Debug::Log("No color pipeline to clear");
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
    if (!lut_ids.empty()) {
        for (size_t i = 0; i < lut_ids.size(); ++i) {
            int texture_unit = 1 + i; // Start from GL_TEXTURE1
            glActiveTexture(GL_TEXTURE0 + texture_unit);
            glBindTexture(GL_TEXTURE_3D, lut_ids[i]);
            Debug::Log("Bound 3D LUT texture " + std::to_string(lut_ids[i]) + " to texture unit " + std::to_string(texture_unit));
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
    if (!lut_ids.empty()) {
        for (size_t i = 0; i < lut_ids.size(); ++i) {
            int texture_unit = 1 + i; // Start from GL_TEXTURE1
            glActiveTexture(GL_TEXTURE0 + texture_unit);
            glBindTexture(GL_TEXTURE_3D, lut_ids[i]);
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

    // Bind color FBO
    glBindFramebuffer(GL_FRAMEBUFFER, color_fbo);
    glViewport(0, 0, video_width, video_height);

    // Clear with a test color to verify FBO works
    glClearColor(0.0f, 0.5f, 0.0f, 1.0f);  // Dark green
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
    if (!lut_ids.empty()) {
        for (size_t i = 0; i < lut_ids.size(); ++i) {
            int texture_unit = 1 + i; // Start from GL_TEXTURE1
            glActiveTexture(GL_TEXTURE0 + texture_unit);
            glBindTexture(GL_TEXTURE_3D, lut_ids[i]);
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

            // Create bitmap for clipboard
            BITMAPINFOHEADER bi = {};
            bi.biSize = sizeof(BITMAPINFOHEADER);
            bi.biWidth = video_width;
            bi.biHeight = -video_height; // Negative for top-down bitmap
            bi.biPlanes = 1;
            bi.biBitCount = 32;
            bi.biCompression = BI_RGB;

            HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPINFOHEADER) + pixels.size());
            if (hMem) {
                unsigned char* pMem = (unsigned char*)GlobalLock(hMem);
                if (pMem) {
                    memcpy(pMem, &bi, sizeof(BITMAPINFOHEADER));

                    // Convert RGBA to BGRA for Windows
                    for (size_t i = 0; i < pixels.size(); i += 4) {
                        std::swap(pixels[i], pixels[i + 2]); // Swap R and B
                    }

                    memcpy(pMem + sizeof(BITMAPINFOHEADER), pixels.data(), pixels.size());
                    GlobalUnlock(hMem);

                    SetClipboardData(CF_DIB, hMem);
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

        // Get filename from metadata, fallback to generic name
        std::string base_filename = "ump_Screenshot";
        VideoMetadata metadata = ExtractMetadataFast();
        if (!metadata.file_name.empty()) {
            // Remove extension from filename if present
            std::string filename = metadata.file_name;
            size_t dot_pos = filename.find_last_of('.');
            if (dot_pos != std::string::npos) {
                base_filename = filename.substr(0, dot_pos);
            } else {
                base_filename = filename;
            }
            Debug::Log("Screenshot: Using filename '" + base_filename + "' from metadata");
        } else {
            Debug::Log("Screenshot: file_name is empty, using fallback");
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

        // Flip image vertically (OpenGL reads bottom-up, we want top-down)
        std::vector<unsigned char> flipped_pixels(pixels.size());
        for (int y = 0; y < video_height; y++) {
            memcpy(&flipped_pixels[y * video_width * 4],
                   &pixels[(video_height - 1 - y) * video_width * 4],
                   video_width * 4);
        }

        // Save as PNG using stb_image_write
        int result = stbi_write_png(output_filename.c_str(), video_width, video_height, 4,
                                   flipped_pixels.data(), video_width * 4);

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

        // Flip image vertically (OpenGL reads bottom-up, we want top-down)
        std::vector<unsigned char> flipped_pixels(pixels.size());
        for (int y = 0; y < video_height; y++) {
            memcpy(&flipped_pixels[y * video_width * 4],
                   &pixels[(video_height - 1 - y) * video_width * 4],
                   video_width * 4);
        }

        // Save as PNG using stb_image_write
        int result = stbi_write_png(output_filename.c_str(), video_width, video_height, 4,
                                   flipped_pixels.data(), video_width * 4);

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

// EXR sequence loading (DEPRECATED - use LoadEXRSequenceWithShader instead)
bool VideoPlayer::LoadEXRSequence(const std::string& sequence_path, const std::string& layer_name, double fps, const std::vector<std::string>& sequence_files) {
    Debug::Log("LoadEXRSequence: DEPRECATED - redirecting to LoadEXRSequenceWithShader");
    return LoadEXRSequenceWithShader(sequence_files, layer_name, fps);
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

bool VideoPlayer::TestDummyVideoGeneration(int width, int height, double fps) {
    Debug::Log("Testing dummy video generation: " + std::to_string(width) + "x" +
               std::to_string(height) + " @ " + std::to_string(fps) + " fps");

    std::string dummy_path = dummy_generator.GetDummyFor(width, height, fps);

    if (dummy_path.empty()) {
        Debug::Log("ERROR: Failed to generate dummy video");
        return false;
    }

    Debug::Log("Successfully generated dummy: " + dummy_path);

    // Test loading the dummy video in MPV
    LoadFile(dummy_path);

    return true;
}

bool VideoPlayer::LoadEXRSequenceWithShader(const std::vector<std::string>& sequence_files,
                                           const std::string& layer_name,
                                           double fps) {
    if (sequence_files.empty()) {
        Debug::Log("ERROR: Empty sequence files list");
        return false;
    }

    // Get dimensions from first EXR frame
    int width, height;
    if (!ump::DirectEXRCache::GetFrameDimensions(sequence_files[0], width, height)) {
        //Debug::Log("ERROR: Could not get dimensions from first EXR file: " + sequence_files[0]);
        return false;
    }

    Debug::Log("EXR sequence dimensions: " + std::to_string(width) + "x" + std::to_string(height));

    // Generate or get cached dummy video
    std::string dummy_path = dummy_generator.GetDummyFor(width, height, fps);

    if (dummy_path.empty()) {
        Debug::Log("ERROR: Failed to generate dummy video");
        return false;
    }

    // Load dummy video in MPV (handles timeline automatically)
    LoadFile(dummy_path);

    // Override timeline to match EXR sequence length
    double duration = sequence_files.size() / fps;
    mpv_set_property(mpv, "length", MPV_FORMAT_DOUBLE, &duration);
    mpv_set_property_string(mpv, "loop-file", "inf");  // Loop short dummy

    // TODO: Load EXR replacement shader
    // mpv_set_property_string(mpv, "glsl-shaders-append", "shaders/exr_injection.glsl");

    // Store sequence data for frame processing
    exr_sequence_files = sequence_files;
    exr_layer_name = layer_name;
    exr_frame_rate = fps;
    exr_frame_count = static_cast<int>(sequence_files.size());

    // Extract and cache EXR metadata for inspector
    VideoMetadata exr_metadata = ExtractEXRMetadata(sequence_files, layer_name, fps);
    Debug::Log("ExtractEXRMetadata completed, calling project manager to cache metadata");

    // TODO: Need to pass this to ProjectManager for caching
    // This will make EXR metadata appear in the inspector panel

    // TODO: Process initial frame to setup texture
    // return ProcessAndUploadEXRFrame(0);

    Debug::Log("EXR sequence loaded with shader approach (shader loading pending)");
    return true;
}

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

bool VideoPlayer::LoadEXRSequenceWithDummy(const std::vector<std::string>& sequence_files,
                                           const std::string& layer_name,
                                           double fps) {
    if (sequence_files.empty()) {
        Debug::Log("ERROR: Empty sequence files list");
        return false;
    }

    Debug::Log("Loading EXR sequence with hybrid dummy + OpenGL overlay approach");
    Debug::Log("Sequence: " + std::to_string(sequence_files.size()) + " files, layer: " + layer_name + ", fps: " + std::to_string(fps));

    // Extract start frame from sequence filenames
    int start_frame = ExtractStartFrameFromSequence(sequence_files);
    Debug::Log("EXR sequence start frame: " + std::to_string(start_frame));

    // Get dimensions from first EXR frame
    int width, height;
    if (!ump::DirectEXRCache::GetFrameDimensions(sequence_files[0], width, height)) {
        //Debug::Log("ERROR: Could not get dimensions from first EXR file: " + sequence_files[0]);
        return false;
    }

    Debug::Log("EXR sequence dimensions: " + std::to_string(width) + "x" + std::to_string(height));

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

    // Calculate actual sequence duration
    double duration = sequence_files.size() / fps;
    Debug::Log("EXR sequence duration: " + std::to_string(duration) + " seconds (" + std::to_string(sequence_files.size()) + " frames)");

    // Generate or get cached dummy video with full duration
    std::string dummy_path = dummy_generator.GetDummyFor(width, height, fps, duration);
    if (dummy_path.empty()) {
        Debug::Log("ERROR: Failed to generate full-duration dummy video");
        return false;
    }

    Debug::Log("Using full-duration dummy video: " + dummy_path);

    // Load dummy video in MPV (no duration override needed - dummy matches sequence length)
    // CRITICAL: Use async load for EXR mode - don't block UI waiting for dummy
    if (!mpv) return false;

    const char* cmd[] = {"loadfile", dummy_path.c_str(), nullptr};
    mpv_command_async(mpv, 0, cmd);  // Async load - don't block!
    Debug::Log("Started async dummy video load (non-blocking)");

    // Reapply loop settings for the dummy video (MPV resets settings on new file load)
    SetLoop(loop_enabled);
    Debug::Log("Reapplied loop setting: " + std::string(loop_enabled ? "enabled" : "disabled"));

    // Store sequence data for frame processing
    exr_sequence_files = sequence_files;
    exr_layer_name = layer_name;
    exr_frame_rate = fps;
    exr_frame_count = static_cast<int>(sequence_files.size());
    exr_sequence_start_frame = start_frame;
    is_exr_mode = true;

    Debug::Log("EXR sequence data stored: " + std::to_string(exr_frame_count) + " frames, start frame: " + std::to_string(start_frame));

    // NEW: Initialize EXR background cache (non-blocking)
    InitializeEXRCache(sequence_files, layer_name, fps);

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

    // Process initial frame to setup texture
    Debug::Log("Processing initial EXR frame...");
    if (!ProcessAndFeedEXRFrame(0)) {
        Debug::Log("WARNING: Failed to process initial EXR frame");
    }

    Debug::Log("EXR sequence loaded successfully with hybrid approach");
    return true;
}

// NEW: Universal image sequence loading (TIFF/PNG/JPEG with DirectEXRCache)
bool VideoPlayer::LoadImageSequenceWithCache(const std::vector<std::string>& sequence_files,
                                             double fps,
                                             PipelineMode pipeline_mode) {
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

    // Get dimensions from first file
    int width, height;
    if (!loader->GetDimensions(sequence_files[0], width, height)) {
        Debug::Log("ERROR: Could not get dimensions from first file");
        return false;
    }
    Debug::Log("Image sequence dimensions: " + std::to_string(width) + "x" + std::to_string(height));

    // Extract start frame from sequence filenames
    int start_frame = ExtractStartFrameFromSequence(sequence_files);
    Debug::Log("Image sequence start frame: " + std::to_string(start_frame));

    // === CLEAR ALL CACHES BEFORE LOADING NEW IMAGE SEQUENCE ===
    // This ensures clean transitions when switching between image sequences

    // Clear video cache (FrameCache) to free RAM
    if (cache_clear_callback) {
        Debug::Log("Clearing video cache before loading image sequence (cross-cache eviction)");
        cache_clear_callback();
    }

    // Clear existing EXR/image sequence cache
    if (exr_cache_) {
        Debug::Log("Clearing existing EXR/image sequence cache before loading new sequence");
        exr_cache_->Shutdown();
        exr_cache_.reset();
    }

    // Clear thumbnail cache
    if (thumbnail_cache_) {
        Debug::Log("Clearing thumbnail cache before loading image sequence (media switch)");
        thumbnail_cache_->ClearCache();
        thumbnail_cache_.reset();  // Destroy old thumbnail cache
    }

    // Calculate actual sequence duration
    double sequence_duration = static_cast<double>(sequence_files.size()) / fps;

    // Generate dummy video for the full sequence duration
    std::string dummy_path = dummy_generator.GetDummyFor(width, height, fps, sequence_duration);
    if (dummy_path.empty()) {
        Debug::Log("ERROR: Failed to generate full-duration dummy video");
        return false;
    }

    Debug::Log("Using full-duration dummy video: " + dummy_path);

    // Load dummy video in MPV (async to avoid blocking UI)
    if (!mpv) return false;

    const char* cmd[] = {"loadfile", dummy_path.c_str(), nullptr};
    mpv_command_async(mpv, 0, cmd);
    Debug::Log("Started async dummy video load (non-blocking)");

    // Reapply loop settings for the dummy video
    SetLoop(loop_enabled);
    Debug::Log("Reapplied loop setting: " + std::string(loop_enabled ? "enabled" : "disabled"));

    // Store sequence data for frame processing (reuse EXR infrastructure)
    exr_sequence_files = sequence_files;
    exr_layer_name = "";  // No layer concept for TIFF/PNG/JPEG
    exr_frame_rate = fps;
    exr_frame_count = static_cast<int>(sequence_files.size());
    exr_sequence_start_frame = start_frame;
    is_exr_mode = true;  // Reuse EXR mode flag for all image sequences

    Debug::Log("Image sequence data stored: " + std::to_string(exr_frame_count) + " frames, start frame: " + std::to_string(start_frame));

    // NEW: Initialize DirectEXRCache with universal loader
    if (!exr_cache_) {
        Debug::Log("VideoPlayer: Creating DirectEXRCache");
        exr_cache_ = std::make_shared<ump::DirectEXRCache>();
    }

    // Use new Initialize overload with IImageLoader
    if (exr_cache_->Initialize(std::move(loader), sequence_files, "", fps, pipeline_mode, start_frame)) {
        // Apply current configuration
        ump::DirectEXRCacheConfig config = GetCurrentEXRCacheConfig();
        exr_cache_->SetConfig(config);
        Debug::Log("VideoPlayer: Applied cache config: " +
                   std::to_string(config.video_cache_gb) + "GB cache, " +
                   std::to_string(config.read_behind_seconds) + "s read behind");

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

    // Process initial frame to setup texture
    Debug::Log("Processing initial frame...");
    if (!ProcessAndFeedEXRFrame(0)) {
        Debug::Log("WARNING: Failed to process initial frame");
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

    // Handle looping for EXR sequences (MPV loop doesn't apply to our manual frame injection)
    if (loop_enabled) {
        // Wrap around for looping playback
        if (frame_index >= sequence_size) {
            frame_index = frame_index % sequence_size;
        } else if (frame_index < 0) {
            frame_index = 0;
        }
    } else {
        // Clamp to valid range when not looping
        frame_index = std::clamp(frame_index, 0, sequence_size - 1);

        // If we've reached the end and not looping, we should pause
        // (handled in InjectCurrentEXRFrame to avoid const issues)
    }

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
        Debug::Log("EXR Frame Timing: pos=" + std::to_string(position) +
                   "s, fps=" + std::to_string(fps) +
                   ", calc_frame=" + std::to_string(frame_index) +
                   "/" + std::to_string(sequence_size) +
                   ", loop=" + (loop_enabled ? "ON" : "OFF"));
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
    static bool preload_triggered = false;

    if (current_sequence_path != last_sequence_path) {
        // Sequence changed - reset all static tracking variables
        Debug::Log("EXR sequence changed from '" + last_sequence_path + "' to '" + current_sequence_path + "', resetting static tracking");
        last_sequence_path = current_sequence_path;
        last_log_time = std::chrono::steady_clock::now();
        last_injected_frame = -1;
        last_cache_update_frame = -1;
        last_miss_frame = -1;
        preload_triggered = false;
    }

    // Calculate sequence info and current frame FIRST
    int sequence_size = static_cast<int>(exr_sequence_files.size());
    double sequence_duration = sequence_size / exr_frame_rate;
    int target_frame = CalculateCurrentEXRFrameIndex();

    auto now = std::chrono::steady_clock::now();
    auto ms_since_last = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_log_time).count();

    // Log to see call frequency
    if (ms_since_last > 1000) {  // Every second
        /*Debug::Log("*** InjectCurrentEXRFrame called - target frame: " + std::to_string(target_frame) +
                   " (called every " + std::to_string(ms_since_last) + "ms)");*/
        last_log_time = now;
    }

    // Simple loop strategy: Brief pause at loop point to let cache catch up
    // No complex pre-caching - just give the cache a moment to load first frames after seek
    static bool loop_pause_triggered = false;
    static auto loop_pause_start = std::chrono::steady_clock::now();

    if (loop_enabled && target_frame < 5 && last_injected_frame >= sequence_size - 5) {
        // We just looped back to the beginning
        if (!loop_pause_triggered && is_playing) {
            //Debug::Log("EXR loop: Brief pause at loop point to let cache catch up");
            Pause();
            loop_pause_triggered = true;
            loop_pause_start = std::chrono::steady_clock::now();
        }
    }

    // Resume after brief pause (500ms for safer cache load)
    if (loop_pause_triggered) {
        auto pause_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - loop_pause_start).count();

        if (pause_duration >= 500) {
            Debug::Log("EXR loop: Resuming playback after cache pause");
            Play();
            loop_pause_triggered = false;
        }
    }

    // Reset trigger when far from loop point
    if (target_frame > 10) {
        loop_pause_triggered = false;
    }

    // Handle end-of-sequence behavior AFTER pre-caching
    // Check MPV's actual position to detect loop/end conditions
    if (is_playing && mpv) {
        double mpv_position = 0.0;
        if (mpv_get_property(mpv, "time-pos", MPV_FORMAT_DOUBLE, &mpv_position) == 0) {
            if (loop_enabled) {
                // Loop: seek MPV back when it exceeds duration
                if (mpv_position >= sequence_duration) {
                    Debug::Log("EXR loop: MPV position " + std::to_string(mpv_position) + "s >= " +
                              std::to_string(sequence_duration) + "s, seeking to 0");
                    mpv_command_string(mpv, "seek 0 absolute");
                }
            } else {
                // No loop: pause when reaching end
                if (mpv_position >= sequence_duration - (0.5 / exr_frame_rate)) {
                    Debug::Log("EXR end: pausing at " + std::to_string(mpv_position) + "s");
                    Pause();
                    return; // Don't inject more frames after pausing
                }
            }
        }
    }

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

            // Less verbose - only log on frame change
            if (target_frame != last_injected_frame) {
                Debug::Log("EXR frame " + std::to_string(target_frame) +
                          " displayed (texture " + std::to_string(cached_texture) + ")");
                last_injected_frame = target_frame;
            }

            // Update cache position for background processing (throttled - only on frame change)
            // This is now handled by static tracking above (last_injected_frame)
            // No need to spam UpdateCurrentPosition() 60 times per second
            if (target_frame != last_cache_update_frame) {
                exr_cache_->UpdateCurrentPosition(GetPosition());
                last_cache_update_frame = target_frame;
            }

            // Proactive loop caching: Pre-load first 16 frames when approaching end
            // This ensures seamless looping with no visible hitch
            if (loop_enabled && target_frame >= sequence_size - 20 && target_frame < sequence_size - 1) {
                if (!preload_triggered) {
                    Debug::Log("EXR loop: Pre-caching first 16 frames (frames 0-15) for seamless loop at frame " +
                              std::to_string(target_frame));

                    // Request first 16 frames to be cached (background thread will handle)
                    // UpdateCurrentPosition triggers the cache thread to load around that position
                    for (int i = 0; i < 16 && i < sequence_size; i++) {
                        double preload_timestamp = i / exr_frame_rate;
                        exr_cache_->UpdateCurrentPosition(preload_timestamp);
                    }

                    // Restore current position for normal caching
                    exr_cache_->UpdateCurrentPosition(GetPosition());

                    preload_triggered = true;
                    Debug::Log("EXR loop: Pre-cache triggered, first 16 frames requested");
                }
            } else if (target_frame < sequence_size - 20) {
                // Reset trigger when we're far from the end (back in normal playback range)
                preload_triggered = false;
            }

            return;
        }
        // Frame not cached yet - background thread will load it
        // Update cache position to request this frame (only once per target frame)
        if (target_frame != last_miss_frame) {
            exr_cache_->UpdateCurrentPosition(GetPosition());
            last_miss_frame = target_frame;
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

    // Clean up existing textures
    if (video_texture) {
        glDeleteTextures(1, &video_texture);
        video_texture = 0;
    }
    if (fbo) {
        glDeleteFramebuffers(1, &fbo);
        fbo = 0;
    }

    // NEW: Clean up MPV-specific resources
    if (mpv_texture) {
        glDeleteTextures(1, &mpv_texture);
        mpv_texture = 0;
    }
    if (mpv_fbo) {
        glDeleteFramebuffers(1, &mpv_fbo);
        mpv_fbo = 0;
    }

    // Create OpenGL texture with pipeline-specific format
    glGenTextures(1, &video_texture);
    glBindTexture(GL_TEXTURE_2D, video_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, config.internal_format, width, height,
        0, GL_RGBA, config.data_type, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    // Create FBO for final output (after color correction)
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
        GL_TEXTURE_2D, video_texture, 0);

    // Check FBO completeness
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        Debug::Log("ERROR: Video FBO incomplete for " + std::string(PipelineModeToString(mode)) +
                   "! Status: " + std::to_string(status));
    }

    // NEW: Create separate MPV rendering texture and FBO to break pipeline stalls
    glGenTextures(1, &mpv_texture);
    glBindTexture(GL_TEXTURE_2D, mpv_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, config.internal_format, width, height,
        0, GL_RGBA, config.data_type, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    // Create MPV FBO
    glGenFramebuffers(1, &mpv_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, mpv_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
        GL_TEXTURE_2D, mpv_texture, 0);

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

    // Attach to FBO
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
        GL_TEXTURE_2D, color_texture, 0);

    // Check FBO completeness
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        Debug::Log("ERROR: Color FBO incomplete for " + std::string(PipelineModeToString(mode)) +
                   "! Status: " + std::to_string(status));
    } else {
        Debug::Log("Created color processing resources for " + std::string(PipelineModeToString(mode)) + ": " +
                   std::to_string(width) + "x" + std::to_string(height));
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
                                     const std::string& layer_name, double fps) {
    Debug::Log("VideoPlayer::InitializeEXRCache - " + std::to_string(sequence_files.size()) +
               " files, layer: " + layer_name);

    // 🔧 tlRender pattern: Cache created in constructor with threads always running
    // Just call Initialize to swap sequences (threads stay alive)
    if (!exr_cache_) {
        Debug::Log("VideoPlayer: ERROR - EXR cache should be pre-created in constructor!");
        exr_cache_ = std::make_shared<ump::DirectEXRCache>();
    }

    // Load new sequence (threads keep running, just swap data)
    // Create EXR loader for universal pipeline (ensures consistent path with other image loaders)
    auto exr_loader = std::make_unique<ump::EXRImageLoader>();
    if (exr_cache_->Initialize(std::move(exr_loader), sequence_files, layer_name, fps, PipelineMode::HDR_RES, exr_sequence_start_frame)) {
        // Apply current configuration
        ump::DirectEXRCacheConfig config = GetCurrentEXRCacheConfig();
        exr_cache_->SetConfig(config);
        Debug::Log("VideoPlayer: Applied cache config: " +
                   std::to_string(config.video_cache_gb) + "GB cache, " +
                   std::to_string(config.read_behind_seconds) + "s read behind");

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
                                   int dummy_max_gb, int transcode_max_gb, bool clear_on_exit) {
    // Apply to DummyVideoGenerator
    dummy_generator.SetCacheConfig(custom_path, retention_days, dummy_max_gb, clear_on_exit);

    Debug::Log("VideoPlayer: Disk cache settings updated - retention=" + std::to_string(retention_days) +
              " days, dummy limit=" + std::to_string(dummy_max_gb) + " GB, transcode limit=" +
              std::to_string(transcode_max_gb) + " GB, clear on exit=" + std::string(clear_on_exit ? "ON" : "OFF"));
}

size_t VideoPlayer::ClearEXRDiskCache() {
    size_t total_bytes = 0;

    // Clear dummy videos (already configured with custom cache path via SetCacheSettings)
    total_bytes += dummy_generator.ClearAllDummies();

    // Clear EXR transcodes
    // NOTE: We create a temporary transcoder and configure it with the same settings
    // as the dummy generator to ensure it checks both default and custom cache paths
    static ump::EXRTranscoder transcoder;

    // Configure transcoder with current cache settings (custom path, retention, etc.)
    // The dummy_generator already has these settings from SetCacheSettings()
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

GLuint VideoPlayer::GetThumbnailForFrame(int frame, bool allow_fallback) {
    if (!thumbnail_cache_) {
        return 0;  // No thumbnail cache available
    }
    GLuint texture_id = thumbnail_cache_->GetThumbnail(frame, allow_fallback);

    static int log_counter = 0;
    if (log_counter++ % 100 == 0) {  // Log every 100th request to avoid spam
        Debug::Log("VideoPlayer::GetThumbnailForFrame: frame=" + std::to_string(frame) +
                   ", texture_id=" + std::to_string(texture_id) +
                   ", fallback=" + std::string(allow_fallback ? "true" : "false"));
    }

    return texture_id;
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

