#pragma once
#include <string>
#include <iostream>
#include <vector>
#include <deque>
#include <mutex>
#include <atomic>
#include <thread>
#include <condition_variable>

#ifdef _WIN32
#include <windows.h>
#include <debugapi.h>
#endif

namespace Debug {

// Async debug logger - queues messages and writes them in batches
// This prevents I/O operations from blocking the render thread
class AsyncLogger {
public:
    static AsyncLogger& Instance() {
        static AsyncLogger instance;
        return instance;
    }

    void Log(const std::string& message) {
        if (!enabled_.load(std::memory_order_relaxed)) return;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            // Limit queue size to prevent memory growth
            if (queue_.size() < kMaxQueueSize) {
                queue_.push_back(message);
            }
        }
        cv_.notify_one();
    }

    void SetEnabled(bool enabled) {
        enabled_.store(enabled, std::memory_order_relaxed);
    }

    bool IsEnabled() const {
        return enabled_.load(std::memory_order_relaxed);
    }

    void Shutdown() {
        running_.store(false);
        cv_.notify_all();
        if (worker_.joinable()) {
            worker_.join();
        }
        // Flush remaining messages
        FlushAll();
    }

private:
    AsyncLogger() : running_(true), enabled_(true) {
        worker_ = std::thread(&AsyncLogger::WorkerThread, this);
    }

    ~AsyncLogger() {
        Shutdown();
    }

    void WorkerThread() {
        std::vector<std::string> batch;
        batch.reserve(kBatchSize);

        while (running_.load()) {
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait_for(lock, std::chrono::milliseconds(50), [this] {
                    return !queue_.empty() || !running_.load();
                });

                // Grab a batch of messages
                while (!queue_.empty() && batch.size() < kBatchSize) {
                    batch.push_back(std::move(queue_.front()));
                    queue_.pop_front();
                }
            }

            // Write batch outside the lock
            if (!batch.empty()) {
                WriteBatch(batch);
                batch.clear();
            }
        }
    }

    void WriteBatch(const std::vector<std::string>& batch) {
        // Build combined output to minimize I/O calls
        std::string combined;
        for (const auto& msg : batch) {
            combined += msg;
            combined += '\n';
        }

#ifdef _WIN32
        OutputDebugStringA(combined.c_str());
#endif
        // Use '\n' not std::endl to avoid per-line flush
        std::cout << combined << std::flush;
    }

    void FlushAll() {
        std::vector<std::string> remaining;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            remaining.assign(queue_.begin(), queue_.end());
            queue_.clear();
        }
        if (!remaining.empty()) {
            WriteBatch(remaining);
        }
    }

    static constexpr size_t kMaxQueueSize = 10000;
    static constexpr size_t kBatchSize = 100;

    std::deque<std::string> queue_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::thread worker_;
    std::atomic<bool> running_;
    std::atomic<bool> enabled_;
};

// Main logging function - async, non-blocking
inline void Log(const std::string& message) {
    AsyncLogger::Instance().Log(message);
}

// Control functions
inline void SetLoggingEnabled(bool enabled) {
    AsyncLogger::Instance().SetEnabled(enabled);
}

inline bool IsLoggingEnabled() {
    return AsyncLogger::Instance().IsEnabled();
}

inline void ShutdownLogging() {
    AsyncLogger::Instance().Shutdown();
}

}  // namespace Debug
