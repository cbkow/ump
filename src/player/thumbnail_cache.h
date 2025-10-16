#pragma once

#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <atomic>
#include <queue>
#include <glad/gl.h>
#include "image_loader_interface.h"

namespace ump {

// Configuration for thumbnail generation
struct ThumbnailConfig {
    int width = 320;               // Thumbnail width in pixels
    int height = 180;              // Thumbnail height in pixels
    int cache_size = 100;          // Maximum number of thumbnails to cache
    bool enabled = true;           // Enable/disable thumbnail generation
    int prefetch_count = 25;       // Number of strategic frames to prefetch on load
    bool use_nearest_neighbor_fallback = true;  // Show nearest cached frame as preview
};

// Simple LRU cache entry for thumbnails
struct ThumbnailEntry {
    GLuint texture_id = 0;         // OpenGL texture ID
    int width = 0;                 // Actual thumbnail width
    int height = 0;                // Actual thumbnail height
    int access_count = 0;          // For LRU tracking

    ~ThumbnailEntry() {
        if (texture_id != 0) {
            glDeleteTextures(1, &texture_id);
            texture_id = 0;
        }
    }
};

// Thumbnail pixel data waiting for GL texture creation (main thread only)
struct PendingThumbnail {
    int frame = 0;
    int width = 0;
    int height = 0;
    std::vector<uint8_t> pixels;  // Raw pixel data (format determined by gl_type)
    GLenum gl_format = GL_RGBA;   // Always GL_RGBA
    GLenum gl_type = GL_UNSIGNED_BYTE;  // GL_UNSIGNED_BYTE (8-bit) or GL_HALF_FLOAT (16-bit HDR)
};

/**
 * ThumbnailCache - Generates and caches small preview thumbnails for timeline scrubbing
 *
 * Features:
 * - RAM-only caching (no disk persistence)
 * - LRU eviction when cache is full
 * - ASYNC generation on background thread (non-blocking UI)
 * - Works with all IImageLoader formats (EXR/TIFF/PNG/JPEG)
 * - Configurable thumbnail size and cache capacity
 * - Thread-safe GL texture upload on main thread
 *
 * Memory footprint:
 * - 320x180 RGBA8 = ~230 KB per thumbnail
 * - 100 thumbnails = ~23 MB (acceptable)
 */
class ThumbnailCache {
public:
    /**
     * Create thumbnail cache for image sequences
     * @param sequence_files - List of file paths (must be sorted)
     * @param loader - Image loader for this format (EXR/TIFF/PNG/JPEG)
     * @param config - Thumbnail configuration (size, cache capacity, etc.)
     */
    ThumbnailCache(
        std::vector<std::string> sequence_files,
        std::unique_ptr<IImageLoader> loader,
        const ThumbnailConfig& config
    );

    ~ThumbnailCache();

    /**
     * Get thumbnail for a specific frame (non-blocking)
     * @param frame - Frame number (0-based index into sequence_files)
     * @param allow_fallback - If true, return nearest cached frame as preview
     * @return OpenGL texture ID, or 0 if not yet available
     *
     * Note: Returns 0 immediately if not cached, queues async generation
     */
    GLuint GetThumbnail(int frame, bool allow_fallback = false);

    /**
     * Cancel all pending requests (useful when jumping to different timeline position)
     */
    void CancelPendingRequests();

    /**
     * Process pending thumbnails (MUST be called from main/GL thread)
     * Uploads generated pixel data to GL textures
     */
    void ProcessPendingUploads();

    /**
     * Prefetch evenly-distributed frames for timeline preview
     * @param total_frames - Total number of frames in sequence
     *
     * Queues strategic frames (e.g., 25 frames evenly distributed) at low priority
     * for background generation. Creates illusion of instant thumbnails via
     * nearest-neighbor fallback.
     */
    void PrefetchStrategicFrames(int total_frames);

    /**
     * Get thumbnail dimensions (may differ from config if aspect ratio adjusted)
     */
    void GetThumbnailSize(int& width, int& height) const {
        width = config_.width;
        height = config_.height;
    }

    /**
     * Check if thumbnail cache is enabled
     */
    bool IsEnabled() const { return config_.enabled; }

    /**
     * Get cache statistics
     */
    struct Stats {
        int total_cached = 0;      // Number of thumbnails currently cached
        int cache_hits = 0;        // Number of cache hits
        int cache_misses = 0;      // Number of cache misses (queued)
        int generation_failures = 0; // Number of failed generations
        int pending_requests = 0;  // Number of requests in queue
    };
    Stats GetStats() const;

    /**
     * Clear all cached thumbnails (useful when changing settings)
     */
    void ClearCache();

private:
    // Background worker thread function
    void WorkerThread();

    // Generate thumbnail pixel data (runs on background thread)
    std::unique_ptr<PendingThumbnail> GenerateThumbnailPixels(int frame);

    // Create GL texture from pixels (runs on main thread only)
    GLuint CreateGLTexture(const PendingThumbnail& pending);

    // Evict least-recently-used thumbnail if cache is full
    void EvictLRU();

    // Find nearest cached frame for fallback preview
    int FindNearestCachedFrame(int target_frame) const;

    // Configuration
    ThumbnailConfig config_;

    // Image loader (EXR/TIFF/PNG/JPEG)
    std::unique_ptr<IImageLoader> loader_;

    // Sequence files (sorted)
    std::vector<std::string> sequence_files_;

    // Cache: frame number -> thumbnail entry
    std::unordered_map<int, std::unique_ptr<ThumbnailEntry>> cache_;
    mutable std::mutex cache_mutex_;

    // Request priority levels
    enum class RequestPriority {
        LOW = 0,      // Prefetch requests
        HIGH = 1      // On-demand user requests
    };

    // Request with priority
    struct ThumbnailRequest {
        int frame;
        RequestPriority priority;

        // Higher priority values processed first
        bool operator<(const ThumbnailRequest& other) const {
            return priority < other.priority;
        }
    };

    // Async generation
    std::priority_queue<ThumbnailRequest> request_queue_;  // Priority queue for requests
    std::unordered_set<int> requested_frames_;  // Deduplication set
    std::queue<std::unique_ptr<PendingThumbnail>> pending_uploads_;  // Ready for GL upload
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::thread worker_thread_;
    std::atomic<bool> shutdown_{false};

    // Statistics
    std::atomic<int> cache_hits_{0};
    std::atomic<int> cache_misses_{0};
    std::atomic<int> generation_failures_{0};
};

} // namespace ump
