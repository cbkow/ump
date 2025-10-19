#include "sequential_frame_loader.h"
#include "../utils/debug_utils.h"
#include <algorithm>

#undef min
#undef max

namespace ump {

SequentialFrameLoader::SequentialFrameLoader(
    std::unique_ptr<IImageLoader> loader,
    const std::vector<std::string>& files,
    int prefetch_count,
    PipelineMode pipeline_mode,
    const std::string& exr_layer
)
    : loader_(std::move(loader))
    , files_(files)
    , prefetch_count_(prefetch_count)
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

    Debug::Log("SequentialFrameLoader: Initializing for " + std::to_string(files_.size()) +
               " frames, prefetch count: " + std::to_string(prefetch_count_));

    // Start background loader thread
    running_ = true;
    loader_thread_ = std::thread(&SequentialFrameLoader::LoaderThread, this);
}

SequentialFrameLoader::~SequentialFrameLoader() {
    Cancel();

    // Wait for loader thread to finish
    if (loader_thread_.joinable()) {
        loader_thread_.join();
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
    std::lock_guard<std::mutex> lock(prefetch_mutex_);
    prefetch_start_frame_ = start_frame;

    Debug::Log("SequentialFrameLoader: Prefetch hint set to frame " + std::to_string(start_frame));
}

void SequentialFrameLoader::Cancel() {
    if (!cancelled_.exchange(true)) {
        Debug::Log("SequentialFrameLoader: Cancelling");
        running_ = false;
        buffer_cv_.notify_all();
    }
}

void SequentialFrameLoader::LoaderThread() {
    Debug::Log("SequentialFrameLoader: Loader thread started");

    while (running_ && !cancelled_) {
        int target_frame = -1;

        // Determine which frame to load next
        {
            std::lock_guard<std::mutex> lock(prefetch_mutex_);
            target_frame = prefetch_start_frame_;
        }

        // Check if we already have enough frames in buffer
        {
            std::lock_guard<std::mutex> lock(buffer_mutex_);

            // Count frames >= target_frame
            int buffered_ahead = 0;
            for (const auto& entry : buffer_) {
                if (entry.frame_index >= target_frame) {
                    buffered_ahead++;
                }
            }

            if (buffered_ahead >= prefetch_count_) {
                // Buffer is full, wait a bit
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

            // Find next frame to load
            target_frame = -1;
            for (int i = prefetch_start_frame_;
                 i < prefetch_start_frame_ + prefetch_count_ && i < static_cast<int>(files_.size());
                 ++i) {
                if (!IsFrameInBuffer(i)) {
                    target_frame = i;
                    break;
                }
            }
        }

        if (target_frame == -1) {
            // Nothing to load, wait
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        // Load frame (outside of lock - this is the slow part)
        auto pixel_data = LoadFrame(target_frame);

        if (cancelled_) break;

        if (pixel_data) {
            // Add to buffer
            std::lock_guard<std::mutex> lock(buffer_mutex_);
            buffer_.push_back({target_frame, pixel_data});

            // Trim old frames (keep only recent frames around current position)
            TrimBuffer(prefetch_start_frame_);

            // Notify waiting GetFrame() calls
            buffer_cv_.notify_all();
        } else {
            Debug::Log("ERROR: SequentialFrameLoader failed to load frame " +
                       std::to_string(target_frame));
        }
    }

    Debug::Log("SequentialFrameLoader: Loader thread stopped");
}

std::shared_ptr<PixelData> SequentialFrameLoader::LoadFrame(int frame_index) {
    if (frame_index < 0 || frame_index >= static_cast<int>(files_.size())) {
        return nullptr;
    }

    const std::string& file_path = files_[frame_index];

    try {
        // Use the polymorphic loader (EXR/TIFF/PNG/JPEG)
        // Use auto-detected pipeline mode for optimal memory usage and precision
        // Pass EXR layer name (empty string for non-EXR formats)
        auto pixel_data = loader_->LoadFrame(file_path, exr_layer_, pipeline_mode_);

        if (pixel_data) {
            Debug::Log("SequentialFrameLoader: Loaded frame " + std::to_string(frame_index) +
                       " (" + std::to_string(pixel_data->width) + "x" +
                       std::to_string(pixel_data->height) +
                       (exr_layer_.empty() ? "" : ", layer: " + exr_layer_) + ")");
        }

        return pixel_data;

    } catch (const std::exception& e) {
        Debug::Log("ERROR: SequentialFrameLoader::LoadFrame exception: " + std::string(e.what()));
        return nullptr;
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

void SequentialFrameLoader::TrimBuffer(int current_frame) {
    // Note: Caller must hold buffer_mutex_

    // Keep frames in range [current_frame - 1, current_frame + prefetch_count]
    // This allows for small backward seeks without reloading

    int min_frame = std::max(0, current_frame - 1);
    int max_frame = current_frame + prefetch_count_;

    auto it = buffer_.begin();
    while (it != buffer_.end()) {
        if (it->frame_index < min_frame || it->frame_index > max_frame) {
            Debug::Log("SequentialFrameLoader: Trimming frame " + std::to_string(it->frame_index) +
                       " from buffer");
            it = buffer_.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace ump
