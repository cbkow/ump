#include "direct_exr_cache.h"
#include "../utils/debug_utils.h"

#ifdef QCVIEW_USE_VULKAN
#include "../gpu/vulkan_texture_pool.h"
#elif defined(QCVIEW_USE_METAL)
#include "../gpu/metal_texture_pool.h"
#endif

#ifdef _WIN32
#undef min
#undef max
#endif

#include <ImfInputFile.h>
#include <ImfHeader.h>
#include <ImfChannelList.h>
#include <ImfFrameBuffer.h>
#include <ImfInputPart.h>
#include <ImfMultiPartInputFile.h>
#include <ImfThreading.h>

#include <algorithm>
#include <filesystem>

#ifndef _WIN32
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#ifdef __APPLE__
#include <pthread.h>
#endif
#endif

namespace qcview {

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
    // Linux memory-mapped file via mmap
    fd_ = open(fileName.c_str(), O_RDONLY);
    if (fd_ == -1) {
        throw std::runtime_error("Cannot open file: " + fileName);
    }

    struct stat st;
    if (fstat(fd_, &st) == -1) {
        close(fd_);
        fd_ = -1;
        throw std::runtime_error("Cannot get file size: " + fileName);
    }
    fileSize_ = static_cast<uint64_t>(st.st_size);

    mappedData_ = static_cast<char*>(mmap(nullptr, fileSize_, PROT_READ, MAP_PRIVATE, fd_, 0));
    if (mappedData_ == MAP_FAILED) {
        mappedData_ = nullptr;
        close(fd_);
        fd_ = -1;
        throw std::runtime_error("Cannot mmap file: " + fileName);
    }

    // Hint to kernel: sequential access pattern for better read-ahead
    madvise(mappedData_, fileSize_, MADV_SEQUENTIAL);
#endif
}

MemoryMappedIStream::~MemoryMappedIStream() {
#ifdef _WIN32
    if (mappedData_) UnmapViewOfFile(mappedData_);
    if (hMapping_) CloseHandle(hMapping_);
    if (hFile_ != INVALID_HANDLE_VALUE) CloseHandle(hFile_);
#else
    if (mappedData_ && mappedData_ != MAP_FAILED) munmap(mappedData_, fileSize_);
    if (fd_ != -1) close(fd_);
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
    //Debug::Log("DirectEXRCache: Constructor - starting permanent background threads");

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

#if defined(QCVIEW_USE_METAL) || defined(QCVIEW_USE_VULKAN)
    gpu_upload_running_ = true;
    gpu_upload_thread_ = std::thread(&DirectEXRCache::GPUUploadThread, this);
#endif

    //Debug::Log("DirectEXRCache: Permanent background threads started (ready for sequences)");
}

DirectEXRCache::~DirectEXRCache() {
    //Debug::Log("DirectEXRCache: Destructor - stopping permanent background threads");

    // Stop cache thread
    //Debug::Log("DirectEXRCache: Checking cache thread status...");
    if (cacheRunning_) {
        //Debug::Log("DirectEXRCache: Setting cacheRunning_ = false");
        cacheRunning_ = false;
        //Debug::Log("DirectEXRCache: Notifying cache thread to wake up");
        cache_cv_.notify_all();  // Wake cache thread to check running flag
        if (cacheThread_.joinable()) {
            //Debug::Log("DirectEXRCache: Waiting for cache thread to join (this may block if thread is stuck)...");
            cacheThread_.join();
            //Debug::Log("DirectEXRCache: Cache thread joined successfully");
        } else {
            //Debug::Log("DirectEXRCache: Cache thread was not joinable");
        }
    } else {
        //Debug::Log("DirectEXRCache: Cache thread was not running");
    }

    // Stop I/O worker thread
    //Debug::Log("DirectEXRCache: Checking I/O worker thread status...");
    if (ioRunning_) {
        //Debug::Log("DirectEXRCache: Setting ioRunning_ = false");
        ioRunning_ = false;
        //Debug::Log("DirectEXRCache: Notifying I/O worker thread to wake up");
        io_cv_.notify_all();
        if (ioWorkerThread_.joinable()) {
            //Debug::Log("DirectEXRCache: Waiting for I/O worker thread to join (this may block if thread is stuck)...");
            ioWorkerThread_.join();
            //Debug::Log("DirectEXRCache: I/O worker thread joined successfully");
        } else {
            //Debug::Log("DirectEXRCache: I/O worker thread was not joinable");
        }
    } else {
        //Debug::Log("DirectEXRCache: I/O worker thread was not running");
    }

#if defined(QCVIEW_USE_METAL) || defined(QCVIEW_USE_VULKAN)
    // Stop GPU upload thread
    if (gpu_upload_running_) {
        gpu_upload_running_ = false;
        gpu_upload_cv_.notify_all();
        if (gpu_upload_thread_.joinable()) {
            gpu_upload_thread_.join();
        }
    }
    ClearGPUTextures();
#endif

    // Clean up textures before clearing cache
    int texture_count = 0;
    for (auto& pair : glTextureCache_) {
        if (pair.second && pair.second->texture_id != 0) {
#ifdef QCVIEW_USE_VULKAN
            qcview::VulkanTexturePool::Instance().QueueDelete(static_cast<uint64_t>(pair.second->texture_id));
#elif defined(QCVIEW_USE_METAL)
            qcview::MetalTexturePool::Instance().QueueDelete(static_cast<uint64_t>(pair.second->texture_id));
#else
            glDeleteTextures(1, &pair.second->texture_id);
#endif
            texture_count++;
        }
    }
    glTextureCache_.clear();

    // Also delete any textures queued for deletion (from Shutdown() calls)
    for (GLuint tex_id : texturesToDelete_) {
        if (tex_id != 0) {
#ifdef QCVIEW_USE_VULKAN
            qcview::VulkanTexturePool::Instance().QueueDelete(static_cast<uint64_t>(tex_id));
#elif defined(QCVIEW_USE_METAL)
            qcview::MetalTexturePool::Instance().QueueDelete(static_cast<uint64_t>(tex_id));
#else
            glDeleteTextures(1, &tex_id);
#endif
            texture_count++;
        }
    }
    texturesToDelete_.clear();
    //Debug::Log("DirectEXRCache: Deleted " + std::to_string(texture_count) + " GL textures");

    //Debug::Log("DirectEXRCache: Clearing pixel cache...");
    pixelCache_.Clear();
    //Debug::Log("DirectEXRCache: Pixel cache cleared");

    //Debug::Log("DirectEXRCache: Destructor complete - all resources freed");
}

bool DirectEXRCache::Initialize(const std::vector<std::string>& files,
                                const std::string& layer,
                                double fps,
                                int start_frame,
                                double initial_position) {
    auto init_start = std::chrono::steady_clock::now();

    if (files.empty()) {
        //Debug::Log("DirectEXRCache: ERROR - Empty file list");
        return false;
    }

    //Debug::Log("DirectEXRCache: [INIT] Loading new sequence (" + std::to_string(files.size()) + " frames, start frame: " + std::to_string(start_frame) + ")");

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

    // Probe media dimensions from first non-empty file
    // Sentinels use these so gap/broken frames carry the correct resolution
    frameWidth_ = 0;
    frameHeight_ = 0;
    for (const auto& f : files) {
        if (f.empty()) continue;
        if (loader_) {
            if (loader_->GetDimensions(f, frameWidth_, frameHeight_)) break;
        } else {
            if (GetFrameDimensions(f, frameWidth_, frameHeight_)) break;
        }
    }

    // Set cache size as safety cap (frame-based eviction is the primary limiter)
    // Estimate: (readAhead + readBehind) frames * ~64MB per 4K frame * 1.5 buffer
    int totalWindowFrames = config_.readAheadFrames + config_.readBehindFrames + 50;
    size_t estimatedMaxBytes = static_cast<size_t>(totalWindowFrames) * 64 * 1024 * 1024;
    pixelCache_.SetMaxSize(estimatedMaxBytes);

#if defined(QCVIEW_USE_METAL) || defined(QCVIEW_USE_VULKAN)
    // Evict GPU texture when pixel data is evicted from LRU cache
    pixelCache_.SetEvictionCallback([this](const int& frame, const std::shared_ptr<PixelData>&) {
        EvictGPUTexture(frame);
    });
#endif

    initialized_ = true;

    auto init_end = std::chrono::steady_clock::now();
    auto init_ms = std::chrono::duration_cast<std::chrono::milliseconds>(init_end - init_start).count();

    //Debug::Log("DirectEXRCache: [INIT] Sequence loaded in " + std::to_string(init_ms) + "ms (clear: " +
    //           std::to_string(clear_ms) + "ms) - " + std::to_string(files.size()) +
    //           " frames, cache=" + std::to_string(config_.cacheGB) + "GB");

    // Start preloading from initial position (fill cache around playhead)
    //Debug::Log("DirectEXRCache: [INIT] Starting cache fill from position " + std::to_string(initial_position) + "s");
    UpdateCurrentPosition(initial_position);

    return true;
}

bool DirectEXRCache::Initialize(std::unique_ptr<IImageLoader> loader,
                                const std::vector<std::string>& files,
                                const std::string& layer,
                                double fps,
                                PipelineMode pipeline_mode,
                                int start_frame,
                                double initial_position) {
    // Store the loader and pipeline mode
    loader_ = std::move(loader);
    pipelineMode_ = pipeline_mode;

    // Delegate to the original Initialize for the rest
    return Initialize(files, layer, fps, start_frame, initial_position);
}

void DirectEXRCache::Shutdown() {
    // 🔧 NEW BEHAVIOR: Don't stop threads, just clear sequence data
    // This is now just an alias for clearing - threads stay alive
    //Debug::Log("DirectEXRCache: Shutdown called (clearing sequence, keeping threads alive)");

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

#if defined(QCVIEW_USE_METAL) || defined(QCVIEW_USE_VULKAN)
    ClearGPUTextures();
#endif

    {
        std::lock_guard<std::mutex> fl(failed_frames_mutex_);
        failed_frames_.clear();
    }

    initialized_ = false;
    sequenceFiles_.clear();
}

void DirectEXRCache::RequestFrame(int frame) {
    int sequence_size = static_cast<int>(sequenceFiles_.size());
    if (sequence_size == 0) return;

    // Wrap frame index if looping is enabled
    if (is_looping_) {
        // Wrap to valid range: [0, sequence_size - 1]
        frame = ((frame % sequence_size) + sequence_size) % sequence_size;
    } else {
        // Clamp to valid range
        if (frame < 0 || frame >= sequence_size) {
            return;
        }
    }

    // Already in cache?
    if (pixelCache_.Contains(frame)) {
        return;
    }

    // Already failed? (gap/broken sentinel cached, never retry)
    {
        std::lock_guard<std::mutex> fl(failed_frames_mutex_);
        if (failed_frames_.count(frame)) return;
    }

    {
        std::lock_guard<std::mutex> lock(io_mutex_);

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
    }
    io_cv_.notify_one();
}

GLuint DirectEXRCache::GetTexture(int frame, int& width, int& height) {
#if defined(QCVIEW_USE_METAL) || defined(QCVIEW_USE_VULKAN)
    // Metal/Vulkan fast path: GPU textures are pre-created by the upload thread
    {
        std::shared_lock<std::shared_mutex> lock(gpu_texture_mutex_);
        auto it = gpu_texture_ready_.find(frame);
        if (it != gpu_texture_ready_.end()) {
            GLuint tex_id = static_cast<GLuint>(it->second.pool_id);
            width = it->second.width;
            height = it->second.height;

            // Track as last good frame for fallback
            last_good_texture_ = tex_id;
            last_good_width_ = width;
            last_good_height_ = height;
            return tex_id;
        }
    }

    // GPU texture not ready yet — return last good frame (upload thread will create it shortly)
    if (last_good_texture_ != 0) {
        width = last_good_width_;
        height = last_good_height_;
        return last_good_texture_;
    }
    width = 0;
    height = 0;
    return 0;

#else
    // OpenGL path: create GL textures on-demand on the main thread

    // Step 1: Check if we have pixel data in the cache
    std::shared_ptr<PixelData> pixels;
    if (!pixelCache_.Peek(frame, pixels) || !pixels) {
        // FALLBACK: Return last good frame instead of black
        if (last_good_texture_ != 0) {
            width = last_good_width_;
            height = last_good_height_;
            return last_good_texture_;
        }
        width = 0;
        height = 0;
        return 0;  // Frame not in cache yet
    }

    // Touch in SharedMemoryPool for LRU updates (this is a cache hit)
    TouchInPool(frame);

    // Step 2: Check if we already have a GL texture for this frame
    {
        std::lock_guard<std::mutex> lock(textureMutex_);
        auto it = glTextureCache_.find(frame);
        if (it != glTextureCache_.end() && it->second && it->second->texture_id != 0) {
            width = it->second->width;
            height = it->second->height;

            // Track as last good frame for fallback
            last_good_texture_ = it->second->texture_id;
            last_good_width_ = width;
            last_good_height_ = height;
            return it->second->texture_id;  // Return existing GL texture
        }
    }

    // Step 3: Create GL texture on-demand from pixel data
    GLuint texId = CreateGLTexture(pixels);
    if (texId == 0) {
        // FALLBACK: Return last good frame instead of black
        if (last_good_texture_ != 0) {
            width = last_good_width_;
            height = last_good_height_;
            return last_good_texture_;
        }
        width = 0;
        height = 0;
        return 0;
    }

    // Step 4: Add to GL texture cache (with LRU eviction)
    {
        std::lock_guard<std::mutex> lock(textureMutex_);

        // Evict oldest GL texture if cache is full
        if (glTextureCache_.size() >= MAX_GL_TEXTURE_CACHE) {
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

    // Track as last good frame for fallback
    last_good_texture_ = texId;
    last_good_width_ = width;
    last_good_height_ = height;
    return texId;
#endif
}

bool DirectEXRCache::IsFrameCached(int frame) const {
    // Check if frame exists in pixel cache (CPU-side)
    return pixelCache_.Contains(frame);
}

bool DirectEXRCache::GetFrameOrLoad(int frame, GLuint& texture, int& width, int& height) {
#if defined(QCVIEW_USE_METAL) || defined(QCVIEW_USE_VULKAN)
    // Metal/Vulkan: Check gpu_texture_ready_ directly — O(1) lookup, no pixel cache lock.
    // This avoids contention with the I/O thread's pixelCache_.Add() which holds unique_lock.
    texture = GetTexture(frame, width, height);

    {
        std::shared_lock<std::shared_mutex> lock(gpu_texture_mutex_);
        if (gpu_texture_ready_.count(frame)) {
            // GPU texture exists — this is a true cache hit
            consecutive_misses_.store(0);
            last_was_sync_load_.store(false);
            return true;
        }
    }

    // GPU texture not ready — cache miss
    last_was_sync_load_.store(true);
#else
    // OpenGL: Must check pixel cache since textures are created on-demand on main thread
    bool is_actually_cached = pixelCache_.Contains(frame);
    texture = GetTexture(frame, width, height);

    if (is_actually_cached && texture != 0) {
        consecutive_misses_.store(0);
        last_was_sync_load_.store(false);
        return true;
    }

    last_was_sync_load_.store(true);
#endif

    // When stride > 1, don't reactively request frames — read-ahead is the sole loader.
    // Just return the fallback texture immediately (zero I/O for skipped frames).
#if defined(QCVIEW_USE_METAL) || defined(QCVIEW_USE_VULKAN)
    // Metal/Vulkan: CacheThread manages readahead window — don't call RequestFrame
    // from main thread during playback. It takes mutex_ which contends with I/O thread.
    // Only request reactively when NOT playing (scrubbing, seeking).
    if (config_.playbackStride <= 1 && !isPlaying_.load()) {
        RequestFrame(frame);
        if (frame + 1 < (int)sequenceFiles_.size()) {
            RequestFrame(frame + 1);
        }
        io_cv_.notify_one();
    }
#else
    if (config_.playbackStride <= 1) {
        RequestFrame(frame);
        if (frame + 1 < (int)sequenceFiles_.size()) {
            RequestFrame(frame + 1);
        }
        io_cv_.notify_one();
    }
#endif

    // Return last good frame for display continuity
    if (last_good_texture_ != 0) {
        texture = last_good_texture_;
        width = last_good_width_;
        height = last_good_height_;
    }
    return false;
}

void DirectEXRCache::UpdateCurrentPosition(double timestamp) {
    int current_frame = static_cast<int>(timestamp * fps_ + 0.5);

    // Detect seeks and cancel in-flight requests — lock-free using atomics
    bool isSeek = false;
    bool isOverrunSeek = false;
    bool isLoopWrap = false;
    int prev = previousFrame_.load(std::memory_order_relaxed);

    if (prev >= 0) {
        int delta = current_frame - prev;
        if (delta < -2 || delta > 20) {
            // Check if this is a loop wrap-around (jumping from near out to in).
            // The cache already pre-warms in-point frames via wrap-around read-ahead,
            // so clearing requests would flush the pipeline and cause a stutter.
            if (has_loop_range_ && is_looping_ &&
                prev >= loop_out_frame_ - 2 &&
                current_frame >= loop_in_frame_ && current_frame <= loop_in_frame_ + 2) {
                isLoopWrap = true;
            } else {
                isSeek = true;
            }
        }
    }

    if (overrun_mode_.load() && prev >= 0 && current_frame != prev) {
        int delta = current_frame - prev;
        if (delta != 1 && !isLoopWrap) {
            isOverrunSeek = true;
        }
    }

    previousFrame_.store(current_frame, std::memory_order_relaxed);
    lastCacheUpdateFrame_.store(current_frame, std::memory_order_relaxed);
    lastCacheUpdateTime_.store(timestamp, std::memory_order_relaxed);

    // Cancel all in-flight requests on seek (but NOT on loop wrap-arounds)
    if (isSeek) {
        ClearRequests();

        // Reset overrun mode on seek - user is repositioning, give cache a fresh start
        ResetOverrunMode();
    } else if (isOverrunSeek) {
        // In overrun mode, clear queue on any user seek to prioritize new position
        // Don't reset overrun mode - we're still in slow playback, just repositioned
        Debug::Log("DirectEXRCache: [OVERRUN SEEK] Clearing queue for immediate response");
        ClearRequests();
    }

    // Wake up cache thread immediately (don't wait for next tick)
    // This ensures instant response on seeks and initial load
    cache_cv_.notify_one();
}

void DirectEXRCache::UpdatePlaybackState(bool is_playing) {
    // Lock-free — isPlaying_ is atomic, overrun_mode_ is atomic
    bool was_playing = isPlaying_.exchange(is_playing);

    // Reset overrun mode when transitioning to paused
    if (was_playing && !is_playing) {
        ResetOverrunMode();
    }
}

void DirectEXRCache::ResetOverrunMode() {
    bool was_in_overrun = overrun_mode_.exchange(false);
    consecutive_misses_.store(0);
    last_was_sync_load_.store(false);  // Reset sync load tracking

    // Also reset playback speed
    ResetPlaybackSpeed();

    if (was_in_overrun) {
        Debug::Log("DirectEXRCache: Overrun mode RESET - returning to normal cache-ahead");
    }
}

void DirectEXRCache::ResetPlaybackSpeed() {
    consecutive_misses_.store(0);
    last_was_sync_load_.store(false);
}

void DirectEXRCache::SetLooping(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    is_looping_ = enabled;
    Debug::Log("DirectEXRCache: Looping " + std::string(enabled ? "ENABLED" : "DISABLED") +
               " - wrap-around caching " + std::string(enabled ? "active" : "inactive"));
}

void DirectEXRCache::SetLoopRange(int in_frame, int out_frame) {
    std::lock_guard<std::mutex> lock(mutex_);
    loop_in_frame_ = in_frame;
    loop_out_frame_ = out_frame;
    has_loop_range_ = (in_frame >= 0 && out_frame >= 0 && out_frame > in_frame);
    if (has_loop_range_) {
        Debug::Log("DirectEXRCache: Loop range set to [" + std::to_string(in_frame) +
                   ", " + std::to_string(out_frame) + "]");
    }
}

void DirectEXRCache::ClearLoopRange() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (has_loop_range_) {
        Debug::Log("DirectEXRCache: Loop range cleared");
    }
    loop_in_frame_ = -1;
    loop_out_frame_ = -1;
    has_loop_range_ = false;
}

bool DirectEXRCache::GetFrameDimensions(int& width, int& height) const {
    // Prefer probed dimensions (always correct, even before any frames are cached)
    if (frameWidth_ > 0 && frameHeight_ > 0) {
        width = frameWidth_;
        height = frameHeight_;
        return true;
    }

    // Fallback: Return dimensions from first non-sentinel cached pixel data
    auto keys = pixelCache_.GetKeys();
    for (size_t i = 0; i < keys.size(); ++i) {
        std::shared_ptr<PixelData> pixels;
        if (pixelCache_.Get(keys[i], pixels) && pixels && !pixels->is_sentinel) {
            width = pixels->width;
            height = pixels->height;
            return true;
        }
    }

    // Last resort: return default dimensions
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
        //Debug::Log("DirectEXRCache: Failed to get dimensions - " + std::string(e.what()));
        return false;
    }
}

void DirectEXRCache::ProcessReadyTextures() {
    // GL/Vulkan textures created on-demand in GetTexture()
    // This function now ONLY handles deletion of queued textures
    // MUST be called from main thread.

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
#ifdef QCVIEW_USE_VULKAN
        // Vulkan path: Queue deletions via VulkanTexturePool
        auto& pool = VulkanTexturePool::Instance();
        for (GLuint id : toDelete) {
            pool.QueueDelete(static_cast<uint64_t>(id));
        }
        pool.ProcessPendingDeletions();
#elif defined(QCVIEW_USE_METAL)
        // Metal path: Queue deletions via MetalTexturePool
        auto& pool = MetalTexturePool::Instance();
        for (GLuint id : toDelete) {
            pool.QueueDelete(static_cast<uint64_t>(id));
        }
        pool.ProcessPendingDeletions();
#else
        glDeleteTextures(static_cast<GLsizei>(toDelete.size()), toDelete.data());
#endif
    }

#ifdef QCVIEW_USE_VULKAN
    // Also process any pending Vulkan deletions from other sources
    VulkanTexturePool::Instance().ProcessPendingDeletions();
#elif defined(QCVIEW_USE_METAL)
    // Also process any pending Metal deletions from other sources
    MetalTexturePool::Instance().ProcessPendingDeletions();
#endif
}

bool DirectEXRCache::HasPendingTextureDeletions() const {
    std::lock_guard<std::mutex> lock(textureMutex_);
    return !texturesToDelete_.empty();
}

void DirectEXRCache::ClearRequests() {
    size_t pending = 0;
    size_t inProgress = 0;

    {
        std::lock_guard<std::mutex> lock(io_mutex_);
        pending = videoRequests_.size();
        inProgress = requestsInProgress_.size();

        videoRequests_.clear();

        // DON'T clear requestsInProgress_ - that blocks waiting for futures!
        // Let in-progress tasks finish naturally - but we increment generation
        // so their stale results get discarded instead of cached

        // Increment generation so in-progress tasks know their results are stale
        request_generation_.fetch_add(1);

        // Set flag to reset fill counters on next cache update
        // This makes cache fill restart from new seek position
        needsFillReset_ = true;
    }

    /*Debug::Log("DirectEXRCache: Cleared " + std::to_string(pending) +
               " pending + " + std::to_string(inProgress) + " in-progress (cache preserved)");*/
}

void DirectEXRCache::ClearCache() {
    // Clear both requests AND cache (for config changes)
    ClearRequests();

    // Reset overrun mode when cache is cleared (file reload, config change, etc.)
    ResetOverrunMode();

    // Clear pixel cache
    auto pixel_keys = pixelCache_.GetKeys();
    size_t pixel_count = pixel_keys.size();

    // Remove all entries from SharedMemoryPool first
    if (config_.use_shared_pool) {
        for (int frame : pixel_keys) {
            RemoveFromPool(frame);
        }
    }

    pixelCache_.Clear();

    {
        std::lock_guard<std::mutex> fl(failed_frames_mutex_);
        failed_frames_.clear();
    }

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

#if defined(QCVIEW_USE_METAL) || defined(QCVIEW_USE_VULKAN)
    ClearGPUTextures();
#endif

    // Reset last good texture — cache is empty, stale reference would be invalid
    last_good_texture_ = 0;
    last_good_width_ = 0;
    last_good_height_ = 0;

    //Debug::Log("DirectEXRCache: Cleared cache (" + std::to_string(pixel_count) +
    //           " pixel frames) + requests, queued " + std::to_string(textures_to_delete.size()) +
    //           " GL textures for deletion");
}

void DirectEXRCache::SetPlaybackStride(int stride) {
    stride = std::clamp(stride, 1, 4);
    config_.playbackStride = stride;
}

void DirectEXRCache::SetConfig(const EXRCacheConfig& config) {
    if (!config.IsValid()) {
        //Debug::Log("DirectEXRCache: WARNING - Invalid config");
        return;
    }

    // Check if window size changed
    bool windowChanged = (config.readAheadFrames != config_.readAheadFrames ||
                          config.readBehindFrames != config_.readBehindFrames);

    config_ = config;

    // Update cache size as safety cap (frame-based eviction is primary limiter)
    int totalWindowFrames = config_.readAheadFrames + config_.readBehindFrames + 50;
    size_t estimatedMaxBytes = static_cast<size_t>(totalWindowFrames) * 64 * 1024 * 1024;
    pixelCache_.SetMaxSize(estimatedMaxBytes);

    if (windowChanged) {
        // Mark segments dirty so UI updates
        segmentsDirty_ = true;
    }

    //Debug::Log("DirectEXRCache: Config updated - threads=" +
    //           std::to_string(config_.threadCount) + ", cache=" +
    //           std::to_string(config_.cacheGB) + "GB, readBehind=" +
    //           std::to_string(config_.readBehindFrames) + " frames");
}

DirectEXRCache::Stats DirectEXRCache::GetStats() const {
    // Lock-free — reads atomics updated by I/O thread
    Stats stats;
    stats.totalFrames = static_cast<int>(sequenceFiles_.size());
    stats.cachedFrames = static_cast<int>(pixelCache_.GetCount());
    stats.cacheBytes = pixelCache_.GetSize();
    stats.pendingRequests = cached_pending_count_.load(std::memory_order_relaxed);
    stats.inProgressRequests = cached_in_progress_count_.load(std::memory_order_relaxed);
    return stats;
}

std::vector<CacheSegment> DirectEXRCache::GetCacheSegments() const {
    // Always return cached — CacheThread rebuilds in background when dirty
    std::lock_guard<std::mutex> lock(segmentMutex_);
    return cachedSegments_;
}

void DirectEXRCache::RebuildCacheSegments(const std::vector<int>& sorted_keys) {
    std::vector<CacheSegment> segments;

    if (sorted_keys.empty()) {
        std::lock_guard<std::mutex> lock(segmentMutex_);
        cachedSegments_ = segments;
        segmentsDirty_ = false;
        return;
    }

    CacheSegment current;
    current.start_frame = sorted_keys[0];
    current.end_frame = sorted_keys[0];
    current.start_time = sorted_keys[0] / fps_;
    current.end_time = (sorted_keys[0] + 1) / fps_;

    for (size_t i = 1; i < sorted_keys.size(); ++i) {
        if (sorted_keys[i] == current.end_frame + 1) {
            current.end_frame = sorted_keys[i];
            current.end_time = (sorted_keys[i] + 1) / fps_;
        } else {
            current.density = 1.0;
            segments.push_back(current);
            current.start_frame = sorted_keys[i];
            current.end_frame = sorted_keys[i];
            current.start_time = sorted_keys[i] / fps_;
            current.end_time = (sorted_keys[i] + 1) / fps_;
        }
    }

    current.density = 1.0;
    segments.push_back(current);

    std::lock_guard<std::mutex> lock(segmentMutex_);
    cachedSegments_ = segments;
    segmentsDirty_ = false;
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
        // Wait with timeout — uses cache_sleep_mutex_ (no contention with I/O thread)
        {
            std::unique_lock<std::mutex> lock(cache_sleep_mutex_);
            cache_cv_.wait_for(lock, interval);
        }

        // If no sequence loaded, just sleep and check again
        if (!initialized_ || sequenceFiles_.empty()) {
            continue;
        }

        // NOTE: With adaptive speed control, cache thread keeps running even during reduced playback
        // This allows the cache to fill ahead and recover to full speed faster
        // (Previously paused here in overrun mode, now we keep prefetching)

        iteration++;

        // DEBUG: Log every iteration during initial load
        if (iteration <= 10) {
            //Debug::Log("DirectEXRCache: [CACHE-THREAD] Iteration " + std::to_string(iteration) + " starting");
        }

        // Get current playback position (mutex-protected state exchange)
        // Lock-free reads — these are all atomics now
        int current_frame = lastCacheUpdateFrame_.load(std::memory_order_relaxed);
        bool needsReset = needsFillReset_.exchange(false, std::memory_order_relaxed);

        if (needsReset) {
            cacheFillFrame_ = 0;
            cacheFillByteCount_ = 0;
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

            // Get sequence size for wrap-around calculations
            int sequence_size = static_cast<int>(sequenceFiles_.size());

            // Guard against race condition: Shutdown() may have cleared sequenceFiles_
            // between the check at line 725 and here
            if (sequence_size == 0) {
                continue;
            }

            // CRITICAL: Detect seeks BEFORE updating cacheIterationCount_
            // If position jumped >20 frames, reset iteration counter for post-seek boost
            bool isSeek = false;
            if (lastSeekFrame_ >= 0 && std::abs(current_frame - lastSeekFrame_) > 20) {
                isSeek = true;
                iteration = 1;  // Reset for 2-second post-seek boost (MAX_TEXTURES_POST_SEEK = 4)
               /* Debug::Log("DirectEXRCache: [SEEK] Detected jump from frame " +
                           std::to_string(lastSeekFrame_) + " to " + std::to_string(current_frame) +
                           " - resetting iteration counter for post-seek boost");*/

                // Immediately evict stale frames on major seek
                // This prevents memory tracking issues where old frames consume budget
                int readBehindFrames = config_.playbackStride > 1 ? 0 : config_.readBehindFrames;
                int readAheadFrames = std::min(config_.readAheadFrames, 72);  // Smaller immediate window for seek

                auto cached_frames_pre = pixelCache_.GetKeys();
                int immediate_evicted = 0;

                for (int frame : cached_frames_pre) {
                    bool should_evict = false;

                    if (is_looping_) {
                        // WRAP-AROUND EVICTION: Calculate shortest distance considering loop
                        int forward_distance = (frame - current_frame + sequence_size) % sequence_size;
                        int backward_distance = (current_frame - frame + sequence_size) % sequence_size;

                        // Evict if outside window in BOTH directions
                        if (backward_distance > readBehindFrames && forward_distance > readAheadFrames) {
                            should_evict = true;
                        }
                    } else {
                        // LINEAR EVICTION: Keep frames in window [current - readBehind, current + readAhead]
                        int eviction_threshold_behind = current_frame - readBehindFrames;
                        int eviction_threshold_ahead = current_frame + readAheadFrames;

                        if (frame < eviction_threshold_behind || frame > eviction_threshold_ahead) {
                            should_evict = true;
                        }
                    }

                    if (should_evict) {
                        RemoveFromPool(frame);  // Remove from SharedMemoryPool first
#if defined(QCVIEW_USE_METAL) || defined(QCVIEW_USE_VULKAN)
                        EvictGPUTexture(frame);  // Clean up GPU texture BEFORE pixel data removal
#endif
                        pixelCache_.Remove(frame);
                        immediate_evicted++;
                    }
                }

                if (immediate_evicted > 0) {
                    segmentsDirty_ = true;
                }
            }
            lastSeekFrame_ = current_frame;

            // Update cacheIterationCount_ AFTER seek detection (so ProcessReadyTextures sees reset value)
            cacheIterationCount_ = iteration;

            // Evict old frames with read-behind + read-ahead window
            // Calculate read-behind/read-ahead in frames
            // When stride > 1, zero read-behind so all cache budget goes forward
            int readBehindFrames = config_.playbackStride > 1 ? 0 : config_.readBehindFrames;
            // Use configured read-ahead window for eviction
            int readAheadFrames = config_.readAheadFrames;

            auto cached_frames = pixelCache_.GetKeys();
            int evicted_count = 0;

            // Simply evict pixel data - no GL textures involved
            // GL textures are in separate glTextureCache_ and managed by GetTexture()
            for (int frame : cached_frames) {
                bool should_evict = false;

                if (is_looping_ && has_loop_range_) {
                    // LOOP ZONE EVICTION: Evict frames outside the loop range
                    // and use wrap-around distance within the loop zone
                    if (frame < loop_in_frame_ || frame > loop_out_frame_) {
                        // Outside loop zone - evict
                        should_evict = true;
                    } else {
                        // Within loop zone - use wrap-around distance
                        int loop_size = loop_out_frame_ - loop_in_frame_ + 1;
                        int frame_in_loop = frame - loop_in_frame_;
                        int current_in_loop = current_frame - loop_in_frame_;
                        if (current_in_loop < 0) current_in_loop = 0;
                        if (current_in_loop >= loop_size) current_in_loop = loop_size - 1;

                        int forward_distance = (frame_in_loop - current_in_loop + loop_size) % loop_size;
                        int backward_distance = (current_in_loop - frame_in_loop + loop_size) % loop_size;

                        if (backward_distance > readBehindFrames && forward_distance > readAheadFrames) {
                            should_evict = true;
                        }
                    }
                } else if (is_looping_) {
                    // WRAP-AROUND EVICTION: Calculate shortest distance considering loop
                    // Example: frame 5 is 15 frames "ahead" of frame 90 in a 100-frame sequence
                    int forward_distance = (frame - current_frame + sequence_size) % sequence_size;
                    int backward_distance = (current_frame - frame + sequence_size) % sequence_size;

                    // Evict if outside window in BOTH directions
                    if (backward_distance > readBehindFrames && forward_distance > readAheadFrames) {
                        should_evict = true;
                    }
                } else {
                    // LINEAR EVICTION: Keep frames in window [current - readBehind, current + readAhead]
                    int eviction_threshold_behind = current_frame - readBehindFrames;
                    int eviction_threshold_ahead = current_frame + readAheadFrames;

                    if (frame < eviction_threshold_behind || frame > eviction_threshold_ahead) {
                        should_evict = true;
                    }
                }

                if (should_evict) {
                    RemoveFromPool(frame);  // Remove from SharedMemoryPool first
#if defined(QCVIEW_USE_METAL) || defined(QCVIEW_USE_VULKAN)
                    EvictGPUTexture(frame);  // Clean up GPU texture BEFORE pixel data removal
#endif
                    pixelCache_.Remove(frame);
                    evicted_count++;
                }
            }

            if (evicted_count > 0) {
                segmentsDirty_ = true;  // Mark segments dirty after eviction
                // Log periodically to avoid spam
                static int evict_log_count = 0;
                if (evict_log_count++ % 100 == 0) {
                    Debug::Log("DirectEXRCache: Evicted " + std::to_string(evicted_count) + " frames @ pos " +
                               std::to_string(current_frame) + " window=[" +
                               std::to_string(current_frame - readBehindFrames) + "," +
                               std::to_string(current_frame + readAheadFrames) + "]");
                }
            }

            // Step 2: Fill cache with readahead frames
            size_t cached_bytes = pixelCache_.GetSize();
            size_t max_bytes = pixelCache_.GetMaxSize();

            if (cached_bytes < max_bytes) {
                // Calculate available space, accounting for in-progress AND ready-for-texture
                std::lock_guard<std::mutex> lock(io_mutex_);

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

                //=============================================================
                // THREAD-AWARE BATCHING: Queue in half-thread increments
                // Tighter batching keeps frames closer to playhead
                //=============================================================
                int thread_count = static_cast<int>(config_.threadCount);
                int batch_size = std::max(4, thread_count / 2);  // Half threads, min 4

                // Only add more requests when queue is running low
                // This prevents queuing frames far ahead while immediate frames wait
                int queue_size = static_cast<int>(videoRequests_.size());
                int in_progress = static_cast<int>(requestsInProgress_.size());
                int total_pending = queue_size + in_progress;

                // Keep queue filled to ~batch_size, refill when below half
                if (total_pending >= batch_size) {
                    // Plenty of work queued - let threads catch up
                    continue;
                }

                // Only queue enough to fill to batch_size
                int batch_limit = batch_size - total_pending;

                // Use 80% of available space as safety margin
                size_t safe_available = static_cast<size_t>(available * 0.80);
                int max_to_request = std::min(batch_limit, (int)(safe_available / estimated_frame_size));

                // Fill bi-directionally (read-behind + read-ahead)
                int requested_count = 0;

                // Calculate frame ranges for both directions
                // When stride > 1, zero read-behind so all budget goes forward
                int readBehindFrames = config_.playbackStride > 1 ? 0 : config_.readBehindFrames;

                // Fill read-ahead frames (priority for forward playback)
                // When loop range is active, cache ONLY frames within the loop zone
                if (is_looping_ && has_loop_range_) {
                    // LOOP ZONE MODE: Cache frames within [loop_in_frame_, loop_out_frame_]
                    // Use wrap-around distance matching eviction to avoid fill/evict fighting
                    int loop_size = loop_out_frame_ - loop_in_frame_ + 1;
                    int readAheadFrames = config_.readAheadFrames;

                    // Calculate current position within loop zone
                    int current_in_loop = current_frame - loop_in_frame_;
                    if (current_in_loop < 0) current_in_loop = 0;
                    if (current_in_loop >= loop_size) current_in_loop = loop_size - 1;

                    // Fill frames using SAME wrap-around distance logic as eviction
                    // This prevents requesting frames that would be immediately evicted
                    for (int i = 0; i < loop_size && requested_count < max_to_request; i++) {
                        // Calculate frame with wrap-around within loop zone
                        int frame = loop_in_frame_ + ((current_in_loop + i) % loop_size);

                        // Stride: only cache frames on absolute grid (frame % stride == 0)
                        // i=0 is current frame — skip when stride > 1 (same as normal mode)
                        if (config_.playbackStride > 1 && (i == 0 || frame % config_.playbackStride != 0)) continue;
                        int frame_in_loop = frame - loop_in_frame_;

                        // Calculate wrap-around distances (MUST match eviction logic exactly)
                        int forward_distance = (frame_in_loop - current_in_loop + loop_size) % loop_size;
                        int backward_distance = (current_in_loop - frame_in_loop + loop_size) % loop_size;

                        // Skip if outside read-ahead/read-behind window (would be evicted)
                        if (backward_distance > readBehindFrames && forward_distance > readAheadFrames) {
                            continue;
                        }

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
                } else {
                    // NORMAL MODE: Read-ahead from current position

                    // PRIORITY: Always request current frame FIRST (fixes first-frame delay on high-res files)
                    // When stride > 1, skip this — read-ahead pre-caches stride frames,
                    // non-stride frames are intentional misses (show fallback texture)
                    if (config_.playbackStride <= 1) {
                        if (requested_count < max_to_request &&
                            !pixelCache_.Contains(current_frame) &&
                            requestsInProgress_.find(current_frame) == requestsInProgress_.end()) {
                            bool already_pending = false;
                            for (int pending : videoRequests_) {
                                if (pending == current_frame) { already_pending = true; break; }
                            }
                            if (!already_pending) {
                                videoRequests_.push_back(current_frame);
                                requested_count++;
                            }
                        }
                    }

                    for (int i = 1; i <= config_.readAheadFrames && requested_count < max_to_request; i++) {
                        int frame = current_frame + i;

                        // Wrap or clamp based on looping mode
                        if (is_looping_) {
                            // No loop zone: wrap to beginning of sequence
                            frame = frame % sequence_size;
                        } else {
                            // Stop at end
                            if (frame >= sequence_size) break;
                        }

                        // Stride: only cache frames on absolute grid (frame % stride == 0)
                        // Using absolute frame numbers prevents the grid from shifting every
                        // display frame, which would defeat the stride filter entirely
                        if (config_.playbackStride > 1 && (frame % config_.playbackStride != 0)) continue;

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
                    // Skip entirely when stride > 1 — all budget goes to read-ahead
                    // Only fill if we have remaining capacity (skip in loop zone mode - handled above)
                    for (int i = 1; config_.playbackStride <= 1 && requested_count < max_to_request && i <= readBehindFrames; i++) {
                        int frame = current_frame - i;

                        // Wrap or clamp based on looping mode
                        if (is_looping_) {
                            // No loop zone: wrap to end of sequence
                            frame = ((frame % sequence_size) + sequence_size) % sequence_size;
                        } else {
                            // Stop at beginning
                            if (frame < 0) break;
                        }

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

                        if (dist > 0 && frame_minus >= 0 && config_.playbackStride <= 1 && pixelCache_.Contains(frame_minus)) {
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
                   /* Debug::Log("DirectEXRCache: [ITER-" + std::to_string(iteration) + "] " +
                               std::to_string(iter_ms) + "ms - Requested " +
                               std::to_string(requested_count) + "/" + std::to_string(batch_limit) +
                               " frames (cached: " + std::to_string(cached_bytes / (1024*1024)) +
                               "MB + in-progress: " + std::to_string(in_progress_bytes / (1024*1024)) +
                               "MB = " + std::to_string(total_committed / (1024*1024)) +
                               "MB / " + std::to_string(max_bytes / (1024*1024)) + "MB)");*/
                    io_cv_.notify_one();  // Wake up I/O worker
                }
            }
        }

        // Rebuild cache segments in background if dirty
        if (segmentsDirty_.load()) {
            auto keys = pixelCache_.GetKeys();
            std::sort(keys.begin(), keys.end());
            RebuildCacheSegments(keys);
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
#ifdef __APPLE__
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
#endif

    // Short timeout - check frequently for completed tasks so we can spawn more
    // Aggressive task spawning for fast cache fill
    const std::chrono::milliseconds timeout(10);

    while (ioRunning_) {
        // Wait for work — uses io_mutex_/io_cv_ (no contention with CacheThread)
        {
            std::unique_lock<std::mutex> lock(io_mutex_);
            io_cv_.wait_for(lock, timeout, [this] {
                return !videoRequests_.empty() ||
                       !requestsInProgress_.empty() ||
                       !ioRunning_;
            });
        }

        if (!ioRunning_) break;

        // Use full thread count for maximum cache fill rate
        const size_t max_concurrent = config_.threadCount;

        // Collect frames to spawn under mutex, then spawn outside it.
        // This prevents std::async from blocking while mutex_ is held,
        // which would stall the main thread's UpdateCurrentPosition.
        struct SpawnItem {
            int frame;
            std::string path;
            uint64_t generation;
        };
        std::vector<SpawnItem> to_spawn;

        {
            std::lock_guard<std::mutex> lock(io_mutex_);

            // Check if sequence has been cleared (Shutdown() was called)
            if (!initialized_ || sequenceFiles_.empty()) {
                videoRequests_.clear();
                continue;
            }

            while (!videoRequests_.empty() &&
                   requestsInProgress_.size() + to_spawn.size() < max_concurrent) {

                int frame = videoRequests_.front();
                videoRequests_.pop_front();

                if (frame < 0 || frame >= (int)sequenceFiles_.size()) {
                    continue;
                }

                const std::string path = sequenceFiles_[frame];

                // Gap frame (empty path) → handle outside lock
                if (path.empty()) {
                    to_spawn.push_back({frame, "", request_generation_.load()});
                    continue;
                }

                to_spawn.push_back({frame, path, request_generation_.load()});
            }
        } // mutex_ released BEFORE spawning async tasks

        // Process collected items outside the lock
        for (auto& item : to_spawn) {
            if (item.path.empty()) {
                // Gap sentinel
                auto sentinel = MakeGapSentinel(frameWidth_, frameHeight_);
                pixelCache_.Add(item.frame, sentinel, kSentinelCacheByteSize);
                {
                    std::lock_guard<std::mutex> fl(failed_frames_mutex_);
                    failed_frames_.insert(item.frame);
                }
                segmentsDirty_ = true;
                continue;
            }

            // Spawn async task — std::async may block waiting for thread pool,
            // but we no longer hold mutex_ so main thread isn't stalled
            EXRRequest request;
            request.frame = item.frame;
            request.byteCount = 3840 * 2160 * 4 * sizeof(half);
            request.generation = item.generation;

            const std::string spawn_path = item.path;
            request.future = std::async(std::launch::async, [this, spawn_path]() {
#ifdef __APPLE__
                pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
#endif
                try {
                    return LoadPixels(spawn_path);
                } catch (const std::exception&) {
                    return std::shared_ptr<PixelData>(nullptr);
                }
            });

            {
                std::lock_guard<std::mutex> lock(io_mutex_);
                requestsInProgress_[item.frame] = std::move(request);
            }
        }

        // Check completed requests (non-blocking poll)
        // Collect completed futures under mutex_, process results outside it.
        // This prevents the I/O thread from blocking the main thread during
        // pixelCache_.Add() which can trigger LRU eviction.
        struct CompletedResult {
            int frame;
            std::shared_ptr<PixelData> pixels;
            uint64_t generation;
        };
        std::vector<CompletedResult> completed_results;

        {
            std::lock_guard<std::mutex> lock(io_mutex_);

            auto it = requestsInProgress_.begin();
            while (it != requestsInProgress_.end()) {
                if (it->second.future.valid() &&
                    it->second.future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {

                    try {
                        auto pixelData = it->second.future.get();
                        completed_results.push_back({it->first, pixelData, it->second.generation});
                    } catch (const std::exception&) {
                        completed_results.push_back({it->first, nullptr, it->second.generation});
                    }

                    it = requestsInProgress_.erase(it);
                } else {
                    ++it;
                }
            }
        } // mutex_ released — main thread can now access requestsInProgress_ freely

        // Process completed results outside the mutex
        bool any_new_pixels = false;
        for (auto& result : completed_results) {
            uint64_t current_gen = request_generation_.load();
            bool is_stale = (result.generation != current_gen);

            if (is_stale) {
                // Discard stale result — user seeked since this was requested
            } else if (result.pixels && !result.pixels->pixels.empty()) {
                size_t byteCount = result.pixels->is_sentinel ? kSentinelCacheByteSize : result.pixels->pixels.size();
                pixelCache_.Add(result.frame, result.pixels, byteCount);

                if (result.pixels->is_sentinel) {
                    std::lock_guard<std::mutex> fl(failed_frames_mutex_);
                    failed_frames_.insert(result.frame);
                }

                RegisterWithPool(result.frame, byteCount);
                segmentsDirty_ = true;

                if (!result.pixels->is_sentinel) {
                    any_new_pixels = true;
#if defined(QCVIEW_USE_METAL) || defined(QCVIEW_USE_VULKAN)
                    // Push to GPU upload queue — separate from pixelCache_ to avoid contention
                    {
                        std::lock_guard<std::mutex> ql(gpu_upload_queue_mutex_);
                        gpu_upload_queue_.push_back({result.frame, result.pixels, result.generation});
                    }
#endif
                }
            } else if (!is_stale) {
                auto sentinel = MakeBrokenSentinel(frameWidth_, frameHeight_);
                pixelCache_.Add(result.frame, sentinel, kSentinelCacheByteSize);
                {
                    std::lock_guard<std::mutex> fl(failed_frames_mutex_);
                    failed_frames_.insert(result.frame);
                }
                segmentsDirty_ = true;
            }
        }

#if defined(QCVIEW_USE_METAL) || defined(QCVIEW_USE_VULKAN)
        if (any_new_pixels) {
            gpu_upload_cv_.notify_one();
        }
#endif
        // Update cached counts for lock-free GetStats()
        {
            std::lock_guard<std::mutex> lock(io_mutex_);
            cached_pending_count_.store(static_cast<int>(videoRequests_.size()), std::memory_order_relaxed);
            cached_in_progress_count_.store(static_cast<int>(requestsInProgress_.size()), std::memory_order_relaxed);
        }
    }

    //Debug::Log("DirectEXRCache: I/O worker thread stopped");
}

#if defined(QCVIEW_USE_METAL) || defined(QCVIEW_USE_VULKAN)
//=============================================================================
// GPU Upload Thread (Metal/Vulkan) — creates textures off the main thread
//=============================================================================

void DirectEXRCache::GPUUploadThread() {
#ifdef __APPLE__
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
#endif

    while (gpu_upload_running_) {
        // Wait for I/O thread to push items or shutdown
        {
            std::unique_lock<std::mutex> lock(gpu_upload_mutex_);
            gpu_upload_cv_.wait_for(lock, std::chrono::milliseconds(16), [this] {
                return !gpu_upload_running_;
            });
        }

        if (!gpu_upload_running_) break;

        // Drain the upload queue — I/O thread pushes completed pixel data here
        // This never touches pixelCache_, eliminating shared_mutex contention
        std::deque<GPUUploadItem> items;
        {
            std::lock_guard<std::mutex> ql(gpu_upload_queue_mutex_);
            items.swap(gpu_upload_queue_);  // O(1) swap, clears queue
        }

        // Limit uploads per cycle to avoid starving the main render thread
        // Metal texture creation is fast (shared storage), so allow higher throughput
        int uploads_this_cycle = 0;
        constexpr int kMaxUploadsPerCycle = 8;

        size_t i = 0;
        for (; i < items.size(); ++i) {
            auto& item = items[i];
            if (!gpu_upload_running_) break;

            // Check for stale generation (user seeked since I/O started)
            if (item.generation != request_generation_.load()) continue;

            // Already have a GPU texture for this frame?
            {
                std::shared_lock<std::shared_mutex> lock(gpu_texture_mutex_);
                if (gpu_texture_ready_.count(item.frame)) continue;
            }

            // Limit texture creation per cycle to avoid starving main render thread
            if (++uploads_this_cycle > kMaxUploadsPerCycle) {
                break;  // Put remaining back in queue
            }

            // Create GPU texture (thread-safe on Metal/Vulkan)
            GLuint tex_id = CreateGLTexture(item.pixels);
            if (tex_id == 0) continue;

            // Store in ready map
            {
                std::unique_lock<std::shared_mutex> lock(gpu_texture_mutex_);
                if (item.generation != request_generation_.load()) {
                    // Seek happened — delete stale texture
#ifdef QCVIEW_USE_VULKAN
                    VulkanTexturePool::Instance().QueueDelete(static_cast<uint64_t>(tex_id));
#elif defined(QCVIEW_USE_METAL)
                    MetalTexturePool::Instance().QueueDelete(static_cast<uint64_t>(tex_id));
#endif
                    continue;
                }
                gpu_texture_ready_[item.frame] = {
                    static_cast<uint64_t>(tex_id),
                    item.pixels->width,
                    item.pixels->height
                };
            }
        }

        // Put unprocessed items back in the queue for next cycle
        if (i < items.size()) {
            std::lock_guard<std::mutex> ql(gpu_upload_queue_mutex_);
            for (size_t j = i; j < items.size(); ++j) {
                gpu_upload_queue_.push_back(std::move(items[j]));
            }
        }
    }
}

void DirectEXRCache::EvictGPUTexture(int frame) {
    std::unique_lock<std::shared_mutex> lock(gpu_texture_mutex_);
    auto it = gpu_texture_ready_.find(frame);
    if (it != gpu_texture_ready_.end()) {
#ifdef QCVIEW_USE_VULKAN
        VulkanTexturePool::Instance().QueueDelete(it->second.pool_id);
#elif defined(QCVIEW_USE_METAL)
        MetalTexturePool::Instance().QueueDelete(it->second.pool_id);
#endif
        gpu_texture_ready_.erase(it);
    }
}

void DirectEXRCache::ClearGPUTextures() {
    std::unique_lock<std::shared_mutex> lock(gpu_texture_mutex_);
    for (auto& [frame, entry] : gpu_texture_ready_) {
#ifdef QCVIEW_USE_VULKAN
        VulkanTexturePool::Instance().QueueDelete(entry.pool_id);
#elif defined(QCVIEW_USE_METAL)
        MetalTexturePool::Instance().QueueDelete(entry.pool_id);
#endif
    }
    gpu_texture_ready_.clear();

    // Clear the upload queue too — stale pixel data shouldn't create textures
    {
        std::lock_guard<std::mutex> ql(gpu_upload_queue_mutex_);
        gpu_upload_queue_.clear();
    }
}

#endif // QCVIEW_USE_METAL || QCVIEW_USE_VULKAN

//=============================================================================
// Universal Image Loading (wraps EXR or IImageLoader)
//=============================================================================

std::shared_ptr<PixelData> DirectEXRCache::LoadPixels(const std::string& path) {
    // Gap sentinel: empty path means frame doesn't exist in sequence
    if (path.empty()) {
        return MakeGapSentinel(frameWidth_, frameHeight_);
    }

    // If custom loader is provided, use it (handles missing/broken files internally)
    if (loader_) {
        try {
            return loader_->LoadFrame(path, layerName_, pipelineMode_);
        } catch (...) {
            return MakeGapSentinel(frameWidth_, frameHeight_);
        }
    }

    // EXR path: MemoryMappedIStream constructor opens + stats the file,
    // throwing on missing/unreadable files. No need for separate stat() calls.
    try {
        return LoadEXRPixels(path, layerName_);
    } catch (...) {
        return MakeGapSentinel(frameWidth_, frameHeight_);
    }
}

//=============================================================================
// EXR Loading (memory-mapped pattern vs. the regular cache setup)
//=============================================================================

std::shared_ptr<PixelData> DirectEXRCache::LoadEXRPixels(const std::string& path,
                                                         const std::string& layer) {
    // Memory-mapped stream
    auto stream = std::make_unique<MemoryMappedIStream>(path);
    Imf::MultiPartInputFile file(*stream);

    // Find the correct part for the requested layer
    int numParts = file.parts();
    int targetPartIndex = 0;  // Default to part 0

    if (numParts > 1 && !layer.empty()) {
        // Multi-part EXR: find the part that matches the layer name
        for (int p = 0; p < numParts; ++p) {
            const Imf::Header& partHeader = file.header(p);
            if (partHeader.hasName()) {
                std::string partName = partHeader.name();
                if (partName == layer) {
                    targetPartIndex = p;
                    break;
                }
            }
        }
    }

    // Get header and dimensions from the target part
    const Imf::Header& header = file.header(targetPartIndex);
    const Imath::Box2i displayWindow = header.displayWindow();
    const Imath::Box2i dataWindow = header.dataWindow();

    // Detect fast path when windows match
    const bool fastPath = (displayWindow == dataWindow);

    // Full resolution dimensions
    int width = displayWindow.max.x - displayWindow.min.x + 1;
    int height = displayWindow.max.y - displayWindow.min.y + 1;

    // Setup frame buffer
    const Imf::ChannelList& channels = header.channels();

    // Allocate PixelData directly — no intermediate EXRPixelData + memcpy
    auto data = std::make_shared<PixelData>();
    data->width = width;
    data->height = height;
    data->SetFormat(PixelFormat::RGBA16F);
    data->pipeline_mode = PipelineMode::HDR_RES;

    // Allocate as bytes, interpret as half* for OpenEXR framebuffer slices
    const size_t pixelCount = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
    const size_t byteCount = pixelCount * sizeof(half);
    data->pixels.resize(byteCount);
    half* halfPtr = reinterpret_cast<half*>(data->pixels.data());
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
        //Debug::Log("DirectEXRCache: ERROR - Missing RGB channels for layer '" + layer + "' in " + path);
        return nullptr;
    }

    // Check pixel type consistency across channels
    const Imf::PixelType pixelType = chR->type;
    if (pixelType != chG->type || pixelType != chB->type || (chA && pixelType != chA->type)) {
        //Debug::Log("DirectEXRCache: ERROR - Inconsistent pixel types across RGBA channels in " + path);
        return nullptr;
    }

    // Check sampling rates (must be 1,1 for non-subsampled)
    if (chR->xSampling != 1 || chR->ySampling != 1 ||
        chG->xSampling != 1 || chG->ySampling != 1 ||
        chB->xSampling != 1 || chB->ySampling != 1 ||
        (chA && (chA->xSampling != 1 || chA->ySampling != 1))) {
        //Debug::Log("DirectEXRCache: ERROR - Subsampled channels not supported (sampling must be 1,1) in " + path);
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
    Imf::InputPart part(file, targetPartIndex);

    if (fastPath) {
        // FAST PATH: Direct read when display window == data window
        // This is significantly faster for typical EXR files

        const size_t channelByteCount = sizeof(half);  // We always convert to half in our buffer
        const size_t cb = 4 * channelByteCount;  // RGBA stride per pixel
        const size_t scb = width * 4 * channelByteCount;  // Full scanline stride

        for (int c = 0; c < numChannels; ++c) {
            frameBuffer.insert(
                fullChannelNames[c].c_str(),
                Imf::Slice(
                    Imf::HALF,  // CRITICAL: Buffer type, not file's pixelType! OpenEXR converts automatically
                    (char*)(halfPtr) + (c * channelByteCount),
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
                halfPtr[i * 4 + 3] = 1.0f;
            }
        }

        part.setFrameBuffer(frameBuffer);
        part.readPixels(displayWindow.min.y, displayWindow.max.y);

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
            uint8_t* p = data->pixels.data() +
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
                halfPtr[i * 4 + 3] = 1.0f;
            }
        }
    }

    return data;
}

GLuint DirectEXRCache::CreateGLTexture(const std::shared_ptr<PixelData>& pixels) {
    if (!pixels || pixels->pixels.empty()) {
        return 0;
    }

#ifdef QCVIEW_USE_VULKAN
    // Vulkan path: Create texture via VulkanTexturePool
    auto& pool = VulkanTexturePool::Instance();
    if (!pool.IsInitialized()) {
        return 0;
    }

    // Map PixelFormat to VkFormat
    VkFormat vk_format = VK_FORMAT_R8G8B8A8_UNORM;
    switch (pixels->pixel_format) {
        case PixelFormat::RGBA8:  vk_format = VK_FORMAT_R8G8B8A8_UNORM; break;
        case PixelFormat::RGBA16: vk_format = VK_FORMAT_R16G16B16A16_UNORM; break;
        case PixelFormat::RGBA16F: vk_format = VK_FORMAT_R16G16B16A16_SFLOAT; break;
    }

    uint64_t pool_id = pool.CreateTextureFromPixels(
        pixels->width, pixels->height,
        vk_format,
        pixels->pixels.data(),
        pixels->pixels.size()
    );

    // Pool IDs are small sequential integers, safe to truncate to GLuint
    return static_cast<GLuint>(pool_id);
#elif defined(QCVIEW_USE_METAL)
    // Metal path: Create texture via MetalTexturePool
    auto& pool = MetalTexturePool::Instance();
    if (!pool.IsInitialized()) {
        return 0;
    }

    // Map PixelFormat to Metal format constant
    // 0 = RGBA8Unorm/RGBA16Unorm, 1 = RGBA16Float
    int metal_format = 0;
    switch (pixels->pixel_format) {
        case PixelFormat::RGBA8:  metal_format = 0; break;
        case PixelFormat::RGBA16: metal_format = 0; break;
        case PixelFormat::RGBA16F: metal_format = 1; break;
    }

    uint64_t pool_id = pool.CreateTextureFromPixels(
        pixels->width, pixels->height,
        metal_format,
        pixels->pixels.data(),
        pixels->pixels.size()
    );

    // Pool IDs are small sequential integers, safe to truncate to GLuint
    return static_cast<GLuint>(pool_id);
#else
    // OpenGL path
    // Save current GL state to avoid corrupting ImGui during render
    GLint previous_texture = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous_texture);

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

    // Restore previous texture binding (critical for ImGui compatibility)
    glBindTexture(GL_TEXTURE_2D, previous_texture);

    return texId;
#endif
}

//=============================================================================
// SharedMemoryPool Integration
//=============================================================================

void DirectEXRCache::RegisterWithPool(int frame, size_t bytes) {
    if (!config_.use_shared_pool) return;

    std::string seq_id = GetSequenceIdentifier();
    if (seq_id.empty()) return;

    auto key = MakeEXRKey(seq_id, frame);

    // Create eviction callback that removes from our local cache
    auto eviction_callback = [this, frame]() {
        OnPoolEviction(frame);
    };

    SharedMemoryPool::Instance().RegisterEntry(key, bytes, eviction_callback);
}

void DirectEXRCache::TouchInPool(int frame) {
    if (!config_.use_shared_pool) return;

    std::string seq_id = GetSequenceIdentifier();
    if (seq_id.empty()) return;

    auto key = MakeEXRKey(seq_id, frame);
    SharedMemoryPool::Instance().TouchEntry(key);
}

void DirectEXRCache::RemoveFromPool(int frame) {
    if (!config_.use_shared_pool) return;

    std::string seq_id = GetSequenceIdentifier();
    if (seq_id.empty()) return;

    auto key = MakeEXRKey(seq_id, frame);
    SharedMemoryPool::Instance().RemoveEntry(key);
}

void DirectEXRCache::OnPoolEviction(int frame) {
    // Called by SharedMemoryPool when this frame is evicted due to memory pressure
    // Remove from our local cache (this is thread-safe via pixelCache_ mutex)

    // Check if we also have a GL texture for this frame
    std::lock_guard<std::mutex> lock(textureMutex_);

    auto gl_it = glTextureCache_.find(frame);
    if (gl_it != glTextureCache_.end()) {
        // Queue texture for deletion on main thread
        if (gl_it->second && gl_it->second->texture_id != 0) {
            texturesToDelete_.push_back(gl_it->second->texture_id);
        }
        glTextureCache_.erase(gl_it);
    }

    // Remove from pixel cache without calling pool again (we're already being evicted)
    pixelCache_.Remove(frame);

    segmentsDirty_ = true;
}

} // namespace qcview
