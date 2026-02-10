#include "scrub_decoder.h"
#include "../utils/debug_utils.h"

#include <glad/gl.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}

namespace ump {

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

    // Use fewer threads for scrubbing (less overhead)
    codec_ctx_->thread_count = 2;

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

    initialized_ = true;
    Debug::Log("ScrubDecoder: Initialized " + std::to_string(width_) + "x" +
               std::to_string(height_) + " @ " + std::to_string(fps_) + " fps");
    return true;
}

void ScrubDecoder::Shutdown() {
    std::lock_guard<std::mutex> lock(decode_mutex_);

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

bool ScrubDecoder::SeekToKeyframe(int target_frame) {
    if (!format_ctx_ || video_stream_idx_ < 0) return false;

    AVStream* stream = format_ctx_->streams[video_stream_idx_];

    // Convert frame number to timestamp
    int64_t target_pts = static_cast<int64_t>(target_frame / fps_ * AV_TIME_BASE) + start_time_;

    // Seek backward to keyframe
    int ret = av_seek_frame(format_ctx_, -1, target_pts, AVSEEK_FLAG_BACKWARD);
    if (ret < 0) {
        // Try seeking to beginning
        ret = av_seek_frame(format_ctx_, -1, start_time_, AVSEEK_FLAG_BACKWARD);
        if (ret < 0) return false;
    }

    // Flush codec buffers after seek
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

    // Decode frames until we reach target or close enough
    int decoded_frame = -1;
    int max_frames_to_decode = 30;  // Don't decode more than 30 frames to reach target
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

            // Got the target frame or close enough
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

    // Create/update sws context for conversion to RGBA
    sws_ctx_ = sws_getCachedContext(
        sws_ctx_,
        frame->width, frame->height, static_cast<AVPixelFormat>(frame->format),
        frame->width, frame->height, AV_PIX_FMT_RGBA,
        SWS_BILINEAR, nullptr, nullptr, nullptr
    );

    if (!sws_ctx_) return nullptr;

    // Allocate output buffer
    auto pixels = std::make_shared<PixelData>();
    pixels->width = frame->width;
    pixels->height = frame->height;
    pixels->gl_format = GL_RGBA;
    pixels->gl_type = GL_UNSIGNED_BYTE;
    pixels->pixels.resize(frame->width * frame->height * 4);

    // Set up destination
    uint8_t* dst_data[1] = { pixels->pixels.data() };
    int dst_linesize[1] = { frame->width * 4 };

    // Convert
    sws_scale(sws_ctx_, frame->data, frame->linesize, 0, frame->height,
              dst_data, dst_linesize);

    return pixels;
}

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

} // namespace ump
