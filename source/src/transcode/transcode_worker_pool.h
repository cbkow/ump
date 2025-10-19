#pragma once

#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include "transcode_queue.h"

namespace ump {

/**
 * TranscodeWorkerPool
 *
 * Manages a pool of worker threads that process jobs from TranscodeQueue.
 *
 * Features:
 * - Configurable worker count (1-8)
 * - Automatic job assignment
 * - Pause/Resume all workers
 * - Worker status tracking
 * - Graceful shutdown
 */
class TranscodeWorkerPool {
public:
    enum class Status {
        IDLE,      // No jobs in queue, workers waiting
        ACTIVE,    // Workers processing jobs
        PAUSED,    // Workers paused by user
        STOPPED    // Pool stopped
    };

    struct WorkerInfo {
        int worker_id = -1;
        bool is_active = false;
        std::string current_job_id;
        std::string current_job_name;
        double progress_percent = 0.0;
        double encoding_fps = 0.0;
        std::string status_text;
    };

    explicit TranscodeWorkerPool(TranscodeQueue* queue, int worker_count = 2);
    ~TranscodeWorkerPool();

    // Worker pool control
    void Start();
    void Stop();
    void Pause();
    void Resume();

    // Worker count management
    void SetWorkerCount(int count);  // Safe to call while running
    int GetWorkerCount() const { return worker_count_; }

    // Status
    Status GetStatus() const { return status_; }
    std::vector<WorkerInfo> GetWorkerInfo() const;
    bool IsActive() const { return status_ == Status::ACTIVE; }
    bool IsPaused() const { return status_ == Status::PAUSED; }

private:
    void WorkerThread(int worker_id);
    void UpdateWorkerInfo(int worker_id, const WorkerInfo& info);
    WorkerInfo GetWorkerInfoForID(int worker_id) const;

    TranscodeQueue* queue_;  // Not owned
    int worker_count_ = 2;

    std::vector<std::thread> worker_threads_;
    std::vector<WorkerInfo> worker_info_;

    Status status_ = Status::STOPPED;

    std::atomic<bool> should_stop_{false};
    std::atomic<bool> is_paused_{false};

    mutable std::mutex worker_mutex_;
    std::condition_variable worker_cv_;

    mutable std::mutex info_mutex_;  // Protects worker_info_
};

} // namespace ump
