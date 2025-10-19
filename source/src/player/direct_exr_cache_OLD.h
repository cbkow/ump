#pragma once

#include <string>
#include <vector>
#include <memory>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <future>
#include <map>
#include <deque>
#include <list>
#include <condition_variable>

#include <glad/gl.h>
#include <half.h>

// Forward declarations for OpenEXR (using versioned namespace directly)
namespace Imf_3_3 {
    class ChannelList;
}

namespace ump {

// Forward declarations
class GPUTexturePool;

//=============================================================================
// Cache Segment (for timeline visualization)
//=============================================================================

struct CacheSegment {
    enum Type { SCRUB_CACHE, LOOKAHEAD_CACHE };
    double start_time = 0.0;
    double end_time = 0.0;
    Type type = SCRUB_CACHE;
    float density = 1.0f;
};

//=============================================================================
// Cache Configuration
//=============================================================================

enum class CacheDirection {
    Forward,    // Cache ahead during playback
    Reverse     // Cache behind during reverse playback
};

struct DirectEXRCacheConfig {
    // Cache size and behavior
    double video_cache_gb = 18.0;             // Total cache size in GB (default 18GB)
    float read_behind_seconds = 0.5f;         // Seconds to cache behind playhead

    // Threading configuration (tlRender-style)
    size_t thread_count = 16;                 // Parallel EXR load count
    size_t max_pending_requests = 64;         // Request queue depth

    // GPU texture pooling
    int gpu_memory_pool_mb = 2048;            // GPU memory pool size
    int gpu_max_textures = 1000;              // Max pooled textures
    int gpu_texture_ttl_seconds = 300;        // Texture time-to-live

    bool IsValid() const {
        return video_cache_gb >= 2.0 && video_cache_gb <= 96.0 &&
               read_behind_seconds >= 0.0f && read_behind_seconds <= 5.0f &&
               gpu_memory_pool_mb >= 512 && gpu_memory_pool_mb <= 8192 &&
               thread_count >= 1 && thread_count <= 32 &&
               max_pending_requests >= thread_count;
    }
};

//=============================================================================
// Cache Frame Data
//=============================================================================

// Pixel data loaded from EXR (CPU-side, can be loaded on background thread)
struct DirectEXRPixelData {
    std::vector<half> pixels;  // RGBA half-float pixels
    int width = 0;
    int height = 0;
    std::string file_path;
    std::string layer_name;
};

struct DirectEXRFrame {
    GLuint texture_id = 0;
    int width = 0;
    int height = 0;
    size_t byte_count = 0;                    // For LRU sizing
    bool is_pooled_texture = false;
    std::chrono::steady_clock::time_point last_accessed;

    std::string file_path;                    // For debugging
    std::string layer_name;

    ~DirectEXRFrame();
};

//=============================================================================
// Simple LRU Cache (inspired by tlRender's ftk::LRUCache)
//=============================================================================

template<typename K, typename V>
class SimpleLRUCache {
public:
    SimpleLRUCache() : max_size_bytes_(0), current_size_bytes_(0) {}

    void SetMaxSize(size_t max_bytes) {
        std::lock_guard<std::mutex> lock(mutex_);
        max_size_bytes_ = max_bytes;
        EvictIfNeeded();
    }

    size_t GetMaxSize() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return max_size_bytes_;
    }

    size_t GetSize() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return current_size_bytes_;
    }

    bool Contains(const K& key) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return cache_.find(key) != cache_.end();
    }

    bool Get(const K& key, V& value) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = cache_.find(key);
        if (it != cache_.end()) {
            value = it->second;
            Touch(key);
            return true;
        }
        return false;
    }

    void Add(const K& key, const V& value, size_t byte_count) {
        std::lock_guard<std::mutex> lock(mutex_);

        // Remove existing if present
        auto it = cache_.find(key);
        if (it != cache_.end()) {
            current_size_bytes_ -= sizes_[key];
            cache_.erase(it);
            sizes_.erase(key);
        }

        // Add new entry
        cache_[key] = value;
        sizes_[key] = byte_count;
        current_size_bytes_ += byte_count;

        // Update LRU tracking
        Touch(key);

        // Evict if over limit
        EvictIfNeeded();
    }

    void Touch(const K& key) {
        // Move to end (most recently used)
        lru_list_.remove(key);
        lru_list_.push_back(key);
    }

    void Clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        cache_.clear();
        sizes_.clear();
        lru_list_.clear();
        current_size_bytes_ = 0;
    }

    std::vector<K> GetKeys() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<K> keys;
        keys.reserve(cache_.size());
        for (const auto& pair : cache_) {
            keys.push_back(pair.first);
        }
        return keys;
    }

private:
    void EvictIfNeeded() {
        // Evict least recently used items until under limit
        while (current_size_bytes_ > max_size_bytes_ && !lru_list_.empty()) {
            K oldest_key = lru_list_.front();
            lru_list_.pop_front();

            auto it = cache_.find(oldest_key);
            if (it != cache_.end()) {
                current_size_bytes_ -= sizes_[oldest_key];
                cache_.erase(it);
                sizes_.erase(oldest_key);
            }
        }
    }

    mutable std::mutex mutex_;
    std::map<K, V> cache_;
    std::map<K, size_t> sizes_;
    std::list<K> lru_list_;
    size_t max_size_bytes_;
    size_t current_size_bytes_;
};

//=============================================================================
// Direct EXR Cache (tlRender-based architecture)
//=============================================================================

class DirectEXRCache {
public:
    struct CacheStats {
        int total_frames_in_sequence = 0;
        int frames_cached = 0;
        double cache_percentage = 0.0;
        size_t memory_usage_mb = 0;
        bool background_thread_active = false;

        // Request stats
        int cache_hits = 0;
        int cache_misses = 0;
        double hit_ratio = 0.0;

        // Performance
        double average_load_time_ms = 0.0;
        int total_frames_loaded = 0;
        int requests_pending = 0;        // NEW: Pending in queue
        int requests_in_progress = 0;    // NEW: Currently loading
    };

public:
    DirectEXRCache();
    ~DirectEXRCache();

    // Initialization
    bool Initialize(const std::vector<std::string>& sequence_files,
                   const std::string& layer_name, double fps);
    void Shutdown();

    // Main cache interface (matches old API for easy migration)
    bool GetFrame(int frame_index, GLuint& texture_id, int& width, int& height);

    // Synchronous load - tries cache first, loads on miss
    bool GetFrameOrLoad(int frame_index, GLuint& texture_id, int& width, int& height);

    void UpdateCurrentPosition(double timestamp);
    void UpdatePlaybackState(bool is_playing);

    // Configuration
    void SetConfig(const DirectEXRCacheConfig& config);
    DirectEXRCacheConfig GetConfig() const;
    void SetCacheWindow(double seconds) { /* For API compatibility */ }

    // Cache control
    void StartBackgroundCaching();
    void StopBackgroundCaching();
    void SetCachingEnabled(bool enabled);
    bool IsCachingEnabled() const { return caching_enabled_; }
    void ClearCache();

    // Stats and monitoring
    CacheStats GetStats() const;
    bool IsInitialized() const { return is_initialized_; }
    std::vector<CacheSegment> GetCacheSegments() const;

    // Texture creation (must be called from main thread with GL context)
    void ProcessReadyTextures();

    // Static utility
    static bool GetFrameDimensions(const std::string& file_path, int& width, int& height);

private:
    //=========================================================================
    // tlRender-style Request Management
    //=========================================================================

    struct PixelLoadRequest {
        int frame_index;
        uint64_t id;  // Unique ID for tracking
        std::future<std::shared_ptr<DirectEXRPixelData>> future;
        size_t byte_count;
    };

    struct PlaybackState {
        double current_time = 0.0;
        bool is_playing = false;
        CacheDirection direction = CacheDirection::Forward;

        bool operator==(const PlaybackState& other) const {
            return current_time == other.current_time &&
                   is_playing == other.is_playing &&
                   direction == other.direction;
        }

        bool operator!=(const PlaybackState& other) const {
            return !(*this == other);
        }
    };

    struct Mutex {
        PlaybackState state;
        bool clear_requests = false;
        bool clear_cache = false;
        std::mutex mutex;
    };

    struct Thread {
        // Request queues (tlRender pattern)
        std::deque<int> pending_frame_requests;     // Frames waiting to be loaded
        std::list<PixelLoadRequest> in_progress_requests;  // Currently loading

        // Cache fill tracking (instance variables, NOT static)
        int video_fill_frame = 0;
        size_t video_fill_byte_count = 0;

        // State tracking
        PlaybackState state;

        // Threading
        std::thread thread;
        std::condition_variable cv;  // For instant wake (tlRender pattern)
        std::atomic<bool> running{false};

        // Timing
        std::chrono::steady_clock::time_point log_timer;
    };

    struct PendingPixelData {
        int frame_index;
        std::shared_ptr<DirectEXRPixelData> pixel_data;
    };

    //=========================================================================
    // Member Variables
    //=========================================================================

    // Core state
    bool is_initialized_ = false;
    std::vector<std::string> sequence_files_;
    std::string selected_layer_;
    double fps_ = 24.0;

    // Configuration
    DirectEXRCacheConfig config_;
    mutable std::mutex config_mutex_;

    // LRU Cache (frame_index -> frame data)
    SimpleLRUCache<int, std::shared_ptr<DirectEXRFrame>> frame_cache_;

    // Threading (tlRender pattern)
    Mutex mutex_;
    Thread thread_;

    // Texture creation queue (main thread only)
    std::vector<PendingPixelData> ready_for_texture_creation_;
    mutable std::mutex texture_queue_mutex_;

    // Playback state atomics
    std::atomic<bool> caching_enabled_{true};
    std::atomic<uint64_t> next_request_id_{1};

    // GPU texture pooling
    std::unique_ptr<GPUTexturePool> texture_pool_;

    // Statistics
    mutable std::atomic<int> cache_hits_{0};
    mutable std::atomic<int> cache_misses_{0};
    mutable std::atomic<int> total_frames_loaded_{0};
    mutable std::vector<double> load_times_;
    mutable std::mutex stats_mutex_;

    // Cached dimensions (avoid reopening files)
    mutable int cached_width_ = 0;
    mutable int cached_height_ = 0;
    mutable bool dimensions_cached_ = false;

    //=========================================================================
    // Private Methods (tlRender pattern)
    //=========================================================================

    // Thread main loop
    void CacheThreadMain();

    // Cache update (tlRender's cacheUpdate pattern)
    void CacheUpdate();

    // Request management
    void CancelPendingRequests();

    // Cache window calculation (dynamic, relative to playhead)
    int GetCacheStartFrame() const;
    int GetCacheEndFrame() const;
    int GetCacheFrameCount() const;
    size_t CalculateFrameByteCount(int width, int height) const;

    // OpenEXR loading (unchanged)
    std::shared_ptr<DirectEXRPixelData> LoadPixelsFromEXR(const std::string& file_path,
                                                          const std::string& layer_name);
    std::shared_ptr<DirectEXRFrame> LoadFrameDirect(const std::string& file_path,
                                                     const std::string& layer_name);
    std::shared_ptr<DirectEXRFrame> CreateFrameFromPixels(std::shared_ptr<DirectEXRPixelData> pixel_data);

    // Channel name resolution (Blender multi-layer support)
    std::string FindChannelName(const Imf_3_3::ChannelList& channels,
                               const std::string& layer_name,
                               const std::string& component) const;
    std::vector<std::string> GenerateChannelPatterns(const std::string& layer_name,
                                                     const std::string& component) const;

    // GPU management
    void InitializeTexturePool();
    GLuint CreateTextureFromPixels(const std::vector<half>& pixels, int width, int height);
};

} // namespace ump
