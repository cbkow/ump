#include "transcode_queue.h"
#include "../utils/debug_utils.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <algorithm>

// Undefine Windows.h macros that conflict with our methods
#ifdef AddJob
#undef AddJob
#endif
#ifdef GetJob
#undef GetJob
#endif

using json = nlohmann::json;

namespace qcview {

TranscodeQueue::TranscodeQueue() {
}

TranscodeQueue::~TranscodeQueue() {
    if (auto_save_enabled_ && !auto_save_path_.empty()) {
        SaveQueue(auto_save_path_);
    }
}

void TranscodeQueue::AddJob(std::unique_ptr<TranscodeJob> job) {
    std::lock_guard<std::mutex> lock(queue_mutex_);

    Debug::Log("TranscodeQueue: Adding job " + job->GetJobName() + " (ID: " + job->GetJobID() + ")");

    jobs_.push_back(std::move(job));
    SortByPriority();

    AutoSave();
}

bool TranscodeQueue::RemoveJob(const std::string& job_id) {
    std::lock_guard<std::mutex> lock(queue_mutex_);

    int index = FindJobIndex(job_id);
    if (index < 0) {
        Debug::Log("WARNING: TranscodeQueue::RemoveJob - Job not found: " + job_id);
        return false;
    }

    Debug::Log("TranscodeQueue: Removing job " + jobs_[index]->GetJobName());

    jobs_.erase(jobs_.begin() + index);

    AutoSave();
    return true;
}

TranscodeJob* TranscodeQueue::GetJob(const std::string& job_id) {
    std::lock_guard<std::mutex> lock(queue_mutex_);

    int index = FindJobIndex(job_id);
    if (index < 0) {
        return nullptr;
    }

    return jobs_[index].get();
}

std::vector<TranscodeJob*> TranscodeQueue::GetAllJobs() {
    std::lock_guard<std::mutex> lock(queue_mutex_);

    std::vector<TranscodeJob*> result;
    result.reserve(jobs_.size());

    for (auto& job : jobs_) {
        result.push_back(job.get());
    }

    return result;
}

TranscodeJob* TranscodeQueue::GetNextJob() {
    std::lock_guard<std::mutex> lock(queue_mutex_);

    // Get completed job IDs for dependency checking
    std::vector<std::string> completed_ids;
    for (const auto& job : jobs_) {
        if (job->GetStatus() == TranscodeJob::Status::COMPLETED) {
            completed_ids.push_back(job->GetJobID());
        }
    }

    // Find highest priority job that is queued and has dependencies met
    for (auto& job : jobs_) {
        if (job->GetStatus() == TranscodeJob::Status::QUEUED &&
            job->AreDependenciesMet(completed_ids)) {
            return job.get();
        }
    }

    return nullptr;
}

int TranscodeQueue::GetJobCount() const {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    return static_cast<int>(jobs_.size());
}

int TranscodeQueue::GetQueuedJobCount() const {
    std::lock_guard<std::mutex> lock(queue_mutex_);

    int count = 0;
    for (const auto& job : jobs_) {
        if (job->GetStatus() == TranscodeJob::Status::QUEUED) {
            count++;
        }
    }
    return count;
}

int TranscodeQueue::GetActiveJobCount() const {
    std::lock_guard<std::mutex> lock(queue_mutex_);

    int count = 0;
    for (const auto& job : jobs_) {
        if (job->GetStatus() == TranscodeJob::Status::ENCODING) {
            count++;
        }
    }
    return count;
}

int TranscodeQueue::GetCompletedJobCount() const {
    std::lock_guard<std::mutex> lock(queue_mutex_);

    int count = 0;
    for (const auto& job : jobs_) {
        if (job->GetStatus() == TranscodeJob::Status::COMPLETED) {
            count++;
        }
    }
    return count;
}

int TranscodeQueue::GetFailedJobCount() const {
    std::lock_guard<std::mutex> lock(queue_mutex_);

    int count = 0;
    for (const auto& job : jobs_) {
        if (job->GetStatus() == TranscodeJob::Status::FAILED) {
            count++;
        }
    }
    return count;
}

bool TranscodeQueue::StartJob(const std::string& job_id) {
    std::lock_guard<std::mutex> lock(queue_mutex_);

    int index = FindJobIndex(job_id);
    if (index < 0) {
        return false;
    }

    bool started = jobs_[index]->Start();

    if (started) {
        AutoSave();
    }

    return started;
}

bool TranscodeQueue::PauseJob(const std::string& job_id) {
    std::lock_guard<std::mutex> lock(queue_mutex_);

    int index = FindJobIndex(job_id);
    if (index < 0) {
        return false;
    }

    jobs_[index]->Pause();
    AutoSave();

    return true;
}

bool TranscodeQueue::ResumeJob(const std::string& job_id) {
    std::lock_guard<std::mutex> lock(queue_mutex_);

    int index = FindJobIndex(job_id);
    if (index < 0) {
        return false;
    }

    jobs_[index]->Resume();
    AutoSave();

    return true;
}

bool TranscodeQueue::CancelJob(const std::string& job_id) {
    std::lock_guard<std::mutex> lock(queue_mutex_);

    int index = FindJobIndex(job_id);
    if (index < 0) {
        return false;
    }

    jobs_[index]->Cancel();
    AutoSave();

    return true;
}

bool TranscodeQueue::MoveJobUp(const std::string& job_id) {
    std::lock_guard<std::mutex> lock(queue_mutex_);

    int index = FindJobIndex(job_id);
    if (index <= 0) {
        return false;  // Already at top or not found
    }

    std::swap(jobs_[index], jobs_[index - 1]);
    AutoSave();

    return true;
}

bool TranscodeQueue::MoveJobDown(const std::string& job_id) {
    std::lock_guard<std::mutex> lock(queue_mutex_);

    int index = FindJobIndex(job_id);
    if (index < 0 || index >= static_cast<int>(jobs_.size()) - 1) {
        return false;  // Already at bottom or not found
    }

    std::swap(jobs_[index], jobs_[index + 1]);
    AutoSave();

    return true;
}

bool TranscodeQueue::MoveJobToTop(const std::string& job_id) {
    std::lock_guard<std::mutex> lock(queue_mutex_);

    int index = FindJobIndex(job_id);
    if (index <= 0) {
        return false;  // Already at top or not found
    }

    auto job = std::move(jobs_[index]);
    jobs_.erase(jobs_.begin() + index);
    jobs_.insert(jobs_.begin(), std::move(job));

    AutoSave();
    return true;
}

bool TranscodeQueue::SetJobPriority(const std::string& job_id, TranscodeJob::Priority priority) {
    std::lock_guard<std::mutex> lock(queue_mutex_);

    int index = FindJobIndex(job_id);
    if (index < 0) {
        return false;
    }

    // Modify priority via const_cast (since we own the job)
    auto& config = const_cast<TranscodeJob::Config&>(jobs_[index]->GetConfig());
    config.priority = priority;

    SortByPriority();
    AutoSave();

    return true;
}

std::vector<TranscodeJob*> TranscodeQueue::GetJobsByStatus(TranscodeJob::Status status) {
    std::lock_guard<std::mutex> lock(queue_mutex_);

    std::vector<TranscodeJob*> result;
    for (auto& job : jobs_) {
        if (job->GetStatus() == status) {
            result.push_back(job.get());
        }
    }

    return result;
}

std::vector<std::string> TranscodeQueue::GetCompletedJobIDs() const {
    std::lock_guard<std::mutex> lock(queue_mutex_);

    std::vector<std::string> result;
    for (const auto& job : jobs_) {
        if (job->GetStatus() == TranscodeJob::Status::COMPLETED) {
            result.push_back(job->GetJobID());
        }
    }

    return result;
}

void TranscodeQueue::ClearCompleted() {
    std::lock_guard<std::mutex> lock(queue_mutex_);

    jobs_.erase(
        std::remove_if(jobs_.begin(), jobs_.end(), [](const std::unique_ptr<TranscodeJob>& job) {
            return job->GetStatus() == TranscodeJob::Status::COMPLETED;
        }),
        jobs_.end()
    );

    AutoSave();
    Debug::Log("TranscodeQueue: Cleared completed jobs");
}

void TranscodeQueue::ClearFailed() {
    std::lock_guard<std::mutex> lock(queue_mutex_);

    jobs_.erase(
        std::remove_if(jobs_.begin(), jobs_.end(), [](const std::unique_ptr<TranscodeJob>& job) {
            return job->GetStatus() == TranscodeJob::Status::FAILED;
        }),
        jobs_.end()
    );

    AutoSave();
    Debug::Log("TranscodeQueue: Cleared failed jobs");
}

void TranscodeQueue::ClearAll() {
    std::lock_guard<std::mutex> lock(queue_mutex_);

    jobs_.clear();

    AutoSave();
    Debug::Log("TranscodeQueue: Cleared all jobs");
}

TranscodeQueue::Stats TranscodeQueue::GetStats() const {
    std::lock_guard<std::mutex> lock(queue_mutex_);

    Stats stats;
    stats.total = static_cast<int>(jobs_.size());

    for (const auto& job : jobs_) {
        switch (job->GetStatus()) {
            case TranscodeJob::Status::QUEUED:
                stats.queued++;
                break;
            case TranscodeJob::Status::ENCODING:
                stats.encoding++;
                break;
            case TranscodeJob::Status::PAUSED:
                stats.paused++;
                break;
            case TranscodeJob::Status::COMPLETED:
                stats.completed++;
                break;
            case TranscodeJob::Status::FAILED:
                stats.failed++;
                break;
            case TranscodeJob::Status::CANCELLED:
                stats.cancelled++;
                break;
        }
    }

    return stats;
}

// ============================================================================
// Persistence
// ============================================================================

bool TranscodeQueue::SaveQueue(const std::string& filepath) {
    std::lock_guard<std::mutex> lock(queue_mutex_);

    try {
        json j;
        j["version"] = 1;
        j["jobs"] = json::array();

        for (const auto& job : jobs_) {
            j["jobs"].push_back(job->ToJSON());
        }

        std::ofstream file(filepath);
        if (!file.is_open()) {
            Debug::Log("ERROR: TranscodeQueue::SaveQueue - Could not open file: " + filepath);
            return false;
        }

        file << j.dump(2);  // Pretty print with 2-space indent

        Debug::Log("TranscodeQueue: Saved " + std::to_string(jobs_.size()) + " jobs to " + filepath);
        return true;

    } catch (const std::exception& e) {
        Debug::Log("ERROR: TranscodeQueue::SaveQueue - Exception: " + std::string(e.what()));
        return false;
    }
}

bool TranscodeQueue::LoadQueue(const std::string& filepath) {
    std::lock_guard<std::mutex> lock(queue_mutex_);

    try {
        std::ifstream file(filepath);
        if (!file.is_open()) {
            Debug::Log("TranscodeQueue::LoadQueue - File not found (creating new queue): " + filepath);
            return false;  // Not an error, just no existing queue
        }

        json j;
        file >> j;

        int version = j["version"].get<int>();
        if (version != 1) {
            Debug::Log("WARNING: TranscodeQueue::LoadQueue - Unsupported version: " + std::to_string(version));
            return false;
        }

        jobs_.clear();

        for (const auto& job_json : j["jobs"]) {
            auto job = TranscodeJob::FromJSON(job_json);
            if (job) {
                jobs_.push_back(std::move(job));
            }
        }

        Debug::Log("TranscodeQueue: Loaded " + std::to_string(jobs_.size()) + " jobs from " + filepath);
        return true;

    } catch (const std::exception& e) {
        Debug::Log("ERROR: TranscodeQueue::LoadQueue - Exception: " + std::string(e.what()));
        return false;
    }
}

void TranscodeQueue::SetAutoSavePath(const std::string& filepath) {
    auto_save_path_ = filepath;
    Debug::Log("TranscodeQueue: Auto-save path set to " + filepath);
}

void TranscodeQueue::AutoSave() {
    if (!auto_save_enabled_ || auto_save_path_.empty()) {
        return;
    }

    // Note: We're already holding queue_mutex_ when this is called
    // So we need to save without locking again

    try {
        json j;
        j["version"] = 1;
        j["jobs"] = json::array();

        for (const auto& job : jobs_) {
            j["jobs"].push_back(job->ToJSON());
        }

        std::ofstream file(auto_save_path_);
        if (file.is_open()) {
            file << j.dump(2);
        }

    } catch (const std::exception& e) {
        Debug::Log("WARNING: TranscodeQueue::AutoSave - Failed: " + std::string(e.what()));
    }
}

// ============================================================================
// Private Methods
// ============================================================================

void TranscodeQueue::SortByPriority() {
    std::sort(jobs_.begin(), jobs_.end(), [](const std::unique_ptr<TranscodeJob>& a,
                                             const std::unique_ptr<TranscodeJob>& b) {
        // Sort by priority (higher first), then by creation time (older first)
        if (a->GetPriority() != b->GetPriority()) {
            return static_cast<int>(a->GetPriority()) > static_cast<int>(b->GetPriority());
        }
        return a->GetCreatedTime() < b->GetCreatedTime();
    });
}

int TranscodeQueue::FindJobIndex(const std::string& job_id) const {
    for (size_t i = 0; i < jobs_.size(); ++i) {
        if (jobs_[i]->GetJobID() == job_id) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

} // namespace qcview
