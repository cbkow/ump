#pragma once

#ifdef _WIN32

#include <string>
#include <memory>
#include <array>
#include <d3d11_1.h>
#include <wrl/client.h>
#include <glad/gl.h>

#include "pipeline_mode.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/pixdesc.h>
}

namespace qcview {

class D3D11YUVRenderer;
class D3D11VideoInterop;

//=============================================================================
// D3D11VAVideoDecoder
//
// FFmpeg + D3D11VA hardware accelerated video decoder.
// Key design: OUR D3D11 device is passed TO FFmpeg (not the other way around).
// This ensures texture compatibility with our rendering pipeline.
//
// Based on LAVFilters' proven D3D11VA implementation approach:
// 1. Create D3D11 device first (from D3D11DeviceManager)
// 2. Pass device to FFmpeg's hw_device_ctx
// 3. Configure frame pool with D3D11_BIND_SHADER_RESOURCE for direct SRV use
// 4. Create Y/UV SRVs directly on decode textures (no copy needed)
// 5. Render YUV to RGB via D3D11YUVRenderer
// 6. Share result to OpenGL via D3D11VideoInterop
//
// Flow:
//   FFmpeg (D3D11VA HW decode)
//       ↓ Our D3D11 device passed to FFmpeg
//   D3D11 Texture (NV12/P010) with BIND_SHADER_RESOURCE
//       ↓ Direct SRV creation (no copy needed)
//   D3D11YUVRenderer (PQ EOTF + BT.2020)
//       ↓ Renders to interop texture
//   RGBA16F → GL Interop → OpenGL → OCIO
//=============================================================================

class D3D11VAVideoDecoder {
public:
    D3D11VAVideoDecoder();
    ~D3D11VAVideoDecoder();

    // Non-copyable
    D3D11VAVideoDecoder(const D3D11VAVideoDecoder&) = delete;
    D3D11VAVideoDecoder& operator=(const D3D11VAVideoDecoder&) = delete;

    //=========================================================================
    // Initialization
    //=========================================================================

    // Initialize with external D3D11 device (from D3D11DeviceManager)
    bool Initialize(ID3D11Device* device, const std::string& video_path);

    // Shutdown and release all resources
    void Shutdown();

    // Check if initialized
    bool IsInitialized() const { return initialized_; }

    //=========================================================================
    // Frame Access
    //=========================================================================

    // Get frame as GL texture
    // Returns 0 on failure
    GLuint GetFrameAsGLTexture(int frame_number);

    //=========================================================================
    // Seeking
    //=========================================================================

    bool SeekToFrame(int frame_number);
    bool SeekToTime(double time_seconds);

    //=========================================================================
    // Video Metadata
    //=========================================================================

    int GetWidth() const { return width_; }
    int GetHeight() const { return height_; }
    double GetFPS() const { return fps_; }
    double GetDuration() const { return duration_; }
    int GetFrameCount() const { return frame_count_; }

    // Returns actual last decodable frame, -1 if unknown
    int GetActualLastFrame() const { return actual_last_frame_; }

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
    // Initialization Helpers
    //=========================================================================

    bool InitializeFFmpeg(const std::string& path);
    bool ConfigureHardwareContext();
    bool OpenCodec();

    // FFmpeg get_format callback for hardware format negotiation
    static AVPixelFormat GetHWFormat(AVCodecContext* ctx, const AVPixelFormat* pix_fmts);
    bool SetupFramesContext();

    //=========================================================================
    // Decoding Helpers
    //=========================================================================

    bool DecodeFrameAtPosition(int target_frame);
    bool SeekToKeyframe(int64_t target_pts);
    bool CreatePlaneSRVs(ID3D11Texture2D* texture, int array_index,
                         ID3D11ShaderResourceView** srv_y,
                         ID3D11ShaderResourceView** srv_uv);
    bool EnsureInteropTexture();

    //=========================================================================
    // State
    //=========================================================================

    bool initialized_ = false;
    std::string video_path_;

    // D3D11 device (passed in, not owned - but we AddRef via ComPtr)
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;

    //=========================================================================
    // FFmpeg Components
    //=========================================================================

    AVFormatContext* format_ctx_ = nullptr;
    AVCodecContext* codec_ctx_ = nullptr;
    AVBufferRef* hw_device_ctx_ = nullptr;  // Points to OUR device
    AVBufferRef* hw_frames_ctx_ = nullptr;  // Frame pool with proper flags
    int video_stream_index_ = -1;

    // Current decoded frame
    AVFrame* current_frame_ = nullptr;
    AVPacket* packet_ = nullptr;
    int current_frame_number_ = -1;

    //=========================================================================
    // D3D11 Rendering Pipeline
    //=========================================================================

    std::unique_ptr<D3D11YUVRenderer> yuv_renderer_;
    std::unique_ptr<D3D11VideoInterop> interop_;

    // Temporary SRVs for current frame
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv_y_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv_uv_;

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
    int actual_last_frame_ = -1;

    // HDR info
    bool is_hdr_ = false;
    bool is_10bit_ = false;
    bool is_full_range_ = false;
    VideoRangeMode video_range_override_ = VideoRangeMode::AUTO;

    // Surface format
    DXGI_FORMAT surface_format_ = DXGI_FORMAT_NV12;

    // True when decoder pool lacks BIND_SHADER_RESOURCE (Intel Xe fallback).
    // CreatePlaneSRVs will copy to a staging texture before creating SRVs.
    bool needs_srv_copy_ = false;

    // GPU staging texture for SRV copy path (reused across frames)
    Microsoft::WRL::ComPtr<ID3D11Texture2D> srv_staging_texture_;
    UINT srv_staging_width_ = 0;
    UINT srv_staging_height_ = 0;
    DXGI_FORMAT srv_staging_format_ = DXGI_FORMAT_UNKNOWN;

    // Frame caching - avoid re-rendering same frame
    int last_rendered_frame_ = -1;

    // Time base for seeking
    AVRational time_base_ = {0, 1};

    //=========================================================================
    // Frame Buffer (Delay Queue)
    //=========================================================================

    // Buffered frame structure
    struct BufferedFrame {
        int frame_number = -1;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        int texture_array_index = 0;
        double pts = 0.0;
        bool valid = false;

        void Reset() {
            frame_number = -1;
            texture.Reset();
            texture_array_index = 0;
            pts = 0.0;
            valid = false;
        }
    };

    // Delay queue size for B-frame reordering
    static constexpr int kFrameBufferSize = 2;
    std::array<BufferedFrame, kFrameBufferSize> frame_buffer_;
    int buffer_head_ = 0;  // Index of oldest frame in buffer
    int buffer_count_ = 0; // Number of valid frames in buffer

    // Buffer management
    void ClearFrameBuffer();
    bool BufferContainsFrame(int frame_number) const;
    BufferedFrame* GetBufferedFrame(int frame_number);
    bool DecodeNextFrameToBuffer();
};

} // namespace qcview

#endif // _WIN32
