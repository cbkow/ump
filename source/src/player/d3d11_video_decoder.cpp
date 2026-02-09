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
// YUVFormatDesc Implementation
//=============================================================================

YUVFormatDesc YUVFormatDesc::FromAVPixelFormat(AVPixelFormat fmt) {
    YUVFormatDesc desc = {};
    desc.plane_count = 0;  // Default to unsupported

    switch (fmt) {
        // 2-plane formats (NV12/P010)
        case AV_PIX_FMT_NV12:
            desc = {2, 2, 2, {DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8G8_UNORM, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN}, false, 8, false, false};
            break;
        case AV_PIX_FMT_P010:
            desc = {2, 2, 2, {DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16G16_UNORM, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN}, true, 10, false, false};
            break;

        // 3-plane 8-bit YUV
        case AV_PIX_FMT_YUV420P:
            desc = {3, 2, 2, {DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_UNKNOWN}, false, 8, false, false};
            break;
        case AV_PIX_FMT_YUV422P:
            desc = {3, 2, 1, {DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_UNKNOWN}, false, 8, false, false};
            break;
        case AV_PIX_FMT_YUV444P:
            desc = {3, 1, 1, {DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_UNKNOWN}, false, 8, false, false};
            break;

        // 3-plane 10-bit YUV
        case AV_PIX_FMT_YUV420P10LE:
            desc = {3, 2, 2, {DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_UNKNOWN}, true, 10, false, false};
            break;
        case AV_PIX_FMT_YUV422P10LE:
            desc = {3, 2, 1, {DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_UNKNOWN}, true, 10, false, false};
            break;
        case AV_PIX_FMT_YUV444P10LE:
            desc = {3, 1, 1, {DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_UNKNOWN}, true, 10, false, false};
            break;

        // 3-plane 12-bit YUV (DNxHR HQX, ARRI, RED)
        case AV_PIX_FMT_YUV420P12LE:
            desc = {3, 2, 2, {DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_UNKNOWN}, true, 12, false, false};
            break;
        case AV_PIX_FMT_YUV422P12LE:
            desc = {3, 2, 1, {DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_UNKNOWN}, true, 12, false, false};
            break;
        case AV_PIX_FMT_YUV444P12LE:
            desc = {3, 1, 1, {DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_UNKNOWN}, true, 12, false, false};
            break;

        // 4-plane YUVA 8-bit (with alpha)
        case AV_PIX_FMT_YUVA420P:
            desc = {4, 2, 2, {DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8_UNORM}, false, 8, true, false};
            break;
        case AV_PIX_FMT_YUVA422P:
            desc = {4, 2, 1, {DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8_UNORM}, false, 8, true, false};
            break;
        case AV_PIX_FMT_YUVA444P:
            desc = {4, 1, 1, {DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8_UNORM}, false, 8, true, false};
            break;

        // 4-plane YUVA 10-bit (ProRes 4444)
        case AV_PIX_FMT_YUVA420P10LE:
            desc = {4, 2, 2, {DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16_UNORM}, true, 10, true, false};
            break;
        case AV_PIX_FMT_YUVA422P10LE:
            desc = {4, 2, 1, {DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16_UNORM}, true, 10, true, false};
            break;
        case AV_PIX_FMT_YUVA444P10LE:
            desc = {4, 1, 1, {DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16_UNORM}, true, 10, true, false};
            break;

        // 4-plane YUVA 12-bit (ProRes 4444 XQ)
        case AV_PIX_FMT_YUVA444P12LE:
            desc = {4, 1, 1, {DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16_UNORM}, true, 12, true, false};
            break;

        // GBRP RGB planar (plane order: G, B, R)
        case AV_PIX_FMT_GBRP:
            desc = {3, 1, 1, {DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_UNKNOWN}, false, 8, false, true};
            break;
        case AV_PIX_FMT_GBRP10LE:
            desc = {3, 1, 1, {DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_UNKNOWN}, true, 10, false, true};
            break;
        case AV_PIX_FMT_GBRP12LE:
            desc = {3, 1, 1, {DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_UNKNOWN}, true, 12, false, true};
            break;

        // GBRAP with alpha
        case AV_PIX_FMT_GBRAP:
            desc = {4, 1, 1, {DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8_UNORM}, false, 8, true, true};
            break;
        case AV_PIX_FMT_GBRAP10LE:
            desc = {4, 1, 1, {DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16_UNORM}, true, 10, true, true};
            break;
        case AV_PIX_FMT_GBRAP12LE:
            desc = {4, 1, 1, {DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16_UNORM}, true, 12, true, true};
            break;

        default:
            // Unsupported - will need sws_scale fallback
            desc.plane_count = 0;
            break;
    }
    return desc;
}

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
    // Guard against double initialization
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

    // Enable multithread protection for D3D11 context
    // Required for async decode thread to safely upload textures
    // This provides built-in thread safety for the immediate context
    Microsoft::WRL::ComPtr<ID3D10Multithread> multithread;
    if (SUCCEEDED(device_->QueryInterface(IID_PPV_ARGS(&multithread)))) {
        multithread->SetMultithreadProtected(TRUE);
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

    // Initialize delay queue for B-frame codecs
    // This ensures we buffer frames before output even on first playback
    frames_since_seek_ = 0;
    if (codec_ctx_->has_b_frames > 0) {
        delay_queue_filling_ = true;
        Debug::Log("D3D11VideoDecoder: B-frame codec detected (has_b_frames=" +
                   std::to_string(codec_ctx_->has_b_frames) +
                   "), delay queue enabled");
    } else {
        delay_queue_filling_ = false;
    }

    // Start decode thread
    decode_running_ = true;
    decode_thread_ = std::thread(&D3D11VideoDecoder::DecodeThreadFunc, this);

    // Set higher priority for decode thread to reduce jitter
    SetThreadPriority(decode_thread_.native_handle(), THREAD_PRIORITY_ABOVE_NORMAL);

    initialized_ = true;

    Debug::Log("D3D11VideoDecoder: Initialized - " +
               std::to_string(width_) + "x" + std::to_string(height_) +
               " @ " + std::to_string(fps_) + " fps" +
               " [" + (decode_mode_ == DecodeMode::HARDWARE ? "D3D11VA HW" : "Software") + "]" +
               (is_hdr_ ? " [HDR]" : " [SDR]") +
               (is_10bit_ ? " [10-bit]" : " [8-bit]") +
               (codec_ctx_->has_b_frames > 0 ? " [B-frames]" : ""));

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

    // Detect BT.2020 primaries SEPARATELY from HDR transfer
    // This allows BT.2020 SDR content to use correct color matrix
    is_bt2020_ = (codec_ctx_->color_primaries == AVCOL_PRI_BT2020);

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
               std::to_string(frame_count_) + " frames" +
               " pix_fmt=" + std::to_string(codec_ctx_->pix_fmt) +
               " color_range=" + std::to_string(codec_ctx_->color_range) +
               " full_range=" + std::to_string(is_full_range_));

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
    // Let FFmpeg auto-detect optimal thread count (0 = auto)
    // FFmpeg will choose based on codec, resolution, and CPU cores
    codec_ctx_->thread_count = 0;
    codec_ctx_->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;

    Debug::Log("D3D11VideoDecoder: Using auto thread_count (av_cpu_count=" +
               std::to_string(av_cpu_count()) + ")");
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
        frame_ready_cv_.notify_all();
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
    last_requested_frame_ = -1;
    last_srv_rendered_frame_ = -1;
    last_srv_requested_frame_ = -1;

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
    frame_map_.clear();
}

bool D3D11VideoDecoder::BufferContainsFrame(int frame_number) const {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    auto it = frame_map_.find(frame_number);
    if (it != frame_map_.end()) {
        // Verify the slot is still valid (map might be stale)
        int idx = it->second;
        return frame_buffer_[idx].valid && frame_buffer_[idx].frame_number == frame_number;
    }
    return false;
}

D3D11VideoDecoder::BufferedFrame* D3D11VideoDecoder::GetBufferedFrame(int frame_number) {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    auto it = frame_map_.find(frame_number);
    if (it != frame_map_.end()) {
        int idx = it->second;
        // Verify the slot is still valid and contains the expected frame
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

    // Upload Y plane (D3D11 multithread protection enabled via SetMultithreadProtected)
    D3D11_BOX y_box = {0, 0, 0, (UINT)frame_width, (UINT)frame_height, 1};
    D3D11_BOX uv_box = {0, 0, 0, (UINT)(frame_width / 2), (UINT)(frame_height / 2), 1};
    context_->UpdateSubresource(staging_y_.Get(), 0, &y_box,
                                upload_frame->data[0],
                                upload_frame->linesize[0], 0);

    // Upload UV plane (NV12/P010 has interleaved UV in plane 1)
    context_->UpdateSubresource(staging_uv_.Get(), 0, &uv_box,
                                upload_frame->data[1],
                                upload_frame->linesize[1], 0);

    if (converted_frame) {
        av_frame_free(&converted_frame);
    }

    return true;
}

//=============================================================================
// UploadSoftwareFrameToSlot - Direct plane upload (NO sws_scale)
//=============================================================================

bool D3D11VideoDecoder::UploadSoftwareFrameToSlot(AVFrame* frame, BufferedFrame& slot) {
    if (!frame || !frame->data[0]) return false;

    AVPixelFormat pix_fmt = static_cast<AVPixelFormat>(frame->format);
    YUVFormatDesc fmt_desc = YUVFormatDesc::FromAVPixelFormat(pix_fmt);

    // Unsupported format - fall back to sws_scale (rare)
    if (fmt_desc.plane_count == 0) {
        Debug::Log("D3D11VideoDecoder: Unsupported pixel format " + std::to_string(pix_fmt) + ", using legacy upload");
        return UploadSoftwareFrameToSlotLegacy(frame, slot);
    }

    slot.plane_count = fmt_desc.plane_count;
    slot.chroma_w = fmt_desc.chroma_w;
    slot.chroma_h = fmt_desc.chroma_h;
    slot.bit_depth = fmt_desc.bit_depth;
    slot.is_nv12_layout = (fmt_desc.plane_count == 2);
    slot.has_alpha = fmt_desc.has_alpha;
    slot.is_rgb_planar = fmt_desc.is_rgb_planar;

    // Upload each plane using async staging textures (Map/Unmap + CopyResource)
    for (int p = 0; p < fmt_desc.plane_count; p++) {
        int plane_w, plane_h;

        if (p == 0) {
            // Y plane (or G plane for GBRP) - always full resolution
            plane_w = frame->width;
            plane_h = frame->height;
        } else if (fmt_desc.has_alpha && p == fmt_desc.plane_count - 1) {
            // Alpha plane - always same size as Y/luma plane
            plane_w = frame->width;
            plane_h = frame->height;
        } else if (fmt_desc.plane_count == 2 && p == 1) {
            // NV12 layout: UV plane is half height but contains both U and V
            plane_w = frame->width;  // Full width for interleaved UV
            plane_h = frame->height / 2;
        } else {
            // Chroma planes (U, V or B, R for GBRP)
            plane_w = frame->width / fmt_desc.chroma_w;
            plane_h = frame->height / fmt_desc.chroma_h;
        }

        slot.plane_widths[p] = plane_w;
        slot.plane_heights[p] = plane_h;

        // Create/resize GPU texture if needed (render target)
        bool need_create_gpu = !slot.plane_textures[p];
        if (slot.plane_textures[p]) {
            D3D11_TEXTURE2D_DESC desc;
            slot.plane_textures[p]->GetDesc(&desc);
            if (desc.Width != (UINT)plane_w || desc.Height != (UINT)plane_h ||
                desc.Format != fmt_desc.plane_formats[p]) {
                need_create_gpu = true;
            }
        }

        if (need_create_gpu) {
            slot.plane_textures[p].Reset();
            slot.plane_srvs[p].Reset();

            D3D11_TEXTURE2D_DESC desc = {};
            desc.Width = plane_w;
            desc.Height = plane_h;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = fmt_desc.plane_formats[p];
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

            HRESULT hr = device_->CreateTexture2D(&desc, nullptr, &slot.plane_textures[p]);
            if (FAILED(hr)) {
                Debug::Log("D3D11VideoDecoder: Failed to create plane " + std::to_string(p) + " texture");
                return false;
            }

            hr = device_->CreateShaderResourceView(slot.plane_textures[p].Get(),
                                                    nullptr, &slot.plane_srvs[p]);
            if (FAILED(hr)) {
                Debug::Log("D3D11VideoDecoder: Failed to create plane " + std::to_string(p) + " SRV");
                return false;
            }
        }

        // Create/resize staging texture if needed (CPU-writable)
        bool need_create_staging = !slot.staging_textures[p];
        if (slot.staging_textures[p]) {
            D3D11_TEXTURE2D_DESC desc;
            slot.staging_textures[p]->GetDesc(&desc);
            if (desc.Width != (UINT)plane_w || desc.Height != (UINT)plane_h ||
                desc.Format != fmt_desc.plane_formats[p]) {
                need_create_staging = true;
            }
        }

        if (need_create_staging) {
            slot.staging_textures[p].Reset();

            D3D11_TEXTURE2D_DESC staging_desc = {};
            staging_desc.Width = plane_w;
            staging_desc.Height = plane_h;
            staging_desc.MipLevels = 1;
            staging_desc.ArraySize = 1;
            staging_desc.Format = fmt_desc.plane_formats[p];
            staging_desc.SampleDesc.Count = 1;
            staging_desc.Usage = D3D11_USAGE_STAGING;
            staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

            HRESULT hr = device_->CreateTexture2D(&staging_desc, nullptr, &slot.staging_textures[p]);
            if (FAILED(hr)) {
                Debug::Log("D3D11VideoDecoder: Failed to create plane " + std::to_string(p) + " staging texture");
                return false;
            }
        }

        // Async upload: Map staging → memcpy → Unmap → CopyResource (non-blocking)
        // Lock context for thread safety (shared immediate context)
        D3D11_MAPPED_SUBRESOURCE mapped;
        HRESULT hr = context_->Map(slot.staging_textures[p].Get(), 0, D3D11_MAP_WRITE, 0, &mapped);
        if (FAILED(hr)) {
            Debug::Log("D3D11VideoDecoder: Failed to map staging texture for plane " + std::to_string(p));
            return false;
        }

        // Copy frame data to staging texture (no lock needed - CPU memory access)
        int bytes_per_pixel = fmt_desc.is_10bit ? 2 : 1;
        // For R8G8/R16G16 formats (UV plane in NV12), multiply by 2
        if (p == 1 && fmt_desc.plane_count == 2) {
            bytes_per_pixel *= 2;
        }
        int src_pitch = frame->linesize[p];
        int dst_pitch = mapped.RowPitch;
        int copy_width = plane_w * bytes_per_pixel;

        const uint8_t* src = frame->data[p];
        uint8_t* dst = static_cast<uint8_t*>(mapped.pData);

        // Row-by-row copy (handles pitch mismatch)
        for (int y = 0; y < plane_h; y++) {
            memcpy(dst + y * dst_pitch, src + y * src_pitch, copy_width);
        }

        context_->Unmap(slot.staging_textures[p].Get(), 0);

        // Queue async copy from staging to GPU texture (non-blocking)
        context_->CopyResource(slot.plane_textures[p].Get(), slot.staging_textures[p].Get());
    }

    // Update surface format for renderer (used by legacy code paths)
    surface_format_ = fmt_desc.is_10bit ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;

    // Log first successful async upload
    static bool first_async_upload = true;
    if (first_async_upload) {
        Debug::Log("D3D11VideoDecoder: Async plane upload - " +
                   std::to_string(frame->width) + "x" + std::to_string(frame->height) +
                   " pix_fmt=" + std::to_string(pix_fmt) +
                   " planes=" + std::to_string(fmt_desc.plane_count) +
                   " chroma=" + std::to_string(4) + ":" +
                   std::to_string(4 / fmt_desc.chroma_w) + ":" +
                   std::to_string(4 / fmt_desc.chroma_w / fmt_desc.chroma_h) +
                   " bit_depth=" + std::to_string(fmt_desc.bit_depth) +
                   " alpha=" + std::to_string(fmt_desc.has_alpha) +
                   " rgb_planar=" + std::to_string(fmt_desc.is_rgb_planar));
        first_async_upload = false;
    }

    return true;
}

//=============================================================================
// UploadSoftwareFrameToSlotLegacy - Fallback with sws_scale conversion
//=============================================================================

bool D3D11VideoDecoder::UploadSoftwareFrameToSlotLegacy(AVFrame* frame, BufferedFrame& slot) {
    if (!frame || !frame->data[0]) {
        return false;
    }

    AVPixelFormat pix_fmt = static_cast<AVPixelFormat>(frame->format);
    int frame_width = frame->width;
    int frame_height = frame->height;

    bool is_10bit_frame = (pix_fmt == AV_PIX_FMT_YUV420P10 ||
                           pix_fmt == AV_PIX_FMT_YUV422P10 ||
                           pix_fmt == AV_PIX_FMT_YUV444P10 ||
                           pix_fmt == AV_PIX_FMT_P010);

    AVPixelFormat target_fmt = is_10bit_frame ? AV_PIX_FMT_P010 : AV_PIX_FMT_NV12;
    bool needs_conversion = (pix_fmt != AV_PIX_FMT_NV12 && pix_fmt != AV_PIX_FMT_P010);

    AVFrame* upload_frame = frame;
    AVFrame* converted_frame = nullptr;

    if (needs_conversion) {
        sws_ctx_ = sws_getCachedContext(
            sws_ctx_,
            frame_width, frame_height, pix_fmt,
            frame_width, frame_height, target_fmt,
            SWS_BILINEAR, nullptr, nullptr, nullptr
        );

        if (!sws_ctx_) {
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

    // Set up slot for legacy 2-plane NV12/P010 format
    // NOTE: plane_count = 0 signals to use sw_srv_y/sw_srv_uv in render path
    // (plane_srvs[] are not populated by legacy upload)
    slot.plane_count = 0;
    slot.chroma_w = 2;
    slot.chroma_h = 2;
    slot.is_nv12_layout = true;

    // Check if slot's textures need to be created/recreated
    bool need_create = !slot.sw_texture_y;
    if (slot.sw_texture_y) {
        D3D11_TEXTURE2D_DESC desc;
        slot.sw_texture_y->GetDesc(&desc);
        if (desc.Width != (UINT)frame_width || desc.Height != (UINT)frame_height) {
            need_create = true;
        }
    }

    if (need_create) {
        slot.sw_texture_y.Reset();
        slot.sw_texture_uv.Reset();
        slot.sw_srv_y.Reset();
        slot.sw_srv_uv.Reset();

        DXGI_FORMAT y_format = is_10bit_frame ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM;
        DXGI_FORMAT uv_format = is_10bit_frame ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R8G8_UNORM;

        D3D11_TEXTURE2D_DESC y_desc = {};
        y_desc.Width = frame_width;
        y_desc.Height = frame_height;
        y_desc.MipLevels = 1;
        y_desc.ArraySize = 1;
        y_desc.Format = y_format;
        y_desc.SampleDesc.Count = 1;
        y_desc.Usage = D3D11_USAGE_DEFAULT;
        y_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        HRESULT hr = device_->CreateTexture2D(&y_desc, nullptr, &slot.sw_texture_y);
        if (FAILED(hr)) {
            if (converted_frame) av_frame_free(&converted_frame);
            return false;
        }

        D3D11_TEXTURE2D_DESC uv_desc = y_desc;
        uv_desc.Width = frame_width / 2;
        uv_desc.Height = frame_height / 2;
        uv_desc.Format = uv_format;

        hr = device_->CreateTexture2D(&uv_desc, nullptr, &slot.sw_texture_uv);
        if (FAILED(hr)) {
            if (converted_frame) av_frame_free(&converted_frame);
            return false;
        }

        hr = device_->CreateShaderResourceView(slot.sw_texture_y.Get(), nullptr, &slot.sw_srv_y);
        if (FAILED(hr)) {
            if (converted_frame) av_frame_free(&converted_frame);
            return false;
        }

        hr = device_->CreateShaderResourceView(slot.sw_texture_uv.Get(), nullptr, &slot.sw_srv_uv);
        if (FAILED(hr)) {
            if (converted_frame) av_frame_free(&converted_frame);
            return false;
        }

        // Update surface format for renderer
        surface_format_ = is_10bit_frame ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;
    }

    // Upload Y plane to this slot's texture (D3D11 multithread protection enabled)
    D3D11_BOX y_box = {0, 0, 0, (UINT)frame_width, (UINT)frame_height, 1};
    D3D11_BOX uv_box = {0, 0, 0, (UINT)(frame_width / 2), (UINT)(frame_height / 2), 1};
    context_->UpdateSubresource(slot.sw_texture_y.Get(), 0, &y_box,
                                upload_frame->data[0],
                                upload_frame->linesize[0], 0);

    // Upload UV plane
    context_->UpdateSubresource(slot.sw_texture_uv.Get(), 0, &uv_box,
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

    int receive_attempts = 0;
    int read_attempts = 0;
    const int max_receive_attempts = 100;  // Prevent infinite loop

    while (receive_attempts < max_receive_attempts) {
        receive_attempts++;
        int ret = avcodec_receive_frame(codec_ctx_, current_frame_);

        if (ret == 0) {
            // Got a frame - success
            // Use best_effort_timestamp for correct display order (handles B-frame reordering)
            // Fall back to pts if best_effort_timestamp is not available
            int64_t timestamp = current_frame_->best_effort_timestamp;
            int64_t raw_pts = current_frame_->pts;
            if (timestamp == AV_NOPTS_VALUE) {
                timestamp = raw_pts;
            }
            double pts_seconds = (timestamp != AV_NOPTS_VALUE) ?
                                 timestamp * av_q2d(time_base_) : 0.0;
            int old_frame = current_frame_number_;
            current_frame_number_ = static_cast<int>(pts_seconds * fps_ + 0.5);

            // Debug: log frame order to diagnose B-frame issues
            static int debug_count = 0;
            if (debug_count++ < 30 || debug_count % 100 == 0) {
                Debug::Log("D3D11VideoDecoder: Decoded frame " + std::to_string(current_frame_number_) +
                           " (pts=" + std::to_string(raw_pts) +
                           ", best=" + std::to_string(timestamp) +
                           ", prev=" + std::to_string(old_frame) +
                           ", pict_type=" + std::string(1, av_get_picture_type_char((AVPictureType)current_frame_->pict_type)) + ")");
            }
            return true;
        }

        if (ret == AVERROR_EOF) {
            // Normal end of stream
            return false;
        }

        if (ret != AVERROR(EAGAIN)) {
            // Decode error
            char errbuf[256];
            av_strerror(ret, errbuf, sizeof(errbuf));
            Debug::Log("D3D11VideoDecoder: Decode error: " + std::string(errbuf));
            return false;
        }

        // Need more input - read next packet
        read_attempts++;
        ret = av_read_frame(format_ctx_, packet_);
        if (ret < 0) {
            if (ret == AVERROR_EOF) {
                // Send NULL packet to flush decoder
                avcodec_send_packet(codec_ctx_, nullptr);
                continue;  // Try to receive flushed frames
            }
            char errbuf[256];
            av_strerror(ret, errbuf, sizeof(errbuf));
            Debug::Log("D3D11VideoDecoder: Read error: " + std::string(errbuf));
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
            Debug::Log("D3D11VideoDecoder: Send packet error: " + std::string(errbuf));
            // Don't fail - try to continue receiving frames that may already be decoded
        }
    }

    Debug::Log("D3D11VideoDecoder: Exceeded max receive attempts (" +
               std::to_string(max_receive_attempts) + ")");
    return false;
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

    // Reset delay queue state for B-frame reordering
    // After a seek, we need to buffer several frames before output to allow
    // libavcodec to properly reorder B-frames
    frames_since_seek_ = 0;
    if (codec_ctx_->has_b_frames > 0) {
        delay_queue_filling_ = true;
    }

    return true;
}

//=============================================================================
// GetFrameAsGLTexture
//=============================================================================

GLuint D3D11VideoDecoder::GetFrameAsGLTexture(int frame_number) {
    if (!initialized_) {
        return 0;
    }

    // Try to upgrade from CPU fallback to zero-copy interop now that GL context is ready
    // This handles the case where decoder was created before GL context was available
    // (common in DUAL_VIEW mode where decoder is created during timeline load)
    if (interop_ && !interop_->HasZeroCopyInterop()) {
        static bool tried_reinit = false;
        if (!tried_reinit) {
            tried_reinit = true;
            interop_->TryReinitializeZeroCopy();
        }
    }

    // Clamp frame number
    frame_number = std::clamp(frame_number, 0,
                              frame_count_ > 0 ? frame_count_ - 1 : 0);

    // Return cached texture if same frame REQUESTED (even if we rendered a fallback frame)
    // This prevents infinite seek loops when B-frame delay means frame 0 isn't immediately available
    if (frame_number == last_requested_frame_ && interop_ && interop_->GetGLTexture() != 0) {
        return interop_->GetGLTexture();
    }

    // Update decode target and playhead for background thread
    current_playhead_ = frame_number;
    decode_target_ = frame_number;
    decode_cv_.notify_one();

    // Check if frame is in buffer
    BufferedFrame* buffered = GetBufferedFrame(frame_number);

    if (!buffered) {
        // Check if seek is needed
        int head = decode_head_.load();

        // Very lenient seek threshold - only seek when REALLY far from target
        // For forward playback, let decoder catch up naturally (sequential decode is faster than seek)
        // Seek threshold: ~2.5 seconds at 24fps = 60 frames
        constexpr int kSeekThresholdFrames = 60;

        // Need seek only if:
        // 1. Backward seek (target is behind decode head)
        // 2. Target is WAY ahead (more than 2.5 seconds beyond current decode position)
        // 3. Head is uninitialized (first decode)
        bool need_backward_seek = (head >= 0) && (frame_number < head - kFrameBufferSize);
        bool need_forward_seek = (head >= 0) && (frame_number > head + kSeekThresholdFrames);
        bool need_initial_seek = (head < 0);

        bool need_seek = need_backward_seek || need_forward_seek || need_initial_seek;

        if (need_seek && !decode_seeking_.load() && !seek_pending_.load()) {
            Debug::Log("D3D11VideoDecoder: Seek triggered - head=" + std::to_string(head) +
                       ", target=" + std::to_string(frame_number) +
                       ", reason=" + (need_backward_seek ? "backward" : (need_forward_seek ? "forward_far" : "initial")));
            last_seek_target_ = frame_number;
            seek_pending_ = true;
            eof_reached_ = false;  // Reset EOF when seeking
            decode_cv_.notify_one();
        }

        // Wait for frame with adaptive timeout
        // B-frame codecs need longer for reordering after seek
        int timeout_ms = (codec_ctx_ && codec_ctx_->has_b_frames > 0) ? 150 : 32;

        {
            std::unique_lock<std::mutex> lock(frame_ready_mutex_);
            frame_ready_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] {
                // Wait for delay queue to fill (B-frame reordering) AND for requested frame
                bool queue_ready = !delay_queue_filling_.load();
                bool frame_ready = GetBufferedFrame(frame_number) != nullptr;
                return (queue_ready && frame_ready) || !decode_running_;
            });
        }

        // Don't return frames while delay queue is still filling
        // This prevents showing mis-ordered frames after a seek
        if (delay_queue_filling_.load()) {
            // Return last rendered frame or black while waiting for B-frame reordering
            return (last_rendered_frame_ >= 0) ? interop_->GetGLTexture() : 0;
        }

        buffered = GetBufferedFrame(frame_number);

        // For B-frame codecs: only fall back to closest if it's AHEAD of target
        // Showing an earlier frame causes flickering due to B-frame display order
        if (!buffered) {
            BufferedFrame* closest = GetClosestBufferedFrame(frame_number);
            if (closest) {
                // Only use closest if it's at or ahead of target (forward progress)
                // For B-frame codecs, showing earlier frames causes visible reordering artifacts
                bool is_bframe_codec = codec_ctx_ && codec_ctx_->has_b_frames > 0;
                if (!is_bframe_codec || closest->frame_number >= frame_number) {
                    buffered = closest;
                    Debug::Log("D3D11VideoDecoder: Fell back to closest frame " +
                               std::to_string(closest->frame_number) + " for requested " +
                               std::to_string(frame_number));
                }
                // For B-frame codecs with only earlier frames: return last rendered
                // This prevents showing out-of-order frames
            }
        }
    }

    // If still no frame, return last rendered or 0
    if (!buffered) {
        return (last_rendered_frame_ >= 0) ? interop_->GetGLTexture() : 0;
    }

    // Ensure interop texture exists
    if (!EnsureInteropTexture()) {
        return 0;
    }

    // Lock interop for D3D11 rendering
    if (!interop_->LockForD3D11()) {
        return 0;
    }

    // Render YUV to RGB
    YUVRenderParams params;
    params.width = width_;
    params.height = height_;
    // Use actual bit depth from frame (important for 12-bit content)
    params.bit_depth = buffered->bit_depth > 0 ? buffered->bit_depth : (is_10bit_ ? 10 : 8);
    params.is_hdr = is_hdr_;
    params.is_full_range = GetEffectiveFullRange();  // Use override if set
    params.use_texture_array = buffered->is_hw_frame;
    params.color_space = is_bt2020_ ? YUVColorSpace::BT_2020 : YUVColorSpace::BT_709;

    bool render_ok = false;

    if (buffered->is_hw_frame) {
        // HW frame - use pre-created SRVs from our copied texture
        params.plane_count = 2;  // HW decode is always NV12/P010
        params.use_texture_array = false;  // We copied to a non-array texture

        if (buffered->hw_copied && buffered->hw_srv_y && buffered->hw_srv_uv) {
            // Use our copied texture's SRVs
            render_ok = yuv_renderer_->Render(buffered->hw_srv_y.Get(), buffered->hw_srv_uv.Get(),
                                              interop_->GetRTV(), params);
        } else {
            // Fallback to original texture array (shouldn't happen with new code)
            srv_y_.Reset();
            srv_uv_.Reset();
            if (!CreatePlaneSRVs(buffered->hw_texture.Get(), buffered->texture_array_index,
                                 srv_y_.GetAddressOf(), srv_uv_.GetAddressOf())) {
                interop_->UnlockForGL();
                return 0;
            }
            params.use_texture_array = true;
            render_ok = yuv_renderer_->Render(srv_y_.Get(), srv_uv_.Get(), interop_->GetRTV(), params);
        }
    } else if (buffered->plane_count == 4) {
        // SW frame with 4-plane upload (YUVA or GBRAP)
        if (!buffered->plane_srvs[0] || !buffered->plane_srvs[1] ||
            !buffered->plane_srvs[2] || !buffered->plane_srvs[3]) {
            Debug::Log("D3D11VideoDecoder: Missing plane SRVs for 4-plane frame");
            interop_->UnlockForGL();
            return 0;
        }
        params.plane_count = 4;
        params.has_alpha = buffered->has_alpha;
        params.is_rgb_planar = buffered->is_rgb_planar;

        // Log first 4-plane render from decoder side
        static bool first_4plane_log = true;
        if (first_4plane_log) {
            Debug::Log("D3D11VideoDecoder: 4-plane render - has_alpha=" +
                       std::to_string(params.has_alpha) +
                       " is_rgb_planar=" + std::to_string(params.is_rgb_planar) +
                       " bit_depth=" + std::to_string(params.bit_depth) +
                       " (frame=" + std::to_string(buffered->bit_depth) + ")" +
                       " is_full_range=" + std::to_string(params.is_full_range));
            first_4plane_log = false;
        }
        render_ok = yuv_renderer_->Render(
            buffered->plane_srvs[0].Get(),
            buffered->plane_srvs[1].Get(),
            buffered->plane_srvs[2].Get(),
            buffered->plane_srvs[3].Get(),
            interop_->GetRTV(), params);
    } else if (buffered->plane_count == 3) {
        // SW frame with direct 3-plane upload (YUV420P, YUV422P, YUV444P, GBRP)
        if (!buffered->plane_srvs[0] || !buffered->plane_srvs[1] || !buffered->plane_srvs[2]) {
            Debug::Log("D3D11VideoDecoder: Missing plane SRVs for 3-plane frame");
            interop_->UnlockForGL();
            return 0;
        }
        params.plane_count = 3;
        params.is_rgb_planar = buffered->is_rgb_planar;

        // Log first 3-plane render from decoder side
        static bool first_3plane_log = true;
        if (first_3plane_log) {
            Debug::Log("D3D11VideoDecoder: 3-plane render - is_rgb_planar=" +
                       std::to_string(params.is_rgb_planar) +
                       " bit_depth=" + std::to_string(params.bit_depth) +
                       " (frame=" + std::to_string(buffered->bit_depth) + ")" +
                       " is_full_range=" + std::to_string(params.is_full_range));
            first_3plane_log = false;
        }
        render_ok = yuv_renderer_->Render(
            buffered->plane_srvs[0].Get(),
            buffered->plane_srvs[1].Get(),
            buffered->plane_srvs[2].Get(),
            interop_->GetRTV(), params);
    } else if (buffered->plane_count == 2) {
        // SW frame with direct 2-plane upload (NV12/P010)
        if (!buffered->plane_srvs[0] || !buffered->plane_srvs[1]) {
            Debug::Log("D3D11VideoDecoder: Missing plane SRVs for 2-plane frame");
            interop_->UnlockForGL();
            return 0;
        }
        params.plane_count = 2;
        render_ok = yuv_renderer_->Render(
            buffered->plane_srvs[0].Get(),
            buffered->plane_srvs[1].Get(),
            interop_->GetRTV(), params);
    } else {
        // Legacy path - use sw_srv_y/sw_srv_uv (fallback)
        if (!buffered->sw_srv_y || !buffered->sw_srv_uv) {
            Debug::Log("D3D11VideoDecoder: Missing legacy SRVs");
            interop_->UnlockForGL();
            return 0;
        }
        params.plane_count = 2;
        render_ok = yuv_renderer_->Render(
            buffered->sw_srv_y.Get(),
            buffered->sw_srv_uv.Get(),
            interop_->GetRTV(), params);
    }

    interop_->UnlockForGL();

    if (!render_ok) {
        return 0;
    }

    // Debug: log if we're rendering a different frame than requested
    if (buffered->frame_number != frame_number) {
        static int mismatch_count = 0;
        if (mismatch_count++ < 50) {
            Debug::Log("D3D11VideoDecoder: MISMATCH - requested=" + std::to_string(frame_number) +
                       " rendering=" + std::to_string(buffered->frame_number));
        }
    }

    last_rendered_frame_ = buffered->frame_number;
    // Track the REQUESTED frame so subsequent requests for same frame use cache
    // (even if we fell back to a different frame due to B-frame delay)
    last_requested_frame_ = frame_number;
    return interop_->GetGLTexture();
}

//=============================================================================
// GetFrameAsD3D11SRV - For Unified Dual View Compositor
//=============================================================================

void D3D11VideoDecoder::SetExternalCompositorMode(bool enabled) {
    use_external_compositor_ = enabled;

    // If enabling and intermediate texture doesn't exist, it will be created on demand
    if (!enabled) {
        // Clean up intermediate resources when disabled
        intermediate_srv_.Reset();
        intermediate_rtv_.Reset();
        intermediate_texture_.Reset();
        intermediate_width_ = 0;
        intermediate_height_ = 0;
        last_srv_rendered_frame_ = -1;
        last_srv_requested_frame_ = -1;
    }
}

ID3D11ShaderResourceView* D3D11VideoDecoder::GetFrameAsD3D11SRV(int frame_number) {
    if (!initialized_) {
        return nullptr;
    }

    // Clamp frame number
    frame_number = std::clamp(frame_number, 0,
                              frame_count_ > 0 ? frame_count_ - 1 : 0);

    // Return cached SRV if same frame REQUESTED (even if we rendered a fallback frame)
    // This prevents infinite seek loops when B-frame delay means frame 0 isn't immediately available
    if (frame_number == last_srv_requested_frame_ && intermediate_srv_) {
        return intermediate_srv_.Get();
    }

    // Update decode target and playhead for background thread
    current_playhead_ = frame_number;
    decode_target_ = frame_number;
    decode_cv_.notify_one();

    // Check if frame is in buffer
    BufferedFrame* buffered = GetBufferedFrame(frame_number);

    // Debug: log initial state periodically
    static int srv_log_count = 0;
    bool should_log = (++srv_log_count % 50 == 1);

    if (should_log) {
        Debug::Log("GetFrameAsD3D11SRV: frame=" + std::to_string(frame_number) +
                   " buffered=" + (buffered ? "yes" : "no") +
                   " delay_filling=" + (delay_queue_filling_.load() ? "yes" : "no") +
                   " head=" + std::to_string(decode_head_.load()) +
                   " buffer_count=" + std::to_string(buffer_count_));
    }

    if (!buffered) {
        // Check if seek is needed (same logic as GetFrameAsGLTexture)
        int head = decode_head_.load();
        constexpr int kSeekThresholdFrames = 60;

        bool need_backward_seek = (head >= 0) && (frame_number < head - kFrameBufferSize);
        bool need_forward_seek = (head >= 0) && (frame_number > head + kSeekThresholdFrames);
        bool need_initial_seek = (head < 0);

        bool need_seek = need_backward_seek || need_forward_seek || need_initial_seek;

        if (need_seek && !decode_seeking_.load() && !seek_pending_.load()) {
            last_seek_target_ = frame_number;
            seek_pending_ = true;
            eof_reached_ = false;
            decode_cv_.notify_one();
        }

        // Wait for frame
        int timeout_ms = (codec_ctx_ && codec_ctx_->has_b_frames > 0) ? 150 : 32;
        {
            std::unique_lock<std::mutex> lock(frame_ready_mutex_);
            frame_ready_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] {
                bool queue_ready = !delay_queue_filling_.load();
                bool frame_ready = GetBufferedFrame(frame_number) != nullptr;
                return (queue_ready && frame_ready) || !decode_running_;
            });
        }

        if (delay_queue_filling_.load()) {
            if (should_log) {
                Debug::Log("GetFrameAsD3D11SRV: returning null - delay queue filling");
            }
            return (last_srv_rendered_frame_ >= 0) ? intermediate_srv_.Get() : nullptr;
        }

        buffered = GetBufferedFrame(frame_number);

        if (!buffered) {
            BufferedFrame* closest = GetClosestBufferedFrame(frame_number);
            if (closest) {
                bool is_bframe_codec = codec_ctx_ && codec_ctx_->has_b_frames > 0;
                if (!is_bframe_codec || closest->frame_number >= frame_number) {
                    buffered = closest;
                }
            }
        }
    }

    if (!buffered) {
        if (should_log) {
            Debug::Log("GetFrameAsD3D11SRV: returning null - no buffered frame after wait");
        }
        return (last_srv_rendered_frame_ >= 0) ? intermediate_srv_.Get() : nullptr;
    }

    // Ensure intermediate texture exists and matches video dimensions
    if (!intermediate_texture_ ||
        intermediate_width_ != width_ ||
        intermediate_height_ != height_) {

        intermediate_srv_.Reset();
        intermediate_rtv_.Reset();
        intermediate_texture_.Reset();

        D3D11_TEXTURE2D_DESC texDesc = {};
        texDesc.Width = width_;
        texDesc.Height = height_;
        texDesc.MipLevels = 1;
        texDesc.ArraySize = 1;
        texDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;  // HDR-ready
        texDesc.SampleDesc.Count = 1;
        texDesc.Usage = D3D11_USAGE_DEFAULT;
        texDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

        HRESULT hr = device_->CreateTexture2D(&texDesc, nullptr, &intermediate_texture_);
        if (FAILED(hr)) {
            Debug::Log("D3D11VideoDecoder: Failed to create intermediate texture");
            return nullptr;
        }

        hr = device_->CreateRenderTargetView(intermediate_texture_.Get(), nullptr, &intermediate_rtv_);
        if (FAILED(hr)) {
            Debug::Log("D3D11VideoDecoder: Failed to create intermediate RTV");
            intermediate_texture_.Reset();
            return nullptr;
        }

        hr = device_->CreateShaderResourceView(intermediate_texture_.Get(), nullptr, &intermediate_srv_);
        if (FAILED(hr)) {
            Debug::Log("D3D11VideoDecoder: Failed to create intermediate SRV");
            intermediate_rtv_.Reset();
            intermediate_texture_.Reset();
            return nullptr;
        }

        intermediate_width_ = width_;
        intermediate_height_ = height_;

        Debug::Log("D3D11VideoDecoder: Created intermediate texture " +
                   std::to_string(width_) + "x" + std::to_string(height_));
    }

    // Render YUV to RGB on intermediate texture (same logic as GetFrameAsGLTexture)
    YUVRenderParams params;
    params.width = width_;
    params.height = height_;
    params.bit_depth = buffered->bit_depth > 0 ? buffered->bit_depth : (is_10bit_ ? 10 : 8);
    params.is_hdr = is_hdr_;
    params.is_full_range = GetEffectiveFullRange();
    params.use_texture_array = buffered->is_hw_frame;
    params.color_space = is_bt2020_ ? YUVColorSpace::BT_2020 : YUVColorSpace::BT_709;

    bool render_ok = false;

    if (buffered->is_hw_frame) {
        params.plane_count = 2;
        params.use_texture_array = false;

        if (buffered->hw_copied && buffered->hw_srv_y && buffered->hw_srv_uv) {
            render_ok = yuv_renderer_->Render(buffered->hw_srv_y.Get(), buffered->hw_srv_uv.Get(),
                                              intermediate_rtv_.Get(), params);
        } else {
            srv_y_.Reset();
            srv_uv_.Reset();
            if (!CreatePlaneSRVs(buffered->hw_texture.Get(), buffered->texture_array_index,
                                 srv_y_.GetAddressOf(), srv_uv_.GetAddressOf())) {
                return nullptr;
            }
            params.use_texture_array = true;
            render_ok = yuv_renderer_->Render(srv_y_.Get(), srv_uv_.Get(),
                                              intermediate_rtv_.Get(), params);
        }
    } else if (buffered->plane_count == 4) {
        if (!buffered->plane_srvs[0] || !buffered->plane_srvs[1] ||
            !buffered->plane_srvs[2] || !buffered->plane_srvs[3]) {
            return nullptr;
        }
        params.plane_count = 4;
        params.has_alpha = buffered->has_alpha;
        params.is_rgb_planar = buffered->is_rgb_planar;
        render_ok = yuv_renderer_->Render(
            buffered->plane_srvs[0].Get(),
            buffered->plane_srvs[1].Get(),
            buffered->plane_srvs[2].Get(),
            buffered->plane_srvs[3].Get(),
            intermediate_rtv_.Get(), params);
    } else if (buffered->plane_count == 3) {
        if (!buffered->plane_srvs[0] || !buffered->plane_srvs[1] || !buffered->plane_srvs[2]) {
            return nullptr;
        }
        params.plane_count = 3;
        params.is_rgb_planar = buffered->is_rgb_planar;
        render_ok = yuv_renderer_->Render(
            buffered->plane_srvs[0].Get(),
            buffered->plane_srvs[1].Get(),
            buffered->plane_srvs[2].Get(),
            intermediate_rtv_.Get(), params);
    } else if (buffered->plane_count == 2) {
        if (!buffered->plane_srvs[0] || !buffered->plane_srvs[1]) {
            return nullptr;
        }
        params.plane_count = 2;
        render_ok = yuv_renderer_->Render(
            buffered->plane_srvs[0].Get(),
            buffered->plane_srvs[1].Get(),
            intermediate_rtv_.Get(), params);
    } else {
        if (!buffered->sw_srv_y || !buffered->sw_srv_uv) {
            return nullptr;
        }
        params.plane_count = 2;
        render_ok = yuv_renderer_->Render(
            buffered->sw_srv_y.Get(),
            buffered->sw_srv_uv.Get(),
            intermediate_rtv_.Get(), params);
    }

    if (!render_ok) {
        return nullptr;
    }

    last_srv_rendered_frame_ = buffered->frame_number;
    // Track the REQUESTED frame so subsequent requests for same frame use cache
    // (even if we fell back to a different frame due to B-frame delay)
    // Note: frame_number was captured at function entry before any fallback
    last_srv_requested_frame_ = frame_number;
    return intermediate_srv_.Get();
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
    // Only wake decode thread if playhead actually changed or we're force seeking
    // This prevents CPU spin when paused on the same frame
    int prev_playhead = current_playhead_.load();
    bool playhead_changed = (frame_number != prev_playhead);

    current_playhead_ = frame_number;
    decode_target_ = frame_number;

    // If playhead didn't change and not force seeking, skip all work
    // This is the common case when paused - called 60x/sec but nothing to do
    if (!playhead_changed && !force_seek) {
        return;
    }

    // Check if we're within a reasonable decode range of current position
    // Don't seek if decoder is already close to target
    // For B-frame codecs, we need larger backward tolerance since early frames
    // may not be available immediately after seek (e.g., frame 0 may not be
    // available until frame 4+ is decoded due to reordering delay)
    int head = decode_head_.load();
    int backward_tolerance = (codec_ctx_ && codec_ctx_->has_b_frames > 0) ? 16 : 4;
    bool in_decode_range = (head >= 0) &&
                           (frame_number >= head - backward_tolerance) &&
                           (frame_number <= head + 60);   // Within forward decode range

    // Skip seek if we're in decode range and not force seeking
    if (in_decode_range && !force_seek) {
        decode_cv_.notify_one();  // Wake decoder to continue filling buffer
        return;
    }

    bool has_frame = BufferContainsFrame(frame_number);

    // Only trigger seek if:
    // 1. force_seek is true OR frame is not in buffer AND not in decode range
    // 2. AND no seek is already pending or in progress
    // 3. AND we haven't already sought for this exact target recently
    bool should_seek = force_seek || (!has_frame && !in_decode_range);

    // Prevent re-seeking for same target that we just sought to
    // (covers B-frame case where target frame 0 may never be directly available)
    if (should_seek && frame_number == last_seek_target_.load() && !force_seek) {
        // Already sought for this target - don't seek again
        return;
    }

    if (should_seek && !seek_pending_.load() && !decode_seeking_.load()) {
        last_seek_target_ = frame_number;
        seek_pending_ = true;
    }

    // Wake up decode thread only if we have actual work
    decode_cv_.notify_one();
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
    last_requested_frame_ = -1;
    last_srv_rendered_frame_ = -1;
    last_srv_requested_frame_ = -1;
    decode_head_ = -1;
    decode_target_ = target_frame;

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
    Debug::Log("D3D11VideoDecoder: Decode thread started");

    int frames_decoded = 0;
    int status_counter = 0;
    auto last_status_time = std::chrono::steady_clock::now();

    while (decode_running_) {
        // Calculate timeout based on frame rate (reduces CPU usage when idle)
        // Use ~1 frame duration, with bounds: min 15ms, max 30ms (for low/unknown fps)
        int timeout_ms = (fps_ > 0) ? std::max(15, std::min(30, static_cast<int>(1000.0 / fps_))) : 30;

        // Wait for work
        {
            std::unique_lock<std::mutex> lock(decode_mutex_);
            decode_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this] {
                // Don't wake up if we've hit EOF and no seek is pending
                if (eof_reached_ && !seek_pending_) return false;
                return !decode_running_ || seek_pending_ || NeedsMoreFrames();
            });
        }

        if (!decode_running_) break;

        // Handle seek first - this resets EOF state
        if (seek_pending_) {
            Debug::Log("D3D11VideoDecoder: DecodeThread handling seek to frame " + std::to_string(last_seek_target_.load()) +
                       " (" + std::to_string(width_) + "x" + std::to_string(height_) + ")");
            eof_reached_ = false;  // Reset EOF on seek
            consecutive_decode_failures_ = 0;  // Reset failure counter
            PerformSeekInternal(last_seek_target_.load());
            seek_pending_ = false;
            frames_decoded = 0;  // Reset counter after seek
            frame_ready_cv_.notify_all();
            continue;
        }

        // Don't try to decode if we've already hit EOF
        if (eof_reached_) {
            continue;
        }

        // Decode one frame if buffer needs it
        if (NeedsMoreFrames()) {
            if (DecodeNextFrame()) {
                AddCurrentFrameToBuffer();
                frame_ready_cv_.notify_one();
                consecutive_decode_failures_ = 0;  // Reset on success
                frames_decoded++;

                // Periodic status logging (every 100 frames or 2 seconds)
                status_counter++;
                auto now = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_status_time).count();
                if (status_counter >= 100 || elapsed >= 2000) {
                    int target = decode_target_.load();
                    int head = decode_head_.load();
                    Debug::Log("D3D11VideoDecoder: Decoded " + std::to_string(frames_decoded) +
                               " frames, target=" + std::to_string(target) +
                               ", head=" + std::to_string(head) +
                               ", current=" + std::to_string(current_frame_number_));
                    status_counter = 0;
                    last_status_time = now;
                }
            } else {
                // DecodeNextFrame returned false - could be EOF or temporary error
                consecutive_decode_failures_++;

                // Log every failure to help debug
                Debug::Log("D3D11VideoDecoder: Decode failed, consecutive=" +
                           std::to_string(consecutive_decode_failures_) +
                           ", current_frame=" + std::to_string(current_frame_number_));

                if (consecutive_decode_failures_ >= 10) {
                    eof_reached_ = true;
                    Debug::Log("D3D11VideoDecoder: EOF reached after " +
                               std::to_string(consecutive_decode_failures_) +
                               " consecutive failures, decoded " + std::to_string(frames_decoded) + " frames total");
                }
            }
        }
    }

    Debug::Log("D3D11VideoDecoder: Decode thread stopped, decoded " + std::to_string(frames_decoded) + " frames total");
}

//=============================================================================
// Async Decode Helpers
//=============================================================================

bool D3D11VideoDecoder::NeedsMoreFrames() const {
    int target = decode_target_.load();

    std::lock_guard<std::mutex> lock(buffer_mutex_);

    // Count frames at or ahead of target
    int ahead = 0;
    int valid_count = 0;
    for (int i = 0; i < buffer_count_; i++) {
        int idx = (buffer_head_ + i) % kFrameBufferSize;
        if (frame_buffer_[idx].valid) {
            valid_count++;
            if (frame_buffer_[idx].frame_number >= target) {
                ahead++;
            }
        }
    }

    bool needs = ahead < kFrameBufferSize / 2;

    // Debug: log occasionally when we think we need frames
    static int log_counter = 0;
    if (needs && (++log_counter % 100 == 0)) {
        //Debug::Log("D3D11VideoDecoder: NeedsMoreFrames=true, target=" +
        //           std::to_string(target) + ", ahead=" + std::to_string(ahead) +
        //           ", valid=" + std::to_string(valid_count) +
        //           ", buffer_count=" + std::to_string(buffer_count_));
    }

    return needs;
}

void D3D11VideoDecoder::AddCurrentFrameToBuffer() {
    int insert_idx = -1;
    BufferedFrame* bf = nullptr;
    bool was_full = false;
    int old_frame_number = -1;

    // Debug: log what we're trying to add
    static int add_log_count = 0;
    bool should_log_add = (++add_log_count % 50 == 1);
    if (should_log_add) {
        Debug::Log("AddCurrentFrameToBuffer: frame=" + std::to_string(current_frame_number_) +
                   " decode_mode=" + std::to_string(static_cast<int>(decode_mode_)) +
                   " frame_format=" + std::to_string(current_frame_ ? current_frame_->format : -1) +
                   " AV_PIX_FMT_D3D11=" + std::to_string(AV_PIX_FMT_D3D11));
    }

    // Reserve a slot under lock
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);

        // O(1) duplicate check using frame_map_
        auto it = frame_map_.find(current_frame_number_);
        if (it != frame_map_.end()) {
            int idx = it->second;
            if (frame_buffer_[idx].valid && frame_buffer_[idx].frame_number == current_frame_number_) {
                // Already have this frame successfully, skip
                if (should_log_add) Debug::Log("AddCurrentFrameToBuffer: skipping duplicate");
                return;
            }
        }

        // Find a slot to use
        if (buffer_count_ < kFrameBufferSize) {
            // Buffer not full - use next slot
            insert_idx = (buffer_head_ + buffer_count_) % kFrameBufferSize;
            // Don't increment buffer_count_ yet - wait for successful upload
        } else {
            // Buffer full - reuse oldest slot
            insert_idx = buffer_head_;
            was_full = true;
        }
        bf = &frame_buffer_[insert_idx];

        // Mark slot invalid BEFORE upload to prevent render thread from using stale data
        // while we're modifying the textures
        if (was_full && bf->valid) {
            old_frame_number = bf->frame_number;
            bf->valid = false;
            // Remove old frame from map
            frame_map_.erase(old_frame_number);
        }
    }

    // Do the upload outside the lock
    bool upload_ok = false;
    if (decode_mode_ == DecodeMode::HARDWARE &&
        current_frame_->format == AV_PIX_FMT_D3D11) {
        // CRITICAL: Copy HW texture to our own texture to prevent D3D11VA pool reuse
        // D3D11VA recycles texture slots, so we must copy immediately
        ID3D11Texture2D* src_texture = (ID3D11Texture2D*)current_frame_->data[0];
        int src_array_index = (int)(intptr_t)current_frame_->data[1];

        // Create or reuse our local texture
        D3D11_TEXTURE2D_DESC src_desc;
        src_texture->GetDesc(&src_desc);

        if (!bf->hw_texture) {
            // Create a non-array texture for our copy
            D3D11_TEXTURE2D_DESC desc = src_desc;
            desc.ArraySize = 1;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            desc.MiscFlags = 0;
            HRESULT hr = device_->CreateTexture2D(&desc, nullptr, bf->hw_texture.GetAddressOf());
            if (FAILED(hr)) {
                Debug::Log("D3D11VideoDecoder: Failed to create HW copy texture");
                bf->hw_texture.Reset();
            }
        }

        if (bf->hw_texture) {
            // Copy from texture array slice to our texture (D3D11 multithread protection enabled)
            context_->CopySubresourceRegion(
                bf->hw_texture.Get(), 0,  // dest texture, subresource 0
                0, 0, 0,                   // dest x, y, z
                src_texture, src_array_index,  // src texture, array slice
                nullptr                    // copy entire subresource
            );

            // Create SRVs for Y and UV planes
            bf->hw_srv_y.Reset();
            bf->hw_srv_uv.Reset();

            DXGI_FORMAT srv_format_y = (src_desc.Format == DXGI_FORMAT_P010) ?
                                       DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM;
            DXGI_FORMAT srv_format_uv = (src_desc.Format == DXGI_FORMAT_P010) ?
                                        DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R8G8_UNORM;

            D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
            srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            srv_desc.Texture2D.MipLevels = 1;
            srv_desc.Texture2D.MostDetailedMip = 0;

            srv_desc.Format = srv_format_y;
            device_->CreateShaderResourceView(bf->hw_texture.Get(), &srv_desc, bf->hw_srv_y.GetAddressOf());

            srv_desc.Format = srv_format_uv;
            device_->CreateShaderResourceView(bf->hw_texture.Get(), &srv_desc, bf->hw_srv_uv.GetAddressOf());

            bf->is_hw_frame = true;
            bf->hw_copied = true;
            bf->plane_count = 2;
            bf->bit_depth = is_10bit_ ? 10 : 8;
            bf->is_nv12_layout = true;
            upload_ok = (bf->hw_srv_y && bf->hw_srv_uv);
        }
    } else {
        // Software decode - upload to this slot's own staging textures
        if (UploadSoftwareFrameToSlot(current_frame_, *bf)) {
            bf->is_hw_frame = false;
            upload_ok = true;
        } else {
            Debug::Log("D3D11VideoDecoder: Upload failed for frame " + std::to_string(current_frame_number_));
        }
    }

    // Debug: log upload result
    if (should_log_add) {
        Debug::Log("AddCurrentFrameToBuffer: upload_ok=" + std::string(upload_ok ? "yes" : "no"));
    }

    // Finalize under lock
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        if (upload_ok) {
            bf->frame_number = current_frame_number_;
            bf->pts = current_frame_->pts != AV_NOPTS_VALUE ?
                      current_frame_->pts * av_q2d(time_base_) : 0.0;
            bf->valid = true;
            decode_head_ = current_frame_number_;

            // Update frame_map_ with new frame
            frame_map_[current_frame_number_] = insert_idx;

            // Only update counts on success
            if (!was_full) {
                buffer_count_++;
            } else {
                // Advance head since we consumed the oldest slot
                buffer_head_ = (buffer_head_ + 1) % kFrameBufferSize;
            }

            // Track delay queue for B-frame reordering
            // Use dynamic depth based on actual B-frame count (HEVC can have 4-8)
            int count = ++frames_since_seek_;
            int delay_depth = kDelayQueueDepth;
            if (codec_ctx_ && codec_ctx_->has_b_frames > 0) {
                // Need at least has_b_frames + 2 to ensure proper reordering
                delay_depth = std::max(kDelayQueueDepth, codec_ctx_->has_b_frames + 2);
            }
            if (delay_queue_filling_.load() && count >= delay_depth) {
                delay_queue_filling_ = false;
                Debug::Log("D3D11VideoDecoder: Delay queue filled after " +
                           std::to_string(count) + " frames (depth=" +
                           std::to_string(delay_depth) + "), buffer has " +
                           std::to_string(buffer_count_) + " frames");
                // Notify render thread that frames are now ready
                frame_ready_cv_.notify_all();
            }
        } else if (was_full && old_frame_number >= 0) {
            // Upload failed while overwriting - restore the old frame so it's still usable
            // The texture data is unchanged (upload failed), so we can restore validity
            bf->frame_number = old_frame_number;
            bf->valid = true;
            // Restore old frame in map
            frame_map_[old_frame_number] = insert_idx;
        }
        // If upload failed on a new slot (was_full=false), slot stays empty and can be retried
    }
}

void D3D11VideoDecoder::PerformSeekInternal(int target_frame) {
    Debug::Log("D3D11VideoDecoder: PerformSeekInternal to frame " + std::to_string(target_frame) +
               " (" + std::to_string(width_) + "x" + std::to_string(height_) + ")");
    decode_seeking_ = true;

    ClearFrameBuffer();

    double target_time = target_frame / fps_;
    int64_t target_pts = (int64_t)(target_time / av_q2d(time_base_));

    avcodec_flush_buffers(codec_ctx_);
    av_seek_frame(format_ctx_, video_stream_index_, target_pts, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(codec_ctx_);

    // Reset delay queue state for B-frame reordering
    frames_since_seek_ = 0;
    if (codec_ctx_->has_b_frames > 0) {
        delay_queue_filling_ = true;
    }

    current_frame_number_ = -1;
    // Set decode_head_ to target to prevent immediate re-seek
    // (head < 0 check in GetFrameAsGLTexture would trigger another seek)
    decode_head_ = target_frame;
    eof_reached_ = false;  // Reset EOF on seek
    decode_seeking_ = false;
}

D3D11VideoDecoder::BufferedFrame* D3D11VideoDecoder::GetClosestBufferedFrame(int frame_number) {
    std::lock_guard<std::mutex> lock(buffer_mutex_);

    BufferedFrame* closest = nullptr;
    int min_diff = INT_MAX;

    for (int i = 0; i < buffer_count_; i++) {
        int idx = (buffer_head_ + i) % kFrameBufferSize;
        if (frame_buffer_[idx].valid) {
            int diff = std::abs(frame_buffer_[idx].frame_number - frame_number);
            if (diff < min_diff) {
                min_diff = diff;
                closest = &frame_buffer_[idx];
            }
        }
    }

    return closest;
}

} // namespace ump

#endif // _WIN32
