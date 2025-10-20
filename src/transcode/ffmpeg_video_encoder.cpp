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

    // NOTE: Header write deferred to WriteHeader() method
    // This allows adding audio streams before finalizing the header

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

bool FFMPEGVideoEncoder::WriteHeader() {
    if (!format_ctx_) {
        Debug::Log("ERROR: FFMPEGVideoEncoder::WriteHeader - Encoder not open");
        return false;
    }

    if (header_written_) {
        Debug::Log("WARNING: FFMPEGVideoEncoder::WriteHeader - Header already written");
        return true;
    }

    // Write file header (must be called after all streams are added)
    int ret = avformat_write_header(format_ctx_, nullptr);
    if (ret < 0) {
        char errbuf[128];
        av_strerror(ret, errbuf, sizeof(errbuf));
        Debug::Log("ERROR: FFMPEGVideoEncoder::WriteHeader - Error writing header: " + std::string(errbuf));
        return false;
    }

    header_written_ = true;
    Debug::Log("FFMPEGVideoEncoder: Header written successfully");

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

    // Cleanup audio contexts (decoder/encoder/resampler/fifo)
    for (auto& audio_info : audio_streams_) {
        if (audio_info.fifo) {
            av_audio_fifo_free(audio_info.fifo);
        }
        if (audio_info.swr_ctx) {
            swr_free(&audio_info.swr_ctx);
        }
        if (audio_info.decoder_ctx) {
            avcodec_free_context(&audio_info.decoder_ctx);
        }
        if (audio_info.encoder_ctx) {
            avcodec_free_context(&audio_info.encoder_ctx);
        }
    }
    audio_streams_.clear();

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

// ============================================================================
// Audio Stream Support (NEW - Stream Copy Mode)
// ============================================================================

bool FFMPEGVideoEncoder::AddAudioStream(AVFormatContext* source_format_ctx, int source_stream_index) {
    if (!format_ctx_) {
        Debug::Log("ERROR: FFMPEGVideoEncoder::AddAudioStream - Encoder not open");
        return false;
    }

    if (source_stream_index < 0 || source_stream_index >= (int)source_format_ctx->nb_streams) {
        Debug::Log("ERROR: FFMPEGVideoEncoder::AddAudioStream - Invalid stream index");
        return false;
    }

    AVStream* source_stream = source_format_ctx->streams[source_stream_index];

    if (source_stream->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) {
        Debug::Log("ERROR: FFMPEGVideoEncoder::AddAudioStream - Not an audio stream");
        return false;
    }

    // Create output audio stream
    AVStream* output_stream = avformat_new_stream(format_ctx_, nullptr);
    if (!output_stream) {
        Debug::Log("ERROR: FFMPEGVideoEncoder::AddAudioStream - Failed to create output stream");
        return false;
    }

    // Copy codec parameters (stream copy mode - no re-encoding)
    int ret = avcodec_parameters_copy(output_stream->codecpar, source_stream->codecpar);
    if (ret < 0) {
        char errbuf[128];
        av_strerror(ret, errbuf, sizeof(errbuf));
        Debug::Log("ERROR: FFMPEGVideoEncoder::AddAudioStream - Failed to copy codec parameters: " + std::string(errbuf));
        return false;
    }

    // Set codec tag to 0 (let muxer decide)
    output_stream->codecpar->codec_tag = 0;

    // Copy time base
    output_stream->time_base = source_stream->time_base;

    // Store mapping info
    AudioStreamInfo info;
    info.source_stream_index = source_stream_index;
    info.output_stream_index = output_stream->index;
    info.output_stream = output_stream;
    info.source_time_base = source_stream->time_base;

    audio_streams_.push_back(info);

    const AVCodec* codec = avcodec_find_decoder(source_stream->codecpar->codec_id);
    std::string codec_name = codec ? codec->name : "unknown";

    Debug::Log("FFMPEGVideoEncoder: Added audio stream #" + std::to_string(source_stream_index) +
               " (" + codec_name + ", " +
               std::to_string(source_stream->codecpar->sample_rate) + " Hz, " +
               std::to_string(source_stream->codecpar->ch_layout.nb_channels) + " channels)");

    return true;
}

bool FFMPEGVideoEncoder::AddAudioStreamWithEncoding(AVFormatContext* source_format_ctx, int source_stream_index,
                                                     const std::string& target_codec, int target_bitrate_kbps) {
    if (!format_ctx_) {
        Debug::Log("ERROR: FFMPEGVideoEncoder::AddAudioStreamWithEncoding - Encoder not open");
        return false;
    }

    if (source_stream_index < 0 || source_stream_index >= (int)source_format_ctx->nb_streams) {
        Debug::Log("ERROR: FFMPEGVideoEncoder::AddAudioStreamWithEncoding - Invalid stream index");
        return false;
    }

    AVStream* source_stream = source_format_ctx->streams[source_stream_index];

    if (source_stream->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) {
        Debug::Log("ERROR: FFMPEGVideoEncoder::AddAudioStreamWithEncoding - Not an audio stream");
        return false;
    }

    // ========================================================================
    // Step 1: Create decoder for source audio
    // ========================================================================

    const AVCodec* source_codec = avcodec_find_decoder(source_stream->codecpar->codec_id);
    if (!source_codec) {
        Debug::Log("ERROR: FFMPEGVideoEncoder::AddAudioStreamWithEncoding - Source decoder not found");
        return false;
    }

    AVCodecContext* decoder_ctx = avcodec_alloc_context3(source_codec);
    if (!decoder_ctx) {
        Debug::Log("ERROR: FFMPEGVideoEncoder::AddAudioStreamWithEncoding - Failed to allocate decoder context");
        return false;
    }

    int ret = avcodec_parameters_to_context(decoder_ctx, source_stream->codecpar);
    if (ret < 0) {
        char errbuf[128];
        av_strerror(ret, errbuf, sizeof(errbuf));
        Debug::Log("ERROR: FFMPEGVideoEncoder::AddAudioStreamWithEncoding - Failed to copy decoder parameters: " + std::string(errbuf));
        avcodec_free_context(&decoder_ctx);
        return false;
    }

    ret = avcodec_open2(decoder_ctx, source_codec, nullptr);
    if (ret < 0) {
        char errbuf[128];
        av_strerror(ret, errbuf, sizeof(errbuf));
        Debug::Log("ERROR: FFMPEGVideoEncoder::AddAudioStreamWithEncoding - Failed to open decoder: " + std::string(errbuf));
        avcodec_free_context(&decoder_ctx);
        return false;
    }

    Debug::Log("FFMPEGVideoEncoder: Opened decoder for " + std::string(source_codec->name));

    // ========================================================================
    // Step 2: Find AAC encoder (try libfdk_aac first, fallback to native aac)
    // ========================================================================

    const AVCodec* encoder = avcodec_find_encoder_by_name(target_codec.c_str());
    if (!encoder) {
        // Fallback: if target_codec == "aac", try "libfdk_aac" first
        if (target_codec == "aac") {
            encoder = avcodec_find_encoder_by_name("libfdk_aac");
            if (!encoder) {
                encoder = avcodec_find_encoder_by_name("aac");
            }
        }
    }

    if (!encoder) {
        Debug::Log("ERROR: FFMPEGVideoEncoder::AddAudioStreamWithEncoding - Encoder not found: " + target_codec);
        avcodec_free_context(&decoder_ctx);
        return false;
    }

    Debug::Log("FFMPEGVideoEncoder: Using audio encoder: " + std::string(encoder->name));

    // ========================================================================
    // Step 3: Configure encoder context
    // ========================================================================

    AVCodecContext* encoder_ctx = avcodec_alloc_context3(encoder);
    if (!encoder_ctx) {
        Debug::Log("ERROR: FFMPEGVideoEncoder::AddAudioStreamWithEncoding - Failed to allocate encoder context");
        avcodec_free_context(&decoder_ctx);
        return false;
    }

    // Copy basic parameters from source
    encoder_ctx->sample_rate = decoder_ctx->sample_rate;
    encoder_ctx->ch_layout = decoder_ctx->ch_layout;
    encoder_ctx->bit_rate = target_bitrate_kbps * 1000;

    // AAC requires specific sample format (usually planar float)
    if (encoder->sample_fmts) {
        encoder_ctx->sample_fmt = encoder->sample_fmts[0];  // Use first supported format
    } else {
        encoder_ctx->sample_fmt = AV_SAMPLE_FMT_FLTP;  // Default for AAC
    }

    // Set time base
    encoder_ctx->time_base = {1, encoder_ctx->sample_rate};

    // Some formats need global header
    if (format_ctx_->oformat->flags & AVFMT_GLOBALHEADER) {
        encoder_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    ret = avcodec_open2(encoder_ctx, encoder, nullptr);
    if (ret < 0) {
        char errbuf[128];
        av_strerror(ret, errbuf, sizeof(errbuf));
        Debug::Log("ERROR: FFMPEGVideoEncoder::AddAudioStreamWithEncoding - Failed to open encoder: " + std::string(errbuf));
        avcodec_free_context(&decoder_ctx);
        avcodec_free_context(&encoder_ctx);
        return false;
    }

    Debug::Log("FFMPEGVideoEncoder: Opened encoder " + std::string(encoder->name) + " at " +
               std::to_string(target_bitrate_kbps) + " kbps");
    Debug::Log("FFMPEGVideoEncoder: Encoder frame_size = " + std::to_string(encoder_ctx->frame_size) +
               " (0 = variable size accepted)");

    // ========================================================================
    // Step 4: Create resampler (for format/sample rate conversion)
    // ========================================================================

    SwrContext* swr_ctx = nullptr;

    // Check if we need resampling (different sample format, rate, or layout)
    bool needs_resampling = (decoder_ctx->sample_fmt != encoder_ctx->sample_fmt) ||
                            (decoder_ctx->sample_rate != encoder_ctx->sample_rate) ||
                            (av_channel_layout_compare(&decoder_ctx->ch_layout, &encoder_ctx->ch_layout) != 0);

    if (needs_resampling) {
        ret = swr_alloc_set_opts2(&swr_ctx,
                                  &encoder_ctx->ch_layout,
                                  encoder_ctx->sample_fmt,
                                  encoder_ctx->sample_rate,
                                  &decoder_ctx->ch_layout,
                                  decoder_ctx->sample_fmt,
                                  decoder_ctx->sample_rate,
                                  0, nullptr);

        if (ret < 0) {
            char errbuf[128];
            av_strerror(ret, errbuf, sizeof(errbuf));
            Debug::Log("ERROR: FFMPEGVideoEncoder::AddAudioStreamWithEncoding - Failed to allocate resampler: " + std::string(errbuf));
            avcodec_free_context(&decoder_ctx);
            avcodec_free_context(&encoder_ctx);
            return false;
        }

        ret = swr_init(swr_ctx);
        if (ret < 0) {
            char errbuf[128];
            av_strerror(ret, errbuf, sizeof(errbuf));
            Debug::Log("ERROR: FFMPEGVideoEncoder::AddAudioStreamWithEncoding - Failed to initialize resampler: " + std::string(errbuf));
            swr_free(&swr_ctx);
            avcodec_free_context(&decoder_ctx);
            avcodec_free_context(&encoder_ctx);
            return false;
        }

        Debug::Log("FFMPEGVideoEncoder: Created resampler (format/rate conversion required)");
    }

    // ========================================================================
    // Step 5: Create output stream with encoder parameters
    // ========================================================================

    AVStream* output_stream = avformat_new_stream(format_ctx_, nullptr);
    if (!output_stream) {
        Debug::Log("ERROR: FFMPEGVideoEncoder::AddAudioStreamWithEncoding - Failed to create output stream");
        if (swr_ctx) swr_free(&swr_ctx);
        avcodec_free_context(&decoder_ctx);
        avcodec_free_context(&encoder_ctx);
        return false;
    }

    ret = avcodec_parameters_from_context(output_stream->codecpar, encoder_ctx);
    if (ret < 0) {
        char errbuf[128];
        av_strerror(ret, errbuf, sizeof(errbuf));
        Debug::Log("ERROR: FFMPEGVideoEncoder::AddAudioStreamWithEncoding - Failed to copy encoder parameters: " + std::string(errbuf));
        if (swr_ctx) swr_free(&swr_ctx);
        avcodec_free_context(&decoder_ctx);
        avcodec_free_context(&encoder_ctx);
        return false;
    }

    output_stream->time_base = encoder_ctx->time_base;

    // ========================================================================
    // Step 6: Create audio FIFO for fixed-size frame buffering
    // ========================================================================

    AVAudioFifo* fifo = av_audio_fifo_alloc(encoder_ctx->sample_fmt,
                                             encoder_ctx->ch_layout.nb_channels,
                                             encoder_ctx->frame_size * 2);  // Buffer for 2 frames
    if (!fifo) {
        Debug::Log("ERROR: FFMPEGVideoEncoder::AddAudioStreamWithEncoding - Failed to create audio FIFO");
        if (swr_ctx) swr_free(&swr_ctx);
        avcodec_free_context(&decoder_ctx);
        avcodec_free_context(&encoder_ctx);
        return false;
    }

    Debug::Log("FFMPEGVideoEncoder: Created audio FIFO for frame buffering (frame_size=" +
               std::to_string(encoder_ctx->frame_size) + ")");

    // ========================================================================
    // Step 7: Store contexts in AudioStreamInfo
    // ========================================================================

    AudioStreamInfo info;
    info.source_stream_index = source_stream_index;
    info.output_stream_index = output_stream->index;
    info.output_stream = output_stream;
    info.source_time_base = source_stream->time_base;
    info.needs_reencoding = true;
    info.decoder_ctx = decoder_ctx;
    info.encoder_ctx = encoder_ctx;
    info.swr_ctx = swr_ctx;
    info.fifo = fifo;

    audio_streams_.push_back(info);

    Debug::Log("FFMPEGVideoEncoder: Added audio stream #" + std::to_string(source_stream_index) +
               " with re-encoding (" + std::string(source_codec->name) + " -> " + std::string(encoder->name) + ", " +
               std::to_string(encoder_ctx->sample_rate) + " Hz, " +
               std::to_string(encoder_ctx->ch_layout.nb_channels) + " channels, " +
               std::to_string(target_bitrate_kbps) + " kbps)");

    return true;
}

bool FFMPEGVideoEncoder::WriteAudioPacket(AVPacket* packet, int source_stream_index) {
    if (!format_ctx_ || !packet) {
        return false;
    }

    // Find corresponding output stream
    AudioStreamInfo* audio_info = nullptr;
    for (auto& info : audio_streams_) {
        if (info.source_stream_index == source_stream_index) {
            audio_info = &info;
            break;
        }
    }

    if (!audio_info || !audio_info->output_stream) {
        Debug::Log("ERROR: FFMPEGVideoEncoder::WriteAudioPacket - Audio stream not found");
        return false;
    }

    // ========================================================================
    // MODE 1: Stream copy (no re-encoding)
    // ========================================================================
    if (!audio_info->needs_reencoding) {
        // Clone packet (we'll modify timestamps)
        AVPacket* out_packet = av_packet_clone(packet);
        if (!out_packet) {
            Debug::Log("ERROR: FFMPEGVideoEncoder::WriteAudioPacket - Failed to clone packet");
            return false;
        }

        // Update stream index to output stream
        out_packet->stream_index = audio_info->output_stream_index;

        // Rescale timestamps from source time base to output time base
        AVRational src_time_base = audio_info->source_time_base;
        AVRational dst_time_base = audio_info->output_stream->time_base;

        // Validate time base to prevent division by zero
        if (dst_time_base.den == 0) {
            Debug::Log("ERROR: FFMPEGVideoEncoder::WriteAudioPacket - Invalid output time_base (den=0)");
            av_packet_free(&out_packet);
            return false;
        }

        // Rescale timestamps
        if (packet->pts != AV_NOPTS_VALUE) {
            out_packet->pts = av_rescale_q(packet->pts, src_time_base, dst_time_base);
        }
        if (packet->dts != AV_NOPTS_VALUE) {
            out_packet->dts = av_rescale_q(packet->dts, src_time_base, dst_time_base);
        }
        if (packet->duration > 0) {
            out_packet->duration = av_rescale_q(packet->duration, src_time_base, dst_time_base);
        }
        out_packet->pos = -1;  // Position in stream (let muxer decide)

        // Write packet with interleaving (maintains proper A/V sync)
        int ret = av_interleaved_write_frame(format_ctx_, out_packet);

        av_packet_free(&out_packet);

        if (ret < 0) {
            char errbuf[128];
            av_strerror(ret, errbuf, sizeof(errbuf));
            Debug::Log("ERROR: FFMPEGVideoEncoder::WriteAudioPacket - Failed to write packet: " + std::string(errbuf));
            return false;
        }

        return true;
    }

    // ========================================================================
    // MODE 2: Re-encode (decode → resample → FIFO → encode)
    // ========================================================================

    // Helper lambda to encode and write one frame from FIFO
    auto encode_frame_from_fifo = [&](AudioStreamInfo* info) -> bool {
        int frame_size = info->encoder_ctx->frame_size;

        // Check if we have enough samples
        if (av_audio_fifo_size(info->fifo) < frame_size) {
            return true;  // Not an error, just not enough samples yet
        }

        // Allocate frame for encoder
        AVFrame* encode_frame = av_frame_alloc();
        if (!encode_frame) {
            Debug::Log("ERROR: Failed to allocate encode frame");
            return false;
        }

        encode_frame->nb_samples = frame_size;
        encode_frame->sample_rate = info->encoder_ctx->sample_rate;
        encode_frame->ch_layout = info->encoder_ctx->ch_layout;
        encode_frame->format = info->encoder_ctx->sample_fmt;
        encode_frame->pts = info->next_pts;
        info->next_pts += frame_size;

        int ret = av_frame_get_buffer(encode_frame, 0);
        if (ret < 0) {
            av_frame_free(&encode_frame);
            return false;
        }

        // Read samples from FIFO
        ret = av_audio_fifo_read(info->fifo, (void**)encode_frame->data, frame_size);
        if (ret < frame_size) {
            av_frame_free(&encode_frame);
            return false;
        }

        // Encode frame
        ret = avcodec_send_frame(info->encoder_ctx, encode_frame);
        av_frame_free(&encode_frame);

        if (ret < 0) {
            char errbuf[128];
            av_strerror(ret, errbuf, sizeof(errbuf));
            Debug::Log("ERROR: Failed to send frame to encoder: " + std::string(errbuf));
            return false;
        }

        // Receive and write encoded packets
        while (true) {
            AVPacket* encoded_packet = av_packet_alloc();
            if (!encoded_packet) return false;

            ret = avcodec_receive_packet(info->encoder_ctx, encoded_packet);

            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                av_packet_free(&encoded_packet);
                break;
            } else if (ret < 0) {
                av_packet_free(&encoded_packet);
                return false;
            }

            // Set stream index and rescale timestamps
            encoded_packet->stream_index = info->output_stream_index;
            av_packet_rescale_ts(encoded_packet, info->encoder_ctx->time_base, info->output_stream->time_base);

            // Write encoded packet
            ret = av_interleaved_write_frame(format_ctx_, encoded_packet);
            av_packet_free(&encoded_packet);

            if (ret < 0) {
                return false;
            }
        }

        return true;
    };

    // Step 1: Decode packet to raw audio frames
    int ret = avcodec_send_packet(audio_info->decoder_ctx, packet);
    if (ret < 0) {
        char errbuf[128];
        av_strerror(ret, errbuf, sizeof(errbuf));
        Debug::Log("ERROR: Failed to send packet to decoder: " + std::string(errbuf));
        return false;
    }

    // Process all decoded frames
    while (true) {
        AVFrame* decoded_frame = av_frame_alloc();
        if (!decoded_frame) {
            return false;
        }

        ret = avcodec_receive_frame(audio_info->decoder_ctx, decoded_frame);

        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            av_frame_free(&decoded_frame);
            break;
        } else if (ret < 0) {
            av_frame_free(&decoded_frame);
            return false;
        }

        // Step 2: Resample if needed
        const uint8_t** input_data = (const uint8_t**)decoded_frame->data;
        int input_samples = decoded_frame->nb_samples;

        if (audio_info->swr_ctx) {
            // Calculate max output samples
            int max_output = av_rescale_rnd(
                swr_get_delay(audio_info->swr_ctx, decoded_frame->sample_rate) + decoded_frame->nb_samples,
                audio_info->encoder_ctx->sample_rate,
                decoded_frame->sample_rate,
                AV_ROUND_UP);

            // Allocate temporary buffer for resampled data
            uint8_t** resampled_data = nullptr;
            ret = av_samples_alloc_array_and_samples(&resampled_data, nullptr,
                                                      audio_info->encoder_ctx->ch_layout.nb_channels,
                                                      max_output,
                                                      audio_info->encoder_ctx->sample_fmt, 0);
            if (ret < 0) {
                av_frame_free(&decoded_frame);
                return false;
            }

            // Resample
            int output_samples = swr_convert(audio_info->swr_ctx,
                                             resampled_data, max_output,
                                             input_data, input_samples);

            if (output_samples < 0) {
                av_freep(&resampled_data[0]);
                av_freep(&resampled_data);
                av_frame_free(&decoded_frame);
                return false;
            }

            // Write resampled data to FIFO
            ret = av_audio_fifo_write(audio_info->fifo, (void**)resampled_data, output_samples);

            av_freep(&resampled_data[0]);
            av_freep(&resampled_data);
        } else {
            // No resampling needed - write directly to FIFO
            ret = av_audio_fifo_write(audio_info->fifo, (void**)input_data, input_samples);
        }

        av_frame_free(&decoded_frame);

        if (ret < 0) {
            return false;
        }

        // Step 3: Encode as many frames as possible from FIFO
        while (av_audio_fifo_size(audio_info->fifo) >= audio_info->encoder_ctx->frame_size) {
            if (!encode_frame_from_fifo(audio_info)) {
                return false;
            }
        }
    }

    return true;
}

} // namespace ump
