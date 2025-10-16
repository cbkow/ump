// Prevent Windows min/max macros from conflicting with std::min/max
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#endif

#include "media_background_extractor.h"
#include "frame_cache.h"
#include "video_player.h"  // For PIPELINE_CONFIGS
#include "../metadata/video_metadata.h"
#include "../utils/debug_utils.h"
#include <algorithm>
#include <cmath>
#include <mutex>
#include <cstdlib>

// Windows-specific includes for hardware decode (BEFORE extern "C")
#ifdef _WIN32
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#endif

// FFmpeg includes
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/hwcontext.h>
#include <libswscale/swscale.h>
}

MediaBackgroundExtractor::MediaBackgroundExtractor(FrameCache* parent_cache, const ExtractorConfig& cfg)
    : config(cfg), parent_cache(parent_cache) {
    Debug::Log("MediaBackgroundExtractor: Initializing with " + std::to_string(config.max_batch_size) + " batch size");

    // Initialize FFmpeg (global, thread-safe)
    av_log_set_level(AV_LOG_WARNING);  // Reduce spam

    InitializeTexturePool();
}

MediaBackgroundExtractor::~MediaBackgroundExtractor() {
    Debug::Log("MediaBackgroundExtractor: Shutting down");
    Shutdown();
    DestroyTexturePool();
}

bool MediaBackgroundExtractor::Initialize(const std::string& video_path, const VideoMetadata* metadata) {
    Debug::Log("MediaBackgroundExtractor: Initializing for " + video_path);

    if (initialized.load()) {
        Debug::Log("MediaBackgroundExtractor: Already initialized, cleaning up first");
        Shutdown();
    }

    this->video_path = video_path;

    // Note: Sequential caching state removed

    // Open video file
    format_context = avformat_alloc_context();
    if (avformat_open_input(&format_context, video_path.c_str(), nullptr, nullptr) < 0) {
        Debug::Log("MediaBackgroundExtractor: Failed to open video file: " + video_path);
        return false;
    }

    if (avformat_find_stream_info(format_context, nullptr) < 0) {
        Debug::Log("MediaBackgroundExtractor: Failed to find stream info");
        avformat_close_input(&format_context);
        return false;
    }

    // Find video stream
    video_stream_index = av_find_best_stream(format_context, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (video_stream_index < 0) {
        Debug::Log("MediaBackgroundExtractor: No video stream found");
        avformat_close_input(&format_context);
        return false;
    }

    AVStream* video_stream = format_context->streams[video_stream_index];

    // Store video properties
    duration = format_context->duration / (double)AV_TIME_BASE;
    frame_rate = av_q2d(video_stream->r_frame_rate);
    if (frame_rate <= 0) {
        frame_rate = av_q2d(video_stream->avg_frame_rate);
        if (frame_rate <= 0) frame_rate = 30.0;  // Fallback
    }
    start_time = video_stream->start_time;

    // Get video dimensions from codec parameters
    video_width = video_stream->codecpar->width;
    video_height = video_stream->codecpar->height;

    // Validate duration and calculate max frames
    double dur = duration.load();
    double fps = frame_rate.load();
    int estimated_frames = (dur > 0 && fps > 0) ? static_cast<int>(dur * fps) : 0;

    Debug::Log("MediaBackgroundExtractor: Video properties - " +
               std::to_string(video_width.load()) + "x" + std::to_string(video_height.load()) +
               " @ " + std::to_string(fps) + "fps, " +
               std::to_string(dur) + "s duration, ~" + std::to_string(estimated_frames) + " frames");

    // Warning for suspicious durations
    if (dur <= 0) {
        Debug::Log("MediaBackgroundExtractor: WARNING - Invalid duration detected, frame bounds may be unreliable");
    } else if (estimated_frames > 100000) {
        Debug::Log("MediaBackgroundExtractor: WARNING - Very long video (" + std::to_string(estimated_frames) +
                  " frames), consider using lower cache settings");
    }

    // Setup decoder with hardware acceleration
    if (!SetupHardwareDecode()) {
        Debug::Log("MediaBackgroundExtractor: Hardware decode setup failed, continuing with software");
    }

    // NEW: Setup metadata-driven conversion strategy for conditional 4444 color matrix
    if (metadata && metadata->is_loaded) {
        auto strategy = ConversionStrategy::FromMetadata(*metadata);
        SetConversionStrategy(strategy);
        Debug::Log("MediaBackgroundExtractor: Conversion strategy initialized - " + strategy.GetDescription());
    } else {
        Debug::Log("MediaBackgroundExtractor: No metadata provided - using standard processing");
    }

    initialized = true;
    Debug::Log("MediaBackgroundExtractor: Initialization complete");
    return true;
}

bool MediaBackgroundExtractor::SetupHardwareDecode() {
    AVStream* video_stream = format_context->streams[video_stream_index];
    const AVCodec* decoder = nullptr;

    // Try hardware decoders based on config
    if (config.hw_config.mode == HardwareDecodeMode::NVDEC || config.hw_config.mode == HardwareDecodeMode::AUTO) {
        if (InitializeNVDEC()) {
            current_hw_mode = HardwareDecodeMode::NVDEC;
            current_hw_decoder_name = "NVDEC";
            Debug::Log("MediaBackgroundExtractor: Using NVDEC hardware acceleration");
        }
    }

    if (current_hw_mode == HardwareDecodeMode::SOFTWARE_ONLY &&
        (config.hw_config.mode == HardwareDecodeMode::D3D11VA || config.hw_config.mode == HardwareDecodeMode::AUTO)) {
        if (InitializeD3D11VA()) {
            current_hw_mode = HardwareDecodeMode::D3D11VA;
            current_hw_decoder_name = "D3D11VA";
            Debug::Log("MediaBackgroundExtractor: Using D3D11VA hardware acceleration");
        }
    }

    // Create codec context
    if (current_hw_mode != HardwareDecodeMode::SOFTWARE_ONLY) {
        // Hardware decoder
        decoder = avcodec_find_decoder_by_name((current_hw_decoder_name + "_" + avcodec_get_name(video_stream->codecpar->codec_id)).c_str());
    }

    if (!decoder) {
        // Fallback to software decoder
        decoder = avcodec_find_decoder(video_stream->codecpar->codec_id);
        current_hw_mode = HardwareDecodeMode::SOFTWARE_ONLY;
        current_hw_decoder_name = "Software";
        Debug::Log("MediaBackgroundExtractor: Using software decoder");
    }

    if (!decoder) {
        Debug::Log("MediaBackgroundExtractor: No suitable decoder found");
        return false;
    }

    codec_context = avcodec_alloc_context3(decoder);
    if (avcodec_parameters_to_context(codec_context, video_stream->codecpar) < 0) {
        Debug::Log("MediaBackgroundExtractor: Failed to copy codec parameters");
        return false;
    }

    // Set hardware device context if using hardware decode
    if (hw_device_ctx && current_hw_mode != HardwareDecodeMode::SOFTWARE_ONLY) {
        codec_context->hw_device_ctx = av_buffer_ref(hw_device_ctx);
    }

    // Configure for frame extraction performance
    codec_context->thread_count = 0;  // Auto-detect threads
    codec_context->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;

    if (avcodec_open2(codec_context, decoder, nullptr) < 0) {
        Debug::Log("MediaBackgroundExtractor: Failed to open codec");
        return false;
    }

    return true;
}

bool MediaBackgroundExtractor::InitializeD3D11VA() {
#ifdef _WIN32
    // Create D3D11 device for hardware decode
    int ret = av_hwdevice_ctx_create(&hw_device_ctx, AV_HWDEVICE_TYPE_D3D11VA, nullptr, nullptr, 0);
    if (ret < 0) {
        Debug::Log("MediaBackgroundExtractor: D3D11VA initialization failed: " + std::to_string(ret));
        return false;
    }
    return true;
#else
    return false;
#endif
}

bool MediaBackgroundExtractor::InitializeNVDEC() {
    // Check if NVDEC is available
    return av_hwdevice_ctx_create(&hw_device_ctx, AV_HWDEVICE_TYPE_CUDA, nullptr, nullptr, 0) >= 0;
}

void MediaBackgroundExtractor::StartBackgroundExtraction() {
    if (!initialized.load()) {
        Debug::Log("MediaBackgroundExtractor: Cannot start - not initialized");
        return;
    }

    if (current_state.load() != ExtractorState::STOPPED) {
        Debug::Log("MediaBackgroundExtractor: Background extraction already active");
        return;
    }

    Debug::Log("MediaBackgroundExtractor: Starting background extraction with " +
               std::to_string(config.max_concurrent_batches) + " worker threads");

    shutdown_requested = false;
    SetState(ExtractorState::EXTRACTING);

    // Start worker threads with thread-safe contexts
    worker_threads.clear();
    for (int i = 0; i < config.max_concurrent_batches; ++i) {
        worker_threads.emplace_back(&MediaBackgroundExtractor::WorkerThread, this);
    }

    // Start with window-based caching around current position
    double initial_timestamp = current_playhead_position.load();
    RequestWindowAroundPlayhead(initial_timestamp);

    Debug::Log("MediaBackgroundExtractor: Using window-only extraction strategy");

    extraction_start_time = std::chrono::steady_clock::now();
}

void MediaBackgroundExtractor::StopBackgroundExtraction() {
    Debug::Log("MediaBackgroundExtractor: Stopping background extraction");

    shutdown_requested = true;
    SetState(ExtractorState::STOPPED);

    // Wake up all worker threads
    queue_cv.notify_all();
    worker_cv.notify_all();

    // Wait for threads to finish
    for (auto& thread : worker_threads) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    worker_threads.clear();

    // Clear pending work
    ClearPendingRequests();

    Debug::Log("MediaBackgroundExtractor: Background extraction stopped");
}

void MediaBackgroundExtractor::WorkerThread() {
    Debug::Log("MediaBackgroundExtractor: Worker thread started");

    // Wait for initialization to complete
    while (!initialized.load() && !shutdown_requested.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (shutdown_requested.load()) {
        Debug::Log("MediaBackgroundExtractor: Worker thread shutting down before initialization");
        return;
    }

    // Create thread-local FFmpeg context for video files
    WorkerContext worker_ctx;
    if (!worker_ctx.Initialize(video_path, config.hw_config)) {
        Debug::Log("MediaBackgroundExtractor: Failed to initialize worker context");
        return;
    }

    AVFrame* frame = av_frame_alloc();
    if (!frame) {
        Debug::Log("MediaBackgroundExtractor: Failed to allocate AVFrame");
        return;
    }

    while (!shutdown_requested.load()) {
        // Event-driven state check - no polling
        if (!ShouldExtract()) {
            std::unique_lock<std::mutex> lock(queue_mutex);
            worker_cv.wait(lock);  // Pure event-driven - wait until state changes
            continue;
        }

        // Get next batch of work
        ExtractionBatch batch = BuildNextBatch();
        if (batch.frames.empty()) {
            // No more work available - window-based caching is demand-driven
            // Worker will wait for new requests from window updates

            std::unique_lock<std::mutex> lock(queue_mutex);
            queue_cv.wait(lock);  // Pure event-driven - wait until work is available
            continue;
        }

        // Process the batch with worker context
        std::vector<ExtractionResult> results = ProcessBatch(batch, worker_ctx);

        // Note: Memory enforcement now handled by global checks in main loop

        // Queue results for main thread processing
        {
            std::lock_guard<std::mutex> lock(results_mutex);
            for (const auto& result : results) {
                completed_results.push(result);
                UpdateStats(result);
            }
        }
    }

    if (frame) {
        av_frame_free(&frame);
    }
    Debug::Log("MediaBackgroundExtractor: Worker thread finished");
}

ExtractionBatch MediaBackgroundExtractor::BuildNextBatch() {
    std::lock_guard<std::mutex> lock(queue_mutex);

    std::vector<FrameExtractionRequest> batch_frames;
    batch_frames.reserve(config.max_batch_size);

    // Extract up to max_batch_size requests
    while (!request_queue.empty() && batch_frames.size() < config.max_batch_size) {
        FrameExtractionRequest request = request_queue.top();
        request_queue.pop();

        // Skip if frame is already cached
        if (!IsFrameAlreadyCached(request.frame_number)) {
            batch_frames.push_back(request);
        }
    }

    if (batch_frames.empty()) {
        return ExtractionBatch();
    }

    // Sort batch by timestamp for sequential access optimization
    std::sort(batch_frames.begin(), batch_frames.end(),
        [](const FrameExtractionRequest& a, const FrameExtractionRequest& b) {
            return a.timestamp < b.timestamp;
        });

    bool is_sequential = true;
    if (batch_frames.size() > 1) {
        double fps = frame_rate.load();
        for (size_t i = 1; i < batch_frames.size(); ++i) {
            double expected_timestamp = batch_frames[i-1].timestamp + (1.0 / fps);
            if (std::abs(batch_frames[i].timestamp - expected_timestamp) > (2.0 / fps)) {
                is_sequential = false;
                break;
            }
        }
    }

    return ExtractionBatch(std::move(batch_frames), is_sequential);
}

std::vector<ExtractionResult> MediaBackgroundExtractor::ProcessBatch(const ExtractionBatch& batch, WorkerContext& worker_ctx) {
    std::vector<ExtractionResult> results;
    results.reserve(batch.frames.size());

    AVFrame* frame = av_frame_alloc();
    if (!frame) {
        Debug::Log("MediaBackgroundExtractor: Failed to allocate frame for batch processing");
        return results;
    }

    auto batch_start = std::chrono::steady_clock::now();

    for (const auto& request : batch.frames) {
        ExtractionResult result = ExtractSingleFrame(request, frame, worker_ctx);
        results.push_back(result);

        // Early exit if shutdown requested
        if (shutdown_requested.load()) {
            break;
        }
    }

    if (frame) {
        av_frame_free(&frame);
    }

    auto batch_end = std::chrono::steady_clock::now();
    double batch_time = std::chrono::duration<double, std::milli>(batch_end - batch_start).count();

    //Debug::Log("MediaBackgroundExtractor: Processed batch of " + std::to_string(batch.frames.size()) +
    //           " frames in " + std::to_string(batch_time) + "ms");

    return results;
}

ExtractionResult MediaBackgroundExtractor::ExtractSingleFrame(const FrameExtractionRequest& request, AVFrame* frame, WorkerContext& worker_ctx) {
    ExtractionResult result;
    result.frame_number = request.frame_number;
    result.timestamp = request.timestamp;
    result.completed_at = std::chrono::steady_clock::now();

    // Decode frame at timestamp
    if (!DecodeFrameAtTimestamp(request.timestamp, frame, worker_ctx)) {
        result.error_message = "Failed to decode frame at timestamp " + std::to_string(request.timestamp);
        return result;
    }

    // Extract to pixel buffer instead of texture (texture creation happens on main thread)
    std::vector<uint8_t> pixel_data;
    int width = frame->width;
    int height = frame->height;

    if (!ConvertFrameToPixelBuffer(frame, pixel_data, width, height)) {
        result.error_message = "Failed to convert frame to pixel buffer";
        return result;
    }

    result.success = true;
    result.texture_id = 0;  // Will be created on main thread
    result.width = width;
    result.height = height;
    // Calculate memory bytes based on pipeline mode
    size_t bytes_per_pixel;
    switch (config.pipeline_mode) {
        case PipelineMode::HIGH_RES:
            bytes_per_pixel = 8;  // 16-bit RGBA (4 channels * 2 bytes)
            break;
        case PipelineMode::ULTRA_HIGH_RES:
            bytes_per_pixel = 16; // 32-bit float RGBA (4 channels * 4 bytes) - EXR only
            break;
        case PipelineMode::HDR_RES:
            bytes_per_pixel = 8;  // 16-bit RGBA (4 channels * 2 bytes) - half-float equivalent
            break;
        default:  // PipelineMode::NORMAL
            bytes_per_pixel = 4;  // 8-bit RGBA (4 channels * 1 byte)
            break;
    }
    result.memory_bytes = width * height * bytes_per_pixel;
    result.pixel_data = std::move(pixel_data);  // Store pixel data for texture creation

    return result;
}

bool MediaBackgroundExtractor::DecodeFrameAtTimestamp(double timestamp, AVFrame* output_frame, WorkerContext& worker_ctx) {
    if (!worker_ctx.format_context || !worker_ctx.codec_context) {
        return false;
    }

    // Convert timestamp to stream timebase
    AVStream* stream = worker_ctx.format_context->streams[worker_ctx.video_stream_index];
    int64_t target_pts = av_rescale_q(timestamp * AV_TIME_BASE, AV_TIME_BASE_Q, stream->time_base);

    // Seek to target timestamp
    if (av_seek_frame(worker_ctx.format_context, worker_ctx.video_stream_index, target_pts, AVSEEK_FLAG_BACKWARD) < 0) {
        Debug::Log("MediaBackgroundExtractor: Seek failed for timestamp " + std::to_string(timestamp));
        return false;
    }

    avcodec_flush_buffers(worker_ctx.codec_context);

    AVPacket* packet = av_packet_alloc();
    if (!packet) {
        return false;
    }

    bool found_frame = false;

    // Read packets until we find our target frame
    while (av_read_frame(worker_ctx.format_context, packet) >= 0) {
        if (packet->stream_index == worker_ctx.video_stream_index) {
            if (avcodec_send_packet(worker_ctx.codec_context, packet) >= 0) {
                while (avcodec_receive_frame(worker_ctx.codec_context, output_frame) >= 0) {
                    // Check if this is close to our target timestamp
                    double frame_timestamp = output_frame->pts * av_q2d(stream->time_base);
                    if (std::abs(frame_timestamp - timestamp) < (1.0 / frame_rate.load())) {
                        found_frame = true;
                        break;
                    }
                    av_frame_unref(output_frame);
                }
            }
            if (found_frame) break;
        }
        av_packet_unref(packet);
    }

    av_packet_free(&packet);
    return found_frame;
}

bool MediaBackgroundExtractor::ConvertFrameToPixelBuffer(AVFrame* frame, std::vector<uint8_t>& pixel_data, int& width, int& height) {
    if (!frame || frame->width <= 0 || frame->height <= 0) {
        return false;
    }

    // NEW: For video formats that need color matrix processing, wait for metadata before processing
    if (!has_conversion_strategy) {
        // Check if this format needs metadata for proper color matrix application
        std::string format_name = av_get_pix_fmt_name((AVPixelFormat)frame->format);
        if (format_name.find("444") != std::string::npos ||
            format_name.find("422") != std::string::npos ||
            format_name.find("420") != std::string::npos ||
            format_name.find("rgba") != std::string::npos ||
            format_name.find("yuv") != std::string::npos) {
            Debug::Log("MediaBackgroundExtractor: Delaying " + format_name + " frame processing - waiting for metadata");
            return false;  // Skip processing until metadata arrives
        }
    }

    width = frame->width;
    height = frame->height;

    // Get pipeline configuration for output format
    auto it = PIPELINE_CONFIGS.find(config.pipeline_mode);
    if (it == PIPELINE_CONFIGS.end()) {
        it = PIPELINE_CONFIGS.find(PipelineMode::NORMAL);  // Fallback
    }
    const PipelineConfig& pipeline_config = it->second;

    // Determine target FFmpeg pixel format based on pipeline mode
    AVPixelFormat target_format;
    size_t bytes_per_pixel;

    switch (config.pipeline_mode) {
        case PipelineMode::HIGH_RES:
            target_format = AV_PIX_FMT_RGBA64LE;  // 16-bit RGBA
            bytes_per_pixel = 8;
            break;
        case PipelineMode::ULTRA_HIGH_RES:
            target_format = AV_PIX_FMT_RGBAF32LE;  // 32-bit float RGBA (EXR only)
            bytes_per_pixel = 16;
            break;
        case PipelineMode::HDR_RES:
            target_format = AV_PIX_FMT_RGBA64LE;   // 16-bit RGBA (half-float equivalent)
            bytes_per_pixel = 8;
            break;
        default:  // PipelineMode::NORMAL
            target_format = AV_PIX_FMT_RGBA;  // 8-bit RGBA
            bytes_per_pixel = 4;
            break;
    }

    // Allocate target frame
    AVFrame* target_frame = av_frame_alloc();
    target_frame->format = target_format;
    target_frame->width = width;
    target_frame->height = height;

    if (av_frame_get_buffer(target_frame, 32) < 0) {
        av_frame_free(&target_frame);
        return false;
    }

    // Setup software scaler for format conversion with conditional color matrix support
    int sws_flags = SWS_POINT;  // Default: Nearest neighbor - zero interpolation/processing

    // NEW: Apply conversion strategy for 4444 formats only
    if (has_conversion_strategy && conversion_strategy && conversion_strategy->ShouldApplyColorMatrix()) {
        sws_flags = conversion_strategy->sws_algorithm;  // Use higher quality scaling for 4444
        //Debug::Log("MediaBackgroundExtractor: Applying 4444 color matrix conversion");
    }

    SwsContext* sws_ctx = sws_getContext(
        frame->width, frame->height, (AVPixelFormat)frame->format,
        width, height, target_format,
        sws_flags,
        nullptr, nullptr, nullptr);

    if (!sws_ctx) {
        av_frame_free(&target_frame);
        return false;
    }

    // NEW: Apply format-specific color matrix processing
    if (has_conversion_strategy && conversion_strategy && conversion_strategy->ShouldApplyColorMatrix()) {
        const int *src_coefficients = nullptr;
        const int *dst_coefficients = nullptr;
        std::string processing_type;

        if (conversion_strategy->ShouldApplyFullMatrix()) {
            // FULL_MATRIX mode: Apply both colorspace and range conversion (4444 formats)
            src_coefficients = sws_getCoefficients(conversion_strategy->source_colorspace);
            dst_coefficients = sws_getCoefficients(conversion_strategy->source_colorspace);
            processing_type = "Full Matrix (4444)";
        } else if (conversion_strategy->ShouldApplyRangeOnly()) {
            // RANGE_ONLY mode: Apply only range conversion using identity colorspace (422/420 formats)
            src_coefficients = sws_getCoefficients(SWS_CS_ITU709);  // Identity/standard coefficients
            dst_coefficients = sws_getCoefficients(SWS_CS_ITU709);
            processing_type = "Range Only (422/420)";
        }

        if (src_coefficients && dst_coefficients) {
          /*  Debug::Log("MediaBackgroundExtractor: " + processing_type + " - src_colorspace=" +
                      std::to_string(conversion_strategy->source_colorspace) +
                      ", src_range=" + std::to_string(conversion_strategy->source_range) +
                      ", dst_range=1 (full RGB)");*/

            int ret = sws_setColorspaceDetails(sws_ctx,
                src_coefficients, conversion_strategy->source_range,  // Source: From metadata
                dst_coefficients, 1,                                  // Dest: Full range RGB
                0, 1 << 16, 1 << 16);                                // Brightness, contrast, saturation

            if (ret >= 0) {
                //Debug::Log("MediaBackgroundExtractor: Applied " + processing_type + " - " + conversion_strategy->debug_info + " -> RGB");
            } else {
                //Debug::Log("MediaBackgroundExtractor: Failed to apply " + processing_type + ", ret=" + std::to_string(ret));
            }
        } else {
          /*  Debug::Log("MediaBackgroundExtractor: ERROR - NULL coefficients for " + processing_type + "! src=" +
                      std::string(src_coefficients ? "valid" : "NULL") +
                      ", dst=" + std::string(dst_coefficients ? "valid" : "NULL"));*/
        }
    }

    // Convert to target format with conditional color processing for 4444
    sws_scale(sws_ctx, frame->data, frame->linesize, 0, height,
              target_frame->data, target_frame->linesize);

    // Copy raw pixel data to byte vector
    size_t data_size = width * height * bytes_per_pixel;
    pixel_data.resize(data_size);
    std::memcpy(pixel_data.data(), target_frame->data[0], data_size);

    sws_freeContext(sws_ctx);
    av_frame_free(&target_frame);

    return true;
}

GLuint MediaBackgroundExtractor::CreateTextureFromPixels(const std::vector<uint8_t>& pixel_data, int width, int height) {
    // This method will be called on the main rendering thread
    GLuint texture_id = 0;
    glGenTextures(1, &texture_id);

    if (texture_id == 0) {
        //Debug::Log("MediaBackgroundExtractor: Failed to create texture from pixels");
        return 0;
    }

    glBindTexture(GL_TEXTURE_2D, texture_id);

    // Set texture format based on pipeline mode
    GLenum internal_format, format, type;
    switch (config.pipeline_mode) {
        case PipelineMode::HIGH_RES:
            internal_format = GL_RGBA16;
            format = GL_RGBA;
            type = GL_UNSIGNED_SHORT;
            break;
        case PipelineMode::ULTRA_HIGH_RES:
            internal_format = GL_RGBA16F;
            format = GL_RGBA;
            type = GL_FLOAT;  // Full float for EXR
            break;
        case PipelineMode::HDR_RES:
            internal_format = GL_RGBA16F;
            format = GL_RGBA;
            type = GL_HALF_FLOAT;  // Half-float for video HDR
            break;
        default:  // PipelineMode::NORMAL
            internal_format = GL_RGBA8;
            format = GL_RGBA;
            type = GL_UNSIGNED_BYTE;
            break;
    }

    glTexImage2D(GL_TEXTURE_2D, 0, internal_format, width, height, 0,
                 format, type, pixel_data.data());

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glBindTexture(GL_TEXTURE_2D, 0);

    return texture_id;
}

// Note: RequestSequentialFrames() removed - using window-around-playhead only

void MediaBackgroundExtractor::RequestFrame(int frame_number, double timestamp, int priority) {
    // Validate frame bounds
    if (frame_number < 0) return;

    // For videos, use duration-based calculation
    double dur = duration.load();
    double fps = frame_rate.load();
    if (dur > 0 && fps > 0) {
        int max_frame = static_cast<int>(dur * fps) - 1;
        if (frame_number > max_frame) return;
    }

    // Check if frame is already cached
    if (parent_cache && IsFrameAlreadyCached(frame_number)) {
        return;
    }

    std::lock_guard<std::mutex> lock(queue_mutex);

    // Safety queue limit
    if (request_queue.size() >= config.max_queue_size) {
        return;
    }

    // Simple duplicate check with set
    if (requested_frames.count(frame_number) > 0) {
        return; // Already requested
    }

    // Add to tracking set
    requested_frames.insert(frame_number);

    FrameExtractionRequest request;
    request.frame_number = frame_number;
    request.timestamp = timestamp;
    request.priority = priority;
    request.requested_at = std::chrono::steady_clock::now();

    request_queue.push(request);
    queue_cv.notify_one();
}

// Additional implementation methods would continue here...
// (Texture pool management, timeline reposition handling, cleanup, etc.)

void MediaBackgroundExtractor::Shutdown() {
    if (!initialized.load()) return;

    StopBackgroundExtraction();

    // Cleanup FFmpeg resources
    if (codec_context) {
        avcodec_free_context(&codec_context);
    }
    if (format_context) {
        avformat_close_input(&format_context);
    }
    if (hw_device_ctx) {
        av_buffer_unref(&hw_device_ctx);
    }
    CleanupHardwareContext();

    initialized = false;
    Debug::Log("MediaBackgroundExtractor: Shutdown complete");
}

void MediaBackgroundExtractor::InitializeTexturePool() {
    std::lock_guard<std::mutex> lock(texture_pool_mutex);

    Debug::Log("MediaBackgroundExtractor: Initializing texture pool with " +
               std::to_string(config.texture_pool_size) + " textures");

    // Validate texture pool size
    if (config.texture_pool_size <= 0 || config.texture_pool_size > 1000) {
        Debug::Log("MediaBackgroundExtractor: Invalid texture pool size, using default 50");
        config.texture_pool_size = 50;
    }

    texture_pool.clear();
    available_textures = std::queue<GLuint>(); // Clear queue

    // Pre-allocate textures for efficient batching
    texture_pool.reserve(config.texture_pool_size);

    for (int i = 0; i < config.texture_pool_size; ++i) {
        GLuint texture_id = 0;
        glGenTextures(1, &texture_id);

        // Check for OpenGL errors
        GLenum error = glGetError();
        if (error != GL_NO_ERROR || texture_id == 0) {
            Debug::Log("MediaBackgroundExtractor: Failed to create texture " + std::to_string(i) +
                      ", OpenGL error: " + std::to_string(error));
            continue;
        }

        // Pre-configure texture parameters for RGBA data
        glBindTexture(GL_TEXTURE_2D, texture_id);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);

        // Check for configuration errors
        error = glGetError();
        if (error != GL_NO_ERROR) {
            Debug::Log("MediaBackgroundExtractor: Failed to configure texture " + std::to_string(i) +
                      ", OpenGL error: " + std::to_string(error));
            glDeleteTextures(1, &texture_id);
            continue;
        }

        texture_pool.push_back(texture_id);
        available_textures.push(texture_id);
    }

    Debug::Log("MediaBackgroundExtractor: Texture pool initialized with " +
               std::to_string(texture_pool.size()) + " textures");
}

void MediaBackgroundExtractor::DestroyTexturePool() {
    std::lock_guard<std::mutex> lock(texture_pool_mutex);

    Debug::Log("MediaBackgroundExtractor: Destroying texture pool");

    // Delete all textures in the pool
    for (GLuint texture_id : texture_pool) {
        if (texture_id != 0) {
            glDeleteTextures(1, &texture_id);
        }
    }

    texture_pool.clear();
    available_textures = std::queue<GLuint>(); // Clear queue
}

GLuint MediaBackgroundExtractor::AcquireTexture() {
    std::lock_guard<std::mutex> lock(texture_pool_mutex);

    if (!available_textures.empty()) {
        GLuint texture_id = available_textures.front();
        available_textures.pop();
        return texture_id;
    }

    // Pool exhausted - create new texture on demand
    GLuint texture_id;
    glGenTextures(1, &texture_id);

    if (texture_id != 0) {
        glBindTexture(GL_TEXTURE_2D, texture_id);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);

        Debug::Log("MediaBackgroundExtractor: Texture pool exhausted, created new texture " +
                   std::to_string(texture_id));
    }

    return texture_id;
}

void MediaBackgroundExtractor::ReleaseTexture(GLuint texture_id) {
    if (texture_id == 0) return;

    std::lock_guard<std::mutex> lock(texture_pool_mutex);

    // Check if this texture belongs to our pool
    auto it = std::find(texture_pool.begin(), texture_pool.end(), texture_id);
    if (it != texture_pool.end()) {
        // Return to available pool for reuse
        available_textures.push(texture_id);
    } else {
        // Not from our pool - delete it
        glDeleteTextures(1, &texture_id);
    }
}

bool MediaBackgroundExtractor::IsFrameAlreadyCached(int frame_number) const {
    if (!parent_cache) return false;
    return parent_cache->IsFrameCached(frame_number);
}

// Note: IsVideoCacheComplete() removed - not needed for window-based caching

// ============================================================================
// State Management
// ============================================================================

void MediaBackgroundExtractor::SetState(ExtractorState new_state) {
    ExtractorState old_state = current_state.exchange(new_state);
    if (old_state != new_state) {
        // State changed - notify all worker threads
        worker_cv.notify_all();

        // Log state transition for debugging
        const char* state_names[] = {"STOPPED", "EXTRACTING", "PAUSED_PLAYBACK", "PAUSED_REPOSITION", "PAUSED_MANUAL"};
        Debug::Log("MediaBackgroundExtractor: State " + std::string(state_names[static_cast<int>(old_state)]) +
                   " -> " + std::string(state_names[static_cast<int>(new_state)]));
    }
}

bool MediaBackgroundExtractor::ShouldExtract() const {
    ExtractorState state = current_state.load();

    // Only extract when in EXTRACTING state
    if (state != ExtractorState::EXTRACTING) {
        return false;
    }

    // Note: Timeline repositioning pause logic removed - simplified to state-only check
    return true;
}

// Note: ShouldPauseForReposition() removed - timeline repositioning logic simplified

// Note: NotifyTimelineReposition() removed - window updates automatically handle seeking

void MediaBackgroundExtractor::RequestWindowAroundPlayhead(double center_timestamp) {
    // Check if we should skip this request due to little movement
    static double last_center = -1.0;
    if (std::abs(center_timestamp - last_center) < 0.2) {
        return; // Less than 0.2 second movement - skip for timeline responsiveness
    }
    last_center = center_timestamp;

    double fps = frame_rate.load();
    int center_frame = static_cast<int>(center_timestamp * fps);

    Debug::Log("MediaBackgroundExtractor: Starting RAM-bounded spiral from frame " + std::to_string(center_frame));

    // Determine spiral limits for videos
    int max_spiral_distance = 1000; // Default limit for videos

    // Simple spiral until RAM limit reached
    for (int dist = 0; dist < max_spiral_distance; ++dist) {
        // Check global RAM before each frame
        if (!CanRequestMoreFrames()) {
            Debug::Log("MediaBackgroundExtractor: Stopping spiral at distance " + std::to_string(dist) + " - RAM limit reached");
            break;
        }

        int priority = (std::max)(1, 1000 - dist);  // Higher priority closer to playhead

        // Frame before center
        int before_frame = center_frame - dist;
        if (before_frame >= 0) {
            RequestFrame(before_frame, before_frame / fps, priority);
        }

        // Frame after center (skip dist=0 to avoid duplicate)
        if (dist > 0) {
            int after_frame = center_frame + dist;
            RequestFrame(after_frame, after_frame / fps, priority);
        }
    }
}

// Note: CalculateWindowSize removed - using RAM-bounded spiral instead

void MediaBackgroundExtractor::ClearPendingRequests() {
    std::lock_guard<std::mutex> lock(queue_mutex);
    while (!request_queue.empty()) {
        request_queue.pop();
    }
    requested_frames.clear();  // Clear tracking set too
}

void MediaBackgroundExtractor::ForceWindowRefresh() {
    double current_time = current_playhead_position.load();

    // Bypass the position change threshold by slightly adjusting timestamp
    // This ensures RequestWindowAroundPlayhead actually runs
    RequestWindowAroundPlayhead(current_time + 0.25);
    RequestWindowAroundPlayhead(current_time);

    Debug::Log("MediaBackgroundExtractor: Forced window refresh at " +
               std::to_string(current_time) + "s");
}

void MediaBackgroundExtractor::NotifyPlaybackState(bool is_playing) {
    ExtractorState current = current_state.load();

    if (is_playing && config.pause_during_playback) {
        // Pause for playback - only if currently extracting or paused for reposition
        if (current == ExtractorState::EXTRACTING || current == ExtractorState::PAUSED_REPOSITION) {
            SetState(ExtractorState::PAUSED_PLAYBACK);
            Debug::Log("MediaBackgroundExtractor: Playback started - pausing extraction");
        }
    } else if (!is_playing) {
        // Resume from playback pause - only if currently paused for playback AND playback has stopped
        if (current == ExtractorState::PAUSED_PLAYBACK) {
            SetState(ExtractorState::EXTRACTING);
            Debug::Log("MediaBackgroundExtractor: Playback stopped - resuming extraction");
        }
    }

    // Debug: Log extraction activity when video should be paused
    if (!is_playing && config.pause_during_playback) {
        static auto last_pause_log = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        auto time_since_last = std::chrono::duration_cast<std::chrono::seconds>(now - last_pause_log);

        if (time_since_last.count() >= 10) { // Log every 10 seconds when paused
            ExtractorState state = current_state.load();
            const char* state_names[] = {"STOPPED", "EXTRACTING", "PAUSED_PLAYBACK", "PAUSED_REPOSITION", "PAUSED_MANUAL"};
            Debug::Log("MediaBackgroundExtractor: Video paused, extractor state: " +
                      std::string(state_names[static_cast<int>(state)]));
            last_pause_log = now;
        }
    }
}

void MediaBackgroundExtractor::CleanupHardwareContext() {
    if (sws_context) {
        sws_freeContext(sws_context);
        sws_context = nullptr;
    }

    if (hw_device_ctx) {
        av_buffer_unref(&hw_device_ctx);
        hw_device_ctx = nullptr;
    }

    current_hw_mode = HardwareDecodeMode::SOFTWARE_ONLY;
    current_hw_decoder_name = "None";
}

void MediaBackgroundExtractor::UpdateStats(const ExtractionResult& result) {
    std::lock_guard<std::mutex> lock(stats_mutex);

    if (result.success) {
        stats.total_frames_extracted++;

        // Update timing stats
        auto now = std::chrono::steady_clock::now();
        if (extraction_start_time != std::chrono::steady_clock::time_point{}) {
            double elapsed_seconds = std::chrono::duration<double>(now - extraction_start_time).count();
            stats.frames_per_second = stats.total_frames_extracted / elapsed_seconds;
        }
    } else {
        stats.failed_extractions++;
    }
}

MediaBackgroundExtractor::ExtractorStats MediaBackgroundExtractor::GetStats() const {
    std::lock_guard<std::mutex> lock(stats_mutex);
    ExtractorStats current_stats = stats;
    current_stats.current_hardware_decoder = current_hw_decoder_name;
    current_stats.is_hardware_accelerated = (current_hw_mode != HardwareDecodeMode::SOFTWARE_ONLY);

    {
        std::lock_guard<std::mutex> queue_lock(queue_mutex);
        current_stats.pending_requests = request_queue.size();
    }

    return current_stats;
}

void MediaBackgroundExtractor::PauseExtraction() {
    ExtractorState current = current_state.load();
    if (current == ExtractorState::EXTRACTING) {
        SetState(ExtractorState::PAUSED_MANUAL);
        Debug::Log("MediaBackgroundExtractor: Extraction paused");
    }
}

void MediaBackgroundExtractor::ResumeExtraction() {
    ExtractorState current = current_state.load();
    if (current == ExtractorState::PAUSED_MANUAL) {
        SetState(ExtractorState::EXTRACTING);
        Debug::Log("MediaBackgroundExtractor: Extraction resumed");
    }
}

void MediaBackgroundExtractor::SetPlayheadPosition(double timestamp) {
    double old_position = current_playhead_position.exchange(timestamp);

    // If playhead moved significantly, update window
    double position_change = std::abs(timestamp - old_position);
    if (position_change > 0.1) { // More than 0.1 second movement for responsive timeline
        RequestWindowAroundPlayhead(timestamp);
        //Debug::Log("MediaBackgroundExtractor: Updated window for playhead position " +
        //          std::to_string(timestamp) + "s");
    }
}

void MediaBackgroundExtractor::UpdateHardwareConfig(const HardwareDecodeConfig& new_config) {
    config.hw_config = new_config;

    // If initialized, we would need to restart to apply hardware changes
    if (initialized.load()) {
        Debug::Log("MediaBackgroundExtractor: Hardware config updated - restart required for changes to take effect");
    }
}

void MediaBackgroundExtractor::RequestFrameRange(int start_frame, int end_frame, int base_priority) {
    for (int frame = start_frame; frame <= end_frame; ++frame) {
        if (frame >= 0) {
            double fps = frame_rate.load();
            double timestamp = frame / fps;
            int priority = (std::max)(1, base_priority - abs(frame - start_frame));
            RequestFrame(frame, timestamp, priority);
        }
    }
}

int MediaBackgroundExtractor::CalculateFramePriority(int frame_number, double timestamp) const {
    double playhead = current_playhead_position.load();
    double distance = std::abs(timestamp - playhead);

    // Priority decreases with distance from playhead
    // 1000 = immediate (0 seconds), 1 = far (10+ seconds)
    int priority = static_cast<int>(1000.0 * std::exp(-distance / 5.0));
    return (std::max)(1, priority);
}

std::vector<MediaBackgroundExtractor::GPUInfo> MediaBackgroundExtractor::EnumerateGPUs() {
    std::vector<GPUInfo> gpus;

    // Simplified GPU enumeration - Windows DXGI calls would go here
    // For now, provide basic GPU options without DXGI dependencies

    // Add NVIDIA option (if available)
    GPUInfo nvidia_info;
    nvidia_info.index = 0;
    nvidia_info.name = "NVIDIA GPU (NVDEC)";
    nvidia_info.is_nvidia = true;
    nvidia_info.supports_d3d11va = true;
    nvidia_info.supports_nvdec = true;
    gpus.push_back(nvidia_info);

    // Add generic D3D11VA option
    GPUInfo d3d11va_info;
    d3d11va_info.index = 1;
    d3d11va_info.name = "Hardware Decode (D3D11VA)";
    d3d11va_info.is_nvidia = false;
    d3d11va_info.supports_d3d11va = true;
    d3d11va_info.supports_nvdec = false;
    gpus.push_back(d3d11va_info);

    // Always add a software fallback option
    GPUInfo software_info;
    software_info.index = -1;
    software_info.name = "Software (CPU)";
    software_info.is_nvidia = false;
    software_info.supports_d3d11va = false;
    software_info.supports_nvdec = false;
    gpus.push_back(software_info);

    return gpus;
}

void MediaBackgroundExtractor::ProcessCompletedFrames() {
    std::vector<ExtractionResult> results_to_process;

    // Debug: Log when ProcessCompletedFrames is called
    static int call_count = 0;
    if (++call_count % 300 == 0) { // Log every 300 calls (~5 seconds at 60fps)
        //Debug::Log("ProcessCompletedFrames: Called " + std::to_string(call_count) + " times");
    }

    // Non-blocking approach: Use try_lock to avoid blocking the main thread
    if (results_mutex.try_lock()) {
        // Got the lock - quickly grab a limited number of results
        const size_t MAX_FRAMES_PER_CALL = 2; // Process max 2 frames per render loop
        size_t processed = 0;

        while (!completed_results.empty() && processed < MAX_FRAMES_PER_CALL) {
            results_to_process.push_back(std::move(completed_results.front()));
            completed_results.pop();
            processed++;
        }

        // Debug: Log when we actually have results to process
        if (!results_to_process.empty()) {
            //Debug::Log("ProcessCompletedFrames: Processing " + std::to_string(results_to_process.size()) + " completed frames");
        }

        results_mutex.unlock();
    }
    // If we can't get the lock, skip this frame - background threads are busy

    // Process results on main thread (with proper OpenGL context)
    for (const auto& result : results_to_process) {
        if (result.success && parent_cache) {
            // Add extracted frame to parent cache with pixel data
            parent_cache->AddExtractedFrame(result.frame_number, result.timestamp,
                                           result.pixel_data, result.width, result.height,
                                           result.from_native_image);
        }

        // Remove from requested set regardless of success/failure
        // Add to extracted frames set if successful for timeline visualization
        {
            std::lock_guard<std::mutex> lock(queue_mutex);
            requested_frames.erase(result.frame_number);
            if (result.success) {
                extracted_frames.insert(result.frame_number);
                //Debug::Log("ProcessCompletedFrames: Added frame " + std::to_string(result.frame_number) + " to extracted_frames (total: " + std::to_string(extracted_frames.size()) + ")");
            }
        }

        if (result.success && parent_cache) {
            // Throttle success logging to reduce spam
            static auto last_success_log = std::chrono::steady_clock::now();
            static int success_count = 0;
            success_count++;

            auto now = std::chrono::steady_clock::now();
            auto time_since_last = std::chrono::duration_cast<std::chrono::seconds>(now - last_success_log);

            if (time_since_last.count() >= 5) { // Log only every 5 seconds
                Debug::Log("MediaBackgroundExtractor: Successfully processed " + std::to_string(success_count) +
                          " frames (latest: frame " + std::to_string(result.frame_number) +
                          " at " + std::to_string(result.timestamp) + "s)");
                last_success_log = now;
                success_count = 0;
            }
        } else if (!result.success) {
            Debug::Log("MediaBackgroundExtractor: Failed to extract frame " +
                      std::to_string(result.frame_number) + ": " + result.error_message);
        }
    }
}

// ============================================================================
// WorkerContext Implementation (Thread-safe FFmpeg contexts)
// ============================================================================

void MediaBackgroundExtractor::WorkerContext::Cleanup() {
    if (codec_context) {
        avcodec_free_context(&codec_context);
    }
    if (format_context) {
        avformat_close_input(&format_context);
    }
    if (hw_device_ctx) {
        av_buffer_unref(&hw_device_ctx);
    }
    if (sws_context) {
        sws_freeContext(sws_context);
        sws_context = nullptr;
    }
    initialized = false;
}

bool MediaBackgroundExtractor::WorkerContext::Initialize(const std::string& video_path, const HardwareDecodeConfig& hw_config) {
    // Open video file
    format_context = avformat_alloc_context();
    if (avformat_open_input(&format_context, video_path.c_str(), nullptr, nullptr) < 0) {
        Debug::Log("WorkerContext: Failed to open video file: " + video_path);
        return false;
    }

    if (avformat_find_stream_info(format_context, nullptr) < 0) {
        Debug::Log("WorkerContext: Failed to find stream info");
        avformat_close_input(&format_context);
        return false;
    }

    // Find video stream
    video_stream_index = av_find_best_stream(format_context, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (video_stream_index < 0) {
        Debug::Log("WorkerContext: No video stream found");
        avformat_close_input(&format_context);
        return false;
    }

    AVStream* video_stream = format_context->streams[video_stream_index];

    // Setup decoder (always software for thread safety)
    const AVCodec* decoder = avcodec_find_decoder(video_stream->codecpar->codec_id);
    if (!decoder) {
        Debug::Log("WorkerContext: No suitable decoder found");
        return false;
    }

    codec_context = avcodec_alloc_context3(decoder);
    if (avcodec_parameters_to_context(codec_context, video_stream->codecpar) < 0) {
        Debug::Log("WorkerContext: Failed to copy codec parameters");
        return false;
    }

    // Configure for frame extraction performance
    codec_context->thread_count = 1;  // Single threaded for safety
    codec_context->thread_type = FF_THREAD_SLICE;

    if (avcodec_open2(codec_context, decoder, nullptr) < 0) {
        Debug::Log("WorkerContext: Failed to open codec");
        return false;
    }

    initialized = true;
    Debug::Log("WorkerContext: Successfully initialized for " + video_path);
    return true;
}

bool MediaBackgroundExtractor::CanRequestMoreFrames() const {
    if (!parent_cache) return false;

    // Removed: Memory-based throttling (memory-based eviction removed)
    // Always allow extraction requests - time-based eviction will handle cache management
    return true;
}

std::vector<MediaBackgroundExtractor::CacheSegment> MediaBackgroundExtractor::GetCacheSegments() const {
    std::vector<CacheSegment> segments;

    // Get copy of extracted frames to avoid holding lock too long
    std::set<int> frames_copy;
    {
        std::lock_guard<std::mutex> lock(queue_mutex);
        frames_copy = extracted_frames;
    }

    // Debug: Log extracted frames count
    static size_t last_frames_count = 0;
    if (frames_copy.size() != last_frames_count) {
        //Debug::Log("BackgroundExtractor: GetCacheSegments called, " + std::to_string(frames_copy.size()) + " extracted frames");
        last_frames_count = frames_copy.size();
    }

    if (frames_copy.empty()) {
        return segments;
    }

    double fps = frame_rate.load();
    if (fps <= 0) {
        return segments;
    }

    // Group consecutive frames into segments
    auto it = frames_copy.begin();
    while (it != frames_copy.end()) {
        CacheSegment segment;
        segment.type = CacheSegment::BACKGROUND_CACHED;

        // Start new segment
        int start_frame = *it;
        int current_frame = start_frame;

        // Find consecutive frames
        while (it != frames_copy.end() && *it <= current_frame + 2) { // Allow small gaps (2 frames)
            current_frame = *it;
            ++it;
        }

        // Convert frame numbers to timestamps
        segment.start_time = start_frame / fps;
        segment.end_time = current_frame / fps;

        segments.push_back(segment);
    }

    return segments;
}

void MediaBackgroundExtractor::RemoveFrameFromTracking(int frame_number) {
    std::lock_guard<std::mutex> lock(queue_mutex);

    size_t removed = extracted_frames.erase(frame_number);
    if (removed > 0) {
        Debug::Log("MediaBackgroundExtractor: Removed frame " + std::to_string(frame_number) +
                   " from tracking (total: " + std::to_string(extracted_frames.size()) + ")");
    }
}

// ============================================================================
// ConversionStrategy Methods - Conditional 4444 Color Matrix Support
// ============================================================================

void MediaBackgroundExtractor::SetConversionStrategy(const ConversionStrategy& strategy) {
    conversion_strategy = std::make_unique<ConversionStrategy>(strategy);
    has_conversion_strategy = true;

    if (strategy.ShouldApplyColorMatrix()) {
        //Debug::Log("MediaBackgroundExtractor: Set 4444 color matrix strategy - " + strategy.GetDescription());
    } else {
        //Debug::Log("MediaBackgroundExtractor: Set standard processing strategy - " + strategy.GetDescription());
    }
}

void MediaBackgroundExtractor::UpdateVideoMetadata(const VideoMetadata& metadata) {
    if (!metadata.is_loaded) {
        Debug::Log("MediaBackgroundExtractor: UpdateVideoMetadata called with unloaded metadata");
        return;
    }

    // Create and set conversion strategy from the updated metadata
    auto strategy = ConversionStrategy::FromMetadata(metadata);
    SetConversionStrategy(strategy);
    Debug::Log("MediaBackgroundExtractor: Updated conversion strategy from metadata - " + strategy.GetDescription());
}

