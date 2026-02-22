#include "decoder_cleanup_queue.h"
#include "streaming_video_decoder.h"
#include "../utils/debug_utils.h"

#include <chrono>

namespace qcview {

//=============================================================================
// Singleton Instance
//=============================================================================

DecoderCleanupQueue& DecoderCleanupQueue::Instance() {
    static DecoderCleanupQueue instance;
    return instance;
}

//=============================================================================
// Constructor / Destructor
//=============================================================================

DecoderCleanupQueue::DecoderCleanupQueue() {
    running_ = true;
    cleanup_thread_ = std::thread(&DecoderCleanupQueue::CleanupThread, this);
    Debug::Log("DecoderCleanupQueue: Initialized");
}

DecoderCleanupQueue::~DecoderCleanupQueue() {
    Shutdown();
}

//=============================================================================
// Core Operations
//=============================================================================

void DecoderCleanupQueue::Abandon(std::shared_ptr<StreamingVideoDecoder> decoder) {
    if (!decoder) return;
    if (!running_.load()) {
        // Queue is shut down, clean synchronously
        Debug::Log("DecoderCleanupQueue: Queue shut down, cleaning synchronously");
        decoder->Shutdown();
        decoder.reset();
        total_cleaned_++;
        return;
    }

    {
        std::lock_guard<std::mutex> lock(queue_mutex_);

        // If queue is full, force synchronous cleanup of oldest
        // This bounds memory usage even during rapid seeking
        if (static_cast<int>(queue_.size()) >= kMaxQueueSize) {
            auto oldest = std::move(queue_.front());
            queue_.pop();

            Debug::Log("DecoderCleanupQueue: Queue full (" +
                       std::to_string(kMaxQueueSize) +
                       "), forcing sync cleanup of oldest");

            // Release lock before potentially slow cleanup
            queue_mutex_.unlock();

            // Synchronous shutdown
            if (oldest) {
                oldest->Shutdown();
                oldest.reset();
                total_cleaned_++;
            }

            // Re-acquire lock to add new decoder
            queue_mutex_.lock();
        }

        queue_.push(std::move(decoder));
        pending_count_ = static_cast<int>(queue_.size());
    }

    queue_cv_.notify_one();
}

void DecoderCleanupQueue::FlushAll() {
    Debug::Log("DecoderCleanupQueue: Flushing all pending decoders...");

    std::queue<std::shared_ptr<StreamingVideoDecoder>> to_clean;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        std::swap(to_clean, queue_);
        pending_count_ = 0;
    }

    int flushed = 0;
    while (!to_clean.empty()) {
        auto decoder = std::move(to_clean.front());
        to_clean.pop();
        if (decoder) {
            decoder->Shutdown();
            decoder.reset();
            total_cleaned_++;
            flushed++;
        }
    }

    Debug::Log("DecoderCleanupQueue: Flush complete, cleaned " +
               std::to_string(flushed) + " decoders");
}

void DecoderCleanupQueue::Shutdown() {
    if (!running_.load()) return;

    Debug::Log("DecoderCleanupQueue: Shutting down...");

    // Signal thread to stop
    running_ = false;
    queue_cv_.notify_all();

    // Wait for thread to finish
    if (cleanup_thread_.joinable()) {
        cleanup_thread_.join();
    }

    // Flush any remaining decoders
    FlushAll();

    Debug::Log("DecoderCleanupQueue: Shutdown complete (total cleaned: " +
               std::to_string(total_cleaned_.load()) + ")");
}

//=============================================================================
// Cleanup Thread
//=============================================================================

void DecoderCleanupQueue::CleanupThread() {
    Debug::Log("DecoderCleanupQueue: Cleanup thread started");

    while (running_.load()) {
        std::shared_ptr<StreamingVideoDecoder> decoder;

        // Wait for work
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait_for(lock, std::chrono::milliseconds(100), [this] {
                return !running_.load() || !queue_.empty();
            });

            if (!running_.load() && queue_.empty()) break;
            if (queue_.empty()) continue;

            decoder = std::move(queue_.front());
            queue_.pop();
            pending_count_ = static_cast<int>(queue_.size());
        }

        // Clean up the decoder (outside lock)
        if (decoder) {
            auto start = std::chrono::steady_clock::now();

            // Shutdown signals the decoder to stop its threads
            decoder->Shutdown();

            // Release our reference - destructor runs, frees FFmpeg resources
            decoder.reset();

            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count();

            total_cleaned_++;

            // Log slow cleanups for monitoring
            if (elapsed > kCleanupWarningMs) {
                Debug::Log("DecoderCleanupQueue: Cleanup took " +
                           std::to_string(elapsed) + "ms (total cleaned: " +
                           std::to_string(total_cleaned_.load()) +
                           ", pending: " + std::to_string(pending_count_.load()) + ")");
            }
        }
    }

    Debug::Log("DecoderCleanupQueue: Cleanup thread stopped");
}

} // namespace qcview
