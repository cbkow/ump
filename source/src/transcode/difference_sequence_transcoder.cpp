#include "difference_sequence_transcoder.h"
#include "../metadata/video_metadata.h"
#include "../player/media_background_extractor.h"
#include "../utils/debug_utils.h"
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cmath>

// FFmpeg includes
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/hwcontext.h>
#include <libswscale/swscale.h>
#include <libavutil/opt.h>
}

// STB image write (implementation in video_player.cpp)
#include "../../external/glfw/deps/stb_image_write.h"

namespace ump {

// ============================================================================
// DecoderContext Implementation
// ============================================================================

DifferenceSequenceTranscoder::DecoderContext::~DecoderContext() {
    Cleanup();
}

void DifferenceSequenceTranscoder::DecoderContext::Cleanup() {
    if (sws_ctx) {
        sws_freeContext(sws_ctx);
        sws_ctx = nullptr;
    }
    if (codec_ctx) {
        avcodec_free_context(&codec_ctx);
    }
    if (format_ctx) {
        avformat_close_input(&format_ctx);
    }
    if (hw_device_ctx) {
        av_buffer_unref(&hw_device_ctx);
    }
    initialized = false;
}

bool DifferenceSequenceTranscoder::DecoderContext::Initialize(const std::string& video_path, HardwareDecodeMode hw_mode) {
    // Open video file
    if (avformat_open_input(&format_ctx, video_path.c_str(), nullptr, nullptr) < 0) {
        Debug::Log("ERROR: Failed to open video: " + video_path);
        return false;
    }

    if (avformat_find_stream_info(format_ctx, nullptr) < 0) {
        Debug::Log("ERROR: Failed to find stream info");
        return false;
    }

    // Find video stream
    video_stream_index = av_find_best_stream(format_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (video_stream_index < 0) {
        Debug::Log("ERROR: No video stream found");
        return false;
    }

    AVStream* video_stream = format_ctx->streams[video_stream_index];
    const AVCodec* codec = avcodec_find_decoder(video_stream->codecpar->codec_id);
    if (!codec) {
        Debug::Log("ERROR: Codec not found");
        return false;
    }

    codec_ctx = avcodec_alloc_context3(codec);
    if (!codec_ctx) {
        Debug::Log("ERROR: Failed to allocate codec context");
        return false;
    }

    if (avcodec_parameters_to_context(codec_ctx, video_stream->codecpar) < 0) {
        Debug::Log("ERROR: Failed to copy codec parameters");
        return false;
    }

    // Try hardware decode (optional, fallback to software)
    if (hw_mode != HardwareDecodeMode::SOFTWARE_ONLY) {
        // Hardware decode setup omitted for simplicity - can add later if needed
    }

    if (avcodec_open2(codec_ctx, codec, nullptr) < 0) {
        Debug::Log("ERROR: Failed to open codec");
        return false;
    }

    initialized = true;
    return true;
}

// ============================================================================
// Constructor / Destructor
// ============================================================================

DifferenceSequenceTranscoder::DifferenceSequenceTranscoder() {
    Debug::Log("DifferenceSequenceTranscoder: Constructor");
}

DifferenceSequenceTranscoder::~DifferenceSequenceTranscoder() {
    Debug::Log("DifferenceSequenceTranscoder: Destructor");
    Cleanup();
}

// ============================================================================
// Initialization
// ============================================================================

bool DifferenceSequenceTranscoder::Initialize(
    const std::string& primary_video_path,
    const VideoMetadata& primary_metadata,
    const std::string& comparison_video_path,
    const VideoMetadata& comparison_metadata,
    const TranscodeConfig& config)
{
    Debug::Log("DifferenceSequenceTranscoder: Initializing");
    Debug::Log("  Primary: " + primary_video_path);
    Debug::Log("  Comparison: " + comparison_video_path);

    config_ = config;
    primary_video_path_ = primary_video_path;
    comparison_video_path_ = comparison_video_path;
    primary_metadata_ = primary_metadata;
    comparison_metadata_ = comparison_metadata;

    // Calculate output dimensions
    output_width_ = (config_.max_width > 0 && primary_metadata_.width > config_.max_width)
                   ? config_.max_width
                   : primary_metadata_.width;
    output_height_ = (int)(output_width_ * ((double)primary_metadata_.height / primary_metadata_.width));

    frame_rate_ = primary_metadata_.frame_rate;
    total_frames_ = primary_metadata_.total_frames;
    duration_ = (frame_rate_ > 0) ? (total_frames_ / frame_rate_) : 0.0;

    // Setup cache directory
    SetupCacheDirectory();

    // Check if already cached
    if (!config_.force_retranscode && IsCached()) {
        Debug::Log("DifferenceSequenceTranscoder: Already cached");
        complete_ = true;
        initialized_ = true;
        return true;
    }

    // Initialize both decoders
    if (!primary_decoder_.Initialize(primary_video_path_, config_.hw_decode)) {
        Debug::Log("ERROR: Failed to initialize primary decoder");
        return false;
    }

    if (!comparison_decoder_.Initialize(comparison_video_path_, config_.hw_decode)) {
        Debug::Log("ERROR: Failed to initialize comparison decoder");
        return false;
    }

    initialized_ = true;
    Debug::Log("DifferenceSequenceTranscoder: Initialized successfully");
    return true;
}

void DifferenceSequenceTranscoder::Cleanup() {
    CancelTranscode();
    primary_decoder_.Cleanup();
    comparison_decoder_.Cleanup();
    initialized_ = false;
}

// ============================================================================
// Cache Management
// ============================================================================

void DifferenceSequenceTranscoder::SetupCacheDirectory() {
    cache_key_ = GenerateCacheKey();

    // Build cache directory path (use EXRtranscodes subfolder for disk space management)
    if (!config_.custom_cache_path.empty()) {
        combined_cache_dir_ = fs::path(config_.custom_cache_path) / "EXRtranscodes" / cache_key_;
    } else {
        char* localappdata = getenv("LOCALAPPDATA");
        if (localappdata) {
            combined_cache_dir_ = fs::path(localappdata) / "ump" / "EXRtranscodes" / cache_key_;
        } else {
            combined_cache_dir_ = fs::path("cache") / "EXRtranscodes" / cache_key_;
        }
    }

    Debug::Log("DifferenceSequenceTranscoder: Cache directory: " + combined_cache_dir_.string());
}

std::string DifferenceSequenceTranscoder::GenerateCacheKey() const {
    // Generate hash from both video paths + resolution + amplification
    std::hash<std::string> hasher;
    size_t hash1 = hasher(primary_video_path_);
    size_t hash2 = hasher(comparison_video_path_);
    size_t combined_hash = hash1 ^ (hash2 << 1);

    std::stringstream ss;
    ss << std::hex << combined_hash << "_" << output_width_ << "_diff";
    return ss.str();
}

bool DifferenceSequenceTranscoder::IsCached() const {
    if (!fs::exists(combined_cache_dir_)) {
        return false;
    }

    // Check if we have enough frames
    int cached_frames = 0;
    for (const auto& entry : fs::directory_iterator(combined_cache_dir_)) {
        if (entry.path().extension() == ".png") {
            cached_frames++;
        }
    }

    return cached_frames >= total_frames_;
}

// ============================================================================
// Transcoding
// ============================================================================

bool DifferenceSequenceTranscoder::StartTranscode() {
    if (!initialized_) {
        Debug::Log("ERROR: Not initialized");
        return false;
    }

    if (transcoding_) {
        Debug::Log("ERROR: Already transcoding");
        return false;
    }

    if (complete_) {
        Debug::Log("DifferenceSequenceTranscoder: Already complete");
        return true;
    }

    // Create cache directory
    try {
        fs::create_directories(combined_cache_dir_);
    } catch (const std::exception& e) {
        Debug::Log("ERROR: Failed to create cache directory: " + std::string(e.what()));
        return false;
    }

    // Start worker thread
    transcoding_ = true;
    cancel_requested_ = false;
    progress_ = 0.0f;
    transcode_thread_ = std::thread(&DifferenceSequenceTranscoder::TranscodeWorker, this);

    return true;
}

void DifferenceSequenceTranscoder::CancelTranscode() {
    Debug::Log("DifferenceSequenceTranscoder: Cancel requested");
    cancel_requested_ = true;

    if (transcode_thread_.joinable()) {
        transcode_thread_.join();
    }

    transcoding_ = false;
}

void DifferenceSequenceTranscoder::TranscodeWorker() {
    Debug::Log("DifferenceSequenceTranscoder: Worker thread started with " + std::to_string(num_threads_) + " parallel workers");

    // Initialize decoder contexts for each worker thread
    primary_decoders_.resize(num_threads_);
    comparison_decoders_.resize(num_threads_);

    for (int i = 0; i < num_threads_; ++i) {
        if (!primary_decoders_[i].Initialize(primary_video_path_, config_.hw_decode)) {
            Debug::Log("ERROR: Failed to initialize primary decoder " + std::to_string(i));
            transcoding_ = false;
            return;
        }
        if (!comparison_decoders_[i].Initialize(comparison_video_path_, config_.hw_decode)) {
            Debug::Log("ERROR: Failed to initialize comparison decoder " + std::to_string(i));
            transcoding_ = false;
            return;
        }
    }

    // Reset progress tracking
    next_frame_ = 0;
    frames_completed_ = 0;

    // Launch worker threads
    worker_threads_.clear();
    for (int i = 0; i < num_threads_; ++i) {
        worker_threads_.emplace_back(&DifferenceSequenceTranscoder::ParallelTranscodeWorker, this, i);
    }

    // Wait for all workers to complete
    for (auto& thread : worker_threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }

    // Cleanup decoder contexts
    primary_decoders_.clear();
    comparison_decoders_.clear();

    if (cancel_requested_) {
        Debug::Log("DifferenceSequenceTranscoder: Cancelled");
        transcoding_ = false;
        return;
    }

    Debug::Log("DifferenceSequenceTranscoder: Transcode complete!");
    transcoding_ = false;
    complete_ = true;
    progress_ = 1.0f;

    if (completion_callback_) {
        completion_callback_();
    }
}

void DifferenceSequenceTranscoder::ParallelTranscodeWorker(int worker_id) {
    while (true) {
        if (cancel_requested_) {
            return;
        }

        // Get next frame to process (atomic)
        int frame = next_frame_.fetch_add(1);

        if (frame >= total_frames_) {
            // No more frames to process
            return;
        }

        // Process this frame
        if (!TranscodeFrame(frame, primary_decoders_[worker_id], comparison_decoders_[worker_id])) {
            Debug::Log("ERROR: Worker " + std::to_string(worker_id) + " failed to transcode frame " + std::to_string(frame));
            cancel_requested_ = true;
            return;
        }

        // Update progress
        int completed = frames_completed_.fetch_add(1) + 1;
        progress_ = (float)completed / (float)total_frames_;

        if (progress_callback_) {
            progress_callback_(progress_);
        }

        if (completed % 100 == 0) {
            Debug::Log("DifferenceSequenceTranscoder: Progress " + std::to_string((int)(progress_ * 100)) + "%");
        }
    }
}

bool DifferenceSequenceTranscoder::TranscodeFrame(int frame_number, DecoderContext& primary_ctx, DecoderContext& comparison_ctx) {
    double timestamp = frame_number / frame_rate_;

    // Allocate frames
    AVFrame* primary_frame = av_frame_alloc();
    AVFrame* comparison_frame = av_frame_alloc();

    if (!primary_frame || !comparison_frame) {
        av_frame_free(&primary_frame);
        av_frame_free(&comparison_frame);
        return false;
    }

    // Decode both frames
    bool primary_ok = DecodeFrameAtTimestamp(timestamp, primary_frame, primary_ctx);
    bool comparison_ok = DecodeFrameAtTimestamp(timestamp, comparison_frame, comparison_ctx);

    if (!primary_ok || !comparison_ok) {
        av_frame_free(&primary_frame);
        av_frame_free(&comparison_frame);
        return false;
    }

    // Composite and save
    bool success = CompositeAndSaveDifference(primary_frame, comparison_frame, frame_number);

    av_frame_free(&primary_frame);
    av_frame_free(&comparison_frame);

    return success;
}

bool DifferenceSequenceTranscoder::DecodeFrameAtTimestamp(double timestamp, AVFrame* output_frame, DecoderContext& ctx) {
    // Seek to timestamp
    int64_t seek_target = (int64_t)(timestamp * AV_TIME_BASE);
    av_seek_frame(ctx.format_ctx, -1, seek_target, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(ctx.codec_ctx);

    AVPacket* packet = av_packet_alloc();
    bool found = false;

    while (av_read_frame(ctx.format_ctx, packet) >= 0) {
        if (packet->stream_index == ctx.video_stream_index) {
            if (avcodec_send_packet(ctx.codec_ctx, packet) >= 0) {
                if (avcodec_receive_frame(ctx.codec_ctx, output_frame) >= 0) {
                    found = true;
                    av_packet_unref(packet);
                    break;
                }
            }
        }
        av_packet_unref(packet);
    }

    av_packet_free(&packet);
    return found;
}

bool DifferenceSequenceTranscoder::CompositeAndSaveDifference(AVFrame* primary_frame, AVFrame* comparison_frame, int frame_number) {
    // Convert both frames to RGB
    SwsContext* sws_ctx = sws_getContext(
        primary_frame->width, primary_frame->height, (AVPixelFormat)primary_frame->format,
        output_width_, output_height_, AV_PIX_FMT_RGB24,
        SWS_BILINEAR, nullptr, nullptr, nullptr
    );

    if (!sws_ctx) {
        return false;
    }

    // Allocate RGB buffers
    int rgb_size = output_width_ * output_height_ * 3;
    unsigned char* primary_rgb = new unsigned char[rgb_size];
    unsigned char* comparison_rgb = new unsigned char[rgb_size];
    unsigned char* diff_rgb = new unsigned char[rgb_size];

    // Convert primary to RGB
    uint8_t* primary_data[1] = { primary_rgb };
    int primary_linesize[1] = { output_width_ * 3 };
    sws_scale(sws_ctx, primary_frame->data, primary_frame->linesize, 0, primary_frame->height, primary_data, primary_linesize);

    // Convert comparison to RGB
    uint8_t* comparison_data[1] = { comparison_rgb };
    int comparison_linesize[1] = { output_width_ * 3 };
    sws_scale(sws_ctx, comparison_frame->data, comparison_frame->linesize, 0, comparison_frame->height, comparison_data, comparison_linesize);

    sws_freeContext(sws_ctx);

    // CPU-based difference compositing
    for (int i = 0; i < rgb_size; i += 3) {
        for (int c = 0; c < 3; ++c) {
            float p = primary_rgb[i + c] / 255.0f;
            float q = comparison_rgb[i + c] / 255.0f;
            float diff = std::abs(p - q) * config_.amplification;
            diff = (std::min)(1.0f, (std::max)(0.0f, diff));
            diff_rgb[i + c] = (unsigned char)(diff * 255.0f);
        }
    }

    // Build output path
    std::stringstream ss;
    ss << combined_cache_dir_.string() << "/frame_" << std::setfill('0') << std::setw(5) << frame_number << ".png";
    std::string output_path = ss.str();

    // Save PNG
    bool success = SaveFrameAsPNG(diff_rgb, output_width_, output_height_, output_path);

    delete[] primary_rgb;
    delete[] comparison_rgb;
    delete[] diff_rgb;

    return success;
}

bool DifferenceSequenceTranscoder::SaveFrameAsPNG(unsigned char* rgb_data, int width, int height, const std::string& output_path) {
    int stride = width * 3;
    int result = stbi_write_png(output_path.c_str(), width, height, 3, rgb_data, stride);
    return result != 0;
}

} // namespace ump
