#pragma once

#include <string>
#include <vector>
#include <memory>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <future>
#include <map>
#include <deque>
#include <functional>

#include <glad/gl.h>
#include <half.h>

#include <OpenEXR/ImfIO.h>

#include "image_loader_interface.h"
#include "pipeline_mode.h"

#ifdef _WIN32
    #include <windows.h>
#endif

namespace ump {

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
// Clean DirectEXRCache - Pure tlRender Architecture
// Zero legacy code. Minimal state. Fast.
//=============================================================================

// Configuration
struct EXRCacheConfig {
    // tlRender default: 16 I/O threads for sequences (SequenceOptions.threadCount = 16)
    // This helps with slow multilayer EXRs (31 channels, DWAB compression ~900ms/frame)
    size_t threadCount = 16;            // Parallel EXR loads (matches tlRender)
    double cacheGB = 18.0;             // LRU cache size

    // tlRender pattern: Read-behind for instant backward scrubbing
    double readBehindSeconds = 0.5;    // Keep frames BEHIND playhead (0.5s default like tlRender)

    // Compatibility fields (unused in clean version)
    double video_cache_gb = 18.0;      // Alias for cacheGB
    double read_behind_seconds = 0.5;  // Alias for readBehindSeconds
    size_t gpu_memory_pool_mb = 8192;  // Unused
    size_t gpu_max_textures = 512;     // Unused
    double gpu_texture_ttl_seconds = 5.0;  // Unused

    bool IsValid() const {
        return threadCount >= 1 && threadCount <= 32 &&
               cacheGB >= 1.0 && cacheGB <= 128.0 &&
               readBehindSeconds >= 0.0 && readBehindSeconds <= 5.0;
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

// Pixel data loaded from EXR (CPU-side, background thread safe)
// Using 64-byte aligned allocation for SIMD/cache line optimization
struct EXRPixelData {
    std::vector<half, AlignedAllocator<half, 64>> pixels;  // RGBA half-float (64-byte aligned)
    int width = 0;
    int height = 0;
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
    void SetEvictionCallback(EvictionCallback callback) { evictionCallback_ = callback; }

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

    bool Get(const K& key, V& value) const {
        std::lock_guard<std::mutex> lock(mutex_);
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
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = cache_.find(key);
        if (it != cache_.end()) {
            value = it->second;
            return true;
        }
        return false;
    }

    void Add(const K& key, const V& value, size_t bytes) {
        std::lock_guard<std::mutex> lock(mutex_);

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

        // Evict if needed
        while (currentBytes_ > maxBytes_ && !lruList_.empty()) {
            K oldest = lruList_.front();
            lruList_.pop_front();

            // Call eviction callback BEFORE erasing (so callback can access the value)
            if (evictionCallback_) {
                auto it = cache_.find(oldest);
                if (it != cache_.end()) {
                    evictionCallback_(oldest, it->second);
                }
            }

            currentBytes_ -= sizes_[oldest];
            cache_.erase(oldest);
            sizes_.erase(oldest);
        }
    }

    void Clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        cache_.clear();
        sizes_.clear();
        lruList_.clear();
        currentBytes_ = 0;
    }

    // Remove without returning the value (for eviction without texture deletion callback)
    void Remove(const K& key) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = cache_.find(key);
        if (it != cache_.end()) {
            currentBytes_ -= sizes_[key];
            cache_.erase(it);
            sizes_.erase(key);
            lruList_.remove(key);
        }
    }

    // Remove and return the value (so caller can extract GL texture ID for deletion)
    bool RemoveAndGet(const K& key, V& value) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = cache_.find(key);
        if (it != cache_.end()) {
            value = it->second;
            currentBytes_ -= sizes_[key];
            cache_.erase(it);
            sizes_.erase(key);
            lruList_.remove(key);
            return true;
        }
        return false;
    }

    std::vector<K> GetKeys() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<K> keys;
        for (const auto& p : cache_) keys.push_back(p.first);
        return keys;
    }

private:
    void Touch(const K& key) {
        lruList_.remove(key);
        lruList_.push_back(key);
    }

    mutable std::mutex mutex_;
    std::map<K, V> cache_;
    std::map<K, size_t> sizes_;
    std::list<K> lruList_;
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
    bool Initialize(const std::vector<std::string>& files,
                   const std::string& layer,
                   double fps,
                   int start_frame = 0);

    // Initialize with universal loader (NEW - supports TIFF/PNG/JPEG/EXR)
    bool Initialize(std::unique_ptr<IImageLoader> loader,
                   const std::vector<std::string>& files,
                   const std::string& layer,
                   double fps,
                   PipelineMode pipeline_mode,
                   int start_frame = 0);

    void Shutdown();

    // Request a frame (returns immediately with future)
    // tlRender pattern: Request returns future, worker thread fulfills it
    void RequestFrame(int frame);

    // Get cached texture (returns 0 if not ready)
    GLuint GetTexture(int frame, int& width, int& height);

    // Compatibility method for old GetFrameOrLoad interface
    bool GetFrameOrLoad(int frame, GLuint& texture, int& width, int& height);

    // Process completed pixel loads (MUST call from main thread with GL context)
    void ProcessReadyTextures();

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

    // Compatibility methods
    bool GetFrameDimensions(int& width, int& height) const;

    // Static method for getting dimensions from a file
    static bool GetFrameDimensions(const std::string& filePath, int& width, int& height);

    bool IsInitialized() const { return initialized_; }

private:
    //=========================================================================
    // tlRender-style Request Management
    //=========================================================================

    struct EXRRequest {
        int frame;
        std::future<std::shared_ptr<PixelData>> future;  // Changed from EXRPixelData
        size_t byteCount;
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
    std::mutex mutex_;
    std::condition_variable cv_;

    std::deque<int> videoRequests_;                    // Pending frames to load
    std::map<int, EXRRequest> requestsInProgress_;     // Currently loading
    bool needsFillReset_ = false;                      // Flag to reset fill counters on next cache update

    //=========================================================================
    // GL Texture Management (main thread only)
    //=========================================================================

    std::vector<GLuint> texturesToDelete_;  // GL textures marked for deletion (deleted on main thread)
    std::mutex textureMutex_;

    //=========================================================================
    // Universal Image Loading (replaces EXR-only loading)
    //=========================================================================

    // NEW: Universal loader (runtime polymorphism)
    std::shared_ptr<PixelData> LoadPixels(const std::string& path);

    // LEGACY: EXR-specific loading (preserved for backward compatibility)
    std::shared_ptr<EXRPixelData> LoadEXRPixels(const std::string& path,
                                                 const std::string& layer);

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

    // NEW: Pipeline mode for current sequence
    PipelineMode pipelineMode_ = PipelineMode::NORMAL;

    // tlRender pattern: LRU cache for CPU pixel data (NOT GL textures!)
    // Changed from EXRPixelData to PixelData for universal support
    SimpleLRU<int, std::shared_ptr<PixelData>> pixelCache_;

    // Small GL texture cache for recently used frames (created on-demand during GetTexture)
    // Keep this small (8-16 textures) to prevent GPU memory bloat
    std::map<int, std::shared_ptr<EXRTexture>> glTextureCache_;
    const size_t MAX_GL_TEXTURE_CACHE = 16;  // Max number of resident GL textures

    // tlRender pattern: Track playback state for cache direction
    double lastCacheUpdateTime_ = 0.0;
    int lastCacheUpdateFrame_ = -1;
    int previousFrame_ = -1;  // Track previous frame to detect direction
    CacheDirection cacheDirection_ = CacheDirection::Forward;
    bool isPlaying_ = false;

    // tlRender pattern: Pre-calculated frame size (from actual file, not estimated)
    size_t actualFrameSize_ = 0;  // Calculated from first loaded frame
    bool hasActualFrameSize_ = false;

    // tlRender pattern: Fill frame counter (reset on seek for correct fill start)
    int cacheFillFrame_ = 0;
    size_t cacheFillByteCount_ = 0;

    // Progressive ramp-up for initial load (better UX + prevents spike)
    std::atomic<int> cacheIterationCount_{0};  // Track cache thread iterations
    int lastSeekFrame_{-1};  // Detect seeks to reset ramp-up

    // Cached segments (optimization - avoid rebuilding every UI frame)
    mutable std::mutex segmentMutex_;
    mutable std::vector<CacheSegment> cachedSegments_;
    mutable std::atomic<bool> segmentsDirty_{true};  // Rebuild on next request
};

} // namespace ump
