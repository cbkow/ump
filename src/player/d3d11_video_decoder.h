#pragma once

#ifdef _WIN32

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
#include <d3d11_1.h>
#include <wrl/client.h>
#include <glad/gl.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/pixdesc.h>
#include <libavutil/cpu.h>
#include <libswscale/swscale.h>
}

#include "video_decoder_interface.h"
#include "pipeline_mode.h"

namespace qcview {

class D3D11YUVRenderer;
class D3D11VideoInterop;

//=============================================================================
// YUVFormatDesc
//
// Describes how to upload and render a YUV format.
// Maps FFmpeg pixel formats to D3D11 texture formats.
//=============================================================================

struct YUVFormatDesc {
    int plane_count;              // 2 for NV12/P010, 3 for planar YUV, 4 for YUVA/GBRAP
    int chroma_w;                 // Chroma width divisor (1=4:4:4, 2=4:2:2/4:2:0)
    int chroma_h;                 // Chroma height divisor (1=4:4:4/4:2:2, 2=4:2:0)
    DXGI_FORMAT plane_formats[4]; // Format for each plane texture (extended for alpha)
    bool is_10bit;
    int bit_depth = 8;            // 8, 10, or 12
    bool has_alpha = false;       // YUVA/GBRAP formats
    bool is_rgb_planar = false;   // GBRP/GBRAP formats (plane order: G, B, R, [A])

    static YUVFormatDesc FromAVPixelFormat(AVPixelFormat fmt);
};

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

    //=========================================================================
    // D3D11-Specific: Direct D3D11 SRV Access (for Unified Compositor)
    //=========================================================================

    // Get frame as D3D11 SRV - renders YUV to intermediate RGBA texture
    // Used by DualViewPipeline to composite both sources before interop
    // Returns nullptr if frame not available
    ID3D11ShaderResourceView* GetFrameAsD3D11SRV(int frame_number);

    // Get intermediate texture for external compositing
    ID3D11Texture2D* GetIntermediateTexture() const { return intermediate_texture_.Get(); }

    // Enable external compositor mode (skips interop, uses intermediate texture)
    void SetExternalCompositorMode(bool enabled);
    bool IsExternalCompositorMode() const { return use_external_compositor_; }

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

    //=========================================================================
    // Frame Buffer Entry (defined early for use in method signatures)
    //=========================================================================

    struct BufferedFrame {
        int frame_number = -1;
        double pts = 0.0;
        bool valid = false;
        bool is_hw_frame = false;  // True if from D3D11VA, false if SW upload
        bool hw_copied = false;    // True if HW frame was copied to local texture

        // Hardware decode (D3D11VA) - copied to local texture to avoid pool reuse
        Microsoft::WRL::ComPtr<ID3D11Texture2D> hw_texture;      // Our own copy
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> hw_srv_y;   // Y plane SRV
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> hw_srv_uv;  // UV plane SRV
        int texture_array_index = 0;  // Only used during copy

        // Software decode - up to 4 plane textures (GPU-side, render target)
        int plane_count = 0;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> plane_textures[4];
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> plane_srvs[4];
        int plane_widths[4] = {0, 0, 0, 0};   // Actual width of each plane
        int plane_heights[4] = {0, 0, 0, 0};  // Actual height of each plane

        // Staging textures for async upload (CPU-writable, D3D11_USAGE_STAGING)
        Microsoft::WRL::ComPtr<ID3D11Texture2D> staging_textures[4];

        // Format info for shader
        int chroma_w = 1;  // Chroma subsampling (1=4:4:4, 2=4:2:2/4:2:0)
        int chroma_h = 1;
        int bit_depth = 8; // 8, 10, or 12
        bool is_nv12_layout = false;  // True for 2-plane interleaved UV
        bool has_alpha = false;       // YUVA/GBRAP formats
        bool is_rgb_planar = false;   // GBRP/GBRAP formats

        // Legacy SW textures (kept for fallback path)
        Microsoft::WRL::ComPtr<ID3D11Texture2D> sw_texture_y;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> sw_texture_uv;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> sw_srv_y;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> sw_srv_uv;

        void Reset() {
            frame_number = -1;
            pts = 0.0;
            valid = false;
            is_hw_frame = false;
            hw_copied = false;
            // Don't reset hw_texture - keep allocated for reuse
            hw_srv_y.Reset();
            hw_srv_uv.Reset();
            texture_array_index = 0;
            plane_count = 0;
            for (int i = 0; i < 4; i++) {
                plane_textures[i].Reset();
                plane_srvs[i].Reset();
                staging_textures[i].Reset();
                plane_widths[i] = 0;
                plane_heights[i] = 0;
            }
            chroma_w = 1;
            chroma_h = 1;
            bit_depth = 8;
            is_nv12_layout = false;
            has_alpha = false;
            is_rgb_planar = false;
            // Keep legacy sw textures allocated for reuse in fallback path
        }
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
    bool UploadSoftwareFrameToSlot(AVFrame* frame, BufferedFrame& slot);
    bool UploadSoftwareFrameToSlotLegacy(AVFrame* frame, BufferedFrame& slot);  // Fallback with sws_scale
    DXGI_FORMAT GetYPlaneFormat(AVPixelFormat pix_fmt) const;
    DXGI_FORMAT GetUVPlaneFormat(AVPixelFormat pix_fmt) const;

    // Create Y/UV SRVs for rendering
    bool CreatePlaneSRVs(ID3D11Texture2D* texture, int array_index,
                         ID3D11ShaderResourceView** srv_y,
                         ID3D11ShaderResourceView** srv_uv);
    bool EnsureInteropTexture();

    //=========================================================================
    // Frame Buffer Management
    //=========================================================================

    static constexpr int kFrameBufferSize = 16;  // ~667ms lookahead at 24fps
    static constexpr int kDelayQueueDepth = 4;   // Min frames to buffer before output (B-frame reordering)
    std::array<BufferedFrame, kFrameBufferSize> frame_buffer_;
    int buffer_head_ = 0;
    int buffer_count_ = 0;
    mutable std::mutex buffer_mutex_;

    // O(1) frame lookup: frame_number -> buffer_index
    std::unordered_map<int, int> frame_map_;

    // Delay queue for B-frame reordering
    std::atomic<int> frames_since_seek_{0};      // Frames decoded since last seek
    std::atomic<bool> delay_queue_filling_{false}; // True while filling delay queue after seek

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

    // Async decode control
    std::atomic<int> decode_target_{0};          // Frame render thread needs
    std::atomic<int> decode_head_{-1};           // Last frame decoded
    std::atomic<bool> decode_seeking_{false};    // Seek in progress

    // Frame ready notification
    std::condition_variable frame_ready_cv_;
    std::mutex frame_ready_mutex_;

    // Async decode helpers
    bool NeedsMoreFrames() const;
    void AddCurrentFrameToBuffer();
    void PerformSeekInternal(int target_frame);
    BufferedFrame* GetClosestBufferedFrame(int frame_number);

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
    std::atomic<bool> eof_reached_{false};  // True when video reaches end
    int consecutive_decode_failures_{0};    // Track failures to detect true EOF
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

    // External compositor mode - uses intermediate texture instead of interop
    bool use_external_compositor_ = false;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> intermediate_texture_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> intermediate_rtv_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> intermediate_srv_;
    int intermediate_width_ = 0;
    int intermediate_height_ = 0;
    int last_srv_rendered_frame_ = -1;  // Track last frame rendered to intermediate
    int last_srv_requested_frame_ = -1; // Track last frame REQUESTED (for fallback caching)

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
    bool is_bt2020_ = false;  // True if BT.2020 color primaries (separate from is_hdr_)
    VideoRangeMode video_range_override_ = VideoRangeMode::AUTO;

    // Surface format
    DXGI_FORMAT surface_format_ = DXGI_FORMAT_NV12;

    // Frame caching - avoid re-rendering same frame
    int last_rendered_frame_ = -1;
    int last_requested_frame_ = -1;  // Track requested frame for fallback caching
};

} // namespace qcview

#endif // _WIN32
