#include "d3d11va_video_decoder.h"

#ifdef _WIN32

#include "../gpu/d3d11_yuv_renderer.h"
#include "../gpu/d3d11_video_interop.h"
#include "../utils/debug_utils.h"

#include <d3d10.h>  // For ID3D10Multithread
#include <cstdlib>  // For std::abs
#include <climits>  // For INT_MAX

namespace qcview {

//=============================================================================
// Constructor / Destructor
//=============================================================================

D3D11VAVideoDecoder::D3D11VAVideoDecoder() {
}

D3D11VAVideoDecoder::~D3D11VAVideoDecoder() {
    Shutdown();
}

//=============================================================================
// Initialize
//=============================================================================

bool D3D11VAVideoDecoder::Initialize(ID3D11Device* device, const std::string& path) {
    if (!device) {
        Debug::Log("D3D11VAVideoDecoder: Device is null");
        return false;
    }

    if (path.empty()) {
        Debug::Log("D3D11VAVideoDecoder: Path is empty");
        return false;
    }

    // Store device (ComPtr AddRefs automatically)
    device_ = device;
    device_->GetImmediateContext(&context_);
    video_path_ = path;

    // Initialize FFmpeg components (open file, find stream, create codec context)
    if (!InitializeFFmpeg(path)) {
        Debug::Log("D3D11VAVideoDecoder: Failed to initialize FFmpeg");
        Shutdown();
        return false;
    }

    // Configure hardware context with OUR device
    if (!ConfigureHardwareContext()) {
        Debug::Log("D3D11VAVideoDecoder: Failed to configure hardware context");
        Shutdown();
        return false;
    }

    // Open the codec (frame pool is set up via get_format callback)
    if (!OpenCodec()) {
        Debug::Log("D3D11VAVideoDecoder: Failed to open codec");
        Shutdown();
        return false;
    }

    // Initialize YUV renderer
    yuv_renderer_ = std::make_unique<D3D11YUVRenderer>();
    if (!yuv_renderer_->Initialize(device_.Get())) {
        Debug::Log("D3D11VAVideoDecoder: Failed to initialize YUV renderer");
        Shutdown();
        return false;
    }

    // Initialize interop
    interop_ = std::make_unique<D3D11VideoInterop>();
    if (!interop_->Initialize(device_.Get())) {
        Debug::Log("D3D11VAVideoDecoder: Failed to initialize D3D11 interop");
        Shutdown();
        return false;
    }

    // Allocate packet
    packet_ = av_packet_alloc();
    if (!packet_) {
        Debug::Log("D3D11VAVideoDecoder: Failed to allocate packet");
        Shutdown();
        return false;
    }

    initialized_ = true;

    Debug::Log("D3D11VAVideoDecoder: Initialized - " +
               std::to_string(width_) + "x" + std::to_string(height_) +
               " @ " + std::to_string(fps_) + " fps" +
               (is_hdr_ ? " [HDR]" : " [SDR]") +
               (is_10bit_ ? " [10-bit]" : " [8-bit]") +
               " format=" + (surface_format_ == DXGI_FORMAT_P010 ? "P010" : "NV12"));

    return true;
}

//=============================================================================
// InitializeFFmpeg - Open file and configure codec context
//=============================================================================

bool D3D11VAVideoDecoder::InitializeFFmpeg(const std::string& path) {
    // Open input file
    int ret = avformat_open_input(&format_ctx_, path.c_str(), nullptr, nullptr);
    if (ret < 0) {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        Debug::Log("D3D11VAVideoDecoder: Failed to open file: " + std::string(errbuf));
        return false;
    }

    // Find stream info
    ret = avformat_find_stream_info(format_ctx_, nullptr);
    if (ret < 0) {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        Debug::Log("D3D11VAVideoDecoder: Failed to find stream info: " + std::string(errbuf));
        return false;
    }

    // Find video stream
    video_stream_index_ = av_find_best_stream(format_ctx_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (video_stream_index_ < 0) {
        Debug::Log("D3D11VAVideoDecoder: No video stream found");
        return false;
    }

    AVStream* stream = format_ctx_->streams[video_stream_index_];
    AVCodecParameters* codecpar = stream->codecpar;
    time_base_ = stream->time_base;

    // Find decoder
    const AVCodec* codec = avcodec_find_decoder(codecpar->codec_id);
    if (!codec) {
        Debug::Log("D3D11VAVideoDecoder: Codec not found for codec_id=" + std::to_string(codecpar->codec_id));
        return false;
    }

    Debug::Log("D3D11VAVideoDecoder: Using codec: " + std::string(codec->name));

    // Allocate codec context
    codec_ctx_ = avcodec_alloc_context3(codec);
    if (!codec_ctx_) {
        Debug::Log("D3D11VAVideoDecoder: Failed to allocate codec context");
        return false;
    }

    // Copy codec parameters
    ret = avcodec_parameters_to_context(codec_ctx_, codecpar);
    if (ret < 0) {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        Debug::Log("D3D11VAVideoDecoder: Failed to copy codec params: " + std::string(errbuf));
        return false;
    }

    // Extract metadata
    width_ = codec_ctx_->width;
    height_ = codec_ctx_->height;

    // Get FPS - use same priority as FFmpegMetadataExtractor:
    // r_frame_rate first (actual frame rate), avg_frame_rate as fallback
    AVRational fps_rational = stream->r_frame_rate;
    if (fps_rational.num == 0 || fps_rational.den == 0) {
        fps_rational = stream->avg_frame_rate;
    }
    if (fps_rational.num > 0 && fps_rational.den > 0) {
        fps_ = av_q2d(fps_rational);
    } else {
        fps_ = 24.0;  // Fallback
    }
    Debug::Log("D3D11VAVideoDecoder: FPS = " + std::to_string(fps_) +
               " (r_frame_rate=" + std::to_string(stream->r_frame_rate.num) + "/" +
               std::to_string(stream->r_frame_rate.den) +
               ", avg_frame_rate=" + std::to_string(stream->avg_frame_rate.num) + "/" +
               std::to_string(stream->avg_frame_rate.den) + ")");

    // Get duration
    if (stream->duration != AV_NOPTS_VALUE) {
        duration_ = stream->duration * av_q2d(stream->time_base);
    } else if (format_ctx_->duration != AV_NOPTS_VALUE) {
        duration_ = format_ctx_->duration / (double)AV_TIME_BASE;
    } else {
        duration_ = 0.0;
    }

    // Calculate frame count
    frame_count_ = (duration_ > 0 && fps_ > 0) ? (int)(duration_ * fps_ + 0.5) : 0;
    actual_last_frame_ = frame_count_ > 0 ? frame_count_ - 1 : -1;

    // Check for HDR (BT.2020 + PQ or HLG)
    is_hdr_ = (codec_ctx_->color_primaries == AVCOL_PRI_BT2020) &&
              (codec_ctx_->color_trc == AVCOL_TRC_SMPTE2084 ||
               codec_ctx_->color_trc == AVCOL_TRC_ARIB_STD_B67);

    // Check for 10-bit
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(codec_ctx_->pix_fmt);
    is_10bit_ = desc && desc->comp[0].depth > 8;

    // Check color range
    is_full_range_ = (codec_ctx_->color_range == AVCOL_RANGE_JPEG);

    // Allocate frame
    current_frame_ = av_frame_alloc();
    if (!current_frame_) {
        Debug::Log("D3D11VAVideoDecoder: Failed to allocate frame");
        return false;
    }

    Debug::Log("D3D11VAVideoDecoder: FFmpeg initialized - " +
               std::to_string(width_) + "x" + std::to_string(height_) +
               " @ " + std::to_string(fps_) + " fps, " +
               std::to_string(frame_count_) + " frames, " +
               std::to_string(duration_) + "s" +
               (is_hdr_ ? " [HDR]" : "") +
               (is_10bit_ ? " [10-bit]" : ""));

    return true;
}

//=============================================================================
// ConfigureHardwareContext - Pass OUR device to FFmpeg
//=============================================================================

bool D3D11VAVideoDecoder::ConfigureHardwareContext() {
    // Allocate hw device context
    hw_device_ctx_ = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
    if (!hw_device_ctx_) {
        Debug::Log("D3D11VAVideoDecoder: Failed to allocate hw device context");
        return false;
    }

    AVHWDeviceContext* device_ctx = (AVHWDeviceContext*)hw_device_ctx_->data;
    AVD3D11VADeviceContext* d3d11_ctx = (AVD3D11VADeviceContext*)device_ctx->hwctx;

    // KEY: Pass OUR device to FFmpeg (not letting FFmpeg create its own)
    d3d11_ctx->device = device_.Get();
    device_->AddRef();  // FFmpeg will release this when context is freed

    // Get device context
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> temp_context;
    device_->GetImmediateContext(&temp_context);
    d3d11_ctx->device_context = temp_context.Get();
    temp_context->AddRef();  // FFmpeg will release

    // Enable multithread protection (like LAVFilters)
    Microsoft::WRL::ComPtr<ID3D10Multithread> multithread;
    if (SUCCEEDED(device_->QueryInterface(IID_PPV_ARGS(&multithread)))) {
        multithread->SetMultithreadProtected(TRUE);
        Debug::Log("D3D11VAVideoDecoder: Multithread protection enabled");
    }

    int ret = av_hwdevice_ctx_init(hw_device_ctx_);
    if (ret < 0) {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        Debug::Log("D3D11VAVideoDecoder: Failed to init hw device context: " + std::string(errbuf));
        av_buffer_unref(&hw_device_ctx_);
        return false;
    }

    // Assign to codec context
    codec_ctx_->hw_device_ctx = av_buffer_ref(hw_device_ctx_);

    Debug::Log("D3D11VAVideoDecoder: Hardware context configured with our D3D11 device");
    return true;
}

//=============================================================================
// GetHWFormat - FFmpeg callback for hardware format negotiation
//=============================================================================

AVPixelFormat D3D11VAVideoDecoder::GetHWFormat(AVCodecContext* ctx, const AVPixelFormat* pix_fmts) {
    D3D11VAVideoDecoder* self = static_cast<D3D11VAVideoDecoder*>(ctx->opaque);

    for (const AVPixelFormat* p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
        if (*p == AV_PIX_FMT_D3D11) {
            Debug::Log("D3D11VAVideoDecoder: get_format selecting AV_PIX_FMT_D3D11");

            // Set up frames context with our custom bind flags
            if (self && self->SetupFramesContext()) {
                return AV_PIX_FMT_D3D11;
            }
        }
    }

    Debug::Log("D3D11VAVideoDecoder: get_format - D3D11 format not available, falling back");
    return AV_PIX_FMT_NONE;
}

//=============================================================================
// SetupFramesContext - Configure frame pool with BIND_SHADER_RESOURCE
//=============================================================================

bool D3D11VAVideoDecoder::SetupFramesContext() {
    // Free any existing frames context
    if (hw_frames_ctx_) {
        av_buffer_unref(&hw_frames_ctx_);
    }

    // Allocate frames context
    hw_frames_ctx_ = av_hwframe_ctx_alloc(hw_device_ctx_);
    if (!hw_frames_ctx_) {
        Debug::Log("D3D11VAVideoDecoder: Failed to allocate frames context");
        return false;
    }

    AVHWFramesContext* frames_ctx = (AVHWFramesContext*)hw_frames_ctx_->data;
    frames_ctx->format = AV_PIX_FMT_D3D11;
    frames_ctx->sw_format = is_10bit_ ? AV_PIX_FMT_P010 : AV_PIX_FMT_NV12;
    frames_ctx->width = codec_ctx_->width;
    frames_ctx->height = codec_ctx_->height;
    // Base pool size + extra frames for buffering
    frames_ctx->initial_pool_size = 20 + 6;

    // KEY: Add shader resource binding for direct SRV creation
    AVD3D11VAFramesContext* d3d11_frames = (AVD3D11VAFramesContext*)frames_ctx->hwctx;
    d3d11_frames->BindFlags = D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE;
    d3d11_frames->MiscFlags = 0;

    int ret = av_hwframe_ctx_init(hw_frames_ctx_);
    if (ret < 0) {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        Debug::Log("D3D11VAVideoDecoder: Failed to init frames context: " + std::string(errbuf));
        av_buffer_unref(&hw_frames_ctx_);
        return false;
    }

    // Assign to codec context
    codec_ctx_->hw_frames_ctx = av_buffer_ref(hw_frames_ctx_);

    surface_format_ = is_10bit_ ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;

    Debug::Log("D3D11VAVideoDecoder: Frame pool configured via get_format - " +
               std::string(is_10bit_ ? "P010" : "NV12") + " " +
               std::to_string(codec_ctx_->width) + "x" + std::to_string(codec_ctx_->height) +
               " BindFlags=DECODER|SHADER_RESOURCE");

    return true;
}

//=============================================================================
// OpenCodec
//=============================================================================

bool D3D11VAVideoDecoder::OpenCodec() {
    // Set threading options
    codec_ctx_->thread_count = 1;  // Single-threaded for HW decode
    codec_ctx_->thread_type = 0;

    // Set up get_format callback for hardware format negotiation
    codec_ctx_->opaque = this;
    codec_ctx_->get_format = &D3D11VAVideoDecoder::GetHWFormat;

    int ret = avcodec_open2(codec_ctx_, nullptr, nullptr);
    if (ret < 0) {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        Debug::Log("D3D11VAVideoDecoder: Failed to open codec: " + std::string(errbuf));
        return false;
    }

    Debug::Log("D3D11VAVideoDecoder: Codec opened successfully");
    return true;
}

//=============================================================================
// Shutdown
//=============================================================================

void D3D11VAVideoDecoder::Shutdown() {
    // Clear frame buffer first (releases texture refs)
    ClearFrameBuffer();

    if (interop_) {
        interop_->Shutdown();
        interop_.reset();
    }

    if (yuv_renderer_) {
        yuv_renderer_->Shutdown();
        yuv_renderer_.reset();
    }

    srv_y_.Reset();
    srv_uv_.Reset();

    if (packet_) {
        av_packet_free(&packet_);
    }

    if (current_frame_) {
        av_frame_free(&current_frame_);
    }

    if (codec_ctx_) {
        avcodec_free_context(&codec_ctx_);
    }

    if (hw_frames_ctx_) {
        av_buffer_unref(&hw_frames_ctx_);
    }

    if (hw_device_ctx_) {
        av_buffer_unref(&hw_device_ctx_);
    }

    if (format_ctx_) {
        avformat_close_input(&format_ctx_);
    }

    context_.Reset();
    device_.Reset();

    initialized_ = false;
    current_frame_number_ = -1;
    last_rendered_frame_ = -1;

    Debug::Log("D3D11VAVideoDecoder: Shutdown complete");
}

//=============================================================================
// CreatePlaneSRVs - Create Y/UV SRVs directly on decode texture
//=============================================================================

bool D3D11VAVideoDecoder::CreatePlaneSRVs(ID3D11Texture2D* texture, int array_index,
                                           ID3D11ShaderResourceView** srv_y,
                                           ID3D11ShaderResourceView** srv_uv) {
    D3D11_TEXTURE2D_DESC desc;
    texture->GetDesc(&desc);

    // Debug: log texture properties
    Debug::Log("D3D11VAVideoDecoder: Texture desc - " +
               std::to_string(desc.Width) + "x" + std::to_string(desc.Height) +
               " Format=" + std::to_string(desc.Format) +
               " ArraySize=" + std::to_string(desc.ArraySize) +
               " BindFlags=0x" + std::to_string(desc.BindFlags) +
               " MiscFlags=0x" + std::to_string(desc.MiscFlags) +
               " array_index=" + std::to_string(array_index));

    // Check if texture has SHADER_RESOURCE bind flag
    if (!(desc.BindFlags & D3D11_BIND_SHADER_RESOURCE)) {
        Debug::Log("D3D11VAVideoDecoder: WARNING - Texture missing BIND_SHADER_RESOURCE flag!");
    }

    bool is_array = desc.ArraySize > 1;

    // Y plane SRV
    D3D11_SHADER_RESOURCE_VIEW_DESC y_desc = {};
    y_desc.Format = (surface_format_ == DXGI_FORMAT_P010) ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM;

    if (is_array) {
        y_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        y_desc.Texture2DArray.MostDetailedMip = 0;
        y_desc.Texture2DArray.MipLevels = 1;
        y_desc.Texture2DArray.FirstArraySlice = array_index;
        y_desc.Texture2DArray.ArraySize = 1;
    } else {
        y_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        y_desc.Texture2D.MostDetailedMip = 0;
        y_desc.Texture2D.MipLevels = 1;
    }

    HRESULT hr = device_->CreateShaderResourceView(texture, &y_desc, srv_y);
    if (FAILED(hr)) {
        Debug::Log("D3D11VAVideoDecoder: Failed to create Y SRV, hr=" + std::to_string(hr));
        return false;
    }

    // UV plane SRV
    D3D11_SHADER_RESOURCE_VIEW_DESC uv_desc = {};
    uv_desc.Format = (surface_format_ == DXGI_FORMAT_P010) ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R8G8_UNORM;

    if (is_array) {
        uv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        uv_desc.Texture2DArray.MostDetailedMip = 0;
        uv_desc.Texture2DArray.MipLevels = 1;
        uv_desc.Texture2DArray.FirstArraySlice = array_index;
        uv_desc.Texture2DArray.ArraySize = 1;
    } else {
        uv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        uv_desc.Texture2D.MostDetailedMip = 0;
        uv_desc.Texture2D.MipLevels = 1;
    }

    hr = device_->CreateShaderResourceView(texture, &uv_desc, srv_uv);
    if (FAILED(hr)) {
        (*srv_y)->Release();
        *srv_y = nullptr;
        Debug::Log("D3D11VAVideoDecoder: Failed to create UV SRV, hr=" + std::to_string(hr));
        return false;
    }

    return true;
}

//=============================================================================
// EnsureInteropTexture
//=============================================================================

bool D3D11VAVideoDecoder::EnsureInteropTexture() {
    if (interop_width_ == width_ && interop_height_ == height_ && interop_->GetGLTexture() != 0) {
        return true;
    }

    // Create/resize interop texture (RGBA16F for HDR)
    if (!interop_->CreateSharedTexture(width_, height_, DXGI_FORMAT_R16G16B16A16_FLOAT)) {
        Debug::Log("D3D11VAVideoDecoder: Failed to create interop texture");
        return false;
    }

    interop_width_ = width_;
    interop_height_ = height_;

    Debug::Log("D3D11VAVideoDecoder: Created interop texture " +
               std::to_string(width_) + "x" + std::to_string(height_) + " RGBA16F");

    return true;
}

//=============================================================================
// SeekToKeyframe
//=============================================================================

bool D3D11VAVideoDecoder::SeekToKeyframe(int64_t target_pts) {
    int ret = av_seek_frame(format_ctx_, video_stream_index_, target_pts, AVSEEK_FLAG_BACKWARD);
    if (ret < 0) {
        // Try seeking from beginning
        ret = av_seek_frame(format_ctx_, video_stream_index_, 0, AVSEEK_FLAG_BACKWARD);
        if (ret < 0) {
            char errbuf[256];
            av_strerror(ret, errbuf, sizeof(errbuf));
            Debug::Log("D3D11VAVideoDecoder: Seek failed: " + std::string(errbuf));
            return false;
        }
    }

    avcodec_flush_buffers(codec_ctx_);
    return true;
}

//=============================================================================
// DecodeFrameAtPosition
//=============================================================================

bool D3D11VAVideoDecoder::DecodeFrameAtPosition(int target_frame) {
    if (target_frame < 0) target_frame = 0;
    if (frame_count_ > 0 && target_frame >= frame_count_) {
        target_frame = frame_count_ - 1;
    }

    // Calculate target PTS
    double target_time = target_frame / fps_;
    int64_t target_pts = (int64_t)(target_time / av_q2d(time_base_));

    // Determine if we need to seek
    bool need_seek = false;
    if (current_frame_number_ < 0) {
        need_seek = true;
    } else if (target_frame < current_frame_number_) {
        need_seek = true;
    } else if (target_frame > current_frame_number_ + 30) {
        // Too far ahead, seek instead of decoding through
        need_seek = true;
    }

    if (need_seek) {
        if (!SeekToKeyframe(target_pts)) {
            return false;
        }
        current_frame_number_ = -1;
    }

    // Decode frames until we reach target
    while (true) {
        av_frame_unref(current_frame_);

        int ret = av_read_frame(format_ctx_, packet_);
        if (ret < 0) {
            if (ret == AVERROR_EOF) {
                // Try to flush decoder
                ret = avcodec_send_packet(codec_ctx_, nullptr);
                if (ret >= 0) {
                    ret = avcodec_receive_frame(codec_ctx_, current_frame_);
                    if (ret >= 0) {
                        current_frame_number_++;
                        if (current_frame_number_ >= target_frame) {
                            return true;
                        }
                    }
                }
            }
            return false;
        }

        if (packet_->stream_index != video_stream_index_) {
            av_packet_unref(packet_);
            continue;
        }

        ret = avcodec_send_packet(codec_ctx_, packet_);
        av_packet_unref(packet_);

        if (ret < 0) {
            if (ret == AVERROR(EAGAIN)) {
                // Decoder has frames, receive them
            } else {
                char errbuf[256];
                av_strerror(ret, errbuf, sizeof(errbuf));
                Debug::Log("D3D11VAVideoDecoder: send_packet error: " + std::string(errbuf));
                continue;
            }
        }

        while (true) {
            ret = avcodec_receive_frame(codec_ctx_, current_frame_);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                break;
            }
            if (ret < 0) {
                char errbuf[256];
                av_strerror(ret, errbuf, sizeof(errbuf));
                Debug::Log("D3D11VAVideoDecoder: receive_frame error: " + std::string(errbuf));
                break;
            }

            current_frame_number_++;

            if (current_frame_number_ >= target_frame) {
                return true;
            }
        }
    }

    return false;
}

//=============================================================================
// GetFrameAsGLTexture - With frame buffer support
//=============================================================================

GLuint D3D11VAVideoDecoder::GetFrameAsGLTexture(int frame_number) {
    if (!initialized_) {
        return 0;
    }

    // Clamp frame number
    if (frame_number < 0) frame_number = 0;
    if (frame_count_ > 0 && frame_number >= frame_count_) {
        frame_number = frame_count_ - 1;
    }

    // Return cached texture if same frame
    if (frame_number == last_rendered_frame_ && interop_ && interop_->GetGLTexture() != 0) {
        return interop_->GetGLTexture();
    }

    // Check if frame is in buffer
    BufferedFrame* buffered = GetBufferedFrame(frame_number);

    if (!buffered) {
        // Frame not in buffer - need to decode
        // Check if we need to seek (going backward or too far ahead)
        bool need_seek = false;
        if (current_frame_number_ < 0) {
            need_seek = true;
        } else if (frame_number < current_frame_number_) {
            need_seek = true;
        } else if (frame_number > current_frame_number_ + 30) {
            // Too far ahead, seek instead of decoding through
            need_seek = true;
        }

        if (need_seek) {
            // Full seek with flush
            double target_time = frame_number / fps_;
            int64_t target_pts = (int64_t)(target_time / av_q2d(time_base_));

            Debug::Log("D3D11VAVideoDecoder: Seeking to frame " + std::to_string(frame_number) +
                       " (pts=" + std::to_string(target_pts) + ", time=" + std::to_string(target_time) + "s)");

            ClearFrameBuffer();
            current_frame_number_ = -1;
            avcodec_flush_buffers(codec_ctx_);

            int ret = av_seek_frame(format_ctx_, video_stream_index_, target_pts, AVSEEK_FLAG_BACKWARD);
            if (ret < 0) {
                Debug::Log("D3D11VAVideoDecoder: Seek to target failed, trying start");
                ret = av_seek_frame(format_ctx_, video_stream_index_, 0, AVSEEK_FLAG_BACKWARD);
                if (ret < 0) {
                    Debug::Log("D3D11VAVideoDecoder: Seek to start also failed for frame " + std::to_string(frame_number));
                    return 0;
                }
            }
            avcodec_flush_buffers(codec_ctx_);
        }

        // Decode frames until we reach target (filling buffer along the way)
        // Allow some tolerance since PTS-to-frame conversion may have rounding differences
        int max_decode_attempts = 120;  // Increased for longer GOP distances
        int decode_count = 0;

        while (!BufferContainsFrame(frame_number)) {
            if (!DecodeNextFrameToBuffer()) {
                // Decode failed - try to use closest frame we have
                Debug::Log("D3D11VAVideoDecoder: Decode failed after " +
                           std::to_string(decode_count) + " frames, current=" +
                           std::to_string(current_frame_number_) +
                           ", target=" + std::to_string(frame_number) +
                           ", total_frames=" + std::to_string(frame_count_));
                break;
            }

            decode_count++;

            // If we've overshot the target frame, stop - we have a nearby frame
            if (current_frame_number_ > frame_number) {
                break;
            }

            // Safety limit - but log useful info
            if (decode_count >= max_decode_attempts) {
                Debug::Log("D3D11VAVideoDecoder: Max decode attempts (" +
                           std::to_string(max_decode_attempts) + ") reached for frame " +
                           std::to_string(frame_number) + ", current=" +
                           std::to_string(current_frame_number_));
                break;
            }
        }

        // Try exact match first
        buffered = GetBufferedFrame(frame_number);

        // If no exact match, find closest frame in buffer
        if (!buffered && buffer_count_ > 0) {
            int closest_diff = INT_MAX;
            for (int i = 0; i < buffer_count_; i++) {
                int idx = (buffer_head_ + i) % kFrameBufferSize;
                if (frame_buffer_[idx].valid) {
                    int diff = std::abs(frame_buffer_[idx].frame_number - frame_number);
                    if (diff < closest_diff) {
                        closest_diff = diff;
                        buffered = &frame_buffer_[idx];
                    }
                }
            }

            if (buffered) {
                Debug::Log("D3D11VAVideoDecoder: Using closest frame " +
                           std::to_string(buffered->frame_number) +
                           " for requested " + std::to_string(frame_number));
            }
        }

        if (!buffered) {
            Debug::Log("D3D11VAVideoDecoder: No frames in buffer for " + std::to_string(frame_number));
            return 0;
        }
    }

    // Get texture from buffered frame
    ID3D11Texture2D* texture = buffered->texture.Get();
    int array_index = buffered->texture_array_index;

    if (!texture) {
        Debug::Log("D3D11VAVideoDecoder: No texture in buffered frame");
        return 0;
    }

    // Release old SRVs
    srv_y_.Reset();
    srv_uv_.Reset();

    // Create Y/UV plane SRVs directly on decode texture
    if (!CreatePlaneSRVs(texture, array_index,
                         srv_y_.GetAddressOf(), srv_uv_.GetAddressOf())) {
        Debug::Log("D3D11VAVideoDecoder: Failed to create plane SRVs");
        return 0;
    }

    // Ensure interop texture exists
    if (!EnsureInteropTexture()) {
        return 0;
    }

    // Lock interop for D3D11 rendering
    if (!interop_->LockForD3D11()) {
        Debug::Log("D3D11VAVideoDecoder: Failed to lock interop for D3D11");
        return 0;
    }

    // Render YUV to RGB
    YUVRenderParams params;
    params.width = width_;
    params.height = height_;
    params.bit_depth = is_10bit_ ? 10 : 8;
    params.is_hdr = is_hdr_;
    params.is_full_range = GetEffectiveFullRange();  // Use override if set
    params.use_texture_array = true;  // D3D11VA uses texture arrays
    params.color_space = is_hdr_ ? YUVColorSpace::BT_2020 : YUVColorSpace::BT_709;

    bool render_ok = yuv_renderer_->Render(
        srv_y_.Get(), srv_uv_.Get(),
        interop_->GetRTV(), params
    );

    // Unlock for GL
    interop_->UnlockForGL();

    if (!render_ok) {
        Debug::Log("D3D11VAVideoDecoder: YUV render failed");
        return 0;
    }

    last_rendered_frame_ = frame_number;
    return interop_->GetGLTexture();
}

//=============================================================================
// SeekToFrame - Prepare for seeking (actual FFmpeg seek done in GetFrameAsGLTexture)
//=============================================================================

bool D3D11VAVideoDecoder::SeekToFrame(int frame_number) {
    if (!initialized_) return false;

    if (frame_number < 0) frame_number = 0;
    if (frame_count_ > 0 && frame_number >= frame_count_) {
        frame_number = frame_count_ - 1;
    }

    // OPTIMIZATION: Don't clear buffer if target frame is already buffered or nearby
    // This prevents flickering when scrubbing and holding on same position
    if (BufferContainsFrame(frame_number)) {
        // Frame already in buffer - no action needed, GetFrameAsGLTexture will find it
        return true;
    }

    // Check if we can reach target by decoding forward (within reasonable range)
    if (current_frame_number_ >= 0 &&
        frame_number >= current_frame_number_ &&
        frame_number <= current_frame_number_ + 30) {
        // Can decode forward - don't clear buffer, let GetFrameAsGLTexture handle it
        return true;
    }

    // Need actual seek - clear buffer and mark for seek
    ClearFrameBuffer();
    last_rendered_frame_ = -1;
    current_frame_number_ = -1;  // Force seek on next GetFrameAsGLTexture

    return true;
}

//=============================================================================
// SeekToTime
//=============================================================================

bool D3D11VAVideoDecoder::SeekToTime(double time_seconds) {
    if (!initialized_) return false;

    int frame_number = (int)(time_seconds * fps_);
    return SeekToFrame(frame_number);
}

//=============================================================================
// Frame Buffer Management (delay queue for B-frame reordering)
//=============================================================================

void D3D11VAVideoDecoder::ClearFrameBuffer() {
    for (auto& frame : frame_buffer_) {
        frame.Reset();
    }
    buffer_head_ = 0;
    buffer_count_ = 0;
}

bool D3D11VAVideoDecoder::BufferContainsFrame(int frame_number) const {
    for (int i = 0; i < buffer_count_; i++) {
        int idx = (buffer_head_ + i) % kFrameBufferSize;
        if (frame_buffer_[idx].valid && frame_buffer_[idx].frame_number == frame_number) {
            return true;
        }
    }
    return false;
}

D3D11VAVideoDecoder::BufferedFrame* D3D11VAVideoDecoder::GetBufferedFrame(int frame_number) {
    for (int i = 0; i < buffer_count_; i++) {
        int idx = (buffer_head_ + i) % kFrameBufferSize;
        if (frame_buffer_[idx].valid && frame_buffer_[idx].frame_number == frame_number) {
            return &frame_buffer_[idx];
        }
    }
    return nullptr;
}

bool D3D11VAVideoDecoder::DecodeNextFrameToBuffer() {
    // Decode loop: Try to receive first, only send packets when EAGAIN
    // This properly drains all buffered frames due to B-frame reordering
    av_frame_unref(current_frame_);

    while (true) {
        // Step 1: Try to receive a frame first (drain decoder buffer)
        int ret = avcodec_receive_frame(codec_ctx_, current_frame_);

        if (ret == 0) {
            // Got a frame - success!
            break;
        }

        if (ret == AVERROR_EOF) {
            // Decoder fully drained
            Debug::Log("D3D11VAVideoDecoder: Decoder EOF, current_frame=" +
                       std::to_string(current_frame_number_));
            return false;
        }

        if (ret != AVERROR(EAGAIN)) {
            // Unexpected error
            char errbuf[256];
            av_strerror(ret, errbuf, sizeof(errbuf));
            Debug::Log("D3D11VAVideoDecoder: receive_frame error: " + std::string(errbuf));
            return false;
        }

        // Step 2: EAGAIN - decoder needs more data, read and send a packet
        ret = av_read_frame(format_ctx_, packet_);
        if (ret < 0) {
            if (ret == AVERROR_EOF) {
                // No more packets - send NULL to flush decoder
                Debug::Log("D3D11VAVideoDecoder: Hit stream EOF, flushing decoder...");
                avcodec_send_packet(codec_ctx_, nullptr);
                // Loop back to receive the flushed frames
                continue;
            }
            char errbuf[256];
            av_strerror(ret, errbuf, sizeof(errbuf));
            Debug::Log("D3D11VAVideoDecoder: av_read_frame error: " + std::string(errbuf));
            return false;
        }

        // Skip non-video packets
        if (packet_->stream_index != video_stream_index_) {
            av_packet_unref(packet_);
            continue;
        }

        // Send packet to decoder
        ret = avcodec_send_packet(codec_ctx_, packet_);
        av_packet_unref(packet_);

        if (ret < 0 && ret != AVERROR(EAGAIN)) {
            char errbuf[256];
            av_strerror(ret, errbuf, sizeof(errbuf));
            Debug::Log("D3D11VAVideoDecoder: send_packet error: " + std::string(errbuf));
            // Don't return false here - try to continue and receive what's buffered
        }

        // Loop back to try receiving again
    }

    // Got a frame - verify it's D3D11 hardware frame
    if (current_frame_->format != AV_PIX_FMT_D3D11) {
        Debug::Log("D3D11VAVideoDecoder: Frame is not D3D11 format");
        return false;
    }

    // Get texture from frame
    ID3D11Texture2D* texture = (ID3D11Texture2D*)current_frame_->data[0];
    int array_index = (int)(intptr_t)current_frame_->data[1];

    if (!texture) {
        Debug::Log("D3D11VAVideoDecoder: No texture in decoded frame");
        return false;
    }

    // Calculate actual frame number from PTS (not an incrementing counter!)
    // This is critical for correct frame identification after seeks
    double pts_seconds = (current_frame_->pts != AV_NOPTS_VALUE) ?
                         current_frame_->pts * av_q2d(time_base_) : 0.0;
    int actual_frame_number = static_cast<int>(pts_seconds * fps_ + 0.5);

    // Update current_frame_number_ to reflect actual position in stream
    int prev_frame = current_frame_number_;
    current_frame_number_ = actual_frame_number;

    // Log occasionally (every 30 frames) or on significant jumps
    static int decode_log_counter = 0;
    if (decode_log_counter++ % 30 == 0 || std::abs(actual_frame_number - prev_frame) > 5) {
        Debug::Log("D3D11VAVideoDecoder: Decoded frame " + std::to_string(actual_frame_number) +
                   " (pts=" + std::to_string(pts_seconds) + "s, prev=" + std::to_string(prev_frame) + ")");
    }

    // Add to buffer (circular)
    int insert_idx;
    if (buffer_count_ < kFrameBufferSize) {
        insert_idx = (buffer_head_ + buffer_count_) % kFrameBufferSize;
        buffer_count_++;
    } else {
        // Buffer full, overwrite oldest
        insert_idx = buffer_head_;
        buffer_head_ = (buffer_head_ + 1) % kFrameBufferSize;
    }

    // Store frame reference (texture is owned by FFmpeg pool, we just ref it)
    BufferedFrame& bf = frame_buffer_[insert_idx];
    bf.frame_number = actual_frame_number;
    bf.texture = texture;  // ComPtr AddRefs
    bf.texture_array_index = array_index;
    bf.pts = pts_seconds;
    bf.valid = true;

    return true;
}

} // namespace qcview

#endif // _WIN32
