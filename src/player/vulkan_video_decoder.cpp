#include "vulkan_video_decoder.h"

#ifdef __linux__

#include "../gpu/vulkan_texture_pool.h"
#include "../gpu/vulkan_device.h"
#include "../utils/debug_utils.h"
#include "hw_context_manager.h"

#include <cstdlib>
#include <climits>
#include <algorithm>
#include <pthread.h>

extern "C" {
#include <libavutil/imgutils.h>
}

namespace qcview {

//=============================================================================
// Constructor / Destructor
//=============================================================================

VulkanVideoDecoder::VulkanVideoDecoder() {
}

VulkanVideoDecoder::~VulkanVideoDecoder() {
    Shutdown();
}

//=============================================================================
// Codec Detection
//=============================================================================

bool VulkanVideoDecoder::SupportsHardwareDecode(AVCodecID codec_id) const {
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

VulkanVideoDecoder::DecodeMode VulkanVideoDecoder::DetermineDecodeMode(AVCodecID codec_id) const {
    if (force_software_) {
        Debug::Log("VulkanVideoDecoder: Software decode forced by user setting");
        return DecodeMode::SOFTWARE;
    }
    if (SupportsHardwareDecode(codec_id)) {
        return DecodeMode::HARDWARE;
    }
    return DecodeMode::SOFTWARE;
}

//=============================================================================
// Initialize
//=============================================================================

bool VulkanVideoDecoder::Initialize() {
    if (initialized_) return true;

    if (video_path_.empty()) {
        Debug::Log("VulkanVideoDecoder: Video path not set");
        return false;
    }

    if (!InitializeFFmpeg()) {
        Debug::Log("VulkanVideoDecoder: Failed to initialize FFmpeg");
        Shutdown();
        return false;
    }

    decode_mode_ = DetermineDecodeMode(codec_ctx_->codec_id);

    if (decode_mode_ == DecodeMode::HARDWARE) {
        if (!ConfigureHardwareContext()) {
            Debug::Log("VulkanVideoDecoder: HW context failed, falling back to software");
            decode_mode_ = DecodeMode::SOFTWARE;
            hw_accel_type_ = HWAccelType::NONE;
        } else {
            hw_accel_type_ = HWAccelType::VAAPI;
        }
    }

    if (decode_mode_ == DecodeMode::SOFTWARE) {
        if (!ConfigureSoftwareContext()) {
            Debug::Log("VulkanVideoDecoder: Failed to configure software context");
            Shutdown();
            return false;
        }
    }

    if (!OpenCodec()) {
        // HW codec open failed — fall back to software
        if (decode_mode_ == DecodeMode::HARDWARE) {
            Debug::Log("VulkanVideoDecoder: HW codec open failed, retrying with software");

            if (codec_ctx_) avcodec_free_context(&codec_ctx_);
            if (hw_device_ctx_) { av_buffer_unref(&hw_device_ctx_); hw_device_ctx_ = nullptr; }
            if (format_ctx_) avformat_close_input(&format_ctx_);

            decode_mode_ = DecodeMode::SOFTWARE;
            hw_accel_type_ = HWAccelType::NONE;

            if (!InitializeFFmpeg() || !ConfigureSoftwareContext() || !OpenCodec()) {
                Debug::Log("VulkanVideoDecoder: SW fallback also failed");
                Shutdown();
                return false;
            }
            Debug::Log("VulkanVideoDecoder: Fell back to software decode");
        } else {
            Debug::Log("VulkanVideoDecoder: Failed to open codec");
            Shutdown();
            return false;
        }
    }

    packet_ = av_packet_alloc();
    if (!packet_) {
        Debug::Log("VulkanVideoDecoder: Failed to allocate packet");
        Shutdown();
        return false;
    }

    // Initialize GPU decode path for HW-decoded frames
    if (decode_mode_ == DecodeMode::HARDWARE) {
        hw_frame_importer_ = std::make_unique<VulkanHWFrameImporter>();
        yuv_renderer_ = std::make_unique<VulkanYUVRenderer>();

        if (hw_frame_importer_->Initialize() && yuv_renderer_->Initialize()) {
            gpu_decode_available_ = true;
            Debug::Log("VulkanVideoDecoder: GPU decode path available (DMA-BUF + YUV shader)");
        } else {
            Debug::Log("VulkanVideoDecoder: GPU decode path not available, using CPU fallback");
            hw_frame_importer_.reset();
            yuv_renderer_.reset();
            gpu_decode_available_ = false;
        }
    }

    BuildKeyframeIndex();

    frames_since_seek_ = 0;
    if (codec_ctx_->has_b_frames > 0) {
        delay_queue_filling_ = true;
        Debug::Log("VulkanVideoDecoder: B-frame codec detected (has_b_frames=" +
                   std::to_string(codec_ctx_->has_b_frames) + ")");
    } else {
        delay_queue_filling_ = false;
    }

    // Start decode thread
    decode_running_ = true;
    decode_thread_ = std::thread(&VulkanVideoDecoder::DecodeThreadFunc, this);

    // Set higher priority for decode thread
    sched_param param;
    param.sched_priority = 0;
    pthread_setschedparam(decode_thread_.native_handle(), SCHED_OTHER, &param);

    initialized_ = true;

    Debug::Log("VulkanVideoDecoder: Initialized - " +
               std::to_string(width_) + "x" + std::to_string(height_) +
               " @ " + std::to_string(fps_) + " fps" +
               " [" + (decode_mode_ == DecodeMode::HARDWARE ? "VA-API HW" : "Software") + "]" +
               (is_hdr_ ? " [HDR]" : " [SDR]") +
               (is_10bit_ ? " [10-bit]" : " [8-bit]") +
               (codec_ctx_->has_b_frames > 0 ? " [B-frames]" : ""));

    return true;
}

//=============================================================================
// InitializeFFmpeg
//=============================================================================

bool VulkanVideoDecoder::InitializeFFmpeg() {
    int ret = avformat_open_input(&format_ctx_, video_path_.c_str(), nullptr, nullptr);
    if (ret < 0) {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        Debug::Log("VulkanVideoDecoder: Failed to open file: " + std::string(errbuf));
        return false;
    }

    ret = avformat_find_stream_info(format_ctx_, nullptr);
    if (ret < 0) {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        Debug::Log("VulkanVideoDecoder: Failed to find stream info: " + std::string(errbuf));
        return false;
    }

    video_stream_index_ = av_find_best_stream(format_ctx_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (video_stream_index_ < 0) {
        Debug::Log("VulkanVideoDecoder: No video stream found");
        return false;
    }

    AVStream* stream = format_ctx_->streams[video_stream_index_];
    AVCodecParameters* codecpar = stream->codecpar;
    time_base_ = stream->time_base;

    const AVCodec* codec = avcodec_find_decoder(codecpar->codec_id);
    if (!codec) {
        Debug::Log("VulkanVideoDecoder: Codec not found for codec_id=" + std::to_string(codecpar->codec_id));
        return false;
    }

    Debug::Log("VulkanVideoDecoder: Using codec: " + std::string(codec->name));

    is_intra_only_codec_ = (codecpar->codec_id == AV_CODEC_ID_PRORES ||
                            codecpar->codec_id == AV_CODEC_ID_DNXHD ||
                            codecpar->codec_id == AV_CODEC_ID_MJPEG ||
                            codecpar->codec_id == AV_CODEC_ID_RAWVIDEO);

    codec_ctx_ = avcodec_alloc_context3(codec);
    if (!codec_ctx_) {
        Debug::Log("VulkanVideoDecoder: Failed to allocate codec context");
        return false;
    }

    ret = avcodec_parameters_to_context(codec_ctx_, codecpar);
    if (ret < 0) {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        Debug::Log("VulkanVideoDecoder: Failed to copy codec params: " + std::string(errbuf));
        return false;
    }

    width_ = codec_ctx_->width;
    height_ = codec_ctx_->height;

    AVRational fps_rational = stream->r_frame_rate;
    if (fps_rational.num == 0 || fps_rational.den == 0) {
        fps_rational = stream->avg_frame_rate;
    }
    if (fps_rational.num > 0 && fps_rational.den > 0) {
        fps_ = av_q2d(fps_rational);
    } else {
        fps_ = 24.0;
    }

    if (stream->duration != AV_NOPTS_VALUE) {
        duration_ = stream->duration * av_q2d(stream->time_base);
    } else if (format_ctx_->duration != AV_NOPTS_VALUE) {
        duration_ = format_ctx_->duration / (double)AV_TIME_BASE;
    } else {
        duration_ = 0.0;
    }

    frame_count_ = (duration_ > 0 && fps_ > 0) ? (int)(duration_ * fps_ + 0.5) : 0;

    is_hdr_ = (codec_ctx_->color_primaries == AVCOL_PRI_BT2020) &&
              (codec_ctx_->color_trc == AVCOL_TRC_SMPTE2084 ||
               codec_ctx_->color_trc == AVCOL_TRC_ARIB_STD_B67);

    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(codec_ctx_->pix_fmt);
    is_10bit_ = desc && desc->comp[0].depth > 8;

    is_full_range_ = (codec_ctx_->color_range == AVCOL_RANGE_JPEG);

    current_frame_ = av_frame_alloc();
    sw_frame_ = av_frame_alloc();
    if (!current_frame_ || !sw_frame_) {
        Debug::Log("VulkanVideoDecoder: Failed to allocate frames");
        return false;
    }

    Debug::Log("VulkanVideoDecoder: FFmpeg initialized - " +
               std::to_string(width_) + "x" + std::to_string(height_) +
               " @ " + std::to_string(fps_) + " fps, " +
               std::to_string(frame_count_) + " frames" +
               " pix_fmt=" + std::to_string(codec_ctx_->pix_fmt));

    return true;
}

//=============================================================================
// ConfigureHardwareContext (VA-API)
//=============================================================================

bool VulkanVideoDecoder::ConfigureHardwareContext() {
    // Try to get shared VA-API context from HWContextManager
    auto& hw_mgr = HWContextManager::Instance();
    AVBufferRef* vaapi_ctx = hw_mgr.GetContext(static_cast<int>(AV_HWDEVICE_TYPE_VAAPI));

    if (vaapi_ctx) {
        // Use shared context
        hw_device_ctx_ = av_buffer_ref(vaapi_ctx);
        Debug::Log("VulkanVideoDecoder: Using shared VA-API context from HWContextManager");
    } else {
        // Create our own VA-API context
        int ret = av_hwdevice_ctx_create(&hw_device_ctx_, AV_HWDEVICE_TYPE_VAAPI,
                                          nullptr, nullptr, 0);
        if (ret < 0) {
            char errbuf[256];
            av_strerror(ret, errbuf, sizeof(errbuf));
            Debug::Log("VulkanVideoDecoder: VA-API not available: " + std::string(errbuf));
            hw_device_ctx_ = nullptr;
            return false;
        }
        Debug::Log("VulkanVideoDecoder: Created own VA-API context");
    }

    codec_ctx_->hw_device_ctx = av_buffer_ref(hw_device_ctx_);
    return true;
}

//=============================================================================
// ConfigureSoftwareContext
//=============================================================================

bool VulkanVideoDecoder::ConfigureSoftwareContext() {
    codec_ctx_->thread_count = 0;  // Auto-detect
    codec_ctx_->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;

    Debug::Log("VulkanVideoDecoder: Using auto thread_count (av_cpu_count=" +
               std::to_string(av_cpu_count()) + ")");
    return true;
}

//=============================================================================
// GetHWFormat callback
//=============================================================================

AVPixelFormat VulkanVideoDecoder::GetHWFormat(AVCodecContext* /*ctx*/, const AVPixelFormat* pix_fmts) {
    for (const AVPixelFormat* p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
        if (*p == AV_PIX_FMT_VAAPI) {
            return AV_PIX_FMT_VAAPI;
        }
    }
    return AV_PIX_FMT_NONE;
}

//=============================================================================
// OpenCodec
//=============================================================================

bool VulkanVideoDecoder::OpenCodec() {
    if (decode_mode_ == DecodeMode::HARDWARE) {
        codec_ctx_->thread_count = 1;
        codec_ctx_->thread_type = 0;
        codec_ctx_->opaque = this;
        codec_ctx_->get_format = &VulkanVideoDecoder::GetHWFormat;
    }

    int ret = avcodec_open2(codec_ctx_, nullptr, nullptr);
    if (ret < 0) {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        Debug::Log("VulkanVideoDecoder: Failed to open codec: " + std::string(errbuf));
        return false;
    }

    return true;
}

//=============================================================================
// Shutdown
//=============================================================================

void VulkanVideoDecoder::RequestShutdown() {
    if (decode_running_) {
        decode_running_ = false;
        decode_cv_.notify_all();
        frame_ready_cv_.notify_all();
    }
}

void VulkanVideoDecoder::Shutdown() {
    if (decode_running_) {
        decode_running_ = false;
        decode_cv_.notify_all();
        frame_ready_cv_.notify_all();
        if (decode_thread_.joinable()) {
            decode_thread_.join();
        }
    }

    // ClearFrameBuffer now handles pool texture cleanup
    ClearFrameBuffer();

    // Destroy GPU decode resources
    DestroyGPUOutputImage();
    if (yuv_renderer_) { yuv_renderer_->Shutdown(); yuv_renderer_.reset(); }
    if (hw_frame_importer_) { hw_frame_importer_->Shutdown(); hw_frame_importer_.reset(); }
    gpu_decode_available_ = false;

    if (sws_ctx_) {
        sws_freeContext(sws_ctx_);
        sws_ctx_ = nullptr;
    }

    if (packet_) av_packet_free(&packet_);
    if (current_frame_) av_frame_free(&current_frame_);
    if (sw_frame_) av_frame_free(&sw_frame_);
    if (codec_ctx_) avcodec_free_context(&codec_ctx_);
    if (hw_device_ctx_) { av_buffer_unref(&hw_device_ctx_); hw_device_ctx_ = nullptr; }
    if (format_ctx_) avformat_close_input(&format_ctx_);

    initialized_ = false;
    current_frame_number_ = -1;

    Debug::Log("VulkanVideoDecoder: Shutdown complete");
}

//=============================================================================
// HardReset
//=============================================================================

void VulkanVideoDecoder::HardReset(int target_frame) {
    Debug::Log("VulkanVideoDecoder: Hard reset to frame " + std::to_string(target_frame));

    std::string path = video_path_;
    auto config = config_;
    auto mode = pipeline_mode_;

    Shutdown();

    video_path_ = path;
    config_ = config;
    pipeline_mode_ = mode;

    if (Initialize()) {
        UpdatePlayhead(target_frame);
    }
}

//=============================================================================
// BuildKeyframeIndex
//=============================================================================

void VulkanVideoDecoder::BuildKeyframeIndex() {
    if (is_intra_only_codec_) {
        keyframe_index_built_ = true;
        return;
    }

    if (decode_mode_ == DecodeMode::HARDWARE) {
        keyframe_index_built_ = true;
        Debug::Log("VulkanVideoDecoder: Skipping keyframe index for HW decode mode");
        return;
    }

    keyframe_positions_.clear();
    AVPacket* pkt = av_packet_alloc();
    if (!pkt) {
        keyframe_index_built_ = true;
        return;
    }

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

    av_seek_frame(format_ctx_, video_stream_index_, 0, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(codec_ctx_);

    keyframe_index_built_ = true;
    Debug::Log("VulkanVideoDecoder: Built keyframe index with " +
               std::to_string(keyframe_positions_.size()) + " keyframes");
}

//=============================================================================
// Frame Buffer Management
//=============================================================================

void VulkanVideoDecoder::ClearFrameBuffer() {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    for (auto& frame : frame_buffer_) {
        if (frame.pool_texture_id != 0) {
            VulkanTexturePool::Instance().QueueDelete(frame.pool_texture_id);
            frame.pool_texture_id = 0;
        }
        frame.Reset();
    }
    buffer_head_ = 0;
    buffer_count_ = 0;
    frame_map_.clear();
}

bool VulkanVideoDecoder::BufferContainsFrame(int frame_number) const {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    auto it = frame_map_.find(frame_number);
    if (it != frame_map_.end()) {
        int idx = it->second;
        return frame_buffer_[idx].valid && frame_buffer_[idx].frame_number == frame_number;
    }
    return false;
}

VulkanVideoDecoder::BufferedFrame* VulkanVideoDecoder::GetBufferedFrame(int frame_number) {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    auto it = frame_map_.find(frame_number);
    if (it != frame_map_.end()) {
        int idx = it->second;
        if (frame_buffer_[idx].valid && frame_buffer_[idx].frame_number == frame_number) {
            return &frame_buffer_[idx];
        }
    }
    return nullptr;
}

VulkanVideoDecoder::BufferedFrame* VulkanVideoDecoder::GetClosestBufferedFrame(int frame_number) {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    BufferedFrame* closest = nullptr;
    int min_dist = INT_MAX;

    for (int i = 0; i < buffer_count_; i++) {
        int idx = (buffer_head_ + i) % kFrameBufferSize;
        if (frame_buffer_[idx].valid) {
            int dist = std::abs(frame_buffer_[idx].frame_number - frame_number);
            if (dist < min_dist) {
                min_dist = dist;
                closest = &frame_buffer_[idx];
            }
        }
    }
    return closest;
}

//=============================================================================
// ConvertFrameToRGBA — CPU sws_scale conversion
//=============================================================================

bool VulkanVideoDecoder::ConvertFrameToRGBA(AVFrame* frame, std::shared_ptr<PixelData>& out) {
    if (!frame || frame->width <= 0 || frame->height <= 0) return false;

    int w = frame->width;
    int h = frame->height;

    // Determine output format based on bit depth
    AVPixelFormat src_fmt = static_cast<AVPixelFormat>(frame->format);
    AVPixelFormat dst_fmt;
    PixelFormat pixel_fmt;

    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(src_fmt);
    int bit_depth = desc ? desc->comp[0].depth : 8;

    if (bit_depth > 8) {
        // 10/12/16-bit → RGBA16 (RGBA64LE)
        dst_fmt = AV_PIX_FMT_RGBA64LE;
        pixel_fmt = PixelFormat::RGBA16;
    } else {
        // 8-bit → RGBA8
        dst_fmt = AV_PIX_FMT_RGBA;
        pixel_fmt = PixelFormat::RGBA8;
    }

    size_t bpp = PixelFormatBytesPerPixel(pixel_fmt);

    // Get or recreate sws context
    sws_ctx_ = sws_getCachedContext(sws_ctx_,
        w, h, src_fmt,
        w, h, dst_fmt,
        SWS_BILINEAR, nullptr, nullptr, nullptr);

    if (!sws_ctx_) {
        Debug::Log("VulkanVideoDecoder: Failed to create sws context for fmt=" +
                   std::to_string(src_fmt));
        return false;
    }

    // Allocate output
    out = std::make_shared<PixelData>();
    out->width = w;
    out->height = h;
    out->SetFormat(pixel_fmt);
    out->pipeline_mode = pipeline_mode_;
    out->pixels.resize(static_cast<size_t>(w) * h * bpp);

    // Convert
    uint8_t* dst_data[1] = { out->pixels.data() };
    int dst_linesize[1] = { static_cast<int>(w * bpp) };

    sws_scale(sws_ctx_, frame->data, frame->linesize, 0, h, dst_data, dst_linesize);

    return true;
}

//=============================================================================
// DecodeNextFrame
//=============================================================================

bool VulkanVideoDecoder::DecodeNextFrame() {
    if (!format_ctx_ || !codec_ctx_) return false;

    // Receive-first loop — matches proven D3D11 decoder pattern.
    // This correctly handles B-frame reordering and flush draining:
    //   1. Try to receive a decoded frame
    //   2. If EAGAIN, read and send more packets
    //   3. If demuxer EOF, send null packet to flush decoder's B-frame queue
    //   4. Continue receiving until all flushed frames are drained
    int receive_attempts = 0;
    const int max_receive_attempts = 100;

    while (receive_attempts < max_receive_attempts) {
        receive_attempts++;
        int ret = avcodec_receive_frame(codec_ctx_, current_frame_);

        if (ret == 0) {
            // Got a frame — calculate frame number from PTS
            int64_t timestamp = current_frame_->best_effort_timestamp;
            if (timestamp == AV_NOPTS_VALUE) {
                timestamp = current_frame_->pts;
            }
            if (timestamp != AV_NOPTS_VALUE) {
                double pts_seconds = timestamp * av_q2d(time_base_);
                current_frame_number_ = static_cast<int>(pts_seconds * fps_ + 0.5);
            } else {
                current_frame_number_++;
            }
            return true;
        }

        if (ret == AVERROR_EOF) {
            // Decoder fully drained — real EOF
            return false;
        }

        if (ret != AVERROR(EAGAIN)) {
            // Decode error
            return false;
        }

        // EAGAIN — need more input, read next packet
        ret = av_read_frame(format_ctx_, packet_);
        if (ret < 0) {
            if (ret == AVERROR_EOF) {
                // Demuxer EOF — flush decoder to drain buffered B-frames
                avcodec_send_packet(codec_ctx_, nullptr);
                continue;  // Loop back to receive flushed frames
            }
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
            // Don't fail — try to continue receiving frames already decoded
        }
    }

    return false;
}

//=============================================================================
// AddCurrentFrameToBuffer
//=============================================================================

void VulkanVideoDecoder::AddCurrentFrameToBuffer() {
    int insert_idx = -1;
    BufferedFrame* bf = nullptr;
    bool was_full = false;
    int old_frame_number = -1;
    uint64_t old_pool_texture = 0;
    AVFrame* old_hw_frame = nullptr;

    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);

        // Duplicate check
        auto it = frame_map_.find(current_frame_number_);
        if (it != frame_map_.end()) {
            int idx = it->second;
            if (frame_buffer_[idx].valid && frame_buffer_[idx].frame_number == current_frame_number_) {
                return;  // Already have this frame
            }
        }

        if (buffer_count_ < kFrameBufferSize) {
            insert_idx = (buffer_head_ + buffer_count_) % kFrameBufferSize;
        } else {
            insert_idx = buffer_head_;
            was_full = true;
        }
        bf = &frame_buffer_[insert_idx];

        if (was_full && bf->valid) {
            old_frame_number = bf->frame_number;
            old_pool_texture = bf->pool_texture_id;
            old_hw_frame = bf->hw_frame;
            bf->hw_frame = nullptr;
            bf->valid = false;
            frame_map_.erase(old_frame_number);
        }
    }

    // Clean up evicted frame resources (outside lock)
    // NOTE: We keep old_pool_texture for reuse by the GPU path (same resolution).
    // Only delete it if the new frame won't use it (SW path or non-HW frame).
    if (old_hw_frame) {
        av_frame_unref(old_hw_frame);
        av_frame_free(&old_hw_frame);
    }

    // GPU path: store ref'd VA-API frame for deferred GPU conversion on main thread
    if (gpu_decode_available_ &&
        decode_mode_ == DecodeMode::HARDWARE &&
        current_frame_->format == AV_PIX_FMT_VAAPI) {

        // Ref the frame so VA-API surface stays valid until main thread processes it
        AVFrame* ref_frame = av_frame_alloc();
        if (ref_frame && av_frame_ref(ref_frame, current_frame_) == 0) {
            std::lock_guard<std::mutex> lock(buffer_mutex_);
            bf->frame_number = current_frame_number_;
            bf->pts = (current_frame_->pts != AV_NOPTS_VALUE) ?
                       current_frame_->pts * av_q2d(time_base_) : 0.0;
            bf->valid = true;
            bf->pixel_data = nullptr;
            bf->pool_texture_id = old_pool_texture;  // Reuse evicted slot's texture
            bf->gpu_uploaded = false;
            bf->hw_frame = ref_frame;  // Deferred GPU conversion

            frame_map_[current_frame_number_] = insert_idx;

            if (!was_full) {
                buffer_count_++;
            } else {
                buffer_head_ = (buffer_head_ + 1) % kFrameBufferSize;
            }

            decode_head_ = current_frame_number_;
            frames_since_seek_++;
            if (delay_queue_filling_ && frames_since_seek_ >= kDelayQueueDepth) {
                delay_queue_filling_ = false;
            }
            return;
        }
        if (ref_frame) av_frame_free(&ref_frame);
        // Fall through to CPU path if ref failed
    }

    // CPU path: transfer HW frame to CPU, then sws_scale to RGBA
    AVFrame* src_frame = current_frame_;

    if (decode_mode_ == DecodeMode::HARDWARE &&
        current_frame_->format == AV_PIX_FMT_VAAPI) {
        av_frame_unref(sw_frame_);
        int ret = av_hwframe_transfer_data(sw_frame_, current_frame_, 0);
        if (ret < 0) {
            char errbuf[256];
            av_strerror(ret, errbuf, sizeof(errbuf));
            Debug::Log("VulkanVideoDecoder: HW frame transfer failed: " + std::string(errbuf));
            return;
        }
        sw_frame_->pts = current_frame_->pts;
        src_frame = sw_frame_;
    }

    std::shared_ptr<PixelData> pixel_data;
    if (!ConvertFrameToRGBA(src_frame, pixel_data)) {
        Debug::Log("VulkanVideoDecoder: Failed to convert frame " +
                   std::to_string(current_frame_number_) + " to RGBA");
        return;
    }

    // CPU path doesn't reuse GPU textures — delete the evicted slot's texture
    if (old_pool_texture != 0) {
        VulkanTexturePool::Instance().QueueDelete(old_pool_texture);
    }

    // Update buffer slot
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        bf->frame_number = current_frame_number_;
        bf->pts = (current_frame_->pts != AV_NOPTS_VALUE) ?
                   current_frame_->pts * av_q2d(time_base_) : 0.0;
        bf->valid = true;
        bf->pixel_data = pixel_data;
        bf->pool_texture_id = 0;  // Will be uploaded on demand
        bf->gpu_uploaded = false;
        bf->hw_frame = nullptr;

        frame_map_[current_frame_number_] = insert_idx;

        if (!was_full) {
            buffer_count_++;
        } else {
            buffer_head_ = (buffer_head_ + 1) % kFrameBufferSize;
        }
    }

    decode_head_ = current_frame_number_;

    // Update delay queue tracking
    frames_since_seek_++;
    if (delay_queue_filling_ && frames_since_seek_ >= kDelayQueueDepth) {
        delay_queue_filling_ = false;
    }
}

//=============================================================================
// SeekToFrame
//=============================================================================

bool VulkanVideoDecoder::SeekToFrame(int64_t target_pts) {
    int flags = AVSEEK_FLAG_BACKWARD;
    int ret = av_seek_frame(format_ctx_, video_stream_index_, target_pts, flags);
    if (ret < 0) {
        // Try seeking by byte
        ret = av_seek_frame(format_ctx_, video_stream_index_, target_pts,
                           AVSEEK_FLAG_BACKWARD | AVSEEK_FLAG_BYTE);
        if (ret < 0) return false;
    }
    avcodec_flush_buffers(codec_ctx_);
    return true;
}

//=============================================================================
// PerformSeekInternal
//=============================================================================

void VulkanVideoDecoder::PerformSeekInternal(int target_frame) {
    decode_seeking_ = true;

    // Convert frame number to PTS
    double target_seconds = target_frame / fps_;
    int64_t target_pts = static_cast<int64_t>(target_seconds / av_q2d(time_base_));

    ClearFrameBuffer();

    if (!SeekToFrame(target_pts)) {
        Debug::Log("VulkanVideoDecoder: Seek failed for frame " + std::to_string(target_frame));
    }

    // Reset sws context after seek (format may change)
    if (sws_ctx_) {
        sws_freeContext(sws_ctx_);
        sws_ctx_ = nullptr;
    }

    frames_since_seek_ = 0;
    if (codec_ctx_->has_b_frames > 0) {
        delay_queue_filling_ = true;
    }

    decode_seeking_ = false;
}

//=============================================================================
// DecodeThreadFunc
//=============================================================================

void VulkanVideoDecoder::DecodeThreadFunc() {
    Debug::Log("VulkanVideoDecoder: Decode thread started");

    int frames_decoded = 0;
    auto last_status_time = std::chrono::steady_clock::now();

    while (decode_running_) {
        int timeout_ms = (fps_ > 0) ? std::max(15, std::min(30, static_cast<int>(1000.0 / fps_))) : 30;

        {
            std::unique_lock<std::mutex> lock(decode_mutex_);
            decode_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this] {
                if (eof_reached_ && !seek_pending_) return false;
                return !decode_running_ || seek_pending_ || NeedsMoreFrames();
            });
        }

        if (!decode_running_) break;

        if (seek_pending_) {
            eof_reached_ = false;
            consecutive_decode_failures_ = 0;
            PerformSeekInternal(last_seek_target_.load());
            seek_pending_ = false;
            frames_decoded = 0;
            frame_ready_cv_.notify_all();
            continue;
        }

        if (eof_reached_) continue;

        if (NeedsMoreFrames()) {
            if (DecodeNextFrame()) {
                AddCurrentFrameToBuffer();
                frame_ready_cv_.notify_one();
                consecutive_decode_failures_ = 0;
                frames_decoded++;

                auto now = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_status_time).count();
                if (frames_decoded % 100 == 0 || elapsed >= 2000) {
                    Debug::Log("VulkanVideoDecoder: Decoded " + std::to_string(frames_decoded) +
                               " frames, target=" + std::to_string(decode_target_.load()) +
                               ", head=" + std::to_string(decode_head_.load()));
                    last_status_time = now;
                }
            } else {
                consecutive_decode_failures_++;
                if (consecutive_decode_failures_ >= 10) {
                    eof_reached_ = true;
                    Debug::Log("VulkanVideoDecoder: EOF reached after " +
                               std::to_string(frames_decoded) + " frames");
                }
            }
        }
    }

    Debug::Log("VulkanVideoDecoder: Decode thread stopped, decoded " +
               std::to_string(frames_decoded) + " frames total");
}

//=============================================================================
// NeedsMoreFrames
//=============================================================================

bool VulkanVideoDecoder::NeedsMoreFrames() const {
    int target = decode_target_.load();

    std::lock_guard<std::mutex> lock(buffer_mutex_);

    int ahead = 0;
    for (int i = 0; i < buffer_count_; i++) {
        int idx = (buffer_head_ + i) % kFrameBufferSize;
        if (frame_buffer_[idx].valid && frame_buffer_[idx].frame_number >= target) {
            ahead++;
        }
    }

    return ahead < kFrameBufferSize / 2;
}

//=============================================================================
// GetFrame — Returns PixelData from ring buffer
//=============================================================================

std::shared_ptr<PixelData> VulkanVideoDecoder::GetFrame(int frame_number) {
    if (!initialized_) return nullptr;

    frame_number = std::clamp(frame_number, 0,
                              frame_count_ > 0 ? frame_count_ - 1 : 0);

    BufferedFrame* bf = GetBufferedFrame(frame_number);
    if (bf && bf->pixel_data) {
        return bf->pixel_data;
    }

    return nullptr;
}

//=============================================================================
// GetFrameAsPoolTexture — Upload to VulkanTexturePool on demand
//=============================================================================

uint64_t VulkanVideoDecoder::GetFrameAsPoolTexture(int frame_number) {
    if (!initialized_) return 0;

    frame_number = std::clamp(frame_number, 0,
                              frame_count_ > 0 ? frame_count_ - 1 : 0);

    // Update decode target
    current_playhead_ = frame_number;
    decode_target_ = frame_number;
    decode_cv_.notify_one();

    // Check buffer
    BufferedFrame* bf = GetBufferedFrame(frame_number);

    if (!bf) {
        // Trigger seek if needed
        int head = decode_head_.load();
        constexpr int kSeekThresholdFrames = 60;

        bool need_backward = (head >= 0) && (frame_number < head - kFrameBufferSize);
        bool need_forward = (head >= 0) && (frame_number > head + kSeekThresholdFrames);
        bool need_initial = (head < 0);

        if ((need_backward || need_forward || need_initial) &&
            !decode_seeking_.load() && !seek_pending_.load()) {
            last_seek_target_ = frame_number;
            seek_pending_ = true;
            eof_reached_ = false;
            decode_cv_.notify_one();
        }

        // Wait briefly for frame
        int timeout_ms = (codec_ctx_ && codec_ctx_->has_b_frames > 0) ? 150 : 32;
        {
            std::unique_lock<std::mutex> lock(frame_ready_mutex_);
            frame_ready_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] {
                bool queue_ready = !delay_queue_filling_.load();
                bool frame_ready = GetBufferedFrame(frame_number) != nullptr;
                return (queue_ready && frame_ready) || !decode_running_;
            });
        }

        if (delay_queue_filling_.load()) return 0;

        bf = GetBufferedFrame(frame_number);

        // Fall back to closest frame
        if (!bf) {
            bf = GetClosestBufferedFrame(frame_number);
            if (!bf) return 0;
        }
    }

    // GPU-path frames already have pool texture
    if (bf->gpu_uploaded && bf->pool_texture_id != 0) {
        return bf->pool_texture_id;
    }

    // Deferred GPU conversion: HW frame stored by decode thread, convert now on main thread
    if (bf->hw_frame) {
        uint64_t gpu_texture = ProcessHWFrameOnGPU(bf->hw_frame, bf->pool_texture_id);
        // Release the ref'd VA-API frame regardless of success
        av_frame_unref(bf->hw_frame);
        av_frame_free(&bf->hw_frame);
        bf->hw_frame = nullptr;

        if (gpu_texture != 0) {
            bf->pool_texture_id = gpu_texture;
            bf->gpu_uploaded = true;
            return bf->pool_texture_id;
        }
        // GPU conversion failed — no pixel_data fallback available for HW frames
        return 0;
    }

    if (!bf->pixel_data) return 0;

    // Upload to VulkanTexturePool if not already done (CPU path)
    if (bf->pool_texture_id == 0) {
        auto& pool = VulkanTexturePool::Instance();

        VkFormat vk_fmt;
        switch (bf->pixel_data->pixel_format) {
            case PixelFormat::RGBA16:  vk_fmt = VK_FORMAT_R16G16B16A16_UNORM; break;
            case PixelFormat::RGBA16F: vk_fmt = VK_FORMAT_R16G16B16A16_SFLOAT; break;
            default:                   vk_fmt = VK_FORMAT_R8G8B8A8_UNORM; break;
        }

        bf->pool_texture_id = pool.CreateTextureFromPixels(
            bf->pixel_data->width, bf->pixel_data->height,
            vk_fmt,
            bf->pixel_data->pixels.data(),
            bf->pixel_data->pixels.size()
        );
    }

    return bf->pool_texture_id;
}

//=============================================================================
// GetClosestFrame
//=============================================================================

std::shared_ptr<PixelData> VulkanVideoDecoder::GetClosestFrame(int frame_number, int* actual_frame) {
    BufferedFrame* bf = GetClosestBufferedFrame(frame_number);
    if (bf && bf->pixel_data) {
        if (actual_frame) *actual_frame = bf->frame_number;
        return bf->pixel_data;
    }
    return nullptr;
}

bool VulkanVideoDecoder::HasFrame(int frame_number) const {
    return BufferContainsFrame(frame_number);
}

//=============================================================================
// Keyframe Access
//=============================================================================

std::shared_ptr<PixelData> VulkanVideoDecoder::GetKeyframe(int target_frame, int* actual_keyframe) {
    int keyframe = GetNearestKeyframePosition(target_frame);
    if (actual_keyframe) *actual_keyframe = keyframe;
    return GetFrame(keyframe);
}

int VulkanVideoDecoder::GetNearestKeyframePosition(int target_frame) const {
    if (is_intra_only_codec_ || keyframe_positions_.empty()) {
        return target_frame;
    }
    auto it = std::upper_bound(keyframe_positions_.begin(), keyframe_positions_.end(), target_frame);
    if (it == keyframe_positions_.begin()) return keyframe_positions_.front();
    --it;
    return *it;
}

bool VulkanVideoDecoder::IsIntraFrameCodec() const {
    return is_intra_only_codec_;
}

//=============================================================================
// UpdatePlayhead
//=============================================================================

void VulkanVideoDecoder::UpdatePlayhead(int frame_number, SeekQuality quality, bool force_seek) {
    int prev_playhead = current_playhead_.load();
    bool playhead_changed = (frame_number != prev_playhead);

    current_playhead_ = frame_number;
    decode_target_ = frame_number;

    if (!playhead_changed && !force_seek) return;

    // Check if seek is needed
    int head = decode_head_.load();
    constexpr int kSeekThresholdFrames = 60;

    bool need_backward = (head >= 0) && (frame_number < head - kFrameBufferSize);
    bool need_forward = (head >= 0) && (frame_number > head + kSeekThresholdFrames);
    bool need_initial = (head < 0);

    if ((need_backward || need_forward || need_initial || force_seek) &&
        !decode_seeking_.load() && !seek_pending_.load()) {
        last_seek_target_ = frame_number;
        seek_pending_ = true;
        eof_reached_ = false;
    }

    decode_cv_.notify_one();
}

//=============================================================================
// Demand-Driven Decode API
//=============================================================================

void VulkanVideoDecoder::SetNeededFrames(const std::vector<int>& frames_by_priority) {
    {
        std::lock_guard<std::mutex> lock(decode_mutex_);
        needed_frames_ = frames_by_priority;
    }
    if (!frames_by_priority.empty()) {
        decode_target_ = frames_by_priority.front();
    }
    decode_cv_.notify_one();
}

DecodeStatus VulkanVideoDecoder::GetDecodeStatus() const {
    DecodeStatus status;
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    for (int i = 0; i < buffer_count_; i++) {
        int idx = (buffer_head_ + i) % kFrameBufferSize;
        if (frame_buffer_[idx].valid) {
            status.have.push_back(frame_buffer_[idx].frame_number);
        }
    }
    status.currently_decoding = decode_head_.load();
    return status;
}

void VulkanVideoDecoder::EvictOutsideWindow(const std::set<int>& keep_frames) {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    for (int i = 0; i < buffer_count_; i++) {
        int idx = (buffer_head_ + i) % kFrameBufferSize;
        if (frame_buffer_[idx].valid &&
            keep_frames.find(frame_buffer_[idx].frame_number) == keep_frames.end()) {
            frame_map_.erase(frame_buffer_[idx].frame_number);
            if (frame_buffer_[idx].pool_texture_id != 0) {
                VulkanTexturePool::Instance().QueueDelete(frame_buffer_[idx].pool_texture_id);
            }
            frame_buffer_[idx].Reset();
        }
    }
}

std::set<int> VulkanVideoDecoder::GetBufferedFramesSet() const {
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

//=============================================================================
// Buffer Status
//=============================================================================

int VulkanVideoDecoder::GetBufferedAhead() const {
    int target = current_playhead_.load();
    int count = 0;
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    for (int i = 0; i < buffer_count_; i++) {
        int idx = (buffer_head_ + i) % kFrameBufferSize;
        if (frame_buffer_[idx].valid && frame_buffer_[idx].frame_number >= target) {
            count++;
        }
    }
    return count;
}

int VulkanVideoDecoder::GetBufferedBehind() const {
    int target = current_playhead_.load();
    int count = 0;
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    for (int i = 0; i < buffer_count_; i++) {
        int idx = (buffer_head_ + i) % kFrameBufferSize;
        if (frame_buffer_[idx].valid && frame_buffer_[idx].frame_number < target) {
            count++;
        }
    }
    return count;
}

int VulkanVideoDecoder::GetBufferSize() const {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    return buffer_count_;
}

void VulkanVideoDecoder::GetBufferedRange(int& start_frame, int& end_frame) const {
    start_frame = INT_MAX;
    end_frame = -1;
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    for (int i = 0; i < buffer_count_; i++) {
        int idx = (buffer_head_ + i) % kFrameBufferSize;
        if (frame_buffer_[idx].valid) {
            start_frame = std::min(start_frame, frame_buffer_[idx].frame_number);
            end_frame = std::max(end_frame, frame_buffer_[idx].frame_number);
        }
    }
    if (start_frame == INT_MAX) start_frame = 0;
}

void VulkanVideoDecoder::ClearBuffer() {
    // ClearFrameBuffer now handles pool texture cleanup
    ClearFrameBuffer();
}

//=============================================================================
// Looping
//=============================================================================

void VulkanVideoDecoder::SetLoopPoints(int start_frame, int end_frame) {
    loop_start_ = start_frame;
    loop_end_ = end_frame;
}

void VulkanVideoDecoder::ClearLoopPoints() {
    loop_start_ = -1;
    loop_end_ = -1;
}

//=============================================================================
// ProcessHWFrameOnGPU — DMA-BUF import + YUV shader → RGBA16F pool texture
//=============================================================================

uint64_t VulkanVideoDecoder::ProcessHWFrameOnGPU(AVFrame* vaapi_frame, uint64_t existing_texture) {
    if (!hw_frame_importer_ || !yuv_renderer_) return 0;

    // Import VA-API frame planes via DMA-BUF
    auto imported = hw_frame_importer_->ImportFrame(vaapi_frame);
    if (!imported.valid) return 0;

    int w = imported.width;
    int h = imported.height;

    // Ensure we have an RGBA16F output image of the right size
    if (!EnsureGPUOutputImage(w, h)) {
        hw_frame_importer_->ReleaseImportedFrame(imported);
        return 0;
    }

    // YUV render pass handles UNDEFINED → COLOR_ATTACHMENT → SHADER_READ transition
    // No separate barrier submit needed

    // Set up YUV render params
    YUVRenderParams params;
    params.width = w;
    params.height = h;
    params.bit_depth = imported.bit_depth;
    params.is_hdr = is_hdr_;
    params.is_full_range = GetEffectiveFullRange();
    params.color_space = is_hdr_ ? YUVColorSpace::BT_2020 : YUVColorSpace::BT_709;
    params.plane_count = imported.num_planes;
    params.has_alpha = false;
    params.is_rgb_planar = false;

    // Run YUV → RGBA16F shader
    bool ok = yuv_renderer_->Render(
        imported.views, imported.num_planes,
        gpu_output_image_, gpu_output_view_,
        w, h, params);

    // Release imported DMA-BUF resources (YUV render is complete, fence waited inside Render)
    hw_frame_importer_->ReleaseImportedFrame(imported);

    if (!ok) {
        Debug::Log("VulkanVideoDecoder: YUV render failed for HW frame");
        return 0;
    }

    auto& pool = VulkanTexturePool::Instance();

    // Reuse existing pool texture if available (same dimensions guaranteed for a single video)
    if (existing_texture != 0) {
        if (pool.UpdateTextureFromImage(existing_texture, gpu_output_image_, w, h)) {
            return existing_texture;
        }
        // Dimensions changed or texture was evicted — fall through to create new
    }

    // Create a new pool texture
    uint64_t tex_id = pool.CreateTextureFromImage(
        gpu_output_image_, w, h, VK_FORMAT_R16G16B16A16_SFLOAT);

    return tex_id;
}

//=============================================================================
// EnsureGPUOutputImage — Reusable RGBA16F render target
//=============================================================================

bool VulkanVideoDecoder::EnsureGPUOutputImage(int width, int height) {
    if (gpu_output_image_ != VK_NULL_HANDLE &&
        gpu_output_width_ == width && gpu_output_height_ == height) {
        return true;
    }

    DestroyGPUOutputImage();

    auto& dev = VulkanDeviceManager::Instance();

    VkImageCreateInfo image_info = {};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    image_info.extent = { static_cast<uint32_t>(width),
                          static_cast<uint32_t>(height), 1 };
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                       VK_IMAGE_USAGE_SAMPLED_BIT |
                       VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo alloc_info = {};
    alloc_info.usage = VMA_MEMORY_USAGE_GPU_ONLY;

    VkResult vr = vmaCreateImage(dev.GetAllocator(), &image_info, &alloc_info,
                                  &gpu_output_image_, &gpu_output_alloc_, nullptr);
    if (vr != VK_SUCCESS) {
        Debug::Log("VulkanVideoDecoder: Failed to create GPU output image");
        return false;
    }

    VkImageViewCreateInfo view_info = {};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = gpu_output_image_;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.layerCount = 1;

    vr = vkCreateImageView(dev.GetDevice(), &view_info, nullptr, &gpu_output_view_);
    if (vr != VK_SUCCESS) {
        Debug::Log("VulkanVideoDecoder: Failed to create GPU output image view");
        vmaDestroyImage(dev.GetAllocator(), gpu_output_image_, gpu_output_alloc_);
        gpu_output_image_ = VK_NULL_HANDLE;
        gpu_output_alloc_ = VK_NULL_HANDLE;
        return false;
    }

    gpu_output_width_ = width;
    gpu_output_height_ = height;
    return true;
}

//=============================================================================
// DestroyGPUOutputImage
//=============================================================================

void VulkanVideoDecoder::DestroyGPUOutputImage() {
    auto& dev = VulkanDeviceManager::Instance();
    if (!dev.IsInitialized()) return;

    if (gpu_output_view_ != VK_NULL_HANDLE) {
        vkDestroyImageView(dev.GetDevice(), gpu_output_view_, nullptr);
        gpu_output_view_ = VK_NULL_HANDLE;
    }
    if (gpu_output_image_ != VK_NULL_HANDLE) {
        vmaDestroyImage(dev.GetAllocator(), gpu_output_image_, gpu_output_alloc_);
        gpu_output_image_ = VK_NULL_HANDLE;
        gpu_output_alloc_ = VK_NULL_HANDLE;
    }
    gpu_output_width_ = 0;
    gpu_output_height_ = 0;
}

} // namespace qcview

#endif // __linux__
