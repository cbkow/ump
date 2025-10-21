#include "transcode_job.h"
#include "../utils/debug_utils.h"
#include <nlohmann/json.hpp>
#include <random>
#include <sstream>
#include <iomanip>
#include <algorithm>

using json = nlohmann::json;

namespace ump {

TranscodeJob::TranscodeJob(const Config& config)
    : config_(config)
    , created_time_(std::chrono::system_clock::now())
{
    transcoder_ = std::make_unique<VideoTranscoder>();

    // Generate job ID if not provided
    if (config_.job_id.empty()) {
        config_.job_id = GenerateJobID();
    }

    Debug::Log("TranscodeJob created: " + config_.job_name + " (ID: " + config_.job_id + ")");
}

TranscodeJob::~TranscodeJob() {
    if (status_ == Status::ENCODING) {
        Cancel();
    }
}

bool TranscodeJob::Start() {
    if (status_ != Status::QUEUED && status_ != Status::PAUSED) {
        Debug::Log("ERROR: TranscodeJob::Start - Invalid state: " + GetStatusString());
        return false;
    }

    Debug::Log("TranscodeJob: Starting job " + config_.job_name);

    UpdateStatus(Status::ENCODING);
    started_time_ = std::chrono::system_clock::now();
    pause_requested_ = false;

    // Start transcode with progress callback
    auto callback = [this](const VideoTranscoder::Progress& progress) {
        ProgressCallback(progress);
    };

    bool started = transcoder_->StartTranscode(config_.transcode_config, callback);

    if (!started) {
        UpdateStatus(Status::FAILED);
        error_message_ = "Failed to start transcode";
        return false;
    }

    return true;
}

void TranscodeJob::Pause() {
    if (status_ != Status::ENCODING) {
        Debug::Log("WARNING: TranscodeJob::Pause - Not encoding");
        return;
    }

    Debug::Log("TranscodeJob: Pausing job " + config_.job_name);
    pause_requested_ = true;
    transcoder_->CancelTranscode();
    UpdateStatus(Status::PAUSED);
}

void TranscodeJob::Resume() {
    if (status_ != Status::PAUSED) {
        Debug::Log("WARNING: TranscodeJob::Resume - Not paused");
        return;
    }

    Debug::Log("TranscodeJob: Resuming job " + config_.job_name);

    // Update config to resume from where we left off
    int last_frame = current_progress_.current_frame;
    config_.transcode_config.start_frame = last_frame;

    Start();
}

void TranscodeJob::Cancel() {
    if (status_ != Status::ENCODING && status_ != Status::PAUSED) {
        Debug::Log("WARNING: TranscodeJob::Cancel - Not active");
        return;
    }

    Debug::Log("TranscodeJob: Cancelling job " + config_.job_name);
    transcoder_->CancelTranscode();
    UpdateStatus(Status::CANCELLED);
    completed_time_ = std::chrono::system_clock::now();
}

VideoTranscoder::Progress TranscodeJob::GetProgress() const {
    std::lock_guard<std::mutex> lock(progress_mutex_);
    return current_progress_;
}

double TranscodeJob::GetProgressPercent() const {
    std::lock_guard<std::mutex> lock(progress_mutex_);
    return current_progress_.progress_percent;
}

std::string TranscodeJob::GetStatusString() const {
    switch (status_) {
        case Status::QUEUED: return "Queued";
        case Status::ENCODING: return "Encoding";
        case Status::PAUSED: return "Paused";
        case Status::COMPLETED: return "Completed";
        case Status::FAILED: return "Failed";
        case Status::CANCELLED: return "Cancelled";
        default: return "Unknown";
    }
}

bool TranscodeJob::AreDependenciesMet(const std::vector<std::string>& completed_job_ids) const {
    if (!HasDependencies()) {
        return true;
    }

    // Check if all dependencies are in completed list
    for (const auto& dep_id : config_.dependency_job_ids) {
        if (std::find(completed_job_ids.begin(), completed_job_ids.end(), dep_id) == completed_job_ids.end()) {
            return false;
        }
    }

    return true;
}

void TranscodeJob::ProgressCallback(const VideoTranscoder::Progress& progress) {
    {
        std::lock_guard<std::mutex> lock(progress_mutex_);
        current_progress_ = progress;
    }

    // Check for completion or error
    if (progress.is_complete) {
        UpdateStatus(Status::COMPLETED);
        completed_time_ = std::chrono::system_clock::now();
        Debug::Log("TranscodeJob: Completed successfully - " + config_.job_name);

        // Call metadata write callback if provided
        if (config_.metadata_write_callback && !config_.source_file_path.empty()) {
            Debug::Log("TranscodeJob: Calling metadata write callback (offset=" + std::to_string(config_.timecode_offset_frames) + " frames)");
            config_.metadata_write_callback(config_.source_file_path, config_.transcode_config.output_path, config_.timecode_offset_frames);
        }
    } else if (progress.is_error) {
        UpdateStatus(Status::FAILED);
        completed_time_ = std::chrono::system_clock::now();
        error_message_ = progress.error_message;
        Debug::Log("ERROR: TranscodeJob failed - " + config_.job_name + ": " + error_message_);
    }
}

void TranscodeJob::UpdateStatus(Status new_status) {
    status_ = new_status;
}

std::string TranscodeJob::GenerateJobID() {
    // Generate UUID-like job ID: timestamp + random hex
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dis(0, 0xFFFFFFFF);

    std::stringstream ss;
    ss << std::hex << std::setfill('0')
       << std::setw(12) << ms
       << "-"
       << std::setw(8) << dis(gen);

    return ss.str();
}

// ============================================================================
// JSON Serialization
// ============================================================================

json TranscodeJob::ToJSON() const {
    json j;

    // Job metadata
    j["job_id"] = config_.job_id;
    j["job_name"] = config_.job_name;
    j["priority"] = static_cast<int>(config_.priority);
    j["status"] = static_cast<int>(status_);

    // Dependencies
    j["dependencies"] = config_.dependency_job_ids;

    // Transcode config
    auto& tc = config_.transcode_config;
    j["transcode_config"] = {
        {"input_files", tc.input_files},
        {"fps", tc.fps},
        {"start_frame", tc.start_frame},
        {"end_frame", tc.end_frame},
        {"src_colorspace", tc.src_colorspace},
        {"display", tc.display},
        {"view", tc.view},
        {"looks", tc.looks},
        {"scene_luts", tc.scene_luts},
        {"display_luts", tc.display_luts},
        {"output_path", tc.output_path},
        {"output_width", tc.output_width},
        {"output_height", tc.output_height}
    };

    // Encoder settings
    auto& es = tc.encoder_settings;
    j["encoder_settings"] = {
        {"codec", es.codec},
        {"crf", es.crf},
        {"preset", es.preset},
        {"pixel_format", es.pixel_format},
        {"bitrate_kbps", es.bitrate_kbps},
        {"profile", es.profile},
        {"gop_size", es.gop_size},
        {"max_b_frames", es.max_b_frames}
    };

    // Timestamps
    j["created_time"] = std::chrono::duration_cast<std::chrono::seconds>(
        created_time_.time_since_epoch()).count();

    if (started_time_.time_since_epoch().count() > 0) {
        j["started_time"] = std::chrono::duration_cast<std::chrono::seconds>(
            started_time_.time_since_epoch()).count();
    }

    if (completed_time_.time_since_epoch().count() > 0) {
        j["completed_time"] = std::chrono::duration_cast<std::chrono::seconds>(
            completed_time_.time_since_epoch()).count();
    }

    // Progress
    std::lock_guard<std::mutex> lock(progress_mutex_);
    j["progress"] = {
        {"current_frame", current_progress_.current_frame},
        {"total_frames", current_progress_.total_frames},
        {"progress_percent", current_progress_.progress_percent}
    };

    if (!error_message_.empty()) {
        j["error_message"] = error_message_;
    }

    return j;
}

std::unique_ptr<TranscodeJob> TranscodeJob::FromJSON(const json& j) {
    Config config;

    // Job metadata
    config.job_id = j["job_id"].get<std::string>();
    config.job_name = j["job_name"].get<std::string>();
    config.priority = static_cast<Priority>(j["priority"].get<int>());

    // Dependencies
    if (j.contains("dependencies")) {
        config.dependency_job_ids = j["dependencies"].get<std::vector<std::string>>();
    }

    // Transcode config
    auto& tc_json = j["transcode_config"];
    auto& tc = config.transcode_config;

    tc.input_files = tc_json["input_files"].get<std::vector<std::string>>();
    tc.fps = tc_json["fps"].get<double>();
    tc.start_frame = tc_json["start_frame"].get<int>();
    tc.end_frame = tc_json["end_frame"].get<int>();
    tc.src_colorspace = tc_json["src_colorspace"].get<std::string>();
    tc.display = tc_json["display"].get<std::string>();
    tc.view = tc_json["view"].get<std::string>();
    tc.looks = tc_json["looks"].get<std::string>();
    tc.scene_luts = tc_json["scene_luts"].get<std::vector<std::string>>();
    tc.display_luts = tc_json["display_luts"].get<std::vector<std::string>>();
    tc.output_path = tc_json["output_path"].get<std::string>();
    tc.output_width = tc_json["output_width"].get<int>();
    tc.output_height = tc_json["output_height"].get<int>();

    // Encoder settings
    auto& es_json = j["encoder_settings"];
    auto& es = tc.encoder_settings;

    es.codec = es_json["codec"].get<std::string>();
    es.crf = es_json["crf"].get<int>();
    es.preset = es_json["preset"].get<std::string>();
    es.pixel_format = es_json["pixel_format"].get<std::string>();
    es.bitrate_kbps = es_json["bitrate_kbps"].get<int>();
    es.profile = es_json["profile"].get<std::string>();
    es.gop_size = es_json["gop_size"].get<int>();
    es.max_b_frames = es_json["max_b_frames"].get<int>();

    // Create job
    auto job = std::make_unique<TranscodeJob>(config);

    // Restore status (but reset ENCODING to QUEUED)
    Status status = static_cast<Status>(j["status"].get<int>());
    if (status == Status::ENCODING) {
        status = Status::QUEUED;  // Reset interrupted jobs
    }
    job->status_ = status;

    // Restore timestamps
    if (j.contains("created_time")) {
        job->created_time_ = std::chrono::system_clock::time_point(
            std::chrono::seconds(j["created_time"].get<int64_t>()));
    }

    if (j.contains("started_time")) {
        job->started_time_ = std::chrono::system_clock::time_point(
            std::chrono::seconds(j["started_time"].get<int64_t>()));
    }

    if (j.contains("completed_time")) {
        job->completed_time_ = std::chrono::system_clock::time_point(
            std::chrono::seconds(j["completed_time"].get<int64_t>()));
    }

    // Restore progress
    if (j.contains("progress")) {
        auto& prog = j["progress"];
        job->current_progress_.current_frame = prog["current_frame"].get<int>();
        job->current_progress_.total_frames = prog["total_frames"].get<int>();
        job->current_progress_.progress_percent = prog["progress_percent"].get<double>();
    }

    if (j.contains("error_message")) {
        job->error_message_ = j["error_message"].get<std::string>();
    }

    return job;
}

} // namespace ump
