#include "scrub_decoder.h"
#include "../utils/debug_utils.h"

#include <glad/gl.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <libavutil/hwcontext.h>
}

#ifdef __APPLE__
#include <CoreVideo/CoreVideo.h>
#endif

#ifdef QCVIEW_USE_METAL
#include "../gpu/metal_hwframe_extractor.h"
#include "../gpu/metal_yuv_renderer.h"
#include "../gpu/metal_texture_pool.h"
#endif

#ifdef __APPLE__
// Codecs that VideoToolbox can HW-decode
static bool ScrubVTSupported(AVCodecID codec_id) {
    switch (codec_id) {
        case AV_CODEC_ID_H264:
        case AV_CODEC_ID_HEVC:
        case AV_CODEC_ID_PRORES:
        case AV_CODEC_ID_VP9:
        case AV_CODEC_ID_AV1:
        case AV_CODEC_ID_MPEG4:
        case AV_CODEC_ID_MPEG2VIDEO:
            return true;
        default:
            return false;
    }
}

static enum AVPixelFormat scrub_get_hw_format(AVCodecContext* ctx,
                                               const enum AVPixelFormat* pix_fmts) {
    for (const enum AVPixelFormat* p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
        if (*p == AV_PIX_FMT_VIDEOTOOLBOX) {
            return AV_PIX_FMT_VIDEOTOOLBOX;
        }
    }
    return pix_fmts[0];
}
#endif

namespace qcview {

//=============================================================================
// ScrubDecoder Implementation - Single-frame on-demand decoder
//=============================================================================

ScrubDecoder::ScrubDecoder(const std::string& video_path)
    : video_path_(video_path) {}

ScrubDecoder::~ScrubDecoder() {
    Shutdown();
}

bool ScrubDecoder::Initialize() {
    std::lock_guard<std::mutex> lock(decode_mutex_);

    if (initialized_) return true;

    // Open input file
    if (avformat_open_input(&format_ctx_, video_path_.c_str(), nullptr, nullptr) < 0) {
        Debug::Log("ScrubDecoder: Failed to open: " + video_path_);
        return false;
    }

    // Find stream info
    if (avformat_find_stream_info(format_ctx_, nullptr) < 0) {
        Debug::Log("ScrubDecoder: Failed to find stream info");
        avformat_close_input(&format_ctx_);
        return false;
    }

    // Find video stream
    video_stream_idx_ = av_find_best_stream(format_ctx_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (video_stream_idx_ < 0) {
        Debug::Log("ScrubDecoder: No video stream found");
        avformat_close_input(&format_ctx_);
        return false;
    }

    AVStream* video_stream = format_ctx_->streams[video_stream_idx_];

    // Get video metadata
    width_ = video_stream->codecpar->width;
    height_ = video_stream->codecpar->height;

    // Get FPS
    if (video_stream->avg_frame_rate.den > 0 && video_stream->avg_frame_rate.num > 0) {
        fps_ = av_q2d(video_stream->avg_frame_rate);
    } else if (video_stream->r_frame_rate.den > 0 && video_stream->r_frame_rate.num > 0) {
        fps_ = av_q2d(video_stream->r_frame_rate);
    } else {
        fps_ = 24.0;
    }

    // Get duration and frame count
    double duration = 0.0;
    if (format_ctx_->duration > 0) {
        duration = static_cast<double>(format_ctx_->duration) / AV_TIME_BASE;
    } else if (video_stream->duration > 0) {
        duration = static_cast<double>(video_stream->duration) * av_q2d(video_stream->time_base);
    }
    frame_count_ = static_cast<int>(duration * fps_ + 0.5);
    if (frame_count_ <= 0) frame_count_ = 1;

    // Get start time
    if (video_stream->start_time != AV_NOPTS_VALUE) {
        start_time_ = av_rescale_q(video_stream->start_time,
                                   video_stream->time_base,
                                   AV_TIME_BASE_Q);
    }

    // Find decoder
    const AVCodec* codec = avcodec_find_decoder(video_stream->codecpar->codec_id);
    if (!codec) {
        Debug::Log("ScrubDecoder: No decoder found");
        avformat_close_input(&format_ctx_);
        return false;
    }

    // Create codec context
    codec_ctx_ = avcodec_alloc_context3(codec);
    if (!codec_ctx_) {
        Debug::Log("ScrubDecoder: Failed to allocate codec context");
        avformat_close_input(&format_ctx_);
        return false;
    }

    if (avcodec_parameters_to_context(codec_ctx_, video_stream->codecpar) < 0) {
        Debug::Log("ScrubDecoder: Failed to copy codec params");
        avcodec_free_context(&codec_ctx_);
        avformat_close_input(&format_ctx_);
        return false;
    }

    // VideoToolbox HW acceleration for INTRA codecs only (ProRes, DNxHD, MJPEG).
    // Inter-frame codecs (H.264/HEVC) use CPU software decode — the synchronous
    // seek-flush-decode-forward pattern works reliably with CPU but has issues with
    // VideoToolbox's async B-frame reorder pipeline. This matches the Windows
    // ScrubDecoder which is always CPU-only.
#ifdef __APPLE__
    bool is_intra = (codec_ctx_->codec_id == AV_CODEC_ID_PRORES ||
                     codec_ctx_->codec_id == AV_CODEC_ID_DNXHD ||
                     codec_ctx_->codec_id == AV_CODEC_ID_MJPEG ||
                     codec_ctx_->codec_id == AV_CODEC_ID_RAWVIDEO);
    if (is_intra && ScrubVTSupported(codec_ctx_->codec_id)) {
        int ret = av_hwdevice_ctx_create(&hw_device_ctx_, AV_HWDEVICE_TYPE_VIDEOTOOLBOX,
                                          nullptr, nullptr, 0);
        if (ret >= 0) {
            codec_ctx_->hw_device_ctx = av_buffer_ref(hw_device_ctx_);
            codec_ctx_->get_format = scrub_get_hw_format;
            hw_accel_active_ = true;
            Debug::Log("ScrubDecoder: VideoToolbox HW acceleration enabled (intra codec)");
        } else {
            Debug::Log("ScrubDecoder: VideoToolbox not available, using CPU decode");
        }
    }
#endif

    // Thread count: HW accel handles its own threading, CPU uses fewer threads for low latency
    codec_ctx_->thread_count = hw_accel_active_ ? 0 : 4;

    if (avcodec_open2(codec_ctx_, codec, nullptr) < 0) {
        Debug::Log("ScrubDecoder: Failed to open codec");
        avcodec_free_context(&codec_ctx_);
        avformat_close_input(&format_ctx_);
        return false;
    }

    // Allocate frame and packet
    frame_ = av_frame_alloc();
    rgb_frame_ = av_frame_alloc();
    packet_ = av_packet_alloc();

    if (!frame_ || !rgb_frame_ || !packet_) {
        Debug::Log("ScrubDecoder: Failed to allocate frame/packet");
        Shutdown();
        return false;
    }

    // Build frame index from container metadata for precise seeking
    frame_index_ = std::make_shared<qcview::FrameIndex>();
    if (frame_index_->BuildFromStreamIndex(format_ctx_, video_stream_idx_, fps_)) {
        Debug::Log("ScrubDecoder: FrameIndex built (" +
                   std::to_string(frame_index_->TotalFrames()) + " frames, " +
                   std::to_string(frame_index_->GetKeyframePositions().size()) + " keyframes)");
        // Update frame count from index if it has better data
        if (frame_index_->TotalFrames() > 0) {
            frame_count_ = frame_index_->TotalFrames();
        }
    } else {
        Debug::Log("ScrubDecoder: No stream index entries, using arithmetic seeking");
        frame_index_.reset();
    }

    initialized_ = true;
    Debug::Log("ScrubDecoder: Initialized " + std::to_string(width_) + "x" +
               std::to_string(height_) + " @ " + std::to_string(fps_) + " fps" +
               (hw_accel_active_ ? " [VideoToolbox]" : " [CPU]"));
    return true;
}

void ScrubDecoder::Shutdown() {
    std::lock_guard<std::mutex> lock(decode_mutex_);

#ifdef QCVIEW_USE_METAL
    // Clean up last pool texture
    if (last_pool_texture_id_ != 0) {
        MetalTexturePool::Instance().QueueDelete(last_pool_texture_id_);
        last_pool_texture_id_ = 0;
    }
    hw_extractor_.reset();
    yuv_renderer_.reset();
    gpu_pipeline_ready_ = false;
#endif

    last_decoded_pixels_.reset();
    last_decoded_frame_ = -1;

    if (sws_ctx_) {
        sws_freeContext(sws_ctx_);
        sws_ctx_ = nullptr;
    }
    if (packet_) {
        av_packet_free(&packet_);
        packet_ = nullptr;
    }
    if (rgb_frame_) {
        av_frame_free(&rgb_frame_);
        rgb_frame_ = nullptr;
    }
    if (frame_) {
        av_frame_free(&frame_);
        frame_ = nullptr;
    }
    if (codec_ctx_) {
        avcodec_free_context(&codec_ctx_);
        codec_ctx_ = nullptr;
    }
    if (hw_device_ctx_) {
        av_buffer_unref(&hw_device_ctx_);
        hw_device_ctx_ = nullptr;
    }
    hw_accel_active_ = false;
    if (format_ctx_) {
        avformat_close_input(&format_ctx_);
        format_ctx_ = nullptr;
    }

    initialized_ = false;
}

std::shared_ptr<PixelData> ScrubDecoder::GetClosestFrame(int frame_number, int* actual_frame) {
    std::lock_guard<std::mutex> lock(decode_mutex_);

    if (!initialized_) return nullptr;

    // Clamp frame number
    frame_number = std::max(0, std::min(frame_number, frame_count_ - 1));

    // Return cached frame if same as last request
    if (frame_number == last_decoded_frame_ && last_decoded_pixels_) {
        if (actual_frame) *actual_frame = last_decoded_frame_;
        return last_decoded_pixels_;
    }

    return DecodeFrame(frame_number, actual_frame);
}

std::shared_ptr<PixelData> ScrubDecoder::GetFrame(int frame_number) {
    return GetClosestFrame(frame_number, nullptr);
}

std::shared_ptr<PixelData> ScrubDecoder::GetKeyframe(int frame_number, int* actual_frame) {
    std::lock_guard<std::mutex> lock(decode_mutex_);
    if (!initialized_) return nullptr;

    frame_number = std::max(0, std::min(frame_number, frame_count_ - 1));

    // Seek to keyframe before target
    if (!SeekToKeyframe(frame_number)) return nullptr;

    AVStream* stream = format_ctx_->streams[video_stream_idx_];

    // Decode just the FIRST frame (the keyframe) — don't chase the target
    for (int attempt = 0; attempt < 10; attempt++) {
        int ret = av_read_frame(format_ctx_, packet_);
        if (ret < 0) break;

        if (packet_->stream_index != video_stream_idx_) {
            av_packet_unref(packet_);
            continue;
        }

        ret = avcodec_send_packet(codec_ctx_, packet_);
        av_packet_unref(packet_);
        if (ret < 0) continue;

        ret = avcodec_receive_frame(codec_ctx_, frame_);
        if (ret == 0) {
            int64_t pts = frame_->best_effort_timestamp;
            if (pts == AV_NOPTS_VALUE) pts = frame_->pts;
            if (pts == AV_NOPTS_VALUE) pts = 0;

            double time_sec = static_cast<double>(pts) * av_q2d(stream->time_base);
            int kf_frame = static_cast<int>(time_sec * fps_ + 0.5);

            auto pixels = ConvertFrame(frame_);
            if (pixels) {
                last_decoded_frame_ = kf_frame;
                last_decoded_pixels_ = pixels;
                if (actual_frame) *actual_frame = kf_frame;
                return pixels;
            }
        }
    }

    if (actual_frame) *actual_frame = -1;
    return nullptr;
}

bool ScrubDecoder::SeekToKeyframe(int target_frame) {
    if (!format_ctx_ || video_stream_idx_ < 0) return false;

    int ret;
    if (frame_index_ && frame_index_->IsComplete()) {
        // Use FrameIndex for exact keyframe PTS — no arithmetic approximation
        int64_t keyframe_pts = frame_index_->KeyframePTSBefore(target_frame);
        ret = av_seek_frame(format_ctx_, video_stream_idx_, keyframe_pts, AVSEEK_FLAG_BACKWARD);
    } else {
        // Arithmetic fallback
        int64_t target_pts = static_cast<int64_t>(target_frame / fps_ * AV_TIME_BASE) + start_time_;
        ret = av_seek_frame(format_ctx_, -1, target_pts, AVSEEK_FLAG_BACKWARD);
    }

    if (ret < 0) {
        // Try seeking to beginning
        int64_t start = (frame_index_ && frame_index_->IsComplete())
            ? frame_index_->PTSForFrame(0)
            : start_time_;
        ret = av_seek_frame(format_ctx_,
                            frame_index_ ? video_stream_idx_ : -1,
                            start, AVSEEK_FLAG_BACKWARD);
        if (ret < 0) return false;
    }

    avcodec_flush_buffers(codec_ctx_);
    return true;
}

std::shared_ptr<PixelData> ScrubDecoder::DecodeFrame(int target_frame, int* actual_frame) {
    if (!format_ctx_ || !codec_ctx_) return nullptr;

    AVStream* stream = format_ctx_->streams[video_stream_idx_];

    // Seek to keyframe before target
    if (!SeekToKeyframe(target_frame)) {
        return nullptr;
    }

    // Decode forward from keyframe to target. VideoToolbox HW decode is fast
    // enough for long GOPs. The FrameIndex gives us exact seek PTS (above),
    // but we use generous decode limits to handle B-frame reorder queues.
    int max_frames_to_decode = hw_accel_active_ ? 120 : 60;
    int decoded_frame = -1;
    int frames_decoded = 0;

    while (frames_decoded < max_frames_to_decode) {
        int ret = av_read_frame(format_ctx_, packet_);
        if (ret < 0) {
            break;  // EOF or error
        }

        if (packet_->stream_index != video_stream_idx_) {
            av_packet_unref(packet_);
            continue;
        }

        ret = avcodec_send_packet(codec_ctx_, packet_);
        av_packet_unref(packet_);

        if (ret < 0) continue;

        while (ret >= 0) {
            ret = avcodec_receive_frame(codec_ctx_, frame_);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                break;
            }
            if (ret < 0) {
                break;
            }

            // Calculate frame number from PTS
            int64_t pts = frame_->best_effort_timestamp;
            if (pts == AV_NOPTS_VALUE) pts = frame_->pts;
            if (pts == AV_NOPTS_VALUE) pts = 0;

            double time_sec = static_cast<double>(pts) * av_q2d(stream->time_base);
            decoded_frame = static_cast<int>(time_sec * fps_ + 0.5);
            frames_decoded++;

            // Got the target frame or past it
            if (decoded_frame >= target_frame) {
                auto pixels = ConvertFrame(frame_);
                if (pixels) {
                    last_decoded_frame_ = decoded_frame;
                    last_decoded_pixels_ = pixels;
                    if (actual_frame) *actual_frame = decoded_frame;
                    return pixels;
                }
            }
        }
    }

    // If we decoded at least one frame, return it
    if (decoded_frame >= 0 && frames_decoded > 0) {
        auto pixels = ConvertFrame(frame_);
        if (pixels) {
            last_decoded_frame_ = decoded_frame;
            last_decoded_pixels_ = pixels;
            if (actual_frame) *actual_frame = decoded_frame;
            return pixels;
        }
    }

    return nullptr;
}

std::shared_ptr<PixelData> ScrubDecoder::ConvertFrame(AVFrame* frame) {
    if (!frame || frame->width <= 0 || frame->height <= 0) return nullptr;

#ifdef __APPLE__
    // VideoToolbox HW frame: extract CVPixelBuffer → lock → sws_scale from NV12/P010
    if (hw_accel_active_ && frame->format == AV_PIX_FMT_VIDEOTOOLBOX) {
        CVPixelBufferRef pb = (CVPixelBufferRef)frame->data[3];
        if (!pb) return nullptr;

        CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);

        int w = (int)CVPixelBufferGetWidth(pb);
        int h = (int)CVPixelBufferGetHeight(pb);
        OSType pixel_format = CVPixelBufferGetPixelFormatType(pb);

        // Determine source AVPixelFormat from CVPixelBuffer format
        AVPixelFormat src_fmt;
        switch (pixel_format) {
            case kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange:
            case kCVPixelFormatType_420YpCbCr8BiPlanarFullRange:
                src_fmt = AV_PIX_FMT_NV12;
                break;
            case kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange:
            case kCVPixelFormatType_420YpCbCr10BiPlanarFullRange:
                src_fmt = AV_PIX_FMT_P010LE;
                break;
            case kCVPixelFormatType_422YpCbCr8:
                src_fmt = AV_PIX_FMT_UYVY422;
                break;
            case kCVPixelFormatType_32BGRA:
                src_fmt = AV_PIX_FMT_BGRA;
                break;
            default:
                // Fallback: try NV12
                src_fmt = AV_PIX_FMT_NV12;
                break;
        }

        // Build source data/linesize from CVPixelBuffer planes
        uint8_t* src_data[4] = {nullptr};
        int src_linesize[4] = {0};
        size_t plane_count = CVPixelBufferGetPlaneCount(pb);
        if (plane_count > 0) {
            for (size_t i = 0; i < plane_count && i < 4; i++) {
                src_data[i] = (uint8_t*)CVPixelBufferGetBaseAddressOfPlane(pb, i);
                src_linesize[i] = (int)CVPixelBufferGetBytesPerRowOfPlane(pb, i);
            }
        } else {
            // Non-planar (e.g., BGRA)
            src_data[0] = (uint8_t*)CVPixelBufferGetBaseAddress(pb);
            src_linesize[0] = (int)CVPixelBufferGetBytesPerRow(pb);
        }

        sws_ctx_ = sws_getCachedContext(
            sws_ctx_,
            w, h, src_fmt,
            w, h, AV_PIX_FMT_RGBA,
            SWS_BILINEAR, nullptr, nullptr, nullptr
        );

        std::shared_ptr<PixelData> pixels;
        if (sws_ctx_) {
            pixels = std::make_shared<PixelData>();
            pixels->width = w;
            pixels->height = h;
            pixels->SetFormat(PixelFormat::RGBA8);
            pixels->pixels.resize(w * h * 4);

            uint8_t* dst_data[1] = { pixels->pixels.data() };
            int dst_linesize[1] = { w * 4 };

            sws_scale(sws_ctx_, src_data, src_linesize, 0, h, dst_data, dst_linesize);
        }

        CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
        return pixels;
    }
#endif

    // Software decode path: frame is already in a CPU pixel format
    sws_ctx_ = sws_getCachedContext(
        sws_ctx_,
        frame->width, frame->height, static_cast<AVPixelFormat>(frame->format),
        frame->width, frame->height, AV_PIX_FMT_RGBA,
        SWS_BILINEAR, nullptr, nullptr, nullptr
    );

    if (!sws_ctx_) return nullptr;

    auto pixels = std::make_shared<PixelData>();
    pixels->width = frame->width;
    pixels->height = frame->height;
    pixels->SetFormat(PixelFormat::RGBA8);
    pixels->pixels.resize(frame->width * frame->height * 4);

    uint8_t* dst_data[1] = { pixels->pixels.data() };
    int dst_linesize[1] = { frame->width * 4 };

    sws_scale(sws_ctx_, frame->data, frame->linesize, 0, frame->height,
              dst_data, dst_linesize);

    return pixels;
}

//=============================================================================
// Metal Zero-Copy Path
//=============================================================================

#ifdef QCVIEW_USE_METAL

uint64_t ScrubDecoder::GetFrameAsPoolTexture(int frame_number, int* actual_frame) {
    std::lock_guard<std::mutex> lock(decode_mutex_);

    if (!initialized_ || !hw_accel_active_) return 0;

    frame_number = std::max(0, std::min(frame_number, frame_count_ - 1));

    // Initialize GPU pipeline on first use (lazy — avoids Metal calls during Initialize)
    if (!gpu_pipeline_ready_) {
        hw_extractor_ = std::make_unique<MetalHWFrameExtractor>();
        yuv_renderer_ = std::make_unique<MetalYUVRenderer>();
        if (hw_extractor_->Initialize() && yuv_renderer_->Initialize()) {
            gpu_pipeline_ready_ = true;

            // Detect color properties from codec context
            if (codec_ctx_) {
                is_full_range_ = (codec_ctx_->color_range == AVCOL_RANGE_JPEG);
                is_bt2020_ = (codec_ctx_->color_primaries == AVCOL_PRI_BT2020);
                is_hdr_ = (codec_ctx_->color_trc == AVCOL_TRC_SMPTE2084 ||
                           codec_ctx_->color_trc == AVCOL_TRC_ARIB_STD_B67);
            }
            Debug::Log("ScrubDecoder: Metal GPU pipeline ready");
        } else {
            hw_extractor_.reset();
            yuv_renderer_.reset();
            Debug::Log("ScrubDecoder: Metal GPU pipeline init failed");
            return 0;
        }
    }

    return DecodeFrameToPoolTexture(frame_number, actual_frame);
}

uint64_t ScrubDecoder::DecodeFrameToPoolTexture(int target_frame, int* actual_frame) {
    if (!format_ctx_ || !codec_ctx_ || !gpu_pipeline_ready_) return 0;

    AVStream* stream = format_ctx_->streams[video_stream_idx_];

    // Seek to keyframe before target
    if (!SeekToKeyframe(target_frame)) {
        return 0;
    }

    // Generous decode limit — FrameIndex gives exact seek PTS,
    // but we need margin for B-frame reorder queue
    int max_frames_to_decode = 120;
    int decoded_frame = -1;
    int frames_decoded = 0;

    while (frames_decoded < max_frames_to_decode) {
        int ret = av_read_frame(format_ctx_, packet_);
        if (ret < 0) break;

        if (packet_->stream_index != video_stream_idx_) {
            av_packet_unref(packet_);
            continue;
        }

        ret = avcodec_send_packet(codec_ctx_, packet_);
        av_packet_unref(packet_);
        if (ret < 0) continue;

        while (ret >= 0) {
            ret = avcodec_receive_frame(codec_ctx_, frame_);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            if (ret < 0) break;

            // Calculate frame number from PTS
            int64_t pts = frame_->best_effort_timestamp;
            if (pts == AV_NOPTS_VALUE) pts = frame_->pts;
            if (pts == AV_NOPTS_VALUE) pts = 0;

            double time_sec = static_cast<double>(pts) * av_q2d(stream->time_base);
            decoded_frame = static_cast<int>(time_sec * fps_ + 0.5);
            frames_decoded++;

            if (decoded_frame >= target_frame) {
                // Got our target frame — run GPU pipeline
                if (frame_->format == AV_PIX_FMT_VIDEOTOOLBOX) {
                    CVPixelBufferRef pb = (CVPixelBufferRef)frame_->data[3];
                    if (pb && hw_extractor_) {
                        CVPixelBufferRetain(pb);

                        MetalTextureFrame tex_frame = hw_extractor_->ExtractFromPixelBuffer((void*)pb);
                        if (tex_frame.valid && yuv_renderer_) {
                            void* rgba_texture = yuv_renderer_->RenderToRGBA(
                                tex_frame.y_texture, tex_frame.uv_texture,
                                tex_frame.width, tex_frame.height,
                                tex_frame.bit_depth, is_full_range_,
                                is_bt2020_, is_hdr_,
                                static_cast<int>(tex_frame.subsampling));

                            hw_extractor_->ReleaseFrame(tex_frame);

                            if (rgba_texture) {
                                uint64_t pool_id = MetalTexturePool::Instance().RegisterExistingTexture(
                                    rgba_texture, tex_frame.width, tex_frame.height,
                                    1 /* RGBA16Float */);

                                // Track for cleanup on Shutdown. Don't QueueDelete eagerly —
                                // the pool's LRU eviction handles memory pressure, and eager
                                // deletion can race with ImGui still referencing the texture.
                                last_pool_texture_id_ = pool_id;

                                CVPixelBufferRelease(pb);

                                if (actual_frame) *actual_frame = decoded_frame;
                                last_decoded_frame_ = decoded_frame;
                                return pool_id;
                            }
                        } else {
                            hw_extractor_->ReleaseFrame(tex_frame);
                        }

                        CVPixelBufferRelease(pb);
                    }
                }
                // HW path failed — caller can fall back to PixelData path
                if (actual_frame) *actual_frame = decoded_frame;
                return 0;
            }
        }
    }

    if (actual_frame) *actual_frame = decoded_frame;
    return 0;
}

uint64_t ScrubDecoder::GetKeyframeAsPoolTexture(int frame_number, int* actual_frame) {
    std::lock_guard<std::mutex> lock(decode_mutex_);
    if (!initialized_ || !hw_accel_active_) return 0;

    frame_number = std::max(0, std::min(frame_number, frame_count_ - 1));

    if (!gpu_pipeline_ready_) {
        hw_extractor_ = std::make_unique<MetalHWFrameExtractor>();
        yuv_renderer_ = std::make_unique<MetalYUVRenderer>();
        if (hw_extractor_->Initialize() && yuv_renderer_->Initialize()) {
            gpu_pipeline_ready_ = true;
            if (codec_ctx_) {
                is_full_range_ = (codec_ctx_->color_range == AVCOL_RANGE_JPEG);
                is_bt2020_ = (codec_ctx_->color_primaries == AVCOL_PRI_BT2020);
                is_hdr_ = (codec_ctx_->color_trc == AVCOL_TRC_SMPTE2084 ||
                           codec_ctx_->color_trc == AVCOL_TRC_ARIB_STD_B67);
            }
        } else {
            hw_extractor_.reset();
            yuv_renderer_.reset();
            return 0;
        }
    }

    if (!SeekToKeyframe(frame_number)) return 0;

    AVStream* stream = format_ctx_->streams[video_stream_idx_];

    // Decode just the first frame (keyframe)
    for (int attempt = 0; attempt < 10; attempt++) {
        int ret = av_read_frame(format_ctx_, packet_);
        if (ret < 0) break;

        if (packet_->stream_index != video_stream_idx_) {
            av_packet_unref(packet_);
            continue;
        }

        ret = avcodec_send_packet(codec_ctx_, packet_);
        av_packet_unref(packet_);
        if (ret < 0) continue;

        ret = avcodec_receive_frame(codec_ctx_, frame_);
        if (ret == 0 && frame_->format == AV_PIX_FMT_VIDEOTOOLBOX) {
            int64_t pts = frame_->best_effort_timestamp;
            if (pts == AV_NOPTS_VALUE) pts = frame_->pts;
            if (pts == AV_NOPTS_VALUE) pts = 0;
            double time_sec = static_cast<double>(pts) * av_q2d(stream->time_base);
            int kf_frame = static_cast<int>(time_sec * fps_ + 0.5);

            CVPixelBufferRef pb = (CVPixelBufferRef)frame_->data[3];
            if (pb && hw_extractor_) {
                CVPixelBufferRetain(pb);
                MetalTextureFrame tex_frame = hw_extractor_->ExtractFromPixelBuffer((void*)pb);
                if (tex_frame.valid && yuv_renderer_) {
                    void* rgba_texture = yuv_renderer_->RenderToRGBA(
                        tex_frame.y_texture, tex_frame.uv_texture,
                        tex_frame.width, tex_frame.height,
                        tex_frame.bit_depth, is_full_range_,
                        is_bt2020_, is_hdr_,
                        static_cast<int>(tex_frame.subsampling));
                    hw_extractor_->ReleaseFrame(tex_frame);

                    if (rgba_texture) {
                        uint64_t pool_id = MetalTexturePool::Instance().RegisterExistingTexture(
                            rgba_texture, tex_frame.width, tex_frame.height, 1);

                        last_pool_texture_id_ = pool_id;
                        CVPixelBufferRelease(pb);

                        last_decoded_frame_ = kf_frame;
                        if (actual_frame) *actual_frame = kf_frame;
                        return pool_id;
                    }
                } else {
                    hw_extractor_->ReleaseFrame(tex_frame);
                }
                CVPixelBufferRelease(pb);
            }
        }
    }

    if (actual_frame) *actual_frame = -1;
    return 0;
}

#else
// Non-Metal: pool texture path not available
uint64_t ScrubDecoder::GetFrameAsPoolTexture(int frame_number, int* actual_frame) {
    (void)frame_number;
    if (actual_frame) *actual_frame = -1;
    return 0;
}
uint64_t ScrubDecoder::GetKeyframeAsPoolTexture(int frame_number, int* actual_frame) {
    (void)frame_number;
    if (actual_frame) *actual_frame = -1;
    return 0;
}
std::shared_ptr<PixelData> ScrubDecoder::GetKeyframe(int frame_number, int* actual_frame) {
    (void)frame_number;
    if (actual_frame) *actual_frame = -1;
    return nullptr;
}
#endif

//=============================================================================
// ScrubDecoderManager Implementation
//=============================================================================

ScrubDecoderManager::~ScrubDecoderManager() {
    ClearAll();
}

ScrubDecoder* ScrubDecoderManager::GetDecoder(const std::string& source_path) {
    if (source_path.empty()) return nullptr;

    std::lock_guard<std::mutex> lock(mutex_);

    // Check if decoder already exists
    auto it = decoders_.find(source_path);
    if (it != decoders_.end()) {
        return it->second.get();
    }

    // Create new decoder
    auto decoder = std::make_unique<ScrubDecoder>(source_path);
    if (!decoder->Initialize()) {
        Debug::Log("ScrubDecoderManager: Failed to create decoder for " + source_path);
        return nullptr;
    }

    ScrubDecoder* raw_ptr = decoder.get();
    decoders_[source_path] = std::move(decoder);

    Debug::Log("ScrubDecoderManager: Created decoder for " + source_path +
               " (total: " + std::to_string(decoders_.size()) + ")");
    return raw_ptr;
}

void ScrubDecoderManager::ClearAll() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!decoders_.empty()) {
        Debug::Log("ScrubDecoderManager: Clearing " + std::to_string(decoders_.size()) + " decoders");
        decoders_.clear();
    }
}

void ScrubDecoderManager::Clear(const std::string& source_path) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = decoders_.find(source_path);
    if (it != decoders_.end()) {
        Debug::Log("ScrubDecoderManager: Clearing decoder for " + source_path);
        decoders_.erase(it);
    }
}

bool ScrubDecoderManager::HasDecoders() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !decoders_.empty();
}

} // namespace qcview
