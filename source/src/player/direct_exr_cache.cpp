#include "direct_exr_cache.h"
#include "../utils/debug_utils.h"

#ifdef _WIN32
#undef min
#undef max
#endif

#include <ImfInputFile.h>
#include <ImfChannelList.h>
#include <ImfFrameBuffer.h>
#include <ImfInputPart.h>
#include <ImfMultiPartInputFile.h>
#include <ImfThreading.h>

#include <algorithm>

namespace ump {

//=============================================================================
// MemoryMappedIStream Implementation (shared utility)
//=============================================================================

MemoryMappedIStream::MemoryMappedIStream(const std::string& fileName)
    : Imf::IStream(fileName.c_str())
    , filePath_(fileName)
{
#ifdef _WIN32
    // Windows memory-mapped file with optimizations
    int wlen = MultiByteToWideChar(CP_UTF8, 0, fileName.c_str(), -1, nullptr, 0);
    std::vector<wchar_t> wpath(wlen);
    MultiByteToWideChar(CP_UTF8, 0, fileName.c_str(), -1, wpath.data(), wlen);

    // Optimization: Use FILE_FLAG_SEQUENTIAL_SCAN for better read-ahead
    hFile_ = CreateFileW(wpath.data(), GENERIC_READ, FILE_SHARE_READ,
                         nullptr, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                         nullptr);

    if (hFile_ == INVALID_HANDLE_VALUE) {
        throw std::runtime_error("Cannot open file: " + fileName);
    }

    LARGE_INTEGER size;
    if (!GetFileSizeEx(hFile_, &size)) {
        CloseHandle(hFile_);
        throw std::runtime_error("Cannot get file size: " + fileName);
    }
    fileSize_ = static_cast<uint64_t>(size.QuadPart);

    hMapping_ = CreateFileMappingW(hFile_, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!hMapping_) {
        CloseHandle(hFile_);
        throw std::runtime_error("Cannot create file mapping: " + fileName);
    }

    mappedData_ = static_cast<char*>(MapViewOfFile(hMapping_, FILE_MAP_READ, 0, 0, 0));

    if (!mappedData_) {
        CloseHandle(hMapping_);
        CloseHandle(hFile_);
        throw std::runtime_error("Cannot map view of file: " + fileName);
    }

    // Optimization: Prefetch hint
    WIN32_MEMORY_RANGE_ENTRY range;
    range.VirtualAddress = mappedData_;
    range.NumberOfBytes = fileSize_;
    PrefetchVirtualMemory(GetCurrentProcess(), 1, &range, 0);
#else
    // TODO: Linux mmap implementation
    throw std::runtime_error("Memory-mapped files not implemented on this platform");
#endif
}

MemoryMappedIStream::~MemoryMappedIStream() {
#ifdef _WIN32
    if (mappedData_) UnmapViewOfFile(mappedData_);
    if (hMapping_) CloseHandle(hMapping_);
    if (hFile_ != INVALID_HANDLE_VALUE) CloseHandle(hFile_);
#endif
}

char* MemoryMappedIStream::readMemoryMapped(int n) {
    if (currentPos_ + n > fileSize_) {
        throw std::runtime_error("Read past end of file");
    }
    char* ptr = mappedData_ + currentPos_;
    currentPos_ += n;
    return ptr;
}

bool MemoryMappedIStream::read(char c[], int n) {
    if (currentPos_ + n > fileSize_) {
        throw std::runtime_error("Read past end of file");
    }
    std::memcpy(c, mappedData_ + currentPos_, n);
    currentPos_ += n;
    return currentPos_ < fileSize_;
}

uint64_t MemoryMappedIStream::tellg() {
    return currentPos_;
}

void MemoryMappedIStream::seekg(uint64_t pos) {
    currentPos_ = pos;
}

//=============================================================================
// DirectEXRCache Implementation
//=============================================================================

DirectEXRCache::DirectEXRCache() {
    Debug::Log("DirectEXRCache: Constructor - starting permanent background threads");

    // Single-threaded OpenEXR decompression
    // Setting to 0 tells OpenEXR to use single-threaded mode per file
    // We parallelize at the I/O level (8 concurrent files), NOT compression level
    // This prevents thread over-subscription: 8 files * 1 thread = 8 threads total
    Imf::setGlobalThreadCount(0);

    // The pixelCache_ just holds shared_ptrs to PixelData - automatic cleanup via shared_ptr
    // Threads wait idle until a sequence is loaded
    cacheRunning_ = true;
    cacheThread_ = std::thread(&DirectEXRCache::CacheThread, this);

    ioRunning_ = true;
    ioWorkerThread_ = std::thread(&DirectEXRCache::IOWorkerThread, this);

    Debug::Log("DirectEXRCache: Permanent background threads started (ready for sequences)");
}

DirectEXRCache::~DirectEXRCache() {
    Debug::Log("DirectEXRCache: Destructor - stopping permanent background threads");

    // Stop cache thread
    Debug::Log("DirectEXRCache: Checking cache thread status...");
    if (cacheRunning_) {
        Debug::Log("DirectEXRCache: Setting cacheRunning_ = false");
        cacheRunning_ = false;
        Debug::Log("DirectEXRCache: Notifying cache thread to wake up");
        cv_.notify_all();  // Wake threads to check running flag
        if (cacheThread_.joinable()) {
            Debug::Log("DirectEXRCache: Waiting for cache thread to join (this may block if thread is stuck)...");
            cacheThread_.join();
            Debug::Log("DirectEXRCache: Cache thread joined successfully");
        } else {
            Debug::Log("DirectEXRCache: Cache thread was not joinable");
        }
    } else {
        Debug::Log("DirectEXRCache: Cache thread was not running");
    }

    // Stop I/O worker thread
    Debug::Log("DirectEXRCache: Checking I/O worker thread status...");
    if (ioRunning_) {
        Debug::Log("DirectEXRCache: Setting ioRunning_ = false");
        ioRunning_ = false;
        Debug::Log("DirectEXRCache: Notifying I/O worker thread to wake up");
        cv_.notify_all();
        if (ioWorkerThread_.joinable()) {
            Debug::Log("DirectEXRCache: Waiting for I/O worker thread to join (this may block if thread is stuck)...");
            ioWorkerThread_.join();
            Debug::Log("DirectEXRCache: I/O worker thread joined successfully");
        } else {
            Debug::Log("DirectEXRCache: I/O worker thread was not joinable");
        }
    } else {
        Debug::Log("DirectEXRCache: I/O worker thread was not running");
    }

    // Clean up GL textures before clearing cache
    Debug::Log("DirectEXRCache: Deleting GL textures...");
    int texture_count = 0;
    for (auto& pair : glTextureCache_) {
        if (pair.second && pair.second->texture_id != 0) {
            glDeleteTextures(1, &pair.second->texture_id);
            texture_count++;
        }
    }
    glTextureCache_.clear();
    Debug::Log("DirectEXRCache: Deleted " + std::to_string(texture_count) + " GL textures");

    Debug::Log("DirectEXRCache: Clearing pixel cache...");
    pixelCache_.Clear();
    Debug::Log("DirectEXRCache: Pixel cache cleared");

    Debug::Log("DirectEXRCache: Destructor complete - all resources freed");
}

bool DirectEXRCache::Initialize(const std::vector<std::string>& files,
                                const std::string& layer,
                                double fps,
                                int start_frame) {
    auto init_start = std::chrono::steady_clock::now();

    if (files.empty()) {
        Debug::Log("DirectEXRCache: ERROR - Empty file list");
        return false;
    }

    Debug::Log("DirectEXRCache: [INIT] Loading new sequence (" + std::to_string(files.size()) + " frames, start frame: " + std::to_string(start_frame) + ")");

    auto clear_start = std::chrono::steady_clock::now();
    // Clear old sequence data (threads keep running)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        videoRequests_.clear();
        requestsInProgress_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(textureMutex_);
        texturesToDelete_.clear();
        // Clean up GL textures
        for (auto& pair : glTextureCache_) {
            if (pair.second && pair.second->texture_id != 0) {
                texturesToDelete_.push_back(pair.second->texture_id);
            }
        }
        glTextureCache_.clear();
    }
    pixelCache_.Clear();
    segmentsDirty_ = true;  // Segments invalid after clear
    auto clear_end = std::chrono::steady_clock::now();
    auto clear_ms = std::chrono::duration_cast<std::chrono::milliseconds>(clear_end - clear_start).count();

    // Load new sequence
    sequenceFiles_ = files;
    layerName_ = layer;
    fps_ = fps;
    startFrame_ = start_frame;

    // Set cache size
    pixelCache_.SetMaxSize(static_cast<size_t>(config_.cacheGB * 1024 * 1024 * 1024));

    initialized_ = true;

    auto init_end = std::chrono::steady_clock::now();
    auto init_ms = std::chrono::duration_cast<std::chrono::milliseconds>(init_end - init_start).count();

    Debug::Log("DirectEXRCache: [INIT] Sequence loaded in " + std::to_string(init_ms) + "ms (clear: " +
               std::to_string(clear_ms) + "ms) - " + std::to_string(files.size()) +
               " frames, cache=" + std::to_string(config_.cacheGB) + "GB");

    // Start preloading from frame 0 (fill cache on load)
    Debug::Log("DirectEXRCache: [INIT] Starting initial cache fill from frame 0");
    UpdateCurrentPosition(0.0);

    return true;
}

bool DirectEXRCache::Initialize(std::unique_ptr<IImageLoader> loader,
                                const std::vector<std::string>& files,
                                const std::string& layer,
                                double fps,
                                PipelineMode pipeline_mode,
                                int start_frame) {
    // Store the loader and pipeline mode
    loader_ = std::move(loader);
    pipelineMode_ = pipeline_mode;

    // Delegate to the original Initialize for the rest
    return Initialize(files, layer, fps, start_frame);
}

void DirectEXRCache::Shutdown() {
    // 🔧 NEW BEHAVIOR: Don't stop threads, just clear sequence data
    // This is now just an alias for clearing - threads stay alive
    Debug::Log("DirectEXRCache: Shutdown called (clearing sequence, keeping threads alive)");

    {
        std::lock_guard<std::mutex> lock(mutex_);
        videoRequests_.clear();
        requestsInProgress_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(textureMutex_);
        texturesToDelete_.clear();
    }
    // Clean up GL texture cache
    {
        std::lock_guard<std::mutex> lock(textureMutex_);
        for (auto& pair : glTextureCache_) {
            if (pair.second && pair.second->texture_id != 0) {
                texturesToDelete_.push_back(pair.second->texture_id);
            }
        }
        glTextureCache_.clear();
    }

    pixelCache_.Clear();

    initialized_ = false;
    sequenceFiles_.clear();
}

void DirectEXRCache::RequestFrame(int frame) {
    if (frame < 0 || frame >= static_cast<int>(sequenceFiles_.size())) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    // Already in cache?
    if (pixelCache_.Contains(frame)) {
        return;
    }

    // Already in progress?
    if (requestsInProgress_.find(frame) != requestsInProgress_.end()) {
        return;
    }

    // Already in queue?
    for (int req : videoRequests_) {
        if (req == frame) return;
    }

    // Add to queue
    videoRequests_.push_back(frame);
    cv_.notify_one();
}

GLuint DirectEXRCache::GetTexture(int frame, int& width, int& height) {
    // Cache holds CPU pixel data, create GL textures on-demand

    // Step 1: Check if we have pixel data in the cache
    std::shared_ptr<PixelData> pixels;
    if (!pixelCache_.Peek(frame, pixels) || !pixels) {
        width = 0;
        height = 0;
        return 0;  // Frame not in cache yet
    }

    // Step 2: Check if we already have a GL texture for this frame
    {
        std::lock_guard<std::mutex> lock(textureMutex_);
        auto it = glTextureCache_.find(frame);
        if (it != glTextureCache_.end() && it->second && it->second->texture_id != 0) {
            width = it->second->width;
            height = it->second->height;
            return it->second->texture_id;  // Return existing GL texture
        }
    }

    // Step 3: Create GL texture on-demand from pixel data
    GLuint texId = CreateGLTexture(pixels);
    if (texId == 0) {
        width = 0;
        height = 0;
        return 0;
    }

    // Step 4: Add to GL texture cache (with LRU eviction)
    {
        std::lock_guard<std::mutex> lock(textureMutex_);

        // Evict oldest GL texture if cache is full
        if (glTextureCache_.size() >= MAX_GL_TEXTURE_CACHE) {
            // Find oldest (assuming map order, but we should really track access time)
            auto oldest = glTextureCache_.begin();
            if (oldest->second && oldest->second->texture_id != 0) {
                texturesToDelete_.push_back(oldest->second->texture_id);
            }
            glTextureCache_.erase(oldest);
        }

        // Add new texture to cache
        auto tex = std::make_shared<EXRTexture>();
        tex->texture_id = texId;
        tex->width = pixels->width;
        tex->height = pixels->height;
        tex->byteCount = pixels->pixels.size();  // Already in bytes
        glTextureCache_[frame] = tex;

        width = pixels->width;
        height = pixels->height;
    }

    return texId;
}

bool DirectEXRCache::GetFrameOrLoad(int frame, GLuint& texture, int& width, int& height) {
    // Get from cache if available
    texture = GetTexture(frame, width, height);

    // If not cached, request it
    if (texture == 0) {
        RequestFrame(frame);
        return false;
    }

    return true;
}

void DirectEXRCache::UpdateCurrentPosition(double timestamp) {
    int current_frame = static_cast<int>(timestamp * fps_);

    // Detect seeks and cancel in-flight requests
    bool isSeek = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);

        // Detect seek: jump > 20 frames
        if (previousFrame_ >= 0 && std::abs(current_frame - previousFrame_) > 20) {
            isSeek = true;
        }

        previousFrame_ = current_frame;
        lastCacheUpdateFrame_ = current_frame;
        lastCacheUpdateTime_ = timestamp;
    }

    // Cancel all in-flight requests on seek
    if (isSeek) {
        Debug::Log("DirectEXRCache: [SEEK DETECTED] Canceling all in-flight requests");
        ClearRequests();
    }

    // Wake up cache thread immediately (don't wait for next tick)
    // This ensures instant response on seeks and initial load
    cv_.notify_one();
}

void DirectEXRCache::UpdatePlaybackState(bool is_playing) {
    std::lock_guard<std::mutex> lock(mutex_);
    isPlaying_ = is_playing;
    Debug::Log("DirectEXRCache: Playback state updated - " + std::string(is_playing ? "PLAYING" : "PAUSED"));
}

bool DirectEXRCache::GetFrameDimensions(int& width, int& height) const {
    // Return dimensions from first cached pixel data
    auto keys = pixelCache_.GetKeys();
    if (!keys.empty()) {
        std::shared_ptr<PixelData> pixels;
        if (pixelCache_.Get(keys[0], pixels)) {
            width = pixels->width;
            height = pixels->height;
            return true;
        }
    }

    // Fallback: return default dimensions
    width = 3840;
    height = 2160;
    return false;
}

bool DirectEXRCache::GetFrameDimensions(const std::string& filePath, int& width, int& height) {
    try {
        auto stream = std::make_unique<MemoryMappedIStream>(filePath);
        Imf::MultiPartInputFile file(*stream);

        const Imf::Header& header = file.header(0);
        Imath::Box2i dw = header.dataWindow();

        width = dw.max.x - dw.min.x + 1;
        height = dw.max.y - dw.min.y + 1;

        return true;
    } catch (const std::exception& e) {
        Debug::Log("DirectEXRCache: Failed to get dimensions - " + std::string(e.what()));
        return false;
    }
}

void DirectEXRCache::ProcessReadyTextures() {
    // GL textures created on-demand in GetTexture()
    // This function now ONLY handles deletion of queued GL textures
    // MUST be called from main thread with GL context. I keep forgetting this.

    const int MAX_DELETES_PER_FRAME = 20;
    std::vector<GLuint> toDelete;
    {
        std::lock_guard<std::mutex> lock(textureMutex_);
        if (!texturesToDelete_.empty()) {
            int count = std::min(MAX_DELETES_PER_FRAME, (int)texturesToDelete_.size());
            toDelete.insert(toDelete.end(),
                           texturesToDelete_.begin(),
                           texturesToDelete_.begin() + count);
            texturesToDelete_.erase(texturesToDelete_.begin(), texturesToDelete_.begin() + count);
        }
    }

    if (!toDelete.empty()) {
        glDeleteTextures(static_cast<GLsizei>(toDelete.size()), toDelete.data());

        int remaining_deletes = 0;
        {
            std::lock_guard<std::mutex> lock(textureMutex_);
            remaining_deletes = (int)texturesToDelete_.size();
        }

        Debug::Log("DirectEXRCache: [TEX-DELETE] Deleted " + std::to_string(toDelete.size()) +
                   " GL textures (" + std::to_string(remaining_deletes) + " queued)");
    }
}

void DirectEXRCache::ClearRequests() {
    size_t pending = 0;
    size_t inProgress = 0;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending = videoRequests_.size();
        inProgress = requestsInProgress_.size();

        videoRequests_.clear();

        // Clear the map and let the futures destruct naturally
        requestsInProgress_.clear();

        // Set flag to reset fill counters on next cache update
        // This makes cache fill restart from new seek position
        needsFillReset_ = true;
    }

    Debug::Log("DirectEXRCache: Cleared " + std::to_string(pending) +
               " pending + " + std::to_string(inProgress) + " in-progress (cache preserved)");
}

void DirectEXRCache::ClearCache() {
    // Clear both requests AND cache (for config changes)
    ClearRequests();

    // Clear pixel cache
    auto pixel_keys = pixelCache_.GetKeys();
    size_t pixel_count = pixel_keys.size();
    pixelCache_.Clear();

    // Clear GL texture cache and queue textures for deletion
    std::vector<GLuint> textures_to_delete;
    {
        std::lock_guard<std::mutex> lock(textureMutex_);
        for (auto& pair : glTextureCache_) {
            if (pair.second && pair.second->texture_id != 0) {
                textures_to_delete.push_back(pair.second->texture_id);
            }
        }
        glTextureCache_.clear();
    }

    // Queue GL textures for deletion on main thread
    if (!textures_to_delete.empty()) {
        std::lock_guard<std::mutex> lock(textureMutex_);
        texturesToDelete_.insert(texturesToDelete_.end(),
                                textures_to_delete.begin(),
                                textures_to_delete.end());
    }

    Debug::Log("DirectEXRCache: Cleared cache (" + std::to_string(pixel_count) +
               " pixel frames) + requests, queued " + std::to_string(textures_to_delete.size()) +
               " GL textures for deletion");
}

void DirectEXRCache::SetConfig(const EXRCacheConfig& config) {
    if (!config.IsValid()) {
        Debug::Log("DirectEXRCache: WARNING - Invalid config");
        return;
    }

    // Check if cache size changed - if so, clear cache 
    bool cacheSizeChanged = (config.cacheGB != config_.cacheGB);

    config_ = config;
    pixelCache_.SetMaxSize(static_cast<size_t>(config_.cacheGB * 1024 * 1024 * 1024));

    if (cacheSizeChanged) {
      /*  Debug::Log("DirectEXRCache: Cache size changed - clearing cache");
        ClearCache();*/
    }

    Debug::Log("DirectEXRCache: Config updated - threads=" +
               std::to_string(config_.threadCount) + ", cache=" +
               std::to_string(config_.cacheGB) + "GB, readBehind=" +
               std::to_string(config_.readBehindSeconds) + "s");
}

DirectEXRCache::Stats DirectEXRCache::GetStats() const {
    Stats stats;
    stats.totalFrames = static_cast<int>(sequenceFiles_.size());
    stats.cachedFrames = static_cast<int>(pixelCache_.GetKeys().size());
    stats.cacheBytes = pixelCache_.GetSize();

    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(mutex_));
    stats.pendingRequests = static_cast<int>(videoRequests_.size());
    stats.inProgressRequests = static_cast<int>(requestsInProgress_.size());

    return stats;
}

std::vector<CacheSegment> DirectEXRCache::GetCacheSegments() const {
    // Use cached segments, only rebuild when dirty
    // Avoid expensive iteration/sort on every UI frame

    // Quick check without lock
    if (!segmentsDirty_.load()) {
        std::lock_guard<std::mutex> lock(segmentMutex_);
        return cachedSegments_;
    }

    // Rebuild segments (dirty)
    std::vector<CacheSegment> segments;

    // Get all cached frame indices
    auto keys = pixelCache_.GetKeys();
    if (keys.empty()) {
        std::lock_guard<std::mutex> lock(segmentMutex_);
        cachedSegments_ = segments;
        segmentsDirty_ = false;
        return segments;
    }

    // Sort frame indices
    std::sort(keys.begin(), keys.end());

    // Group into contiguous segments and convert to time
    CacheSegment current;
    current.start_frame = keys[0];
    current.end_frame = keys[0];
    current.start_time = keys[0] / fps_;
    current.end_time = keys[0] / fps_;

    for (size_t i = 1; i < keys.size(); ++i) {
        if (keys[i] == current.end_frame + 1) {
            // Contiguous - extend current segment
            current.end_frame = keys[i];
            current.end_time = keys[i] / fps_;
        } else {
            // Gap - save current segment and start new one
            current.density = 1.0; // Full density
            segments.push_back(current);

            current.start_frame = keys[i];
            current.end_frame = keys[i];
            current.start_time = keys[i] / fps_;
            current.end_time = keys[i] / fps_;
        }
    }

    // Add final segment
    current.density = 1.0;
    segments.push_back(current);

    // Cache the result
    {
        std::lock_guard<std::mutex> lock(segmentMutex_);
        cachedSegments_ = segments;
        segmentsDirty_ = false;
    }

    return segments;
}

//=============================================================================
// Cache Management Thread (runs continuously)
//=============================================================================

void DirectEXRCache::CacheThread() {
    Debug::Log("DirectEXRCache: Cache management thread started");

    // We use 10ms as a balance between responsiveness and CPU usage
    const std::chrono::milliseconds interval(10);  // 100 ticks/second for fast response
    int iteration = 0;

    while (cacheRunning_) {
        // Wait with timeout (interruptible via cv_.notify_one())
        // This allows instant response on seeks/position updates instead of waiting up to 100ms
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_for(lock, interval);
        }

        // If no sequence loaded, just sleep and check again
        if (!initialized_ || sequenceFiles_.empty()) {
            continue;
        }

        iteration++;

        // DEBUG: Log every iteration during initial load
        if (iteration <= 10) {
            Debug::Log("DirectEXRCache: [CACHE-THREAD] Iteration " + std::to_string(iteration) + " starting");
        }

        // Get current playback position (mutex-protected state exchange)
        int current_frame = -1;
        bool needsReset = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            current_frame = lastCacheUpdateFrame_;
            needsReset = needsFillReset_;

            // Reset fill counters on seek
            if (needsFillReset_) {
                cacheFillFrame_ = 0;
                cacheFillByteCount_ = 0;
                needsFillReset_ = false;
                Debug::Log("DirectEXRCache: [FILL-RESET] Reset fill counters to start from frame " +
                           std::to_string(current_frame));
            }
        }

        // Periodic status logging every 2 seconds (20 iterations @ 100ms)
        if (iteration % 20 == 0) {
            size_t cached_bytes = pixelCache_.GetSize();
            size_t max_bytes = pixelCache_.GetMaxSize();
            auto cached_frames = pixelCache_.GetKeys();

         /*   Debug::Log("DirectEXRCache: Cache status - Frame: " + std::to_string(current_frame) +
                       ", Cached frames: " + std::to_string(cached_frames.size()) +
                       ", Memory: " + std::to_string(cached_bytes / (1024*1024)) + "/" +
                       std::to_string(max_bytes / (1024*1024)) + " MB");*/
        }

        // Cache management logic (only if we have a valid position)
        if (current_frame >= 0) {
            auto iter_start = std::chrono::steady_clock::now();

            // CRITICAL: Detect seeks BEFORE updating cacheIterationCount_
            // If position jumped >20 frames, reset iteration counter for post-seek boost
            bool isSeek = false;
            if (lastSeekFrame_ >= 0 && std::abs(current_frame - lastSeekFrame_) > 20) {
                isSeek = true;
                iteration = 1;  // Reset for 2-second post-seek boost (MAX_TEXTURES_POST_SEEK = 4)
                Debug::Log("DirectEXRCache: [SEEK] Detected jump from frame " +
                           std::to_string(lastSeekFrame_) + " to " + std::to_string(current_frame) +
                           " - resetting iteration counter for post-seek boost");

                // Immediately evict stale frames on major seek
                // This prevents memory tracking issues where old frames consume budget
                int readBehindFrames = static_cast<int>(config_.readBehindSeconds * fps_);
                int readAheadFrames = 72;  // Smaller immediate window ~3s @ 24fps
                int eviction_threshold_behind = current_frame - readBehindFrames;
                int eviction_threshold_ahead = current_frame + readAheadFrames;

                auto cached_frames_pre = pixelCache_.GetKeys();
                int immediate_evicted = 0;

                for (int frame : cached_frames_pre) {
                    if (frame < eviction_threshold_behind || frame > eviction_threshold_ahead) {
                        pixelCache_.Remove(frame);
                        immediate_evicted++;
                    }
                }

                if (immediate_evicted > 0) {
                    segmentsDirty_ = true;
                    size_t freed_bytes = immediate_evicted * (hasActualFrameSize_ ? actualFrameSize_ : (3840 * 2160 * 4 * sizeof(half)));
                    Debug::Log("DirectEXRCache: [SEEK-EVICTION] Immediately evicted " + std::to_string(immediate_evicted) +
                               " stale frames (~" + std::to_string(freed_bytes / (1024*1024)) + "MB freed)");
                }
            }
            lastSeekFrame_ = current_frame;

            // Update cacheIterationCount_ AFTER seek detection (so ProcessReadyTextures sees reset value)
            cacheIterationCount_ = iteration;

            // Evict old frames with read-behind + read-ahead window 
            // Calculate read-behind/read-ahead in frames
            int readBehindFrames = static_cast<int>(config_.readBehindSeconds * fps_);
            //  Also define a read-ahead window for eviction
            // After a major seek, frames FAR ahead of the playhead should be evicted too
            int readAheadFrames = 180;  // Keep ~7.5 seconds ahead @ 24fps (was infinite before!)

            auto cached_frames = pixelCache_.GetKeys();

            // Keep frames in window: [current - readBehind, current + readAhead]
            int eviction_threshold_behind = current_frame - readBehindFrames;
            int eviction_threshold_ahead = current_frame + readAheadFrames;
            int evicted_count = 0;

            // Simply evict pixel data - no GL textures involved
            // GL textures are in separate glTextureCache_ and managed by GetTexture()
            for (int frame : cached_frames) {
                // Evict frames both BEHIND and FAR AHEAD of playhead
                if (frame < eviction_threshold_behind || frame > eviction_threshold_ahead) {
                    pixelCache_.Remove(frame);
                    evicted_count++;
                }
            }

            if (evicted_count > 0) {
                segmentsDirty_ = true;  // Mark segments dirty after eviction
                Debug::Log("DirectEXRCache: Cache thread @ frame " + std::to_string(current_frame) +
                           " - Evicted " + std::to_string(evicted_count) + " pixel data frames outside window [" +
                           std::to_string(eviction_threshold_behind) + ", " + std::to_string(eviction_threshold_ahead) + "]");
            }

            // Step 2: Fill cache with readahead frames
            size_t cached_bytes = pixelCache_.GetSize();
            size_t max_bytes = pixelCache_.GetMaxSize();

            if (cached_bytes < max_bytes) {
                // Calculate available space, accounting for in-progress AND ready-for-texture
                std::lock_guard<std::mutex> lock(mutex_);

                // Use pre-calculated actual frame size
                int estimated_frame_size = 3840 * 2160 * 4 * sizeof(half); // Default estimate ~64MB

                if (hasActualFrameSize_) {
                    // Use cached actual size 
                    estimated_frame_size = static_cast<int>(actualFrameSize_);
                } else {
                    // Try to get actual size from first cached frame
                    auto cached_frames = pixelCache_.GetKeys();
                    if (!cached_frames.empty()) {
                        std::shared_ptr<PixelData> first_pixels;
                        if (pixelCache_.Peek(cached_frames[0], first_pixels) && first_pixels) {
                            actualFrameSize_ = first_pixels->pixels.size();
                            hasActualFrameSize_ = true;
                            estimated_frame_size = static_cast<int>(actualFrameSize_);

                            //Debug::Log("DirectEXRCache: Detected actual frame size: " +
                            //           std::to_string(actualFrameSize_ / (1024*1024)) + "MB ");
                        }
                    }
                }

                // Limit in-flight requests to prevent unbounded accumulation
                // Count total requests pending: in queue + in progress
                size_t total_requests_pending = requestsInProgress_.size() + videoRequests_.size();

                // Hard limit on concurrent requests to prevent spamming
                const size_t MAX_CONCURRENT_REQUESTS = 32;

                if (total_requests_pending >= MAX_CONCURRENT_REQUESTS) {
                    // Too many requests already pending - wait for them to complete
                    continue;
                }

                size_t in_progress_bytes = (requestsInProgress_.size() + videoRequests_.size()) * estimated_frame_size;

                // Available space = max - cached - in_progress
                size_t total_committed = cached_bytes + in_progress_bytes;
                if (total_committed >= max_bytes) {
                    // Already over budget with pending requests
                    continue;
                }

                size_t available = max_bytes - total_committed;

                // Conservative batching - limit batch size
                int batch_limit;
                if (iteration == 1) {
                    batch_limit = config_.threadCount * 4;  // Deep initial saturation
                } else {
                    batch_limit = 72;  // 3 seconds @ 24fps - need DEEP buffer since we're slow at GL texture creation
                }

                // Use 80% of available space as safety margin
                size_t safe_available = static_cast<size_t>(available * 0.80);
                int max_to_request = std::min(batch_limit, (int)(safe_available / estimated_frame_size));

                // Fill bi-directionally (read-behind + read-ahead)
                int requested_count = 0;

                // Calculate frame ranges for both directions
                int readAheadStart = current_frame + 1;
                int readBehindStart = current_frame - 1;
                int readBehindFrames = static_cast<int>(config_.readBehindSeconds * fps_);
                int readBehindEnd = current_frame - readBehindFrames;

                // Fill read-ahead frames (priority for forward playback)
                for (int i = 1; i <= max_to_request && (current_frame + i) < (int)sequenceFiles_.size(); i++) {
                    int frame = current_frame + i;

                    // Skip if already cached
                    if (pixelCache_.Contains(frame)) continue;

                    // Skip if already in progress
                    if (requestsInProgress_.find(frame) != requestsInProgress_.end()) continue;

                    // Skip if already pending
                    bool already_pending = false;
                    for (int pending : videoRequests_) {
                        if (pending == frame) {
                            already_pending = true;
                            break;
                        }
                    }
                    if (already_pending) continue;

                    // Add to request queue
                    videoRequests_.push_back(frame);
                    requested_count++;
                }

                // Fill read-behind frames (for backward scrubbing responsiveness)
                // Only fill if we have remaining capacity
                for (int i = 1; requested_count < max_to_request && i <= readBehindFrames; i++) {
                    int frame = current_frame - i;
                    if (frame < 0) break;

                    // Skip if already cached
                    if (pixelCache_.Contains(frame)) continue;

                    // Skip if already in progress
                    if (requestsInProgress_.find(frame) != requestsInProgress_.end()) continue;

                    // Skip if already pending
                    bool already_pending = false;
                    for (int pending : videoRequests_) {
                        if (pending == frame) {
                            already_pending = true;
                            break;
                        }
                    }
                    if (already_pending) continue;

                    // Add to request queue
                    videoRequests_.push_back(frame);
                    requested_count++;
                }

                // Post-fill cache prioritization (reverse touch)
                // Touch cached frames in REVERSE order so frames closest to current time
                // are touched LAST and thus stay in cache longest (LRU keeps most recently touched)
                auto cached_frame_list = pixelCache_.GetKeys();
                if (!cached_frame_list.empty()) {
                    // Build list of frames within cache budget, sorted by distance from current frame
                    std::vector<int> frames_to_prioritize;
                    size_t priority_bytes = 0;

                    // Calculate how many frames fit in budget
                    for (int dist = 0; dist < (int)sequenceFiles_.size() && priority_bytes < max_bytes; dist++) {
                        // Check both directions from current frame
                        int frame_plus = current_frame + dist;
                        int frame_minus = current_frame - dist;

                        if (frame_plus < (int)sequenceFiles_.size() && pixelCache_.Contains(frame_plus)) {
                            frames_to_prioritize.push_back(frame_plus);
                            priority_bytes += estimated_frame_size;
                        }

                        if (dist > 0 && frame_minus >= 0 && pixelCache_.Contains(frame_minus)) {
                            frames_to_prioritize.push_back(frame_minus);
                            priority_bytes += estimated_frame_size;
                        }

                        if (priority_bytes >= max_bytes) break;
                    }

                    // Touch in REVERSE order (furthest from current frame first)
                    // This makes closest frames stay in cache longest
                    for (auto it = frames_to_prioritize.rbegin(); it != frames_to_prioritize.rend(); ++it) {
                        std::shared_ptr<PixelData> pixels;
                        pixelCache_.Get(*it, pixels);  // Get() calls Touch() internally
                    }
                }

                auto iter_end = std::chrono::steady_clock::now();
                auto iter_ms = std::chrono::duration_cast<std::chrono::milliseconds>(iter_end - iter_start).count();

                if (requested_count > 0) {
                    Debug::Log("DirectEXRCache: [ITER-" + std::to_string(iteration) + "] " +
                               std::to_string(iter_ms) + "ms - Requested " +
                               std::to_string(requested_count) + "/" + std::to_string(batch_limit) +
                               " frames (cached: " + std::to_string(cached_bytes / (1024*1024)) +
                               "MB + in-progress: " + std::to_string(in_progress_bytes / (1024*1024)) +
                               "MB = " + std::to_string(total_committed / (1024*1024)) +
                               "MB / " + std::to_string(max_bytes / (1024*1024)) + "MB)");
                    cv_.notify_one();  // Wake up I/O worker
                }
            }
        }

        // Sleep until next iteration
        std::this_thread::sleep_for(interval);
    }

    Debug::Log("DirectEXRCache: Cache management thread stopped");
}

//=============================================================================
// I/O Worker Thread (spawns and manages async load tasks)
//=============================================================================

void DirectEXRCache::IOWorkerThread() {
    Debug::Log("DirectEXRCache: I/O worker thread started");

    // Short timeout - check frequently for completed tasks so we can spawn more
    // Aggressive task spawning for fast cache fill
    const std::chrono::milliseconds timeout(10);

    while (ioRunning_) {
        //Wait for work (condition variable)
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_for(lock, timeout, [this] {
                return !videoRequests_.empty() ||
                       !requestsInProgress_.empty() ||
                       !ioRunning_;
            });
        }

        if (!ioRunning_) break;

        // Spawn async tasks (up to threadCount concurrent)
        {
            std::lock_guard<std::mutex> lock(mutex_);

            // Check if sequence has been cleared (Shutdown() was called)
            if (!initialized_ || sequenceFiles_.empty()) {
                videoRequests_.clear();  // Clear stale requests
                continue;
            }

            int spawned = 0;
            while (!videoRequests_.empty() &&
                   requestsInProgress_.size() < config_.threadCount) {

                int frame = videoRequests_.front();
                videoRequests_.pop_front();

                // Validate frame index before accessing sequenceFiles_
                // After a seek, old requests may have invalid frame indices
                if (frame < 0 || frame >= (int)sequenceFiles_.size()) {
             /*       Debug::Log("DirectEXRCache: [IO-SKIP] Frame " + std::to_string(frame) +
                               " out of bounds (0-" + std::to_string(sequenceFiles_.size()) + "), skipping");*/
                    continue;
                }

                // Create request
                EXRRequest request;
                request.frame = frame;
                request.byteCount = 3840 * 2160 * 4 * sizeof(half);  // Estimate

                // Spawn async task
                const std::string path = sequenceFiles_[frame];
                const std::string layer = layerName_;

                // Validate path is not empty before spawning async task
                if (path.empty()) {
                   /* Debug::Log("DirectEXRCache: [IO-SKIP] Frame " + std::to_string(frame) +
                               " has empty path, skipping");*/
                    continue;
                }

                request.future = std::async(std::launch::async, [this, path, frame]() {
                    try {
                        auto load_start = std::chrono::steady_clock::now();
                        auto result = LoadPixels(path);
                        auto load_end = std::chrono::steady_clock::now();
                        auto load_ms = std::chrono::duration_cast<std::chrono::milliseconds>(load_end - load_start).count();

                        if (result) {
                           /* Debug::Log("DirectEXRCache: [IO-LOAD] Frame " + std::to_string(frame) +
                                       " loaded in " + std::to_string(load_ms) + "ms (" +
                                       std::to_string(result->pixels.size() / (1024*1024)) + "MB)");*/
                        } else {
                            //Debug::Log("DirectEXRCache: [IO-LOAD] Frame " + std::to_string(frame) + " returned null");
                        }
                        return result;
                    } catch (const std::exception& e) {
                        //Debug::Log("DirectEXRCache: [IO-LOAD] ERROR frame " + std::to_string(frame) + " - " + std::string(e.what()));
                        return std::shared_ptr<PixelData>(nullptr);
                    }
                });

                requestsInProgress_[frame] = std::move(request);
                spawned++;
            }

            if (spawned > 0) {
               /* Debug::Log("DirectEXRCache: [IO-SPAWN] Launched " + std::to_string(spawned) +
                           " async tasks (" + std::to_string(requestsInProgress_.size()) +
                           " in-progress, " + std::to_string(videoRequests_.size()) + " pending)");*/
            }
        }

        // Check completed requests (non-blocking poll)
        {
            std::lock_guard<std::mutex> lock(mutex_);

            int completed = 0;
            auto it = requestsInProgress_.begin();
            while (it != requestsInProgress_.end()) {
                if (it->second.future.valid() &&
                    it->second.future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {

                    try {
                        auto pixelData = it->second.future.get();

                        if (pixelData && !pixelData->pixels.empty()) {
                            // Add directly to pixel cache (no intermediate queue!)
                            size_t byteCount = pixelData->pixels.size();  // Already in bytes (uint8_t vector)
                            pixelCache_.Add(it->first, pixelData, byteCount);
                            segmentsDirty_ = true;  // Mark segments dirty for UI update
                            completed++;
                          /*  Debug::Log("DirectEXRCache: [IO-COMPLETE] Frame " + std::to_string(it->first) +
                                       " added to pixel cache (" + std::to_string(byteCount / (1024*1024)) + "MB)");*/
                        }
                    } catch (const std::exception& e) {
                       /* Debug::Log("DirectEXRCache: Error processing completed request - " +
                                   std::string(e.what()));*/
                    }

                    it = requestsInProgress_.erase(it);
                } else {
                    ++it;
                }
            }
        }
    }

    //Debug::Log("DirectEXRCache: I/O worker thread stopped");
}

//=============================================================================
// Universal Image Loading (wraps EXR or IImageLoader)
//=============================================================================

std::shared_ptr<PixelData> DirectEXRCache::LoadPixels(const std::string& path) {
    // If custom loader is provided, use it
    if (loader_) {
        return loader_->LoadFrame(path, layerName_, pipelineMode_);
    }

    // Otherwise, fall back to legacy EXR loading and convert
    auto exr_pixels = LoadEXRPixels(path, layerName_);
    if (!exr_pixels) {
        return nullptr;
    }

    // Convert EXRPixelData to PixelData
    auto pixels = std::make_shared<PixelData>();
    pixels->width = exr_pixels->width;
    pixels->height = exr_pixels->height;
    pixels->gl_format = GL_RGBA;
    pixels->gl_type = GL_HALF_FLOAT;
    pixels->pipeline_mode = PipelineMode::HDR_RES;  // EXR is always HDR

    // Convert half vector to uint8_t vector (reinterpret bytes)
    size_t byte_count = exr_pixels->pixels.size() * sizeof(half);
    pixels->pixels.resize(byte_count);
    std::memcpy(pixels->pixels.data(), exr_pixels->pixels.data(), byte_count);

    return pixels;
}

//=============================================================================
// EXR Loading (memory-mapped pattern vs. the regular cache setup)
//=============================================================================

std::shared_ptr<EXRPixelData> DirectEXRCache::LoadEXRPixels(const std::string& path,
                                                             const std::string& layer) {
    // Memory-mapped stream 
    auto stream = std::make_unique<MemoryMappedIStream>(path);
    Imf::MultiPartInputFile file(*stream);

    // Get header and dimensions (check both windows)
    const Imf::Header& header = file.header(0);
    const Imath::Box2i displayWindow = header.displayWindow();
    const Imath::Box2i dataWindow = header.dataWindow();

    //Detect fast path when windows match
    const bool fastPath = (displayWindow == dataWindow);

    // Use display window for output dimensions 
    int width = displayWindow.max.x - displayWindow.min.x + 1;
    int height = displayWindow.max.y - displayWindow.min.y + 1;

    // Setup frame buffer
    const Imf::ChannelList& channels = header.channels();

    // Debug logging 
    static bool loggedFileInfo = false;
    if (!loggedFileInfo) {
        const char* compressionNames[] = {
            "NO_COMPRESSION", "RLE", "ZIPS", "ZIP", "PIZ", "PXR24",
            "B44", "B44A", "DWAA", "DWAB"
        };
        int compressionType = static_cast<int>(header.compression());
        const char* compressionName = (compressionType >= 0 && compressionType < 10)
            ? compressionNames[compressionType] : "UNKNOWN";

        // Count total channels in file
        int totalChannels = 0;
        for (auto it = channels.begin(); it != channels.end(); ++it) {
            totalChannels++;
        }

       /* Debug::Log("DirectEXRCache: EXR file info - " +
                   std::to_string(width) + "x" + std::to_string(height) +
                   ", Compression: " + std::string(compressionName) +
                   ", Path: " + (fastPath ? "FAST" : "SLOW") +
                   " (display " + (fastPath ? "==" : "!=") + " data window)" +
                   ", Total channels: " + std::to_string(totalChannels) +
                   ", Requested layer: '" + layer + "'");*/
        loggedFileInfo = true;
    }

    // Allocate pixel buffer with optimizations
    auto data = std::make_shared<EXRPixelData>();
    data->width = width;
    data->height = height;

    // Optimization: Reserve capacity first to avoid reallocation during resize
    // With aligned allocator, this ensures single allocation at proper alignment
    const size_t pixelCount = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
    data->pixels.reserve(pixelCount);
    data->pixels.resize(pixelCount);  // RGBA
    Imf::FrameBuffer frameBuffer;

    // Try with layer prefix first, then without (for single-layer EXRs)
    std::string layerPrefix = layer.empty() ? "" : (layer + ".");

    // Find RGBA channels with layer prefix
    std::string channelR = layerPrefix + "R";
    std::string channelG = layerPrefix + "G";
    std::string channelB = layerPrefix + "B";
    std::string channelA = layerPrefix + "A";

    const Imf::Channel* chR = channels.findChannel(channelR.c_str());
    const Imf::Channel* chG = channels.findChannel(channelG.c_str());
    const Imf::Channel* chB = channels.findChannel(channelB.c_str());
    const Imf::Channel* chA = channels.findChannel(channelA.c_str());

    // If not found with layer prefix, try root-level channels (single-layer EXR)
    if (!chR && !layer.empty()) {
        channelR = "R";
        channelG = "G";
        channelB = "B";
        channelA = "A";
        layerPrefix = "";

        chR = channels.findChannel("R");
        chG = channels.findChannel("G");
        chB = channels.findChannel("B");
        chA = channels.findChannel("A");
    }

    if (!chR || !chG || !chB) {
        Debug::Log("DirectEXRCache: ERROR - Missing RGB channels for layer '" + layer + "' in " + path);
        return nullptr;
    }

    // Check pixel type consistency across channels
    const Imf::PixelType pixelType = chR->type;
    if (pixelType != chG->type || pixelType != chB->type || (chA && pixelType != chA->type)) {
        Debug::Log("DirectEXRCache: ERROR - Inconsistent pixel types across RGBA channels in " + path);
        return nullptr;
    }

    // Check sampling rates (must be 1,1 for non-subsampled)
    if (chR->xSampling != 1 || chR->ySampling != 1 ||
        chG->xSampling != 1 || chG->ySampling != 1 ||
        chB->xSampling != 1 || chB->ySampling != 1 ||
        (chA && (chA->xSampling != 1 || chA->ySampling != 1))) {
        Debug::Log("DirectEXRCache: ERROR - Subsampled channels not supported (sampling must be 1,1) in " + path);
        return nullptr;
    }

    bool hasAlpha = (chA != nullptr);

    // (layerPrefix might have changed during fallback logic)
    std::string fullChannelNames[4] = {
        channelR,
        channelG,
        channelB,
        channelA
    };
    int numChannels = hasAlpha ? 4 : 3;

    // Read pixels using using the dual-path approach
    Imf::InputPart part(file, 0);

    if (fastPath) {
        // FAST PATH: Direct read when display window == data window
        // This is significantly faster for typical EXR files

        // Use detected pixel type, not hardcoded HALF
        const size_t channelByteCount = sizeof(half);  // We always convert to half in our buffer
        const size_t cb = 4 * channelByteCount;  // RGBA stride per pixel
        const size_t scb = width * 4 * channelByteCount;  // Full scanline stride

        for (int c = 0; c < numChannels; ++c) {
            frameBuffer.insert(
                fullChannelNames[c].c_str(),
                Imf::Slice(
                    Imf::HALF,  // CRITICAL: Buffer type, not file's pixelType! OpenEXR converts automatically
                    (char*)(data->pixels.data()) + (c * channelByteCount),
                    cb,   // xStride - move 4 channels per pixel
                    scb,  // yStride - full scanline
                    1, 1, // x,y sampling
                    0.0f  // fill value
                )
            );
        }

        // Fill alpha with 1.0 if no alpha channel (before read)
        if (!hasAlpha) {
            for (int i = 0; i < width * height; ++i) {
                data->pixels[i * 4 + 3] = 1.0f;
            }
        }

        part.setFrameBuffer(frameBuffer);

        // PROFILING: Time the actual decompression
        auto read_start = std::chrono::steady_clock::now();
        part.readPixels(displayWindow.min.y, displayWindow.max.y);
        auto read_end = std::chrono::steady_clock::now();
        auto read_ms = std::chrono::duration_cast<std::chrono::milliseconds>(read_end - read_start).count();

        static int readCount = 0;
        if (readCount++ < 5) {
            Debug::Log("DirectEXRCache: [FAST-PATH-READ] readPixels took " + std::to_string(read_ms) + "ms");
        }

    } else {
        // SLOW PATH: Handle mismatched windows with intermediate buffer
        // This handles edge cases where display != data window
        const Imath::Box2i intersectedWindow = Imath::Box2i(
            Imath::V2i(std::max(displayWindow.min.x, dataWindow.min.x),
                      std::max(displayWindow.min.y, dataWindow.min.y)),
            Imath::V2i(std::min(displayWindow.max.x, dataWindow.max.x),
                      std::min(displayWindow.max.y, dataWindow.max.y))
        );

        const int dataWidth = dataWindow.max.x - dataWindow.min.x + 1;
        const size_t channelByteCount = sizeof(half);
        const size_t cb = 4 * channelByteCount;

        // Optimization: Use aligned buffer for scanline reads
        const size_t bufSize = dataWidth * cb;
        std::vector<char, AlignedAllocator<char, 64>> buf;
        buf.reserve(bufSize);
        buf.resize(bufSize);

        for (int c = 0; c < numChannels; ++c) {
            frameBuffer.insert(
                fullChannelNames[c].c_str(),
                Imf::Slice(
                    Imf::HALF,  // CRITICAL: Buffer type, not file's pixelType! OpenEXR converts automatically
                    buf.data() - (dataWindow.min.x * cb) + (c * channelByteCount),
                    cb,
                    0,  // yStride = 0 for single scanline buffer
                    1, 1,
                    0.0f
                )
            );
        }

        part.setFrameBuffer(frameBuffer);

        // Read scanline by scanline and copy to output
        const size_t scb = width * 4 * channelByteCount;
        for (int y = displayWindow.min.y; y <= displayWindow.max.y; ++y) {
            uint8_t* p = reinterpret_cast<uint8_t*>(data->pixels.data()) +
                        ((y - displayWindow.min.y) * scb);
            uint8_t* end = p + scb;

            if (y >= intersectedWindow.min.y && y <= intersectedWindow.max.y) {
                // Fill left padding
                size_t size = (intersectedWindow.min.x - displayWindow.min.x) * cb;
                std::memset(p, 0, size);
                p += size;

                // Read and copy actual data
                size = (intersectedWindow.max.x - intersectedWindow.min.x + 1) * cb;
                part.readPixels(y, y);
                std::memcpy(
                    p,
                    buf.data() + std::max(displayWindow.min.x - dataWindow.min.x, 0) * cb,
                    size);
                p += size;
            }

            // Fill remaining with zeros
            std::memset(p, 0, end - p);
        }

        // Fill alpha with 1.0 if no alpha channel
        if (!hasAlpha) {
            for (int i = 0; i < width * height; ++i) {
                data->pixels[i * 4 + 3] = 1.0f;
            }
        }
    }

    return data;
}

GLuint DirectEXRCache::CreateGLTexture(const std::shared_ptr<PixelData>& pixels) {
    if (!pixels || pixels->pixels.empty()) {
        return 0;
    }

    GLuint texId = 0;
    glGenTextures(1, &texId);
    glBindTexture(GL_TEXTURE_2D, texId);

    // Determine internal format based on GL type
    GLint internalFormat = GL_RGBA16F;  // Default for HDR
    if (pixels->gl_type == GL_UNSIGNED_BYTE) {
        internalFormat = GL_RGBA8;
    } else if (pixels->gl_type == GL_UNSIGNED_SHORT) {
        internalFormat = GL_RGBA16;
    }

    // Upload pixel data with appropriate format/type
    glTexImage2D(GL_TEXTURE_2D, 0, internalFormat,
                 pixels->width, pixels->height, 0,
                 pixels->gl_format, pixels->gl_type, pixels->pixels.data());

    // Set filtering
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glBindTexture(GL_TEXTURE_2D, 0);

    return texId;
}

} // namespace ump
