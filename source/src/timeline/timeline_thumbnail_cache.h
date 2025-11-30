#pragma once

#include <memory>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <list>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <atomic>
#include <deque>
#include <glad/gl.h>
#include "../player/image_loader_interface.h"
#include "../player/shared_memory_pool.h"

namespace ump {

// Forward declarations
class StreamingVideoDecoder;

//=============================================================================
// Timeline Thumbnail Key - Identifies a frame from a source clip
//=============================================================================

struct TimelineThumbnailKey {
    std::string source_path;    // Source media file path
    int source_frame;           // Frame number within the source

    bool operator<(const TimelineThumbnailKey& other) const {
        if (source_path != other.source_path) return source_path < other.source_path;
        return source_frame < other.source_frame;
    }

    bool operator==(const TimelineThumbnailKey& other) const {
        return source_path == other.source_path && source_frame == other.source_frame;
    }
};

//=============================================================================
// Configuration
//=============================================================================

struct TimelineThumbnailConfig {
    int width = 320;               // Thumbnail width in pixels
    int height = 180;              // Thumbnail height in pixels
    int cache_size = 100;          // Maximum number of thumbnails to cache
    bool enabled = true;           // Enable/disable thumbnail generation
};

//=============================================================================
// Cached Thumbnail Entry
//=============================================================================

struct TimelineThumbnailEntry {
    GLuint texture_id = 0;
    int width = 0;
    int height = 0;
};

//=============================================================================
// TimelineThumbnailCache - Independent LRU cache for trim/slip preview
//=============================================================================

/**
 * TimelineThumbnailCache - Separate thumbnail cache for timeline editing previews
 *
 * Unlike the main TimelineCache which uses window-based eviction around the playhead,
 * this cache uses simple LRU eviction and can hold thumbnails for ANY frame from
 * ANY source file, regardless of playhead position.
 *
 * This enables trim/slip preview to show frames from anywhere in the source clip,
 * not just within the playback cache window.
 *
 * Memory footprint:
 * - 320x180 RGBA8 = ~230 KB per thumbnail
 * - 100 thumbnails = ~23 MB
 */
class TimelineThumbnailCache {
public:
    TimelineThumbnailCache();
    ~TimelineThumbnailCache();

    // Initialize the cache
    void Initialize(double fps);

    // Shutdown and cleanup
    void Shutdown();

    /**
     * Get thumbnail for a specific source file + frame (non-blocking)
     * @param source_path - Path to the source media file
     * @param source_frame - Frame number within the source
     * @param width - Output: thumbnail width (0 if not cached)
     * @param height - Output: thumbnail height (0 if not cached)
     * @param allow_fallback - If true, return nearest cached frame as preview
     * @return OpenGL texture ID, or 0 if not yet available
     *
     * Note: Returns 0 immediately if not cached (and no fallback), queues async generation
     */
    GLuint GetThumbnail(const std::string& source_path, int source_frame,
                        int& width, int& height, bool allow_fallback = true);

    /**
     * Start precaching thumbnails for a clip range (call when user clicks on clip)
     * @param source_path - Path to the source media file
     * @param start_frame - First frame to precache
     * @param end_frame - Last frame to precache
     * @param priority_frame - Frame to load first (current position)
     * @param step - Step size for precaching (e.g., every 6 frames)
     */
    void PrecacheRange(const std::string& source_path, int start_frame, int end_frame,
                       int priority_frame = -1, int step = 6);

    /**
     * Process pending thumbnails (MUST be called from main/GL thread)
     * Uploads generated pixel data to GL textures
     */
    void ProcessPendingUploads();

    /**
     * Clear all cached thumbnails and delete GL textures
     * Call this when mouse is released after trim/drag
     */
    void Clear();

    /**
     * Cancel all pending requests (useful when switching clips)
     */
    void CancelPendingRequests();

    /**
     * Check if thumbnail cache is enabled
     */
    bool IsEnabled() const { return config_.enabled; }

    /**
     * Get cache statistics
     */
    struct Stats {
        int total_cached = 0;
        int cache_hits = 0;
        int cache_misses = 0;
        int pending_requests = 0;
    };
    Stats GetStats() const;

private:
    // Background worker thread
    void WorkerThread();

    // Generate thumbnail pixels (runs on background thread)
    std::shared_ptr<PixelData> LoadThumbnailPixels(const TimelineThumbnailKey& key);

    // Create GL texture from pixels (runs on main thread only)
    GLuint CreateGLTexture(const std::shared_ptr<PixelData>& pixels);

    // Evict least-recently-used thumbnail if cache is full
    void EvictLRU();

    // Find nearest cached frame for fallback preview (same source path)
    int FindNearestCachedFrame(const std::string& source_path, int target_frame) const;

    // Get or create loader for a source path
    struct LoaderInfo {
        std::unique_ptr<IImageLoader> image_loader;
        std::unique_ptr<StreamingVideoDecoder> video_decoder;
        bool is_video = false;
        int frame_count = 0;
        double fps = 24.0;
    };
    std::shared_ptr<LoaderInfo> GetOrCreateLoader(const std::string& source_path);

    // Configuration
    TimelineThumbnailConfig config_;
    double fps_ = 24.0;
    bool initialized_ = false;

    // LRU Cache: key -> thumbnail entry
    std::map<TimelineThumbnailKey, TimelineThumbnailEntry> cache_;
    std::list<TimelineThumbnailKey> lru_order_;  // Front = most recently used
    mutable std::mutex cache_mutex_;

    // Source loaders (reused across requests)
    std::map<std::string, std::shared_ptr<LoaderInfo>> loaders_;
    mutable std::mutex loaders_mutex_;

    // Request queue
    struct ThumbnailRequest {
        TimelineThumbnailKey key;
        bool high_priority = true;  // User requests are always high priority
    };
    std::deque<ThumbnailRequest> request_queue_;
    std::set<TimelineThumbnailKey> in_progress_;
    mutable std::mutex queue_mutex_;
    std::condition_variable queue_cv_;

    // Pending uploads (pixel data ready for GL texture creation)
    struct PendingUpload {
        TimelineThumbnailKey key;
        std::shared_ptr<PixelData> pixels;
    };
    std::deque<PendingUpload> pending_uploads_;
    mutable std::mutex upload_mutex_;

    // Worker thread
    std::thread worker_thread_;
    std::atomic<bool> running_{false};

    // Statistics
    std::atomic<int> cache_hits_{0};
    std::atomic<int> cache_misses_{0};
};

} // namespace ump
