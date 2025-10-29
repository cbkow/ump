#pragma once

#include <glad/gl.h>
#include <string>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <chrono>

namespace ump {

/**
 * DifferenceCache
 *
 * Loads pre-computed difference PNG sequences from disk.
 * Supports two modes:
 *   - DISK_FALLBACK: LRU RAM cache with disk fallback (memory-efficient)
 *   - RAM_ONLY: Pre-load entire sequence into RAM (EXR cache style, fastest playback)
 *
 * RAM_ONLY mode:
 *   - Loads all frames at initialization (or smart chunks based on RAM)
 *   - Zero disk I/O during playback
 *   - Always 60fps, no cache misses
 */
class DifferenceCache {
public:
    enum class CacheMode {
        DISK_FALLBACK,  // LRU RAM cache with disk fallback
        RAM_ONLY        // Pre-load all frames (EXR cache style)
    };
    struct CachedDifference {
        GLuint texture_id = 0;
        int width = 0;
        int height = 0;
        std::chrono::steady_clock::time_point last_accessed;
        bool is_valid = false;

        void ReleaseTexture();
    };

    DifferenceCache();
    ~DifferenceCache();

    // Initialize with combined cache directory from TranscodeManager
    bool Initialize(
        const std::string& combined_cache_dir,
        int total_frames,
        double frame_rate,
        CacheMode mode = CacheMode::RAM_ONLY  // Default to RAM-only for best performance
    );

    void Cleanup();

    // Frame access (call from RenderDifference)
    bool GetDifferenceTexture(int frame_number, GLuint& texture_id, int& width, int& height);

    // Configuration
    void SetMaxCacheSize(size_t max_mb) { max_cache_size_mb_ = max_mb; }

    // Stats
    struct CacheStats {
        size_t total_cached_frames = 0;
        size_t cache_hits = 0;
        size_t cache_misses = 0;
        size_t disk_loads = 0;
        float hit_ratio = 0.0f;
    };
    CacheStats GetStats() const;

    // Check if sequences exist on disk
    bool AreSequencesReady() const;

private:
    // Combined cache directory (pre-computed differences)
    std::string combined_cache_dir_;
    int total_frames_ = 0;
    double frame_rate_ = 30.0;
    CacheMode cache_mode_ = CacheMode::RAM_ONLY;

    // RAM cache (frame_number -> CachedDifference)
    std::unordered_map<int, std::unique_ptr<CachedDifference>> ram_cache_;
    mutable std::mutex cache_mutex_;
    size_t max_cache_size_mb_ = 2048;  // 2GB default

    // Statistics
    mutable std::atomic<size_t> cache_hits_{0};
    mutable std::atomic<size_t> cache_misses_{0};
    mutable std::atomic<size_t> disk_loads_{0};

    // Initialized flag
    bool initialized_ = false;

    // Frame loading from disk (pre-computed difference PNG)
    GLuint LoadPNGTexture(const std::string& path, int& width, int& height);
    std::string GetDifferenceFramePath(int frame_number) const;

    // Pre-loading (RAM_ONLY mode)
    bool PreloadAllFrames();

    // LRU eviction (DISK_FALLBACK mode)
    void EvictLRUFrames();
    size_t CalculateCacheMemoryUsage() const;
};

} // namespace ump
