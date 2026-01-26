#include "audio_decoder.h"
#include "../utils/debug_utils.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
}

#include <cstring>
#include <algorithm>

namespace ump {

//=============================================================================
// Constructor / Destructor
//=============================================================================

AudioDecoder::AudioDecoder() {
    // Set default output format
    output_format_.sample_rate = 48000;
    output_format_.channels = 2;
    output_format_.bytes_per_sample = 4;  // float32
}

AudioDecoder::~AudioDecoder() {
    Close();
}

//=============================================================================
// Lifecycle
//=============================================================================

bool AudioDecoder::Open(const std::string& file_path) {
    if (is_open_) {
        Close();
    }

    file_path_ = file_path;
    Debug::Log("AudioDecoder: Opening " + file_path);

    if (!OpenAudioStream()) {
        Debug::Log("AudioDecoder: No audio stream found in " + file_path);
        has_audio_ = false;
        return false;
    }

    has_audio_ = true;

    // Create ring buffer (~11 seconds at 48kHz stereo float32)
    ring_buffer_ = std::make_unique<AudioRingBuffer>(2 * 1024 * 1024);

    is_open_ = true;
    Debug::Log("AudioDecoder: Opened successfully - " +
               std::to_string(source_sample_rate_) + " Hz, " +
               std::to_string(source_channels_) + " ch, " +
               std::to_string(duration_) + "s duration");

    return true;
}

void AudioDecoder::Close() {
    if (!is_open_) return;

    Debug::Log("AudioDecoder: Closing...");

    Stop();
    CloseAudioStream();

    ring_buffer_.reset();

    is_open_ = false;
    has_audio_ = false;
    file_path_.clear();
    duration_ = 0.0;
    source_sample_rate_ = 0;
    source_channels_ = 0;

    Debug::Log("AudioDecoder: Closed");
}

void AudioDecoder::Start() {
    if (!is_open_ || running_) return;

    Debug::Log("AudioDecoder: Starting decode thread");

    eof_reached_ = false;
    running_ = true;
    decode_thread_ = std::thread(&AudioDecoder::DecodeThread, this);
}

void AudioDecoder::Stop() {
    if (!running_) return;

    Debug::Log("AudioDecoder: Stopping decode thread");

    running_ = false;
    seek_cv_.notify_all();

    if (decode_thread_.joinable()) {
        decode_thread_.join();
    }

    Debug::Log("AudioDecoder: Decode thread stopped");
}

//=============================================================================
// FFmpeg Initialization
//=============================================================================

bool AudioDecoder::OpenAudioStream() {
    // Open input file
    format_ctx_ = avformat_alloc_context();
    if (avformat_open_input(&format_ctx_, file_path_.c_str(), nullptr, nullptr) < 0) {
        Debug::Log("AudioDecoder: Failed to open input file");
        return false;
    }

    // Find stream info
    if (avformat_find_stream_info(format_ctx_, nullptr) < 0) {
        Debug::Log("AudioDecoder: Failed to find stream info");
        avformat_close_input(&format_ctx_);
        format_ctx_ = nullptr;
        return false;
    }

    // Find audio stream
    audio_stream_idx_ = av_find_best_stream(format_ctx_, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (audio_stream_idx_ < 0) {
        Debug::Log("AudioDecoder: No audio stream found");
        avformat_close_input(&format_ctx_);
        format_ctx_ = nullptr;
        return false;
    }

    AVStream* audio_stream = format_ctx_->streams[audio_stream_idx_];

    // Get source audio info
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(59, 37, 100)
    // FFmpeg 5.1+
    source_channels_ = audio_stream->codecpar->ch_layout.nb_channels;
#else
    source_channels_ = audio_stream->codecpar->channels;
#endif
    source_sample_rate_ = audio_stream->codecpar->sample_rate;

    // Get duration
    if (format_ctx_->duration > 0) {
        duration_ = static_cast<double>(format_ctx_->duration) / AV_TIME_BASE;
    } else if (audio_stream->duration > 0) {
        duration_ = static_cast<double>(audio_stream->duration) * av_q2d(audio_stream->time_base);
    }

    // Get start time
    if (audio_stream->start_time != AV_NOPTS_VALUE) {
        start_time_ = audio_stream->start_time;
    }

    // Find decoder
    const AVCodec* codec = avcodec_find_decoder(audio_stream->codecpar->codec_id);
    if (!codec) {
        Debug::Log("AudioDecoder: No decoder found for audio codec");
        avformat_close_input(&format_ctx_);
        format_ctx_ = nullptr;
        return false;
    }

    // Allocate codec context
    codec_ctx_ = avcodec_alloc_context3(codec);
    if (!codec_ctx_) {
        Debug::Log("AudioDecoder: Failed to allocate codec context");
        avformat_close_input(&format_ctx_);
        format_ctx_ = nullptr;
        return false;
    }

    // Copy codec parameters
    if (avcodec_parameters_to_context(codec_ctx_, audio_stream->codecpar) < 0) {
        Debug::Log("AudioDecoder: Failed to copy codec parameters");
        avcodec_free_context(&codec_ctx_);
        avformat_close_input(&format_ctx_);
        format_ctx_ = nullptr;
        return false;
    }

    // Open codec
    if (avcodec_open2(codec_ctx_, codec, nullptr) < 0) {
        Debug::Log("AudioDecoder: Failed to open codec");
        avcodec_free_context(&codec_ctx_);
        avformat_close_input(&format_ctx_);
        format_ctx_ = nullptr;
        return false;
    }

    // Setup resampler
    if (!SetupResampler()) {
        Debug::Log("AudioDecoder: Failed to setup resampler");
        avcodec_free_context(&codec_ctx_);
        avformat_close_input(&format_ctx_);
        format_ctx_ = nullptr;
        return false;
    }

    // Allocate frame and packet
    decode_frame_ = av_frame_alloc();
    packet_ = av_packet_alloc();

    if (!decode_frame_ || !packet_) {
        Debug::Log("AudioDecoder: Failed to allocate frame/packet");
        CloseAudioStream();
        return false;
    }

    return true;
}

bool AudioDecoder::SetupResampler() {
    // Setup SwrContext to convert to float32 stereo 48kHz
#if LIBSWRESAMPLE_VERSION_INT >= AV_VERSION_INT(4, 5, 100)
    // FFmpeg 5.1+ with channel layout API
    AVChannelLayout out_ch_layout = AV_CHANNEL_LAYOUT_STEREO;
    AVChannelLayout in_ch_layout;

#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(59, 37, 100)
    in_ch_layout = codec_ctx_->ch_layout;
#else
    av_channel_layout_default(&in_ch_layout, codec_ctx_->channels);
#endif

    int ret = swr_alloc_set_opts2(&swr_ctx_,
                                   &out_ch_layout,
                                   AV_SAMPLE_FMT_FLT,
                                   output_format_.sample_rate,
                                   &in_ch_layout,
                                   codec_ctx_->sample_fmt,
                                   codec_ctx_->sample_rate,
                                   0, nullptr);
    if (ret < 0) {
        Debug::Log("AudioDecoder: Failed to allocate resampler");
        return false;
    }
#else
    // Older FFmpeg
    int64_t out_ch_layout = AV_CH_LAYOUT_STEREO;
    int64_t in_ch_layout = codec_ctx_->channel_layout;
    if (in_ch_layout == 0) {
        in_ch_layout = av_get_default_channel_layout(codec_ctx_->channels);
    }

    swr_ctx_ = swr_alloc_set_opts(nullptr,
                                   out_ch_layout,
                                   AV_SAMPLE_FMT_FLT,
                                   output_format_.sample_rate,
                                   in_ch_layout,
                                   codec_ctx_->sample_fmt,
                                   codec_ctx_->sample_rate,
                                   0, nullptr);
    if (!swr_ctx_) {
        Debug::Log("AudioDecoder: Failed to allocate resampler");
        return false;
    }
#endif

    // Apply normalization to prevent hot output levels
    // Use -6dB (0.5) to match Premiere/MPV playback levels
    av_opt_set_double(swr_ctx_, "rematrix_volume", 0.5, 0);

    if (swr_init(swr_ctx_) < 0) {
        Debug::Log("AudioDecoder: Failed to initialize resampler");
        swr_free(&swr_ctx_);
        swr_ctx_ = nullptr;
        return false;
    }

    return true;
}

void AudioDecoder::CleanupResampler() {
    if (swr_ctx_) {
        swr_free(&swr_ctx_);
        swr_ctx_ = nullptr;
    }
}

void AudioDecoder::CloseAudioStream() {
    if (decode_frame_) {
        av_frame_free(&decode_frame_);
        decode_frame_ = nullptr;
    }

    if (packet_) {
        av_packet_free(&packet_);
        packet_ = nullptr;
    }

    CleanupResampler();

    if (codec_ctx_) {
        avcodec_free_context(&codec_ctx_);
        codec_ctx_ = nullptr;
    }

    if (format_ctx_) {
        avformat_close_input(&format_ctx_);
        format_ctx_ = nullptr;
    }

    audio_stream_idx_ = -1;
}

//=============================================================================
// Seeking
//=============================================================================

void AudioDecoder::Seek(double position) {
    if (!is_open_) return;

    Debug::Log("AudioDecoder: Seek requested to " + std::to_string(position) + "s");

    {
        std::lock_guard<std::mutex> lock(seek_mutex_);
        seek_target_ = position;
        seek_requested_ = true;
    }
    seek_cv_.notify_one();
}

void AudioDecoder::FlushAndSeek(double position) {
    if (!format_ctx_ || audio_stream_idx_ < 0) return;

    AVStream* audio_stream = format_ctx_->streams[audio_stream_idx_];

    // Clear the ring buffer
    ring_buffer_->Clear();

    // Flush codec
    avcodec_flush_buffers(codec_ctx_);

    // Convert position to stream timestamp
    int64_t target_ts = static_cast<int64_t>(position / av_q2d(audio_stream->time_base));

    // Add start time offset if present
    if (start_time_ != AV_NOPTS_VALUE && start_time_ > 0) {
        target_ts += start_time_;
    }

    // Seek backward to ensure we don't miss any data
    int ret = av_seek_frame(format_ctx_, audio_stream_idx_, target_ts, AVSEEK_FLAG_BACKWARD);
    if (ret < 0) {
        Debug::Log("AudioDecoder: Seek failed, trying alternative method");
        // Try seeking from start
        ret = av_seek_frame(format_ctx_, audio_stream_idx_, 0, AVSEEK_FLAG_BACKWARD);
    }

    // Reset EOF flag
    eof_reached_ = false;

    // Update positions
    decode_position_ = position;
    read_position_ = position;

    Debug::Log("AudioDecoder: Seeked to " + std::to_string(position) + "s");
}

//=============================================================================
// Decode Thread
//=============================================================================

void AudioDecoder::DecodeThread() {
    Debug::Log("AudioDecoder: Decode thread started");

    while (running_) {
        // Check for seek request
        {
            std::unique_lock<std::mutex> lock(seek_mutex_);
            if (seek_requested_) {
                FlushAndSeek(seek_target_);
                seek_requested_ = false;
            }
        }

        // If buffer is nearly full, wait
        if (ring_buffer_->AvailableWrite() < 8192) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        // If EOF reached, wait for seek
        if (eof_reached_) {
            std::unique_lock<std::mutex> lock(seek_mutex_);
            seek_cv_.wait_for(lock, std::chrono::milliseconds(50), [this] {
                return !running_ || seek_requested_;
            });
            continue;
        }

        // Decode next packet
        if (!DecodeNextPacket()) {
            // EOF or error
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    Debug::Log("AudioDecoder: Decode thread exiting");
}

bool AudioDecoder::DecodeNextPacket() {
    if (!format_ctx_ || !codec_ctx_) return false;

    // Read packet
    int ret = av_read_frame(format_ctx_, packet_);
    if (ret < 0) {
        if (ret == AVERROR_EOF) {
            eof_reached_ = true;
            Debug::Log("AudioDecoder: Reached end of file");
        }
        return false;
    }

    // Check if this packet is from our audio stream
    if (packet_->stream_index != audio_stream_idx_) {
        av_packet_unref(packet_);
        return true;  // Not an error, just not our stream
    }

    // Send packet to decoder
    ret = avcodec_send_packet(codec_ctx_, packet_);
    av_packet_unref(packet_);

    if (ret < 0) {
        return false;
    }

    // Receive decoded frames
    while (ret >= 0) {
        ret = avcodec_receive_frame(codec_ctx_, decode_frame_);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        }
        if (ret < 0) {
            return false;
        }

        // Calculate output sample count after resampling
        int64_t delay = swr_get_delay(swr_ctx_, codec_ctx_->sample_rate);
        int dst_nb_samples = static_cast<int>(av_rescale_rnd(
            delay + decode_frame_->nb_samples,
            output_format_.sample_rate,
            codec_ctx_->sample_rate,
            AV_ROUND_UP));

        // Allocate output buffer
        size_t output_size = dst_nb_samples * output_format_.channels * sizeof(float);
        std::vector<uint8_t> output_buffer(output_size);
        uint8_t* output_ptr = output_buffer.data();

        // Resample
        int samples_converted = swr_convert(swr_ctx_,
                                            &output_ptr, dst_nb_samples,
                                            (const uint8_t**)decode_frame_->data,
                                            decode_frame_->nb_samples);

        if (samples_converted > 0) {
            size_t bytes_to_write = samples_converted * output_format_.channels * sizeof(float);

            // Write to ring buffer - wait if buffer is full to avoid dropping samples
            // This is critical for WAV files which decode very fast
            size_t total_written = 0;
            const uint8_t* write_ptr = output_buffer.data();

            while (total_written < bytes_to_write && running_ && !seek_requested_) {
                size_t remaining = bytes_to_write - total_written;
                size_t written = ring_buffer_->Write(write_ptr + total_written, remaining);
                total_written += written;

                if (total_written < bytes_to_write) {
                    // Buffer full - wait a bit for consumer to drain some
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
            }

            // Update decode position
            if (decode_frame_->pts != AV_NOPTS_VALUE) {
                AVStream* stream = format_ctx_->streams[audio_stream_idx_];
                double pts = static_cast<double>(decode_frame_->pts - start_time_) * av_q2d(stream->time_base);
                decode_position_ = pts;
            } else {
                // Estimate based on samples decoded
                decode_position_ = decode_position_.load() +
                    static_cast<double>(samples_converted) / output_format_.sample_rate;
            }
        }

        av_frame_unref(decode_frame_);
    }

    return true;
}

//=============================================================================
// PCM Data Access
//=============================================================================

size_t AudioDecoder::Read(float* output, size_t frame_count) {
    if (!ring_buffer_ || !output) return 0;

    size_t bytes_requested = frame_count * output_format_.BytesPerFrame();
    size_t bytes_read = ring_buffer_->Read(output, bytes_requested);
    size_t frames_read = bytes_read / output_format_.BytesPerFrame();

    // Fill remainder with silence if buffer underrun
    if (frames_read < frame_count) {
        size_t silence_start = frames_read * output_format_.channels;
        size_t silence_count = (frame_count - frames_read) * output_format_.channels;
        std::memset(output + silence_start, 0, silence_count * sizeof(float));
    }

    // Update read position
    read_position_ = read_position_.load() +
        static_cast<double>(frames_read) / output_format_.sample_rate;

    return frames_read;
}

double AudioDecoder::GetBufferedDuration() const {
    if (!ring_buffer_) return 0.0;
    return output_format_.BytesToSeconds(ring_buffer_->AvailableRead());
}

bool AudioDecoder::HasData() const {
    if (!ring_buffer_) return false;
    // Consider "has data" if we have at least 10ms worth
    return ring_buffer_->AvailableRead() >= output_format_.SecondsToBytes(0.01);
}

} // namespace ump
