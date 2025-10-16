#include "thumbnail_cache.h"
#include "../utils/debug_utils.h"
#include <algorithm>
#include <cmath>
#include <glad/gl.h>

// Prevent Windows min/max macros from conflicting with Imath
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

#include <Imath/half.h>

// STB image resize for high-quality thumbnail downscaling
// Note: STB_IMAGE_RESIZE_IMPLEMENTATION is already defined in exr_transcoder.cpp
#include "../../external/stb/stb_image_resize2.h"

namespace ump {

ThumbnailCache::ThumbnailCache(
    std::vector<std::string> sequence_files,
    std::unique_ptr<IImageLoader> loader,
    const ThumbnailConfig& config
)
    : config_(config)
    , loader_(std::move(loader))
    , sequence_files_(std::move(sequence_files))
{
    Debug::Log("ThumbnailCache: Initialized with " + std::to_string(sequence_files_.size()) +
               " files, " + std::to_string(config_.width) + "x" + std::to_string(config_.height) +
               " thumbnails, cache size: " + std::to_string(config_.cache_size));

    if (!config_.enabled) {
        Debug::Log("ThumbnailCache: Disabled by configuration");
        return;
    }

    // Start background worker thread
    worker_thread_ = std::thread(&ThumbnailCache::WorkerThread, this);
    Debug::Log("ThumbnailCache: Started async worker thread");
}

ThumbnailCache::~ThumbnailCache() {
    Debug::Log("ThumbnailCache: Destructor - stopping worker thread");

    // Signal shutdown and wake worker thread
    Debug::Log("ThumbnailCache: Setting shutdown flag to true");
    shutdown_.store(true);
    Debug::Log("ThumbnailCache: Notifying worker thread to wake up");
    queue_cv_.notify_all();

    // Wait for worker thread to finish
    if (worker_thread_.joinable()) {
        Debug::Log("ThumbnailCache: Waiting for worker thread to join (this may block if thread is stuck)...");
        worker_thread_.join();
        Debug::Log("ThumbnailCache: Worker thread joined successfully");
    } else {
        Debug::Log("ThumbnailCache: Worker thread was not joinable");
    }

    Debug::Log("ThumbnailCache: Clearing cache...");
    ClearCache();
    Debug::Log("ThumbnailCache: Destructor complete");
}

GLuint ThumbnailCache::GetThumbnail(int frame, bool allow_fallback) {
    if (!config_.enabled) {
        return 0;
    }

    if (frame < 0 || frame >= static_cast<int>(sequence_files_.size())) {
        return 0;  // Out of bounds
    }

    std::lock_guard<std::mutex> lock(cache_mutex_);

    // Check cache for exact frame first
    auto it = cache_.find(frame);
    if (it != cache_.end()) {
        cache_hits_++;
        it->second->access_count++;
        return it->second->texture_id;  // Exact match!
    }

    // Cache miss - queue this frame with HIGH priority (on-demand request)
    cache_misses_++;

    {
        std::lock_guard<std::mutex> queue_lock(queue_mutex_);

        // Only queue if not already requested
        if (requested_frames_.find(frame) == requested_frames_.end()) {
            request_queue_.push({frame, RequestPriority::HIGH});
            requested_frames_.insert(frame);
            queue_cv_.notify_one();  // Wake worker thread
        }
    }

    // If fallback enabled, try to find nearest cached frame as preview
    if (allow_fallback && config_.use_nearest_neighbor_fallback) {
        int nearest = FindNearestCachedFrame(frame);
        if (nearest >= 0) {
            auto nearest_it = cache_.find(nearest);
            if (nearest_it != cache_.end()) {
                // Return nearest cached thumbnail as preview
                return nearest_it->second->texture_id;
            }
        }
    }

    return 0;  // Not ready yet, will be available after ProcessPendingUploads()
}

void ThumbnailCache::CancelPendingRequests() {
    std::lock_guard<std::mutex> lock(queue_mutex_);

    // Clear all pending requests (priority_queue doesn't have clear())
    while (!request_queue_.empty()) {
        request_queue_.pop();
    }
    requested_frames_.clear();
}

// Worker thread function - runs in background
void ThumbnailCache::WorkerThread() {
    Debug::Log("ThumbnailCache: Worker thread started");

    while (!shutdown_.load()) {
        int frame = -1;

        // Wait for work
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait(lock, [this]() {
                return shutdown_.load() || !request_queue_.empty();
            });

            if (shutdown_.load()) break;

            if (!request_queue_.empty()) {
                // Get highest priority request
                ThumbnailRequest req = request_queue_.top();
                request_queue_.pop();
                frame = req.frame;
            }
        }

        // Generate thumbnail pixels (CPU-only, no GL calls)
        if (frame >= 0) {
            auto pending = GenerateThumbnailPixels(frame);

            if (pending) {
                // Add to pending uploads queue for main thread
                std::lock_guard<std::mutex> lock(queue_mutex_);
                pending_uploads_.push(std::move(pending));
            }

            // Remove from requested set
            std::lock_guard<std::mutex> lock(queue_mutex_);
            requested_frames_.erase(frame);
        }
    }

    Debug::Log("ThumbnailCache: Worker thread stopped");
}

// Generate thumbnail pixel data (runs on background thread)
std::unique_ptr<PendingThumbnail> ThumbnailCache::GenerateThumbnailPixels(int frame) {
    if (!loader_) {
        generation_failures_++;
        return nullptr;
    }

    const std::string& file_path = sequence_files_[frame];

    // Use LoadThumbnail() for optimized low-resolution decode
    // This bypasses expensive color management and uses format-specific optimizations
    int max_thumb_size = (std::max)(config_.width, config_.height);
    auto pixel_data = loader_->LoadThumbnail(file_path, max_thumb_size);
    if (!pixel_data || pixel_data->pixels.empty()) {
        Debug::Log("ThumbnailCache: Failed to load thumbnail " + std::to_string(frame) + ": " + file_path);
        generation_failures_++;
        return nullptr;
    }

    // Calculate thumbnail dimensions maintaining aspect ratio
    int source_width = pixel_data->width;
    int source_height = pixel_data->height;
    float source_aspect = static_cast<float>(source_width) / source_height;
    float target_aspect = static_cast<float>(config_.width) / config_.height;

    int thumb_width = config_.width;
    int thumb_height = config_.height;

    if (source_aspect > target_aspect) {
        // Source is wider - fit width, adjust height
        thumb_height = static_cast<int>(config_.width / source_aspect);
    } else {
        // Source is taller - fit height, adjust width
        thumb_width = static_cast<int>(config_.height * source_aspect);
    }

    // Allocate buffer for resized thumbnail
    std::vector<uint8_t> thumbnail_pixels;
    GLenum thumbnail_gl_type;

    if (pixel_data->gl_type == GL_HALF_FLOAT) {
        // EXR thumbnails - keep as half-float to preserve HDR data for OCIO color management
        Debug::Log("ThumbnailCache: Generating HDR half-float thumbnail for frame " + std::to_string(frame));

        thumbnail_pixels.resize(thumb_width * thumb_height * 4 * sizeof(Imath::half));
        thumbnail_gl_type = GL_HALF_FLOAT;

        // Convert half → float for stb_image_resize (which doesn't support half directly)
        std::vector<float> source_float(source_width * source_height * 4);
        std::vector<float> thumb_float(thumb_width * thumb_height * 4);

        const Imath::half* src_half = reinterpret_cast<const Imath::half*>(pixel_data->pixels.data());
        for (size_t i = 0; i < source_float.size(); i++) {
            // Explicit half→float conversion to avoid linker issues
            uint16_t bits = src_half[i].bits();
            int sign = (bits >> 15) & 0x1;
            int exp = (bits >> 10) & 0x1F;
            int mantissa = bits & 0x3FF;

            if (exp == 0) {
                // Denormalized or zero
                source_float[i] = (sign ? -1.0f : 1.0f) * (mantissa / 1024.0f) * powf(2.0f, -14.0f);
            } else if (exp == 31) {
                // Inf or NaN
                source_float[i] = (mantissa == 0) ?
                    (sign ? -INFINITY : INFINITY) : NAN;
            } else {
                // Normalized
                float val = (1.0f + mantissa / 1024.0f) * powf(2.0f, exp - 15.0f);
                source_float[i] = sign ? -val : val;
            }
        }

        // Resize in float space (preserves HDR values)
        stbir_resize_float_linear(
            source_float.data(), source_width, source_height, 0,
            thumb_float.data(), thumb_width, thumb_height, 0,
            STBIR_RGBA
        );

        // Convert float → half for storage
        Imath::half* thumb_half = reinterpret_cast<Imath::half*>(thumbnail_pixels.data());
        for (size_t i = 0; i < thumb_float.size(); i++) {
            thumb_half[i] = Imath::half(thumb_float[i]);
        }

    } else if (pixel_data->gl_type == GL_UNSIGNED_BYTE) {
        // 8-bit source (PNG8, JPEG) - direct resize
        thumbnail_pixels.resize(thumb_width * thumb_height * 4);
        thumbnail_gl_type = GL_UNSIGNED_BYTE;

        stbir_resize_uint8_linear(
            pixel_data->pixels.data(), source_width, source_height, 0,
            thumbnail_pixels.data(), thumb_width, thumb_height, 0,
            STBIR_RGBA
        );

    } else if (pixel_data->gl_type == GL_UNSIGNED_SHORT) {
        // 16-bit integer source (PNG16, TIFF16) - convert to 8-bit
        thumbnail_pixels.resize(thumb_width * thumb_height * 4);
        thumbnail_gl_type = GL_UNSIGNED_BYTE;

        std::vector<uint8_t> source_8bit(source_width * source_height * 4);
        const uint16_t* source_16 = reinterpret_cast<const uint16_t*>(pixel_data->pixels.data());
        for (size_t i = 0; i < source_8bit.size(); i++) {
            source_8bit[i] = static_cast<uint8_t>(source_16[i] >> 8);
        }

        stbir_resize_uint8_linear(
            source_8bit.data(), source_width, source_height, 0,
            thumbnail_pixels.data(), thumb_width, thumb_height, 0,
            STBIR_RGBA
        );
    } else {
        // Unknown format
        Debug::Log("ThumbnailCache: Unknown pixel format for frame " + std::to_string(frame));
        generation_failures_++;
        return nullptr;
    }

    // Create pending thumbnail for main thread upload
    auto pending = std::make_unique<PendingThumbnail>();
    pending->frame = frame;
    pending->width = thumb_width;
    pending->height = thumb_height;
    pending->pixels = std::move(thumbnail_pixels);
    pending->gl_format = GL_RGBA;
    pending->gl_type = thumbnail_gl_type;  // GL_HALF_FLOAT for EXR, GL_UNSIGNED_BYTE for others

    return pending;
}

// Create GL texture from pixels (runs on main thread only)
GLuint ThumbnailCache::CreateGLTexture(const PendingThumbnail& pending) {
    GLuint texture_id = 0;
    glGenTextures(1, &texture_id);
    if (texture_id == 0) {
        Debug::Log("ThumbnailCache: Failed to create GL texture for frame " + std::to_string(pending.frame));
        generation_failures_++;
        return 0;
    }

    // Select internal format based on pixel type
    GLenum internal_format = (pending.gl_type == GL_HALF_FLOAT) ? GL_RGBA16F : GL_RGBA8;

    glBindTexture(GL_TEXTURE_2D, texture_id);
    glTexImage2D(GL_TEXTURE_2D, 0, internal_format, pending.width, pending.height, 0,
                 pending.gl_format, pending.gl_type, pending.pixels.data());

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glBindTexture(GL_TEXTURE_2D, 0);

    return texture_id;
}

// Process pending uploads (MUST be called from main/GL thread)
void ThumbnailCache::ProcessPendingUploads() {
    std::queue<std::unique_ptr<PendingThumbnail>> uploads_to_process;

    // Grab all pending uploads
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        uploads_to_process.swap(pending_uploads_);
    }

    if (uploads_to_process.empty()) {
        return;  // Nothing to process
    }

   /* Debug::Log("ThumbnailCache::ProcessPendingUploads: Processing " +
               std::to_string(uploads_to_process.size()) + " pending thumbnails");*/

    // Process uploads (create GL textures and add to cache)
    int uploaded_count = 0;
    while (!uploads_to_process.empty()) {
        auto pending = std::move(uploads_to_process.front());
        uploads_to_process.pop();

        GLuint texture_id = CreateGLTexture(*pending);

        if (texture_id != 0) {
            std::lock_guard<std::mutex> lock(cache_mutex_);

            // Evict LRU entry if cache is full
            if (static_cast<int>(cache_.size()) >= config_.cache_size) {
                EvictLRU();
            }

            // Add to cache
            auto entry = std::make_unique<ThumbnailEntry>();
            entry->texture_id = texture_id;
            entry->width = pending->width;
            entry->height = pending->height;
            entry->access_count = 0;  // Will be incremented on next GetThumbnail()
            cache_[pending->frame] = std::move(entry);
            uploaded_count++;

            /*Debug::Log("ThumbnailCache: Uploaded frame " + std::to_string(pending->frame) +
                       " -> GL texture " + std::to_string(texture_id));*/
        }
    }

   /* Debug::Log("ThumbnailCache::ProcessPendingUploads: Uploaded " + std::to_string(uploaded_count) +
               " thumbnails, cache now has " + std::to_string(cache_.size()) + " entries");*/
}

void ThumbnailCache::EvictLRU() {
    if (cache_.empty()) {
        return;
    }

    // Find entry with lowest access count (LRU)
    auto lru_it = cache_.begin();
    int min_access = lru_it->second->access_count;

    for (auto it = cache_.begin(); it != cache_.end(); ++it) {
        if (it->second->access_count < min_access) {
            min_access = it->second->access_count;
            lru_it = it;
        }
    }

    // Evict (destructor will delete GL texture)
    cache_.erase(lru_it);
}

ThumbnailCache::Stats ThumbnailCache::GetStats() const {
    Stats stats;

    {
        std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(cache_mutex_));
        stats.total_cached = static_cast<int>(cache_.size());
    }

    {
        std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(queue_mutex_));
        stats.pending_requests = static_cast<int>(request_queue_.size() + pending_uploads_.size());
    }

    stats.cache_hits = cache_hits_.load();
    stats.cache_misses = cache_misses_.load();
    stats.generation_failures = generation_failures_.load();

    return stats;
}

void ThumbnailCache::ClearCache() {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    cache_.clear();  // Unique_ptr destructors will delete GL textures

    // Reset stats
    cache_hits_ = 0;
    cache_misses_ = 0;
    generation_failures_ = 0;

    Debug::Log("ThumbnailCache: Cache cleared");
}

int ThumbnailCache::FindNearestCachedFrame(int target_frame) const {
    // Note: cache_mutex_ should already be locked by caller
    if (cache_.empty()) {
        return -1;
    }

    int min_distance = INT_MAX;
    int nearest_frame = -1;

    for (const auto& [frame_num, entry] : cache_) {
        int distance = std::abs(frame_num - target_frame);
        if (distance < min_distance) {
            min_distance = distance;
            nearest_frame = frame_num;
        }
    }

    return nearest_frame;
}

void ThumbnailCache::PrefetchStrategicFrames(int total_frames) {
    if (!config_.enabled || config_.prefetch_count <= 0) {
        Debug::Log("ThumbnailCache: Prefetch disabled or count is 0");
        return;
    }

    if (total_frames <= 0) {
        Debug::Log("ThumbnailCache: No frames to prefetch (total_frames=" + std::to_string(total_frames) + ")");
        return;
    }

    // Calculate evenly-distributed frame indices
    std::vector<int> prefetch_frames;
    int step = (std::max)(1, total_frames / config_.prefetch_count);

    for (int i = 0; i < config_.prefetch_count && i * step < total_frames; i++) {
        prefetch_frames.push_back(i * step);
    }

    // Queue all prefetch frames with LOW priority
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);

        for (int frame : prefetch_frames) {
            // Only queue if not already requested
            if (requested_frames_.find(frame) == requested_frames_.end()) {
                request_queue_.push({frame, RequestPriority::LOW});
                requested_frames_.insert(frame);
            }
        }

        // Wake worker thread if needed
        if (!prefetch_frames.empty()) {
            queue_cv_.notify_one();
        }
    }

    Debug::Log("ThumbnailCache: Queued " + std::to_string(prefetch_frames.size()) +
               " strategic prefetch frames (step=" + std::to_string(step) + ")");
}

} // namespace ump
