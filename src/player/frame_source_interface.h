#pragma once

#include <string>
#include <vector>
#include <memory>
#include <glad/gl.h>
#include "pipeline_mode.h"

namespace ump {

//=============================================================================
// Buffer Health - Used for Adaptive Speed Coordination
//=============================================================================

struct BufferHealth {
    int frames_ahead = 0;           // Contiguous frames cached ahead of playhead
    int frames_behind = 0;          // Frames cached behind playhead
    double fill_rate_fps = 0.0;     // Cache fill rate (frames/second)
    double speed_factor = 1.0;      // Current playback speed multiplier (0.0-1.0)
    bool needs_buffer_wait = false; // Critical buffer underrun - pause needed
    int total_cached = 0;           // Total frames in cache
    int total_frames = 0;           // Total frames in sequence
};

//=============================================================================
// Frame Source Configuration
//=============================================================================

struct FrameSourceConfig {
    std::vector<std::string> files;      // Sequence file paths
    std::string layer;                    // EXR layer name (empty for other formats)
    double fps = 24.0;                    // Frame rate
    PipelineMode pipeline_mode = PipelineMode::HDR_RES;
    int read_ahead_frames = 72;           // Frames to cache ahead
    int read_behind_frames = 12;          // Frames to keep behind
    size_t thread_count = 16;             // I/O threads
    int start_frame = 0;                  // First frame number (for frame numbering)
    bool use_shared_pool = false;         // Use global SharedMemoryPool
};

//=============================================================================
// IFrameSource Interface
//=============================================================================

// Unified interface for frame sources (image sequences, video decoders)
// Allows MultiCacheCoordinator to manage different source types uniformly
class IFrameSource {
public:
    virtual ~IFrameSource() = default;

    //=========================================================================
    // Lifecycle
    //=========================================================================

    virtual bool Initialize(const FrameSourceConfig& config) = 0;
    virtual void Shutdown() = 0;
    virtual bool IsInitialized() const = 0;

    //=========================================================================
    // Position & Playback State
    //=========================================================================

    // Update playhead position (seconds)
    virtual void UpdatePosition(double timestamp) = 0;

    // Set playback state
    virtual void SetPlaying(bool playing) = 0;

    // Loop control
    virtual void SetLoopRange(int in_frame, int out_frame) = 0;
    virtual void ClearLoopRange() = 0;
    virtual void SetLooping(bool enabled) = 0;

    //=========================================================================
    // Frame Access
    //=========================================================================

    // Get frame texture
    // Returns GL texture ID (0 if not available)
    // exact: set to true if this is the exact requested frame (not fallback)
    virtual GLuint GetFrame(int frame, int& width, int& height, bool& exact) = 0;

    // Check if specific frame is in cache
    virtual bool IsFrameCached(int frame) const = 0;

    // Process pending texture operations (must call from main thread with GL context)
    virtual void ProcessPendingOperations() = 0;

    //=========================================================================
    // Buffer Health (for statistics)
    //=========================================================================

    // Get current buffer health metrics
    virtual BufferHealth GetBufferHealth() const = 0;

    // Reset playback state (call on seek, pause)
    virtual void ResetPlaybackState() = 0;

    //=========================================================================
    // Source Metadata
    //=========================================================================

    virtual int GetTotalFrames() const = 0;
    virtual double GetFPS() const = 0;
    virtual bool GetDimensions(int& width, int& height) const = 0;
    virtual std::string GetSourceType() const = 0;  // "EXR", "TIFF", "Video", etc.
};

} // namespace ump
