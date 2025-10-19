#pragma once

#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <functional>
#include "transcode_job.h"

// Undefine Windows.h macros that conflict with our methods
#ifdef AddJob
#undef AddJob
#endif
#ifdef GetJob
#undef GetJob
#endif

namespace ump {

/**
 * TranscodeQueue
 *
 * Thread-safe persistent queue for managing transcode jobs.
 *
 * Features:
 * - Priority sorting
 * - Job dependency handling
 * - JSON persistence (auto-save)
 * - Status filtering
 * - Queue reordering
 */
class TranscodeQueue {
public:
    TranscodeQueue();
    ~TranscodeQueue();

    // Job management
    void AddJob(std::unique_ptr<TranscodeJob> job);
    bool RemoveJob(const std::string& job_id);
    TranscodeJob* GetJob(const std::string& job_id);
    std::vector<TranscodeJob*> GetAllJobs();

    // Queue operations
    TranscodeJob* GetNextJob();  // Get next ready job (respects priority & dependencies)
    int GetJobCount() const;
    int GetQueuedJobCount() const;
    int GetActiveJobCount() const;
    int GetCompletedJobCount() const;
    int GetFailedJobCount() const;

    // Job control
    bool StartJob(const std::string& job_id);
    bool PauseJob(const std::string& job_id);
    bool ResumeJob(const std::string& job_id);
    bool CancelJob(const std::string& job_id);

    // Queue reordering
    bool MoveJobUp(const std::string& job_id);
    bool MoveJobDown(const std::string& job_id);
    bool MoveJobToTop(const std::string& job_id);
    bool SetJobPriority(const std::string& job_id, TranscodeJob::Priority priority);

    // Status filtering
    std::vector<TranscodeJob*> GetJobsByStatus(TranscodeJob::Status status);
    std::vector<std::string> GetCompletedJobIDs() const;  // For dependency checking

    // Cleanup
    void ClearCompleted();
    void ClearFailed();
    void ClearAll();

    // Persistence
    bool SaveQueue(const std::string& filepath);
    bool LoadQueue(const std::string& filepath);
    void SetAutoSavePath(const std::string& filepath);
    void EnableAutoSave(bool enable) { auto_save_enabled_ = enable; }

    // Statistics
    struct Stats {
        int total = 0;
        int queued = 0;
        int encoding = 0;
        int paused = 0;
        int completed = 0;
        int failed = 0;
        int cancelled = 0;
    };
    Stats GetStats() const;

private:
    void AutoSave();
    void SortByPriority();
    int FindJobIndex(const std::string& job_id) const;

    std::vector<std::unique_ptr<TranscodeJob>> jobs_;
    mutable std::mutex queue_mutex_;

    std::string auto_save_path_;
    bool auto_save_enabled_ = true;
};

} // namespace ump
