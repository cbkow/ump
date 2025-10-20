#include "video_transcoder.h"
#include "../player/image_loaders.h"
#include "../player/media_background_extractor.h"  // For ConversionStrategy
#include "../metadata/video_metadata.h"
#include "../utils/debug_utils.h"
#include <filesystem>
#include <algorithm>
#include <cmath>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}

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

// ============================================================================
// Helper: Check if audio codec is compatible with output container
// ============================================================================
static bool IsAudioCodecCompatible(const std::string& audio_codec, const std::string& video_codec) {
    // MP4 container (H.264/H.265) has strict audio codec requirements
    if (video_codec == "libx264" || video_codec == "libx265") {
        // Compatible codecs for MP4
        if (audio_codec == "aac" ||
            audio_codec == "mp3" ||
            audio_codec == "ac3" ||
            audio_codec == "eac3" ||
            audio_codec == "mp2") {
            return true;
        }
        // Incompatible codecs (PCM, FLAC, etc.) need re-encoding
        return false;
    }

    // Other containers (MOV, MKV) accept most codecs
    return true;
}

// ============================================================================
// Helper: Extract metadata from video file using FFmpeg
// ============================================================================
static VideoMetadata ExtractVideoMetadata(const std::string& video_path) {
    VideoMetadata metadata;

    AVFormatContext* format_ctx = nullptr;

    // Open video file
    if (avformat_open_input(&format_ctx, video_path.c_str(), nullptr, nullptr) < 0) {
        Debug::Log("ExtractVideoMetadata: Failed to open video file: " + video_path);
        return metadata;
    }

    // Read stream info
    if (avformat_find_stream_info(format_ctx, nullptr) < 0) {
        Debug::Log("ExtractVideoMetadata: Failed to find stream info");
        avformat_close_input(&format_ctx);
        return metadata;
    }

    // Find video stream
    int video_stream_index = -1;
    for (unsigned int i = 0; i < format_ctx->nb_streams; i++) {
        if (format_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_index = i;
            break;
        }
    }

    if (video_stream_index < 0) {
        Debug::Log("ExtractVideoMetadata: No video stream found");
        avformat_close_input(&format_ctx);
        return metadata;
    }

    AVStream* video_stream = format_ctx->streams[video_stream_index];
    AVCodecParameters* codecpar = video_stream->codecpar;

    // Basic file info
    metadata.PopulateBasicFileInfo(video_path);

    // Video properties
    metadata.width = codecpar->width;
    metadata.height = codecpar->height;

    // Frame rate
    if (video_stream->avg_frame_rate.den != 0) {
        metadata.frame_rate = static_cast<double>(video_stream->avg_frame_rate.num) /
                             video_stream->avg_frame_rate.den;
    }

    // Codec name
    const AVCodec* codec = avcodec_find_decoder(codecpar->codec_id);
    if (codec) {
        metadata.video_codec = codec->name;
    }

    // Pixel format
    metadata.pixel_format = av_get_pix_fmt_name(static_cast<AVPixelFormat>(codecpar->format));

    // Colorspace
    switch (codecpar->color_space) {
        case AVCOL_SPC_BT709:
            metadata.colorspace = "bt709";
            break;
        case AVCOL_SPC_BT470BG:
        case AVCOL_SPC_SMPTE170M:
            metadata.colorspace = "bt601";
            break;
        case AVCOL_SPC_BT2020_NCL:
        case AVCOL_SPC_BT2020_CL:
            metadata.colorspace = "bt2020nc";
            break;
        default:
            metadata.colorspace = "unknown";
            break;
    }

    // Color range
    switch (codecpar->color_range) {
        case AVCOL_RANGE_JPEG:
            metadata.range_type = "full";
            break;
        case AVCOL_RANGE_MPEG:
            metadata.range_type = "limited";
            break;
        default:
            metadata.range_type = "unknown";
            break;
    }

    metadata.is_loaded = true;

    Debug::Log("ExtractVideoMetadata: Successfully extracted metadata");
    Debug::Log("  Codec: " + metadata.video_codec);
    Debug::Log("  Pixel Format: " + std::string(metadata.pixel_format));
    Debug::Log("  Colorspace: " + metadata.colorspace);
    Debug::Log("  Range: " + metadata.range_type);
    Debug::Log("  Resolution: " + std::to_string(metadata.width) + "x" + std::to_string(metadata.height));

    // Cleanup
    avformat_close_input(&format_ctx);

    return metadata;
}

void VideoTranscoder::TranscodeThread(TranscodeConfig config, ProgressCallback callback) {
    auto start_time = std::chrono::steady_clock::now();

    Progress progress;

    // Audio support: source format context for audio packet reading
    AVFormatContext* source_format_ctx = nullptr;
    std::vector<int> audio_stream_indices;

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
            auto video_loader = std::make_unique<VideoImageLoader>(
                config.input_video_path,
                config.fps,
                config.video_duration
            );
            Debug::Log("VideoTranscoder: Created VideoImageLoader for: " + config.input_video_path);

            // ================================================================
            // NEW: Extract metadata and set conversion strategy for color handling
            // This ensures transcoded videos have correct colors matching viewport
            // ================================================================
            VideoMetadata metadata = ExtractVideoMetadata(config.input_video_path);
            if (metadata.is_loaded) {
                auto strategy = ConversionStrategy::FromMetadata(metadata);
                video_loader->SetConversionStrategy(strategy);
                Debug::Log("VideoTranscoder: Conversion strategy set - " + strategy.GetDescription());
            } else {
                Debug::Log("VideoTranscoder: WARNING - Failed to extract metadata, colors may be incorrect");
            }

            loader = std::move(video_loader);
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
            // Use floor to avoid trying to decode partial frames at the end
            // Subtract small epsilon to handle floating point rounding errors
            int total_frames = static_cast<int>(std::floor(config.video_duration * config.fps - 0.01));
            frame_paths.reserve(total_frames);
            for (int i = 0; i < total_frames; ++i) {
                frame_paths.push_back(std::to_string(i));
            }
            Debug::Log("VideoTranscoder: Generated " + std::to_string(frame_paths.size()) + " frame indices for video (duration=" +
                       std::to_string(config.video_duration) + "s, fps=" + std::to_string(config.fps) + ")");
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
        // NEW: STEP 5.5: Add Audio Streams (if source is video and copy_audio is enabled)
        // ===================================================================
        if (config.input_mode == InputMode::VIDEO_FILE && config.encoder_settings.copy_audio) {
            progress.current_status_text = "Detecting audio streams...";
            UpdateProgress(progress);
            if (callback) callback(progress);

            // Open source video file to detect audio streams
            if (avformat_open_input(&source_format_ctx, config.input_video_path.c_str(), nullptr, nullptr) == 0) {
                if (avformat_find_stream_info(source_format_ctx, nullptr) >= 0) {
                    // Find all audio streams
                    for (unsigned int i = 0; i < source_format_ctx->nb_streams; i++) {
                        if (source_format_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                            audio_stream_indices.push_back(i);
                        }
                    }

                    if (audio_stream_indices.empty()) {
                        Debug::Log("VideoTranscoder: No audio streams found in source video");
                    } else {
                        Debug::Log("VideoTranscoder: Found " + std::to_string(audio_stream_indices.size()) + " audio stream(s)");

                        // Add audio streams to encoder (with smart codec compatibility)
                        for (int audio_idx : audio_stream_indices) {
                            AVStream* audio_stream = source_format_ctx->streams[audio_idx];
                            const AVCodec* audio_codec = avcodec_find_decoder(audio_stream->codecpar->codec_id);
                            std::string audio_codec_name = audio_codec ? audio_codec->name : "unknown";

                            // Check if audio codec is compatible with output container
                            bool compatible = IsAudioCodecCompatible(audio_codec_name, config.encoder_settings.codec);

                            if (compatible) {
                                // Stream copy mode (fast, lossless)
                                Debug::Log("VideoTranscoder: Audio codec '" + audio_codec_name + "' is compatible - using stream copy");
                                if (!encoder_->AddAudioStream(source_format_ctx, audio_idx)) {
                                    Debug::Log("WARNING: Failed to add audio stream #" + std::to_string(audio_idx));
                                }
                            } else {
                                // Re-encode mode (convert to AAC)
                                Debug::Log("VideoTranscoder: Audio codec '" + audio_codec_name + "' is incompatible - re-encoding to AAC 192kbps");
                                if (!encoder_->AddAudioStreamWithEncoding(source_format_ctx, audio_idx, "aac", 192)) {
                                    Debug::Log("WARNING: Failed to add audio stream #" + std::to_string(audio_idx) + " with encoding");
                                }
                            }
                        }
                    }
                } else {
                    Debug::Log("WARNING: Failed to find stream info for audio detection");
                }
            } else {
                Debug::Log("WARNING: Failed to open source file for audio detection");
            }
        }

        // ===================================================================
        // STEP 5.6: Write Header (must be called after adding all streams)
        // ===================================================================
        progress.current_status_text = "Writing file header...";
        UpdateProgress(progress);
        if (callback) callback(progress);

        if (!encoder_->WriteHeader()) {
            throw std::runtime_error("Failed to write file header");
        }

        // ===================================================================
        // STEP 6: Encode All Frames
        // ===================================================================
        int max_frame_index;
        if (config.input_mode == InputMode::VIDEO_FILE) {
            // Use same calculation as frame path generation to avoid off-by-one errors
            max_frame_index = static_cast<int>(std::floor(config.video_duration * config.fps - 0.01)) - 1;
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
        // NEW: STEP 6.5: Copy Audio Packets (if audio streams exist)
        // ===================================================================
        if (source_format_ctx && !audio_stream_indices.empty()) {
            progress.current_status_text = "Copying audio streams...";
            UpdateProgress(progress);
            if (callback) callback(progress);

            Debug::Log("VideoTranscoder: Copying audio packets from source");

            // Seek back to beginning of source file
            av_seek_frame(source_format_ctx, -1, 0, AVSEEK_FLAG_BACKWARD);

            // Read and copy all audio packets
            AVPacket* packet = av_packet_alloc();
            int audio_packet_count = 0;

            while (av_read_frame(source_format_ctx, packet) >= 0) {
                if (cancel_requested_) {
                    av_packet_free(&packet);
                    throw std::runtime_error("Cancelled by user");
                }

                // Check if this is an audio stream we're copying
                bool is_audio_stream = false;
                for (int audio_idx : audio_stream_indices) {
                    if (packet->stream_index == audio_idx) {
                        is_audio_stream = true;
                        break;
                    }
                }

                if (is_audio_stream) {
                    // Write audio packet (encoder handles timestamp rescaling and interleaving)
                    if (!encoder_->WriteAudioPacket(packet, packet->stream_index)) {
                        Debug::Log("WARNING: Failed to write audio packet");
                    }
                    audio_packet_count++;
                }

                av_packet_unref(packet);
            }

            av_packet_free(&packet);

            Debug::Log("VideoTranscoder: Copied " + std::to_string(audio_packet_count) + " audio packets");
        }

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

    // Cleanup (handles both success and error cases)
    if (source_format_ctx) {
        avformat_close_input(&source_format_ctx);
        source_format_ctx = nullptr;
    }

    encoder_.reset();
    frame_loader_.reset();
    color_transform_.reset();

    is_transcoding_ = false;
    Debug::Log("VideoTranscoder: Thread finished");
}

} // namespace ump
