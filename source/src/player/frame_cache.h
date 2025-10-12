#pragma once

#include <glad/gl.h>
#include <unordered_map>
#include <memory>
#include <atomic>
#include <mutex>
#include <thread>
#include <chrono>
#include <string>
#include <queue>
#include <condition_variable>
#include <vector>
#include <set>

class VideoPlayer;
class GPUFrameCache;
class MediaBackgroundExtractor;
struct VideoMetadata;
#include "pipeline_mode.h"

// Forward declaration - defined in mpv_extractor.h
struct MPVConversionStrategy;
// GPU conversion strategy removed - no longer needed

// Removed: Disk cache forward declarations (simplified to RAM-only cache)

struct CachedFrame {
    GLuint texture_id = 0;
    int width = 0;
    int height = 0;
    double timestamp = 0.0;
    // Removed: memory_size tracking (memory-based eviction removed)
    std::chrono::steady_clock::time_point last_accessed;
    bool is_valid = false;
    
    // Store pixel data until texture creation on main thread
    std::vector<uint8_t> pixel_data;
    bool texture_created = false;
    PipelineMode pipeline_mode = PipelineMode::NORMAL;  // Store pipeline mode for texture creation
    
    CachedFrame() = default;
    ~CachedFrame();
    
    // Non-copyable but movable
    CachedFrame(const CachedFrame&) = delete;
    CachedFrame& operator=(const CachedFrame&) = delete;
    CachedFrame(CachedFrame&& other) noexcept;
    CachedFrame& operator=(CachedFrame&& other) noexcept;
    
    void CreateTexture(int w, int h, const void* data, PipelineMode pipeline_mode = PipelineMode::NORMAL);
    void ReleaseTexture();
    bool EnsureTextureCreated(); // Create texture from pixel_data if not already created
    // Removed: GetMemorySize() method (memory-based eviction removed)
};

class FrameCache {
public:
    struct CacheConfig {
        // RAM Cache settings
        int max_cache_seconds = 20;             // Maximum seconds of video to cache (NEW: replaces memory limit)
        bool use_centered_caching = true;       // true = center around seekbar, false = sequential full video
        int cache_width = 1920;                 // Fixed width for consistent quality
        int cache_height = -1;                  // Calculate from video aspect ratio
        int background_thread_priority = -1;    // Lower priority for background extraction
        // TEMPORARY: Keyframe cache settings for compilation - will be removed
        bool enable_keyframe_cache = false;      // Disabled - background extractor handles all caching
        size_t keyframe_cache_size_mb = 0;       // Not used
        bool adaptive_threading = true;         // Slow down during playback
        int max_extractions_per_second = 100;   // Maximum extraction rate for fastest caching

        // Hardware decode settings
        bool enable_nvidia_decode = false;      // Use NVDEC when available (checkbox setting)
        bool prefer_d3d11va = true;            // Default to D3D11VA for broader compatibility
        int max_batch_size = 8;                // Frames per extraction batch (smaller for responsiveness)
        int max_concurrent_batches = 8;        // Parallel batch processing threads (more parallelism)
        bool pause_during_playback = true;     // Don't steal resources during playback

        // Pipeline format settings
        PipelineMode pipeline_mode = PipelineMode::NORMAL;  // Current pipeline mode for texture format

        // Removed: Disk cache settings (simplified to RAM-only cache)
    };

    explicit FrameCache(const CacheConfig& config = CacheConfig{});
    ~FrameCache();

    // Main interface
    bool GetCachedFrame(double timestamp, GLuint& texture_id, int& width, int& height);
    void UpdateScrubPosition(double timestamp, VideoPlayer* video_player);
    void NotifyPlaybackState(bool is_playing); // Inform cache about playback state
    void SetVideoFile(const std::string& video_path, const VideoMetadata* metadata = nullptr);
    void UpdateVideoMetadata(const std::string& video_path, const VideoMetadata& metadata);
    void InvalidateCache();
    void SetCacheConfig(const CacheConfig& config);
    
    // Cache statistics
    struct CacheStats {
        size_t total_frames_cached = 0;
        // Removed: memory_used_mb (memory-based eviction removed)
        size_t cache_hits = 0;
        size_t cache_misses = 0;
        float hit_ratio = 0.0f;
        double coverage_start = 0.0;
        double coverage_end = 0.0;
    };
    CacheStats GetStats() const;
    
    // TEMPORARY: Cache visualization structure kept for compilation - will be removed
    struct CacheSegment {
        double start_time = 0.0;
        double end_time = 0.0;
        float density = 1.0f;  // 0.0 = sparse, 1.0 = full coverage
        enum Type {
            SCRUB_CACHE,        // RAM cache (green)
            KEYFRAME_CACHE      // Keyframe cache (blue)
        } type = SCRUB_CACHE;
    };
    std::vector<CacheSegment> GetCacheSegments() const;
    
    // Control
    // EXR PATTERN: Removed PauseBackgroundCaching, ResumeBackgroundCaching, RestartBackgroundThread
    // EXR PATTERN: Removed StartBackgroundCaching, StopBackgroundCaching
    // Thread is created once in constructor, runs permanently until destructor
    void SetCachingEnabled(bool enabled); // Enable/disable caching operations (thread keeps running)
    void ClearCachedFrames(); // Clear all cached frames but keep cache structure
    bool IsBackgroundCachingActive() const { return background_thread_active; }
    bool IsInitialized() const;

    // Frame processing interface (no longer opportunistic - only processes background results)
    bool TryCacheCurrentFrame(VideoPlayer* video_player); // Called from main render loop

    // Get current pipeline mode (may be updated by auto-detection)
    PipelineMode GetPipelineMode() const { return config.pipeline_mode; }

    // Background extractor integration
    void AddExtractedFrame(int frame_number, double timestamp, GLuint texture_id, int width, int height); // Called by background extractor
    void AddExtractedFrame(int frame_number, double timestamp, const std::vector<uint8_t>& pixel_data, int width, int height, bool from_native_image = false); // Called by background extractor with pixel data
    bool IsFrameCached(int frame_number) const; // Check if frame is already cached

    // Removed: Disk cache interface (simplified to RAM-only cache)

private:
    CacheConfig config;
    
    // TEMPORARY: Keep old frame storage for compilation - will be removed after cleanup
    std::unordered_map<int, std::unique_ptr<CachedFrame>> scrub_cache;    // Frame number -> cached frame
    std::unordered_map<int, std::unique_ptr<CachedFrame>> keyframe_cache; // Keyframe cache for long seeks
    mutable std::mutex cache_mutex;
    
    // Removed: Memory management tracking (memory-based eviction removed)
    std::atomic<size_t> cache_hits{0};
    std::atomic<size_t> cache_misses{0};
    
    // Background caching
    std::thread background_thread;
    std::atomic<bool> background_thread_active{false};
    std::atomic<bool> shutdown_requested{false};
    std::atomic<bool> caching_enabled{true};
    std::atomic<double> current_scrub_position{0.0};
    std::atomic<bool> main_player_is_playing{false};
    VideoPlayer* cached_video_player = nullptr;
    
    // Rate limiting removed - only RAM limit constrains caching
    
    // Frame extraction - UPDATED: using media background processing for maximum performance
    std::unique_ptr<MediaBackgroundExtractor> background_extractor;
    std::string current_video_path;

    // Removed: Disk cache members (simplified to RAM-only cache)

    // GPU conversion strategy removed - background extractor handles metadata

    // Sequential caching state
    std::atomic<int> sequential_cache_position{0}; // Track where we left off in sequential scan
    std::atomic<bool> sequential_cache_complete{false}; // Track if we've cached all frames

    // Removed: Frame request queue (opportunistic caching removed)

    // Removed: Immediate disk caching threads (simplified to RAM-only cache)

    
    // Internal methods
    void BackgroundCacheWorker();
    void ExtractFrameAtPosition(double timestamp, VideoPlayer* video_player);

    // Removed: Disk caching worker methods (simplified to RAM-only cache)

    // Removed: Opportunistic caching methods (using only spiral background caching)

    // RAM cache management (simplified from 3-tier to RAM-only)
    bool GetFrameFromRAM(int frame_number, GLuint& texture_id, int& width, int& height);

    int TimestampToFrameNumber(double timestamp, double fps) const;
    double FrameNumberToTimestamp(int frame_number, double fps) const;
    void EvictOldFrames();
    void EvictFramesBeyondWindow(double center_timestamp, double window_seconds);
    // Removed: EvictFramesFarthestFromSeekbar() (memory-based eviction removed)

    // Seconds-based cache management (NEW: replaces memory-based)
    void EvictFramesBeyondSeconds(double center_timestamp, int max_seconds);
    bool ShouldCacheFrame(int frame_number, double current_frame) const;
    // Removed: EstimateFrameSize() and EvictOldestFrames() (memory-based eviction removed)

    // OpenGL helpers - UPDATED for GPU copying
    bool ExtractFrameFromCurrentTexture(VideoPlayer* video_player, double timestamp,
                                       GLuint& texture_id, int& width, int& height);
    bool ReadTexturePixels(GLuint texture_id, int width, int height, std::vector<uint8_t>& pixels);
};