#include "transcode_worker_pool.h"
#include "../utils/debug_utils.h"
#include <algorithm>

namespace ump {

TranscodeWorkerPool::TranscodeWorkerPool(TranscodeQueue* queue, int worker_count)
    : queue_(queue)
    , worker_count_(std::clamp(worker_count, 1, 8))
{
    Debug::Log("TranscodeWorkerPool: Created with " + std::to_string(worker_count_) + " workers");

    // Initialize worker info
    worker_info_.resize(worker_count_);
    for (int i = 0; i < worker_count_; ++i) {
        worker_info_[i].worker_id = i;
        worker_info_[i].is_active = false;
    }
}

TranscodeWorkerPool::~TranscodeWorkerPool() {
    Stop();
}

void TranscodeWorkerPool::Start() {
    if (status_ == Status::ACTIVE) {
        Debug::Log("WARNING: TranscodeWorkerPool::Start - Already started");
        return;
    }

    Debug::Log("TranscodeWorkerPool: Starting worker pool");

    should_stop_ = false;
    is_paused_ = false;
    status_ = Status::ACTIVE;

    // Launch worker threads
    worker_threads_.clear();
    for (int i = 0; i < worker_count_; ++i) {
        worker_threads_.emplace_back(&TranscodeWorkerPool::WorkerThread, this, i);
    }

    Debug::Log("TranscodeWorkerPool: Started " + std::to_string(worker_count_) + " worker threads");
}

void TranscodeWorkerPool::Stop() {
    if (status_ == Status::STOPPED) {
        return;
    }

    Debug::Log("TranscodeWorkerPool: Stopping worker pool");

    should_stop_ = true;
    is_paused_ = false;
    status_ = Status::STOPPED;

    // Wake all workers
    worker_cv_.notify_all();

    // Join all threads
    for (auto& thread : worker_threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }

    worker_threads_.clear();

    Debug::Log("TranscodeWorkerPool: Stopped");
}

void TranscodeWorkerPool::Pause() {
    if (status_ != Status::ACTIVE) {
        Debug::Log("WARNING: TranscodeWorkerPool::Pause - Not active");
        return;
    }

    Debug::Log("TranscodeWorkerPool: Pausing");

    is_paused_ = true;
    status_ = Status::PAUSED;
}

void TranscodeWorkerPool::Resume() {
    if (status_ != Status::PAUSED) {
        Debug::Log("WARNING: TranscodeWorkerPool::Resume - Not paused");
        return;
    }

    Debug::Log("TranscodeWorkerPool: Resuming");

    is_paused_ = false;
    status_ = Status::ACTIVE;

    // Wake workers
    worker_cv_.notify_all();
}

void TranscodeWorkerPool::SetWorkerCount(int count) {
    int new_count = std::clamp(count, 1, 8);

    if (new_count == worker_count_) {
        return;
    }

    Debug::Log("TranscodeWorkerPool: Changing worker count from " +
               std::to_string(worker_count_) + " to " + std::to_string(new_count));

    bool was_active = (status_ == Status::ACTIVE);

    // Stop existing workers
    if (status_ != Status::STOPPED) {
        Stop();
    }

    // Update count
    worker_count_ = new_count;

    // Resize worker info
    worker_info_.resize(worker_count_);
    for (int i = 0; i < worker_count_; ++i) {
        worker_info_[i].worker_id = i;
        worker_info_[i].is_active = false;
    }

    // Restart if was active
    if (was_active) {
        Start();
    }
}

std::vector<TranscodeWorkerPool::WorkerInfo> TranscodeWorkerPool::GetWorkerInfo() const {
    std::lock_guard<std::mutex> lock(info_mutex_);
    return worker_info_;
}

void TranscodeWorkerPool::WorkerThread(int worker_id) {
    Debug::Log("Worker " + std::to_string(worker_id) + ": Started");

    WorkerInfo info;
    info.worker_id = worker_id;
    info.is_active = false;

    while (!should_stop_.load()) {
        // Wait if paused
        if (is_paused_.load()) {
            std::unique_lock<std::mutex> lock(worker_mutex_);
            worker_cv_.wait(lock, [this] { return !is_paused_.load() || should_stop_.load(); });

            if (should_stop_.load()) break;
        }

        // Get next job from queue
        TranscodeJob* job = queue_->GetNextJob();

        if (!job) {
            // No jobs available, wait a bit
            info.is_active = false;
            info.current_job_id.clear();
            info.current_job_name.clear();
            info.status_text = "Idle";
            UpdateWorkerInfo(worker_id, info);

            std::unique_lock<std::mutex> lock(worker_mutex_);
            worker_cv_.wait_for(lock, std::chrono::seconds(1), [this] { return should_stop_.load(); });
            continue;
        }

        // Start job
        info.is_active = true;
        info.current_job_id = job->GetJobID();
        info.current_job_name = job->GetJobName();
        info.status_text = "Starting...";
        info.progress_percent = 0.0;
        info.encoding_fps = 0.0;
        UpdateWorkerInfo(worker_id, info);

        Debug::Log("Worker " + std::to_string(worker_id) + ": Starting job " + job->GetJobName());

        bool started = queue_->StartJob(job->GetJobID());

        if (!started) {
            Debug::Log("ERROR: Worker " + std::to_string(worker_id) + ": Failed to start job");
            info.is_active = false;
            UpdateWorkerInfo(worker_id, info);
            continue;
        }

        // Poll job progress until complete
        while (!should_stop_.load() && !is_paused_.load()) {
            auto progress = job->GetProgress();

            info.progress_percent = progress.progress_percent;
            info.encoding_fps = progress.encoding_fps;
            info.status_text = progress.current_status_text;
            UpdateWorkerInfo(worker_id, info);

            // Check if job finished
            if (progress.is_complete || progress.is_error) {
                break;
            }

            // Check job status
            auto status = job->GetStatus();
            if (status == TranscodeJob::Status::COMPLETED ||
                status == TranscodeJob::Status::FAILED ||
                status == TranscodeJob::Status::CANCELLED) {
                break;
            }

            // Sleep briefly
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        // Job finished
        Debug::Log("Worker " + std::to_string(worker_id) + ": Finished job " +
                   job->GetJobName() + " - " + job->GetStatusString());

        info.is_active = false;
        info.current_job_id.clear();
        info.current_job_name.clear();
        info.progress_percent = 0.0;
        info.encoding_fps = 0.0;
        info.status_text = "Idle";
        UpdateWorkerInfo(worker_id, info);

        // Brief pause before next job
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    Debug::Log("Worker " + std::to_string(worker_id) + ": Stopped");
}

void TranscodeWorkerPool::UpdateWorkerInfo(int worker_id, const WorkerInfo& info) {
    std::lock_guard<std::mutex> lock(info_mutex_);
    if (worker_id >= 0 && worker_id < static_cast<int>(worker_info_.size())) {
        worker_info_[worker_id] = info;
    }
}

TranscodeWorkerPool::WorkerInfo TranscodeWorkerPool::GetWorkerInfoForID(int worker_id) const {
    std::lock_guard<std::mutex> lock(info_mutex_);
    if (worker_id >= 0 && worker_id < static_cast<int>(worker_info_.size())) {
        return worker_info_[worker_id];
    }
    return WorkerInfo();
}

} // namespace ump
