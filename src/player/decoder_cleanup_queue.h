#pragma once

#include <memory>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>

namespace qcview {

// Forward declaration
class StreamingVideoDecoder;

//=============================================================================
// Decoder Cleanup Queue
//
// Centralized, controlled cleanup of abandoned video decoders.
// When a decoder is "abandoned" (user seeked away), it's queued here for
// background cleanup instead of being destroyed inline (which could block).
//
// Features:
// - Single cleanup thread (no thread explosion)
// - Bounded queue (prevents unbounded memory growth)
// - Synchronous flush for clean shutdown
// - Monitoring stats for debugging
//
// Usage:
//   DecoderCleanupQueue::Instance().Abandon(std::move(old_decoder));
//   // ... later, during shutdown ...
//   DecoderCleanupQueue::Instance().FlushAll();
//   DecoderCleanupQueue::Instance().Shutdown();
//=============================================================================

class DecoderCleanupQueue {
public:
    // Singleton access
    static DecoderCleanupQueue& Instance();

    // Delete copy/move
    DecoderCleanupQueue(const DecoderCleanupQueue&) = delete;
    DecoderCleanupQueue& operator=(const DecoderCleanupQueue&) = delete;
    DecoderCleanupQueue(DecoderCleanupQueue&&) = delete;
    DecoderCleanupQueue& operator=(DecoderCleanupQueue&&) = delete;

    //=========================================================================
    // Core Operations
    //=========================================================================

    // Queue a decoder for background cleanup (instant return, non-blocking)
    // If queue is full, oldest decoder is cleaned synchronously (bounded block)
    void Abandon(std::shared_ptr<StreamingVideoDecoder> decoder);

    // Force synchronous cleanup of all pending decoders
    // Call during application shutdown to ensure no leaks
    void FlushAll();

    // Stop the cleanup thread (call after FlushAll during shutdown)
    void Shutdown();

    //=========================================================================
    // Monitoring
    //=========================================================================

    // Number of decoders waiting to be cleaned
    int GetPendingCount() const { return pending_count_.load(); }

    // Total decoders cleaned since startup
    int GetTotalCleaned() const { return total_cleaned_.load(); }

    // Is the cleanup thread running?
    bool IsRunning() const { return running_.load(); }

private:
    DecoderCleanupQueue();
    ~DecoderCleanupQueue();

    // Background cleanup thread
    void CleanupThread();

    // Queue of decoders awaiting cleanup
    std::queue<std::shared_ptr<StreamingVideoDecoder>> queue_;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;

    // Cleanup thread
    std::thread cleanup_thread_;
    std::atomic<bool> running_{false};

    // Stats
    std::atomic<int> pending_count_{0};
    std::atomic<int> total_cleaned_{0};

    // Configuration
    static constexpr int kMaxQueueSize = 10;          // Max pending before forcing sync cleanup
    static constexpr int kCleanupWarningMs = 100;     // Log if cleanup takes longer than this
};

} // namespace qcview
