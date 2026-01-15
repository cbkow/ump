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
        cv_.notify_all();  // Wake threads to check running flag
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
        cv_.notify_all();
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

    // Clean up GL textures before clearing cache
    //Debug::Log("DirectEXRCache: Deleting GL textures...");
    int texture_count = 0;
    for (auto& pair : glTextureCache_) {
        if (pair.second && pair.second->texture_id != 0) {
            glDeleteTextures(1, &pair.second->texture_id);
            texture_count++;
        }
    }
    glTextureCache_.clear();

    // Also delete any textures queued for deletion (from Shutdown() calls)
    for (GLuint tex_id : texturesToDelete_) {
        if (tex_id != 0) {
            glDeleteTextures(1, &tex_id);
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

    // Set cache size as safety cap (frame-based eviction is the primary limiter)
    // Estimate: (readAhead + readBehind) frames * ~64MB per 4K frame * 1.5 buffer
    int totalWindowFrames = config_.readAheadFrames + static_cast<int>(config_.readBehindSeconds * fps_) + 50;
    size_t estimatedMaxBytes = static_cast<size_t>(totalWindowFrames) * 64 * 1024 * 1024;
    pixelCache_.SetMaxSize(estimatedMaxBytes);

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

    // Track as last good frame for fallback
    last_good_texture_ = texId;
    last_good_width_ = width;
    last_good_height_ = height;
    return texId;
}

bool DirectEXRCache::IsFrameCached(int frame) const {
    // Check if frame exists in pixel cache (CPU-side)
    return pixelCache_.Contains(frame);
}

bool DirectEXRCache::GetFrameOrLoad(int frame, GLuint& texture, int& width, int& height) {
    // CRITICAL: Check pixel cache directly to detect true cache hits vs fallback textures
    // GetTexture() returns last_good_texture_ on miss, which masks the actual cache state
    bool is_actually_cached = pixelCache_.Contains(frame);

    // Get texture (may return fallback if not cached)
    texture = GetTexture(frame, width, height);

    // Calculate frames ahead (for adaptive speed decisions)
    int frames_ahead = 0;
    {
        auto keys = pixelCache_.GetKeys();
        for (int cached_frame : keys) {
            if (cached_frame > frame && cached_frame <= frame + config_.readAheadFrames) {
                frames_ahead++;
            }
        }
        frames_ahead_count_.store(frames_ahead);
    }

    // If frame is actually in pixel cache, it's a true cache hit
    if (is_actually_cached && texture != 0) {
        consecutive_misses_.store(0);
        last_was_sync_load_.store(false);  // Cache hit - not a sync load

        // Hybrid adaptive speed control:
        // - Buffer healthy → full speed (don't check rate)
        // - Buffer low → use rate to determine sustainable speed
        // - Already slowed → use rate to decide when to speed up
        if (isPlaying_ && fps_ > 0) {
            double current_speed = playback_speed_factor_.load();
            auto now = std::chrono::steady_clock::now();
            auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - last_speed_change_time_).count();

            constexpr int BUFFER_HEALTHY_THRESHOLD = 70; // Frames ahead = safe, can restore speed
            constexpr int BUFFER_LOW_THRESHOLD = 38;     // Frames ahead = getting risky, check rate

            if (frames_ahead >= BUFFER_HEALTHY_THRESHOLD) {
                // BUFFER HEALTHY: Stay at full speed, ignore rate
                if (current_speed < 1.0 && elapsed_ms >= SPEED_RESTORE_DEBOUNCE_MS) {
                    playback_speed_factor_.store(1.0);
                    last_speed_change_time_ = now;
                    overrun_mode_.store(false);
                    Debug::Log("DirectEXRCache: Buffer healthy (" + std::to_string(frames_ahead) +
                               " ahead) - restored to full speed");
                }
            } else if (frames_ahead < BUFFER_LOW_THRESHOLD) {
                // BUFFER LOW: Use rate to determine sustainable speed
                double fill_rate = measured_fill_rate_.load();
                if (fill_rate > 0.1) {
                    double sustainable_speed = (fill_rate * RATE_SAFETY_MARGIN) / fps_;

                    // Quantize to nice steps
                    double new_speed;
                    if (sustainable_speed >= 0.75) {
                        new_speed = 1.0;
                    } else if (sustainable_speed >= 0.375) {
                        new_speed = 0.5;
                    } else if (sustainable_speed >= 0.1875) {
                        new_speed = 0.25;
                    } else {
                        new_speed = 0.125;
                    }

                    if (new_speed < current_speed) {
                        playback_speed_factor_.store(new_speed);
                        last_speed_change_time_ = now;
                        overrun_mode_.store(new_speed < 1.0);
                        Debug::Log("DirectEXRCache: Buffer low (" + std::to_string(frames_ahead) +
                                   " ahead) - rate-based slowdown to " + std::to_string(new_speed) +
                                   " (fill=" + std::to_string(fill_rate) + "fps)");
                    }
                }
            }
            // else: BUFFER MEDIUM (3-7 frames) - maintain current speed, wait and see
        }
        return true;
    }

    // === CACHE MISS (frame not in pixel cache) ===
    last_was_sync_load_.store(true);

    if (isPlaying_ && fps_ > 0) {
        consecutive_misses_.fetch_add(1);
        double current_speed = playback_speed_factor_.load();
        double fill_rate = measured_fill_rate_.load();

        // Cache miss = we're consuming faster than filling
        // Drop to sustainable speed immediately
        double sustainable_speed = (fill_rate > 0.1)
            ? (fill_rate * RATE_SAFETY_MARGIN) / fps_
            : 0.125;  // Fallback if no rate data yet

        // Quantize to nice steps
        double new_speed;
        if (sustainable_speed >= 0.75) {
            new_speed = 1.0;
        } else if (sustainable_speed >= 0.375) {
            new_speed = 0.5;
        } else if (sustainable_speed >= 0.1875) {
            new_speed = 0.25;
        } else {
            new_speed = 0.125;
        }

        if (new_speed < current_speed) {
            playback_speed_factor_.store(new_speed);
            last_speed_change_time_ = std::chrono::steady_clock::now();
            overrun_mode_.store(true);
            Debug::Log("DirectEXRCache: Cache miss - drop to " + std::to_string(new_speed) +
                       " (fill=" + std::to_string(fill_rate) + "fps, target=" +
                       std::to_string(fps_) + "fps)");
        }
    }

    // Request frame via background thread (always keep prefetching!)
    RequestFrame(frame);

    // Also request next frame for lookahead
    if (frame + 1 < (int)sequenceFiles_.size()) {
        RequestFrame(frame + 1);
    }

    // Wake up IO thread immediately
    cv_.notify_one();

    // Return last good frame for display continuity
    if (last_good_texture_ != 0) {
        texture = last_good_texture_;
        width = last_good_width_;
        height = last_good_height_;
    }
    return false;
}

void DirectEXRCache::UpdateCurrentPosition(double timestamp) {
    int current_frame = static_cast<int>(timestamp * fps_);

    // Detect seeks and cancel in-flight requests
    bool isSeek = false;
    bool isOverrunSeek = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);

        // Detect seek: forward jump > 20 frames, OR backward jump > 2 frames
        // Backward seeks (loops, user scrub) should always reset overrun mode
        // Forward threshold is higher to avoid false positives from playback jitter
        if (previousFrame_ >= 0) {
            int delta = current_frame - previousFrame_;
            if (delta < -2 || delta > 20) {
                isSeek = true;
            }
        }

        // In overrun mode, ANY position change (even 1 frame jump from user seek) should clear queue
        // This ensures immediate response when user scrubs during overrun playback
        if (overrun_mode_.load() && previousFrame_ >= 0 && current_frame != previousFrame_) {
            // Check if this is a user-initiated seek (not our own stepping)
            // Stepping increments by 1, seeks typically jump more or go backwards
            int delta = current_frame - previousFrame_;
            if (delta != 1) {  // Not a forward step - must be user seek
                isOverrunSeek = true;
            }
        }

        previousFrame_ = current_frame;
        lastCacheUpdateFrame_ = current_frame;
        lastCacheUpdateTime_ = timestamp;
    }

    // Cancel all in-flight requests on seek
    if (isSeek) {
        //Debug::Log("DirectEXRCache: [SEEK DETECTED] Canceling all in-flight requests");
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
    cv_.notify_one();
}

void DirectEXRCache::UpdatePlaybackState(bool is_playing) {
    std::lock_guard<std::mutex> lock(mutex_);
    isPlaying_ = is_playing;

    // Reset overrun mode when paused - user interaction resets state
    if (!is_playing) {
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
    double old_speed = playback_speed_factor_.exchange(1.0);
    consecutive_misses_.store(0);
    last_was_sync_load_.store(false);

    if (old_speed < 1.0) {
        Debug::Log("DirectEXRCache: Playback speed reset to 1.0");
    }
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
        //Debug::Log("DirectEXRCache: Failed to get dimensions - " + std::string(e.what()));
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

        //Debug::Log("DirectEXRCache: [TEX-DELETE] Deleted " + std::to_string(toDelete.size()) +
        //           " GL textures (" + std::to_string(remaining_deletes) + " queued)");
    }
}

bool DirectEXRCache::HasPendingTextureDeletions() const {
    std::lock_guard<std::mutex> lock(textureMutex_);
    return !texturesToDelete_.empty();
}

void DirectEXRCache::ClearRequests() {
    size_t pending = 0;
    size_t inProgress = 0;

    {
        std::lock_guard<std::mutex> lock(mutex_);
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

    // Reset rate tracking - old rate data is invalid after seek
    {
        std::lock_guard<std::mutex> rate_lock(rate_mutex_);
        frame_completion_times_.clear();
        measured_fill_rate_.store(0.0);
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

    //Debug::Log("DirectEXRCache: Cleared cache (" + std::to_string(pixel_count) +
    //           " pixel frames) + requests, queued " + std::to_string(textures_to_delete.size()) +
    //           " GL textures for deletion");
}

void DirectEXRCache::SetConfig(const EXRCacheConfig& config) {
    if (!config.IsValid()) {
        //Debug::Log("DirectEXRCache: WARNING - Invalid config");
        return;
    }

    // Check if window size changed
    bool windowChanged = (config.readAheadFrames != config_.readAheadFrames ||
                          config.readBehindSeconds != config_.readBehindSeconds);

    config_ = config;

    // Update cache size as safety cap (frame-based eviction is primary limiter)
    int totalWindowFrames = config_.readAheadFrames + static_cast<int>(config_.readBehindSeconds * fps_) + 50;
    size_t estimatedMaxBytes = static_cast<size_t>(totalWindowFrames) * 64 * 1024 * 1024;
    pixelCache_.SetMaxSize(estimatedMaxBytes);

    if (windowChanged) {
        // Mark segments dirty so UI updates
        segmentsDirty_ = true;
    }

    //Debug::Log("DirectEXRCache: Config updated - threads=" +
    //           std::to_string(config_.threadCount) + ", cache=" +
    //           std::to_string(config_.cacheGB) + "GB, readBehind=" +
    //           std::to_string(config_.readBehindSeconds) + "s");
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

        // NOTE: With adaptive speed control, cache thread keeps running even during reduced playback
        // This allows the cache to fill ahead and recover to full speed faster
        // (Previously paused here in overrun mode, now we keep prefetching)

        iteration++;

        // DEBUG: Log every iteration during initial load
        if (iteration <= 10) {
            //Debug::Log("DirectEXRCache: [CACHE-THREAD] Iteration " + std::to_string(iteration) + " starting");
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
          /*      Debug::Log("DirectEXRCache: [FILL-RESET] Reset fill counters to start from frame " +
                           std::to_string(current_frame));*/
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
                int readBehindFrames = static_cast<int>(config_.readBehindSeconds * fps_);
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
                        pixelCache_.Remove(frame);
                        immediate_evicted++;
                    }
                }

                if (immediate_evicted > 0) {
                    segmentsDirty_ = true;
                    size_t freed_bytes = immediate_evicted * (hasActualFrameSize_ ? actualFrameSize_ : (3840 * 2160 * 4 * sizeof(half)));
                   /* Debug::Log("DirectEXRCache: [SEEK-EVICTION] Immediately evicted " + std::to_string(immediate_evicted) +
                               " stale frames (~" + std::to_string(freed_bytes / (1024*1024)) + "MB freed)");*/
                }
            }
            lastSeekFrame_ = current_frame;

            // Update cacheIterationCount_ AFTER seek detection (so ProcessReadyTextures sees reset value)
            cacheIterationCount_ = iteration;

            // Evict old frames with read-behind + read-ahead window
            // Calculate read-behind/read-ahead in frames
            int readBehindFrames = static_cast<int>(config_.readBehindSeconds * fps_);
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
                    pixelCache_.Remove(frame);
                    evicted_count++;
                }
            }

            if (evicted_count > 0) {
                segmentsDirty_ = true;  // Mark segments dirty after eviction
                /*Debug::Log("DirectEXRCache: Cache thread @ frame " + std::to_string(current_frame) +
                           " - Evicted " + std::to_string(evicted_count) + " pixel data frames outside window [" +
                           std::to_string(eviction_threshold_behind) + ", " + std::to_string(eviction_threshold_ahead) + "]");*/
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

                // Conservative batching - limit how many NEW requests per iteration
                int batch_limit;
                if (iteration == 1) {
                    batch_limit = config_.threadCount * 4;  // Deep initial saturation
                } else {
                    batch_limit = std::min(72, config_.readAheadFrames);  // Rate limit per iteration
                }

                // Use 80% of available space as safety margin
                size_t safe_available = static_cast<size_t>(available * 0.80);
                int max_to_request = std::min(batch_limit, (int)(safe_available / estimated_frame_size));

                // Fill bi-directionally (read-behind + read-ahead)
                int requested_count = 0;

                // Calculate frame ranges for both directions
                int readBehindFrames = static_cast<int>(config_.readBehindSeconds * fps_);

                // Fill read-ahead frames (priority for forward playback)
                // When loop range is active, cache ONLY frames within the loop zone
                if (is_looping_ && has_loop_range_) {
                    // LOOP ZONE MODE: Cache frames within [loop_in_frame_, loop_out_frame_]
                    // Iterate from current position forward, wrapping at Out→In
                    int loop_size = loop_out_frame_ - loop_in_frame_ + 1;

                    // Start from current frame (or loop_in if current is outside zone)
                    int base = current_frame;
                    if (base < loop_in_frame_) base = loop_in_frame_;
                    if (base > loop_out_frame_) base = loop_in_frame_;

                    for (int i = 0; i < loop_size && requested_count < max_to_request; i++) {
                        // Calculate frame with wrap-around within loop zone
                        int frame = loop_in_frame_ + ((base - loop_in_frame_ + i) % loop_size);

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
                    // Only fill if we have remaining capacity (skip in loop zone mode - handled above)
                    for (int i = 1; requested_count < max_to_request && i <= readBehindFrames; i++) {
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
                   /* Debug::Log("DirectEXRCache: [ITER-" + std::to_string(iteration) + "] " +
                               std::to_string(iter_ms) + "ms - Requested " +
                               std::to_string(requested_count) + "/" + std::to_string(batch_limit) +
                               " frames (cached: " + std::to_string(cached_bytes / (1024*1024)) +
                               "MB + in-progress: " + std::to_string(in_progress_bytes / (1024*1024)) +
                               "MB = " + std::to_string(total_committed / (1024*1024)) +
                               "MB / " + std::to_string(max_bytes / (1024*1024)) + "MB)");*/
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
    //Debug::Log("DirectEXRCache: I/O worker thread started");

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

        // Use full thread count for maximum cache fill rate
        // Adaptive speed control handles playback pacing, so we always want fast cache filling
        const size_t max_concurrent = config_.threadCount;

        // Spawn async tasks (up to max_concurrent)
        {
            std::lock_guard<std::mutex> lock(mutex_);

            // Check if sequence has been cleared (Shutdown() was called)
            if (!initialized_ || sequenceFiles_.empty()) {
                videoRequests_.clear();  // Clear stale requests
                continue;
            }

            int spawned = 0;
            while (!videoRequests_.empty() &&
                   requestsInProgress_.size() < max_concurrent) {

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
                request.generation = request_generation_.load();  // Capture generation for stale detection

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

                        // Check if request is stale (user seeked since this was requested)
                        uint64_t current_gen = request_generation_.load();
                        bool is_stale = (it->second.generation != current_gen);

                        if (is_stale) {
                            // Discard stale result - user has seeked since this was requested
                            // Debug::Log("DirectEXRCache: [STALE] Discarding frame " + std::to_string(it->first));
                        } else if (pixelData && !pixelData->pixels.empty()) {
                            // Add directly to pixel cache (no intermediate queue!)
                            size_t byteCount = pixelData->pixels.size();  // Already in bytes (uint8_t vector)
                            pixelCache_.Add(it->first, pixelData, byteCount);

                            // Register with SharedMemoryPool for global LRU coordination
                            RegisterWithPool(it->first, byteCount);

                            // Track completion time for rate measurement
                            {
                                std::lock_guard<std::mutex> rate_lock(rate_mutex_);
                                auto now = std::chrono::steady_clock::now();
                                frame_completion_times_.push_back(now);

                                // Prune old entries outside the measurement window
                                auto cutoff = now - std::chrono::milliseconds(
                                    static_cast<int>(RATE_WINDOW_SECONDS * 1000));
                                while (!frame_completion_times_.empty() &&
                                       frame_completion_times_.front() < cutoff) {
                                    frame_completion_times_.pop_front();
                                }

                                // Update measured fill rate
                                if (frame_completion_times_.size() >= 2) {
                                    auto window_start = frame_completion_times_.front();
                                    auto window_duration = std::chrono::duration<double>(now - window_start).count();
                                    if (window_duration > 0.1) {  // Need at least 100ms of data
                                        measured_fill_rate_.store(frame_completion_times_.size() / window_duration);
                                    }
                                }
                            }

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

    //Detect fast path when windows match
    const bool fastPath = (displayWindow == dataWindow);

    // Use display window for output dimensions
    int width = displayWindow.max.x - displayWindow.min.x + 1;
    int height = displayWindow.max.y - displayWindow.min.y + 1;

    // Setup frame buffer
    const Imf::ChannelList& channels = header.channels();

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
            //Debug::Log("DirectEXRCache: [FAST-PATH-READ] readPixels took " + std::to_string(read_ms) + "ms");
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

} // namespace ump
