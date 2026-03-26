#pragma once

#include <string>
#include <vector>
#include <memory>
#include <set>

#include "image_loader_interface.h"
#include "pipeline_mode.h"

namespace qcview {

//=============================================================================
// Hardware Acceleration Type
//=============================================================================

enum class HWAccelType {
    NONE,           // Software decode
    CUDA,           // NVIDIA NVDEC via CUDA
    D3D11VA,        // Direct3D 11 Video Acceleration (NVIDIA/Intel/AMD)
    QSV,            // Intel Quick Sync Video
    DXVA2,          // Direct3D 9 Video Acceleration (legacy fallback)
    VAAPI,          // Video Acceleration API (Linux)
    VIDEOTOOLBOX    // VideoToolbox (macOS)
};

// Convert HWAccelType to string for logging
inline const char* HWAccelTypeToString(HWAccelType type) {
    switch (type) {
        case HWAccelType::CUDA: return "CUDA (NVDEC)";
        case HWAccelType::D3D11VA: return "D3D11VA";
        case HWAccelType::QSV: return "Quick Sync";
        case HWAccelType::DXVA2: return "DXVA2";
        case HWAccelType::VAAPI: return "VAAPI";
        case HWAccelType::VIDEOTOOLBOX: return "VideoToolbox";
        default: return "Software";
    }
}

//=============================================================================
// Seek Quality Mode (for adaptive scrubbing)
//=============================================================================

enum class SeekQuality {
    PREVIEW,    // Fast: skip B-frames, skip loop filter (for scrubbing)
    NORMAL      // Full quality decode (current behavior)
};

//=============================================================================
// Video Decoder Backend Type
//=============================================================================

enum class VideoDecoderBackend {
    AUTO,       // Auto-select best available
    FFMPEG,     // Force FFmpeg backend (CPU ring buffer)
    D3D11,      // D3D11 GPU-native backend (Windows only)
    VULKAN,     // Vulkan backend (Linux only)
    METAL       // Metal/VideoToolbox backend (macOS only)
};

inline const char* VideoDecoderBackendToString(VideoDecoderBackend backend) {
    switch (backend) {
        case VideoDecoderBackend::FFMPEG: return "FFmpeg";
        case VideoDecoderBackend::D3D11: return "D3D11";
        case VideoDecoderBackend::VULKAN: return "Vulkan";
        case VideoDecoderBackend::METAL: return "Metal";
        default: return "Auto";
    }
}

//=============================================================================
// Streaming Video Decoder Configuration
//=============================================================================

struct StreamingDecoderConfig {
    int readAheadFrames = 108;     // ~4.5 seconds @ 24fps
    int readBehindFrames = 12;     // ~0.5 seconds for backward scrub
    bool useHardwareAccel = true;  // Enable hardware acceleration
    int decodeThreads = 4;         // Decode threads (for software fallback)
    bool preferD3D11VA = false;    // Prefer D3D11VA for zero-copy interop

    // Tuning parameters
    int minBufferBeforePlay = 24;  // Min frames buffered before considering "ready"
};

//=============================================================================
// Decode Status (for demand-driven decode API)
//=============================================================================

struct DecodeStatus {
    std::vector<int> have;              // Frames currently buffered
    std::vector<int> missing;           // Frames needed but not buffered, in priority order
    int currently_decoding = -1;        // Frame being decoded right now (-1 if idle)
};

//=============================================================================
// Abstract Video Decoder Interface
//
// Common interface for video decoding backends (FFmpeg, D3D11, etc.)
// Implementations must provide:
// - Ring buffer frame caching
// - Background decode thread
// - Keyframe handling for inter-frame codecs
// - Hardware acceleration support
//=============================================================================

class IVideoDecoder {
public:
    virtual ~IVideoDecoder() = default;

    //=========================================================================
    // Lifecycle
    //=========================================================================

    // Initialize decoder and start decode thread
    // Returns true on success
    virtual bool Initialize() = 0;

    // Stop decode thread and release resources
    virtual void Shutdown() = 0;

    // Signal decode thread to stop without waiting for it to join.
    // Call this on multiple decoders before destroying them to allow parallel exit.
    // Default implementation calls Shutdown() for decoders that don't override.
    virtual void RequestShutdown() { Shutdown(); }

    // Hard reset: close and reopen to recover from bad state
    // Thread-safe: can be called from any thread
    virtual void HardReset(int target_frame) = 0;

    // Check if decoder is ready
    virtual bool IsInitialized() const = 0;

    //=========================================================================
    // Frame Access
    //=========================================================================

    // Get frame from buffer (instant if buffered)
    // Returns nullptr if frame not yet decoded
    // Thread-safe: can be called from any thread
    virtual std::shared_ptr<PixelData> GetFrame(int frame_number) = 0;

    // Get closest frame from buffer when exact frame not available
    // Useful during scrubbing to show something rather than nothing
    // Returns nullptr if buffer is empty
    // Thread-safe
    virtual std::shared_ptr<PixelData> GetClosestFrame(int frame_number, int* actual_frame = nullptr) = 0;

    // Check if frame is available in buffer
    // Thread-safe
    virtual bool HasFrame(int frame_number) const = 0;

    //=========================================================================
    // Keyframe Access (for H.264/inter-frame codec scrubbing)
    //=========================================================================

    // Get the nearest keyframe at or before the target frame
    // For intra-frame codecs (ProRes, DNxHD), returns target_frame itself
    // Thread-safe
    virtual std::shared_ptr<PixelData> GetKeyframe(int target_frame, int* actual_keyframe = nullptr) = 0;

    // Get the frame number of the nearest keyframe at or before target
    // Returns target_frame if keyframe index not built or codec is all-intra
    virtual int GetNearestKeyframePosition(int target_frame) const = 0;

    // Check if the video is an intra-frame codec (every frame is a keyframe)
    // ProRes, DNxHD, MJPEG, etc. return true
    virtual bool IsIntraFrameCodec() const = 0;

    // Check if keyframe index has been built
    virtual bool HasKeyframeIndex() const = 0;

    // Get all keyframe positions (for debugging/visualization)
    virtual const std::vector<int>& GetKeyframePositions() const = 0;

    //=========================================================================
    // Playhead Management
    //=========================================================================

    // Update playhead position
    // Triggers seek if frame is outside buffer range
    // quality: PREVIEW for fast scrubbing, NORMAL for full quality
    // force_seek: bypass all tolerance checks and seek immediately (for refinement)
    // Thread-safe
    virtual void UpdatePlayhead(int frame_number, SeekQuality quality = SeekQuality::NORMAL, bool force_seek = false) = 0;

    //=========================================================================
    // Demand-Driven Decode API (for CacheWindowEngine integration)
    //=========================================================================

    // Set which frames are needed, in priority order
    // The decoder will decode missing frames in this order
    // Replaces UpdatePlayhead for decode decisions
    virtual void SetNeededFrames(const std::vector<int>& frames_by_priority) = 0;

    // Report current decode status
    virtual DecodeStatus GetDecodeStatus() const = 0;

    // Evict frames outside the given keep set
    // Simple eviction: anything not in keep_frames gets removed
    virtual void EvictOutsideWindow(const std::set<int>& keep_frames) = 0;

    // Get frames currently in buffer as a set (for quick lookup)
    virtual std::set<int> GetBufferedFramesSet() const = 0;

    //=========================================================================
    // Buffer Status (for UI/debugging)
    //=========================================================================

    // Get number of frames buffered ahead of playhead
    virtual int GetBufferedAhead() const = 0;

    // Get number of frames buffered behind playhead
    virtual int GetBufferedBehind() const = 0;

    // Get total buffer size
    virtual int GetBufferSize() const = 0;

    // Get range of buffered frames [start, end]
    virtual void GetBufferedRange(int& start_frame, int& end_frame) const = 0;

    // Clear the frame buffer (free memory for inactive decoders)
    // Thread-safe: can be called from any thread
    virtual void ClearBuffer() = 0;

    // Check if a seek is currently in progress (requested but not yet completed)
    // When true, buffer contents may be stale/invalid
    virtual bool IsSeekPending() const = 0;

    // Get the last seek target frame (for debugging/validation)
    virtual int GetLastSeekTarget() const = 0;

    //=========================================================================
    // Metadata
    //=========================================================================

    virtual int GetWidth() const = 0;
    virtual int GetHeight() const = 0;
    virtual double GetFPS() const = 0;
    virtual double GetDuration() const = 0;
    virtual int GetFrameCount() const = 0;
    virtual const std::string& GetPath() const = 0;

    //=========================================================================
    // Hardware Acceleration
    //=========================================================================

    virtual HWAccelType GetHWAccelType() const = 0;
    virtual bool IsHardwareAccelerated() const = 0;

    //=========================================================================
    // Configuration
    //=========================================================================

    virtual void SetConfig(const StreamingDecoderConfig& config) = 0;
    virtual const StreamingDecoderConfig& GetConfig() const = 0;

    // Pipeline mode (for bit depth / HDR support)
    virtual void SetPipelineMode(PipelineMode mode) = 0;
    virtual PipelineMode GetPipelineMode() const = 0;

    // Shuttle mode - disables window-based decode throttling
    // When enabled, decoder runs as fast as possible without waiting
    virtual void SetShuttleMode(bool enabled) = 0;
    virtual bool IsShuttleMode() const = 0;

    //=========================================================================
    // Looping
    //=========================================================================

    // Set loop points for seamless playback looping
    // start_frame/end_frame are in source frame numbers
    // When enabled, decoder will automatically seek back to start when reaching end
    virtual void SetLoopPoints(int start_frame, int end_frame) = 0;
    virtual void ClearLoopPoints() = 0;

    //=========================================================================
    // Backend Identification
    //=========================================================================

    // Get the name of the decoder backend (e.g., "FFmpeg", "D3D11")
    virtual const char* GetBackendName() const = 0;

    // Get the backend type enum
    virtual VideoDecoderBackend GetBackendType() const = 0;
};

} // namespace qcview
