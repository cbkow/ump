#pragma once

#include <string>
#include <memory>
#include <atomic>
#include <chrono>
#include <functional>
#include <map>

#include <glad/gl.h>

#ifdef _WIN32
#include <d3d11_1.h>
#endif

#include "../player/pipeline_mode.h"

namespace ump {

// Forward declarations
class PlaylistTimeline;
class PlaylistClip;
class VideoDisplayComponent;
class PlaybackTimer;
class AudioMixer;
class TimelineCache;
class TimelineFlattener;
class ImageSequenceDecoder;
struct MediaItem;

//=============================================================================
// Playlist Playback Controller Configuration
//=============================================================================

struct PlaylistPlaybackConfig {
    int readAheadFrames = 108;       // Frames to prefetch ahead (~4.5s @ 24fps)
    int readBehindFrames = 12;       // Frames to keep behind for backward scrub
    int io_threads = 8;              // Background I/O threads
    double prewarm_seconds = 4.5;    // How far ahead to pre-warm next clip decoder
    PipelineMode pipeline_mode = PipelineMode::NORMAL;

    // NOTE: Single-decoder mode is now handled in TimelineCache (use_single_decoder)
    // when source_mode == PLAYLIST. PlaylistPlaybackController is dead code.
};

//=============================================================================
// Playlist Playback Controller
//
// Simplified playback controller for unified playlist that:
// - Uses native decoders (MPV for video, native loaders for image sequences)
// - Pre-warms next clip's decoder as boundary approaches
// - Handles clip transitions seamlessly
//
// This replaces the complex ManagedVideoDecoder orchestration with a simpler
// approach where each clip type uses its optimal decoder directly.
//=============================================================================

class PlaylistPlaybackController {
public:
    PlaylistPlaybackController();
    ~PlaylistPlaybackController();

    //=========================================================================
    // Initialization
    //=========================================================================

    // Initialize for playlist playback
    bool Initialize(PlaylistTimeline* timeline, VideoDisplayComponent* video_player);

    // Initialize internal cache/flattener (call after clips are added)
    bool InitializeCache();

    // Shutdown and clean up
    void Shutdown();

    //=========================================================================
    // Playback State
    //=========================================================================

    bool IsInitialized() const { return initialized_; }
    bool IsPlaying() const;
    bool IsWaitingForBuffer() const { return waiting_for_buffer_; }
    double GetPosition() const;
    double GetDuration() const;
    int GetCurrentFrame() const;
    double GetFPS() const { return fps_; }

    //=========================================================================
    // Transport Controls
    //=========================================================================

    void Play();
    void Pause();
    void TogglePlayPause();
    void Seek(double position);
    void SeekRelative(double delta);
    void StepForward(int frames = 1);
    void StepBackward(int frames = 1);
    void GoToStart();
    void GoToEnd();

    //=========================================================================
    // Loop Control
    //=========================================================================

    void SetLooping(bool enabled);
    bool IsLooping() const;

    //=========================================================================
    // Update Loop (Call Each Frame)
    //=========================================================================

    // Update timer and return current texture for display
    // Returns 0 if not ready
    GLuint Update(int& width, int& height);

    // Process pending GPU uploads (call from GL thread)
    void ProcessPendingUploads();

    // Update timer (call from render loop)
    void UpdateTimer();

#ifdef _WIN32
    // D3D11 rendering mode
    void SetD3D11RenderingMode(bool enabled);
    bool IsD3D11RenderingMode() const { return use_d3d11_rendering_; }
    ID3D11ShaderResourceView* UpdateD3D11(int& width, int& height);
#endif

    //=========================================================================
    // Configuration
    //=========================================================================

    void SetConfig(const PlaylistPlaybackConfig& config);
    PlaylistPlaybackConfig GetConfig() const { return config_; }

    //=========================================================================
    // Volume/Mute Control
    //=========================================================================

    void SetVolume(double volume);  // 0.0 to 1.0
    void SetMuted(bool muted);
    double GetVolume() const { return volume_; }
    bool IsMuted() const { return muted_; }

    //=========================================================================
    // Clip Information
    //=========================================================================

    // Get current clip being played
    PlaylistClip* GetCurrentClip() const { return current_clip_; }

    // Get clip that's been pre-warmed for upcoming transition
    PlaylistClip* GetPrewarmedClip() const { return prewarmed_clip_; }

    // Get current clip name for UI display
    std::string GetCurrentClipName() const;
    std::string GetCurrentSourcePath() const;

    // Get pipeline mode for current clip
    PipelineMode GetCurrentPipelineMode() const;

    //=========================================================================
    // Scrub Mode
    //=========================================================================

    void SetScrubMode(bool enabled);
    bool IsScrubMode() const { return is_scrubbing_; }

    //=========================================================================
    // Fast Seek (Rewind/Fast Forward)
    //=========================================================================

    void StartRewind();
    void StartFastForward();
    void StopFastSeek();
    void UpdateFastSeek();
    bool IsFastSeeking() const { return is_fast_seeking_; }
    bool IsFastForward() const { return fast_forward_; }
    double GetFastSeekSpeed() const { return fast_seek_speed_; }

    //=========================================================================
    // Callbacks
    //=========================================================================

    // Called when clip transition occurs
    using ClipTransitionCallback = std::function<void(PlaylistClip* from, PlaylistClip* to)>;
    void SetClipTransitionCallback(ClipTransitionCallback callback) { clip_transition_callback_ = callback; }

    // Called when playlist reaches end (non-looping mode)
    using PlaylistEndedCallback = std::function<void()>;
    void SetPlaylistEndedCallback(PlaylistEndedCallback callback) { playlist_ended_callback_ = callback; }

    //=========================================================================
    // Access to Internal Components
    //=========================================================================

    PlaybackTimer* GetTimer() const { return timer_.get(); }
    TimelineCache* GetCache() const { return cache_.get(); }
    AudioMixer* GetAudioMixer() const { return audio_mixer_.get(); }

    //=========================================================================
    // Notify of Timeline Edits
    //=========================================================================

    void NotifyTracksEdited();
    void UpdateDuration(double new_duration);

private:
    //=========================================================================
    // Internal Methods
    //=========================================================================

    // Check and handle clip transitions
    void CheckClipBoundary();

    // Transition to a new clip
    void TransitionToClip(PlaylistClip* clip);

    // Pre-warm decoder for upcoming clip
    void PrewarmNextClip();

    // Load decoder for a clip
    void LoadClipDecoder(PlaylistClip* clip, bool is_prewarm = false);

    // Unload decoder for a clip
    void UnloadClipDecoder(PlaylistClip* clip);

    // Get frame from current clip's decoder
    GLuint GetCurrentFrame(int& width, int& height);

    //=========================================================================
    // State
    //=========================================================================

    bool initialized_ = false;
    PlaylistPlaybackConfig config_;

    // External references (not owned)
    PlaylistTimeline* timeline_ = nullptr;
    VideoDisplayComponent* video_player_ = nullptr;

    // Owned components
    std::unique_ptr<PlaybackTimer> timer_;
    std::unique_ptr<AudioMixer> audio_mixer_;
    std::unique_ptr<TimelineCache> cache_;
    std::unique_ptr<TimelineFlattener> flattener_;

    // Timeline properties
    double timeline_duration_ = 0.0;
    double fps_ = 24.0;
    int width_ = 1920;
    int height_ = 1080;

    // Current playback state
    PlaylistClip* current_clip_ = nullptr;
    PlaylistClip* prewarmed_clip_ = nullptr;
    std::atomic<int> current_frame_{0};
    std::atomic<bool> is_playing_{false};

    // Wait-for-frame state
    bool waiting_for_buffer_ = false;
    std::chrono::steady_clock::time_point waiting_start_time_;
    static constexpr int kMaxWaitMs = 2000;

    // Volume/mute
    double volume_ = 1.0;
    bool muted_ = false;

    // Scrub mode
    bool is_scrubbing_ = false;

    // Fast seek state
    bool is_fast_seeking_ = false;
    bool fast_forward_ = true;
    double fast_seek_speed_ = 1.0;
    std::chrono::steady_clock::time_point fast_seek_start_time_;
    std::chrono::steady_clock::time_point last_fast_seek_update_;

    // Current display texture
    GLuint current_texture_ = 0;
    int current_texture_width_ = 0;
    int current_texture_height_ = 0;

    // Last good texture (for visual continuity during source switches)
    GLuint last_good_texture_ = 0;
    int last_good_texture_width_ = 0;
    int last_good_texture_height_ = 0;

#ifdef _WIN32
    bool use_d3d11_rendering_ = false;
    ID3D11ShaderResourceView* current_srv_d3d_ = nullptr;
    ID3D11ShaderResourceView* last_good_srv_d3d_ = nullptr;
#endif

    // Callbacks
    ClipTransitionCallback clip_transition_callback_;
    PlaylistEndedCallback playlist_ended_callback_;

    // Frame-locked timing
    std::chrono::steady_clock::time_point last_timer_update_;
    double accumulated_time_ = 0.0;
    bool timer_initialized_ = false;
};

} // namespace ump
