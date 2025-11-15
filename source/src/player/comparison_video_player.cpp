#include "comparison_video_player.h"
#include "../utils/debug_utils.h"
#include <GLFW/glfw3.h>
#include <cstring>
#include <sstream>

namespace ump {

ComparisonVideoPlayer::ComparisonVideoPlayer() {
}

ComparisonVideoPlayer::~ComparisonVideoPlayer() {
    Cleanup();
}

bool ComparisonVideoPlayer::Initialize() {
    Debug::Log("ComparisonVideoPlayer: Initializing...");

    // Create MPV instance
    mpv_ = mpv_create();
    if (!mpv_) {
        Debug::Log("ERROR: ComparisonVideoPlayer: Failed to create MPV instance");
        return false;
    }

    // Configure MPV
    ConfigureBasicOptions();
    ConfigureVideoOptions();
    ConfigureNoAudioOptions();
    ConfigureNoCacheOptions();

    // Initialize MPV
    if (mpv_initialize(mpv_) < 0) {
        Debug::Log("ERROR: ComparisonVideoPlayer: Failed to initialize MPV");
        mpv_terminate_destroy(mpv_);
        mpv_ = nullptr;
        return false;
    }

    // Setup OpenGL rendering
    if (!SetupOpenGL()) {
        Debug::Log("ERROR: ComparisonVideoPlayer: Failed to setup OpenGL");
        mpv_terminate_destroy(mpv_);
        mpv_ = nullptr;
        return false;
    }

    Debug::Log("ComparisonVideoPlayer: Initialized successfully");
    return true;
}

void ComparisonVideoPlayer::Cleanup() {
    Debug::Log("ComparisonVideoPlayer: Cleaning up...");

    has_video_ = false;
    file_path_.clear();

    // Cleanup OpenGL resources
    if (fbo_) {
        glDeleteFramebuffers(1, &fbo_);
        fbo_ = 0;
    }
    if (texture_) {
        glDeleteTextures(1, &texture_);
        texture_ = 0;
    }

    // Cleanup MPV render context
    if (mpv_gl_) {
        mpv_render_context_free(mpv_gl_);
        mpv_gl_ = nullptr;
    }

    // Cleanup MPV instance
    if (mpv_) {
        mpv_terminate_destroy(mpv_);
        mpv_ = nullptr;
    }

    Debug::Log("ComparisonVideoPlayer: Cleanup complete");
}

bool ComparisonVideoPlayer::LoadFile(const std::string& path) {
    if (!mpv_) {
        Debug::Log("ERROR: ComparisonVideoPlayer: MPV not initialized");
        return false;
    }

    Debug::Log("ComparisonVideoPlayer: Loading file: " + path);

    // Load file
    const char* cmd[] = {"loadfile", path.c_str(), nullptr};
    if (mpv_command(mpv_, cmd) < 0) {
        Debug::Log("ERROR: ComparisonVideoPlayer: Failed to load file: " + path);
        return false;
    }

    file_path_ = path;

    // Wait for file to load (simple blocking wait)
    bool file_loaded = false;
    int wait_count = 0;
    const int max_wait = 100; // 10 seconds max

    while (!file_loaded && wait_count < max_wait) {
        mpv_event* event = mpv_wait_event(mpv_, 0.1);
        if (event->event_id == MPV_EVENT_FILE_LOADED) {
            file_loaded = true;
        } else if (event->event_id == MPV_EVENT_END_FILE) {
            Debug::Log("ERROR: ComparisonVideoPlayer: File load failed or ended early");
            return false;
        }
        wait_count++;
    }

    if (!file_loaded) {
        Debug::Log("ERROR: ComparisonVideoPlayer: Timeout waiting for file load");
        return false;
    }

    // Get video dimensions
    UpdateProperties();

    has_video_ = (width_ > 0 && height_ > 0);

    if (has_video_) {
        // Create textures for the video
        CreateVideoTextures(width_, height_);
        Debug::Log("ComparisonVideoPlayer: File loaded successfully (" +
                   std::to_string(width_) + "x" + std::to_string(height_) + ")");
    } else {
        Debug::Log("ERROR: ComparisonVideoPlayer: No valid video dimensions");
        return false;
    }

    // Start paused (will be synced by primary player)
    mpv_command_string(mpv_, "set pause yes");
    is_playing_ = false;

    // Force MPV to render the first frame while paused
    Debug::Log("ComparisonVideoPlayer: Forcing initial frame render");
    UpdateFromMPVEvents();
    UpdateVideoTexture();

    return true;
}

bool ComparisonVideoPlayer::LoadFileTrimmed(const std::string& path, double in_point, double out_point) {
    if (!mpv_) {
        Debug::Log("ERROR: ComparisonVideoPlayer::LoadFileTrimmed: MPV not initialized");
        return false;
    }

    // Validate in/out points
    if (in_point < 0 || out_point <= in_point) {
        Debug::Log("ERROR: ComparisonVideoPlayer::LoadFileTrimmed: Invalid in/out points (in=" +
                   std::to_string(in_point) + ", out=" + std::to_string(out_point) + ")");
        Debug::Log("ComparisonVideoPlayer::LoadFileTrimmed: Falling back to normal LoadFile");
        return LoadFile(path);
    }

    double length = out_point - in_point;

    // Create EDL path: edl://[file],start=[in],length=[length]
    std::ostringstream edl;
    edl << "edl://" << path << ",start=" << in_point << ",length=" << length;
    std::string edl_path = edl.str();

    Debug::Log("ComparisonVideoPlayer::LoadFileTrimmed: Loading trimmed video via EDL");
    Debug::Log("  Original path: " + path);
    Debug::Log("  In point: " + std::to_string(in_point) + "s");
    Debug::Log("  Out point: " + std::to_string(out_point) + "s");
    Debug::Log("  Duration: " + std::to_string(length) + "s");
    Debug::Log("  EDL path: " + edl_path);

    // Use LoadFile with the EDL path
    bool result = LoadFile(edl_path);

    if (result) {
        Debug::Log("ComparisonVideoPlayer::LoadFileTrimmed: Trimmed video loaded successfully");
    } else {
        Debug::Log("ERROR: ComparisonVideoPlayer::LoadFileTrimmed: Failed to load trimmed video");
    }

    return result;
}

void ComparisonVideoPlayer::Unload() {
    Debug::Log("ComparisonVideoPlayer: Unloading video");

    if (mpv_) {
        mpv_command_string(mpv_, "stop");
    }

    has_video_ = false;
    file_path_.clear();
    width_ = 0;
    height_ = 0;

    if (texture_) {
        glDeleteTextures(1, &texture_);
        texture_ = 0;
    }
    if (fbo_) {
        glDeleteFramebuffers(1, &fbo_);
        fbo_ = 0;
    }
}

void ComparisonVideoPlayer::SyncToPosition(double position) {
    if (!mpv_ || !has_video_) return;

    // Debug: Log position sync (only every 60 calls)
    static int sync_counter = 0;
    if (sync_counter++ % 60 == 0) {
        Debug::Log("ComparisonVideoPlayer: Syncing to position " + std::to_string(position));
    }

    // Seek to position
    mpv_set_property_async(mpv_, 0, "time-pos", MPV_FORMAT_DOUBLE, &position);
}

void ComparisonVideoPlayer::SyncPlaybackState(bool is_playing) {
    if (!mpv_ || !has_video_) return;

    if (is_playing != is_playing_) {
        int pause = is_playing ? 0 : 1;
        // Use synchronous property set for immediate response
        mpv_set_property(mpv_, "pause", MPV_FORMAT_FLAG, &pause);
        is_playing_ = is_playing;
        Debug::Log("ComparisonVideoPlayer: Playback state changed to " + std::string(is_playing ? "PLAYING" : "PAUSED"));
    } else {
        // Debug: Log when state hasn't changed
        static int no_change_counter = 0;
        if (no_change_counter++ % 120 == 0) {
            Debug::Log("ComparisonVideoPlayer: Playback state already " + std::string(is_playing ? "PLAYING" : "PAUSED"));
        }
    }
}

double ComparisonVideoPlayer::GetCurrentPosition() const {
    if (!mpv_ || !has_video_) return 0.0;

    double pos = 0.0;
    mpv_get_property(mpv_, "time-pos", MPV_FORMAT_DOUBLE, &pos);
    return pos;
}

void ComparisonVideoPlayer::SetPlaybackSpeed(double speed) {
    if (!mpv_ || !has_video_) return;

    mpv_set_property(mpv_, "speed", MPV_FORMAT_DOUBLE, &speed);
}

void ComparisonVideoPlayer::SetLoop(bool enabled) {
    if (!mpv_ || !has_video_) return;

    // Match primary video's loop-file setting (comparison video doesn't use playlists)
    if (enabled) {
        mpv_set_property_string(mpv_, "loop-file", "inf");
    } else {
        mpv_set_property_string(mpv_, "loop-file", "no");
    }
}

std::string ComparisonVideoPlayer::GetVideoCodec() const {
    if (!mpv_) return "Unknown";

    char* result = nullptr;
    if (mpv_get_property(mpv_, "video-codec", MPV_FORMAT_STRING, &result) == 0 && result) {
        std::string codec(result);
        mpv_free(result);
        return codec;
    }
    return "Unknown";
}

void ComparisonVideoPlayer::UpdateVideoTexture() {
    if (!mpv_gl_ || !has_video_) return;

    // Update MPV rendering
    UpdateFromMPVEvents();

    // Render to our FBO
    mpv_opengl_fbo mpv_fbo;
    mpv_fbo.fbo = static_cast<int>(fbo_);
    mpv_fbo.w = width_;
    mpv_fbo.h = height_;
    mpv_fbo.internal_format = 0;

    int flip_y = 0;  // Match primary video orientation
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_OPENGL_FBO, &mpv_fbo},
        {MPV_RENDER_PARAM_FLIP_Y, &flip_y},
        {MPV_RENDER_PARAM_INVALID, nullptr}
    };

    // Debug: Log render call (only every 60 frames)
    static int render_counter = 0;
    if (render_counter++ % 60 == 0) {
        Debug::Log("ComparisonVideoPlayer: Rendering to FBO " + std::to_string(fbo_) +
                   ", texture " + std::to_string(texture_) +
                   ", size " + std::to_string(width_) + "x" + std::to_string(height_));
    }

    mpv_render_context_render(mpv_gl_, params);
}

// ============================================================================
// Private Configuration Methods
// ============================================================================

void ComparisonVideoPlayer::ConfigureBasicOptions() {
    // Terminal output (suppress)
    mpv_set_option_string(mpv_, "terminal", "no");
    mpv_set_option_string(mpv_, "msg-level", "all=error");

    // Keep window open (for MPV internal use)
    mpv_set_option_string(mpv_, "keep-open", "yes");

    // Transparency support (match main VideoPlayer)
    mpv_set_option_string(mpv_, "background", "none");
    mpv_set_option_string(mpv_, "background-color", "#202020/1.0");
    mpv_set_option_string(mpv_, "blend-subtitles", "yes");
}

void ComparisonVideoPlayer::ConfigureVideoOptions() {
    // Hardware decoding (best performance)
    mpv_set_option_string(mpv_, "hwdec", "auto-safe");
    mpv_set_option_string(mpv_, "vo", "libmpv");
}

void ComparisonVideoPlayer::ConfigureNoAudioOptions() {
    // Disable audio completely
    mpv_set_option_string(mpv_, "audio", "no");
    mpv_set_option_string(mpv_, "ao", "null");
}

void ComparisonVideoPlayer::ConfigureNoCacheOptions() {
    // Disable all caching for minimal overhead
    mpv_set_option_string(mpv_, "cache", "no");
    mpv_set_option_string(mpv_, "cache-pause", "no");
}

bool ComparisonVideoPlayer::SetupOpenGL() {
    // MPV OpenGL initialization parameters
    mpv_opengl_init_params gl_init_params;
    gl_init_params.get_proc_address = GetProcAddress;
    gl_init_params.get_proc_address_ctx = nullptr;

    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_API_TYPE, const_cast<char*>(MPV_RENDER_API_TYPE_OPENGL)},
        {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl_init_params},
        {MPV_RENDER_PARAM_INVALID, nullptr}
    };

    if (mpv_render_context_create(&mpv_gl_, mpv_, params) < 0) {
        Debug::Log("ERROR: ComparisonVideoPlayer: Failed to create MPV render context");
        return false;
    }

    return true;
}

void ComparisonVideoPlayer::CreateVideoTextures(int width, int height) {
    // Delete existing textures if any
    if (texture_) glDeleteTextures(1, &texture_);
    if (fbo_) glDeleteFramebuffers(1, &fbo_);

    // Create texture
    glGenTextures(1, &texture_);
    glBindTexture(GL_TEXTURE_2D, texture_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // Create FBO
    glGenFramebuffers(1, &fbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture_, 0);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        Debug::Log("ERROR: ComparisonVideoPlayer: Framebuffer not complete");
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    Debug::Log("ComparisonVideoPlayer: Created video textures (" +
               std::to_string(width) + "x" + std::to_string(height) + ")");
}

// ============================================================================
// Private Event Handling
// ============================================================================

void ComparisonVideoPlayer::UpdateFromMPVEvents() {
    while (mpv_) {
        mpv_event* event = mpv_wait_event(mpv_, 0);
        if (event->event_id == MPV_EVENT_NONE) break;
        HandleMPVEvent(event);
    }
}

void ComparisonVideoPlayer::HandleMPVEvent(mpv_event* event) {
    switch (event->event_id) {
        case MPV_EVENT_SHUTDOWN:
            Debug::Log("ComparisonVideoPlayer: MPV shutdown event");
            break;
        case MPV_EVENT_LOG_MESSAGE:
            // Suppress log messages
            break;
        default:
            break;
    }
}

void ComparisonVideoPlayer::UpdateProperties() {
    if (!mpv_) return;

    // Get video width
    int64_t width_val = 0;
    if (mpv_get_property(mpv_, "width", MPV_FORMAT_INT64, &width_val) >= 0) {
        width_ = static_cast<int>(width_val);
    }

    // Get video height
    int64_t height_val = 0;
    if (mpv_get_property(mpv_, "height", MPV_FORMAT_INT64, &height_val) >= 0) {
        height_ = static_cast<int>(height_val);
    }
}

// ============================================================================
// Static Methods
// ============================================================================

void* ComparisonVideoPlayer::GetProcAddress(void* ctx, const char* name) {
    return (void*)glfwGetProcAddress(name);
}

} // namespace ump
