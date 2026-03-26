#include "timeline_thumbnail_cache.h"
#include "../utils/debug_utils.h"
#include "../player/image_loaders.h"
#include "../player/streaming_video_decoder.h"
#include <algorithm>
#include <climits>

#ifdef _WIN32
#include <Windows.h>
#endif

#ifdef QCVIEW_USE_VULKAN
#include "../gpu/vulkan_texture_pool.h"
#elif defined(QCVIEW_USE_METAL)
#include "../gpu/metal_texture_pool.h"
#endif

namespace qcview {

TimelineThumbnailCache::TimelineThumbnailCache() {
    Debug::Log("TimelineThumbnailCache: Created");
}

TimelineThumbnailCache::~TimelineThumbnailCache() {
    try {
        Shutdown();
    } catch (...) {
        // Destructor is noexcept — any uncaught exception calls std::terminate.
        // During global static destruction, Shutdown may throw because singletons
        // (Debug::AsyncLogger, MetalTexturePool) are already destroyed.
    }
}

void TimelineThumbnailCache::Initialize(double fps) {
    if (initialized_) return;

    fps_ = fps;
    initialized_ = true;
    running_ = true;
    worker_state_ = ThumbnailWorkerState::ACTIVE;
    active_thread_count_ = kMaxWorkerThreads;
    is_playing_ = false;

    // Start multiple background worker threads
    worker_threads_.reserve(kMaxWorkerThreads);
    for (int i = 0; i < kMaxWorkerThreads; ++i) {
        worker_threads_.emplace_back(&TimelineThumbnailCache::WorkerThread, this, i);

#ifdef _WIN32
        // Lower priority to avoid competing with playback
        SetThreadPriority(worker_threads_.back().native_handle(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
    }

    Debug::Log("TimelineThumbnailCache: Initialized at " + std::to_string(fps) +
               " fps with " + std::to_string(kMaxWorkerThreads) + " worker threads");
}

void TimelineThumbnailCache::Shutdown() {
    if (!initialized_) return;

    // Note: Shutdown() may be called during global static destruction (from ~unique_ptr).
    // At that point Debug::AsyncLogger may already be destroyed, so avoid Debug::Log
    // after setting running_=false.

    running_ = false;
    worker_state_ = ThumbnailWorkerState::STOPPED;

    // Wake all workers so they can exit
    queue_cv_.notify_all();
    worker_cv_.notify_all();

    // Join or detach all worker threads. If join() throws (during static
    // destruction workers may have already crashed), detach to prevent
    // std::thread::~thread() from calling std::terminate on a joinable thread.
    for (auto& thread : worker_threads_) {
        if (thread.joinable()) {
            try {
                thread.join();
            } catch (...) {
                try { thread.detach(); } catch (...) {}
            }
        }
    }
    worker_threads_.clear();

    // Clear cache (delete textures)
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        for (auto& [key, entry] : cache_) {
            if (entry.texture_id != 0) {
#ifdef QCVIEW_USE_VULKAN
                qcview::VulkanTexturePool::Instance().QueueDelete(static_cast<uint64_t>(entry.texture_id));
#elif defined(QCVIEW_USE_METAL)
                // Guard against static destruction order: pool may already be shut down at exit
                if (qcview::MetalTexturePool::ThumbnailInstance().IsInitialized()) {
                    qcview::MetalTexturePool::ThumbnailInstance().QueueDelete(static_cast<uint64_t>(entry.texture_id));
                }
#else
                glDeleteTextures(1, &entry.texture_id);
#endif
            }
        }
        cache_.clear();
        lru_order_.clear();
    }

    // Clear per-worker loaders
    {
        std::lock_guard<std::mutex> lock(loaders_mutex_);
        worker_loaders_.clear();
    }

    // Clear worker task tracking
    {
        std::lock_guard<std::mutex> lock(worker_task_mutex_);
        worker_current_task_.clear();
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

    // If Clear() was just called, don't return any textures until next ProcessPendingUploads
    // This prevents returning stale texture IDs that may have been reassigned
    if (clear_pending_.load()) {
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

        // Only queue if not already in progress or queued (O(1) set lookup)
        if (in_progress_.find(key) == in_progress_.end() &&
            queued_keys_.find(key) == queued_keys_.end()) {
            request_queue_.push_front({key, true});  // High priority at front
            queued_keys_.insert(key);
            queue_cv_.notify_one();
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
        if (in_progress_.find(priority_key) == in_progress_.end() &&
            queued_keys_.find(priority_key) == queued_keys_.end()) {
            request_queue_.push_front({priority_key, true});
            queued_keys_.insert(priority_key);
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

        // Skip if already in progress or queued (O(1) set lookup)
        if (in_progress_.find(key) != in_progress_.end()) continue;
        if (queued_keys_.find(key) != queued_keys_.end()) continue;

        request_queue_.push_back({key, false});  // Low priority at back
        queued_keys_.insert(key);
    }

    queue_cv_.notify_one();

    Debug::Log("TimelineThumbnailCache: Precaching " + source_path +
               " frames " + std::to_string(start_frame) + "-" + std::to_string(end_frame) +
               " (step=" + std::to_string(step) + ")");
}

void TimelineThumbnailCache::CancelPendingRequests() {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    request_queue_.clear();
    queued_keys_.clear();
    // Note: in_progress_ items will finish but results will be discarded if clip changed
}

void TimelineThumbnailCache::ProcessPendingUploads() {
    if (!initialized_) return;

    // Clear the protection flag - safe to return textures now
    // This is called at the start of each frame before any GetThumbnail calls
    clear_pending_ = false;

    std::deque<PendingUpload> uploads;

    {
        std::lock_guard<std::mutex> lock(upload_mutex_);
        uploads.swap(pending_uploads_);
    }

    if (uploads.empty()) return;

    // Metal/Vulkan: textures already created on worker thread — just insert into cache (no limit needed).
    // OpenGL: limit per frame to avoid blocking the main thread with synchronous GPU texture copies.
#if defined(QCVIEW_USE_VULKAN) || defined(QCVIEW_USE_METAL)
    constexpr int kMaxUploadsPerFrame = 64;  // No GPU work, just cache insertions
#else
    constexpr int kMaxUploadsPerFrame = 3;
#endif
    int upload_count = 0;

    while (!uploads.empty()) {
        if (upload_count >= kMaxUploadsPerFrame) {
            // Put remaining uploads back for next frame
            std::lock_guard<std::mutex> lock(upload_mutex_);
            while (!uploads.empty()) {
                pending_uploads_.push_back(std::move(uploads.front()));
                uploads.pop_front();
            }
            break;
        }

        auto& upload = uploads.front();

        // Use pre-created GPU texture if available (Metal/Vulkan worker thread path)
        GLuint texture_id = upload.gpu_texture_id;
        int tex_width = upload.width;
        int tex_height = upload.height;
        if (texture_id == 0 && upload.pixels) {
            // OpenGL fallback: create texture on main thread
            texture_id = CreateGLTexture(upload.pixels);
            tex_width = upload.pixels->width;
            tex_height = upload.pixels->height;
        }

        if (texture_id != 0) {
            std::lock_guard<std::mutex> lock(cache_mutex_);

            // Evict LRU if cache is full
            while (static_cast<int>(cache_.size()) >= config_.cache_size) {
                EvictLRU();
            }

            // Add to cache
            TimelineThumbnailEntry entry;
            entry.texture_id = texture_id;
            entry.width = tex_width;
            entry.height = tex_height;
            cache_[upload.key] = entry;

            // Add to LRU order
            lru_order_.push_front(upload.key);
        }

        uploads.pop_front();
        upload_count++;
    }
}

void TimelineThumbnailCache::Clear() {
    Debug::Log("TimelineThumbnailCache: Clearing cache");

    // Signal workers to discard any in-flight work
    clear_generation_.fetch_add(1, std::memory_order_release);

    // Set clear pending flag - prevents GetThumbnail from returning stale IDs
    // until ProcessPendingUploads is called (next frame)
    clear_pending_ = true;

    // Clear pending requests
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        request_queue_.clear();
        queued_keys_.clear();
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
#ifdef QCVIEW_USE_VULKAN
                qcview::VulkanTexturePool::Instance().QueueDelete(static_cast<uint64_t>(entry.texture_id));
#elif defined(QCVIEW_USE_METAL)
                // Guard against static destruction order: pool may already be shut down at exit
                if (qcview::MetalTexturePool::ThumbnailInstance().IsInitialized()) {
                    qcview::MetalTexturePool::ThumbnailInstance().QueueDelete(static_cast<uint64_t>(entry.texture_id));
                }
#else
                glDeleteTextures(1, &entry.texture_id);
#endif
            }
        }
        cache_.clear();
        lru_order_.clear();
    }

    // NOTE: Do NOT clear worker_loaders_ here — workers may be mid-load.
    // Loaders are only cleared in Shutdown() after threads are joined.
    // The clear_generation_ mechanism handles discarding stale results.

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

void TimelineThumbnailCache::WorkerThread(int worker_id) {
    Debug::Log("TimelineThumbnailCache: Worker thread " + std::to_string(worker_id) + " started");

    while (running_) {
        ThumbnailRequest request;
        bool have_request = false;

        // Wait for work
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);

            queue_cv_.wait(lock, [this, worker_id]() {
                return !running_ ||
                       (!request_queue_.empty() && ShouldWorkerRun(worker_id));
            });

            if (!running_) break;

            // If this worker should pause (playback started), re-queue current work and wait
            if (!ShouldWorkerRun(worker_id)) {
                RequeueCurrentTask(worker_id);
                // Wait for resume signal (unlock queue_mutex, wait on worker_cv_)
                worker_cv_.wait(lock, [this, worker_id]() {
                    return !running_ || ShouldWorkerRun(worker_id);
                });
                if (!running_) break;
                continue;
            }

            if (!request_queue_.empty()) {
                request = request_queue_.front();
                request_queue_.pop_front();
                queued_keys_.erase(request.key);
                in_progress_.insert(request.key);
                have_request = true;

                // Track current task for this worker (for potential re-queuing)
                {
                    std::lock_guard<std::mutex> task_lock(worker_task_mutex_);
                    worker_current_task_[worker_id] = request.key;
                }
            }
        }

        if (have_request) {
            // Capture generation before doing work — if Clear() fires during load, discard result
            uint64_t gen_before = clear_generation_.load(std::memory_order_acquire);

            // Load thumbnail pixels (uses per-worker loaders)
            auto pixels = LoadThumbnailPixels(request.key, worker_id);

            // Check if Clear() was called while we were loading — discard stale result
            if (clear_generation_.load(std::memory_order_acquire) != gen_before) {
                std::lock_guard<std::mutex> lock(queue_mutex_);
                in_progress_.erase(request.key);
                continue;
            }

            if (pixels && running_) {
#if defined(QCVIEW_USE_VULKAN) || defined(QCVIEW_USE_METAL)
                // Metal/Vulkan: create GPU texture on worker thread (API is thread-safe).
                // Main thread just inserts the texture ID into the cache (zero GPU work).
                GLuint texture_id = CreateGLTexture(pixels);
                if (texture_id != 0) {
                    std::lock_guard<std::mutex> lock(upload_mutex_);
                    pending_uploads_.push_back({request.key, nullptr, texture_id,
                                                pixels->width, pixels->height});
                }
#else
                // OpenGL: queue pixel data for main thread texture creation
                std::lock_guard<std::mutex> lock(upload_mutex_);
                pending_uploads_.push_back({request.key, pixels});
#endif
            }

            // Remove from in-progress and clear current task
            {
                std::lock_guard<std::mutex> lock(queue_mutex_);
                in_progress_.erase(request.key);
            }
            {
                std::lock_guard<std::mutex> task_lock(worker_task_mutex_);
                worker_current_task_.erase(worker_id);
            }
        }
    }

    // No Debug::Log here — logger may be destroyed during global static destruction
}

bool TimelineThumbnailCache::ShouldWorkerRun(int worker_id) const {
    // Worker 0 always runs; others only when not playing
    return worker_id == 0 || !is_playing_.load();
}

void TimelineThumbnailCache::RequeueCurrentTask(int worker_id) {
    // Note: queue_mutex_ is already held by caller (WorkerThread)
    // We only need to lock worker_task_mutex_
    std::lock_guard<std::mutex> lock(worker_task_mutex_);

    auto it = worker_current_task_.find(worker_id);
    if (it != worker_current_task_.end()) {
        TimelineThumbnailKey key = it->second;
        worker_current_task_.erase(it);

        // Re-queue at front (high priority)
        // queue_mutex_ is already held by caller, so direct access is safe
        in_progress_.erase(key);
        request_queue_.push_front({key, /*high_priority=*/true});
    }
}

void TimelineThumbnailCache::NotifyPlaybackState(bool is_playing) {
    bool was_playing = is_playing_.exchange(is_playing);

    if (is_playing && !was_playing) {
        // Playback started - reduce to 1 worker (worker 0 only)
        active_thread_count_ = kPlaybackWorkerThreads;
        worker_state_ = ThumbnailWorkerState::PAUSED_PLAYBACK;
        queue_cv_.notify_all();  // Wake workers to check state and pause

        Debug::Log("TimelineThumbnailCache: Playback started - reducing to " +
                   std::to_string(kPlaybackWorkerThreads) + " worker(s)");

    } else if (!is_playing && was_playing) {
        // Playback stopped - resume all workers
        active_thread_count_ = kMaxWorkerThreads;
        worker_state_ = ThumbnailWorkerState::ACTIVE;
        worker_cv_.notify_all();  // Wake paused workers
        queue_cv_.notify_all();   // Also notify queue in case work is waiting

        Debug::Log("TimelineThumbnailCache: Playback stopped - resuming all " +
                   std::to_string(kMaxWorkerThreads) + " workers");
    }
}

std::shared_ptr<TimelineThumbnailCache::LoaderInfo>
TimelineThumbnailCache::GetOrCreateLoader(const std::string& source_path, int worker_id) {
    std::lock_guard<std::mutex> lock(loaders_mutex_);

    // Check if this worker already has a loader for this source
    auto& worker_map = worker_loaders_[worker_id];
    auto it = worker_map.find(source_path);
    if (it != worker_map.end()) {
        return it->second;
    }

    // Create new loader based on file type (per-worker to avoid FFmpeg contention)
    auto info = std::make_shared<LoaderInfo>();

    // Parse out ?layer= parameter if present (for EXR files)
    std::string file_path = source_path;
    std::string layer_name;
    size_t layer_pos = source_path.find("?layer=");
    if (layer_pos != std::string::npos) {
        file_path = source_path.substr(0, layer_pos);
        layer_name = source_path.substr(layer_pos + 7);  // Skip "?layer="
    }

    // Detect media type from extension (use file_path without query params)
    size_t dot_pos = file_path.find_last_of('.');
    std::string ext = (dot_pos != std::string::npos) ? file_path.substr(dot_pos + 1) : "";
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    bool is_video = (ext == "mov" || ext == "mp4" || ext == "mxf" ||
                     ext == "avi" || ext == "mkv" || ext == "m4v");

    if (is_video) {
        info->is_video = true;
        // Create per-worker VideoImageLoader - each worker has its own FFmpeg context
        info->image_loader = std::make_unique<VideoImageLoader>(file_path, fps_, 0.0);
        Debug::Log("TimelineThumbnailCache: Worker " + std::to_string(worker_id) +
                   " created VideoImageLoader for " + file_path);
    } else {
        info->is_video = false;
        // Create appropriate image loader
        if (ext == "exr") {
            auto exr_loader = std::make_unique<EXRImageLoader>();
            // Set layer if specified
            if (!layer_name.empty()) {
                exr_loader->SetLayer(layer_name);
                Debug::Log("TimelineThumbnailCache: Worker " + std::to_string(worker_id) +
                           " created EXRImageLoader with layer '" + layer_name + "'");
            }
            info->image_loader = std::move(exr_loader);
        } else if (ext == "tiff" || ext == "tif") {
            info->image_loader = std::make_unique<TIFFImageLoader>();
        } else if (ext == "png") {
            info->image_loader = std::make_unique<PNGImageLoader>();
        } else if (ext == "jpg" || ext == "jpeg") {
            info->image_loader = std::make_unique<JPEGImageLoader>();
        }
    }

    worker_map[source_path] = info;
    return info;
}

std::shared_ptr<PixelData> TimelineThumbnailCache::LoadThumbnailPixels(const TimelineThumbnailKey& key, int worker_id) {
    auto loader_info = GetOrCreateLoader(key.source_path, worker_id);
    if (!loader_info || !loader_info->image_loader) {
        //Debug::Log("TimelineThumbnailCache::LoadThumbnailPixels: No loader for " + key.source_path);
        return nullptr;
    }

    // Strip ?layer= from path for actual file loading (layer already set on loader)
    std::string file_path = key.source_path;
    size_t layer_pos = file_path.find("?layer=");
    if (layer_pos != std::string::npos) {
        file_path = file_path.substr(0, layer_pos);
    }

    // Use cached loader for both video and image sequences
    // For video: image_loader is a VideoImageLoader (created in GetOrCreateLoader)
    // For images: image_loader is an EXRImageLoader/TIFFImageLoader/etc.
    int max_thumb_size = std::max(config_.width, config_.height);

    /*Debug::Log("TimelineThumbnailCache::LoadThumbnailPixels: Worker " + std::to_string(worker_id) +
               " loading frame " + std::to_string(key.source_frame) + " from " + file_path +
               " (is_video=" + std::to_string(loader_info->is_video) + ")");*/

    if (loader_info->is_video) {
        // Video: pass frame number as string to LoadThumbnail
        auto result = loader_info->image_loader->LoadThumbnail(std::to_string(key.source_frame), max_thumb_size);
        if (result) {
            /*Debug::Log("TimelineThumbnailCache::LoadThumbnailPixels: SUCCESS - got " +
                       std::to_string(result->width) + "x" + std::to_string(result->height));*/
        } else {
           /* Debug::Log("TimelineThumbnailCache::LoadThumbnailPixels: FAILED for frame " +
                       std::to_string(key.source_frame));*/
        }
        return result;
    } else {
        // Image sequence: pass the file path directly (layer already set on EXRImageLoader)
        return loader_info->image_loader->LoadThumbnail(file_path, max_thumb_size);
    }
}

GLuint TimelineThumbnailCache::CreateGLTexture(const std::shared_ptr<PixelData>& pixels) {
    if (!pixels || pixels->pixels.empty()) {
        return 0;
    }

#ifdef QCVIEW_USE_VULKAN
    VkFormat vk_format = VK_FORMAT_R8G8B8A8_UNORM;
    switch (pixels->pixel_format) {
        case PixelFormat::RGBA8:  vk_format = VK_FORMAT_R8G8B8A8_UNORM; break;
        case PixelFormat::RGBA16: vk_format = VK_FORMAT_R16G16B16A16_UNORM; break;
        case PixelFormat::RGBA16F: vk_format = VK_FORMAT_R16G16B16A16_SFLOAT; break;
    }
    uint64_t pool_id = qcview::VulkanTexturePool::Instance().CreateTextureFromPixels(
        pixels->width, pixels->height, vk_format,
        pixels->pixels.data(), pixels->pixels.size());
    return static_cast<GLuint>(pool_id);
#elif defined(QCVIEW_USE_METAL)
    int mtl_format = 0;
    switch (pixels->pixel_format) {
        case PixelFormat::RGBA8:  mtl_format = 0; break;
        case PixelFormat::RGBA16: mtl_format = 0; break;
        case PixelFormat::RGBA16F: mtl_format = 1; break;
    }
    uint64_t pool_id = qcview::MetalTexturePool::ThumbnailInstance().CreateTextureFromPixels(
        pixels->width, pixels->height, mtl_format,
        pixels->pixels.data(), pixels->pixels.size());
    return static_cast<GLuint>(pool_id);
#else
    // Save current GL state to avoid corrupting ImGui during render
    GLint previous_texture = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous_texture);

    GLuint texture_id = 0;
    glGenTextures(1, &texture_id);
    if (texture_id == 0) {
        glBindTexture(GL_TEXTURE_2D, previous_texture);  // Restore even on failure
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

    // Restore previous texture binding (critical for ImGui compatibility)
    glBindTexture(GL_TEXTURE_2D, previous_texture);

    return texture_id;
#endif
}

void TimelineThumbnailCache::EvictLRU() {
    if (lru_order_.empty()) return;

    // LRU item is at the back
    TimelineThumbnailKey lru_key = lru_order_.back();
    lru_order_.pop_back();

    auto it = cache_.find(lru_key);
    if (it != cache_.end()) {
        if (it->second.texture_id != 0) {
#ifdef QCVIEW_USE_VULKAN
            qcview::VulkanTexturePool::Instance().QueueDelete(static_cast<uint64_t>(it->second.texture_id));
#elif defined(QCVIEW_USE_METAL)
            qcview::MetalTexturePool::ThumbnailInstance().QueueDelete(static_cast<uint64_t>(it->second.texture_id));
#else
            glDeleteTextures(1, &it->second.texture_id);
#endif
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

} // namespace qcview
