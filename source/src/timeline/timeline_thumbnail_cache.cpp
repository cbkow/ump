#include "timeline_thumbnail_cache.h"
#include "../utils/debug_utils.h"
#include "../player/image_loaders.h"
#include "../player/streaming_video_decoder.h"
#include <algorithm>
#include <climits>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace ump {

TimelineThumbnailCache::TimelineThumbnailCache() {
    Debug::Log("TimelineThumbnailCache: Created");
}

TimelineThumbnailCache::~TimelineThumbnailCache() {
    Shutdown();
}

void TimelineThumbnailCache::Initialize(double fps) {
    if (initialized_) return;

    fps_ = fps;
    initialized_ = true;
    running_ = true;

    // Start background worker thread
    worker_thread_ = std::thread(&TimelineThumbnailCache::WorkerThread, this);

#ifdef _WIN32
    // Lower priority to avoid competing with playback
    SetThreadPriority(worker_thread_.native_handle(), THREAD_PRIORITY_BELOW_NORMAL);
#endif

    Debug::Log("TimelineThumbnailCache: Initialized at " + std::to_string(fps) + " fps");
}

void TimelineThumbnailCache::Shutdown() {
    if (!initialized_) return;

    Debug::Log("TimelineThumbnailCache: Shutting down...");

    running_ = false;
    queue_cv_.notify_all();

    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }

    // Clear cache (delete GL textures)
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        for (auto& [key, entry] : cache_) {
            if (entry.texture_id != 0) {
                glDeleteTextures(1, &entry.texture_id);
            }
        }
        cache_.clear();
        lru_order_.clear();
    }

    // Clear loaders
    {
        std::lock_guard<std::mutex> lock(loaders_mutex_);
        loaders_.clear();
    }

    initialized_ = false;
    Debug::Log("TimelineThumbnailCache: Shutdown complete");
}

GLuint TimelineThumbnailCache::GetThumbnail(const std::string& source_path, int source_frame,
                                             int& width, int& height, bool allow_fallback) {
    if (!config_.enabled || !initialized_) {
        width = 0;
        height = 0;
        return 0;
    }

    TimelineThumbnailKey key{source_path, source_frame};

    std::lock_guard<std::mutex> lock(cache_mutex_);

    // Check cache for exact match
    auto it = cache_.find(key);
    if (it != cache_.end()) {
        cache_hits_++;

        // Update LRU order - move to front
        lru_order_.remove(key);
        lru_order_.push_front(key);

        width = it->second.width;
        height = it->second.height;
        return it->second.texture_id;
    }

    // Cache miss - queue request
    cache_misses_++;

    {
        std::lock_guard<std::mutex> queue_lock(queue_mutex_);

        // Only queue if not already in progress
        if (in_progress_.find(key) == in_progress_.end()) {
            // Check if already in request queue
            bool already_queued = false;
            for (const auto& req : request_queue_) {
                if (req.key == key) {
                    already_queued = true;
                    break;
                }
            }

            if (!already_queued) {
                request_queue_.push_front({key, true});  // High priority at front
                queue_cv_.notify_one();
            }
        }
    }

    // Try fallback to nearest cached frame from same source
    if (allow_fallback) {
        int nearest = FindNearestCachedFrame(source_path, source_frame);
        if (nearest >= 0) {
            TimelineThumbnailKey nearest_key{source_path, nearest};
            auto nearest_it = cache_.find(nearest_key);
            if (nearest_it != cache_.end()) {
                width = nearest_it->second.width;
                height = nearest_it->second.height;
                return nearest_it->second.texture_id;
            }
        }
    }

    width = 0;
    height = 0;
    return 0;  // Not ready yet
}

void TimelineThumbnailCache::PrecacheRange(const std::string& source_path, int start_frame, int end_frame,
                                            int priority_frame, int step) {
    if (!config_.enabled || !initialized_) return;

    std::lock_guard<std::mutex> queue_lock(queue_mutex_);

    // First, queue the priority frame (current position) with high priority
    if (priority_frame >= start_frame && priority_frame <= end_frame) {
        TimelineThumbnailKey priority_key{source_path, priority_frame};
        if (in_progress_.find(priority_key) == in_progress_.end()) {
            bool already_queued = false;
            for (const auto& req : request_queue_) {
                if (req.key == priority_key) {
                    already_queued = true;
                    break;
                }
            }
            if (!already_queued) {
                request_queue_.push_front({priority_key, true});
            }
        }
    }

    // Then queue strategic frames at intervals
    for (int frame = start_frame; frame <= end_frame; frame += step) {
        if (frame == priority_frame) continue;  // Already queued

        TimelineThumbnailKey key{source_path, frame};

        // Skip if already cached
        {
            std::lock_guard<std::mutex> cache_lock(cache_mutex_);
            if (cache_.find(key) != cache_.end()) continue;
        }

        // Skip if already in progress or queued
        if (in_progress_.find(key) != in_progress_.end()) continue;

        bool already_queued = false;
        for (const auto& req : request_queue_) {
            if (req.key == key) {
                already_queued = true;
                break;
            }
        }

        if (!already_queued) {
            request_queue_.push_back({key, false});  // Low priority at back
        }
    }

    queue_cv_.notify_one();

    Debug::Log("TimelineThumbnailCache: Precaching " + source_path +
               " frames " + std::to_string(start_frame) + "-" + std::to_string(end_frame) +
               " (step=" + std::to_string(step) + ")");
}

void TimelineThumbnailCache::CancelPendingRequests() {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    request_queue_.clear();
    // Note: in_progress_ items will finish but results will be discarded if clip changed
}

void TimelineThumbnailCache::ProcessPendingUploads() {
    if (!initialized_) return;

    std::deque<PendingUpload> uploads;

    {
        std::lock_guard<std::mutex> lock(upload_mutex_);
        uploads.swap(pending_uploads_);
    }

    if (uploads.empty()) return;

    for (auto& upload : uploads) {
        GLuint texture_id = CreateGLTexture(upload.pixels);

        if (texture_id != 0) {
            std::lock_guard<std::mutex> lock(cache_mutex_);

            // Evict LRU if cache is full
            while (static_cast<int>(cache_.size()) >= config_.cache_size) {
                EvictLRU();
            }

            // Add to cache
            TimelineThumbnailEntry entry;
            entry.texture_id = texture_id;
            entry.width = upload.pixels->width;
            entry.height = upload.pixels->height;
            cache_[upload.key] = entry;

            // Add to LRU order
            lru_order_.push_front(upload.key);
        }
    }
}

void TimelineThumbnailCache::Clear() {
    Debug::Log("TimelineThumbnailCache: Clearing cache");

    // Clear pending requests
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        request_queue_.clear();
        in_progress_.clear();
    }

    // Clear pending uploads
    {
        std::lock_guard<std::mutex> lock(upload_mutex_);
        pending_uploads_.clear();
    }

    // Clear cache
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        for (auto& [key, entry] : cache_) {
            if (entry.texture_id != 0) {
                glDeleteTextures(1, &entry.texture_id);
            }
        }
        cache_.clear();
        lru_order_.clear();
    }

    // Clear loaders
    {
        std::lock_guard<std::mutex> lock(loaders_mutex_);
        loaders_.clear();
    }

    cache_hits_ = 0;
    cache_misses_ = 0;
}

TimelineThumbnailCache::Stats TimelineThumbnailCache::GetStats() const {
    Stats stats;

    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        stats.total_cached = static_cast<int>(cache_.size());
    }

    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        stats.pending_requests = static_cast<int>(request_queue_.size() + in_progress_.size());
    }

    stats.cache_hits = cache_hits_.load();
    stats.cache_misses = cache_misses_.load();

    return stats;
}

void TimelineThumbnailCache::WorkerThread() {
    Debug::Log("TimelineThumbnailCache: Worker thread started");

    while (running_) {
        ThumbnailRequest request;
        bool have_request = false;

        // Wait for work
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait(lock, [this]() {
                return !running_ || !request_queue_.empty();
            });

            if (!running_) break;

            if (!request_queue_.empty()) {
                request = request_queue_.front();
                request_queue_.pop_front();
                in_progress_.insert(request.key);
                have_request = true;
            }
        }

        if (have_request) {
            // Load thumbnail pixels
            auto pixels = LoadThumbnailPixels(request.key);

            if (pixels) {
                // Queue for GPU upload
                std::lock_guard<std::mutex> lock(upload_mutex_);
                pending_uploads_.push_back({request.key, pixels});
            }

            // Remove from in-progress
            {
                std::lock_guard<std::mutex> lock(queue_mutex_);
                in_progress_.erase(request.key);
            }
        }
    }

    Debug::Log("TimelineThumbnailCache: Worker thread stopped");
}

std::shared_ptr<TimelineThumbnailCache::LoaderInfo>
TimelineThumbnailCache::GetOrCreateLoader(const std::string& source_path) {
    std::lock_guard<std::mutex> lock(loaders_mutex_);

    auto it = loaders_.find(source_path);
    if (it != loaders_.end()) {
        return it->second;
    }

    // Create new loader based on file type
    auto info = std::make_shared<LoaderInfo>();

    // Detect media type from extension
    std::string ext = source_path.substr(source_path.find_last_of('.') + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    bool is_video = (ext == "mov" || ext == "mp4" || ext == "mxf" ||
                     ext == "avi" || ext == "mkv" || ext == "m4v");

    if (is_video) {
        info->is_video = true;
        // Create and cache VideoImageLoader - will be reused for all frames from this source
        // This is safe because we only have a single worker thread
        info->image_loader = std::make_unique<VideoImageLoader>(source_path, fps_, 0.0);
        Debug::Log("TimelineThumbnailCache: Created VideoImageLoader for " + source_path);
    } else {
        info->is_video = false;
        // Create appropriate image loader
        if (ext == "exr") {
            info->image_loader = std::make_unique<EXRImageLoader>();
        } else if (ext == "tiff" || ext == "tif") {
            info->image_loader = std::make_unique<TIFFImageLoader>();
        } else if (ext == "png") {
            info->image_loader = std::make_unique<PNGImageLoader>();
        } else if (ext == "jpg" || ext == "jpeg") {
            info->image_loader = std::make_unique<JPEGImageLoader>();
        }
    }

    loaders_[source_path] = info;
    return info;
}

std::shared_ptr<PixelData> TimelineThumbnailCache::LoadThumbnailPixels(const TimelineThumbnailKey& key) {
    auto loader_info = GetOrCreateLoader(key.source_path);
    if (!loader_info || !loader_info->image_loader) {
        Debug::Log("TimelineThumbnailCache::LoadThumbnailPixels: No loader for " + key.source_path);
        return nullptr;
    }

    // Use cached loader for both video and image sequences
    // For video: image_loader is a VideoImageLoader (created in GetOrCreateLoader)
    // For images: image_loader is an EXRImageLoader/TIFFImageLoader/etc.
    int max_thumb_size = std::max(config_.width, config_.height);

    Debug::Log("TimelineThumbnailCache::LoadThumbnailPixels: Loading frame " +
               std::to_string(key.source_frame) + " from " + key.source_path +
               " (is_video=" + std::to_string(loader_info->is_video) + ")");

    if (loader_info->is_video) {
        // Video: pass frame number as string to LoadThumbnail
        auto result = loader_info->image_loader->LoadThumbnail(std::to_string(key.source_frame), max_thumb_size);
        if (result) {
            Debug::Log("TimelineThumbnailCache::LoadThumbnailPixels: SUCCESS - got " +
                       std::to_string(result->width) + "x" + std::to_string(result->height));
        } else {
            Debug::Log("TimelineThumbnailCache::LoadThumbnailPixels: FAILED for frame " +
                       std::to_string(key.source_frame));
        }
        return result;
    } else {
        // Image sequence: pass the file path directly
        return loader_info->image_loader->LoadThumbnail(key.source_path, max_thumb_size);
    }
}

GLuint TimelineThumbnailCache::CreateGLTexture(const std::shared_ptr<PixelData>& pixels) {
    if (!pixels || pixels->pixels.empty()) {
        return 0;
    }

    GLuint texture_id = 0;
    glGenTextures(1, &texture_id);
    if (texture_id == 0) {
        return 0;
    }

    // Determine internal format based on pixel type
    GLenum internal_format = GL_RGBA8;
    if (pixels->gl_type == GL_HALF_FLOAT) {
        internal_format = GL_RGBA16F;
    } else if (pixels->gl_type == GL_UNSIGNED_SHORT) {
        internal_format = GL_RGBA16;
    }

    glBindTexture(GL_TEXTURE_2D, texture_id);
    glTexImage2D(GL_TEXTURE_2D, 0, internal_format,
                 pixels->width, pixels->height, 0,
                 pixels->gl_format, pixels->gl_type,
                 pixels->pixels.data());

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glBindTexture(GL_TEXTURE_2D, 0);

    return texture_id;
}

void TimelineThumbnailCache::EvictLRU() {
    if (lru_order_.empty()) return;

    // LRU item is at the back
    TimelineThumbnailKey lru_key = lru_order_.back();
    lru_order_.pop_back();

    auto it = cache_.find(lru_key);
    if (it != cache_.end()) {
        if (it->second.texture_id != 0) {
            glDeleteTextures(1, &it->second.texture_id);
        }
        cache_.erase(it);
    }
}

int TimelineThumbnailCache::FindNearestCachedFrame(const std::string& source_path, int target_frame) const {
    // Note: cache_mutex_ should already be locked by caller
    if (cache_.empty()) {
        return -1;
    }

    int min_distance = INT_MAX;
    int nearest_frame = -1;

    for (const auto& [key, entry] : cache_) {
        // Only consider frames from the same source file
        if (key.source_path != source_path) continue;

        int distance = std::abs(key.source_frame - target_frame);
        if (distance < min_distance) {
            min_distance = distance;
            nearest_frame = key.source_frame;
        }
    }

    return nearest_frame;
}

} // namespace ump
