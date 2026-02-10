#pragma once

#include <string>
#include <map>
#include <list>
#include <mutex>
#include <functional>
#include <atomic>

namespace ump {

// Global cache key for identifying entries across all cache types
struct GlobalCacheKey {
    enum class Source {
        TIMELINE,           // TimelineCache entries
        STANDALONE_EXR,     // DirectEXRCache entries
        STANDALONE_VIDEO    // FrameCache entries
    };

    Source source;
    std::string path;       // Source file path
    int frame;              // Frame number within source

    bool operator<(const GlobalCacheKey& other) const {
        if (source != other.source) return source < other.source;
        if (path != other.path) return path < other.path;
        return frame < other.frame;
    }

    bool operator==(const GlobalCacheKey& other) const {
        return source == other.source && path == other.path && frame == other.frame;
    }
};

// Entry metadata stored in the pool
struct PoolEntry {
    size_t bytes;
    std::function<void()> eviction_callback;
};

// Statistics for monitoring
struct PoolStats {
    size_t budget_bytes = 0;
    size_t used_bytes = 0;
    size_t timeline_entries = 0;
    size_t exr_entries = 0;
    size_t video_entries = 0;
    size_t eviction_count = 0;

    double GetUsageRatio() const {
        return budget_bytes > 0 ? static_cast<double>(used_bytes) / budget_bytes : 0.0;
    }
};

// SharedMemoryPool: Global LRU coordinator for all caches
//
// This class provides a unified memory budget across multiple cache types
// (TimelineCache, DirectEXRCache, FrameCache). When memory pressure occurs,
// the oldest entries across ALL caches are evicted, regardless of which
// cache originally created them.
//
// Usage:
//   1. Call SetBudgetGB() to configure total memory limit
//   2. Each cache calls RegisterEntry() when adding new data
//   3. Each cache calls TouchEntry() on cache hits to update LRU
//   4. Pool automatically calls eviction callbacks when over budget
//   5. Each cache calls RemoveEntry() when manually removing data
//
class SharedMemoryPool {
public:
    // Singleton accessor
    static SharedMemoryPool& Instance();

    // Disable copy/move
    SharedMemoryPool(const SharedMemoryPool&) = delete;
    SharedMemoryPool& operator=(const SharedMemoryPool&) = delete;
    SharedMemoryPool(SharedMemoryPool&&) = delete;
    SharedMemoryPool& operator=(SharedMemoryPool&&) = delete;

    // Configuration
    void SetBudgetGB(double gb);
    double GetBudgetGB() const;
    void SetEnabled(bool enabled);
    bool IsEnabled() const { return enabled_; }

    // Entry management
    // RegisterEntry: Add or update an entry in the pool
    // - If key exists, updates bytes and moves to end of LRU
    // - Triggers eviction if over budget
    // - eviction_callback is called when this entry is evicted (to free resources)
    void RegisterEntry(const GlobalCacheKey& key, size_t bytes,
                       std::function<void()> eviction_callback);

    // TouchEntry: Update LRU position without changing size
    // - Call on cache hits to keep entry fresh
    void TouchEntry(const GlobalCacheKey& key);

    // RemoveEntry: Explicitly remove an entry (without calling eviction callback)
    // - Call when cache removes entry for non-memory reasons
    void RemoveEntry(const GlobalCacheKey& key);

    // Check if entry exists
    bool HasEntry(const GlobalCacheKey& key) const;

    // Statistics
    PoolStats GetStats() const;

    // Clear all entries (calls eviction callbacks)
    void Clear();

    // Clear entries from a specific source (calls eviction callbacks)
    void ClearSource(GlobalCacheKey::Source source);

private:
    SharedMemoryPool();
    ~SharedMemoryPool();

    // Internal: evict oldest entries until under budget
    void EvictIfNeeded();

    // Internal: remove entry from LRU list
    void RemoveFromLRU(const GlobalCacheKey& key);

    mutable std::mutex mutex_;

    // Configuration
    std::atomic<size_t> budget_bytes_{0};
    std::atomic<bool> enabled_{true};

    // Entry storage
    std::map<GlobalCacheKey, PoolEntry> entries_;

    // LRU list (front = oldest, back = newest)
    std::list<GlobalCacheKey> lru_list_;

    // Current usage
    size_t used_bytes_ = 0;

    // Stats
    std::atomic<size_t> eviction_count_{0};
};

// Helper to create keys for different sources
inline GlobalCacheKey MakeTimelineKey(const std::string& path, int frame) {
    return {GlobalCacheKey::Source::TIMELINE, path, frame};
}

inline GlobalCacheKey MakeEXRKey(const std::string& path, int frame) {
    return {GlobalCacheKey::Source::STANDALONE_EXR, path, frame};
}

inline GlobalCacheKey MakeVideoKey(const std::string& path, int frame) {
    return {GlobalCacheKey::Source::STANDALONE_VIDEO, path, frame};
}

} // namespace ump
