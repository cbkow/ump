#include "d3d11_video_decoder.h"

#ifdef _WIN32

#include "../gpu/d3d11_yuv_renderer.h"
#include "../gpu/d3d11_video_interop.h"
#include "../gpu/d3d11_device_manager.h"
#include "../utils/debug_utils.h"

#include <d3d10.h>  // For ID3D10Multithread
#include <cstdlib>  // For std::abs
#include <climits>  // For INT_MAX
#include <algorithm>

namespace ump {

//=============================================================================
// Constructor / Destructor
//=============================================================================

D3D11VideoDecoder::D3D11VideoDecoder() {
}

D3D11VideoDecoder::~D3D11VideoDecoder() {
    Shutdown();
}

//=============================================================================
// Codec Detection
//=============================================================================

bool D3D11VideoDecoder::SupportsHardwareDecode(AVCodecID codec_id) const {
    switch (codec_id) {
        case AV_CODEC_ID_H264:
        case AV_CODEC_ID_HEVC:
        case AV_CODEC_ID_VP9:
        case AV_CODEC_ID_AV1:
            return true;
        default:
            return false;
    }
}

D3D11VideoDecoder::DecodeMode D3D11VideoDecoder::DetermineDecodeMode(AVCodecID codec_id) const {
    if (SupportsHardwareDecode(codec_id)) {
        return DecodeMode::HARDWARE;
    }
    return DecodeMode::SOFTWARE;
}

//=============================================================================
// Initialize
//=============================================================================

bool D3D11VideoDecoder::Initialize() {
    // Guard against double initialization (factory calls Initialize, then ManagedVideoDecoder does too)
    if (initialized_) {
        return true;
    }

    if (video_path_.empty()) {
        Debug::Log("D3D11VideoDecoder: Video path not set");
        return false;
    }

    // Get D3D11 device from manager
    auto& device_mgr = D3D11DeviceManager::Instance();
    if (!device_mgr.IsInitialized()) {
        if (!device_mgr.Initialize(nullptr)) {
            Debug::Log("D3D11VideoDecoder: Failed to initialize D3D11DeviceManager");
            return false;
        }
    }

    device_ = device_mgr.GetDevice();
    device_->GetImmediateContext(&context_);

    if (!device_) {
        Debug::Log("D3D11VideoDecoder: Failed to get D3D11 device");
        return false;
    }

    // Initialize FFmpeg components
    if (!InitializeFFmpeg()) {
        Debug::Log("D3D11VideoDecoder: Failed to initialize FFmpeg");
        Shutdown();
        return false;
    }

    // Determine decode mode based on codec
    decode_mode_ = DetermineDecodeMode(codec_ctx_->codec_id);

    // Configure hardware or software context
    if (decode_mode_ == DecodeMode::HARDWARE) {
        if (!ConfigureHardwareContext()) {
            Debug::Log("D3D11VideoDecoder: HW context failed, falling back to software");
            decode_mode_ = DecodeMode::SOFTWARE;
            hw_accel_type_ = HWAccelType::NONE;
        } else {
            hw_accel_type_ = HWAccelType::D3D11VA;
        }
    }

    if (decode_mode_ == DecodeMode::SOFTWARE) {
        if (!ConfigureSoftwareContext()) {
            Debug::Log("D3D11VideoDecoder: Failed to configure software context");
            Shutdown();
            return false;
        }
    }

    // Open the codec
    if (!OpenCodec()) {
        Debug::Log("D3D11VideoDecoder: Failed to open codec");
        Shutdown();
        return false;
    }

    // Initialize YUV renderer
    yuv_renderer_ = std::make_unique<D3D11YUVRenderer>();
    if (!yuv_renderer_->Initialize(device_.Get())) {
        Debug::Log("D3D11VideoDecoder: Failed to initialize YUV renderer");
        Shutdown();
        return false;
    }

    // Initialize interop
    interop_ = std::make_unique<D3D11VideoInterop>();
    if (!interop_->Initialize(device_.Get())) {
        Debug::Log("D3D11VideoDecoder: Failed to initialize D3D11 interop");
        Shutdown();
        return false;
    }

    // Allocate packet
    packet_ = av_packet_alloc();
    if (!packet_) {
        Debug::Log("D3D11VideoDecoder: Failed to allocate packet");
        Shutdown();
        return false;
    }

    // Build keyframe index for inter-frame codecs
    BuildKeyframeIndex();

    // Start decode thread
    decode_running_ = true;
    decode_thread_ = std::thread(&D3D11VideoDecoder::DecodeThreadFunc, this);

    initialized_ = true;

    Debug::Log("D3D11VideoDecoder: Initialized - " +
               std::to_string(width_) + "x" + std::to_string(height_) +
               " @ " + std::to_string(fps_) + " fps" +
               " [" + (decode_mode_ == DecodeMode::HARDWARE ? "D3D11VA HW" : "Software") + "]" +
               (is_hdr_ ? " [HDR]" : " [SDR]") +
               (is_10bit_ ? " [10-bit]" : " [8-bit]"));

    return true;
}

//=============================================================================
// InitializeFFmpeg
//=============================================================================

bool D3D11VideoDecoder::InitializeFFmpeg() {
    // Open input file
    int ret = avformat_open_input(&format_ctx_, video_path_.c_str(), nullptr, nullptr);
    if (ret < 0) {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        Debug::Log("D3D11VideoDecoder: Failed to open file: " + std::string(errbuf));
        return false;
    }

    // Find stream info
    ret = avformat_find_stream_info(format_ctx_, nullptr);
    if (ret < 0) {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        Debug::Log("D3D11VideoDecoder: Failed to find stream info: " + std::string(errbuf));
        return false;
    }

    // Find video stream
    video_stream_index_ = av_find_best_stream(format_ctx_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (video_stream_index_ < 0) {
        Debug::Log("D3D11VideoDecoder: No video stream found");
        return false;
    }

    AVStream* stream = format_ctx_->streams[video_stream_index_];
    AVCodecParameters* codecpar = stream->codecpar;
    time_base_ = stream->time_base;

    // Find decoder
    const AVCodec* codec = avcodec_find_decoder(codecpar->codec_id);
    if (!codec) {
        Debug::Log("D3D11VideoDecoder: Codec not found for codec_id=" + std::to_string(codecpar->codec_id));
        return false;
    }

    Debug::Log("D3D11VideoDecoder: Using codec: " + std::string(codec->name));

    // Check if intra-only codec
    is_intra_only_codec_ = (codecpar->codec_id == AV_CODEC_ID_PRORES ||
                            codecpar->codec_id == AV_CODEC_ID_DNXHD ||
                            codecpar->codec_id == AV_CODEC_ID_MJPEG ||
                            codecpar->codec_id == AV_CODEC_ID_RAWVIDEO);

    // Allocate codec context
    codec_ctx_ = avcodec_alloc_context3(codec);
    if (!codec_ctx_) {
        Debug::Log("D3D11VideoDecoder: Failed to allocate codec context");
        return false;
    }

    // Copy codec parameters
    ret = avcodec_parameters_to_context(codec_ctx_, codecpar);
    if (ret < 0) {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        Debug::Log("D3D11VideoDecoder: Failed to copy codec params: " + std::string(errbuf));
        return false;
    }

    // Extract metadata
    width_ = codec_ctx_->width;
    height_ = codec_ctx_->height;

    // Get FPS
    AVRational fps_rational = stream->r_frame_rate;
    if (fps_rational.num == 0 || fps_rational.den == 0) {
        fps_rational = stream->avg_frame_rate;
    }
    if (fps_rational.num > 0 && fps_rational.den > 0) {
        fps_ = av_q2d(fps_rational);
    } else {
        fps_ = 24.0;
    }

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

    // Check for HDR
    is_hdr_ = (codec_ctx_->color_primaries == AVCOL_PRI_BT2020) &&
              (codec_ctx_->color_trc == AVCOL_TRC_SMPTE2084 ||
               codec_ctx_->color_trc == AVCOL_TRC_ARIB_STD_B67);

    // Check for 10-bit
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(codec_ctx_->pix_fmt);
    is_10bit_ = desc && desc->comp[0].depth > 8;

    // Check color range
    is_full_range_ = (codec_ctx_->color_range == AVCOL_RANGE_JPEG);

    // Allocate frames
    current_frame_ = av_frame_alloc();
    sw_frame_ = av_frame_alloc();
    if (!current_frame_ || !sw_frame_) {
        Debug::Log("D3D11VideoDecoder: Failed to allocate frames");
        return false;
    }

    Debug::Log("D3D11VideoDecoder: FFmpeg initialized - " +
               std::to_string(width_) + "x" + std::to_string(height_) +
               " @ " + std::to_string(fps_) + " fps, " +
               std::to_string(frame_count_) + " frames");

    return true;
}

//=============================================================================
// ConfigureHardwareContext
//=============================================================================

bool D3D11VideoDecoder::ConfigureHardwareContext() {
    // Allocate hw device context
    hw_device_ctx_ = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
    if (!hw_device_ctx_) {
        Debug::Log("D3D11VideoDecoder: Failed to allocate hw device context");
        return false;
    }

    AVHWDeviceContext* device_ctx = (AVHWDeviceContext*)hw_device_ctx_->data;
    AVD3D11VADeviceContext* d3d11_ctx = (AVD3D11VADeviceContext*)device_ctx->hwctx;

    // Pass OUR device to FFmpeg
    d3d11_ctx->device = device_.Get();
    device_->AddRef();

    Microsoft::WRL::ComPtr<ID3D11DeviceContext> temp_context;
    device_->GetImmediateContext(&temp_context);
    d3d11_ctx->device_context = temp_context.Get();
    temp_context->AddRef();

    // Enable multithread protection
    Microsoft::WRL::ComPtr<ID3D10Multithread> multithread;
    if (SUCCEEDED(device_->QueryInterface(IID_PPV_ARGS(&multithread)))) {
        multithread->SetMultithreadProtected(TRUE);
    }

    int ret = av_hwdevice_ctx_init(hw_device_ctx_);
    if (ret < 0) {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        Debug::Log("D3D11VideoDecoder: Failed to init hw device context: " + std::string(errbuf));
        av_buffer_unref(&hw_device_ctx_);
        return false;
    }

    codec_ctx_->hw_device_ctx = av_buffer_ref(hw_device_ctx_);
    return true;
}

//=============================================================================
// ConfigureSoftwareContext
//=============================================================================

bool D3D11VideoDecoder::ConfigureSoftwareContext() {
    // For software decode, we just configure threading
    codec_ctx_->thread_count = config_.decodeThreads > 0 ? config_.decodeThreads : 4;
    codec_ctx_->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    return true;
}

//=============================================================================
// SetupFramesContext (for HW decode)
//=============================================================================

bool D3D11VideoDecoder::SetupFramesContext() {
    if (hw_frames_ctx_) {
        av_buffer_unref(&hw_frames_ctx_);
    }

    hw_frames_ctx_ = av_hwframe_ctx_alloc(hw_device_ctx_);
    if (!hw_frames_ctx_) {
        Debug::Log("D3D11VideoDecoder: Failed to allocate frames context");
        return false;
    }

    AVHWFramesContext* frames_ctx = (AVHWFramesContext*)hw_frames_ctx_->data;
    frames_ctx->format = AV_PIX_FMT_D3D11;
    frames_ctx->sw_format = is_10bit_ ? AV_PIX_FMT_P010 : AV_PIX_FMT_NV12;
    frames_ctx->width = codec_ctx_->width;
    frames_ctx->height = codec_ctx_->height;
    frames_ctx->initial_pool_size = 20 + 6;

    AVD3D11VAFramesContext* d3d11_frames = (AVD3D11VAFramesContext*)frames_ctx->hwctx;
    d3d11_frames->BindFlags = D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE;
    d3d11_frames->MiscFlags = 0;

    int ret = av_hwframe_ctx_init(hw_frames_ctx_);
    if (ret < 0) {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        Debug::Log("D3D11VideoDecoder: Failed to init frames context: " + std::string(errbuf));
        av_buffer_unref(&hw_frames_ctx_);
        return false;
    }

    codec_ctx_->hw_frames_ctx = av_buffer_ref(hw_frames_ctx_);
    surface_format_ = is_10bit_ ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;

    return true;
}

//=============================================================================
// GetHWFormat callback
//=============================================================================

AVPixelFormat D3D11VideoDecoder::GetHWFormat(AVCodecContext* ctx, const AVPixelFormat* pix_fmts) {
    D3D11VideoDecoder* self = static_cast<D3D11VideoDecoder*>(ctx->opaque);

    for (const AVPixelFormat* p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
        if (*p == AV_PIX_FMT_D3D11) {
            if (self && self->SetupFramesContext()) {
                return AV_PIX_FMT_D3D11;
            }
        }
    }

    return AV_PIX_FMT_NONE;
}

//=============================================================================
// OpenCodec
//=============================================================================

bool D3D11VideoDecoder::OpenCodec() {
    if (decode_mode_ == DecodeMode::HARDWARE) {
        codec_ctx_->thread_count = 1;
        codec_ctx_->thread_type = 0;
        codec_ctx_->opaque = this;
        codec_ctx_->get_format = &D3D11VideoDecoder::GetHWFormat;
    }

    int ret = avcodec_open2(codec_ctx_, nullptr, nullptr);
    if (ret < 0) {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        Debug::Log("D3D11VideoDecoder: Failed to open codec: " + std::string(errbuf));
        return false;
    }

    return true;
}

//=============================================================================
// Shutdown
//=============================================================================

void D3D11VideoDecoder::Shutdown() {
    // Stop decode thread
    if (decode_running_) {
        decode_running_ = false;
        decode_cv_.notify_all();
        if (decode_thread_.joinable()) {
            decode_thread_.join();
        }
    }

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
    staging_y_.Reset();
    staging_uv_.Reset();
    staging_srv_y_.Reset();
    staging_srv_uv_.Reset();

    if (sws_ctx_) {
        sws_freeContext(sws_ctx_);
        sws_ctx_ = nullptr;
    }

    if (packet_) {
        av_packet_free(&packet_);
    }

    if (current_frame_) {
        av_frame_free(&current_frame_);
    }

    if (sw_frame_) {
        av_frame_free(&sw_frame_);
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

    Debug::Log("D3D11VideoDecoder: Shutdown complete");
}

//=============================================================================
// SetPipelineMode
//=============================================================================

void D3D11VideoDecoder::SetPipelineMode(PipelineMode mode) {
    pipeline_mode_ = mode;

    // Recreate interop texture if dimensions change
    if (interop_ && interop_width_ > 0 && interop_height_ > 0) {
        DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
        switch (mode) {
            case PipelineMode::NORMAL:
                format = DXGI_FORMAT_R8G8B8A8_UNORM;
                break;
            case PipelineMode::HIGH_RES:
                format = DXGI_FORMAT_R16G16B16A16_UNORM;
                break;
            case PipelineMode::ULTRA_HIGH_RES:
            case PipelineMode::HDR_RES:
            case PipelineMode::MF_HDR:
                format = DXGI_FORMAT_R16G16B16A16_FLOAT;
                break;
        }
        interop_->CreateSharedTexture(interop_width_, interop_height_, format);
    }
}

//=============================================================================
// BuildKeyframeIndex
//=============================================================================

void D3D11VideoDecoder::BuildKeyframeIndex() {
    // For intra-only codecs, every frame is a keyframe
    if (is_intra_only_codec_) {
        keyframe_index_built_ = true;
        return;
    }

    // For hardware decode mode, skip keyframe indexing
    // D3D11VA handles seeking internally and scanning the file can corrupt HW context state
    if (decode_mode_ == DecodeMode::HARDWARE) {
        keyframe_index_built_ = true;
        Debug::Log("D3D11VideoDecoder: Skipping keyframe index for HW decode mode");
        return;
    }

    // Software decode: build keyframe index for efficient seeking
    keyframe_positions_.clear();
    AVPacket* pkt = av_packet_alloc();
    if (!pkt) {
        keyframe_index_built_ = true;
        return;
    }

    // Seek to start
    av_seek_frame(format_ctx_, video_stream_index_, 0, AVSEEK_FLAG_BACKWARD);

    while (av_read_frame(format_ctx_, pkt) >= 0) {
        if (pkt->stream_index == video_stream_index_) {
            if (pkt->flags & AV_PKT_FLAG_KEY) {
                double pts_seconds = pkt->pts * av_q2d(time_base_);
                int frame_num = static_cast<int>(pts_seconds * fps_ + 0.5);
                keyframe_positions_.push_back(frame_num);
            }
        }
        av_packet_unref(pkt);
    }

    av_packet_free(&pkt);

    // Restore position and flush codec
    av_seek_frame(format_ctx_, video_stream_index_, 0, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(codec_ctx_);

    keyframe_index_built_ = true;
    Debug::Log("D3D11VideoDecoder: Built keyframe index with " +
               std::to_string(keyframe_positions_.size()) + " keyframes");
}

//=============================================================================
// Frame Buffer Management
//=============================================================================

void D3D11VideoDecoder::ClearFrameBuffer() {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    for (auto& frame : frame_buffer_) {
        frame.Reset();
    }
    buffer_head_ = 0;
    buffer_count_ = 0;
}

bool D3D11VideoDecoder::BufferContainsFrame(int frame_number) const {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    for (int i = 0; i < buffer_count_; i++) {
        int idx = (buffer_head_ + i) % kFrameBufferSize;
        if (frame_buffer_[idx].valid && frame_buffer_[idx].frame_number == frame_number) {
            return true;
        }
    }
    return false;
}

D3D11VideoDecoder::BufferedFrame* D3D11VideoDecoder::GetBufferedFrame(int frame_number) {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    for (int i = 0; i < buffer_count_; i++) {
        int idx = (buffer_head_ + i) % kFrameBufferSize;
        if (frame_buffer_[idx].valid && frame_buffer_[idx].frame_number == frame_number) {
            return &frame_buffer_[idx];
        }
    }
    return nullptr;
}

//=============================================================================
// Software Decode Helpers
//=============================================================================

DXGI_FORMAT D3D11VideoDecoder::GetYPlaneFormat(AVPixelFormat pix_fmt) const {
    switch (pix_fmt) {
        case AV_PIX_FMT_YUV420P:
        case AV_PIX_FMT_YUV422P:
        case AV_PIX_FMT_YUV444P:
        case AV_PIX_FMT_NV12:
            return DXGI_FORMAT_R8_UNORM;
        case AV_PIX_FMT_YUV420P10:
        case AV_PIX_FMT_YUV422P10:
        case AV_PIX_FMT_YUV444P10:
        case AV_PIX_FMT_P010:
            return DXGI_FORMAT_R16_UNORM;
        default:
            return DXGI_FORMAT_R8_UNORM;
    }
}

DXGI_FORMAT D3D11VideoDecoder::GetUVPlaneFormat(AVPixelFormat pix_fmt) const {
    switch (pix_fmt) {
        case AV_PIX_FMT_YUV420P:
        case AV_PIX_FMT_YUV422P:
        case AV_PIX_FMT_YUV444P:
        case AV_PIX_FMT_NV12:
            return DXGI_FORMAT_R8G8_UNORM;
        case AV_PIX_FMT_YUV420P10:
        case AV_PIX_FMT_YUV422P10:
        case AV_PIX_FMT_YUV444P10:
        case AV_PIX_FMT_P010:
            return DXGI_FORMAT_R16G16_UNORM;
        default:
            return DXGI_FORMAT_R8G8_UNORM;
    }
}

bool D3D11VideoDecoder::UploadSoftwareFrame(AVFrame* frame) {
    if (!frame || !frame->data[0]) {
        return false;
    }

    AVPixelFormat pix_fmt = static_cast<AVPixelFormat>(frame->format);
    int frame_width = frame->width;
    int frame_height = frame->height;

    // Convert planar YUV to NV12/P010 for D3D11 YUV renderer
    // The renderer expects NV12 (8-bit) or P010 (10-bit) format

    bool is_10bit_frame = (pix_fmt == AV_PIX_FMT_YUV420P10 ||
                           pix_fmt == AV_PIX_FMT_YUV422P10 ||
                           pix_fmt == AV_PIX_FMT_YUV444P10 ||
                           pix_fmt == AV_PIX_FMT_P010);

    AVPixelFormat target_fmt = is_10bit_frame ? AV_PIX_FMT_P010 : AV_PIX_FMT_NV12;

    // Check if we need to convert
    bool needs_conversion = (pix_fmt != AV_PIX_FMT_NV12 && pix_fmt != AV_PIX_FMT_P010);

    AVFrame* upload_frame = frame;
    AVFrame* converted_frame = nullptr;

    if (needs_conversion) {
        // Convert to NV12/P010
        sws_ctx_ = sws_getCachedContext(
            sws_ctx_,
            frame_width, frame_height, pix_fmt,
            frame_width, frame_height, target_fmt,
            SWS_BILINEAR, nullptr, nullptr, nullptr
        );

        if (!sws_ctx_) {
            Debug::Log("D3D11VideoDecoder: Failed to create sws context");
            return false;
        }

        converted_frame = av_frame_alloc();
        converted_frame->format = target_fmt;
        converted_frame->width = frame_width;
        converted_frame->height = frame_height;
        av_frame_get_buffer(converted_frame, 32);

        sws_scale(sws_ctx_, frame->data, frame->linesize, 0, frame_height,
                  converted_frame->data, converted_frame->linesize);

        upload_frame = converted_frame;
    }

    // Create or resize staging textures if needed
    bool recreate_staging = (staging_width_ != frame_width ||
                             staging_height_ != frame_height ||
                             staging_pix_fmt_ != target_fmt);

    if (recreate_staging) {
        staging_y_.Reset();
        staging_uv_.Reset();
        staging_srv_y_.Reset();
        staging_srv_uv_.Reset();

        DXGI_FORMAT y_format = is_10bit_frame ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM;
        DXGI_FORMAT uv_format = is_10bit_frame ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R8G8_UNORM;

        // Y plane texture
        D3D11_TEXTURE2D_DESC y_desc = {};
        y_desc.Width = frame_width;
        y_desc.Height = frame_height;
        y_desc.MipLevels = 1;
        y_desc.ArraySize = 1;
        y_desc.Format = y_format;
        y_desc.SampleDesc.Count = 1;
        y_desc.Usage = D3D11_USAGE_DEFAULT;
        y_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        HRESULT hr = device_->CreateTexture2D(&y_desc, nullptr, &staging_y_);
        if (FAILED(hr)) {
            Debug::Log("D3D11VideoDecoder: Failed to create Y staging texture");
            if (converted_frame) av_frame_free(&converted_frame);
            return false;
        }

        // UV plane texture (half height for NV12/P010)
        D3D11_TEXTURE2D_DESC uv_desc = y_desc;
        uv_desc.Width = frame_width / 2;
        uv_desc.Height = frame_height / 2;
        uv_desc.Format = uv_format;

        hr = device_->CreateTexture2D(&uv_desc, nullptr, &staging_uv_);
        if (FAILED(hr)) {
            Debug::Log("D3D11VideoDecoder: Failed to create UV staging texture");
            if (converted_frame) av_frame_free(&converted_frame);
            return false;
        }

        // Create SRVs
        hr = device_->CreateShaderResourceView(staging_y_.Get(), nullptr, &staging_srv_y_);
        if (FAILED(hr)) {
            Debug::Log("D3D11VideoDecoder: Failed to create Y SRV");
            if (converted_frame) av_frame_free(&converted_frame);
            return false;
        }

        hr = device_->CreateShaderResourceView(staging_uv_.Get(), nullptr, &staging_srv_uv_);
        if (FAILED(hr)) {
            Debug::Log("D3D11VideoDecoder: Failed to create UV SRV");
            if (converted_frame) av_frame_free(&converted_frame);
            return false;
        }

        staging_width_ = frame_width;
        staging_height_ = frame_height;
        staging_pix_fmt_ = target_fmt;
        surface_format_ = is_10bit_frame ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;
    }

    // Upload Y plane
    D3D11_BOX y_box = {0, 0, 0, (UINT)frame_width, (UINT)frame_height, 1};
    context_->UpdateSubresource(staging_y_.Get(), 0, &y_box,
                                upload_frame->data[0],
                                upload_frame->linesize[0], 0);

    // Upload UV plane (NV12/P010 has interleaved UV in plane 1)
    D3D11_BOX uv_box = {0, 0, 0, (UINT)(frame_width / 2), (UINT)(frame_height / 2), 1};
    context_->UpdateSubresource(staging_uv_.Get(), 0, &uv_box,
                                upload_frame->data[1],
                                upload_frame->linesize[1], 0);

    if (converted_frame) {
        av_frame_free(&converted_frame);
    }

    return true;
}

//=============================================================================
// CreatePlaneSRVs (for HW decode textures)
//=============================================================================

bool D3D11VideoDecoder::CreatePlaneSRVs(ID3D11Texture2D* texture, int array_index,
                                         ID3D11ShaderResourceView** srv_y,
                                         ID3D11ShaderResourceView** srv_uv) {
    D3D11_TEXTURE2D_DESC desc;
    texture->GetDesc(&desc);

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
        return false;
    }

    return true;
}

//=============================================================================
// EnsureInteropTexture
//=============================================================================

bool D3D11VideoDecoder::EnsureInteropTexture() {
    DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
    switch (pipeline_mode_) {
        case PipelineMode::NORMAL:
            format = DXGI_FORMAT_R8G8B8A8_UNORM;
            break;
        case PipelineMode::HIGH_RES:
            format = DXGI_FORMAT_R16G16B16A16_UNORM;
            break;
        case PipelineMode::ULTRA_HIGH_RES:
        case PipelineMode::HDR_RES:
        case PipelineMode::MF_HDR:
            format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            break;
    }

    if (interop_width_ == width_ && interop_height_ == height_ && interop_->GetGLTexture() != 0) {
        return true;
    }

    if (!interop_->CreateSharedTexture(width_, height_, format)) {
        Debug::Log("D3D11VideoDecoder: Failed to create interop texture");
        return false;
    }

    interop_width_ = width_;
    interop_height_ = height_;

    return true;
}

//=============================================================================
// DecodeNextFrame
//=============================================================================

bool D3D11VideoDecoder::DecodeNextFrame() {
    av_frame_unref(current_frame_);

    while (true) {
        int ret = avcodec_receive_frame(codec_ctx_, current_frame_);

        if (ret == 0) {
            break;
        }

        if (ret == AVERROR_EOF) {
            return false;
        }

        if (ret != AVERROR(EAGAIN)) {
            return false;
        }

        ret = av_read_frame(format_ctx_, packet_);
        if (ret < 0) {
            if (ret == AVERROR_EOF) {
                avcodec_send_packet(codec_ctx_, nullptr);
                continue;
            }
            return false;
        }

        if (packet_->stream_index != video_stream_index_) {
            av_packet_unref(packet_);
            continue;
        }

        ret = avcodec_send_packet(codec_ctx_, packet_);
        av_packet_unref(packet_);

        if (ret < 0 && ret != AVERROR(EAGAIN)) {
            continue;
        }
    }

    // Calculate frame number from PTS
    double pts_seconds = (current_frame_->pts != AV_NOPTS_VALUE) ?
                         current_frame_->pts * av_q2d(time_base_) : 0.0;
    current_frame_number_ = static_cast<int>(pts_seconds * fps_ + 0.5);

    return true;
}

//=============================================================================
// SeekToKeyframe
//=============================================================================

bool D3D11VideoDecoder::SeekToKeyframe(int64_t target_pts) {
    int ret = av_seek_frame(format_ctx_, video_stream_index_, target_pts, AVSEEK_FLAG_BACKWARD);
    if (ret < 0) {
        ret = av_seek_frame(format_ctx_, video_stream_index_, 0, AVSEEK_FLAG_BACKWARD);
        if (ret < 0) {
            return false;
        }
    }

    avcodec_flush_buffers(codec_ctx_);
    return true;
}

//=============================================================================
// GetFrameAsGLTexture
//=============================================================================

GLuint D3D11VideoDecoder::GetFrameAsGLTexture(int frame_number) {
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
        bool need_seek = false;
        if (current_frame_number_ < 0) {
            need_seek = true;
        } else if (frame_number < current_frame_number_) {
            need_seek = true;
        } else if (frame_number > current_frame_number_ + 30) {
            need_seek = true;
        }

        if (need_seek) {
            double target_time = frame_number / fps_;
            int64_t target_pts = (int64_t)(target_time / av_q2d(time_base_));

            ClearFrameBuffer();
            current_frame_number_ = -1;
            avcodec_flush_buffers(codec_ctx_);

            int ret = av_seek_frame(format_ctx_, video_stream_index_, target_pts, AVSEEK_FLAG_BACKWARD);
            if (ret < 0) {
                ret = av_seek_frame(format_ctx_, video_stream_index_, 0, AVSEEK_FLAG_BACKWARD);
                if (ret < 0) {
                    return 0;
                }
            }
            avcodec_flush_buffers(codec_ctx_);
        }

        // Decode frames until we reach target
        int max_decode_attempts = 120;
        int decode_count = 0;

        while (!BufferContainsFrame(frame_number)) {
            if (!DecodeNextFrame()) {
                break;
            }

            // Add to buffer
            {
                std::lock_guard<std::mutex> lock(buffer_mutex_);
                int insert_idx;
                if (buffer_count_ < kFrameBufferSize) {
                    insert_idx = (buffer_head_ + buffer_count_) % kFrameBufferSize;
                    buffer_count_++;
                } else {
                    insert_idx = buffer_head_;
                    buffer_head_ = (buffer_head_ + 1) % kFrameBufferSize;
                }

                BufferedFrame& bf = frame_buffer_[insert_idx];
                bf.frame_number = current_frame_number_;

                if (decode_mode_ == DecodeMode::HARDWARE && current_frame_->format == AV_PIX_FMT_D3D11) {
                    bf.texture = (ID3D11Texture2D*)current_frame_->data[0];
                    bf.texture_array_index = (int)(intptr_t)current_frame_->data[1];
                    bf.is_hw_frame = true;
                } else {
                    // Software decode - upload to staging texture
                    if (UploadSoftwareFrame(current_frame_)) {
                        bf.texture = staging_y_;  // Store reference (not ideal but works for now)
                        bf.texture_array_index = 0;
                        bf.is_hw_frame = false;
                    }
                }

                bf.pts = current_frame_->pts != AV_NOPTS_VALUE ?
                         current_frame_->pts * av_q2d(time_base_) : 0.0;
                bf.valid = true;
            }

            decode_count++;

            if (current_frame_number_ > frame_number) {
                break;
            }

            if (decode_count >= max_decode_attempts) {
                break;
            }
        }

        buffered = GetBufferedFrame(frame_number);

        // If no exact match, find closest
        if (!buffered && buffer_count_ > 0) {
            std::lock_guard<std::mutex> lock(buffer_mutex_);
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
        }

        if (!buffered) {
            return 0;
        }
    }

    // Ensure interop texture exists
    if (!EnsureInteropTexture()) {
        return 0;
    }

    // Lock interop for D3D11 rendering
    if (!interop_->LockForD3D11()) {
        return 0;
    }

    // Set up SRVs based on frame type
    ID3D11ShaderResourceView* y_srv = nullptr;
    ID3D11ShaderResourceView* uv_srv = nullptr;

    if (buffered->is_hw_frame) {
        // HW frame - create SRVs from texture array
        srv_y_.Reset();
        srv_uv_.Reset();

        if (!CreatePlaneSRVs(buffered->texture.Get(), buffered->texture_array_index,
                             srv_y_.GetAddressOf(), srv_uv_.GetAddressOf())) {
            interop_->UnlockForGL();
            return 0;
        }
        y_srv = srv_y_.Get();
        uv_srv = srv_uv_.Get();
    } else {
        // SW frame - use staging SRVs
        y_srv = staging_srv_y_.Get();
        uv_srv = staging_srv_uv_.Get();
    }

    // Render YUV to RGB
    YUVRenderParams params;
    params.width = width_;
    params.height = height_;
    params.bit_depth = is_10bit_ ? 10 : 8;
    params.is_hdr = is_hdr_;
    params.is_full_range = GetEffectiveFullRange();  // Use override if set
    params.use_texture_array = buffered->is_hw_frame;
    params.color_space = is_hdr_ ? YUVColorSpace::BT_2020 : YUVColorSpace::BT_709;

    bool render_ok = yuv_renderer_->Render(y_srv, uv_srv, interop_->GetRTV(), params);

    interop_->UnlockForGL();

    if (!render_ok) {
        return 0;
    }

    last_rendered_frame_ = frame_number;
    return interop_->GetGLTexture();
}

//=============================================================================
// IVideoDecoder Interface Implementation
//=============================================================================

std::shared_ptr<PixelData> D3D11VideoDecoder::GetFrame(int frame_number) {
    // This decoder is optimized for direct GL texture output
    // For PixelData compatibility, we would need to read back from GPU
    // which defeats the purpose. Return nullptr and use GetFrameAsGLTexture instead.

    // For compatibility with timeline cache, we can implement GPU readback
    // but it's inefficient. The plan is to use GetFrameAsGLTexture directly.
    return nullptr;
}

std::shared_ptr<PixelData> D3D11VideoDecoder::GetClosestFrame(int frame_number, int* actual_frame) {
    // Similar to GetFrame - prefer GetFrameAsGLTexture
    return nullptr;
}

bool D3D11VideoDecoder::HasFrame(int frame_number) const {
    return BufferContainsFrame(frame_number);
}

std::shared_ptr<PixelData> D3D11VideoDecoder::GetKeyframe(int target_frame, int* actual_keyframe) {
    int keyframe = GetNearestKeyframePosition(target_frame);
    if (actual_keyframe) *actual_keyframe = keyframe;
    return GetFrame(keyframe);
}

int D3D11VideoDecoder::GetNearestKeyframePosition(int target_frame) const {
    if (is_intra_only_codec_ || keyframe_positions_.empty()) {
        return target_frame;
    }

    auto it = std::upper_bound(keyframe_positions_.begin(), keyframe_positions_.end(), target_frame);
    if (it == keyframe_positions_.begin()) {
        return keyframe_positions_.front();
    }
    --it;
    return *it;
}

bool D3D11VideoDecoder::IsIntraFrameCodec() const {
    return is_intra_only_codec_;
}

void D3D11VideoDecoder::UpdatePlayhead(int frame_number, SeekQuality quality, bool force_seek) {
    current_playhead_ = frame_number;

    if (force_seek || !BufferContainsFrame(frame_number)) {
        last_seek_target_ = frame_number;
        seek_pending_ = true;

        // Wake up decode thread
        decode_cv_.notify_one();
    }
}

void D3D11VideoDecoder::SetNeededFrames(const std::vector<int>& frames_by_priority) {
    std::lock_guard<std::mutex> lock(decode_mutex_);
    needed_frames_ = frames_by_priority;
    decode_cv_.notify_one();
}

DecodeStatus D3D11VideoDecoder::GetDecodeStatus() const {
    DecodeStatus status;

    std::lock_guard<std::mutex> lock(buffer_mutex_);
    for (int i = 0; i < buffer_count_; i++) {
        int idx = (buffer_head_ + i) % kFrameBufferSize;
        if (frame_buffer_[idx].valid) {
            status.have.push_back(frame_buffer_[idx].frame_number);
        }
    }

    status.currently_decoding = current_frame_number_;
    return status;
}

void D3D11VideoDecoder::EvictOutsideWindow(const std::set<int>& keep_frames) {
    // With only 2-frame buffer, eviction is automatic via circular overwrite
}

std::set<int> D3D11VideoDecoder::GetBufferedFramesSet() const {
    std::set<int> result;
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    for (int i = 0; i < buffer_count_; i++) {
        int idx = (buffer_head_ + i) % kFrameBufferSize;
        if (frame_buffer_[idx].valid) {
            result.insert(frame_buffer_[idx].frame_number);
        }
    }
    return result;
}

int D3D11VideoDecoder::GetBufferedAhead() const {
    int playhead = current_playhead_.load();
    int count = 0;
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    for (int i = 0; i < buffer_count_; i++) {
        int idx = (buffer_head_ + i) % kFrameBufferSize;
        if (frame_buffer_[idx].valid && frame_buffer_[idx].frame_number > playhead) {
            count++;
        }
    }
    return count;
}

int D3D11VideoDecoder::GetBufferedBehind() const {
    int playhead = current_playhead_.load();
    int count = 0;
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    for (int i = 0; i < buffer_count_; i++) {
        int idx = (buffer_head_ + i) % kFrameBufferSize;
        if (frame_buffer_[idx].valid && frame_buffer_[idx].frame_number < playhead) {
            count++;
        }
    }
    return count;
}

int D3D11VideoDecoder::GetBufferSize() const {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    return buffer_count_;
}

void D3D11VideoDecoder::GetBufferedRange(int& start_frame, int& end_frame) const {
    start_frame = -1;
    end_frame = -1;
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    for (int i = 0; i < buffer_count_; i++) {
        int idx = (buffer_head_ + i) % kFrameBufferSize;
        if (frame_buffer_[idx].valid) {
            if (start_frame < 0 || frame_buffer_[idx].frame_number < start_frame) {
                start_frame = frame_buffer_[idx].frame_number;
            }
            if (end_frame < 0 || frame_buffer_[idx].frame_number > end_frame) {
                end_frame = frame_buffer_[idx].frame_number;
            }
        }
    }
}

void D3D11VideoDecoder::ClearBuffer() {
    ClearFrameBuffer();
}

void D3D11VideoDecoder::HardReset(int target_frame) {
    ClearFrameBuffer();
    current_frame_number_ = -1;
    last_rendered_frame_ = -1;

    if (codec_ctx_) {
        avcodec_flush_buffers(codec_ctx_);
    }

    UpdatePlayhead(target_frame, SeekQuality::NORMAL, true);
}

void D3D11VideoDecoder::SetLoopPoints(int start_frame, int end_frame) {
    loop_start_ = start_frame;
    loop_end_ = end_frame;
}

void D3D11VideoDecoder::ClearLoopPoints() {
    loop_start_ = -1;
    loop_end_ = -1;
}

//=============================================================================
// Decode Thread
//=============================================================================

void D3D11VideoDecoder::DecodeThreadFunc() {
    while (decode_running_) {
        std::unique_lock<std::mutex> lock(decode_mutex_);
        decode_cv_.wait_for(lock, std::chrono::milliseconds(10), [this] {
            return !decode_running_ || seek_pending_ || !needed_frames_.empty();
        });

        if (!decode_running_) break;

        if (seek_pending_) {
            seek_pending_ = false;
            // The actual decode happens in GetFrameAsGLTexture for now
            // Future optimization: pre-decode here
        }
    }
}

} // namespace ump

#endif // _WIN32
