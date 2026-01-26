#pragma once

#ifdef WITH_GSTREAMER

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <mutex>
#include <atomic>

// Forward declarations for GStreamer types
typedef struct _GstElement GstElement;
typedef struct _GstPad GstPad;
typedef struct _GstBus GstBus;

namespace ump {

// Forward declarations
class TimelineFlattener;
class PlaybackTimer;
struct OTIOClip;

//=============================================================================
// Audio Source Handle (opaque identifier for managed audio sources)
//=============================================================================

using AudioSourceHandle = uint64_t;

//=============================================================================
// Audio Clip Info (for timeline integration)
//=============================================================================

struct AudioClipInfo {
    std::string clip_id;
    std::string source_path;
    double timeline_start;   // When clip starts on timeline (seconds)
    double timeline_end;     // When clip ends on timeline (seconds)
    double source_in;        // In-point in source file (seconds)
    double source_out;       // Out-point in source file (seconds)
    float volume = 1.0f;     // Volume level (0.0 - 1.0)
    bool muted = false;
};

//=============================================================================
// GStreamer Audio Manager
//
// Replaces AudioPlayer + AudioMixer with a unified GStreamer-based system.
// Handles:
// - Audio decoding from video files (MXF, MOV, MP4)
// - Multi-track mixing via GStreamer's audiomixer element
// - Direct audio output via autoaudiosink (no miniaudio needed)
// - Timeline synchronization for multi-clip playback
//=============================================================================

class GStreamerAudioManager {
public:
    GStreamerAudioManager();
    ~GStreamerAudioManager();

    // Prevent copying
    GStreamerAudioManager(const GStreamerAudioManager&) = delete;
    GStreamerAudioManager& operator=(const GStreamerAudioManager&) = delete;

    //=========================================================================
    // Lifecycle
    //=========================================================================

    bool Initialize();
    void Shutdown();
    bool IsInitialized() const { return initialized_; }

    //=========================================================================
    // Playback Control
    //=========================================================================

    void Play();
    void Pause();
    void Stop();
    bool IsPlaying() const { return is_playing_.load(); }

    // Master volume (0.0 - 1.0)
    void SetVolume(float volume);
    float GetVolume() const { return master_volume_.load(); }

    // Mute all audio
    void SetMuted(bool muted);
    bool IsMuted() const { return is_muted_.load(); }

    //=========================================================================
    // Timeline Integration
    //=========================================================================

    // Set the flattener for querying audible clips
    void SetFlattener(TimelineFlattener* flattener);

    // Set playback timer for position tracking (compatibility with AudioMixer)
    void SetTimer(class PlaybackTimer* timer);

    // Pre-load audio sources for all clips (compatibility with AudioMixer)
    // Note: GStreamer creates sources on-demand, so this just validates sources
    void PreloadClips(const std::vector<OTIOClip>& clips);

    // Clear all loaded clips
    void ClearClips();

    // Update which clips are currently active (call from timeline update loop)
    // This manages dynamic source creation/destruction based on playhead position
    void UpdateActiveClips(double timeline_time);

    // Update (call every frame) - checks for clip switches and sync
    void Update();

    // Seek all active audio sources to match timeline position
    void Seek(double timeline_time);

    // Set timeline position directly (for use without PlaybackTimer)
    void SetTimelinePosition(double position);

    // Get current playback position (for sync checking)
    double GetPosition() const;

    //=========================================================================
    // Source Management (for manual control if needed)
    //=========================================================================

    // Add a single audio source (returns handle for later control)
    AudioSourceHandle AddSource(const std::string& file_path, double start_time = 0.0);

    // Remove a source by handle
    void RemoveSource(AudioSourceHandle handle);

    // Set volume for a specific source
    void SetSourceVolume(AudioSourceHandle handle, float volume);

    // Check if a file has audio
    static bool HasAudio(const std::string& file_path);

    //=========================================================================
    // Status
    //=========================================================================

    int GetActiveSourceCount() const;
    std::vector<std::string> GetActiveSourcePaths() const;

private:
    //=========================================================================
    // Pipeline Building
    //=========================================================================

    bool BuildOutputPipeline();
    void DestroyPipeline();

    //=========================================================================
    // Source Management (Internal)
    //=========================================================================

    struct AudioSource {
        AudioSourceHandle handle = 0;
        std::string file_path;
        std::string clip_id;

        // GStreamer elements for this source
        GstElement* bin = nullptr;        // Container bin for this source
        GstElement* filesrc = nullptr;
        GstElement* decodebin = nullptr;
        GstElement* audioconvert = nullptr;
        GstElement* audioresample = nullptr;
        GstElement* volume = nullptr;
        GstPad* mixer_pad = nullptr;      // Pad on audiomixer

        // Timeline info
        double timeline_start = 0.0;
        double timeline_end = 0.0;
        double source_in = 0.0;
        double source_out = 0.0;

        // State
        bool linked = false;
        float volume_level = 1.0f;
    };

    // Create a source bin and link to mixer
    AudioSourceHandle CreateSource(const AudioClipInfo& clip_info);

    // Remove and cleanup a source
    void DestroySource(AudioSourceHandle handle);

    // Callback when decodebin creates a new pad
    static void OnPadAdded(GstElement* element, GstPad* pad, void* data);

    // Get next unique handle
    AudioSourceHandle NextHandle();

    //=========================================================================
    // State
    //=========================================================================

    bool initialized_ = false;
    std::atomic<bool> is_playing_{false};
    std::atomic<bool> is_muted_{false};
    std::atomic<float> master_volume_{1.0f};

    // Main output pipeline
    GstElement* pipeline_ = nullptr;
    GstElement* audiomixer_ = nullptr;
    GstElement* master_volume_element_ = nullptr;
    GstElement* audiosink_ = nullptr;

    // Source management
    std::map<AudioSourceHandle, std::unique_ptr<AudioSource>> sources_;
    mutable std::mutex sources_mutex_;
    std::atomic<AudioSourceHandle> next_handle_{1};

    // Timeline integration
    TimelineFlattener* flattener_ = nullptr;
    PlaybackTimer* timer_ = nullptr;
    std::atomic<double> current_position_{0.0};

    // Track which clips are currently active (by clip_id)
    std::map<std::string, AudioSourceHandle> active_clips_;
};

} // namespace ump

#endif // WITH_GSTREAMER
