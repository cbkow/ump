#pragma once

// Fix Windows min/max macro conflicts
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
#include <string>

namespace ump {

/**
 * ComparisonVideoPlayer - Lightweight secondary video player for comparison mode
 *
 * Simplified MPV wrapper for dual video review:
 * - Video-only (no audio output)
 * - No caching (passthrough decoding)
 * - Synchronized to primary VideoPlayer position
 * - OpenGL texture output for compositing
 */
class ComparisonVideoPlayer {
public:
    ComparisonVideoPlayer();
    ~ComparisonVideoPlayer();

    // Lifecycle
    bool Initialize();
    void Cleanup();

    // Video loading
    bool LoadFile(const std::string& path);
    void Unload();

    // Synchronization (called by primary VideoPlayer)
    void SyncToPosition(double position);
    void SyncPlaybackState(bool is_playing);
    double GetCurrentPosition() const;
    void SetPlaybackSpeed(double speed);
    void SetLoop(bool enabled);

    // State queries
    bool HasVideo() const { return has_video_; }
    std::string GetFilePath() const { return file_path_; }

    // Texture access for compositing
    GLuint GetTexture() const { return texture_; }
    int GetWidth() const { return width_; }
    int GetHeight() const { return height_; }
    mpv_render_context* GetRenderContext() const { return mpv_gl_; }

    // Update (call once per frame)
    void UpdateVideoTexture();

private:
    // MPV core
    mpv_handle* mpv_ = nullptr;
    mpv_render_context* mpv_gl_ = nullptr;

    // OpenGL resources
    GLuint texture_ = 0;
    GLuint fbo_ = 0;

    // Video properties
    int width_ = 0;
    int height_ = 0;
    std::string file_path_;
    bool has_video_ = false;
    bool is_playing_ = false;

    // Configuration
    void ConfigureBasicOptions();
    void ConfigureVideoOptions();
    void ConfigureNoAudioOptions();
    void ConfigureNoCacheOptions();
    bool SetupOpenGL();
    void CreateVideoTextures(int width, int height);

    // Event handling
    void UpdateFromMPVEvents();
    void HandleMPVEvent(mpv_event* event);

    // Rendering
    void UpdateProperties();

    // OpenGL callback
    static void* GetProcAddress(void* ctx, const char* name);
};

} // namespace ump
