#pragma once

#ifdef _WIN32

#include <string>
#include <memory>
#include <array>
#include <vector>
#include <set>
#include <mutex>
#include <atomic>
#include <thread>
#include <condition_variable>
#include <d3d11_1.h>
#include <wrl/client.h>
#include <glad/gl.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#include "video_decoder_interface.h"
#include "pipeline_mode.h"

namespace ump {

class D3D11YUVRenderer;
class D3D11VideoInterop;

//=============================================================================
// D3D11VideoDecoder
//
// GPU-native video decoder implementing the full IVideoDecoder interface.
// Replaces the CPU ring buffer architecture with a D3D11-based pipeline:
//
// Flow:
//   FFmpeg decode (HW or SW)
//       ├─ HW codec (H.264/HEVC/VP9/AV1): D3D11VA zero-copy texture
//       └─ SW codec (ProRes/DNxHD): CPU AVFrame → D3D11 texture upload
//       ↓
//   D3D11 YUV Shader (GPU) → RGBA output
//       ↓
//   D3D11-GL Interop (zero-copy)
//       ↓
//   OCIO
//
// Benefits:
// - Minimal copies (zero-copy for HW, one copy for SW)
// - Native 10-bit/HDR support
// - Lower memory usage (2-frame GPU buffer vs 120-frame CPU buffer)
// - GPU-accelerated YUV→RGB color conversion
//=============================================================================

class D3D11VideoDecoder : public IVideoDecoder {
public:
    D3D11VideoDecoder();
    ~D3D11VideoDecoder() override;

    // Non-copyable
    D3D11VideoDecoder(const D3D11VideoDecoder&) = delete;
    D3D11VideoDecoder& operator=(const D3D11VideoDecoder&) = delete;

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
    void SetPipelineMode(PipelineMode mode) override;
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

    const char* GetBackendName() const override { return "D3D11"; }
    VideoDecoderBackend GetBackendType() const override { return VideoDecoderBackend::FFMPEG; }

    //=========================================================================
    // D3D11-Specific: Direct GL Texture Access
    //=========================================================================

    // Get frame as GL texture directly (bypasses PixelData for better performance)
    // Returns 0 on failure
    GLuint GetFrameAsGLTexture(int frame_number);

    // Initialize with video path (called before Initialize())
    void SetVideoPath(const std::string& path) { video_path_ = path; }

    //=========================================================================
    // HDR Information
    //=========================================================================

    bool IsHDRContent() const { return is_hdr_; }
    bool Is10BitOutput() const { return is_10bit_; }
    bool IsLimitedRange() const { return !GetEffectiveFullRange(); }

    //=========================================================================
    // Video Range Override
    //=========================================================================

    void SetVideoRangeOverride(VideoRangeMode mode) { video_range_override_ = mode; }
    VideoRangeMode GetVideoRangeOverride() const { return video_range_override_; }

    // Get effective full range considering override
    // User override is respected for all content (including HDR) for testing
    // AUTO mode uses detected range; FULL/LIMITED override detection
    bool GetEffectiveFullRange() const {
        switch (video_range_override_) {
            case VideoRangeMode::FULL: return true;
            case VideoRangeMode::LIMITED: return false;
            default: return is_full_range_;  // AUTO uses detected
        }
    }

private:
    //=========================================================================
    // Decode Mode Selection
    //=========================================================================

    enum class DecodeMode {
        HARDWARE,   // D3D11VA path (H.264, HEVC, VP9, AV1)
        SOFTWARE    // FFmpeg SW → D3D11 upload (ProRes, DNxHD, etc.)
    };

    // Check if codec supports hardware decode
    bool SupportsHardwareDecode(AVCodecID codec_id) const;

    // Determine decode mode based on codec
    DecodeMode DetermineDecodeMode(AVCodecID codec_id) const;

    //=========================================================================
    // Initialization Helpers
    //=========================================================================

    bool InitializeFFmpeg();
    bool ConfigureHardwareContext();
    bool ConfigureSoftwareContext();
    bool OpenCodec();
    bool SetupFramesContext();
    static AVPixelFormat GetHWFormat(AVCodecContext* ctx, const AVPixelFormat* pix_fmts);

    //=========================================================================
    // Decoding Helpers
    //=========================================================================

    bool DecodeNextFrame();
    bool SeekToKeyframe(int64_t target_pts);
    bool RenderFrameToTexture();

    // Software decode specific: Upload YUV planes to D3D11
    bool UploadSoftwareFrame(AVFrame* frame);
    DXGI_FORMAT GetYPlaneFormat(AVPixelFormat pix_fmt) const;
    DXGI_FORMAT GetUVPlaneFormat(AVPixelFormat pix_fmt) const;

    // Create Y/UV SRVs for rendering
    bool CreatePlaneSRVs(ID3D11Texture2D* texture, int array_index,
                         ID3D11ShaderResourceView** srv_y,
                         ID3D11ShaderResourceView** srv_uv);
    bool EnsureInteropTexture();

    //=========================================================================
    // Frame Buffer Management (2-frame GPU buffer)
    //=========================================================================

    struct BufferedFrame {
        int frame_number = -1;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        int texture_array_index = 0;  // For D3D11VA texture arrays
        double pts = 0.0;
        bool valid = false;
        bool is_hw_frame = false;  // True if from D3D11VA, false if SW upload

        void Reset() {
            frame_number = -1;
            texture.Reset();
            texture_array_index = 0;
            pts = 0.0;
            valid = false;
            is_hw_frame = false;
        }
    };

    static constexpr int kFrameBufferSize = 2;  // MPV-style delay queue
    std::array<BufferedFrame, kFrameBufferSize> frame_buffer_;
    int buffer_head_ = 0;
    int buffer_count_ = 0;
    mutable std::mutex buffer_mutex_;

    void ClearFrameBuffer();
    bool BufferContainsFrame(int frame_number) const;
    BufferedFrame* GetBufferedFrame(int frame_number);

    //=========================================================================
    // Keyframe Index (for inter-frame codecs)
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
    std::vector<int> needed_frames_;  // Frames to decode, in priority order

    //=========================================================================
    // State
    //=========================================================================

    std::atomic<bool> initialized_{false};
    std::string video_path_;
    StreamingDecoderConfig config_;
    PipelineMode pipeline_mode_ = PipelineMode::NORMAL;
    DecodeMode decode_mode_ = DecodeMode::SOFTWARE;
    HWAccelType hw_accel_type_ = HWAccelType::NONE;

    // Playhead tracking
    std::atomic<int> current_playhead_{0};
    std::atomic<int> last_seek_target_{-1};
    std::atomic<bool> seek_pending_{false};
    bool shuttle_mode_ = false;

    // Loop points
    int loop_start_ = -1;
    int loop_end_ = -1;

    //=========================================================================
    // D3D11 Device (from D3D11DeviceManager)
    //=========================================================================

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;

    //=========================================================================
    // FFmpeg Components
    //=========================================================================

    AVFormatContext* format_ctx_ = nullptr;
    AVCodecContext* codec_ctx_ = nullptr;
    AVBufferRef* hw_device_ctx_ = nullptr;
    AVBufferRef* hw_frames_ctx_ = nullptr;
    int video_stream_index_ = -1;

    AVFrame* current_frame_ = nullptr;
    AVFrame* sw_frame_ = nullptr;  // For SW decode transfer
    AVPacket* packet_ = nullptr;
    SwsContext* sws_ctx_ = nullptr;  // For format conversion if needed

    int current_frame_number_ = -1;
    AVRational time_base_ = {0, 1};

    //=========================================================================
    // D3D11 Rendering Pipeline
    //=========================================================================

    std::unique_ptr<D3D11YUVRenderer> yuv_renderer_;
    std::unique_ptr<D3D11VideoInterop> interop_;

    // Temporary SRVs for current frame
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv_y_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv_uv_;

    // Software decode staging textures (reused across frames)
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging_y_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging_uv_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> staging_srv_y_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> staging_srv_uv_;
    int staging_width_ = 0;
    int staging_height_ = 0;
    AVPixelFormat staging_pix_fmt_ = AV_PIX_FMT_NONE;

    // Interop texture dimensions
    int interop_width_ = 0;
    int interop_height_ = 0;

    //=========================================================================
    // Video Metadata
    //=========================================================================

    int width_ = 0;
    int height_ = 0;
    double fps_ = 0.0;
    double duration_ = 0.0;
    int frame_count_ = 0;

    // HDR info
    bool is_hdr_ = false;
    bool is_10bit_ = false;
    bool is_full_range_ = false;
    VideoRangeMode video_range_override_ = VideoRangeMode::AUTO;

    // Surface format
    DXGI_FORMAT surface_format_ = DXGI_FORMAT_NV12;

    // Frame caching - avoid re-rendering same frame
    int last_rendered_frame_ = -1;
};

} // namespace ump

#endif // _WIN32
