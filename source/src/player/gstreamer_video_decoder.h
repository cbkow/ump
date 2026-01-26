#pragma once

#ifdef WITH_GSTREAMER

#include <string>
#include <vector>
#include <memory>
#include <thread>
#include <mutex>
#include <chrono>
#include <condition_variable>
#include <atomic>
#include <set>
#include <deque>
#include <unordered_set>

#include "video_decoder_interface.h"

// Forward declarations for GStreamer types
typedef struct _GstElement GstElement;
typedef struct _GstSample GstSample;

namespace ump {

//=============================================================================
// GStreamer Video Decoder with Simple Ring Buffer
//
// - GStreamer handles decoding via appsink (pull-based)
// - We maintain a ring buffer of decoded frames for instant access
// - App controls looping via UpdatePlayhead - no wrap-around logic here
//=============================================================================

class GStreamerVideoDecoder : public IVideoDecoder {
public:
    explicit GStreamerVideoDecoder(const std::string& video_path);
    ~GStreamerVideoDecoder() override;

    // Prevent copying
    GStreamerVideoDecoder(const GStreamerVideoDecoder&) = delete;
    GStreamerVideoDecoder& operator=(const GStreamerVideoDecoder&) = delete;

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
    bool IsIntraFrameCodec() const override { return true; }  // Simplified - treat all as intra
    bool HasKeyframeIndex() const override { return false; }
    const std::vector<int>& GetKeyframePositions() const override { return empty_keyframes_; }

    //=========================================================================
    // IVideoDecoder Implementation - Playhead Management
    //=========================================================================

    void UpdatePlayhead(int frame_number, SeekQuality quality = SeekQuality::NORMAL, bool force_seek = false) override;

    //=========================================================================
    // IVideoDecoder Implementation - Looping (app handles logic, we just store hints)
    //=========================================================================

    void SetLoopPoints(int start_frame, int end_frame) override;
    void ClearLoopPoints() override;

    //=========================================================================
    // IVideoDecoder Implementation - Buffer Status
    //=========================================================================

    int GetBufferedAhead() const override;
    int GetBufferedBehind() const override;
    int GetBufferSize() const override;
    void GetBufferedRange(int& start_frame, int& end_frame) const override;
    void ClearBuffer() override;
    bool IsSeekPending() const override { return seek_pending_.load(); }
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

    void SetConfig(const StreamingDecoderConfig& config) override { config_ = config; }
    const StreamingDecoderConfig& GetConfig() const override { return config_; }
    void SetPipelineMode(PipelineMode mode) override;  // Implemented in .cpp

    // H.264/H.265 PTS sync: Set stream start time offset from container
    // This is extracted from AVStream->start_time by FFprobe and stored in MediaItem
    // Must be called BEFORE Initialize() for correct frame/timestamp mapping
    void SetStreamStartTime(int64_t start_time_us) { stream_start_time_ns_ = start_time_us * 1000; }
    int64_t GetStreamStartTime() const { return stream_start_time_ns_ / 1000; }

    // Set B-frame codec flag (H.264/H.265) - enables first-frame PTS baseline capture
    // Can be called BEFORE Initialize() if codec is known from metadata
    void SetBFrameCodec(bool is_bframe) { is_bframe_codec_ = is_bframe; }
    bool IsBFrameCodec() const { return is_bframe_codec_; }
    PipelineMode GetPipelineMode() const override { return pipeline_mode_; }
    void SetShuttleMode(bool enabled) override { shuttle_mode_ = enabled; }
    bool IsShuttleMode() const override { return shuttle_mode_.load(); }

    //=========================================================================
    // IVideoDecoder Implementation - Backend Identification
    //=========================================================================

    const char* GetBackendName() const override { return "GStreamer"; }
    VideoDecoderBackend GetBackendType() const override { return VideoDecoderBackend::GSTREAMER; }

    //=========================================================================
    // Demand-Driven Decode API
    //=========================================================================

    void SetNeededFrames(const std::vector<int>& frames_by_priority) override {}
    DecodeStatus GetDecodeStatus() const override;
    void EvictOutsideWindow(const std::set<int>& keep_frames) override;
    std::set<int> GetBufferedFramesSet() const override;

private:
    //=========================================================================
    // GStreamer Pipeline
    //=========================================================================

    bool BuildPipeline();
    void DestroyPipeline();
    bool ConfigureAppSink();
    bool ExtractMetadata();
    void DetectHardwareAcceleration();
    void ProbeStreamStartTime();  // FFmpeg probe for H.264/H.265 PTS sync
    void LogNegotiatedCaps();  // Debug: log actual negotiated caps on appsink

    //=========================================================================
    // Frame Pulling from GStreamer
    //=========================================================================

    std::shared_ptr<PixelData> PullNextFrame(int* out_frame_number = nullptr);
    std::shared_ptr<PixelData> ConvertSampleToPixelData(GstSample* sample);
    int FrameNumberFromTimestamp(int64_t timestamp_ns) const;

    //=========================================================================
    // Seeking
    //=========================================================================

    bool SeekToFrame(int target_frame);
    bool SeekToTime(int64_t timestamp_ns);

    //=========================================================================
    // Decode Thread
    //=========================================================================

    void DecodeThread();

    //=========================================================================
    // Ring Buffer Management
    //=========================================================================

    struct BufferedFrame {
        int frame_number = -1;
        std::shared_ptr<PixelData> pixels;
    };

    // Add frame to buffer (called from decode thread)
    void AddToBuffer(int frame_number, std::shared_ptr<PixelData> pixels);

    // Find exact frame in buffer
    std::shared_ptr<PixelData> FindInBuffer(int frame_number) const;

    // Find closest frame in buffer (for scrubbing fallback)
    std::shared_ptr<PixelData> FindClosestInBuffer(int frame_number, int* actual_frame = nullptr) const;

    // Check if we need more frames ahead of playhead
    bool NeedsMoreFramesAhead() const;

    // Evict frames outside the [playhead - behind, playhead + ahead] window
    void EvictOutsideWindow();

    // Update buffer counts (ahead/behind relative to playhead)
    void UpdateBufferCounts();

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

    // H.264/H.265 PTS sync: stream start time offset (in nanoseconds for GStreamer)
    // Set from MediaItem::stream_start_time (which is in AV_TIME_BASE microseconds)
    // Updated to first frame's actual PTS if larger (B-frame reorder delay)
    int64_t stream_start_time_ns_ = 0;
    int64_t first_pts_ns_ = 0;         // First frame's actual PTS (captures B-frame delay)
    bool first_pts_captured_ = false;  // Whether we've seen the first frame
    bool is_bframe_codec_ = false;     // True for H.264/H.265 (codecs with B-frame reorder delay)
    int bframe_delay_ = 2;             // B-frame reorder delay in frames (from FFmpeg video_delay)

    // Playback
    PipelineMode pipeline_mode_ = PipelineMode::NORMAL;
    HWAccelType hw_accel_type_ = HWAccelType::NONE;

    //=========================================================================
    // GStreamer Pipeline Elements
    //=========================================================================

    GstElement* pipeline_ = nullptr;
    GstElement* appsink_ = nullptr;
    GstElement* filesrc_ = nullptr;
    GstElement* decodebin_ = nullptr;
    GstElement* videoconvert_ = nullptr;
    GstElement* capsfilter_ = nullptr;     // For setting output format

    //=========================================================================
    // Ring Buffer (simple sorted deque, no circular wrap-around)
    //=========================================================================

    std::deque<BufferedFrame> ring_buffer_;
    std::unordered_set<int> buffer_frame_set_;  // O(1) lookup for "do we have frame N?"
    mutable std::mutex buffer_mutex_;

    // Buffer tracking
    std::atomic<int> buffer_ahead_count_{0};   // Frames ahead of playhead
    std::atomic<int> buffer_behind_count_{0};  // Frames behind playhead

    // Last displayed frame (fallback during seek transitions)
    std::shared_ptr<PixelData> last_displayed_frame_;
    int last_displayed_frame_number_ = -1;

    //=========================================================================
    // Loop Points (hints only - app handles actual looping via UpdatePlayhead)
    //=========================================================================

    int loop_start_frame_ = 0;
    int loop_end_frame_ = -1;  // -1 = not set

    //=========================================================================
    // Threading
    //=========================================================================

    std::thread decode_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> eof_reached_{false};
    std::atomic<bool> shuttle_mode_{false};

    // Decode thread coordination
    std::mutex decode_mutex_;
    std::condition_variable decode_cv_;

    // Seek state
    std::mutex seek_mutex_;
    std::atomic<bool> seek_pending_{false};
    int seek_target_frame_ = 0;

    // Seek debouncing - prevent rapid duplicate seeks to same frame
    int last_seek_requested_ = -1;
    std::chrono::steady_clock::time_point last_seek_time_;

    // Playhead position (set by UpdatePlayhead, used by decode thread)
    std::atomic<int> playhead_frame_{0};

    // Current decode position (where GStreamer is decoding)
    int decode_frame_ = 0;

    // Empty vector for interface compliance
    std::vector<int> empty_keyframes_;
};

} // namespace ump

#endif // WITH_GSTREAMER
