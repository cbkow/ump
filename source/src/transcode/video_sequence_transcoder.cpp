#include "video_sequence_transcoder.h"
#include "../metadata/video_metadata.h"
#include "../player/media_background_extractor.h"  // For ConversionStrategy and HardwareDecodeMode
#include "../player/hw_context_manager.h"  // Shared hardware decode contexts
#include "../utils/debug_utils.h"
#include <sstream>
#include <iomanip>
#include <fstream>

// FFmpeg includes
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/hwcontext.h>
#include <libswscale/swscale.h>
#include <libavutil/opt.h>
}

// STB image write for PNG output (implementation in video_player.cpp)
#include "../../external/glfw/deps/stb_image_write.h"

// Helper to write PNG with compression level
static void stbi_write_png_with_compression(const char* filename, int w, int h, int comp, const void* data, int stride_in_bytes, int compression_level) {
    stbi_write_png_compression_level = compression_level;
    stbi_write_png(filename, w, h, comp, data, stride_in_bytes);
}

namespace ump {

// ============================================================================
// Constructor / Destructor
// ============================================================================

VideoSequenceTranscoder::VideoSequenceTranscoder() {
    Debug::Log("VideoSequenceTranscoder: Constructor");
}

VideoSequenceTranscoder::~VideoSequenceTranscoder() {
    Debug::Log("VideoSequenceTranscoder: Destructor");
    Cleanup();
}

// ============================================================================
// Lifecycle
// ============================================================================

bool VideoSequenceTranscoder::Initialize(const std::string& video_path, const VideoMetadata& metadata, const TranscodeConfig& config) {
    Debug::Log("VideoSequenceTranscoder: Initializing for " + video_path);

    video_path_ = video_path;
    metadata_ = metadata;
    config_ = config;

    // Create conversion strategy from metadata (4444/422/420 awareness)
    conversion_strategy_ = std::make_unique<ConversionStrategy>(ConversionStrategy::FromMetadata(metadata_));
    Debug::Log("VideoSequenceTranscoder: Conversion strategy - " + conversion_strategy_->GetDescription());

    // Extract video properties from metadata
    output_width_ = (config_.max_width > 0 && metadata_.width > config_.max_width)
                   ? config_.max_width
                   : metadata_.width;
    output_height_ = (int)(output_width_ * ((double)metadata_.height / metadata_.width));
    frame_rate_ = metadata_.frame_rate;
    total_frames_ = metadata_.total_frames > 0 ? metadata_.total_frames : 0;
    duration_ = (frame_rate_ > 0) ? (total_frames_ / frame_rate_) : 0.0;

    Debug::Log("VideoSequenceTranscoder: Output resolution " + std::to_string(output_width_) + "x" + std::to_string(output_height_));
    Debug::Log("VideoSequenceTranscoder: Total frames: " + std::to_string(total_frames_) + " at " + std::to_string(frame_rate_) + " fps");

    // Setup cache directory
    SetupCacheDirectory();

    initialized_ = true;
    return true;
}

bool VideoSequenceTranscoder::StartTranscode() {
    if (!initialized_) {
        Debug::Log("ERROR: VideoSequenceTranscoder: Cannot start - not initialized");
        return false;
    }

    if (transcoding_) {
        Debug::Log("ERROR: VideoSequenceTranscoder: Transcode already in progress");
        return false;
    }

    // Check if already cached
    if (!config_.force_retranscode && IsCached()) {
        Debug::Log("VideoSequenceTranscoder: Sequence already cached, skipping transcode");
        transcode_complete_ = true;
        transcode_progress_ = 1.0f;
        return true;
    }

    // Start transcode thread
    transcoding_ = true;
    transcode_complete_ = false;
    cancel_requested_ = false;
    transcode_progress_ = 0.0f;

    transcode_thread_ = std::thread(&VideoSequenceTranscoder::TranscodeWorker, this);
    Debug::Log("VideoSequenceTranscoder: Transcode thread started");

    return true;
}

void VideoSequenceTranscoder::CancelTranscode() {
    Debug::Log("VideoSequenceTranscoder: Cancel requested");
    cancel_requested_ = true;

    if (transcode_thread_.joinable()) {
        transcode_thread_.join();
    }

    transcoding_ = false;
}

void VideoSequenceTranscoder::Cleanup() {
    Debug::Log("VideoSequenceTranscoder: Cleanup");

    CancelTranscode();

    initialized_ = false;
}

// ============================================================================
// Cache Management
// ============================================================================

void VideoSequenceTranscoder::SetupCacheDirectory() {
    // Follow EXRTranscoder pattern for cache directory
    // Priority:
    // 1. Custom cache path if set
    // 2. %LOCALAPPDATA%\ump\VideoFrameCache\
    // 3. Fallback to temp/video_frame_cache/

    fs::path base_cache_dir;

    if (!config_.custom_cache_path.empty()) {
        base_cache_dir = fs::path(config_.custom_cache_path) / "VideoFrameCache";
    } else {
#ifdef _WIN32
        const char* localappdata = std::getenv("LOCALAPPDATA");
        if (localappdata) {
            base_cache_dir = fs::path(localappdata) / "ump" / "VideoFrameCache";
        } else {
            base_cache_dir = "temp/video_frame_cache";
        }
#else
        // Linux/Mac: use ~/.cache/ump/VideoFrameCache
        const char* home = std::getenv("HOME");
        if (home) {
            base_cache_dir = fs::path(home) / ".cache" / "ump" / "VideoFrameCache";
        } else {
            base_cache_dir = "temp/video_frame_cache";
        }
#endif
    }

    // Generate cache key from video properties
    cache_key_ = GenerateCacheKey();

    // Full cache directory: base/cache_key/
    cache_dir_ = base_cache_dir / cache_key_;

    // Create directory if it doesn't exist
    if (!fs::exists(cache_dir_)) {
        fs::create_directories(cache_dir_);
        Debug::Log("VideoSequenceTranscoder: Created cache directory: " + cache_dir_.string());
    } else {
        Debug::Log("VideoSequenceTranscoder: Using existing cache directory: " + cache_dir_.string());
    }
}

std::string VideoSequenceTranscoder::GenerateCacheKey() const {
    // Generate cache key from: video path + resolution + colorspace + range
    // Format: {filename}_{width}_{colorspace}_{range}

    fs::path video_path(video_path_);
    std::string filename = video_path.stem().string();

    // Sanitize filename (remove invalid characters)
    std::string sanitized = filename;
    for (char& c : sanitized) {
        if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
            c = '_';
        }
    }

    // Build key
    std::ostringstream oss;
    oss << sanitized << "_"
        << output_width_ << "_"
        << metadata_.colorspace << "_"
        << metadata_.range_type;

    return oss.str();
}

std::string VideoSequenceTranscoder::GetCacheDirectory() const {
    return cache_dir_.string();
}

bool VideoSequenceTranscoder::IsCached() const {
    if (!fs::exists(cache_dir_)) {
        return false;
    }

    // Check if all frames exist (sample first, middle, last frames)
    if (total_frames_ <= 0) {
        return false;
    }

    std::vector<int> sample_frames = {0, total_frames_ / 2, total_frames_ - 1};
    for (int frame : sample_frames) {
        std::string frame_path = GetFramePath(frame);
        if (!fs::exists(frame_path)) {
            return false;
        }
    }

    return true;
}

void VideoSequenceTranscoder::ClearCache() {
    if (fs::exists(cache_dir_)) {
        fs::remove_all(cache_dir_);
        Debug::Log("VideoSequenceTranscoder: Cleared cache directory: " + cache_dir_.string());
    }
}

std::string VideoSequenceTranscoder::GetFramePath(int frame_number) const {
    std::ostringstream oss;
    oss << "frame_" << std::setw(5) << std::setfill('0') << frame_number << ".png";
    return (cache_dir_ / oss.str()).string();
}

// ============================================================================
// Transcode Worker
// ============================================================================

void VideoSequenceTranscoder::TranscodeWorker() {
    Debug::Log("VideoSequenceTranscoder: Worker thread started");

    // Initialize FFmpeg context
    WorkerContext ctx;
    if (!ctx.Initialize(video_path_, config_.hw_decode)) {
        Debug::Log("ERROR: VideoSequenceTranscoder: Failed to initialize FFmpeg context");
        transcoding_ = false;
        return;
    }

    // Transcode all frames
    for (int frame = 0; frame < total_frames_; ++frame) {
        if (cancel_requested_) {
            Debug::Log("VideoSequenceTranscoder: Transcode cancelled");
            break;
        }

        double timestamp = frame / frame_rate_;

        if (!ExtractFrameToPNG(frame, timestamp, ctx)) {
            Debug::Log("ERROR: VideoSequenceTranscoder: Failed to extract frame " + std::to_string(frame));
            // Continue with next frame (don't abort entire transcode)
        }

        // Update progress
        transcode_progress_ = (float)(frame + 1) / total_frames_;

        // Log progress every 10%
        if (frame % (total_frames_ / 10) == 0) {
            Debug::Log("VideoSequenceTranscoder: Progress " + std::to_string((int)(transcode_progress_ * 100)) + "%");
        }
    }

    if (!cancel_requested_) {
        transcode_complete_ = true;
        transcode_progress_ = 1.0f;
        Debug::Log("VideoSequenceTranscoder: Transcode complete!");
    }

    transcoding_ = false;
}

bool VideoSequenceTranscoder::ExtractFrameToPNG(int frame_number, double timestamp, WorkerContext& ctx) {
    if (!ctx.initialized) {
        Debug::Log("ERROR: WorkerContext not initialized");
        return false;
    }

    // Check if frame already exists on disk
    std::string output_path = GetFramePath(frame_number);
    if (!config_.force_retranscode && fs::exists(output_path)) {
        return true;  // Already transcoded
    }

    // Allocate frame for decoded data
    AVFrame* frame = av_frame_alloc();
    if (!frame) {
        Debug::Log("ERROR: Failed to allocate frame");
        return false;
    }

    // Decode frame at timestamp
    bool decode_success = DecodeFrameAtTimestamp(timestamp, frame, ctx);
    if (!decode_success) {
        av_frame_free(&frame);
        return false;
    }

    // Save frame as PNG
    bool save_success = SaveFrameAsPNG(frame, output_path, frame_number);

    av_frame_free(&frame);
    return save_success;
}

bool VideoSequenceTranscoder::DecodeFrameAtTimestamp(double timestamp, AVFrame* output_frame, WorkerContext& ctx) {
    AVStream* video_stream = ctx.format_ctx->streams[ctx.video_stream_index];

    // Convert timestamp to stream timebase
    int64_t seek_target = (int64_t)(timestamp * AV_TIME_BASE);
    int64_t stream_timestamp = av_rescale_q(seek_target, AV_TIME_BASE_Q, video_stream->time_base);

    // Seek to timestamp
    if (av_seek_frame(ctx.format_ctx, ctx.video_stream_index, stream_timestamp, AVSEEK_FLAG_BACKWARD) < 0) {
        Debug::Log("ERROR: Failed to seek to timestamp " + std::to_string(timestamp));
        return false;
    }

    // Flush codec buffers after seek
    avcodec_flush_buffers(ctx.codec_ctx);

    // Read packets until we find the target frame
    AVPacket* packet = av_packet_alloc();
    bool frame_found = false;

    while (av_read_frame(ctx.format_ctx, packet) >= 0) {
        if (packet->stream_index == ctx.video_stream_index) {
            // Send packet to decoder
            if (avcodec_send_packet(ctx.codec_ctx, packet) >= 0) {
                // Receive decoded frame
                while (avcodec_receive_frame(ctx.codec_ctx, output_frame) >= 0) {
                    // Calculate frame timestamp
                    double frame_time = output_frame->pts * av_q2d(video_stream->time_base);

                    // Check if this is the frame we want (within 0.5 frame tolerance)
                    double frame_tolerance = 0.5 / frame_rate_;
                    if (std::abs(frame_time - timestamp) < frame_tolerance) {
                        frame_found = true;
                        break;
                    }

                    // If we've gone past the target, stop
                    if (frame_time > timestamp + frame_tolerance) {
                        break;
                    }
                }
            }

            if (frame_found) {
                break;
            }
        }

        av_packet_unref(packet);
    }

    av_packet_free(&packet);

    if (!frame_found) {
        Debug::Log("ERROR: Target frame not found at timestamp " + std::to_string(timestamp));
    }

    return frame_found;
}

bool VideoSequenceTranscoder::SaveFrameAsPNG(AVFrame* frame, const std::string& output_path, int frame_number) {
    if (!frame) {
        Debug::Log("ERROR: Null frame");
        return false;
    }

    // Get source dimensions
    int src_width = frame->width;
    int src_height = frame->height;

    // Calculate output dimensions (respecting max_width)
    int dst_width = output_width_;
    int dst_height = output_height_;

    // Setup swscale context with color matrix from ConversionStrategy
    int sws_flags = conversion_strategy_->sws_algorithm;
    SwsContext* sws_ctx = sws_getContext(
        src_width, src_height, (AVPixelFormat)frame->format,
        dst_width, dst_height, AV_PIX_FMT_RGB24,
        sws_flags, nullptr, nullptr, nullptr);

    if (!sws_ctx) {
        Debug::Log("ERROR: Failed to create swscale context");
        return false;
    }

    // Apply color matrix if needed (4444/422/420 awareness)
    if (conversion_strategy_->ShouldApplyColorMatrix()) {
        const int* src_coefficients = sws_getCoefficients(conversion_strategy_->source_colorspace);
        const int* dst_coefficients = sws_getCoefficients(SWS_CS_DEFAULT);  // RGB output

        // Set color matrix
        sws_setColorspaceDetails(sws_ctx,
            src_coefficients, conversion_strategy_->source_range,  // Source: YUV colorspace + range
            dst_coefficients, 1,  // Destination: RGB full range
            0, 1 << 16, 1 << 16);  // Brightness, contrast, saturation
    }

    // Allocate RGB buffer
    int rgb_linesize = dst_width * 3;  // 3 bytes per pixel (RGB24)
    std::vector<uint8_t> rgb_buffer(rgb_linesize * dst_height);

    uint8_t* dst_data[1] = { rgb_buffer.data() };
    int dst_linesize[1] = { rgb_linesize };

    // Convert frame to RGB
    sws_scale(sws_ctx, frame->data, frame->linesize, 0, src_height,
              dst_data, dst_linesize);

    sws_freeContext(sws_ctx);

    // Save as PNG using stb_image_write with configured compression
    stbi_write_png_compression_level = config_.png_compression_level;
    int result = stbi_write_png(output_path.c_str(), dst_width, dst_height, 3,
                                rgb_buffer.data(), rgb_linesize);

    if (!result) {
        Debug::Log("ERROR: Failed to write PNG: " + output_path);
        return false;
    }

    return true;
}

// ============================================================================
// WorkerContext Implementation
// ============================================================================

VideoSequenceTranscoder::WorkerContext::~WorkerContext() {
    Cleanup();
}

void VideoSequenceTranscoder::WorkerContext::Cleanup() {
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
}

bool VideoSequenceTranscoder::WorkerContext::Initialize(const std::string& video_path, HardwareDecodeMode hw_mode) {
    Debug::Log("VideoSequenceTranscoder::WorkerContext: Initializing FFmpeg for " + video_path);

    // Open input file
    if (avformat_open_input(&format_ctx, video_path.c_str(), nullptr, nullptr) < 0) {
        Debug::Log("ERROR: Failed to open video file: " + video_path);
        return false;
    }

    // Retrieve stream information
    if (avformat_find_stream_info(format_ctx, nullptr) < 0) {
        Debug::Log("ERROR: Failed to find stream information");
        return false;
    }

    // Find video stream
    video_stream_index = -1;
    for (unsigned int i = 0; i < format_ctx->nb_streams; i++) {
        if (format_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_index = i;
            break;
        }
    }

    if (video_stream_index == -1) {
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

    // Setup hardware decode
    // Try hardware decode using shared HWContextManager
    // TEMP DISABLED FOR TESTING - force software decode
    Debug::Log("VideoSequenceTranscoder::WorkerContext: *** HW ACCEL DISABLED FOR TESTING ***");
    if (false && hw_mode != HardwareDecodeMode::SOFTWARE_ONLY) {
        AVHWDeviceType hw_type = AV_HWDEVICE_TYPE_NONE;

        if (hw_mode == HardwareDecodeMode::D3D11VA || hw_mode == HardwareDecodeMode::AUTO) {
            hw_type = AV_HWDEVICE_TYPE_D3D11VA;
        } else if (hw_mode == HardwareDecodeMode::NVDEC) {
            hw_type = AV_HWDEVICE_TYPE_CUDA;  // NVDEC uses CUDA backend
        }

        if (hw_type != AV_HWDEVICE_TYPE_NONE) {
            // Use shared hardware context from HWContextManager
            AVBufferRef* shared_ctx = HWContextManager::Instance().GetContext(static_cast<int>(hw_type));
            if (shared_ctx) {
                hw_device_ctx = av_buffer_ref(shared_ctx);
                if (hw_device_ctx) {
                    codec_ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);
                    Debug::Log("VideoSequenceTranscoder: Using shared hardware context: " +
                              std::string(av_hwdevice_get_type_name(hw_type)));
                }
            } else {
                Debug::Log("VideoSequenceTranscoder: Hardware decode not available, using software");
            }
        }
    }

    // Open codec
    if (avcodec_open2(codec_ctx, codec, nullptr) < 0) {
        Debug::Log("ERROR: Failed to open codec");
        return false;
    }

    Debug::Log("VideoSequenceTranscoder::WorkerContext: Initialized successfully");
    initialized = true;
    return true;
}

} // namespace ump
