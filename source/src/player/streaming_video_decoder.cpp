#include "streaming_video_decoder.h"
#include "../utils/debug_utils.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/hwcontext.h>
}

// Thread-local storage for hw pixel format (needed by get_format callback)
static thread_local AVPixelFormat g_hw_pix_fmt = AV_PIX_FMT_NONE;

// Forward declaration of hardware format callback (defined later in file)
static AVPixelFormat GetHWFormatCallback(AVCodecContext* ctx, const AVPixelFormat* pix_fmts);

#include <cmath>
#include <algorithm>

namespace ump {

//=============================================================================
// Constructor / Destructor
//=============================================================================

StreamingVideoDecoder::StreamingVideoDecoder(const std::string& video_path)
    : video_path_(video_path) {
}

StreamingVideoDecoder::~StreamingVideoDecoder() {
    Shutdown();
}

//=============================================================================
// Lifecycle
//=============================================================================

bool StreamingVideoDecoder::Initialize() {
    if (initialized_) {
        return true;
    }

    Debug::Log("StreamingVideoDecoder: Initializing for " + video_path_);

    // Open video file
    if (!OpenVideo()) {
        Debug::Log("StreamingVideoDecoder: Failed to open video");
        return false;
    }

    // Pre-allocate ring buffer
    int buffer_size = config_.readAheadFrames + config_.readBehindFrames;
    ring_buffer_.clear();

    Debug::Log("StreamingVideoDecoder: Video opened - " +
               std::to_string(width_) + "x" + std::to_string(height_) +
               " @ " + std::to_string(fps_) + " fps, " +
               std::to_string(frame_count_) + " frames, buffer size " +
               std::to_string(buffer_size) + ", decode: " +
               std::string(HWAccelTypeToString(hw_accel_type_)));

    // Start decode thread
    running_ = true;
    decode_thread_ = std::thread(&StreamingVideoDecoder::DecodeThread, this);

    initialized_ = true;
    Debug::Log("StreamingVideoDecoder: Initialized successfully");
    return true;
}

void StreamingVideoDecoder::Shutdown() {
    if (!initialized_) {
        return;
    }

    Debug::Log("StreamingVideoDecoder: Shutting down...");

    // Stop decode thread
    running_ = false;
    seek_cv_.notify_all();
    decode_cv_.notify_all();

    if (decode_thread_.joinable()) {
        decode_thread_.join();
    }

    // Clear buffer
    ClearBuffer();

    // Close video
    CloseVideo();

    initialized_ = false;
    Debug::Log("StreamingVideoDecoder: Shutdown complete");
}

void StreamingVideoDecoder::HardReset(int target_frame) {
    Debug::Log("StreamingVideoDecoder: HardReset to frame " + std::to_string(target_frame));

    // Stop decode thread
    running_ = false;
    seek_cv_.notify_all();
    decode_cv_.notify_all();

    if (decode_thread_.joinable()) {
        decode_thread_.join();
    }

    // Clear buffer
    ClearBuffer();

    // Reset EOF flag
    eof_reached_ = false;

    // Close and reopen video (this resets all FFmpeg state)
    CloseVideo();

    if (!OpenVideo()) {
        Debug::Log("StreamingVideoDecoder: HardReset failed to reopen video");
        initialized_ = false;
        return;
    }

    // Restart decode thread
    running_ = true;
    decode_thread_ = std::thread(&StreamingVideoDecoder::DecodeThread, this);

    // Set target position with NORMAL quality
    playhead_frame_ = target_frame;
    decode_frame_ = target_frame;

    // Request seek to target
    {
        std::lock_guard<std::mutex> lock(seek_mutex_);
        seek_target_frame_ = target_frame;
        seek_quality_ = SeekQuality::NORMAL;
        seek_requested_ = true;
    }
    seek_cv_.notify_one();

    Debug::Log("StreamingVideoDecoder: HardReset complete");
}

//=============================================================================
// FFmpeg Initialization
//=============================================================================

bool StreamingVideoDecoder::OpenVideo() {
    // Open input file
    format_ctx_ = avformat_alloc_context();
    if (avformat_open_input(&format_ctx_, video_path_.c_str(), nullptr, nullptr) < 0) {
        Debug::Log("StreamingVideoDecoder: Failed to open input: " + video_path_);
        return false;
    }

    // Find stream info
    if (avformat_find_stream_info(format_ctx_, nullptr) < 0) {
        Debug::Log("StreamingVideoDecoder: Failed to find stream info");
        avformat_close_input(&format_ctx_);
        return false;
    }

    // Find video stream
    video_stream_idx_ = av_find_best_stream(format_ctx_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (video_stream_idx_ < 0) {
        Debug::Log("StreamingVideoDecoder: No video stream found");
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
        fps_ = 24.0;  // Fallback
    }

    // Get duration
    if (format_ctx_->duration > 0) {
        duration_ = static_cast<double>(format_ctx_->duration) / AV_TIME_BASE;
    } else if (video_stream->duration > 0) {
        duration_ = static_cast<double>(video_stream->duration) * av_q2d(video_stream->time_base);
    } else {
        duration_ = 0.0;
    }

    frame_count_ = static_cast<int>(duration_ * fps_);

    // Ensure at least 1 frame for single-frame videos (MXF subclips, etc.)
    // FFmpeg may return duration that calculates to 0 frames, but if we
    // successfully opened the file, there's at least one frame to decode
    if (frame_count_ <= 0 && duration_ > 0) {
        frame_count_ = 1;
        Debug::Log("StreamingVideoDecoder: Adjusted frame_count from 0 to 1 (single-frame video)");
    }

    // Get start time offset (for HEVC and other formats with non-zero start)
    if (video_stream->start_time != AV_NOPTS_VALUE) {
        start_time_ = av_rescale_q(video_stream->start_time,
                                   video_stream->time_base,
                                   AV_TIME_BASE_Q);
    }

    // Find decoder
    const AVCodec* codec = avcodec_find_decoder(video_stream->codecpar->codec_id);
    if (!codec) {
        Debug::Log("StreamingVideoDecoder: No decoder found for codec");
        avformat_close_input(&format_ctx_);
        return false;
    }

    Debug::Log("StreamingVideoDecoder: Found decoder: " + std::string(codec->name));

    // Try hardware acceleration if enabled
    bool hw_success = false;
    if (config_.useHardwareAccel) {
        // Priority order: CUDA (NVDEC) > D3D11VA > QSV > DXVA2
        // CUDA is NVIDIA-specific and generally fastest
        // D3D11VA works on NVIDIA/Intel/AMD on Windows 8+
        // QSV is Intel-specific
        // DXVA2 is legacy fallback

        struct HWAccelOption {
            int av_type;  // AVHWDeviceType as int
            HWAccelType our_type;
            const char* name;
        };

        HWAccelOption hw_options[] = {
            { static_cast<int>(AV_HWDEVICE_TYPE_CUDA), HWAccelType::CUDA, "CUDA (NVDEC)" },
            { static_cast<int>(AV_HWDEVICE_TYPE_D3D11VA), HWAccelType::D3D11VA, "D3D11VA" },
            { static_cast<int>(AV_HWDEVICE_TYPE_QSV), HWAccelType::QSV, "Quick Sync" },
            { static_cast<int>(AV_HWDEVICE_TYPE_DXVA2), HWAccelType::DXVA2, "DXVA2" },
        };

        for (const auto& opt : hw_options) {
            Debug::Log("StreamingVideoDecoder: Trying " + std::string(opt.name) + "...");

            if (TryHardwareAccel(codec, opt.av_type)) {
                hw_accel_type_ = opt.our_type;
                hw_success = true;
                Debug::Log("StreamingVideoDecoder: Hardware acceleration enabled: " + std::string(opt.name));
                break;
            }
        }

        if (!hw_success) {
            Debug::Log("StreamingVideoDecoder: No hardware acceleration available, using software decode");
        }
    }

    // Allocate codec context
    codec_ctx_ = avcodec_alloc_context3(codec);
    if (!codec_ctx_) {
        Debug::Log("StreamingVideoDecoder: Failed to allocate codec context");
        if (hw_device_ctx_) av_buffer_unref(&hw_device_ctx_);
        avformat_close_input(&format_ctx_);
        return false;
    }

    // Copy codec parameters
    if (avcodec_parameters_to_context(codec_ctx_, video_stream->codecpar) < 0) {
        Debug::Log("StreamingVideoDecoder: Failed to copy codec parameters");
        avcodec_free_context(&codec_ctx_);
        if (hw_device_ctx_) av_buffer_unref(&hw_device_ctx_);
        avformat_close_input(&format_ctx_);
        return false;
    }

    // Configure for hardware or software decode
    if (hw_success && hw_device_ctx_) {
        // Hardware decode configuration
        codec_ctx_->hw_device_ctx = av_buffer_ref(hw_device_ctx_);
        codec_ctx_->get_format = GetHWFormatCallback;

        Debug::Log("StreamingVideoDecoder: Configured hardware decode with " +
                   std::string(HWAccelTypeToString(hw_accel_type_)));
    } else {
        // Software decode configuration
        hw_accel_type_ = HWAccelType::NONE;

        // Set thread count for parallel decode - use more threads for 4K
        int thread_count = config_.decodeThreads;
        if (width_ >= 3840) {
            thread_count = std::max(thread_count, 8);  // More threads for 4K+
        }
        codec_ctx_->thread_count = thread_count;
        codec_ctx_->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;

        Debug::Log("StreamingVideoDecoder: Using software decode with " +
                   std::to_string(thread_count) + " threads");
    }

    // Open codec
    if (avcodec_open2(codec_ctx_, codec, nullptr) < 0) {
        Debug::Log("StreamingVideoDecoder: Failed to open codec");
        avcodec_free_context(&codec_ctx_);
        if (hw_device_ctx_) av_buffer_unref(&hw_device_ctx_);
        avformat_close_input(&format_ctx_);
        return false;
    }

    return true;
}

void StreamingVideoDecoder::CloseVideo() {
    // Free sws context
    if (sws_ctx_) {
        sws_freeContext(sws_ctx_);
        sws_ctx_ = nullptr;
    }

    // Free codec context
    if (codec_ctx_) {
        avcodec_free_context(&codec_ctx_);
        codec_ctx_ = nullptr;
    }

    // Free hardware device context
    if (hw_device_ctx_) {
        av_buffer_unref(&hw_device_ctx_);
        hw_device_ctx_ = nullptr;
    }
    hw_accel_type_ = HWAccelType::NONE;
    hw_pix_fmt_ = AV_PIX_FMT_NONE;

    // Close format context
    if (format_ctx_) {
        avformat_close_input(&format_ctx_);
        format_ctx_ = nullptr;
    }

    video_stream_idx_ = -1;
}

} // namespace ump (temporarily close for static callback)

//=============================================================================
// Hardware Acceleration - Static Callback (outside namespace)
//=============================================================================

// Static callback for hardware pixel format negotiation
// Must be outside namespace for FFmpeg callback compatibility
static AVPixelFormat GetHWFormatCallback(AVCodecContext* ctx, const AVPixelFormat* pix_fmts) {
    // This callback is called by FFmpeg to negotiate pixel format
    // We return the hardware format we configured (stored in thread-local)
    for (const AVPixelFormat* p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
        if (*p == g_hw_pix_fmt) {
            return *p;
        }
    }
    // Fallback to software if hardware format not available
    Debug::Log("StreamingVideoDecoder: Hardware format not available, falling back to software");
    return pix_fmts[0];
}

namespace ump {

//=============================================================================
// Hardware Acceleration - Member Functions
//=============================================================================

bool StreamingVideoDecoder::TryHardwareAccel(const AVCodec* codec, int hw_type_int) {
    AVHWDeviceType hw_type = static_cast<AVHWDeviceType>(hw_type_int);
    // Check if this codec supports this hardware acceleration
    for (int i = 0;; i++) {
        const AVCodecHWConfig* config = avcodec_get_hw_config(codec, i);
        if (!config) {
            break;  // No more hw configs
        }

        if (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX &&
            config->device_type == hw_type) {

            // Found a matching hw config - try to create device context
            int ret = av_hwdevice_ctx_create(&hw_device_ctx_, hw_type, nullptr, nullptr, 0);
            if (ret < 0) {
                char errbuf[256];
                av_strerror(ret, errbuf, sizeof(errbuf));
                Debug::Log("StreamingVideoDecoder: Failed to create " +
                           std::string(av_hwdevice_get_type_name(hw_type)) +
                           " device: " + errbuf);
                return false;
            }

            // Store the hardware pixel format
            hw_pix_fmt_ = static_cast<int>(config->pix_fmt);
            g_hw_pix_fmt = static_cast<AVPixelFormat>(hw_pix_fmt_);  // Set thread-local for callback

            Debug::Log("StreamingVideoDecoder: Created " +
                       std::string(av_hwdevice_get_type_name(hw_type)) +
                       " device context, hw_pix_fmt=" + std::to_string(hw_pix_fmt_));
            return true;
        }
    }

    Debug::Log("StreamingVideoDecoder: Codec doesn't support " +
               std::string(av_hwdevice_get_type_name(hw_type)));
    return false;
}

bool StreamingVideoDecoder::TransferHWFrame(AVFrame* hw_frame, AVFrame* sw_frame) {
    if (!hw_frame || !sw_frame) return false;

    // Transfer data from GPU to CPU
    int ret = av_hwframe_transfer_data(sw_frame, hw_frame, 0);
    if (ret < 0) {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        Debug::Log("StreamingVideoDecoder: Failed to transfer hw frame: " + std::string(errbuf));
        return false;
    }

    // Copy frame properties
    sw_frame->pts = hw_frame->pts;
    sw_frame->pkt_dts = hw_frame->pkt_dts;

    return true;
}

//=============================================================================
// Frame Access
//=============================================================================

std::shared_ptr<PixelData> StreamingVideoDecoder::GetFrame(int frame_number) {
    if (!initialized_) return nullptr;

    // Try to find in buffer
    auto pixels = FindInBuffer(frame_number);
    if (pixels) {
        return pixels;
    }

    // DEBUG: Log buffer state when frame not found (rate-limited)
    // Use instance counter (not static) to track per-decoder misses
    miss_log_count_++;
    if (miss_log_count_ <= 10 || miss_log_count_ % 30 == 0) {
        int buf_start = -1, buf_end = -1;
        GetBufferedRange(buf_start, buf_end);
        // Extract just filename for readability
        std::string filename = video_path_;
        size_t pos = filename.find_last_of("/\\");
        if (pos != std::string::npos) filename = filename.substr(pos + 1);
       /* Debug::Log("StreamingVideoDecoder::GetFrame MISS [" + filename + "]: frame " + std::to_string(frame_number) +
                   ", buffer=[" + std::to_string(buf_start) + "," + std::to_string(buf_end) + "]" +
                   ", playhead=" + std::to_string(playhead_frame_.load()));*/
    }

    // Frame not in buffer - will be decoded soon (or need seek)
    return nullptr;
}

std::shared_ptr<PixelData> StreamingVideoDecoder::GetClosestFrame(int frame_number, int* actual_frame) {
    if (!initialized_) return nullptr;

    std::lock_guard<std::mutex> lock(buffer_mutex_);

    if (ring_buffer_.empty()) {
        // DEBUG: Log when buffer is empty (with filename)
        static int empty_count = 0;
        if (++empty_count <= 5 || empty_count % 30 == 0) {
            std::string filename = video_path_;
            size_t pos = filename.find_last_of("/\\");
            if (pos != std::string::npos) filename = filename.substr(pos + 1);
            Debug::Log("StreamingVideoDecoder::GetClosestFrame [" + filename + "]: buffer EMPTY, requested frame " +
                       std::to_string(frame_number));
        }
        return nullptr;
    }

    // Fast path: check for exact match first (most common case during playback)
    // Also check adjacent frames for near-exact matches (common during light load)
    for (const auto& bf : ring_buffer_) {
        if (bf.frame_number == frame_number) {
            if (actual_frame) *actual_frame = frame_number;
            return bf.pixels;  // Exact match - return immediately
        }
    }

    // Full scan: find the closest frame in buffer
    int closest_frame = -1;
    int closest_distance = INT_MAX;
    std::shared_ptr<PixelData> closest_pixels = nullptr;

    for (const auto& bf : ring_buffer_) {
        int distance = std::abs(bf.frame_number - frame_number);
        if (distance < closest_distance) {
            closest_distance = distance;
            closest_frame = bf.frame_number;
            closest_pixels = bf.pixels;
        }
    }

    if (actual_frame) {
        *actual_frame = closest_frame;
    }

    return closest_pixels;
}

bool StreamingVideoDecoder::HasFrame(int frame_number) const {
    if (!initialized_) return false;

    std::lock_guard<std::mutex> lock(buffer_mutex_);
    for (const auto& bf : ring_buffer_) {
        if (bf.frame_number == frame_number) {
            return true;
        }
    }
    return false;
}

void StreamingVideoDecoder::UpdatePlayhead(int frame_number, SeekQuality quality, bool force_seek) {
    // Safety checks - must be initialized and running
    if (!initialized_ || !running_.load()) return;

    // Clamp to valid range
    frame_number = std::max(0, std::min(frame_number, frame_count_ - 1));

    int old_playhead = playhead_frame_.load();
    playhead_frame_ = frame_number;

    // Adjust ahead count when playhead moves forward
    // (frames that were ahead may now be at/behind playhead)
    // Use atomic fetch_sub with clamping to avoid race condition with AddToBuffer
    if (frame_number > old_playhead) {
        int frames_passed = frame_number - old_playhead;
        // Atomic decrement with floor at 0
        int old_value = buffer_ahead_count_.fetch_sub(frames_passed);
        if (old_value < frames_passed) {
            // Clamped to 0 - restore to 0 if we went negative
            buffer_ahead_count_.store(0);
        }
    }

    // PROACTIVE EVICTION: Always evict frames outside window on every playhead update
    // This prevents memory accumulation during rapid scrubbing
    // Previous threshold (movement > readBehindFrames) was too conservative
    {
        int window_start = frame_number - config_.readBehindFrames;
        int window_end = frame_number + config_.readAheadFrames;
        int evicted = 0;

        std::lock_guard<std::mutex> lock(buffer_mutex_);

        // Evict frames too far BEHIND (from front)
        while (!ring_buffer_.empty()) {
            int front_frame = ring_buffer_.front().frame_number;
            if (front_frame < window_start) {
                buffer_frame_set_.erase(front_frame);
                ring_buffer_.pop_front();
                evicted++;
            } else {
                break;
            }
        }

        // Evict frames too far AHEAD (from back) - handles backward seeks
        while (!ring_buffer_.empty()) {
            int back_frame = ring_buffer_.back().frame_number;
            if (back_frame > window_end) {
                buffer_frame_set_.erase(back_frame);
                ring_buffer_.pop_back();
                evicted++;
            } else {
                break;
            }
        }

        buffer_size_ = static_cast<int>(ring_buffer_.size());

        // Recalculate ahead count after eviction
        int ahead = 0;
        for (const auto& bf : ring_buffer_) {
            if (bf.frame_number >= frame_number) ahead++;
        }
        buffer_ahead_count_ = ahead;
    }

    // FORCE SEEK: Skip all tolerance checks and seek immediately
    // Used when entering REFINING state to ensure exact frame decode
    if (force_seek) {
        std::lock_guard<std::mutex> lock(seek_mutex_);
        Debug::Log("StreamingVideoDecoder: FORCE seek to frame " + std::to_string(frame_number));
        seek_target_frame_ = frame_number;
        seek_quality_ = quality;
        seek_requested_ = true;
        seek_cv_.notify_one();
        decode_cv_.notify_one();
        return;
    }

    // Check if we need to seek
    int buffer_start = -1, buffer_end = -1;
    GetBufferedRange(buffer_start, buffer_end);

    // Get current decode position
    int current_decode = decode_frame_;

    bool in_buffer = (buffer_start >= 0 && frame_number >= buffer_start && frame_number <= buffer_end);

    // More generous tolerance - wait for decode if we're within reasonable range
    bool close_ahead = (buffer_end >= 0 && frame_number > buffer_end &&
                        frame_number - buffer_end < config_.readAheadFrames / 2);  // Half buffer

    // Also consider "close to decode position" - decoder is working towards this frame
    bool close_to_decode = (frame_number >= current_decode &&
                            frame_number - current_decode < config_.readAheadFrames);  // Full buffer tolerance

    // Only request seek if:
    // 1. Frame is not in buffer AND not close ahead AND not close to decode position
    // 2. AND we're not already seeking to this frame (or very close)
    // 3. AND it's a significant jump (not just minor position updates)
    bool need_seek = !in_buffer && !close_ahead && !close_to_decode;

    if (need_seek) {
        std::lock_guard<std::mutex> lock(seek_mutex_);

        auto now = std::chrono::steady_clock::now();
        int current_target = seek_target_frame_;
        bool already_seeking = seek_requested_.load();

        if (quality == SeekQuality::PREVIEW) {
            // During PREVIEW (scrubbing), rate-limit seeks to ~5/second (200ms interval)
            // This gives visual feedback without thrashing
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - last_preview_seek_time_).count();

            // Always update target so refinement knows where to go
            seek_target_frame_ = frame_number;
            seek_quality_ = quality;

            // Only trigger seek if enough time has passed AND not already seeking
            constexpr int kPreviewSeekIntervalMs = 200;  // Max 5 seeks/second during scrubbing
            bool enough_time = elapsed >= kPreviewSeekIntervalMs;
            bool target_far_enough = std::abs(frame_number - current_target) > 48;  // ~2 seconds

            // IMPORTANT: If frame is BEFORE buffer start, we MUST seek - no tolerance allowed
            bool must_seek_backward = (buffer_start >= 0 && frame_number < buffer_start);

            if ((enough_time && !already_seeking && target_far_enough) || must_seek_backward) {
                seek_requested_ = true;
                last_preview_seek_time_ = now;
                seek_cv_.notify_one();
            }
        } else {
            // NORMAL quality - trigger seek immediately (used after scrubbing stops)
            bool target_changed = std::abs(frame_number - current_target) > 24;  // ~1 second tolerance

            // IMPORTANT: If frame is BEFORE buffer start, we MUST seek - no tolerance allowed
            // This handles the case where decoder is at EOF but playhead is way back
            bool must_seek_backward = (buffer_start >= 0 && frame_number < buffer_start);

            if (!already_seeking || target_changed || must_seek_backward) {
                Debug::Log("StreamingVideoDecoder: Seek to frame " + std::to_string(frame_number) +
                           (must_seek_backward ? " (backward seek)" : ""));

                seek_target_frame_ = frame_number;
                seek_quality_ = quality;
                seek_requested_ = true;
                seek_cv_.notify_one();
            }
        }
    }

    // Wake decode thread if it was waiting
    decode_cv_.notify_one();
}

//=============================================================================
// Buffer Status
//=============================================================================

int StreamingVideoDecoder::GetBufferedAhead() const {
    if (!initialized_) return 0;

    int playhead = playhead_frame_.load();
    int count = 0;

    std::lock_guard<std::mutex> lock(buffer_mutex_);
    for (const auto& bf : ring_buffer_) {
        if (bf.frame_number > playhead) {
            count++;
        }
    }
    return count;
}

int StreamingVideoDecoder::GetBufferedBehind() const {
    if (!initialized_) return 0;

    int playhead = playhead_frame_.load();
    int count = 0;

    std::lock_guard<std::mutex> lock(buffer_mutex_);
    for (const auto& bf : ring_buffer_) {
        if (bf.frame_number < playhead) {
            count++;
        }
    }
    return count;
}

int StreamingVideoDecoder::GetBufferSize() const {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    return static_cast<int>(ring_buffer_.size());
}

void StreamingVideoDecoder::GetBufferedRange(int& start_frame, int& end_frame) const {
    std::lock_guard<std::mutex> lock(buffer_mutex_);

    if (ring_buffer_.empty()) {
        start_frame = -1;
        end_frame = -1;
        return;
    }

    start_frame = ring_buffer_.front().frame_number;
    end_frame = ring_buffer_.back().frame_number;
}

//=============================================================================
// Configuration
//=============================================================================

void StreamingVideoDecoder::SetConfig(const StreamingDecoderConfig& config) {
    config_ = config;
}

//=============================================================================
// Decode Thread
//=============================================================================

void StreamingVideoDecoder::DecodeThread() {
    Debug::Log("StreamingVideoDecoder: Decode thread started");

    AVFrame* frame = av_frame_alloc();
    AVPacket* packet = av_packet_alloc();

    if (!frame || !packet) {
        Debug::Log("StreamingVideoDecoder: Failed to allocate frame/packet");
        if (frame) av_frame_free(&frame);
        if (packet) av_packet_free(&packet);
        return;
    }

    decode_frame_ = 0;
    int frames_decoded = 0;

    while (running_) {
        // Check for seek request
        if (seek_requested_.load()) {
            int target;
            SeekQuality quality;
            {
                std::lock_guard<std::mutex> lock(seek_mutex_);
                target = seek_target_frame_;
                quality = seek_quality_;
                seek_requested_ = false;
            }
            FlushAndSeek(target, quality);
            frames_decoded = 0;  // Reset counter after seek
        }

        // Check if buffer is full ahead
        if (!NeedsMoreFrames()) {
            // Wait for playhead to move or seek request (short timeout for responsiveness)
            std::unique_lock<std::mutex> lock(seek_mutex_);
            decode_cv_.wait_for(lock, std::chrono::milliseconds(2), [this] {
                return !running_ || seek_requested_.load() || NeedsMoreFrames();
            });
            continue;
        }

        // Decode next frame
        bool decoded = DecodeNextFrame(frame);
        if (decoded) {
            // Check for duplicate BEFORE expensive ConvertToPixelData
            // This prevents allocating ~33MB only to discard it
            {
                std::lock_guard<std::mutex> lock(buffer_mutex_);
                if (buffer_frame_set_.count(decode_frame_) > 0) {
                    // Frame already in buffer - skip conversion
                    decode_frame_++;
                    av_frame_unref(frame);
                    continue;
                }
            }

            // Convert to PixelData
            auto pixels = ConvertToPixelData(frame);
            if (pixels) {
                AddToBuffer(decode_frame_, pixels);
                frames_decoded++;

                // Log progress rarely to avoid I/O overhead during playback
                if (frames_decoded == 1 || frames_decoded % 100 == 0) {
                    Debug::Log("StreamingVideoDecoder: Decoded frame " +
                               std::to_string(decode_frame_) + " (" +
                               std::to_string(frames_decoded) + " total, ahead=" +
                               std::to_string(buffer_ahead_count_.load()) + ")");
                }
            }
            // Don't log every conversion failure - too noisy
            decode_frame_++;

            // Check for end of video
            if (decode_frame_ >= frame_count_) {
                // Reached end - set EOF flag to prevent spin loop
                if (!eof_reached_.load()) {
                    Debug::Log("StreamingVideoDecoder: Reached end of video at frame " +
                               std::to_string(decode_frame_));
                    eof_reached_ = true;
                }
            }
        } else {
            // DecodeNextFrame returned false - EOF or error
            // Set EOF flag to prevent spin loop trying to decode more frames
            if (!eof_reached_.load()) {
                Debug::Log("StreamingVideoDecoder: EOF/error at decode_frame " +
                           std::to_string(decode_frame_) + " (frame_count=" +
                           std::to_string(frame_count_) + ")");
                eof_reached_ = true;
            }
        }

        av_frame_unref(frame);
    }

    av_frame_free(&frame);
    av_packet_free(&packet);

    Debug::Log("StreamingVideoDecoder: Decode thread stopped");
}

bool StreamingVideoDecoder::DecodeNextFrame(AVFrame* frame) {
    if (!format_ctx_ || !codec_ctx_) return false;

    AVPacket* packet = av_packet_alloc();
    if (!packet) return false;

    bool got_frame = false;

    while (!got_frame && running_) {
        // Try to receive decoded frame first
        int ret = avcodec_receive_frame(codec_ctx_, frame);
        if (ret == 0) {
            got_frame = true;
            break;
        } else if (ret == AVERROR(EAGAIN)) {
            // Need more packets
            ret = av_read_frame(format_ctx_, packet);
            if (ret < 0) {
                if (ret == AVERROR_EOF) {
                    // Send flush packet
                    avcodec_send_packet(codec_ctx_, nullptr);
                } else {
                    av_packet_free(&packet);
                    return false;
                }
            } else if (packet->stream_index == video_stream_idx_) {
                ret = avcodec_send_packet(codec_ctx_, packet);
                if (ret < 0 && ret != AVERROR(EAGAIN)) {
                    av_packet_unref(packet);
                    av_packet_free(&packet);
                    return false;
                }
            }
            av_packet_unref(packet);
        } else if (ret == AVERROR_EOF) {
            // No more frames
            av_packet_free(&packet);
            return false;
        } else {
            // Error
            av_packet_free(&packet);
            return false;
        }
    }

    av_packet_free(&packet);
    return got_frame;
}

std::shared_ptr<PixelData> StreamingVideoDecoder::ConvertToPixelData(AVFrame* frame) {
    if (!frame || frame->width <= 0 || frame->height <= 0) {
        return nullptr;
    }

    // For hardware decode, we need to transfer the frame from GPU to CPU first
    AVFrame* sw_frame = nullptr;
    AVFrame* src_frame = frame;

    bool is_hw_frame = (frame->format == hw_pix_fmt_ && hw_pix_fmt_ != AV_PIX_FMT_NONE);

    if (is_hw_frame) {
        // Allocate a software frame for the transfer
        sw_frame = av_frame_alloc();
        if (!sw_frame) {
            Debug::Log("StreamingVideoDecoder: Failed to allocate sw_frame for hw transfer");
            return nullptr;
        }

        // Transfer from GPU to CPU
        if (!TransferHWFrame(frame, sw_frame)) {
            av_frame_free(&sw_frame);
            Debug::Log("StreamingVideoDecoder: Hardware frame transfer failed");
            return nullptr;
        }

        src_frame = sw_frame;
    }

    // Now src_frame is in CPU memory - check if we need to recreate sws context
    bool need_new_sws = (sws_ctx_ == nullptr ||
                         sws_src_width_ != src_frame->width ||
                         sws_src_height_ != src_frame->height ||
                         sws_src_format_ != src_frame->format);

    if (need_new_sws) {
        if (sws_ctx_) {
            sws_freeContext(sws_ctx_);
        }

        // Use fast bilinear for better performance (especially on 4K)
        sws_ctx_ = sws_getContext(
            src_frame->width, src_frame->height, static_cast<AVPixelFormat>(src_frame->format),
            src_frame->width, src_frame->height, AV_PIX_FMT_RGBA,
            SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);

        if (!sws_ctx_) {
            Debug::Log("StreamingVideoDecoder: Failed to create sws context for format " +
                       std::to_string(src_frame->format));
            if (sw_frame) av_frame_free(&sw_frame);
            return nullptr;
        }

        sws_src_width_ = src_frame->width;
        sws_src_height_ = src_frame->height;
        sws_src_format_ = src_frame->format;

        Debug::Log("StreamingVideoDecoder: Created sws context for " +
                   std::to_string(src_frame->width) + "x" + std::to_string(src_frame->height) +
                   " format=" + std::to_string(src_frame->format) +
                   (is_hw_frame ? " (hw accelerated)" : " (software)"));
    }

    // Allocate output buffer
    auto pixels = std::make_shared<PixelData>();
    pixels->width = src_frame->width;
    pixels->height = src_frame->height;
    pixels->gl_format = GL_RGBA;
    pixels->gl_type = GL_UNSIGNED_BYTE;
    pixels->pipeline_mode = PipelineMode::NORMAL;

    int row_size = src_frame->width * 4;  // RGBA
    pixels->pixels.resize(row_size * src_frame->height);

    // Set up destination pointers
    uint8_t* dst_data[1] = { pixels->pixels.data() };
    int dst_linesize[1] = { row_size };

    // Convert
    sws_scale(sws_ctx_,
              src_frame->data, src_frame->linesize,
              0, src_frame->height,
              dst_data, dst_linesize);

    // Clean up hardware transfer frame
    if (sw_frame) {
        av_frame_free(&sw_frame);
    }

    return pixels;
}

void StreamingVideoDecoder::FlushAndSeek(int target_frame, SeekQuality quality) {
    // Safety check - codec context must be valid
    if (!codec_ctx_ || !format_ctx_) {
        Debug::Log("StreamingVideoDecoder: FlushAndSeek called with invalid context, ignoring");
        return;
    }

    // Only log full quality seeks to reduce noise during scrubbing
    if (quality == SeekQuality::NORMAL) {
        Debug::Log("StreamingVideoDecoder: FlushAndSeek to frame " + std::to_string(target_frame));
    }

    // Clear buffer
    ClearBuffer();

    // Reset EOF flag - we're seeking to a new position so we can decode again
    eof_reached_ = false;

    // Flush codec
    avcodec_flush_buffers(codec_ctx_);

    // Apply skip modes based on quality
    // Note: skip_frame=AVDISCARD_NONREF skips B-frames; for I-frame only codecs (ProRes, DNxHD)
    // this has no effect but is harmless
    if (quality == SeekQuality::PREVIEW) {
        // Fast preview mode: skip B-frames and loop filter for faster decode
        codec_ctx_->skip_frame = AVDISCARD_NONREF;  // Skip B-frames (non-reference frames)
        codec_ctx_->skip_loop_filter = AVDISCARD_ALL;  // Skip deblocking filter
    } else {
        // Normal quality: full decode
        codec_ctx_->skip_frame = AVDISCARD_DEFAULT;
        codec_ctx_->skip_loop_filter = AVDISCARD_DEFAULT;
    }

    // Calculate timestamp
    double timestamp = static_cast<double>(target_frame) / fps_;

    // Add start time offset
    if (start_time_ > 0) {
        timestamp += static_cast<double>(start_time_) / AV_TIME_BASE;
    }

    // Convert to stream timebase
    AVStream* stream = format_ctx_->streams[video_stream_idx_];
    int64_t target_pts = av_rescale_q(
        static_cast<int64_t>(timestamp * AV_TIME_BASE),
        AV_TIME_BASE_Q,
        stream->time_base);

    // Seek flags - for preview mode with hardware decode, use AVSEEK_FLAG_ANY for faster seeking
    int seek_flags = AVSEEK_FLAG_BACKWARD;
    if (quality == SeekQuality::PREVIEW && hw_accel_type_ != HWAccelType::NONE) {
        seek_flags |= AVSEEK_FLAG_ANY;  // Can seek to non-keyframes with hardware decode
    }

    // Seek to target
    int ret = av_seek_frame(format_ctx_, video_stream_idx_, target_pts, seek_flags);
    if (ret < 0) {
        Debug::Log("StreamingVideoDecoder: Seek failed, error " + std::to_string(ret));
    }

    // Update decode position
    decode_frame_ = target_frame;

    // Decode forward to target frame
    AVFrame* frame = av_frame_alloc();
    if (frame) {
        // Decode frames until we reach target
        int decoded_count = 0;
        while (decode_frame_ < target_frame && decoded_count < 120 && running_) {
            if (DecodeNextFrame(frame)) {
                decoded_count++;
                av_frame_unref(frame);
            } else {
                break;
            }
        }
        av_frame_free(&frame);

        if (decoded_count > 0) {
            Debug::Log("StreamingVideoDecoder: Decoded " + std::to_string(decoded_count) +
                       " frames to reach target");
        }
    }

    decode_frame_ = target_frame;

    // DEBUG: Log final state after seek (with filename)
    int buf_start = -1, buf_end = -1;
    GetBufferedRange(buf_start, buf_end);
    std::string filename = video_path_;
    size_t pos = filename.find_last_of("/\\");
    if (pos != std::string::npos) filename = filename.substr(pos + 1);
    Debug::Log("StreamingVideoDecoder::FlushAndSeek COMPLETE [" + filename + "]: target=" + std::to_string(target_frame) +
               ", buffer=[" + std::to_string(buf_start) + "," + std::to_string(buf_end) + "]");
}

//=============================================================================
// Ring Buffer Management
//=============================================================================

void StreamingVideoDecoder::AddToBuffer(int frame_number, std::shared_ptr<PixelData> pixels) {
    int playhead = playhead_frame_.load();
    int evicted = 0;

    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);

        // CRITICAL: O(1) duplicate check using set
        // This prevents memory leak when same frame is decoded multiple times after seek
        if (buffer_frame_set_.count(frame_number) > 0) {
            // Frame already in buffer - skip adding duplicate
            return;
        }

        // Add to buffer and set
        BufferedFrame bf;
        bf.frame_number = frame_number;
        bf.pixels = std::move(pixels);
        ring_buffer_.push_back(std::move(bf));
        buffer_frame_set_.insert(frame_number);

        // Evict frames outside the valid window around playhead
        // Window is [playhead - readBehindFrames, playhead + readAheadFrames]
        int max_size = config_.readAheadFrames + config_.readBehindFrames;
        int window_start = playhead - config_.readBehindFrames;
        int window_end = playhead + config_.readAheadFrames;

        // First pass: evict frames too far BEHIND (from front)
        while (!ring_buffer_.empty()) {
            int front_frame = ring_buffer_.front().frame_number;
            if (front_frame < window_start) {
                buffer_frame_set_.erase(front_frame);
                ring_buffer_.pop_front();
                evicted++;
            } else {
                break;  // Front frame is in valid range
            }
        }

        // Second pass: evict frames too far AHEAD (from back)
        // This handles backward seeks where old frames are now past the window
        while (!ring_buffer_.empty()) {
            int back_frame = ring_buffer_.back().frame_number;
            if (back_frame > window_end) {
                buffer_frame_set_.erase(back_frame);
                ring_buffer_.pop_back();
                evicted++;
            } else {
                break;  // Back frame is in valid range
            }
        }

        // Safety: if still over max size, evict from front
        while (ring_buffer_.size() > static_cast<size_t>(max_size) && !ring_buffer_.empty()) {
            buffer_frame_set_.erase(ring_buffer_.front().frame_number);
            ring_buffer_.pop_front();
            evicted++;
        }

        // Update buffer size
        buffer_size_ = static_cast<int>(ring_buffer_.size());
    }

    // Update ahead count (frames >= playhead)
    // New frame is ahead if frame_number >= playhead
    // Use atomic fetch_add to avoid race condition with UpdatePlayhead
    if (frame_number >= playhead) {
        buffer_ahead_count_.fetch_add(1);
    }
}

std::shared_ptr<PixelData> StreamingVideoDecoder::FindInBuffer(int frame_number) const {
    std::lock_guard<std::mutex> lock(buffer_mutex_);

    for (const auto& bf : ring_buffer_) {
        if (bf.frame_number == frame_number) {
            return bf.pixels;
        }
    }
    return nullptr;
}

void StreamingVideoDecoder::ClearBuffer() {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    ring_buffer_.clear();
    buffer_frame_set_.clear();
    buffer_size_ = 0;
    buffer_ahead_count_ = 0;
}

bool StreamingVideoDecoder::NeedsMoreFrames() const {
    // Don't try to decode more frames if we've reached EOF
    if (eof_reached_.load()) {
        return false;
    }
    // O(1) check using atomic counter
    return buffer_ahead_count_.load() < config_.readAheadFrames;
}

} // namespace ump
