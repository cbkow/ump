#include "ffmpeg_video_encoder.h"
#include "../utils/debug_utils.h"
#include <algorithm>

namespace ump {

FFMPEGVideoEncoder::FFMPEGVideoEncoder() {
}

FFMPEGVideoEncoder::~FFMPEGVideoEncoder() {
    if (is_open_) {
        Close();
    }
}

bool FFMPEGVideoEncoder::Open(const EncoderSettings& settings) {
    if (is_open_) {
        Debug::Log("ERROR: FFMPEGVideoEncoder::Open - Encoder already open");
        return false;
    }

    if (settings.width <= 0 || settings.height <= 0) {
        Debug::Log("ERROR: FFMPEGVideoEncoder::Open - Invalid dimensions");
        return false;
    }

    if (settings.fps <= 0.0) {
        Debug::Log("ERROR: FFMPEGVideoEncoder::Open - Invalid frame rate");
        return false;
    }

    settings_ = settings;

    Debug::Log("FFMPEGVideoEncoder: Opening encoder");
    Debug::Log("  Output: " + settings_.output_path);
    Debug::Log("  Resolution: " + std::to_string(settings_.width) + "x" + std::to_string(settings_.height));
    Debug::Log("  Frame Rate: " + std::to_string(settings_.fps) + " fps");
    Debug::Log("  Codec: " + settings_.codec);

    // Create output context
    if (!CreateOutputContext()) {
        Debug::Log("ERROR: FFMPEGVideoEncoder::Open - Failed to create output context");
        return false;
    }

    // Initialize codec
    if (!InitializeCodec()) {
        Debug::Log("ERROR: FFMPEGVideoEncoder::Open - Failed to initialize codec");
        Close();
        return false;
    }

    // Open output file
    int ret = avio_open(&format_ctx_->pb, settings_.output_path.c_str(), AVIO_FLAG_WRITE);
    if (ret < 0) {
        char errbuf[128];
        av_strerror(ret, errbuf, sizeof(errbuf));
        Debug::Log("ERROR: FFMPEGVideoEncoder - Could not open output file: " + std::string(errbuf));
        Close();
        return false;
    }

    // Write header
    ret = avformat_write_header(format_ctx_, nullptr);
    if (ret < 0) {
        char errbuf[128];
        av_strerror(ret, errbuf, sizeof(errbuf));
        Debug::Log("ERROR: FFMPEGVideoEncoder - Error writing header: " + std::string(errbuf));
        Close();
        return false;
    }

    // Create scaler (RGBA → YUV)
    AVPixelFormat dst_pix_fmt = AV_PIX_FMT_YUV420P;
    if (settings_.pixel_format == "yuv444p") {
        dst_pix_fmt = AV_PIX_FMT_YUV444P;
    } else if (settings_.pixel_format == "yuv422p") {
        dst_pix_fmt = AV_PIX_FMT_YUV422P;
    } else if (settings_.pixel_format == "yuv422p10le") {
        dst_pix_fmt = AV_PIX_FMT_YUV422P10LE;  // ProRes 422
    } else if (settings_.pixel_format == "yuva444p10le") {
        dst_pix_fmt = AV_PIX_FMT_YUVA444P10LE;  // ProRes 4444
    }

    sws_ctx_ = sws_getContext(
        settings_.width, settings_.height, AV_PIX_FMT_RGBA,
        settings_.width, settings_.height, dst_pix_fmt,
        SWS_BICUBIC, nullptr, nullptr, nullptr
    );

    if (!sws_ctx_) {
        Debug::Log("ERROR: FFMPEGVideoEncoder - Could not create scaler context");
        Close();
        return false;
    }

    is_open_ = true;
    frame_count_ = 0;

    Debug::Log("FFMPEGVideoEncoder: Opened successfully");
    return true;
}

bool FFMPEGVideoEncoder::CreateOutputContext() {
    int ret = avformat_alloc_output_context2(&format_ctx_, nullptr, nullptr,
                                             settings_.output_path.c_str());
    if (ret < 0 || !format_ctx_) {
        char errbuf[128];
        av_strerror(ret, errbuf, sizeof(errbuf));
        Debug::Log("ERROR: FFMPEGVideoEncoder - Could not create output context: " + std::string(errbuf));
        return false;
    }

    return true;
}

bool FFMPEGVideoEncoder::InitializeCodec() {
    // Find encoder
    codec_ = avcodec_find_encoder_by_name(settings_.codec.c_str());
    if (!codec_) {
        Debug::Log("ERROR: FFMPEGVideoEncoder - Codec not found: " + settings_.codec);
        return false;
    }

    Debug::Log("FFMPEGVideoEncoder: Found codec: " + std::string(codec_->name));

    // Create stream
    video_stream_ = avformat_new_stream(format_ctx_, nullptr);
    if (!video_stream_) {
        Debug::Log("ERROR: FFMPEGVideoEncoder - Could not create video stream");
        return false;
    }

    video_stream_->id = format_ctx_->nb_streams - 1;

    // Create codec context
    codec_ctx_ = avcodec_alloc_context3(codec_);
    if (!codec_ctx_) {
        Debug::Log("ERROR: FFMPEGVideoEncoder - Could not allocate codec context");
        return false;
    }

    // Set codec parameters
    codec_ctx_->codec_id = codec_->id;
    codec_ctx_->width = settings_.width;
    codec_ctx_->height = settings_.height;

    // Convert fps to AVRational using FFmpeg's built-in parser
    // This is the same method FFmpeg uses for command-line -framerate argument
    AVRational framerate;
    std::string fps_str = std::to_string(settings_.fps);
    int parse_ret = av_parse_video_rate(&framerate, fps_str.c_str());
    if (parse_ret < 0) {
        Debug::Log("ERROR: FFMPEGVideoEncoder - Failed to parse frame rate: " + fps_str);
        return false;
    }

    codec_ctx_->framerate = framerate;
    codec_ctx_->time_base = av_inv_q(framerate);

    Debug::Log("FFMPEGVideoEncoder: Frame rate set to " + std::to_string(framerate.num) + "/" +
               std::to_string(framerate.den) + " (" + fps_str + " fps)");

    codec_ctx_->gop_size = settings_.gop_size;
    codec_ctx_->max_b_frames = settings_.max_b_frames;

    // Pixel format
    if (settings_.pixel_format == "yuv420p") {
        codec_ctx_->pix_fmt = AV_PIX_FMT_YUV420P;
    } else if (settings_.pixel_format == "yuv444p") {
        codec_ctx_->pix_fmt = AV_PIX_FMT_YUV444P;
    } else if (settings_.pixel_format == "yuv422p") {
        codec_ctx_->pix_fmt = AV_PIX_FMT_YUV422P;
    } else if (settings_.pixel_format == "yuv422p10le") {
        codec_ctx_->pix_fmt = AV_PIX_FMT_YUV422P10LE;
    } else if (settings_.pixel_format == "yuva444p10le") {
        codec_ctx_->pix_fmt = AV_PIX_FMT_YUVA444P10LE;
    } else {
        codec_ctx_->pix_fmt = AV_PIX_FMT_YUV420P;  // Default
    }

    // Quality settings
    if (settings_.codec == "libx264" || settings_.codec == "libx265") {
        // CRF mode (constant rate factor)
        if (settings_.bitrate_kbps == 0) {
            av_opt_set_int(codec_ctx_->priv_data, "crf", settings_.crf, 0);
            Debug::Log("FFMPEGVideoEncoder: Using CRF " + std::to_string(settings_.crf));
        } else {
            codec_ctx_->bit_rate = settings_.bitrate_kbps * 1000;
            Debug::Log("FFMPEGVideoEncoder: Using bitrate " + std::to_string(settings_.bitrate_kbps) + " kbps");
        }

        // Preset
        if (!settings_.preset.empty()) {
            av_opt_set(codec_ctx_->priv_data, "preset", settings_.preset.c_str(), 0);
            Debug::Log("FFMPEGVideoEncoder: Using preset: " + settings_.preset);
        }

        // Profile
        if (!settings_.profile.empty()) {
            av_opt_set(codec_ctx_->priv_data, "profile", settings_.profile.c_str(), 0);
        }
    } else if (settings_.codec == "prores_ks") {
        // ProRes profile
        // 0 = Proxy, 1 = LT, 2 = Standard, 3 = HQ, 4 = 4444, 5 = 4444 XQ
        // Use the profile from settings (set by transcode dialog)
        av_opt_set_int(codec_ctx_->priv_data, "profile", settings_.prores_profile, 0);
        Debug::Log("FFMPEGVideoEncoder: Using ProRes profile " + std::to_string(settings_.prores_profile));
    }

    // Some formats need this flag
    if (format_ctx_->oformat->flags & AVFMT_GLOBALHEADER) {
        codec_ctx_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    // Open codec
    int ret = avcodec_open2(codec_ctx_, codec_, nullptr);
    if (ret < 0) {
        char errbuf[128];
        av_strerror(ret, errbuf, sizeof(errbuf));
        Debug::Log("ERROR: FFMPEGVideoEncoder - Could not open codec: " + std::string(errbuf));
        return false;
    }

    // Copy codec parameters to stream
    ret = avcodec_parameters_from_context(video_stream_->codecpar, codec_ctx_);
    if (ret < 0) {
        char errbuf[128];
        av_strerror(ret, errbuf, sizeof(errbuf));
        Debug::Log("ERROR: FFMPEGVideoEncoder - Could not copy codec parameters: " + std::string(errbuf));
        return false;
    }

    video_stream_->time_base = codec_ctx_->time_base;
    video_stream_->avg_frame_rate = framerate;  // Set stream frame rate for container metadata
    video_stream_->r_frame_rate = framerate;    // Set real frame rate for container metadata

    Debug::Log("FFMPEGVideoEncoder: Codec initialized successfully");
    return true;
}

bool FFMPEGVideoEncoder::EncodeFrame(const uint8_t* rgba_pixels, int stride) {
    if (!is_open_) {
        Debug::Log("ERROR: FFMPEGVideoEncoder::EncodeFrame - Encoder not open");
        return false;
    }

    // Allocate frame
    AVFrame* frame = av_frame_alloc();
    if (!frame) {
        Debug::Log("ERROR: FFMPEGVideoEncoder - Could not allocate frame");
        return false;
    }

    frame->format = codec_ctx_->pix_fmt;
    frame->width = codec_ctx_->width;
    frame->height = codec_ctx_->height;
    frame->pts = frame_count_;

    int ret = av_frame_get_buffer(frame, 0);
    if (ret < 0) {
        char errbuf[128];
        av_strerror(ret, errbuf, sizeof(errbuf));
        Debug::Log("ERROR: FFMPEGVideoEncoder - Could not allocate frame data: " + std::string(errbuf));
        av_frame_free(&frame);
        return false;
    }

    // Convert RGBA → YUV
    const uint8_t* src_data[1] = { rgba_pixels };
    int src_linesize[1] = { stride };

    ret = sws_scale(sws_ctx_,
                    src_data, src_linesize,
                    0, codec_ctx_->height,
                    frame->data, frame->linesize);

    if (ret < 0) {
        char errbuf[128];
        av_strerror(ret, errbuf, sizeof(errbuf));
        Debug::Log("ERROR: FFMPEGVideoEncoder - sws_scale failed: " + std::string(errbuf));
        av_frame_free(&frame);
        return false;
    }

    // Encode frame
    bool success = EncodeAndWrite(frame);

    av_frame_free(&frame);

    if (success) {
        frame_count_++;
    }

    return success;
}

bool FFMPEGVideoEncoder::EncodeAndWrite(AVFrame* frame) {
    // Send frame to encoder
    int ret = avcodec_send_frame(codec_ctx_, frame);
    if (ret < 0) {
        char errbuf[128];
        av_strerror(ret, errbuf, sizeof(errbuf));
        Debug::Log("ERROR: FFMPEGVideoEncoder - Error sending frame to encoder: " + std::string(errbuf));
        return false;
    }

    // Receive encoded packets
    while (ret >= 0) {
        AVPacket* pkt = av_packet_alloc();
        if (!pkt) {
            Debug::Log("ERROR: FFMPEGVideoEncoder - Could not allocate packet");
            return false;
        }

        ret = avcodec_receive_packet(codec_ctx_, pkt);

        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            av_packet_free(&pkt);
            break;
        } else if (ret < 0) {
            char errbuf[128];
            av_strerror(ret, errbuf, sizeof(errbuf));
            Debug::Log("ERROR: FFMPEGVideoEncoder - Error receiving packet: " + std::string(errbuf));
            av_packet_free(&pkt);
            return false;
        }

        // Rescale packet timestamps
        av_packet_rescale_ts(pkt, codec_ctx_->time_base, video_stream_->time_base);
        pkt->stream_index = video_stream_->index;

        // Write packet to file
        ret = av_interleaved_write_frame(format_ctx_, pkt);
        if (ret < 0) {
            char errbuf[128];
            av_strerror(ret, errbuf, sizeof(errbuf));
            Debug::Log("ERROR: FFMPEGVideoEncoder - Error writing packet: " + std::string(errbuf));
            av_packet_free(&pkt);
            return false;
        }

        av_packet_free(&pkt);
    }

    return true;
}

bool FFMPEGVideoEncoder::FlushEncoder() {
    if (!codec_ctx_) return true;

    Debug::Log("FFMPEGVideoEncoder: Flushing encoder");

    // Send NULL frame to flush
    int ret = avcodec_send_frame(codec_ctx_, nullptr);
    if (ret < 0 && ret != AVERROR_EOF) {
        char errbuf[128];
        av_strerror(ret, errbuf, sizeof(errbuf));
        Debug::Log("ERROR: FFMPEGVideoEncoder - Error flushing encoder: " + std::string(errbuf));
        return false;
    }

    // Receive remaining packets
    while (true) {
        AVPacket* pkt = av_packet_alloc();
        if (!pkt) break;

        ret = avcodec_receive_packet(codec_ctx_, pkt);

        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            av_packet_free(&pkt);
            break;
        } else if (ret < 0) {
            av_packet_free(&pkt);
            break;
        }

        av_packet_rescale_ts(pkt, codec_ctx_->time_base, video_stream_->time_base);
        pkt->stream_index = video_stream_->index;

        av_interleaved_write_frame(format_ctx_, pkt);
        av_packet_free(&pkt);
    }

    return true;
}

bool FFMPEGVideoEncoder::Close() {
    if (!is_open_) {
        return true;
    }

    Debug::Log("FFMPEGVideoEncoder: Closing encoder");
    Debug::Log("  Total frames encoded: " + std::to_string(frame_count_));

    // Flush encoder
    FlushEncoder();

    // Write trailer
    if (format_ctx_) {
        av_write_trailer(format_ctx_);
    }

    // Cleanup
    if (sws_ctx_) {
        sws_freeContext(sws_ctx_);
        sws_ctx_ = nullptr;
    }

    if (codec_ctx_) {
        avcodec_free_context(&codec_ctx_);
        codec_ctx_ = nullptr;
    }

    if (format_ctx_) {
        if (format_ctx_->pb) {
            avio_closep(&format_ctx_->pb);
        }
        avformat_free_context(format_ctx_);
        format_ctx_ = nullptr;
    }

    is_open_ = false;

    Debug::Log("FFMPEGVideoEncoder: Closed successfully");
    return true;
}

} // namespace ump
