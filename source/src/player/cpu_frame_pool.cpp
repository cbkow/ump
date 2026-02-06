#include "cpu_frame_pool.h"
#include "../utils/debug_utils.h"

extern "C" {
#include <libavutil/imgutils.h>
}

#include <algorithm>
#include <cmath>

namespace ump {

CPUFramePool::CPUFramePool() = default;

CPUFramePool::~CPUFramePool() {
    ClearAll();
}

//=============================================================================
// Configuration
//=============================================================================

void CPUFramePool::SetMaxFramesPerStream(int max_frames) {
    std::lock_guard<std::mutex> lock(mutex_);
    max_frames_per_stream_ = std::max(1, max_frames);
}

void CPUFramePool::SetMaxMemoryBytes(size_t max_bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    max_memory_bytes_ = max_bytes;
    EvictIfNeeded();
}

//=============================================================================
// Frame Storage
//=============================================================================

void CPUFramePool::StoreFrame(int stream_id, int frame_number, AVFrame* frame) {
    if (!frame) return;

    std::lock_guard<std::mutex> lock(mutex_);

    auto& stream = streams_[stream_id];

    // Check if frame already exists
    auto it = stream.frame_index.find(frame_number);
    if (it != stream.frame_index.end()) {
        // Replace existing frame
        size_t idx = it->second;
        auto& entry = stream.frames[idx];
        current_memory_bytes_ -= entry.memory_bytes;
        av_frame_free(&entry.frame);
        entry.frame = frame;
        entry.memory_bytes = CalculateFrameSize(frame);
        current_memory_bytes_ += entry.memory_bytes;
        return;
    }

    // Evict if at max frames for this stream
    while (static_cast<int>(stream.frames.size()) >= max_frames_per_stream_) {
        EvictOldestFrame(stream_id);
    }

    // Add new frame
    size_t frame_size = CalculateFrameSize(frame);
    stream.frames.emplace_back(frame_number, frame, frame_size);
    stream.frame_index[frame_number] = stream.frames.size() - 1;
    current_memory_bytes_ += frame_size;

    // Check global memory limit
    EvictIfNeeded();
}

void CPUFramePool::StoreFrameRef(int stream_id, int frame_number, const AVFrame* frame) {
    if (!frame) return;

    // Create a reference copy
    AVFrame* frame_copy = av_frame_clone(frame);
    if (!frame_copy) {
        Debug::Log("CPUFramePool: failed to clone frame");
        return;
    }

    StoreFrame(stream_id, frame_number, frame_copy);
}

//=============================================================================
// Frame Retrieval
//=============================================================================

AVFrame* CPUFramePool::GetFrame(int stream_id, int frame_number) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto stream_it = streams_.find(stream_id);
    if (stream_it == streams_.end()) return nullptr;

    auto& stream = stream_it->second;
    auto frame_it = stream.frame_index.find(frame_number);
    if (frame_it == stream.frame_index.end()) return nullptr;

    return stream.frames[frame_it->second].frame;
}

AVFrame* CPUFramePool::GetClosestFrame(int stream_id, int target_frame, int* actual_frame) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto stream_it = streams_.find(stream_id);
    if (stream_it == streams_.end()) return nullptr;

    auto& stream = stream_it->second;
    if (stream.frames.empty()) return nullptr;

    // Find closest frame
    int best_frame = -1;
    int best_distance = INT_MAX;
    AVFrame* best_ptr = nullptr;

    for (const auto& entry : stream.frames) {
        int distance = std::abs(entry.frame_number - target_frame);
        if (distance < best_distance) {
            best_distance = distance;
            best_frame = entry.frame_number;
            best_ptr = entry.frame;
        }
    }

    if (actual_frame) *actual_frame = best_frame;
    return best_ptr;
}

bool CPUFramePool::HasFrame(int stream_id, int frame_number) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto stream_it = streams_.find(stream_id);
    if (stream_it == streams_.end()) return false;

    return stream_it->second.frame_index.count(frame_number) > 0;
}

//=============================================================================
// Stream Management
//=============================================================================

void CPUFramePool::ClearStream(int stream_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto stream_it = streams_.find(stream_id);
    if (stream_it == streams_.end()) return;

    auto& stream = stream_it->second;

    for (auto& entry : stream.frames) {
        current_memory_bytes_ -= entry.memory_bytes;
        av_frame_free(&entry.frame);
    }

    stream.frames.clear();
    stream.frame_index.clear();
}

void CPUFramePool::ClearAll() {
    std::lock_guard<std::mutex> lock(mutex_);

    for (auto& pair : streams_) {
        for (auto& entry : pair.second.frames) {
            av_frame_free(&entry.frame);
        }
    }

    streams_.clear();
    current_memory_bytes_ = 0;
}

int CPUFramePool::GetFrameCount(int stream_id) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto stream_it = streams_.find(stream_id);
    if (stream_it == streams_.end()) return 0;

    return static_cast<int>(stream_it->second.frames.size());
}

size_t CPUFramePool::GetMemoryUsage() const {
    return current_memory_bytes_.load();
}

//=============================================================================
// Statistics
//=============================================================================

CPUFramePool::Stats CPUFramePool::GetStats() const {
    std::lock_guard<std::mutex> lock(mutex_);

    Stats stats;
    stats.total_frames = 0;
    stats.total_memory_bytes = current_memory_bytes_.load();
    stats.stream_count = static_cast<int>(streams_.size());

    for (const auto& pair : streams_) {
        stats.total_frames += static_cast<int>(pair.second.frames.size());
    }

    return stats;
}

//=============================================================================
// Internal Helpers
//=============================================================================

size_t CPUFramePool::CalculateFrameSize(const AVFrame* frame) {
    if (!frame) return 0;

    // Use av_image_get_buffer_size if possible
    if (frame->format >= 0 && frame->width > 0 && frame->height > 0) {
        int size = av_image_get_buffer_size(
            static_cast<AVPixelFormat>(frame->format),
            frame->width, frame->height, 32);
        if (size > 0) return static_cast<size_t>(size);
    }

    // Fallback: sum up plane sizes
    size_t total = 0;
    for (int i = 0; i < AV_NUM_DATA_POINTERS && frame->data[i]; i++) {
        if (frame->linesize[i] > 0) {
            // Estimate plane height
            int plane_height = frame->height;
            if (i > 0) {
                // Chroma planes may be smaller
                const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(
                    static_cast<AVPixelFormat>(frame->format));
                if (desc) {
                    plane_height = frame->height >> desc->log2_chroma_h;
                }
            }
            total += frame->linesize[i] * plane_height;
        }
    }

    return total > 0 ? total : static_cast<size_t>(frame->width * frame->height * 4);
}

void CPUFramePool::EvictIfNeeded() {
    // Evict from streams with most frames first
    while (current_memory_bytes_ > max_memory_bytes_) {
        int max_stream_id = -1;
        size_t max_frame_count = 0;

        for (const auto& pair : streams_) {
            if (pair.second.frames.size() > max_frame_count) {
                max_frame_count = pair.second.frames.size();
                max_stream_id = pair.first;
            }
        }

        if (max_stream_id < 0 || max_frame_count == 0) break;

        EvictOldestFrame(max_stream_id);
    }
}

void CPUFramePool::EvictOldestFrame(int stream_id) {
    auto stream_it = streams_.find(stream_id);
    if (stream_it == streams_.end()) return;

    auto& stream = stream_it->second;
    if (stream.frames.empty()) return;

    // Remove oldest (front of deque)
    auto& entry = stream.frames.front();
    current_memory_bytes_ -= entry.memory_bytes;

    // Update index for removed frame
    stream.frame_index.erase(entry.frame_number);

    av_frame_free(&entry.frame);
    stream.frames.pop_front();

    // Rebuild index (deque indices shifted)
    stream.frame_index.clear();
    for (size_t i = 0; i < stream.frames.size(); i++) {
        stream.frame_index[stream.frames[i].frame_number] = i;
    }
}

} // namespace ump
