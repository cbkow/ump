#pragma once

#include <string>
#include <vector>
#include <memory>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <deque>
#include <unordered_set>

#include "image_loader_interface.h"

// Forward declarations for FFmpeg types (in global namespace)
struct AVFormatContext;
struct AVCodecContext;
struct AVCodec;
struct AVFrame;
struct AVPacket;
struct SwsContext;
struct AVBufferRef;

namespace ump {

//=============================================================================
// Hardware Acceleration Type
//=============================================================================

enum class HWAccelType {
    NONE,           // Software decode
    CUDA,           // NVIDIA NVDEC via CUDA
    D3D11VA,        // Direct3D 11 Video Acceleration (NVIDIA/Intel/AMD)
    QSV,            // Intel Quick Sync Video
    DXVA2           // Direct3D 9 Video Acceleration (legacy fallback)
};

// Convert HWAccelType to string for logging
inline const char* HWAccelTypeToString(HWAccelType type) {
    switch (type) {
        case HWAccelType::CUDA: return "CUDA (NVDEC)";
        case HWAccelType::D3D11VA: return "D3D11VA";
        case HWAccelType::QSV: return "Quick Sync";
        case HWAccelType::DXVA2: return "DXVA2";
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
// Streaming Video Decoder Configuration
//=============================================================================

struct StreamingDecoderConfig {
    int readAheadFrames = 120;     // ~5 seconds @ 24fps (larger buffer for 4K)
    int readBehindFrames = 48;     // ~2 seconds for backward scrub
    bool useHardwareAccel = true;  // Enable hardware decode by default
    int decodeThreads = 4;         // FFmpeg internal decode threads (for software fallback)

    // Tuning parameters
    int minBufferBeforePlay = 24;  // Min frames buffered before considering "ready"
};

//=============================================================================
// Streaming Video Decoder
//
// Provides real-time video frame access through continuous decode with buffering.
// Unlike VideoImageLoader (random access per frame), this class:
// - Continuously decodes frames in a background thread
// - Maintains a ring buffer of decoded frames
// - Provides instant frame access from buffer
// - Only seeks when target is outside buffer range
//=============================================================================

class StreamingVideoDecoder {
public:
    explicit StreamingVideoDecoder(const std::string& video_path);
    ~StreamingVideoDecoder();

    // Prevent copying (FFmpeg contexts are not copyable)
    StreamingVideoDecoder(const StreamingVideoDecoder&) = delete;
    StreamingVideoDecoder& operator=(const StreamingVideoDecoder&) = delete;

    //=========================================================================
    // Lifecycle
    //=========================================================================

    // Initialize decoder and start decode thread
    // Returns true on success
    bool Initialize();

    // Stop decode thread and release resources
    void Shutdown();

    // Hard reset: close and reopen video file to recover from bad state
    // Use when soft reset (FlushAndSeek) doesn't work
    // Thread-safe: can be called from any thread
    void HardReset(int target_frame);

    // Check if decoder is ready
    bool IsInitialized() const { return initialized_; }

    //=========================================================================
    // Frame Access
    //=========================================================================

    // Get frame from buffer (instant if buffered)
    // Returns nullptr if frame not yet decoded
    // Thread-safe: can be called from any thread
    std::shared_ptr<PixelData> GetFrame(int frame_number);

    // Get closest frame from buffer when exact frame not available
    // Useful during scrubbing to show something rather than nothing
    // Returns nullptr if buffer is empty
    // Thread-safe
    std::shared_ptr<PixelData> GetClosestFrame(int frame_number, int* actual_frame = nullptr);

    // Check if frame is available in buffer
    // Thread-safe
    bool HasFrame(int frame_number) const;

    // Update playhead position
    // Triggers seek if frame is outside buffer range
    // quality: PREVIEW for fast scrubbing, NORMAL for full quality
    // force_seek: bypass all tolerance checks and seek immediately (for refinement)
    // Thread-safe
    void UpdatePlayhead(int frame_number, SeekQuality quality = SeekQuality::NORMAL, bool force_seek = false);

    //=========================================================================
    // Buffer Status (for UI/debugging)
    //=========================================================================

    // Get number of frames buffered ahead of playhead
    int GetBufferedAhead() const;

    // Get number of frames buffered behind playhead
    int GetBufferedBehind() const;

    // Get total buffer size
    int GetBufferSize() const;

    // Get range of buffered frames [start, end]
    void GetBufferedRange(int& start_frame, int& end_frame) const;

    // Clear the frame buffer (free memory for inactive decoders)
    // Thread-safe: can be called from any thread
    void ClearBuffer();

    //=========================================================================
    // Metadata
    //=========================================================================

    int GetWidth() const { return width_; }
    int GetHeight() const { return height_; }
    double GetFPS() const { return fps_; }
    double GetDuration() const { return duration_; }
    int GetFrameCount() const { return frame_count_; }
    const std::string& GetPath() const { return video_path_; }

    // Hardware acceleration info
    HWAccelType GetHWAccelType() const { return hw_accel_type_; }
    bool IsHardwareAccelerated() const { return hw_accel_type_ != HWAccelType::NONE; }

    //=========================================================================
    // Configuration
    //=========================================================================

    void SetConfig(const StreamingDecoderConfig& config);
    const StreamingDecoderConfig& GetConfig() const { return config_; }

private:
    //=========================================================================
    // Decode Thread
    //=========================================================================

    // Main decode loop (runs in decode_thread_)
    void DecodeThread();

    // Handle seek request (called from decode thread)
    void FlushAndSeek(int target_frame, SeekQuality quality = SeekQuality::NORMAL);

    // Decode next frame from stream
    // Returns true if frame decoded, false on EOF or error
    bool DecodeNextFrame(::AVFrame* frame);

    // Convert AVFrame to PixelData
    std::shared_ptr<PixelData> ConvertToPixelData(::AVFrame* frame);

    //=========================================================================
    // Ring Buffer Management
    //=========================================================================

    struct BufferedFrame {
        int frame_number = -1;
        std::shared_ptr<PixelData> pixels;
    };

    // Add frame to buffer (called from decode thread)
    void AddToBuffer(int frame_number, std::shared_ptr<PixelData> pixels);

    // Find frame in buffer
    // Returns nullptr if not found
    std::shared_ptr<PixelData> FindInBuffer(int frame_number) const;

    // Check if buffer needs more frames ahead
    bool NeedsMoreFrames() const;

    //=========================================================================
    // FFmpeg Initialization
    //=========================================================================

    bool OpenVideo();
    void CloseVideo();

    //=========================================================================
    // Hardware Acceleration
    //=========================================================================

    // Try to initialize hardware decode (returns true if successful)
    // hw_type is AVHWDeviceType cast to int
    bool TryHardwareAccel(const AVCodec* codec, int hw_type);

    // Transfer hardware frame to CPU memory
    bool TransferHWFrame(::AVFrame* hw_frame, ::AVFrame* sw_frame);

    //=========================================================================
    // State
    //=========================================================================

    bool initialized_ = false;
    StreamingDecoderConfig config_;

    // Video metadata
    std::string video_path_;
    int width_ = 0;
    int height_ = 0;
    double fps_ = 0.0;
    double duration_ = 0.0;
    int frame_count_ = 0;
    int64_t start_time_ = 0;  // Container start time offset

    // FFmpeg contexts
    ::AVFormatContext* format_ctx_ = nullptr;
    ::AVCodecContext* codec_ctx_ = nullptr;
    int video_stream_idx_ = -1;

    // Color conversion (cached)
    ::SwsContext* sws_ctx_ = nullptr;
    int sws_src_width_ = 0;
    int sws_src_height_ = 0;
    int sws_src_format_ = -1;

    // Hardware acceleration state
    HWAccelType hw_accel_type_ = HWAccelType::NONE;
    ::AVBufferRef* hw_device_ctx_ = nullptr;
    int hw_pix_fmt_ = -1;  // AVPixelFormat, -1 = AV_PIX_FMT_NONE

    //=========================================================================
    // Ring Buffer
    //=========================================================================

    std::deque<BufferedFrame> ring_buffer_;
    std::unordered_set<int> buffer_frame_set_;  // O(1) duplicate check
    mutable std::mutex buffer_mutex_;

    // Fast O(1) buffer tracking (avoid iterating buffer for NeedsMoreFrames)
    std::atomic<int> buffer_ahead_count_{0};  // Frames ahead of playhead
    std::atomic<int> buffer_size_{0};         // Total frames in buffer

    // Current playhead position (set by UpdatePlayhead)
    std::atomic<int> playhead_frame_{0};

    // Current decode position (incremented by decode thread)
    int decode_frame_ = 0;

    // Debug logging counter (per-instance)
    mutable int miss_log_count_ = 0;

    //=========================================================================
    // Threading
    //=========================================================================

    std::thread decode_thread_;
    std::atomic<bool> running_{false};

    // Seek coordination
    std::mutex seek_mutex_;
    std::condition_variable seek_cv_;
    std::atomic<bool> seek_requested_{false};
    int seek_target_frame_ = 0;
    SeekQuality seek_quality_ = SeekQuality::NORMAL;  // Quality for next seek
    std::chrono::steady_clock::time_point last_preview_seek_time_;  // Rate limiting for scrub seeks

    // Decode pacing
    std::condition_variable decode_cv_;

    // EOF state - prevents spin loop after reaching end of video
    std::atomic<bool> eof_reached_{false};
};

} // namespace ump
