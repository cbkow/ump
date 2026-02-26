#pragma once

#include <string>
#include <memory>
#include <atomic>
#include <chrono>
#include <thread>
#include <mutex>
#include <condition_variable>

#include "ring_buffer.h"

// Forward declarations for FFmpeg types
struct AVFormatContext;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwrContext;

namespace qcview {

/**
 * Audio format info
 * Output format is always float32 stereo 48kHz for miniaudio compatibility
 */
struct AudioFormat {
    int sample_rate = 48000;
    int channels = 2;
    int bytes_per_sample = 4;  // float32

    size_t BytesPerFrame() const { return channels * bytes_per_sample; }

    double BytesToSeconds(size_t bytes) const {
        return static_cast<double>(bytes) / (sample_rate * BytesPerFrame());
    }

    size_t SecondsToBytes(double seconds) const {
        return static_cast<size_t>(seconds * sample_rate * BytesPerFrame());
    }

    size_t SecondsToFrames(double seconds) const {
        return static_cast<size_t>(seconds * sample_rate);
    }

    double FramesToSeconds(size_t frames) const {
        return static_cast<double>(frames) / sample_rate;
    }
};

/**
 * AudioDecoder configuration
 */
struct AudioDecoderConfig {
    double buffer_ahead_seconds = 2.0;   // How far ahead to decode
    int output_sample_rate = 48000;      // Output sample rate
    int output_channels = 2;             // Output channels (stereo)
};

/**
 * AudioDecoder - FFmpeg-based audio decoder
 *
 * Decodes audio from video containers in a background thread.
 * Outputs float32 stereo PCM to a ring buffer for miniaudio consumption.
 *
 * Usage:
 *   AudioDecoder decoder;
 *   decoder.Open("video.mp4");
 *   decoder.Start();
 *   decoder.Seek(10.0);  // Seek to 10 seconds
 *
 *   // In audio callback:
 *   decoder.Read(buffer, frame_count);
 */
class AudioDecoder {
public:
    AudioDecoder();
    ~AudioDecoder();

    // Prevent copying
    AudioDecoder(const AudioDecoder&) = delete;
    AudioDecoder& operator=(const AudioDecoder&) = delete;

    //=========================================================================
    // Lifecycle
    //=========================================================================

    // Open audio from video file
    // Returns true if audio stream found and initialized
    bool Open(const std::string& file_path);

    // Close and release resources
    void Close();

    // Start decode thread (call after Open)
    void Start();

    // Stop decode thread
    void Stop();

    // Check state
    bool IsOpen() const { return is_open_; }
    bool IsRunning() const { return running_.load(); }
    bool HasAudio() const { return has_audio_; }

    //=========================================================================
    // Seeking and Sync
    //=========================================================================

    // Seek to position (seconds from source start)
    // Clears buffer and seeks in decode thread
    void Seek(double position);

    // Get current decode position (seconds)
    double GetDecodePosition() const { return decode_position_.load(); }

    // Get current read position (where we're consuming from)
    double GetReadPosition() const { return read_position_.load(); }

    //=========================================================================
    // PCM Data Access (called from audio callback thread)
    //=========================================================================

    // Read PCM samples from buffer
    // Returns number of frames actually read
    // Fills with silence if buffer underrun
    size_t Read(float* output, size_t frame_count);

    // Get buffered audio duration (seconds)
    double GetBufferedDuration() const;

    // Check if buffer has enough data
    bool HasData() const;

    //=========================================================================
    // Source Info
    //=========================================================================

    const AudioFormat& GetFormat() const { return output_format_; }
    double GetDuration() const { return duration_; }
    int GetSourceSampleRate() const { return source_sample_rate_; }
    int GetSourceChannels() const { return source_channels_; }
    const std::string& GetPath() const { return file_path_; }

    //=========================================================================
    // Configuration
    //=========================================================================

    void SetConfig(const AudioDecoderConfig& config) { config_ = config; }
    const AudioDecoderConfig& GetConfig() const { return config_; }

private:
    //=========================================================================
    // Decode Thread
    //=========================================================================

    void DecodeThread();
    bool DecodeNextPacket();
    void FlushAndSeek(double position);

    //=========================================================================
    // FFmpeg Setup
    //=========================================================================

    bool OpenAudioStream();
    void CloseAudioStream();
    bool SetupResampler();
    void CleanupResampler();

    //=========================================================================
    // State
    //=========================================================================

    bool is_open_ = false;
    bool has_audio_ = false;
    AudioDecoderConfig config_;
    AudioFormat output_format_;
    std::string file_path_;

    // Source info
    int source_sample_rate_ = 0;
    int source_channels_ = 0;
    double duration_ = 0.0;
    int64_t start_time_ = 0;  // Stream start time in timebase units

    // FFmpeg contexts
    AVFormatContext* format_ctx_ = nullptr;
    AVCodecContext* codec_ctx_ = nullptr;
    SwrContext* swr_ctx_ = nullptr;
    AVFrame* decode_frame_ = nullptr;
    AVPacket* packet_ = nullptr;
    int audio_stream_idx_ = -1;

    // Ring buffer for PCM output
    std::unique_ptr<AudioRingBuffer> ring_buffer_;

    // Threading
    std::thread decode_thread_;
    std::atomic<bool> running_{false};
    std::atomic<double> decode_position_{0.0};
    std::atomic<double> read_position_{0.0};

    // Seek coordination
    std::mutex seek_mutex_;
    std::condition_variable seek_cv_;
    std::atomic<bool> seek_requested_{false};
    double seek_target_ = 0.0;

    // EOF tracking
    std::atomic<bool> eof_reached_{false};

    // Seek timing - tracks when last seek completed for cooldown logic
    std::atomic<double> last_seek_time_{0.0};

    // Reusable decode output buffer (avoids per-frame heap allocation)
    std::vector<uint8_t> resample_buffer_;

public:
    // Check if decoder has reached end of file
    bool IsEOF() const { return eof_reached_.load(); }

    // Get seconds since last seek (for cooldown logic in sync correction)
    double SecondsSinceLastSeek() const {
        double now = std::chrono::duration<double>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        double last = last_seek_time_.load();
        return (last > 0.0) ? (now - last) : 999.0;
    }
};

} // namespace qcview
