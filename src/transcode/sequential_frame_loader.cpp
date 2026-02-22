#include "sequential_frame_loader.h"
#include "../utils/debug_utils.h"

#include <OpenEXR/ImfThreading.h>

#include <algorithm>
#include <chrono>

#undef min
#undef max

namespace qcview {

SequentialFrameLoader::SequentialFrameLoader(
    std::unique_ptr<IImageLoader> loader,
    const std::vector<std::string>& files,
    int prefetch_count,
    PipelineMode pipeline_mode,
    const std::string& exr_layer,
    int concurrent_loads,
    int openexr_threads
)
    : loader_(std::move(loader))
    , files_(files)
    , prefetch_count_(prefetch_count)
    , concurrent_loads_(std::clamp(concurrent_loads, 1, 16))
    , openexr_threads_(std::clamp(openexr_threads, 1, 8))
    , pipeline_mode_(pipeline_mode)
    , exr_layer_(exr_layer)
{
    if (!loader_) {
        Debug::Log("ERROR: SequentialFrameLoader initialized with null loader");
        return;
    }

    if (files_.empty()) {
        Debug::Log("ERROR: SequentialFrameLoader initialized with empty file list");
        return;
    }

    // Set OpenEXR thread count for decompression
    // Total threads = concurrent_loads × openexr_threads
    Imf::setGlobalThreadCount(openexr_threads_);

    int total_threads = concurrent_loads_ * openexr_threads_;
    Debug::Log("SequentialFrameLoader: Initializing for " + std::to_string(files_.size()) +
               " frames, prefetch=" + std::to_string(prefetch_count_) +
               ", concurrent_loads=" + std::to_string(concurrent_loads_) +
               ", openexr_threads=" + std::to_string(openexr_threads_) +
               " (total ~" + std::to_string(total_threads) + " threads)");

    // Start I/O worker thread
    running_ = true;
    io_worker_thread_ = std::thread(&SequentialFrameLoader::IOWorkerThread, this);
}

SequentialFrameLoader::~SequentialFrameLoader() {
    Cancel();

    // Wait for I/O worker thread to finish
    if (io_worker_thread_.joinable()) {
        io_worker_thread_.join();
    }

    Debug::Log("SequentialFrameLoader: Destroyed");
}

std::shared_ptr<PixelData> SequentialFrameLoader::GetFrame(int frame_index) {
    if (frame_index < 0 || frame_index >= static_cast<int>(files_.size())) {
        Debug::Log("ERROR: SequentialFrameLoader::GetFrame - Invalid frame index: " +
                   std::to_string(frame_index));
        return nullptr;
    }

    // Wait for frame to be loaded
    std::unique_lock<std::mutex> lock(buffer_mutex_);

    buffer_cv_.wait(lock, [this, frame_index]() {
        return cancelled_ || IsFrameInBuffer(frame_index);
    });

    if (cancelled_) {
        return nullptr;
    }

    // Find frame in buffer
    for (const auto& entry : buffer_) {
        if (entry.frame_index == frame_index) {
            return entry.pixel_data;
        }
    }

    Debug::Log("ERROR: SequentialFrameLoader::GetFrame - Frame not found after wait: " +
               std::to_string(frame_index));
    return nullptr;
}

void SequentialFrameLoader::PrefetchAhead(int start_frame) {
    prefetch_start_frame_.store(start_frame);

    // Queue frames for loading
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);

        // Add frames to pending queue if not already buffered or in progress
        for (int i = 0; i < prefetch_count_ && (start_frame + i) < static_cast<int>(files_.size()); ++i) {
            int frame = start_frame + i;

            // Skip if already in buffer
            {
                std::lock_guard<std::mutex> buf_lock(buffer_mutex_);
                if (IsFrameInBuffer(frame)) continue;
            }

            // Skip if already in progress
            if (IsFrameInProgress(frame)) continue;

            // Skip if already pending
            bool already_pending = false;
            for (int pending : pending_frames_) {
                if (pending == frame) {
                    already_pending = true;
                    break;
                }
            }
            if (already_pending) continue;

            pending_frames_.push_back(frame);
        }
    }

    // Wake up worker thread
    worker_cv_.notify_one();
}

void SequentialFrameLoader::Cancel() {
    if (!cancelled_.exchange(true)) {
        Debug::Log("SequentialFrameLoader: Cancelling");
        running_ = false;

        // Wake all waiting threads
        buffer_cv_.notify_all();
        worker_cv_.notify_all();
    }
}

bool SequentialFrameLoader::IsFrameInBuffer(int frame_index) const {
    // Note: Caller must hold buffer_mutex_
    for (const auto& entry : buffer_) {
        if (entry.frame_index == frame_index) {
            return true;
        }
    }
    return false;
}

bool SequentialFrameLoader::IsFrameInProgress(int frame_index) const {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(progress_mutex_));
    return in_progress_.find(frame_index) != in_progress_.end();
}

void SequentialFrameLoader::TrimBuffer(int current_frame) {
    // Note: Caller must hold buffer_mutex_

    // Keep frames in range [current_frame - 1, current_frame + prefetch_count]
    int min_frame = std::max(0, current_frame - 1);
    int max_frame = current_frame + prefetch_count_;

    auto it = buffer_.begin();
    while (it != buffer_.end()) {
        if (it->frame_index < min_frame || it->frame_index > max_frame) {
            it = buffer_.erase(it);
        } else {
            ++it;
        }
    }
}

void SequentialFrameLoader::IOWorkerThread() {
    Debug::Log("SequentialFrameLoader: I/O worker thread started (concurrent_loads=" +
               std::to_string(concurrent_loads_) + ")");

    const std::chrono::milliseconds timeout(10);

    while (running_ && !cancelled_) {
        // Wait for work
        {
            std::unique_lock<std::mutex> lock(worker_mutex_);
            worker_cv_.wait_for(lock, timeout, [this] {
                std::lock_guard<std::mutex> plock(pending_mutex_);
                std::lock_guard<std::mutex> ilock(progress_mutex_);
                return !pending_frames_.empty() ||
                       !in_progress_.empty() ||
                       !running_;
            });
        }

        if (!running_ || cancelled_) break;

        // Spawn async tasks (up to concurrent_loads_)
        {
            std::lock_guard<std::mutex> plock(pending_mutex_);
            std::lock_guard<std::mutex> ilock(progress_mutex_);

            while (!pending_frames_.empty() &&
                   static_cast<int>(in_progress_.size()) < concurrent_loads_) {

                int frame = pending_frames_.front();
                pending_frames_.pop_front();

                // Validate frame index
                if (frame < 0 || frame >= static_cast<int>(files_.size())) {
                    continue;
                }

                // Skip if already in buffer (may have been loaded while pending)
                {
                    std::lock_guard<std::mutex> buf_lock(buffer_mutex_);
                    if (IsFrameInBuffer(frame)) continue;
                }

                // Create async load request
                LoadRequest request;
                request.frame_index = frame;

                // Capture values for async lambda
                const std::string path = files_[frame];
                const std::string layer = exr_layer_;
                const PipelineMode mode = pipeline_mode_;

                // Capture loader pointer for async task
                // NOTE: Loaders are thread-safe for concurrent LoadFrame calls
                // (each call creates its own file handles and local state)
                // This matches DirectEXRCache pattern - no mutex needed
                IImageLoader* loader_ptr = loader_.get();

                request.future = std::async(std::launch::async, [loader_ptr, path, layer, mode, frame]() {
                    auto start = std::chrono::steady_clock::now();

                    std::shared_ptr<PixelData> result;
                    try {
                        // No mutex - loaders are thread-safe (like DirectEXRCache)
                        result = loader_ptr->LoadFrame(path, layer, mode);
                    } catch (const std::exception& e) {
                        Debug::Log("SequentialFrameLoader: Async load error frame " +
                                   std::to_string(frame) + " - " + std::string(e.what()));
                        return std::shared_ptr<PixelData>(nullptr);
                    }

                    auto end = std::chrono::steady_clock::now();
                    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

                    if (result) {
                        Debug::Log("SequentialFrameLoader: Loaded frame " + std::to_string(frame) +
                                   " in " + std::to_string(ms) + "ms (" +
                                   std::to_string(result->width) + "x" + std::to_string(result->height) + ")");
                    }

                    return result;
                });

                in_progress_[frame] = std::move(request);
            }
        }

        // Check for completed loads (non-blocking poll)
        {
            std::lock_guard<std::mutex> ilock(progress_mutex_);

            auto it = in_progress_.begin();
            while (it != in_progress_.end()) {
                if (it->second.future.valid() &&
                    it->second.future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {

                    try {
                        auto pixel_data = it->second.future.get();
                        int frame = it->first;

                        if (pixel_data && !pixel_data->pixels.empty()) {
                            // Add to buffer
                            {
                                std::lock_guard<std::mutex> buf_lock(buffer_mutex_);
                                buffer_.push_back({frame, pixel_data});

                                // Trim old frames
                                TrimBuffer(prefetch_start_frame_.load());
                            }

                            // Notify waiting GetFrame() calls
                            buffer_cv_.notify_all();
                        } else {
                            Debug::Log("ERROR: SequentialFrameLoader - Frame " +
                                       std::to_string(frame) + " returned null/empty");
                        }
                    } catch (const std::exception& e) {
                        Debug::Log("ERROR: SequentialFrameLoader - Exception getting future: " +
                                   std::string(e.what()));
                    }

                    it = in_progress_.erase(it);
                } else {
                    ++it;
                }
            }
        }
    }

    Debug::Log("SequentialFrameLoader: I/O worker thread stopped");
}

} // namespace qcview
