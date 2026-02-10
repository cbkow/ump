#pragma once

#include <memory>
#include <unordered_map>
#include <deque>
#include <mutex>
#include <atomic>

extern "C" {
#include <libavutil/frame.h>
}

namespace ump {

//=============================================================================
// CPUFramePool
//
// Stores decoded video frames in CPU memory for multi-view and export use cases.
//
// Design goals:
// - Thread-safe access from decoder and render threads
// - Efficient memory reuse via AVFrame reference counting
// - Per-stream frame queues with automatic eviction
// - Support for multiple simultaneous streams (dual view, A/B compare)
//
// Usage:
// - Decoder calls StoreFrame() after readback from GPU
// - Renderer calls GetFrame() to retrieve for GL upload
// - Seeking calls ClearStream() to invalidate stale frames
//=============================================================================

class CPUFramePool {
public:
    CPUFramePool();
    ~CPUFramePool();

    // Non-copyable
    CPUFramePool(const CPUFramePool&) = delete;
    CPUFramePool& operator=(const CPUFramePool&) = delete;

    //=========================================================================
    // Configuration
    //=========================================================================

    // Set maximum frames to keep per stream (default: 8)
    void SetMaxFramesPerStream(int max_frames);

    // Set maximum total memory usage in bytes (default: 512MB)
    // Oldest frames evicted when exceeded
    void SetMaxMemoryBytes(size_t max_bytes);

    //=========================================================================
    // Frame Storage
    //=========================================================================

    // Store a frame for a stream
    // Takes ownership of the frame (caller should not free it)
    // frame_number: source frame number for retrieval
    // stream_id: identifies which stream (0=primary, 1=secondary for dual view)
    void StoreFrame(int stream_id, int frame_number, AVFrame* frame);

    // Store a frame with reference copy (caller keeps ownership)
    // Creates a new reference to the frame data
    void StoreFrameRef(int stream_id, int frame_number, const AVFrame* frame);

    //=========================================================================
    // Frame Retrieval
    //=========================================================================

    // Get a frame by exact frame number
    // Returns nullptr if not found
    // Returned frame is a reference - do not free it
    AVFrame* GetFrame(int stream_id, int frame_number);

    // Get closest frame to target (for scrubbing)
    // Returns nullptr if stream has no frames
    // actual_frame receives the actual frame number returned
    AVFrame* GetClosestFrame(int stream_id, int target_frame, int* actual_frame = nullptr);

    // Check if frame exists
    bool HasFrame(int stream_id, int frame_number) const;

    //=========================================================================
    // Stream Management
    //=========================================================================

    // Clear all frames for a stream (call on seek)
    void ClearStream(int stream_id);

    // Clear all frames for all streams
    void ClearAll();

    // Get number of frames stored for a stream
    int GetFrameCount(int stream_id) const;

    // Get total memory usage in bytes
    size_t GetMemoryUsage() const;

    //=========================================================================
    // Statistics
    //=========================================================================

    struct Stats {
        int total_frames;
        size_t total_memory_bytes;
        int stream_count;
    };

    Stats GetStats() const;

private:
    //=========================================================================
    // Internal Types
    //=========================================================================

    struct FrameEntry {
        int frame_number;
        AVFrame* frame;        // Owned by pool
        size_t memory_bytes;   // Cached size for eviction

        FrameEntry() : frame_number(-1), frame(nullptr), memory_bytes(0) {}
        FrameEntry(int num, AVFrame* f, size_t bytes)
            : frame_number(num), frame(f), memory_bytes(bytes) {}
    };

    struct StreamData {
        std::deque<FrameEntry> frames;  // Ordered by insertion time
        std::unordered_map<int, size_t> frame_index;  // frame_number -> index in deque
    };

    //=========================================================================
    // Internal Helpers
    //=========================================================================

    // Calculate memory size of a frame
    static size_t CalculateFrameSize(const AVFrame* frame);

    // Evict oldest frames to stay under memory limit
    void EvictIfNeeded();

    // Evict oldest frame from a stream
    void EvictOldestFrame(int stream_id);

    //=========================================================================
    // State
    //=========================================================================

    mutable std::mutex mutex_;
    std::unordered_map<int, StreamData> streams_;

    int max_frames_per_stream_ = 8;
    size_t max_memory_bytes_ = 512 * 1024 * 1024;  // 512MB
    std::atomic<size_t> current_memory_bytes_{0};
};

} // namespace ump
