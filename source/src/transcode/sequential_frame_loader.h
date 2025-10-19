#pragma once

#include <string>
#include <vector>
#include <memory>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>

#include "../player/image_loader_interface.h"
#include "../player/pipeline_mode.h"

namespace ump {

/**
 * SequentialFrameLoader
 *
 * Optimized for sequential access during transcode operations.
 * Uses small buffer (4 frames default) with no LRU eviction - guarantees frame availability.
 *
 * Key Differences from DirectEXRCache:
 * - Sequential access pattern (not random)
 * - Small buffer (memory efficient for extreme resolutions like 8K+)
 * - No eviction (frames stay until consumed)
 * - Blocking API (waits until frame ready)
 */
class SequentialFrameLoader {
public:
    /**
     * Constructor
     *
     * @param loader Image loader implementation (EXR/TIFF/PNG/JPEG)
     * @param files Complete list of sequence files
     * @param prefetch_count Number of frames to keep in buffer (default 4)
     * @param pipeline_mode Precision level for loading (NORMAL=8-bit, HDR_RES=16-bit, ULTRA_HIGH_RES=32-bit)
     * @param exr_layer EXR layer name (empty for non-EXR or default layer)
     */
    SequentialFrameLoader(
        std::unique_ptr<IImageLoader> loader,
        const std::vector<std::string>& files,
        int prefetch_count = 4,
        PipelineMode pipeline_mode = PipelineMode::NORMAL,
        const std::string& exr_layer = ""
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

    // Background loader thread
    void LoaderThread();

    // Load a single frame
    std::shared_ptr<PixelData> LoadFrame(int frame_index);

    // Check if frame is in buffer
    bool IsFrameInBuffer(int frame_index) const;

    // Remove old frames from buffer (keep only recent frames)
    void TrimBuffer(int current_frame);

    // Image loader (polymorphic - EXR, TIFF, PNG, JPEG)
    std::unique_ptr<IImageLoader> loader_;

    // Sequence files
    std::vector<std::string> files_;

    // Configuration
    int prefetch_count_;
    PipelineMode pipeline_mode_;
    std::string exr_layer_;

    // Buffer (small, no eviction)
    std::deque<BufferEntry> buffer_;
    mutable std::mutex buffer_mutex_;
    std::condition_variable buffer_cv_;

    // Prefetch hint (for loader thread)
    int prefetch_start_frame_ = 0;
    std::mutex prefetch_mutex_;

    // Background loader thread
    std::thread loader_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> cancelled_{false};
};

} // namespace ump
