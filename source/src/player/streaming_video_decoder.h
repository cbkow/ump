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
#include <set>

#include "video_decoder_interface.h"

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
// Streaming Video Decoder
//
// Provides real-time video frame access through continuous decode with buffering.
// Unlike VideoImageLoader (random access per frame), this class:
// - Continuously decodes frames in a background thread
// - Maintains a ring buffer of decoded frames
// - Provides instant frame access from buffer
// - Only seeks when target is outside buffer range
//=============================================================================

class StreamingVideoDecoder : public IVideoDecoder {
public:
    explicit StreamingVideoDecoder(const std::string& video_path);
    ~StreamingVideoDecoder() override;

    // Prevent copying (FFmpeg contexts are not copyable)
    StreamingVideoDecoder(const StreamingVideoDecoder&) = delete;
    StreamingVideoDecoder& operator=(const StreamingVideoDecoder&) = delete;

    //=========================================================================
    // IVideoDecoder Implementation - Lifecycle
    //=========================================================================

    bool Initialize() override;
    void Shutdown() override;
    void HardReset(int target_frame) override;
    bool IsInitialized() const override { return initialized_; }

    //=========================================================================
    // IVideoDecoder Implementation - Frame Access
    //=========================================================================

    std::shared_ptr<PixelData> GetFrame(int frame_number) override;
    std::shared_ptr<PixelData> GetClosestFrame(int frame_number, int* actual_frame = nullptr) override;
    bool HasFrame(int frame_number) const override;

    //=========================================================================
    // IVideoDecoder Implementation - Keyframe Access
    //=========================================================================

    std::shared_ptr<PixelData> GetKeyframe(int target_frame, int* actual_keyframe = nullptr) override;
    int GetNearestKeyframePosition(int target_frame) const override;
    bool IsIntraFrameCodec() const override { return is_intra_frame_codec_; }
    bool HasKeyframeIndex() const override { return keyframe_index_built_; }
    const std::vector<int>& GetKeyframePositions() const override { return keyframe_positions_; }

    //=========================================================================
    // IVideoDecoder Implementation - Playhead Management
    //=========================================================================

    void UpdatePlayhead(int frame_number, SeekQuality quality = SeekQuality::NORMAL, bool force_seek = false) override;

    //=========================================================================
    // IVideoDecoder Implementation - Demand-Driven Decode API
    //=========================================================================

    void SetNeededFrames(const std::vector<int>& frames_by_priority) override;
    DecodeStatus GetDecodeStatus() const override;
    void EvictOutsideWindow(const std::set<int>& keep_frames) override;
    std::set<int> GetBufferedFramesSet() const override;

    //=========================================================================
    // IVideoDecoder Implementation - Buffer Status
    //=========================================================================

    int GetBufferedAhead() const override;
    int GetBufferedBehind() const override;
    int GetBufferSize() const override;
    void GetBufferedRange(int& start_frame, int& end_frame) const override;
    void ClearBuffer() override;
    bool IsSeekPending() const override { return seek_requested_.load(); }
    int GetLastSeekTarget() const override { return seek_target_frame_; }

    //=========================================================================
    // IVideoDecoder Implementation - Metadata
    //=========================================================================

    int GetWidth() const override { return width_; }
    int GetHeight() const override { return height_; }
    double GetFPS() const override { return fps_; }
    double GetDuration() const override { return duration_; }
    int GetFrameCount() const override { return frame_count_; }
    const std::string& GetPath() const override { return video_path_; }

    //=========================================================================
    // IVideoDecoder Implementation - Hardware Acceleration
    //=========================================================================

    HWAccelType GetHWAccelType() const override { return hw_accel_type_; }
    bool IsHardwareAccelerated() const override { return hw_accel_type_ != HWAccelType::NONE; }

    //=========================================================================
    // IVideoDecoder Implementation - Configuration
    //=========================================================================

    void SetConfig(const StreamingDecoderConfig& config) override;
    const StreamingDecoderConfig& GetConfig() const override { return config_; }
    void SetPipelineMode(PipelineMode mode) override;
    PipelineMode GetPipelineMode() const override { return pipeline_mode_; }
    void SetShuttleMode(bool enabled) override { shuttle_mode_ = enabled; }
    bool IsShuttleMode() const override { return shuttle_mode_.load(); }

    //=========================================================================
    // IVideoDecoder Implementation - Looping
    //=========================================================================

    void SetLoopPoints(int start_frame, int end_frame) override;
    void ClearLoopPoints() override;

    //=========================================================================
    // IVideoDecoder Implementation - Backend Identification
    //=========================================================================

    const char* GetBackendName() const override { return "FFmpeg"; }
    VideoDecoderBackend GetBackendType() const override { return VideoDecoderBackend::FFMPEG; }

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

    // Calculate frame number from AVFrame's PTS
    // Returns -1 if PTS is not available
    int FrameNumberFromPTS(::AVFrame* frame) const;

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
    // Keyframe Index
    //=========================================================================

    // Build keyframe index by scanning the video file
    // Called automatically during Initialize()
    void BuildKeyframeIndex();

    // Decode a single keyframe directly (without filling buffer)
    // Used for scrubbing - decodes just the keyframe, no GOP traversal
    std::shared_ptr<PixelData> DecodeSingleKeyframe(int keyframe_number);

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

    // Pipeline mode
    PipelineMode pipeline_mode_ = PipelineMode::NORMAL;

    // Hardware acceleration state
    HWAccelType hw_accel_type_ = HWAccelType::NONE;
    ::AVBufferRef* hw_device_ctx_ = nullptr;
    int hw_pix_fmt_ = -1;  // AVPixelFormat, -1 = AV_PIX_FMT_NONE
    bool uses_shared_hw_ctx_ = false;  // True if using shared context from HWContextManager

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

    // Shuttle mode - decode as fast as possible without window throttling
    std::atomic<bool> shuttle_mode_{false};

    //=========================================================================
    // Keyframe Index
    //=========================================================================

    std::vector<int> keyframe_positions_;      // Frame numbers of all keyframes
    bool keyframe_index_built_ = false;        // True after BuildKeyframeIndex() completes
    bool is_intra_frame_codec_ = false;        // True for ProRes, DNxHD, MJPEG (every frame is keyframe)
    mutable std::mutex keyframe_mutex_;        // Protects keyframe access during scrubbing

    //=========================================================================
    // Demand-Driven Decode State
    //=========================================================================

    std::vector<int> needed_frames_;           // Frames needed, in priority order
    mutable std::mutex needed_mutex_;          // Protects needed_frames_
    std::atomic<int> currently_decoding_{-1};  // Frame currently being decoded

    //=========================================================================
    // Looping State
    //=========================================================================

    bool loop_enabled_ = false;
    int loop_start_frame_ = 0;
    int loop_end_frame_ = 0;
};

} // namespace ump
