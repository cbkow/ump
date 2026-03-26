#pragma once

#ifdef __APPLE__

#include "video_decoder_interface.h"
#include "frame_index.h"
#include "../gpu/metal_hwframe_extractor.h"
#include "../gpu/metal_yuv_renderer.h"

#include <string>
#include <vector>
#include <array>
#include <set>
#include <memory>
#include <mutex>
#include <atomic>
#include <thread>
#include <condition_variable>
#include <unordered_map>

// Forward declarations
struct AVFormatContext;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct AVBufferRef;

namespace qcview {

//=============================================================================
// MetalVideoDecoder
//
// VideoToolbox HW-accelerated video decoder for macOS.
// Implements IVideoDecoder with:
//   - HW path: FFmpeg VideoToolbox -> CVPixelBuffer -> MTLTexture (zero-copy)
//   - SW path: FFmpeg CPU decode -> sws_scale -> PixelData
//   - Ring buffer frame caching (same pattern as Vulkan/D3D11 decoders)
//   - Background decode thread
//   - Keyframe index for inter-frame codec scrubbing
//=============================================================================

class MetalVideoDecoder : public IVideoDecoder {
public:
    MetalVideoDecoder(const std::string& file_path, const StreamingDecoderConfig& config = {});
    ~MetalVideoDecoder() override;

    //=========================================================================
    // IVideoDecoder interface
    //=========================================================================

    bool Initialize() override;
    void Shutdown() override;
    void RequestShutdown() override;
    void HardReset(int target_frame) override;
    bool IsInitialized() const override { return initialized_.load(); }

    std::shared_ptr<PixelData> GetFrame(int frame_number) override;
    std::shared_ptr<PixelData> GetClosestFrame(int frame_number, int* actual_frame = nullptr) override;
    bool HasFrame(int frame_number) const override;

    std::shared_ptr<PixelData> GetKeyframe(int target_frame, int* actual_keyframe = nullptr) override;
    int GetNearestKeyframePosition(int target_frame) const override;
    bool IsIntraFrameCodec() const override { return is_intra_codec_; }
    bool HasKeyframeIndex() const override;
    const std::vector<int>& GetKeyframePositions() const override;

    void UpdatePlayhead(int frame_number, SeekQuality quality = SeekQuality::NORMAL, bool force_seek = false) override;

    void SetNeededFrames(const std::vector<int>& frames_by_priority) override;
    DecodeStatus GetDecodeStatus() const override;
    void EvictOutsideWindow(const std::set<int>& keep_frames) override;
    std::set<int> GetBufferedFramesSet() const override;

    int GetBufferedAhead() const override;
    int GetBufferedBehind() const override;
    int GetBufferSize() const override;
    void GetBufferedRange(int& start_frame, int& end_frame) const override;
    void ClearBuffer() override;
    bool IsSeekPending() const override { return seek_pending_.load(); }
    int GetLastSeekTarget() const override { return last_seek_target_.load(); }

    int GetWidth() const override { return width_; }
    int GetHeight() const override { return height_; }
    double GetFPS() const override { return fps_; }
    double GetDuration() const override { return duration_; }
    int GetFrameCount() const override { return frame_count_; }
    const std::string& GetPath() const override { return file_path_; }

    HWAccelType GetHWAccelType() const override { return hw_accel_type_; }
    bool IsHardwareAccelerated() const override { return hw_accel_type_ != HWAccelType::NONE; }

    void SetConfig(const StreamingDecoderConfig& config) override { config_ = config; }
    const StreamingDecoderConfig& GetConfig() const override { return config_; }

    void SetPipelineMode(PipelineMode mode) override { pipeline_mode_ = mode; }
    PipelineMode GetPipelineMode() const override { return pipeline_mode_; }

    void SetShuttleMode(bool enabled) override { shuttle_mode_ = enabled; }
    bool IsShuttleMode() const override { return shuttle_mode_; }

    // Play/pause state — controls prefetch behavior
    void SetPlaying(bool playing);
    bool IsPlaying() const { return playing_.load(std::memory_order_acquire); }

    void SetLoopPoints(int start_frame, int end_frame) override;
    void ClearLoopPoints() override;

    const char* GetBackendName() const override { return "Metal/VideoToolbox"; }
    VideoDecoderBackend GetBackendType() const override { return VideoDecoderBackend::AUTO; }

    //=========================================================================
    // Video Range Override
    //=========================================================================

    void SetVideoRangeOverride(VideoRangeMode mode) { video_range_override_ = mode; }
    VideoRangeMode GetVideoRangeOverride() const { return video_range_override_; }

    // Get effective full range considering override
    // AUTO mode uses detected range; FULL/LIMITED override detection
    bool GetEffectiveFullRange() const {
        switch (video_range_override_) {
            case VideoRangeMode::FULL: return true;
            case VideoRangeMode::LIMITED: return false;
            default: return is_full_range_;
        }
    }

    //=========================================================================
    // Metal-specific: GPU texture pool path (zero-copy)
    //=========================================================================

    // Returns a MetalTexturePool ID for the given frame.
    // Called from the main thread. ZERO GPU work — just a pool lookup + Touch.
    // All GPU conversion is done on the decode thread in AddCurrentFrameToBuffer.
    uint64_t GetFrameAsPoolTexture(int frame_number);

private:
    // Ring buffer entry — GPU-ready pool texture (all GPU work done on decode thread)
    struct BufferedFrame {
        int frame_number = -1;
        bool valid = false;
        uint64_t pool_texture_id = 0;            // MetalTexturePool ID (GPU-ready)
        bool gpu_uploaded = false;               // true = pool_texture_id is valid
        std::shared_ptr<PixelData> pixel_data;   // Legacy fallback only (no GPU pipeline)
        void Reset();
    };

    static constexpr int kFrameBufferSize = 16;
    static constexpr int kDelayQueueDepth = 4;  // B-frame reorder fill depth (matches D3D11/Vulkan)

    // FFmpeg setup
    bool OpenFile();
    bool SetupHWAccel();

    // Decode thread
    void DecodeThreadFunc();
    bool NeedsMoreFrames() const;
    bool DecodeOneFrame(AVFrame* frame, AVPacket* packet);
    void AddCurrentFrameToBuffer(AVFrame* frame, int frame_num);
    std::shared_ptr<PixelData> ConvertFrame(AVFrame* frame);

    // File info
    std::string file_path_;
    int width_ = 0;
    int height_ = 0;
    double fps_ = 0.0;
    double duration_ = 0.0;
    int frame_count_ = 0;

    // FFmpeg contexts
    AVFormatContext* format_ctx_ = nullptr;
    AVCodecContext* codec_ctx_ = nullptr;
    int video_stream_idx_ = -1;
    AVBufferRef* hw_device_ctx_ = nullptr;
    void* sws_ctx_ = nullptr;  // Cached SwsContext* (reused across frames, freed in Shutdown)

    // Hardware acceleration
    HWAccelType hw_accel_type_ = HWAccelType::NONE;
    std::unique_ptr<MetalHWFrameExtractor> hw_extractor_;

    // GPU conversion pipeline
    std::unique_ptr<MetalYUVRenderer> yuv_renderer_;
    bool gpu_decode_available_ = false;
    bool is_hdr_ = false;
    bool is_10bit_ = false;
    bool is_full_range_ = false;
    bool is_bt2020_ = false;
    VideoRangeMode video_range_override_ = VideoRangeMode::AUTO;

    // Config
    StreamingDecoderConfig config_;
    PipelineMode pipeline_mode_ = PipelineMode::NORMAL;
    bool shuttle_mode_ = false;
    std::atomic<bool> playing_{false};

    // Ring buffer frame storage (FIFO eviction, matches D3D11/Vulkan)
    mutable std::mutex buffer_mutex_;
    std::array<BufferedFrame, kFrameBufferSize> frame_buffer_;
    std::unordered_map<int, int> frame_map_;  // frame_number → buffer index
    int buffer_head_ = 0;                     // Index of oldest frame in ring
    int buffer_count_ = 0;                    // Number of valid frames (0-16)
    std::condition_variable frame_ready_cv_;   // Signaled when new frame decoded
    std::mutex frame_ready_mutex_;             // Guards frame_ready_cv_ (separate from buffer_mutex_)

    // Frame index (replaces keyframe_positions_ + round(pts*fps) arithmetic)
    std::shared_ptr<FrameIndex> frame_index_;
    mutable std::vector<int> cached_keyframe_positions_;  // for GetKeyframePositions() return-by-ref
    bool is_intra_codec_ = false;

    // Playhead / decode state (matches D3D11/Vulkan pattern)
    std::atomic<int> playhead_{0};
    std::atomic<int> decode_target_{0};            // What decode thread aims for (separate from display playhead)
    std::atomic<int> last_seek_target_{0};
    std::atomic<int> current_decode_frame_{-1};    // Last frame decoded by decode thread
    std::atomic<bool> seek_pending_{false};
    std::atomic<bool> decode_seeking_{false};       // True while seek is executing (prevents re-trigger)
    std::atomic<bool> eof_reached_{false};          // EOF state (accessible from UpdatePlayhead)
    std::atomic<bool> initialized_{false};

    // B-frame delay queue tracking (decode thread only — NOT used to block display on Metal)
    std::atomic<bool> delay_queue_filling_{false};
    int frames_since_seek_ = 0;

    // Decode thread
    std::thread decode_thread_;
    std::atomic<bool> thread_running_{false};
    std::atomic<bool> shutdown_requested_{false};
    std::condition_variable decode_cv_;
    std::mutex decode_mutex_;

    // Demand-driven decode
    std::vector<int> needed_frames_;
    mutable std::mutex needed_mutex_;

    // Last successfully displayed texture (fallback during B-frame pipeline fill)
    uint64_t last_displayed_pool_id_ = 0;

    // Burst decode: skip GPU pipeline for frames before this number
    std::atomic<int> skip_gpu_until_frame_{-1};

    // Loop points
    std::atomic<int> loop_start_{-1};
    std::atomic<int> loop_end_{-1};
};

} // namespace qcview

#endif // __APPLE__
