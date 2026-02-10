#pragma once

#include <string>
#include <vector>
#include <memory>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <future>
#include <map>
#include <atomic>

#include "../player/image_loader_interface.h"
#include "../player/pipeline_mode.h"

namespace ump {

/**
 * SequentialFrameLoader
 *
 * Optimized for sequential access during transcode operations.
 * Uses parallel file loading (like DirectEXRCache) for maximum throughput.
 *
 * Threading Strategy (learned from DirectEXRCache):
 * - Multiple concurrent file loads via std::async (default 8)
 * - OpenEXR threads per file (default 4, hybrid approach)
 * - Total threads: 8 files × 4 OpenEXR threads = 32 threads
 * - Condition variables for instant wakeup (no polling)
 */
class SequentialFrameLoader {
public:
    /**
     * Constructor
     *
     * @param loader Image loader implementation (EXR/TIFF/PNG/JPEG)
     * @param files Complete list of sequence files
     * @param prefetch_count Number of frames to keep in buffer (default 8)
     * @param pipeline_mode Precision level for loading (NORMAL=8-bit, HDR_RES=16-bit, ULTRA_HIGH_RES=32-bit)
     * @param exr_layer EXR layer name (empty for non-EXR or default layer)
     * @param concurrent_loads Number of parallel file loads (default 8)
     * @param openexr_threads OpenEXR decompression threads per file (default 4)
     */
    SequentialFrameLoader(
        std::unique_ptr<IImageLoader> loader,
        const std::vector<std::string>& files,
        int prefetch_count = 8,
        PipelineMode pipeline_mode = PipelineMode::NORMAL,
        const std::string& exr_layer = "",
        int concurrent_loads = 8,
        int openexr_threads = 4
    );

    ~SequentialFrameLoader();

    /**
     * Get frame (blocking)
     *
     * Waits until the requested frame is loaded into the buffer.
     * Thread-safe.
     *
     * @param frame_index 0-based frame index
     * @return Pixel data (nullptr if error or cancelled)
     */
    std::shared_ptr<PixelData> GetFrame(int frame_index);

    /**
     * Prefetch frames ahead
     *
     * Instructs the background loader to prioritize loading frames
     * starting from the specified index.
     *
     * @param start_frame Frame to start prefetching from
     */
    void PrefetchAhead(int start_frame);

    /**
     * Cancel all operations
     *
     * Stops background loading and causes GetFrame() to return nullptr.
     * Call this before destruction if you want to abort early.
     */
    void Cancel();

    /**
     * Get total frame count
     */
    int GetFrameCount() const { return static_cast<int>(files_.size()); }

    /**
     * Check if loader is active
     */
    bool IsRunning() const { return running_; }

private:
    struct BufferEntry {
        int frame_index;
        std::shared_ptr<PixelData> pixel_data;
    };

    // Async load request (like DirectEXRCache pattern)
    struct LoadRequest {
        int frame_index;
        std::future<std::shared_ptr<PixelData>> future;
    };

    // I/O worker thread (spawns and manages async load tasks)
    void IOWorkerThread();

    // Check if frame is in buffer
    bool IsFrameInBuffer(int frame_index) const;

    // Check if frame is already being loaded
    bool IsFrameInProgress(int frame_index) const;

    // Remove old frames from buffer (keep only recent frames)
    void TrimBuffer(int current_frame);

    // Image loader (polymorphic - EXR, TIFF, PNG, JPEG)
    // NOTE: Loaders are thread-safe for concurrent LoadFrame calls
    // (each call creates its own file handles - like DirectEXRCache)
    std::unique_ptr<IImageLoader> loader_;

    // Sequence files
    std::vector<std::string> files_;

    // Configuration
    int prefetch_count_;
    int concurrent_loads_;   // Number of parallel file loads
    int openexr_threads_;    // OpenEXR threads per file
    PipelineMode pipeline_mode_;
    std::string exr_layer_;

    // Completed frames buffer
    std::deque<BufferEntry> buffer_;
    mutable std::mutex buffer_mutex_;
    std::condition_variable buffer_cv_;

    // Pending requests queue
    std::deque<int> pending_frames_;
    std::mutex pending_mutex_;

    // In-progress async loads (like DirectEXRCache::requestsInProgress_)
    std::map<int, LoadRequest> in_progress_;
    std::mutex progress_mutex_;

    // Prefetch hint (for worker thread)
    std::atomic<int> prefetch_start_frame_{0};

    // I/O worker thread
    std::thread io_worker_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> cancelled_{false};
    std::condition_variable worker_cv_;
    std::mutex worker_mutex_;
};

} // namespace ump
