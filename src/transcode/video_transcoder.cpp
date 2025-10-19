#include "video_transcoder.h"
#include "../player/image_loaders.h"
#include "../utils/debug_utils.h"
#include <filesystem>
#include <algorithm>

#undef min
#undef max

namespace ump {

VideoTranscoder::VideoTranscoder() {
}

VideoTranscoder::~VideoTranscoder() {
    CancelTranscode();

    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
}

bool VideoTranscoder::StartTranscode(const TranscodeConfig& config, ProgressCallback callback) {
    if (is_transcoding_) {
        Debug::Log("ERROR: VideoTranscoder::StartTranscode - Already transcoding");
        return false;
    }

    // Validate input based on mode
    if (config.input_mode == InputMode::IMAGE_SEQUENCE) {
        if (config.input_files.empty()) {
            Debug::Log("ERROR: VideoTranscoder::StartTranscode - Empty input file list");
            return false;
        }
    } else if (config.input_mode == InputMode::VIDEO_FILE) {
        if (config.input_video_path.empty()) {
            Debug::Log("ERROR: VideoTranscoder::StartTranscode - Empty video path");
            return false;
        }
        if (config.video_duration <= 0.0) {
            Debug::Log("ERROR: VideoTranscoder::StartTranscode - Invalid video duration");
            return false;
        }
    }

    if (config.fps <= 0.0) {
        Debug::Log("ERROR: VideoTranscoder::StartTranscode - Invalid frame rate");
        return false;
    }

    Debug::Log("VideoTranscoder: Starting transcode");
    if (config.input_mode == InputMode::IMAGE_SEQUENCE) {
        Debug::Log("  Input mode: Image sequence");
        Debug::Log("  Input files: " + std::to_string(config.input_files.size()));
    } else {
        Debug::Log("  Input mode: Video file");
        Debug::Log("  Input video: " + config.input_video_path);
        Debug::Log("  Video duration: " + std::to_string(config.video_duration) + " seconds");
    }
    Debug::Log("  Frame rate: " + std::to_string(config.fps) + " fps");
    Debug::Log("  Frame range: " + std::to_string(config.start_frame) + " to " +
               (config.end_frame >= 0 ? std::to_string(config.end_frame) : "END"));
    Debug::Log("  Output: " + config.output_path);
    Debug::Log("  Codec: " + config.encoder_settings.codec);

    // Reset state
    cancel_requested_ = false;
    is_transcoding_ = true;

    Progress initial_progress;
    initial_progress.status = "Initializing";
    initial_progress.current_status_text = "Starting transcode...";
    UpdateProgress(initial_progress);

    // Start worker thread
    worker_thread_ = std::thread(&VideoTranscoder::TranscodeThread, this, config, callback);

    return true;
}

void VideoTranscoder::CancelTranscode() {
    if (is_transcoding_) {
        Debug::Log("VideoTranscoder: Cancelling transcode");
        cancel_requested_ = true;

        // Cancel frame loader if active
        if (frame_loader_) {
            frame_loader_->Cancel();
        }
    }
}

VideoTranscoder::Progress VideoTranscoder::GetProgress() const {
    std::lock_guard<std::mutex> lock(progress_mutex_);
    return progress_;
}

void VideoTranscoder::UpdateProgress(const Progress& progress) {
    std::lock_guard<std::mutex> lock(progress_mutex_);
    progress_ = progress;
}

void VideoTranscoder::TranscodeThread(TranscodeConfig config, ProgressCallback callback) {
    auto start_time = std::chrono::steady_clock::now();

    Progress progress;

    try {
        // ===================================================================
        // STEP 1: Initialize OCIO Transform (if needed)
        // ===================================================================
        progress.status = "Initializing";

        // Check if OCIO transform is requested
        bool use_ocio = !config.src_colorspace.empty() || !config.display.empty() || !config.view.empty();

        if (use_ocio) {
            progress.current_status_text = "Initializing OCIO color transform...";
            UpdateProgress(progress);
            if (callback) callback(progress);

            color_transform_ = std::make_unique<OCIOCPUTransform>();
            if (!color_transform_->Initialize(config.src_colorspace, config.display,
                                             config.view, config.looks,
                                             config.scene_luts, config.display_luts)) {
                throw std::runtime_error("Failed to initialize OCIO transform");
            }

            Debug::Log("VideoTranscoder: OCIO transform initialized");
        } else {
            // Create passthrough transform (no color conversion)
            color_transform_ = std::make_unique<OCIOCPUTransform>();
            Debug::Log("VideoTranscoder: Using passthrough (no OCIO transform)");
        }

        // ===================================================================
        // STEP 2: Create Image Loader
        // ===================================================================
        progress.current_status_text = "Creating image loader...";
        UpdateProgress(progress);
        if (callback) callback(progress);

        std::unique_ptr<IImageLoader> loader;

        if (config.input_mode == InputMode::VIDEO_FILE) {
            // Video file mode - use VideoImageLoader
            loader = std::make_unique<VideoImageLoader>(
                config.input_video_path,
                config.fps,
                config.video_duration
            );
            Debug::Log("VideoTranscoder: Created VideoImageLoader for: " + config.input_video_path);
        } else {
            // Image sequence mode - detect format from first file
            std::filesystem::path first_file(config.input_files[0]);
            std::string ext = first_file.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

            if (ext == ".exr") {
                loader = std::make_unique<EXRImageLoader>();
            } else if (ext == ".tif" || ext == ".tiff") {
                loader = std::make_unique<TIFFImageLoader>();
            } else if (ext == ".png") {
                loader = std::make_unique<PNGImageLoader>();
            } else if (ext == ".jpg" || ext == ".jpeg") {
                loader = std::make_unique<JPEGImageLoader>();
            } else {
                throw std::runtime_error("Unsupported image format: " + ext);
            }

            Debug::Log("VideoTranscoder: Created loader for format: " + ext);
        }

        // ===================================================================
        // STEP 3: Initialize Sequential Frame Loader
        // ===================================================================
        progress.current_status_text = "Initializing frame loader...";
        UpdateProgress(progress);
        if (callback) callback(progress);

        // For video mode, generate "paths" as frame numbers ("0", "1", "2", ...)
        // VideoImageLoader interprets these as frame indices
        std::vector<std::string> frame_paths;
        if (config.input_mode == InputMode::VIDEO_FILE) {
            int total_frames = static_cast<int>(config.video_duration * config.fps);
            frame_paths.reserve(total_frames);
            for (int i = 0; i < total_frames; ++i) {
                frame_paths.push_back(std::to_string(i));
            }
            Debug::Log("VideoTranscoder: Generated " + std::to_string(frame_paths.size()) + " frame indices for video");
        } else {
            frame_paths = config.input_files;
        }

        frame_loader_ = std::make_unique<SequentialFrameLoader>(
            std::move(loader),
            frame_paths,
            16,  // 16-frame buffer for better performance
            config.pipeline_mode,
            config.exr_layer
        );

        Debug::Log("VideoTranscoder: Frame loader initialized");

        // ===================================================================
        // STEP 4: Load First Frame to Determine Resolution
        // ===================================================================
        progress.current_status_text = "Loading first frame...";
        UpdateProgress(progress);
        if (callback) callback(progress);

        // Prefetch first frame
        frame_loader_->PrefetchAhead(0);

        auto first_frame = frame_loader_->GetFrame(0);
        if (!first_frame) {
            throw std::runtime_error("Failed to load first frame");
        }

        int width = config.output_width > 0 ? config.output_width : first_frame->width;
        int height = config.output_height > 0 ? config.output_height : first_frame->height;

        Debug::Log("VideoTranscoder: Source resolution: " +
                   std::to_string(first_frame->width) + "x" + std::to_string(first_frame->height));
        Debug::Log("VideoTranscoder: Output resolution: " +
                   std::to_string(width) + "x" + std::to_string(height));

        // ===================================================================
        // STEP 5: Initialize Encoder
        // ===================================================================
        progress.current_status_text = "Initializing video encoder...";
        UpdateProgress(progress);
        if (callback) callback(progress);

        config.encoder_settings.width = width;
        config.encoder_settings.height = height;
        config.encoder_settings.fps = config.fps;
        config.encoder_settings.output_path = config.output_path;

        encoder_ = std::make_unique<FFMPEGVideoEncoder>();
        if (!encoder_->Open(config.encoder_settings)) {
            throw std::runtime_error("Failed to open video encoder");
        }

        Debug::Log("VideoTranscoder: Encoder opened");

        // ===================================================================
        // STEP 6: Encode All Frames
        // ===================================================================
        int max_frame_index;
        if (config.input_mode == InputMode::VIDEO_FILE) {
            max_frame_index = static_cast<int>(config.video_duration * config.fps) - 1;
        } else {
            max_frame_index = static_cast<int>(config.input_files.size()) - 1;
        }

        int end_frame = config.end_frame >= 0 ?
                       std::min(config.end_frame, max_frame_index) :
                       max_frame_index;

        int total_frames = end_frame - config.start_frame + 1;

        progress.total_frames = total_frames;
        progress.status = "Encoding";

        Debug::Log("VideoTranscoder: Encoding " + std::to_string(total_frames) + " frames " +
                   "(" + std::to_string(config.start_frame) + " to " + std::to_string(end_frame) + ")");

        for (int i = config.start_frame; i <= end_frame; ++i) {
            if (cancel_requested_) {
                throw std::runtime_error("Cancelled by user");
            }

            // Update progress
            progress.current_frame = i - config.start_frame;
            progress.progress_percent = (progress.current_frame * 100.0) / total_frames;
            progress.current_status_text = "Encoding frame " + std::to_string(progress.current_frame + 1) +
                                          " / " + std::to_string(total_frames);

            auto now = std::chrono::steady_clock::now();
            progress.elapsed_seconds = std::chrono::duration<double>(now - start_time).count();

            if (progress.current_frame > 0) {
                progress.encoding_fps = progress.current_frame / progress.elapsed_seconds;
                int remaining_frames = total_frames - progress.current_frame;
                progress.estimated_remaining_seconds = remaining_frames / progress.encoding_fps;
            }

            UpdateProgress(progress);
            if (callback && (progress.current_frame % 10 == 0 || progress.current_frame == 0)) {
                // Call callback every 10 frames to avoid overhead
                callback(progress);
            }

            // Prefetch next frames
            if (i + 4 <= end_frame) {
                frame_loader_->PrefetchAhead(i);
            }

            // Load frame
            auto pixel_data = frame_loader_->GetFrame(i);
            if (!pixel_data) {
                throw std::runtime_error("Failed to load frame " + std::to_string(i));
            }

            // Apply OCIO transform and convert to RGBA8
            auto transformed = color_transform_->ApplyToUInt8(*pixel_data, width, height);

            // Encode frame
            if (!encoder_->EncodeFrame(transformed.data(), width * 4)) {
                throw std::runtime_error("Failed to encode frame " + std::to_string(i));
            }
        }

        Debug::Log("VideoTranscoder: All frames encoded successfully (" + std::to_string(total_frames) + " frames)");

        // Update progress to 100% before finalizing (fixes 99% stuck issue)
        progress.current_frame = total_frames;
        progress.progress_percent = 100.0;
        progress.current_status_text = "Finalizing video file...";
        UpdateProgress(progress);
        if (callback) callback(progress);

        // ===================================================================
        // STEP 7: Finalize
        // ===================================================================
        UpdateProgress(progress);
        if (callback) callback(progress);

        encoder_->Close();

        // Calculate final stats
        auto end_time = std::chrono::steady_clock::now();
        progress.elapsed_seconds = std::chrono::duration<double>(end_time - start_time).count();
        progress.encoding_fps = total_frames / progress.elapsed_seconds;

        progress.status = "Complete";
        progress.current_status_text = "Transcode completed successfully";
        progress.current_frame = total_frames;
        progress.progress_percent = 100.0;
        progress.estimated_remaining_seconds = 0.0;
        progress.is_complete = true;

        UpdateProgress(progress);
        if (callback) callback(progress);

        Debug::Log("VideoTranscoder: Completed successfully");
        Debug::Log("  Total time: " + std::to_string(progress.elapsed_seconds) + " seconds");
        Debug::Log("  Average speed: " + std::to_string(progress.encoding_fps) + " fps");

    } catch (const std::exception& e) {
        Debug::Log("ERROR: VideoTranscoder exception: " + std::string(e.what()));

        progress.status = "Error";
        progress.current_status_text = "Transcode failed";
        progress.error_message = e.what();
        progress.is_error = true;
        progress.is_complete = false;

        UpdateProgress(progress);
        if (callback) callback(progress);
    }

    // Cleanup
    encoder_.reset();
    frame_loader_.reset();
    color_transform_.reset();

    is_transcoding_ = false;
    Debug::Log("VideoTranscoder: Thread finished");
}

} // namespace ump
