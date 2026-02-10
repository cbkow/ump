#pragma once

#include <string>
#include <vector>
#include <memory>
#include <chrono>
#include <atomic>
#include <mutex>
#include <functional>
#include <nlohmann/json.hpp>
#include "video_transcoder.h"

namespace ump {

/**
 * TranscodeJob
 *
 * Represents a single transcode job in the queue.
 * Wraps VideoTranscoder with additional metadata for queue management.
 */
class TranscodeJob {
public:
    enum class Status {
        QUEUED,          // Waiting to start
        GENERATING_LUT,  // Generating OCIO LUT for video (yellow row)
        ENCODING,        // Currently being processed (green row)
        PAUSED,          // Paused by user
        COMPLETED,       // Finished successfully
        FAILED,          // Encountered error
        CANCELLED        // Cancelled by user
    };

    /**
     * OCIO settings snapshot (captured at job creation time).
     * Used for LUT-based video encoding to ensure consistent color transform.
     */
    struct OCIOSnapshot {
        bool enabled = false;
        std::string src_colorspace;
        std::string display;
        std::string view;
        std::string looks;
        std::vector<std::string> scene_luts;
        std::vector<std::string> display_luts;
    };

    enum class Priority {
        LOW = 0,
        NORMAL = 1,
        HIGH = 2,
        URGENT = 3
    };

    struct Config {
        // Job metadata
        std::string job_name;
        std::string job_id;  // Unique identifier (generated)
        Priority priority = Priority::NORMAL;

        // Transcode configuration
        VideoTranscoder::TranscodeConfig transcode_config;

        // Source file path (for metadata preservation)
        std::string source_file_path;

        // Trim offset for timecode adjustment (in frames, 0 = no trim)
        int timecode_offset_frames = 0;

        // Metadata write callback (called after successful transcode)
        std::function<void(const std::string& source_path, const std::string& output_path, int offset_frames)> metadata_write_callback;

        // Dependencies (optional)
        std::vector<std::string> dependency_job_ids;  // Wait for these jobs to complete

        // OCIO snapshot (captured at job creation for LUT-based encoding)
        OCIOSnapshot ocio_snapshot;

        // Path to generated/cached LUT file (set during encoding for video files)
        std::string lut_path;
    };

    explicit TranscodeJob(const Config& config);
    ~TranscodeJob();

    // Job control
    bool Start();
    void Pause();
    void Resume();
    void Cancel();

    // Getters
    const std::string& GetJobID() const { return config_.job_id; }
    const std::string& GetJobName() const { return config_.job_name; }
    const std::string& GetOutputPath() const { return config_.transcode_config.output_path; }
    Status GetStatus() const { return status_; }
    Priority GetPriority() const { return config_.priority; }
    const Config& GetConfig() const { return config_; }

    // Progress
    VideoTranscoder::Progress GetProgress() const;
    double GetProgressPercent() const;
    std::string GetStatusString() const;

    // Dependencies
    bool HasDependencies() const { return !config_.dependency_job_ids.empty(); }
    const std::vector<std::string>& GetDependencies() const { return config_.dependency_job_ids; }
    bool AreDependenciesMet(const std::vector<std::string>& completed_job_ids) const;

    // Timestamps
    std::chrono::system_clock::time_point GetCreatedTime() const { return created_time_; }
    std::chrono::system_clock::time_point GetStartedTime() const { return started_time_; }
    std::chrono::system_clock::time_point GetCompletedTime() const { return completed_time_; }

    // Serialization
    nlohmann::json ToJSON() const;
    static std::unique_ptr<TranscodeJob> FromJSON(const nlohmann::json& j);

    // Generate unique ID
    static std::string GenerateJobID();

private:
    void ProgressCallback(const VideoTranscoder::Progress& progress);
    void UpdateStatus(Status new_status);

    Config config_;
    Status status_ = Status::QUEUED;

    std::unique_ptr<VideoTranscoder> transcoder_;
    VideoTranscoder::Progress current_progress_;

    std::chrono::system_clock::time_point created_time_;
    std::chrono::system_clock::time_point started_time_;
    std::chrono::system_clock::time_point completed_time_;

    std::string error_message_;

    mutable std::mutex progress_mutex_;
    std::atomic<bool> pause_requested_{false};
};

} // namespace ump
