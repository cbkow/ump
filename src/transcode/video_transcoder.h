#pragma once

#include <string>
#include <vector>
#include <memory>
#include <thread>
#include <atomic>
#include <mutex>
#include <functional>
#include <chrono>

#include "sequential_frame_loader.h"
#include "ocio_cpu_transform.h"
#include "ffmpeg_video_encoder.h"
#include "../player/pipeline_mode.h"

namespace ump {

/**
 * VideoTranscoder
 *
 * Orchestrates the entire transcode pipeline:
 *  1. Sequential frame loading (via SequentialFrameLoader)
 *  2. OCIO color transformation (via OCIOCPUTransform)
 *  3. FFMPEG encoding (via FFMPEGVideoEncoder)
 *
 * Runs in background thread with progress callbacks.
 */
class VideoTranscoder {
public:
    enum class InputMode {
        IMAGE_SEQUENCE,  // Multiple image files (existing behavior)
        VIDEO_FILE       // Single video file (decode frames on the fly)
    };

    struct TranscodeConfig {
        // Input mode
        InputMode input_mode = InputMode::IMAGE_SEQUENCE;

        // Input - Image sequence mode
        std::vector<std::string> input_files;

        // Input - Video file mode
        std::string input_video_path;  // Path to video file
        double video_duration = 0.0;   // Video duration in seconds (for frame count calculation)

        // Input - Audio for image sequences
        std::string audio_file;        // Optional audio file to mux with image sequence output

        // Common input settings
        double fps = 24.0;
        int start_frame = 0;
        int end_frame = -1;  // -1 = all frames
        PipelineMode pipeline_mode = PipelineMode::NORMAL;  // Auto-detected precision level
        std::string exr_layer;  // EXR layer name (empty for non-EXR or default layer)

        // OCIO
        std::string src_colorspace;
        std::string display;
        std::string view;
        std::string looks;
        std::vector<std::string> scene_luts;
        std::vector<std::string> display_luts;

        // Output
        std::string output_path;
        FFMPEGVideoEncoder::EncoderSettings encoder_settings;

        // Scaling (optional)
        int output_width = -1;   // -1 = source resolution
        int output_height = -1;
    };

    struct Progress {
        int current_frame = 0;
        int total_frames = 0;
        double progress_percent = 0.0;  // 0.0 - 100.0

        double elapsed_seconds = 0.0;
        double estimated_remaining_seconds = 0.0;
        double encoding_fps = 0.0;  // Processing speed

        std::string status;  // "Loading", "Encoding", "Complete", "Error"
        std::string current_status_text;  // "Encoding frame 145/500"
        std::string error_message;

        bool is_complete = false;
        bool is_error = false;
    };

    using ProgressCallback = std::function<void(const Progress&)>;

    VideoTranscoder();
    ~VideoTranscoder();

    /**
     * Start transcode in background thread
     *
     * @param config Transcode configuration
     * @param callback Progress callback (called periodically from worker thread)
     * @return true if started successfully
     */
    bool StartTranscode(const TranscodeConfig& config, ProgressCallback callback);

    /**
     * Cancel ongoing transcode
     *
     * Safe to call from any thread.
     */
    void CancelTranscode();

    /**
     * Check if transcode is currently running
     */
    bool IsTranscoding() const { return is_transcoding_; }

    /**
     * Get current progress (thread-safe)
     */
    Progress GetProgress() const;

private:
    // Background transcode thread
    void TranscodeThread(TranscodeConfig config, ProgressCallback callback);

    // Update progress (thread-safe)
    void UpdateProgress(const Progress& progress);

    // Components
    std::unique_ptr<SequentialFrameLoader> frame_loader_;
    std::unique_ptr<OCIOCPUTransform> color_transform_;
    std::unique_ptr<FFMPEGVideoEncoder> encoder_;

    // Worker thread
    std::thread worker_thread_;
    std::atomic<bool> is_transcoding_{false};
    std::atomic<bool> cancel_requested_{false};

    // Progress tracking
    Progress progress_;
    mutable std::mutex progress_mutex_;
};

} // namespace ump
