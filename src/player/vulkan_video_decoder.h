#pragma once

#ifdef __linux__

#include <string>
#include <memory>
#include <array>
#include <vector>
#include <set>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <thread>
#include <condition_variable>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
#include <libavutil/cpu.h>
#include <libswscale/swscale.h>
}

#include "video_decoder_interface.h"
#include "pipeline_mode.h"
#include "../gpu/vulkan_hwframe_importer.h"
#include "../gpu/vulkan_yuv_renderer.h"
#include "../gpu/vulkan_texture_pool.h"

namespace qcview {

//=============================================================================
// VulkanVideoDecoder
//
// Linux video decoder implementing IVideoDecoder.
//
// Flow:
//   FFmpeg decode (HW VA-API or SW)
//       ├─ HW codec (H.264/HEVC/VP9/AV1):
//       │   VA-API → DMA-BUF import → VkImage → YUV shader → RGBA16F VkImage
//       │   (zero-copy GPU path, falls back to CPU if DMA-BUF not available)
//       └─ SW codec (ProRes/DNxHD):
//           CPU AVFrame → sws_scale → RGBA → VulkanTexturePool upload
//       ↓
//   Ring buffer (PixelData for SW, pool texture for GPU)
//       ↓
//   Display via VulkanTexturePool
//=============================================================================

class VulkanVideoDecoder : public IVideoDecoder {
public:
    VulkanVideoDecoder();
    ~VulkanVideoDecoder() override;

    VulkanVideoDecoder(const VulkanVideoDecoder&) = delete;
    VulkanVideoDecoder& operator=(const VulkanVideoDecoder&) = delete;

    //=========================================================================
    // IVideoDecoder Lifecycle
    //=========================================================================

    bool Initialize() override;
    void Shutdown() override;
    void HardReset(int target_frame) override;
    bool IsInitialized() const override { return initialized_.load(); }

    //=========================================================================
    // IVideoDecoder Frame Access
    //=========================================================================

    std::shared_ptr<PixelData> GetFrame(int frame_number) override;
    std::shared_ptr<PixelData> GetClosestFrame(int frame_number, int* actual_frame = nullptr) override;
    bool HasFrame(int frame_number) const override;

    //=========================================================================
    // IVideoDecoder Keyframe Access
    //=========================================================================

    std::shared_ptr<PixelData> GetKeyframe(int target_frame, int* actual_keyframe = nullptr) override;
    int GetNearestKeyframePosition(int target_frame) const override;
    bool IsIntraFrameCodec() const override;
    bool HasKeyframeIndex() const override { return keyframe_index_built_; }
    const std::vector<int>& GetKeyframePositions() const override { return keyframe_positions_; }

    //=========================================================================
    // IVideoDecoder Playhead Management
    //=========================================================================

    void UpdatePlayhead(int frame_number, SeekQuality quality = SeekQuality::NORMAL,
                        bool force_seek = false) override;

    //=========================================================================
    // IVideoDecoder Demand-Driven Decode API
    //=========================================================================

    void SetNeededFrames(const std::vector<int>& frames_by_priority) override;
    DecodeStatus GetDecodeStatus() const override;
    void EvictOutsideWindow(const std::set<int>& keep_frames) override;
    std::set<int> GetBufferedFramesSet() const override;

    //=========================================================================
    // IVideoDecoder Buffer Status
    //=========================================================================

    int GetBufferedAhead() const override;
    int GetBufferedBehind() const override;
    int GetBufferSize() const override;
    void GetBufferedRange(int& start_frame, int& end_frame) const override;
    void ClearBuffer() override;
    bool IsSeekPending() const override { return seek_pending_.load(); }
    int GetLastSeekTarget() const override { return last_seek_target_.load(); }

    //=========================================================================
    // IVideoDecoder Metadata
    //=========================================================================

    int GetWidth() const override { return width_; }
    int GetHeight() const override { return height_; }
    double GetFPS() const override { return fps_; }
    double GetDuration() const override { return duration_; }
    int GetFrameCount() const override { return frame_count_; }
    const std::string& GetPath() const override { return video_path_; }

    //=========================================================================
    // IVideoDecoder Hardware Acceleration
    //=========================================================================

    HWAccelType GetHWAccelType() const override { return hw_accel_type_; }
    bool IsHardwareAccelerated() const override { return hw_accel_type_ != HWAccelType::NONE; }

    //=========================================================================
    // IVideoDecoder Configuration
    //=========================================================================

    void SetConfig(const StreamingDecoderConfig& config) override { config_ = config; }
    const StreamingDecoderConfig& GetConfig() const override { return config_; }
    void SetPipelineMode(PipelineMode mode) override { pipeline_mode_ = mode; }
    PipelineMode GetPipelineMode() const override { return pipeline_mode_; }
    void SetShuttleMode(bool enabled) override { shuttle_mode_ = enabled; }
    bool IsShuttleMode() const override { return shuttle_mode_; }

    //=========================================================================
    // IVideoDecoder Looping
    //=========================================================================

    void SetLoopPoints(int start_frame, int end_frame) override;
    void ClearLoopPoints() override;

    //=========================================================================
    // IVideoDecoder Backend Identification
    //=========================================================================

    const char* GetBackendName() const override { return "Vulkan"; }
    VideoDecoderBackend GetBackendType() const override { return VideoDecoderBackend::FFMPEG; }

    //=========================================================================
    // Vulkan-Specific: Texture Pool Access
    //=========================================================================

    // Get frame as VulkanTexturePool ID (uploads to GPU on demand)
    // Returns 0 on failure
    uint64_t GetFrameAsPoolTexture(int frame_number);

    // Force software decode (disables VA-API hardware acceleration)
    void SetForceSoftwareDecode(bool force) { force_software_ = force; }

    // Initialize with video path (called before Initialize())
    void SetVideoPath(const std::string& path) { video_path_ = path; }

    // HDR Information
    bool IsHDRContent() const { return is_hdr_; }
    bool Is10BitOutput() const { return is_10bit_; }
    bool IsLimitedRange() const { return !is_full_range_; }

private:
    //=========================================================================
    // Decode Mode
    //=========================================================================

    enum class DecodeMode {
        HARDWARE,   // VA-API path (H.264, HEVC, VP9, AV1)
        SOFTWARE    // FFmpeg SW decode (ProRes, DNxHD, etc.)
    };

    //=========================================================================
    // Frame Buffer Entry
    //=========================================================================

    struct BufferedFrame {
        int frame_number = -1;
        double pts = 0.0;
        bool valid = false;
        std::shared_ptr<PixelData> pixel_data;    // SW decode path
        uint64_t pool_texture_id = 0;  // VulkanTexturePool ID (0 = not uploaded)
        bool gpu_uploaded = false;     // true if pool_texture_id was set by GPU path
        AVFrame* hw_frame = nullptr;   // Ref'd VA-API frame for deferred GPU conversion

        void Reset() {
            frame_number = -1;
            pts = 0.0;
            valid = false;
            pixel_data.reset();
            // pool_texture_id cleaned up separately
            pool_texture_id = 0;
            gpu_uploaded = false;
            if (hw_frame) { av_frame_unref(hw_frame); av_frame_free(&hw_frame); hw_frame = nullptr; }
        }
    };

    bool SupportsHardwareDecode(AVCodecID codec_id) const;
    DecodeMode DetermineDecodeMode(AVCodecID codec_id) const;

    //=========================================================================
    // Initialization
    //=========================================================================

    bool InitializeFFmpeg();
    bool ConfigureHardwareContext();
    bool ConfigureSoftwareContext();
    bool OpenCodec();
    static AVPixelFormat GetHWFormat(AVCodecContext* ctx, const AVPixelFormat* pix_fmts);

    //=========================================================================
    // Decoding
    //=========================================================================

    bool DecodeNextFrame();
    bool SeekToFrame(int64_t target_pts);
    bool ConvertFrameToRGBA(AVFrame* frame, std::shared_ptr<PixelData>& out);

    // GPU path: import HW frame via DMA-BUF, run YUV shader, store as pool texture
    // If existing_texture != 0, reuses that pool texture (GPU→GPU copy update).
    // Returns pool texture ID, or 0 on failure.
    uint64_t ProcessHWFrameOnGPU(AVFrame* vaapi_frame, uint64_t existing_texture = 0);

    //=========================================================================
    // Frame Buffer
    //=========================================================================

    static constexpr int kFrameBufferSize = 16;
    static constexpr int kDelayQueueDepth = 4;
    std::array<BufferedFrame, kFrameBufferSize> frame_buffer_;
    int buffer_head_ = 0;
    int buffer_count_ = 0;
    mutable std::mutex buffer_mutex_;

    std::unordered_map<int, int> frame_map_;  // frame_number → buffer index

    std::atomic<int> frames_since_seek_{0};
    std::atomic<bool> delay_queue_filling_{false};

    void ClearFrameBuffer();
    bool BufferContainsFrame(int frame_number) const;
    BufferedFrame* GetBufferedFrame(int frame_number);
    BufferedFrame* GetClosestBufferedFrame(int frame_number);

    //=========================================================================
    // Keyframe Index
    //=========================================================================

    void BuildKeyframeIndex();
    std::vector<int> keyframe_positions_;
    bool keyframe_index_built_ = false;
    bool is_intra_only_codec_ = false;

    //=========================================================================
    // Background Decode Thread
    //=========================================================================

    void DecodeThreadFunc();
    std::thread decode_thread_;
    std::atomic<bool> decode_running_{false};
    std::condition_variable decode_cv_;
    mutable std::mutex decode_mutex_;
    std::vector<int> needed_frames_;

    std::atomic<int> decode_target_{0};
    std::atomic<int> decode_head_{-1};
    std::atomic<bool> decode_seeking_{false};

    std::condition_variable frame_ready_cv_;
    std::mutex frame_ready_mutex_;

    bool NeedsMoreFrames() const;
    void AddCurrentFrameToBuffer();
    void PerformSeekInternal(int target_frame);

    //=========================================================================
    // State
    //=========================================================================

    std::atomic<bool> initialized_{false};
    std::string video_path_;
    StreamingDecoderConfig config_;
    PipelineMode pipeline_mode_ = PipelineMode::NORMAL;
    DecodeMode decode_mode_ = DecodeMode::SOFTWARE;
    HWAccelType hw_accel_type_ = HWAccelType::NONE;

    std::atomic<int> current_playhead_{0};
    std::atomic<int> last_seek_target_{-1};
    std::atomic<bool> seek_pending_{false};
    std::atomic<bool> eof_reached_{false};
    int consecutive_decode_failures_ = 0;
    bool shuttle_mode_ = false;
    bool force_software_ = false;

    int loop_start_ = -1;
    int loop_end_ = -1;

    //=========================================================================
    // FFmpeg
    //=========================================================================

    AVFormatContext* format_ctx_ = nullptr;
    AVCodecContext* codec_ctx_ = nullptr;
    AVBufferRef* hw_device_ctx_ = nullptr;
    int video_stream_index_ = -1;

    AVFrame* current_frame_ = nullptr;
    AVFrame* sw_frame_ = nullptr;
    AVPacket* packet_ = nullptr;
    SwsContext* sws_ctx_ = nullptr;

    int current_frame_number_ = -1;
    AVRational time_base_ = {0, 1};

    //=========================================================================
    // Video Metadata
    //=========================================================================

    int width_ = 0;
    int height_ = 0;
    double fps_ = 0.0;
    double duration_ = 0.0;
    int frame_count_ = 0;

    bool is_hdr_ = false;
    bool is_10bit_ = false;
    bool is_full_range_ = false;

    //=========================================================================
    // GPU Decode (DMA-BUF import + YUV shader)
    //=========================================================================

    std::unique_ptr<VulkanHWFrameImporter> hw_frame_importer_;
    std::unique_ptr<VulkanYUVRenderer> yuv_renderer_;
    bool gpu_decode_available_ = false;

    // RGBA16F output image for GPU YUV conversion (reused across frames)
    VkImage gpu_output_image_ = VK_NULL_HANDLE;
    VmaAllocation gpu_output_alloc_ = VK_NULL_HANDLE;
    VkImageView gpu_output_view_ = VK_NULL_HANDLE;
    int gpu_output_width_ = 0;
    int gpu_output_height_ = 0;

    bool EnsureGPUOutputImage(int width, int height);
    void DestroyGPUOutputImage();
};

} // namespace qcview

#endif // __linux__
