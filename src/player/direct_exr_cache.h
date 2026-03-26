#pragma once

#include <string>
#include <vector>
#include <memory>
#include <thread>
#include <mutex>
#include <shared_mutex>
#include <condition_variable>
#include <future>
#include <map>
#include <deque>
#include <functional>
#include <chrono>
#include <unordered_map>
#include <unordered_set>
#include <atomic>

#include <glad/gl.h>
#include <half.h>

#include <OpenEXR/ImfIO.h>

#include "image_loader_interface.h"
#include "pipeline_mode.h"
#include "shared_memory_pool.h"

#ifdef _WIN32
    #include <windows.h>
#endif

namespace qcview {

//=============================================================================
// Memory-Mapped IStream (tlRender pattern) - Shared utility
//=============================================================================

class MemoryMappedIStream : public Imf::IStream {
public:
    MemoryMappedIStream(const std::string& fileName);
    ~MemoryMappedIStream() override;

    bool isMemoryMapped() const override { return true; }
    char* readMemoryMapped(int n) override;
    bool read(char c[], int n) override;
    uint64_t tellg() override;
    void seekg(uint64_t pos) override;

private:
    std::string filePath_;
    char* mappedData_ = nullptr;
    uint64_t fileSize_ = 0;
    uint64_t currentPos_ = 0;

#ifdef _WIN32
    HANDLE hFile_ = INVALID_HANDLE_VALUE;
    HANDLE hMapping_ = NULL;
#else
    int fd_ = -1;
#endif
};

//=============================================================================
// Clean DirectEXRCache
// Zero legacy code. Minimal state. Fast.
//=============================================================================

// Configuration
struct EXRCacheConfig {
    // Parallel I/O threads for sequences
    // Helps with slow multilayer EXRs (31 channels, DWAB compression ~900ms/frame)
    size_t threadCount = 16;

    // Frame-based cache window (replaces GB-based sizing)
    int readAheadFrames = 72;          // Frames to cache ahead of playhead (~3s @ 24fps)
    int readBehindFrames = 12;         // Frames to keep behind playhead (~0.5s @ 24fps)

    int playbackStride = 1;            // 1 = every frame, 2 = every other, 3 = every 3rd, 4 = every 4th

    // SharedMemoryPool integration
    bool use_shared_pool = false;      // Use global SharedMemoryPool instead of local LRU

    bool IsValid() const {
        return threadCount >= 1 && threadCount <= 32 &&
               readAheadFrames >= 12 && readAheadFrames <= 600 &&
               readBehindFrames >= 0 && readBehindFrames <= 120;
    }
};

// Alias for VideoPlayer compatibility
using DirectEXRCacheConfig = EXRCacheConfig;

// Cache direction for bi-directional caching (tlRender pattern)
enum class CacheDirection {
    Forward,   // Cache ahead of playhead
    Reverse    // Cache behind playhead
};

// Cache segment info (for UI visualization)
struct CacheSegment {
    int start_frame = 0;
    int end_frame = 0;
    double start_time = 0.0;  // For old code compatibility
    double end_time = 0.0;
    double density = 1.0;
};

// Aligned allocator for SIMD optimization
template<typename T, std::size_t Alignment = 64>
class AlignedAllocator {
public:
    using value_type = T;
    using pointer = T*;
    using const_pointer = const T*;
    using reference = T&;
    using const_reference = const T&;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;

    // Rebind allocator to different type
    template<typename U>
    struct rebind {
        using other = AlignedAllocator<U, Alignment>;
    };

    AlignedAllocator() = default;
    AlignedAllocator(const AlignedAllocator&) = default;
    template<typename U>
    AlignedAllocator(const AlignedAllocator<U, Alignment>&) {}

    T* allocate(std::size_t n) {
#ifdef _WIN32
        void* p = _aligned_malloc(n * sizeof(T), Alignment);
        if (!p) throw std::bad_alloc();
        return static_cast<T*>(p);
#else
        void* p = nullptr;
        if (posix_memalign(&p, Alignment, n * sizeof(T)) != 0) throw std::bad_alloc();
        return static_cast<T*>(p);
#endif
    }

    void deallocate(T* p, std::size_t) {
#ifdef _WIN32
        _aligned_free(p);
#else
        free(p);
#endif
    }

    // Required for C++17 compatibility
    template<typename U>
    bool operator==(const AlignedAllocator<U, Alignment>&) const { return true; }

    template<typename U>
    bool operator!=(const AlignedAllocator<U, Alignment>&) const { return false; }
};

// GL texture (GPU-side, main thread only)
struct EXRTexture {
    GLuint texture_id = 0;
    int width = 0;
    int height = 0;
    size_t byteCount = 0;

    // NOTE: GL textures are NOT deleted in destructor because this can be called
    // from any thread. Instead, DirectEXRCache queues textures for deletion
    // and ProcessReadyTextures() deletes them on the main thread.
};

// Simple LRU cache
template<typename K, typename V>
class SimpleLRU {
public:
    using EvictionCallback = std::function<void(const K& key, const V& value)>;

    void SetMaxSize(size_t bytes) { maxBytes_ = bytes; }
    size_t GetMaxSize() const { return maxBytes_; }
    size_t GetSize() const { return currentBytes_; }
    size_t GetCount() const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return cache_.size();
    }
    void SetEvictionCallback(EvictionCallback callback) { evictionCallback_ = callback; }

    bool Contains(const K& key) const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return cache_.find(key) != cache_.end();
    }

    bool Get(const K& key, V& value) {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        auto it = cache_.find(key);
        if (it != cache_.end()) {
            value = it->second;
            Touch(key);
            return true;
        }
        return false;
    }

    bool Get(const K& key, V& value) const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        auto it = cache_.find(key);
        if (it != cache_.end()) {
            value = it->second;
            // Don't touch in const version
            return true;
        }
        return false;
    }

    // Peek without updating LRU (for playback - don't keep old frames fresh)
    bool Peek(const K& key, V& value) const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        auto it = cache_.find(key);
        if (it != cache_.end()) {
            value = it->second;
            return true;
        }
        return false;
    }

    void Add(const K& key, const V& value, size_t bytes) {
        // Collect evicted entries, fire callbacks OUTSIDE the lock
        // to prevent priority inversion (eviction callback may acquire other mutexes)
        std::vector<std::pair<K, V>> evicted;

        {
            std::unique_lock<std::shared_mutex> lock(mutex_);

            // Remove old entry
            auto it = cache_.find(key);
            if (it != cache_.end()) {
                currentBytes_ -= sizes_[key];
                cache_.erase(it);
                sizes_.erase(key);
            }

            // Add new
            cache_[key] = value;
            sizes_[key] = bytes;
            currentBytes_ += bytes;
            Touch(key);

            // Evict if needed — collect evicted entries
            while (currentBytes_ > maxBytes_ && !lruList_.empty()) {
                K oldest = lruList_.front();
                lruList_.pop_front();
                iterMap_.erase(oldest);

                auto eit = cache_.find(oldest);
                if (eit != cache_.end()) {
                    evicted.emplace_back(oldest, eit->second);
                }

                currentBytes_ -= sizes_[oldest];
                cache_.erase(oldest);
                sizes_.erase(oldest);
            }
        } // mutex_ released

        // Fire eviction callbacks outside the lock
        if (evictionCallback_) {
            for (auto& [k, v] : evicted) {
                evictionCallback_(k, v);
            }
        }
    }

    void Clear() {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        cache_.clear();
        sizes_.clear();
        lruList_.clear();
        iterMap_.clear();
        currentBytes_ = 0;
    }

    // Remove without returning the value (for eviction without texture deletion callback)
    void Remove(const K& key) {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        auto it = cache_.find(key);
        if (it != cache_.end()) {
            currentBytes_ -= sizes_[key];
            cache_.erase(it);
            sizes_.erase(key);
            auto iit = iterMap_.find(key);
            if (iit != iterMap_.end()) {
                lruList_.erase(iit->second);
                iterMap_.erase(iit);
            }
        }
    }

    // Remove and return the value (so caller can extract GL texture ID for deletion)
    bool RemoveAndGet(const K& key, V& value) {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        auto it = cache_.find(key);
        if (it != cache_.end()) {
            value = it->second;
            currentBytes_ -= sizes_[key];
            cache_.erase(it);
            sizes_.erase(key);
            auto iit = iterMap_.find(key);
            if (iit != iterMap_.end()) {
                lruList_.erase(iit->second);
                iterMap_.erase(iit);
            }
            return true;
        }
        return false;
    }

    std::vector<K> GetKeys() const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        std::vector<K> keys;
        for (const auto& p : cache_) keys.push_back(p.first);
        return keys;
    }

private:
    void Touch(const K& key) {
        auto it = iterMap_.find(key);
        if (it != iterMap_.end()) {
            lruList_.erase(it->second);
        }
        lruList_.push_back(key);
        iterMap_[key] = std::prev(lruList_.end());
    }

    mutable std::shared_mutex mutex_;
    std::map<K, V> cache_;
    std::map<K, size_t> sizes_;
    std::list<K> lruList_;
    std::unordered_map<K, typename std::list<K>::iterator> iterMap_;  // O(1) Touch/Remove
    size_t maxBytes_ = 0;
    size_t currentBytes_ = 0;
    EvictionCallback evictionCallback_;
};

//=============================================================================
// DirectEXRCache - Clean Implementation
//=============================================================================

class DirectEXRCache {
public:
    DirectEXRCache();
    ~DirectEXRCache();

    // Initialize with sequence (original EXR method - preserved for compatibility)
    // initial_position: Start caching from this position (seconds) instead of 0
    bool Initialize(const std::vector<std::string>& files,
                   const std::string& layer,
                   double fps,
                   int start_frame = 0,
                   double initial_position = 0.0);

    // Initialize with universal loader (NEW - supports TIFF/PNG/JPEG/EXR)
    // initial_position: Start caching from this position (seconds) instead of 0
    bool Initialize(std::unique_ptr<IImageLoader> loader,
                   const std::vector<std::string>& files,
                   const std::string& layer,
                   double fps,
                   PipelineMode pipeline_mode,
                   int start_frame = 0,
                   double initial_position = 0.0);

    void Shutdown();

    // Request a frame (returns immediately with future)
    // Request returns future, worker thread fulfills it
    void RequestFrame(int frame);

    // Get cached texture (returns 0 if not ready)
    GLuint GetTexture(int frame, int& width, int& height);

    // Compatibility method for old GetFrameOrLoad interface
    bool GetFrameOrLoad(int frame, GLuint& texture, int& width, int& height);

    // Process completed pixel loads (MUST call from main thread with GL context)
    void ProcessReadyTextures();

    // Check if there are textures queued for deletion
    bool HasPendingTextureDeletions() const;

    // Clear all pending requests (call on seek - preserves cache!)
    void ClearRequests();

    // Clear cache AND requests (call on config change)
    void ClearCache();

    // Compatibility methods for old interface
    void UpdateCurrentPosition(double timestamp);
    void UpdatePlaybackState(bool is_playing);
    void SetCacheWindow(double seconds) {}  // No-op in clean version
    void SetCachingEnabled(bool enabled) {}  // No-op in clean version
    void StartBackgroundCaching() {}  // No-op - worker thread started in Initialize()

    // Loop control for seamless wrap-around caching
    void SetLooping(bool enabled);
    bool IsLooping() const { return is_looping_; }

    // Loop range for In/Out point constrained playback
    // When set, wrap-around caching stays within [in_frame, out_frame]
    void SetLoopRange(int in_frame, int out_frame);
    void ClearLoopRange();

    // Overrun failsafe control
    // When cache can't keep up during playback, switches to synchronous frame loading
    void ResetOverrunMode();  // Call on seek, pause, or file reload
    bool IsInOverrunMode() const { return overrun_mode_.load(); }
    bool IsFrameCached(int frame) const;  // Check if frame is in cache (for lookahead)
    bool WasLastFrameSyncLoad() const { return last_was_sync_load_.load(); }  // True if last GetFrameOrLoad was sync load

    void ResetPlaybackSpeed();  // Reset overrun state (call on seek, pause)

    // Playback stride (cache-side frame skip for heavy sequences)
    void SetPlaybackStride(int stride);
    int GetPlaybackStride() const { return config_.playbackStride; }

    // Configuration
    void SetConfig(const EXRCacheConfig& config);
    EXRCacheConfig GetConfig() const { return config_; }
    int GetStartFrame() const { return startFrame_; }

    // Stats
    struct Stats {
        int totalFrames = 0;
        int cachedFrames = 0;
        int pendingRequests = 0;
        int inProgressRequests = 0;
        size_t cacheBytes = 0;

        // Compatibility fields (unused in clean version)
        int cache_hits = 0;
        int cache_misses = 0;
        double hit_ratio = 0.0;
        double memory_usage_mb = 0.0;
        bool background_thread_active = true;
        double average_load_time_ms = 0.0;

        // Aliases for old field names
        int& total_frames_in_sequence = totalFrames;
        int& frames_cached = cachedFrames;
        int& pending = pendingRequests;
        int& in_progress = inProgressRequests;
        size_t& total_cached_bytes = cacheBytes;
    };
    using CacheStats = Stats;  // Alias for VideoPlayer compatibility

    Stats GetStats() const;
    std::vector<CacheSegment> GetCacheSegments() const;
    void RebuildCacheSegments(const std::vector<int>& sorted_keys);

    // Compatibility methods
    bool GetFrameDimensions(int& width, int& height) const;

    // Static method for getting dimensions from a file
    static bool GetFrameDimensions(const std::string& filePath, int& width, int& height);

    bool IsInitialized() const { return initialized_; }

private:
    //=========================================================================
    // Request Management
    //=========================================================================

    struct EXRRequest {
        int frame;
        std::future<std::shared_ptr<PixelData>> future;  // Changed from EXRPixelData
        size_t byteCount;
        uint64_t generation;  // Generation when request was created (for stale detection)
    };

    //=========================================================================
    // Cache Management Thread (tlRender pattern - continuous cache management)
    //=========================================================================

    void CacheThread();

    std::thread cacheThread_;
    std::atomic<bool> cacheRunning_{false};

    //=========================================================================
    // I/O Worker Thread (spawns and manages async load tasks)
    //=========================================================================

    void IOWorkerThread();

    std::thread ioWorkerThread_;
    std::atomic<bool> ioRunning_{false};
    std::mutex mutex_;                                 // Init/Shutdown only (rare paths)
    std::condition_variable cv_;                       // Legacy — used to wake CacheThread from seeks

    // I/O dispatch — separate from CacheThread to avoid contention
    std::mutex io_mutex_;                              // Protects videoRequests_ + requestsInProgress_
    std::condition_variable io_cv_;                    // Wakes IOWorkerThread

    // CacheThread sleep — separate from I/O to avoid contention
    std::mutex cache_sleep_mutex_;
    std::condition_variable cache_cv_;

    std::deque<int> videoRequests_;                    // Pending frames to load (guarded by io_mutex_)
    std::map<int, EXRRequest> requestsInProgress_;     // Currently loading (guarded by io_mutex_)
    std::atomic<bool> needsFillReset_{false};          // Flag to reset fill counters on next cache update

    // Atomic cached counts for lock-free GetStats() (updated by I/O thread)
    std::atomic<int> cached_pending_count_{0};
    std::atomic<int> cached_in_progress_count_{0};

    //=========================================================================
    // GL Texture Management (main thread only)
    //=========================================================================

    std::vector<GLuint> texturesToDelete_;  // GL textures marked for deletion (deleted on main thread)
    mutable std::mutex textureMutex_;  // mutable for const HasPendingTextureDeletions()

#if defined(QCVIEW_USE_METAL) || defined(QCVIEW_USE_VULKAN)
    //=========================================================================
    // GPU Upload Thread (Metal/Vulkan only)
    // Creates GPU textures from cached pixel data off the main thread.
    //=========================================================================

    void GPUUploadThread();

    std::thread gpu_upload_thread_;
    std::atomic<bool> gpu_upload_running_{false};
    std::condition_variable gpu_upload_cv_;
    std::mutex gpu_upload_mutex_;

    // Upload queue: I/O thread pushes completed pixels here, GPU thread pops.
    // Separate from pixelCache_ to eliminate shared_mutex contention.
    struct GPUUploadItem {
        int frame;
        std::shared_ptr<PixelData> pixels;
        uint64_t generation;
    };
    std::deque<GPUUploadItem> gpu_upload_queue_;
    std::mutex gpu_upload_queue_mutex_;  // Lightweight, only protects the queue

    // Pre-created GPU textures: frame → {pool_id, width, height}
    struct GPUReadyEntry {
        uint64_t pool_id;
        int width;
        int height;
    };
    std::unordered_map<int, GPUReadyEntry> gpu_texture_ready_;
    mutable std::shared_mutex gpu_texture_mutex_;

    // Clean up GPU texture for a specific frame
    void EvictGPUTexture(int frame);
    // Clean up all GPU textures
    void ClearGPUTextures();
#endif

    //=========================================================================
    // Universal Image Loading (replaces EXR-only loading)
    //=========================================================================

    // NEW: Universal loader (runtime polymorphism)
    std::shared_ptr<PixelData> LoadPixels(const std::string& path);

    // EXR-specific loading (writes directly into PixelData — zero copy)
    std::shared_ptr<PixelData> LoadEXRPixels(const std::string& path, const std::string& layer);

    // GL texture creation (now handles multiple formats via PixelData)
    GLuint CreateGLTexture(const std::shared_ptr<PixelData>& pixels);

    //=========================================================================
    // State
    //=========================================================================

    bool initialized_ = false;
    std::vector<std::string> sequenceFiles_;
    std::string layerName_;
    double fps_ = 24.0;
    int startFrame_ = 0;  // First frame number from sequence filenames (for metadata/display)

    EXRCacheConfig config_;

    // NEW: Runtime-swappable image loader (nullptr = use EXR legacy path)
    std::unique_ptr<IImageLoader> loader_;

    // Probed media dimensions (from first valid file in sequence)
    // Used by sentinel factories so gap/broken frames carry correct dimensions
    int frameWidth_ = 0;
    int frameHeight_ = 0;

    // NEW: Pipeline mode for current sequence
    PipelineMode pipelineMode_ = PipelineMode::NORMAL;

    // LRU cache for CPU pixel data (NOT GL textures!)
    // Changed from EXRPixelData to PixelData for universal support
    SimpleLRU<int, std::shared_ptr<PixelData>> pixelCache_;

    // Small GL texture cache for recently used frames (created on-demand during GetTexture)
    // Keep this small (8-16 textures) to prevent GPU memory bloat
    std::map<int, std::shared_ptr<EXRTexture>> glTextureCache_;
    const size_t MAX_GL_TEXTURE_CACHE = 16;  // Max number of resident GL textures

    // Track playback state for cache direction
    std::atomic<double> lastCacheUpdateTime_{0.0};
    std::atomic<int> lastCacheUpdateFrame_{-1};
    std::atomic<int> previousFrame_{-1};  // Track previous frame to detect direction
    CacheDirection cacheDirection_ = CacheDirection::Forward;
    std::atomic<bool> isPlaying_{false};
    bool is_looping_ = true;  // Wrap-around caching always enabled for seamless looping

    // Loop range for In/Out point constrained playback
    int loop_in_frame_ = -1;   // -1 = use start of sequence (frame 0)
    int loop_out_frame_ = -1;  // -1 = use end of sequence
    bool has_loop_range_ = false;  // True when valid In/Out range is set

    //=========================================================================
    // Overrun Failsafe (sync fallback when cache can't keep up)
    //=========================================================================
    // When playback overruns cache, switch to synchronous single-frame loading
    // This ensures the user always sees their work, even if playback is slower
    std::atomic<bool> overrun_mode_{false};      // Currently in overrun fallback mode
    std::atomic<int> consecutive_misses_{0};      // Track cache misses while playing
    std::atomic<bool> last_was_sync_load_{false}; // True if last GetFrameOrLoad required sync load
    static constexpr int OVERRUN_THRESHOLD = 1;   // Activate overrun on first miss (immediate)
    std::atomic<uint64_t> request_generation_{0}; // Incremented on seek - stale results discarded

    // Frames that failed to load — never retry (gap/broken sentinel already cached)
    std::unordered_set<int> failed_frames_;
    std::mutex failed_frames_mutex_;

    // Pre-calculated frame size (from actual file, not estimated)
    size_t actualFrameSize_ = 0;  // Calculated from first loaded frame
    bool hasActualFrameSize_ = false;

    // Fill frame counter (reset on seek for correct fill start)
    int cacheFillFrame_ = 0;
    size_t cacheFillByteCount_ = 0;

    // Progressive ramp-up for initial load (better UX + prevents spike)
    std::atomic<int> cacheIterationCount_{0};  // Track cache thread iterations
    int lastSeekFrame_{-1};  // Detect seeks to reset ramp-up

    // Cached segments (optimization - avoid rebuilding every UI frame)
    mutable std::mutex segmentMutex_;
    mutable std::vector<CacheSegment> cachedSegments_;
    mutable std::atomic<bool> segmentsDirty_{true};  // Rebuild on next request

    // Last good frame fallback (for visual continuity during frame misses)
    GLuint last_good_texture_ = 0;
    int last_good_width_ = 0;
    int last_good_height_ = 0;

    //=========================================================================
    // SharedMemoryPool Integration
    //=========================================================================

    // Build a unique path identifier for SharedMemoryPool (uses first file in sequence)
    std::string GetSequenceIdentifier() const {
        return sequenceFiles_.empty() ? "" : sequenceFiles_[0];
    }

    // Register a frame with SharedMemoryPool (called when adding to pixelCache_)
    void RegisterWithPool(int frame, size_t bytes);

    // Touch a frame in SharedMemoryPool (called on cache hits)
    void TouchInPool(int frame);

    // Remove a frame from SharedMemoryPool (called when removing from pixelCache_)
    void RemoveFromPool(int frame);

    // Handle eviction callback from SharedMemoryPool
    void OnPoolEviction(int frame);
};

} // namespace qcview
