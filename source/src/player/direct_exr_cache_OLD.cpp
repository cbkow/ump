#include "direct_exr_cache.h"
#include "../utils/debug_utils.h"
#include "../gpu/texture_pool.h"

// Fix Windows min/max macro conflicts with OpenEXR
#ifdef _WIN32
#undef min
#undef max
#endif

#include <ImfInputFile.h>
#include <ImfChannelList.h>
#include <ImfFrameBuffer.h>
#include <ImfInputPart.h>
#include <ImfMultiPartInputFile.h>
#include <ImfRgbaFile.h>
#include <ImfThreading.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <thread>
#include <unordered_set>

namespace ump {

//=============================================================================
// DirectEXRFrame Implementation
//=============================================================================

DirectEXRFrame::~DirectEXRFrame() {
    // Note: GPU texture pooling will handle cleanup
    // Non-pooled textures should be manually deleted before frame destruction
    if (texture_id != 0 && !is_pooled_texture) {
        glDeleteTextures(1, &texture_id);
        texture_id = 0;
    }
}

//=============================================================================
// Memory-Mapped IStream (tlRender-style)
//=============================================================================

namespace {
    // True memory-mapped file stream for optimal EXR loading performance
    // Uses Windows memory-mapped files for zero-copy I/O
    class MemoryMappedIStream : public Imf::IStream {
    public:
        MemoryMappedIStream(const std::string& fileName)
            : Imf::IStream(fileName.c_str())
            , file_path_(fileName)
        {
#ifdef _WIN32
            // Windows: Use CreateFile + CreateFileMapping + MapViewOfFile
            // Convert to wide string for CreateFileW
            int wlen = MultiByteToWideChar(CP_UTF8, 0, fileName.c_str(), -1, nullptr, 0);
            std::vector<wchar_t> wpath(wlen);
            MultiByteToWideChar(CP_UTF8, 0, fileName.c_str(), -1, wpath.data(), wlen);

            file_handle_ = CreateFileW(
                wpath.data(),
                GENERIC_READ,
                FILE_SHARE_READ,
                nullptr,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL,
                nullptr
            );

            if (file_handle_ == INVALID_HANDLE_VALUE) {
                throw std::runtime_error("Cannot open file: " + fileName);
            }

            // Get file size
            LARGE_INTEGER size;
            if (!GetFileSizeEx(file_handle_, &size)) {
                CloseHandle(file_handle_);
                throw std::runtime_error("Cannot get file size: " + fileName);
            }
            file_size_ = static_cast<uint64_t>(size.QuadPart);

            // Create file mapping
            mapping_handle_ = CreateFileMappingW(
                file_handle_,
                nullptr,
                PAGE_READONLY,
                0, 0,  // Map entire file
                nullptr
            );

            if (!mapping_handle_) {
                CloseHandle(file_handle_);
                throw std::runtime_error("Cannot create file mapping: " + fileName);
            }

            // Map view of file
            data_ptr_ = static_cast<const uint8_t*>(MapViewOfFile(
                mapping_handle_,
                FILE_MAP_READ,
                0, 0,  // Map from beginning
                0      // Map entire file
            ));

            if (!data_ptr_) {
                CloseHandle(mapping_handle_);
                CloseHandle(file_handle_);
                throw std::runtime_error("Cannot map view of file: " + fileName);
            }
#else
            // Linux/Mac: Use open + mmap
            int fd = open(fileName.c_str(), O_RDONLY);
            if (fd == -1) {
                throw std::runtime_error("Cannot open file: " + fileName);
            }

            struct stat sb;
            if (fstat(fd, &sb) == -1) {
                close(fd);
                throw std::runtime_error("Cannot stat file: " + fileName);
            }
            file_size_ = sb.st_size;

            data_ptr_ = static_cast<const uint8_t*>(mmap(
                nullptr,
                file_size_,
                PROT_READ,
                MAP_PRIVATE,
                fd,
                0
            ));

            // Can close fd after mmap - mapping persists
            close(fd);

            if (data_ptr_ == MAP_FAILED) {
                throw std::runtime_error("Cannot mmap file: " + fileName);
            }
#endif
        }

        virtual ~MemoryMappedIStream() {
#ifdef _WIN32
            if (data_ptr_) {
                UnmapViewOfFile(data_ptr_);
            }
            if (mapping_handle_) {
                CloseHandle(mapping_handle_);
            }
            if (file_handle_ != INVALID_HANDLE_VALUE) {
                CloseHandle(file_handle_);
            }
#else
            if (data_ptr_ && data_ptr_ != MAP_FAILED) {
                munmap(const_cast<uint8_t*>(data_ptr_), file_size_);
            }
#endif
        }

        virtual bool isMemoryMapped() const override {
            return true;
        }

        virtual char* readMemoryMapped(int n) override {
            if (pos_ + n > file_size_) {
                throw std::runtime_error("Read past end of file");
            }
            char* result = reinterpret_cast<char*>(const_cast<uint8_t*>(data_ptr_ + pos_));
            pos_ += n;
            return result;
        }

        virtual bool read(char c[], int n) override {
            if (pos_ + n > file_size_) {
                throw std::runtime_error("Read past end of file");
            }
            std::memcpy(c, data_ptr_ + pos_, n);
            pos_ += n;
            return pos_ < file_size_;
        }

        virtual uint64_t tellg() override {
            return pos_;
        }

        virtual void seekg(uint64_t pos) override {
            if (pos > file_size_) {
                throw std::runtime_error("Seek past end of file");
            }
            pos_ = pos;
        }

    private:
        std::string file_path_;
        const uint8_t* data_ptr_ = nullptr;
        uint64_t file_size_ = 0;
        uint64_t pos_ = 0;

#ifdef _WIN32
        HANDLE file_handle_ = INVALID_HANDLE_VALUE;
        HANDLE mapping_handle_ = nullptr;
#endif
    };
}

//=============================================================================
// DirectEXRCache Implementation
//=============================================================================

DirectEXRCache::DirectEXRCache() {
    Debug::Log("DirectEXRCache: Constructor");
    InitializeTexturePool();
}

DirectEXRCache::~DirectEXRCache() {
    Debug::Log("DirectEXRCache: Destructor");
    Shutdown();
}

bool DirectEXRCache::Initialize(const std::vector<std::string>& sequence_files,
                                const std::string& layer_name, double fps) {
    Debug::Log("DirectEXRCache: Initialize - " + std::to_string(sequence_files.size()) +
               " files, layer: " + layer_name + ", fps: " + std::to_string(fps));

    if (sequence_files.empty()) {
        Debug::Log("DirectEXRCache: ERROR - Empty sequence files");
        return false;
    }

    // Configure OpenEXR global threading for DWAB/ZIP decompression
    // Use hardware concurrency for optimal multi-threaded decompression
    int num_threads = std::thread::hardware_concurrency();
    if (num_threads == 0) num_threads = 8; // Fallback
    Imf::setGlobalThreadCount(num_threads);
    Debug::Log("DirectEXRCache: Set OpenEXR decompression threads to " + std::to_string(num_threads));

    sequence_files_ = sequence_files;
    selected_layer_ = layer_name;
    fps_ = fps;

    // Set initial cache size
    frame_cache_.SetMaxSize(static_cast<size_t>(config_.video_cache_gb * 1024 * 1024 * 1024));

    // Initialize state
    {
        std::lock_guard<std::mutex> lock(mutex_.mutex);
        mutex_.state.current_time = 0.0;
        mutex_.state.is_playing = false;
        mutex_.state.direction = CacheDirection::Forward;
    }

    cache_hits_ = 0;
    cache_misses_ = 0;

    is_initialized_ = true;
    Debug::Log("DirectEXRCache: Initialization complete");
    return true;
}

void DirectEXRCache::Shutdown() {
    if (!is_initialized_) return;

    Debug::Log("DirectEXRCache: Shutdown starting...");

    // Stop background thread
    StopBackgroundCaching();

    // Clear cache
    ClearCache();

    // Shutdown GPU resources
    if (texture_pool_) {
        texture_pool_.reset();
        Debug::Log("DirectEXRCache: GPU texture pool released");
    }

    is_initialized_ = false;
    sequence_files_.clear();
    selected_layer_.clear();

    Debug::Log("DirectEXRCache: Shutdown complete");
}

bool DirectEXRCache::GetFrame(int frame_index, GLuint& texture_id, int& width, int& height) {
    if (!is_initialized_ || frame_index < 0 || frame_index >= static_cast<int>(sequence_files_.size())) {
        cache_misses_++;
        return false;
    }

    // Try to get from cache
    std::shared_ptr<DirectEXRFrame> frame;
    if (frame_cache_.Get(frame_index, frame)) {
        if (frame && frame->texture_id != 0) {
            texture_id = frame->texture_id;
            width = frame->width;
            height = frame->height;
            cache_hits_++;
            return true;
        }
    }

    // Cache miss
    cache_misses_++;
    return false;
}

bool DirectEXRCache::GetFrameOrLoad(int frame_index, GLuint& texture_id, int& width, int& height) {
    // Try cache first
    if (GetFrame(frame_index, texture_id, width, height)) {
        static int last_logged_hit = -1;
        if (frame_index != last_logged_hit) {
            Debug::Log("DirectEXRCache: Cache HIT for frame " + std::to_string(frame_index));
            last_logged_hit = frame_index;
        }
        return true;
    }

    // Cache miss - load synchronously for immediate display
    if (!is_initialized_ || frame_index < 0 || frame_index >= static_cast<int>(sequence_files_.size())) {
        return false;
    }

    Debug::Log("DirectEXRCache: Cache MISS - loading frame " + std::to_string(frame_index) + " from disk");

    const std::string& file_path = sequence_files_[frame_index];
    auto frame = LoadFrameDirect(file_path, selected_layer_);

    if (!frame || frame->texture_id == 0) {
        Debug::Log("DirectEXRCache: FAILED to load frame " + std::to_string(frame_index));
        return false;
    }

    // Add to cache for future use
    size_t byte_count = CalculateFrameByteCount(frame->width, frame->height);
    frame_cache_.Add(frame_index, frame, byte_count);

    Debug::Log("DirectEXRCache: Frame " + std::to_string(frame_index) + " loaded and cached (" +
               std::to_string(byte_count / (1024*1024)) + " MB, cache now " +
               std::to_string(frame_cache_.GetSize() / (1024*1024)) + " MB)");

    // Return the loaded frame
    texture_id = frame->texture_id;
    width = frame->width;
    height = frame->height;

    cache_hits_++;  // Count as hit since we successfully provided the frame
    return true;
}

void DirectEXRCache::UpdateCurrentPosition(double timestamp) {
    double old_time = 0.0;
    CacheDirection old_direction = CacheDirection::Forward;

    {
        std::lock_guard<std::mutex> lock(mutex_.mutex);
        old_time = mutex_.state.current_time;
        old_direction = mutex_.state.direction;
        mutex_.state.current_time = timestamp;

        // Update direction based on movement
        if (timestamp > old_time) {
            mutex_.state.direction = CacheDirection::Forward;
        } else if (timestamp < old_time) {
            mutex_.state.direction = CacheDirection::Reverse;
        }
    }

    // Detect seek (non-gradual movement > 5 frames)
    double position_delta = std::abs(timestamp - old_time);
    bool is_seek = position_delta > (5.0 / fps_);

    if (is_seek) {
        // Clear requests immediately on seek
        std::lock_guard<std::mutex> lock(mutex_.mutex);
        mutex_.clear_requests = true;

        // Wake thread instantly
        thread_.cv.notify_one();

        Debug::Log("DirectEXRCache: Seek detected (" + std::to_string(position_delta) +
                   "s), cleared requests");
    }
}

void DirectEXRCache::UpdatePlaybackState(bool is_playing) {
    std::lock_guard<std::mutex> lock(mutex_.mutex);
    mutex_.state.is_playing = is_playing;
}

void DirectEXRCache::SetConfig(const DirectEXRCacheConfig& config) {
    if (!config.IsValid()) {
        Debug::Log("DirectEXRCache: WARNING - Invalid config");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(config_mutex_);
        config_ = config;
    }

    // Update cache size
    frame_cache_.SetMaxSize(static_cast<size_t>(config.video_cache_gb * 1024 * 1024 * 1024));

    Debug::Log("DirectEXRCache: Config updated - " +
               std::to_string(config.video_cache_gb) + "GB cache, " +
               std::to_string(config.read_behind_seconds) + "s read behind");
}

DirectEXRCacheConfig DirectEXRCache::GetConfig() const {
    std::lock_guard<std::mutex> lock(config_mutex_);
    return config_;
}

void DirectEXRCache::StartBackgroundCaching() {
    if (thread_.running) return;

    Debug::Log("DirectEXRCache: Starting background caching thread");

    // Initialize state
    {
        std::lock_guard<std::mutex> lock(mutex_.mutex);
        mutex_.state.current_time = 0.0;
        mutex_.state.is_playing = false;
        mutex_.state.direction = CacheDirection::Forward;
    }

    // Initialize thread state - CRITICAL: Set fill frame to cache start
    thread_.video_fill_frame = 0;  // Will be reset to GetCacheStartFrame() on first iteration
    thread_.video_fill_byte_count = 0;

    thread_.running = true;
    thread_.thread = std::thread(&DirectEXRCache::CacheThreadMain, this);

    // Immediately wake the thread to start caching
    thread_.cv.notify_one();

    Debug::Log("DirectEXRCache: Background caching thread started and notified");
}

void DirectEXRCache::StopBackgroundCaching() {
    if (!thread_.running) return;

    Debug::Log("DirectEXRCache: Stopping background caching thread");

    thread_.running = false;
    thread_.cv.notify_one();  // Wake thread to exit

    if (thread_.thread.joinable()) {
        thread_.thread.join();
    }

    Debug::Log("DirectEXRCache: Background caching thread stopped");
}

void DirectEXRCache::SetCachingEnabled(bool enabled) {
    caching_enabled_ = enabled;

    if (enabled) {
        if (!thread_.running) {
            StartBackgroundCaching();
        }
    } else {
        if (thread_.running) {
            StopBackgroundCaching();
        }
    }
}

void DirectEXRCache::ClearCache() {
    Debug::Log("DirectEXRCache: Clearing cache");

    // Release pooled textures before clearing
    auto keys = frame_cache_.GetKeys();
    for (int frame_idx : keys) {
        std::shared_ptr<DirectEXRFrame> frame;
        if (frame_cache_.Get(frame_idx, frame)) {
            if (frame && frame->is_pooled_texture && frame->texture_id != 0 && texture_pool_) {
                texture_pool_->ReleaseTexture(frame->texture_id);
                frame->texture_id = 0;
            }
        }
    }

    frame_cache_.Clear();
    Debug::Log("DirectEXRCache: Cache cleared");
}

DirectEXRCache::CacheStats DirectEXRCache::GetStats() const {
    CacheStats stats;

    stats.total_frames_in_sequence = static_cast<int>(sequence_files_.size());
    stats.frames_cached = static_cast<int>(frame_cache_.GetKeys().size());
    stats.memory_usage_mb = frame_cache_.GetSize() / (1024 * 1024);
    stats.background_thread_active = thread_.running;

    stats.cache_hits = cache_hits_;
    stats.cache_misses = cache_misses_;
    int total = stats.cache_hits + stats.cache_misses;
    stats.hit_ratio = (total > 0) ? static_cast<double>(stats.cache_hits) / total : 0.0;

    if (frame_cache_.GetMaxSize() > 0) {
        stats.cache_percentage = (static_cast<double>(frame_cache_.GetSize()) /
                                 frame_cache_.GetMaxSize()) * 100.0;
    }

    // Performance stats
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        if (!load_times_.empty()) {
            double sum = 0.0;
            for (double t : load_times_) sum += t;
            stats.average_load_time_ms = sum / load_times_.size();
        }
        stats.total_frames_loaded = total_frames_loaded_;
    }

    return stats;
}

std::vector<CacheSegment> DirectEXRCache::GetCacheSegments() const {
    std::vector<CacheSegment> segments;

    if (!is_initialized_ || sequence_files_.empty()) {
        return segments;
    }

    // Get all cached frame indices
    auto keys = frame_cache_.GetKeys();
    if (keys.empty()) {
        return segments;
    }

    // Sort keys
    std::sort(keys.begin(), keys.end());

    // Build continuous segments
    int segment_start = keys[0];
    int segment_end = keys[0];

    for (size_t i = 1; i < keys.size(); ++i) {
        if (keys[i] == segment_end + 1) {
            segment_end = keys[i];
        } else {
            // Finalize current segment
            CacheSegment seg;
            seg.start_time = segment_start / fps_;
            seg.end_time = (segment_end + 1) / fps_;
            seg.type = CacheSegment::SCRUB_CACHE;
            seg.density = 1.0f;
            segments.push_back(seg);

            segment_start = keys[i];
            segment_end = keys[i];
        }
    }

    // Add final segment
    CacheSegment seg;
    seg.start_time = segment_start / fps_;
    seg.end_time = (segment_end + 1) / fps_;
    seg.type = CacheSegment::SCRUB_CACHE;
    seg.density = 1.0f;
    segments.push_back(seg);

    return segments;
}

//=============================================================================
// Private Methods
//=============================================================================

void DirectEXRCache::CacheThreadMain() {
    Debug::Log("DirectEXRCache: Cache thread started (tlRender architecture)");

    thread_.log_timer = std::chrono::steady_clock::now();
    const std::chrono::milliseconds timeout(5);  // tlRender uses 5ms

    while (thread_.running) {
        // Phase 1: Wait on condition variable (instant wake, no busy loop!)
        {
            std::unique_lock<std::mutex> lock(mutex_.mutex);
            thread_.cv.wait_for(lock, timeout, [this] {
                return !thread_.pending_frame_requests.empty() ||
                       !thread_.in_progress_requests.empty() ||
                       !thread_.running;
            });
        }

        if (!thread_.running) break;

        // Phase 2: Get mutex-protected state
        PlaybackState state;
        bool clear_requests = false;
        bool clear_cache = false;
        {
            std::lock_guard<std::mutex> lock(mutex_.mutex);
            state = mutex_.state;
            clear_requests = mutex_.clear_requests;
            mutex_.clear_requests = false;
            clear_cache = mutex_.clear_cache;
            mutex_.clear_cache = false;
        }

        // Phase 3: Handle state changes (INSTANT response)
        if (state != thread_.state) {
            thread_.state = state;
            thread_.video_fill_frame = GetCacheStartFrame();  // Reset to window start
            thread_.video_fill_byte_count = 0;
            clear_requests = true;  // Cancel outdated requests
        }

        // Phase 4: Cancel requests if needed
        if (clear_requests) {
            CancelPendingRequests();
        }

        // Phase 5: Clear cache if needed
        if (clear_cache) {
            ClearCache();
        }

        // Phase 6: Update cache (tlRender's cacheUpdate)
        if (caching_enabled_) {
            CacheUpdate();
        }

        // Phase 7: Logging (every 10 seconds like tlRender)
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - thread_.log_timer).count() >= 10) {
            thread_.log_timer = now;
            Debug::Log("DirectEXRCache: Pending=" + std::to_string(thread_.pending_frame_requests.size()) +
                       ", InProgress=" + std::to_string(thread_.in_progress_requests.size()) +
                       ", Cached=" + std::to_string(frame_cache_.GetKeys().size()) + " frames");
        }
    }

    Debug::Log("DirectEXRCache: Cache thread stopped");
}

// NEW: tlRender-style batch parallel cache update
void DirectEXRCache::CacheUpdate() {
    // =======================================================================
    // Phase 1: Fill Pending Request Queue (up to max_pending_requests)
    // =======================================================================

    const int start_frame = GetCacheStartFrame();
    const int end_frame = GetCacheEndFrame();
    const size_t cache_max_bytes = frame_cache_.GetMaxSize();

    // OPTIMIZED: Build set of requested frames for O(1) lookup instead of O(n) loops
    std::unordered_set<int> requested_frames;
    for (const auto& req : thread_.pending_frame_requests) {
        requested_frames.insert(req);
    }
    for (const auto& req : thread_.in_progress_requests) {
        requested_frames.insert(req.frame_index);
    }

    // Fill queue with frames that need loading
    while (thread_.video_fill_frame <= end_frame &&
           thread_.pending_frame_requests.size() < config_.max_pending_requests &&
           thread_.video_fill_byte_count < cache_max_bytes) {

        int frame_index = thread_.video_fill_frame;

        // Skip if already cached
        if (frame_cache_.Contains(frame_index)) {
            thread_.video_fill_frame++;
            continue;
        }

        // FAST: O(1) lookup instead of O(n) loop
        if (requested_frames.find(frame_index) == requested_frames.end()) {
            thread_.pending_frame_requests.push_back(frame_index);
            requested_frames.insert(frame_index);  // Update set

            // Estimate byte count
            if (!dimensions_cached_ && !sequence_files_.empty()) {
                GetFrameDimensions(sequence_files_[0], cached_width_, cached_height_);
                dimensions_cached_ = true;
            }
            size_t byte_count = CalculateFrameByteCount(
                dimensions_cached_ ? cached_width_ : 3840,
                dimensions_cached_ ? cached_height_ : 2160
            );
            thread_.video_fill_byte_count += byte_count;
        }

        thread_.video_fill_frame++;
    }

    // =======================================================================
    // Phase 2: Spawn Async Tasks (Batch Parallel - up to thread_count)
    // =======================================================================

    while (!thread_.pending_frame_requests.empty() &&
           thread_.in_progress_requests.size() < config_.thread_count) {

        int frame_index = thread_.pending_frame_requests.front();
        thread_.pending_frame_requests.pop_front();

        // Create request
        PixelLoadRequest request;
        request.frame_index = frame_index;
        request.id = next_request_id_++;
        request.byte_count = CalculateFrameByteCount(
            dimensions_cached_ ? cached_width_ : 3840,
            dimensions_cached_ ? cached_height_ : 2160
        );

        // Spawn async task (tlRender pattern)
        const std::string file_path = sequence_files_[frame_index];
        const std::string layer = selected_layer_;

        request.future = std::async(std::launch::async, [this, file_path, layer]() {
            try {
                return LoadPixelsFromEXR(file_path, layer);
            } catch (const std::exception& e) {
                Debug::Log("DirectEXRCache: Exception loading frame: " + std::string(e.what()));
                return std::shared_ptr<DirectEXRPixelData>(nullptr);
            }
        });

        thread_.in_progress_requests.push_back(std::move(request));
    }

    // =======================================================================
    // Phase 3: Check Completed Requests (Non-Blocking Poll)
    // =======================================================================

    auto it = thread_.in_progress_requests.begin();
    while (it != thread_.in_progress_requests.end()) {
        if (it->future.valid() &&
            it->future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {

            try {
                auto pixel_data = it->future.get();

                if (pixel_data && !pixel_data->pixels.empty()) {
                    // Move to texture creation queue (main thread will process)
                    std::lock_guard<std::mutex> lock(texture_queue_mutex_);

                    PendingPixelData pending;
                    pending.frame_index = it->frame_index;
                    pending.pixel_data = pixel_data;
                    ready_for_texture_creation_.push_back(pending);
                }
            } catch (const std::exception& e) {
                Debug::Log("DirectEXRCache: Exception processing completed request: " +
                           std::string(e.what()));
            }

            // Remove from in-progress
            it = thread_.in_progress_requests.erase(it);
        } else {
            ++it;
        }
    }
}

// NEW: Request cancellation on seek
void DirectEXRCache::CancelPendingRequests() {
    Debug::Log("DirectEXRCache: Cancelling pending requests");

    // Clear pending queue
    size_t pending_count = thread_.pending_frame_requests.size();
    thread_.pending_frame_requests.clear();

    // Clear in-progress (futures will complete but we won't process them)
    size_t in_progress_count = thread_.in_progress_requests.size();
    thread_.in_progress_requests.clear();

    // Clear texture creation queue
    {
        std::lock_guard<std::mutex> lock(texture_queue_mutex_);
        ready_for_texture_creation_.clear();
    }

    Debug::Log("DirectEXRCache: Cancelled " + std::to_string(pending_count) +
               " pending + " + std::to_string(in_progress_count) + " in-progress requests");
}

// NEW: Dynamic cache window calculation (tlRender pattern)
int DirectEXRCache::GetCacheFrameCount() const {
    // Calculate how many frames fit in cache
    if (!dimensions_cached_) {
        return 100;  // Default estimate
    }

    size_t bytes_per_frame = CalculateFrameByteCount(cached_width_, cached_height_);
    if (bytes_per_frame == 0) return 100;

    size_t cache_bytes = static_cast<size_t>(config_.video_cache_gb * 1024 * 1024 * 1024);
    return static_cast<int>(cache_bytes / bytes_per_frame);
}

int DirectEXRCache::GetCacheStartFrame() const {
    int current_frame;
    CacheDirection direction;
    {
        // Note: const_cast required for const method accessing mutable mutex
        std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(mutex_.mutex));
        current_frame = static_cast<int>(mutex_.state.current_time * fps_);
        direction = mutex_.state.direction;
    }

    int read_behind_frames = static_cast<int>(config_.read_behind_seconds * fps_);

    if (direction == CacheDirection::Forward) {
        // Forward: cache from (current - read_behind) to (current + cache_size)
        return std::max(0, current_frame - read_behind_frames);
    } else {
        // Reverse: cache from (current - cache_size) to (current + read_behind)
        int cache_frames = GetCacheFrameCount();
        return std::max(0, current_frame - cache_frames + read_behind_frames);
    }
}

int DirectEXRCache::GetCacheEndFrame() const {
    int start = GetCacheStartFrame();
    int cache_frames = GetCacheFrameCount();
    int end = start + cache_frames;
    return std::min(end, static_cast<int>(sequence_files_.size()) - 1);
}

size_t DirectEXRCache::CalculateFrameByteCount(int width, int height) const {
    // RGBA16F = 4 channels * 2 bytes = 8 bytes per pixel
    return static_cast<size_t>(width) * height * 8;
}

void DirectEXRCache::ProcessReadyTextures() {
    // MUST be called from main thread with active GL context

    std::vector<PendingPixelData> to_process;
    {
        std::lock_guard<std::mutex> lock(texture_queue_mutex_);
        if (ready_for_texture_creation_.empty()) {
            return;
        }
        // Move to local vector to minimize lock time
        to_process = std::move(ready_for_texture_creation_);
        ready_for_texture_creation_.clear();
    }

    // Process each ready pixel data (create GL textures)
    for (const auto& pending : to_process) {
        auto frame = CreateFrameFromPixels(pending.pixel_data);

        if (frame && frame->texture_id != 0) {
            // Add to cache
            size_t byte_count = CalculateFrameByteCount(frame->width, frame->height);
            frame_cache_.Add(pending.frame_index, frame, byte_count);

            Debug::Log("DirectEXRCache: Frame " + std::to_string(pending.frame_index) +
                       " texture created (" + std::to_string(byte_count / (1024*1024)) + " MB)");
        }
    }
}

std::shared_ptr<DirectEXRPixelData> DirectEXRCache::LoadPixelsFromEXR(
    const std::string& file_path, const std::string& layer_name) {

    // Phase 1: Load pixels from EXR (NO GL calls - safe for background threads)
    try {
        // Open EXR file with memory-mapped stream
        auto stream = std::make_unique<MemoryMappedIStream>(file_path);
        Imf::MultiPartInputFile file(*stream);

        // Get header and dimensions
        const Imf::Header& header = file.header(0);
        Imath::Box2i dw = header.dataWindow();
        int width = dw.max.x - dw.min.x + 1;
        int height = dw.max.y - dw.min.y + 1;

        // Allocate pixel buffer (RGBA half-float)
        std::vector<half> pixels(width * height * 4);

        // Find actual channel names using pattern matching
        const Imf::ChannelList& channels = header.channels();
        std::string r_channel = FindChannelName(channels, layer_name, "R");
        std::string g_channel = FindChannelName(channels, layer_name, "G");
        std::string b_channel = FindChannelName(channels, layer_name, "B");
        std::string a_channel = FindChannelName(channels, layer_name, "A");

        if (r_channel.empty() || g_channel.empty() || b_channel.empty()) {
            Debug::Log("DirectEXRCache: Could not find RGB channels for layer '" + layer_name + "' in " + file_path);
            return nullptr;
        }

        // Alpha channel is optional
        if (a_channel.empty()) {
            a_channel = r_channel;  // Will fill with 1.0 later if needed
        }

        // Setup frame buffer for direct reading
        Imf::InputPart part(file, 0);
        Imf::FrameBuffer frameBuffer;

        const size_t pixel_stride = sizeof(half) * 4;
        const size_t row_stride = pixel_stride * width;

        frameBuffer.insert(r_channel.c_str(), Imf::Slice(Imf::HALF,
            reinterpret_cast<char*>(&pixels[0]), pixel_stride, row_stride));
        frameBuffer.insert(g_channel.c_str(), Imf::Slice(Imf::HALF,
            reinterpret_cast<char*>(&pixels[1]), pixel_stride, row_stride));
        frameBuffer.insert(b_channel.c_str(), Imf::Slice(Imf::HALF,
            reinterpret_cast<char*>(&pixels[2]), pixel_stride, row_stride));
        frameBuffer.insert(a_channel.c_str(), Imf::Slice(Imf::HALF,
            reinterpret_cast<char*>(&pixels[3]), pixel_stride, row_stride));

        part.setFrameBuffer(frameBuffer);
        part.readPixels(dw.min.y, dw.max.y);

        // Create pixel data object
        auto pixel_data = std::make_shared<DirectEXRPixelData>();
        pixel_data->pixels = std::move(pixels);
        pixel_data->width = width;
        pixel_data->height = height;
        pixel_data->file_path = file_path;
        pixel_data->layer_name = layer_name;

        return pixel_data;

    } catch (const std::exception& e) {
        Debug::Log("DirectEXRCache: Exception loading pixels from " + file_path + ": " + e.what());
        return nullptr;
    }
}

std::shared_ptr<DirectEXRFrame> DirectEXRCache::CreateFrameFromPixels(
    std::shared_ptr<DirectEXRPixelData> pixel_data) {

    // Phase 2: Create GL texture from pixels (MUST be on main thread with GL context)
    if (!pixel_data || pixel_data->pixels.empty()) {
        return nullptr;
    }

    // Create GPU texture
    GLuint texture_id = CreateTextureFromPixels(pixel_data->pixels, pixel_data->width, pixel_data->height);

    if (texture_id == 0) {
        Debug::Log("DirectEXRCache: Failed to create texture from pixels for " + pixel_data->file_path);
        return nullptr;
    }

    // Create frame object
    auto frame = std::make_shared<DirectEXRFrame>();
    frame->texture_id = texture_id;
    frame->width = pixel_data->width;
    frame->height = pixel_data->height;
    frame->byte_count = CalculateFrameByteCount(pixel_data->width, pixel_data->height);
    frame->is_pooled_texture = false; // TODO: Integrate texture pool
    frame->file_path = pixel_data->file_path;
    frame->layer_name = pixel_data->layer_name;
    frame->last_accessed = std::chrono::steady_clock::now();

    return frame;
}

std::shared_ptr<DirectEXRFrame> DirectEXRCache::LoadFrameDirect(const std::string& file_path,
                                                                 const std::string& layer_name) {
    auto start_time = std::chrono::steady_clock::now();

    try {
        // Open EXR file with memory-mapped stream
        auto stream = std::make_unique<MemoryMappedIStream>(file_path);
        Imf::MultiPartInputFile file(*stream);

        // Get header and dimensions
        const Imf::Header& header = file.header(0);
        Imath::Box2i dw = header.dataWindow();
        int width = dw.max.x - dw.min.x + 1;
        int height = dw.max.y - dw.min.y + 1;

        // Allocate pixel buffer (RGBA half-float)
        std::vector<half> pixels(width * height * 4);

        // Find actual channel names using pattern matching
        const Imf::ChannelList& channels = header.channels();
        std::string r_channel = FindChannelName(channels, layer_name, "R");
        std::string g_channel = FindChannelName(channels, layer_name, "G");
        std::string b_channel = FindChannelName(channels, layer_name, "B");
        std::string a_channel = FindChannelName(channels, layer_name, "A");

        if (r_channel.empty() || g_channel.empty() || b_channel.empty()) {
            Debug::Log("DirectEXRCache: Could not find RGB channels for layer '" + layer_name + "' in " + file_path);
            return nullptr;
        }

        // Alpha channel is optional
        if (a_channel.empty()) {
            Debug::Log("DirectEXRCache: No alpha channel found for layer '" + layer_name + "', using opaque");
            a_channel = r_channel;  // Reuse R channel memory, will set alpha to 1.0 later
        }

        Debug::Log("DirectEXRCache: Found channels - R:'" + r_channel + "' G:'" + g_channel +
                  "' B:'" + b_channel + "' A:'" + a_channel + "'");

        // Setup frame buffer for direct reading
        Imf::InputPart part(file, 0);
        Imf::FrameBuffer frameBuffer;

        const size_t pixel_stride = sizeof(half) * 4;
        const size_t row_stride = pixel_stride * width;

        frameBuffer.insert(r_channel.c_str(), Imf::Slice(Imf::HALF,
            reinterpret_cast<char*>(&pixels[0]), pixel_stride, row_stride));
        frameBuffer.insert(g_channel.c_str(), Imf::Slice(Imf::HALF,
            reinterpret_cast<char*>(&pixels[1]), pixel_stride, row_stride));
        frameBuffer.insert(b_channel.c_str(), Imf::Slice(Imf::HALF,
            reinterpret_cast<char*>(&pixels[2]), pixel_stride, row_stride));
        frameBuffer.insert(a_channel.c_str(), Imf::Slice(Imf::HALF,
            reinterpret_cast<char*>(&pixels[3]), pixel_stride, row_stride));

        part.setFrameBuffer(frameBuffer);
        part.readPixels(dw.min.y, dw.max.y);

        // Create GPU texture
        GLuint texture_id = CreateTextureFromPixels(pixels, width, height);

        if (texture_id == 0) {
            Debug::Log("DirectEXRCache: Failed to create texture for " + file_path);
            return nullptr;
        }

        // Create frame object
        auto frame = std::make_shared<DirectEXRFrame>();
        frame->texture_id = texture_id;
        frame->width = width;
        frame->height = height;
        frame->byte_count = CalculateFrameByteCount(width, height);
        frame->is_pooled_texture = false; // TODO: Integrate texture pool
        frame->file_path = file_path;
        frame->layer_name = layer_name;
        frame->last_accessed = std::chrono::steady_clock::now();

        // Record timing
        auto end_time = std::chrono::steady_clock::now();
        double load_time = std::chrono::duration<double, std::milli>(end_time - start_time).count();
        {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            load_times_.push_back(load_time);
            if (load_times_.size() > 100) {
                load_times_.erase(load_times_.begin());
            }
            total_frames_loaded_++;
        }

        return frame;

    } catch (const std::exception& e) {
        Debug::Log("DirectEXRCache: Exception loading " + file_path + ": " + e.what());
        return nullptr;
    }
}

GLuint DirectEXRCache::CreateTextureFromPixels(const std::vector<half>& pixels, int width, int height) {
    GLuint texture_id = 0;

    // Clear any existing errors
    while (glGetError() != GL_NO_ERROR);

    glGenTextures(1, &texture_id);
    GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
        Debug::Log("DirectEXRCache: glGenTextures error: " + std::to_string(error));
        return 0;
    }
    if (texture_id == 0) {
        Debug::Log("DirectEXRCache: glGenTextures returned 0");
        return 0;
    }

    glBindTexture(GL_TEXTURE_2D, texture_id);
    error = glGetError();
    if (error != GL_NO_ERROR) {
        Debug::Log("DirectEXRCache: glBindTexture error: " + std::to_string(error));
        glDeleteTextures(1, &texture_id);
        return 0;
    }

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0,
                 GL_RGBA, GL_HALF_FLOAT, pixels.data());

    error = glGetError();
    if (error != GL_NO_ERROR) {
        Debug::Log("DirectEXRCache: glTexImage2D error: " + std::to_string(error) +
                   " (GL_INVALID_ENUM=1280, GL_INVALID_VALUE=1281, GL_INVALID_OPERATION=1282)");
        glDeleteTextures(1, &texture_id);
        return 0;
    }

    glBindTexture(GL_TEXTURE_2D, 0);

    return texture_id;
}

void DirectEXRCache::InitializeTexturePool() {
    TexturePoolConfig pool_config;
    pool_config.max_memory_mb = config_.gpu_memory_pool_mb;
    pool_config.max_textures = config_.gpu_max_textures;
    pool_config.texture_ttl_seconds = config_.gpu_texture_ttl_seconds;
    pool_config.enable_memory_monitoring = true;

    texture_pool_ = std::make_unique<GPUTexturePool>(pool_config);
    texture_pool_->StartBackgroundEviction();

    Debug::Log("DirectEXRCache: GPU texture pool initialized");
}

//=============================================================================
// Channel Name Resolution (Blender multi-layer support)
//=============================================================================

std::vector<std::string> DirectEXRCache::GenerateChannelPatterns(
    const std::string& layer_name,
    const std::string& component) const {

    std::vector<std::string> patterns;

    if (layer_name.empty() || layer_name == "RGBA") {
        // Default layer patterns
        patterns.push_back(component);
    } else {
        // Layer-specific patterns with different separators and formats
        patterns.push_back(layer_name + "." + component);       // "Combined.R"
        patterns.push_back(layer_name + "_" + component);       // "Combined_R"
        patterns.push_back(layer_name + component);             // "CombinedR"
        patterns.push_back(layer_name + ":" + component);       // "Combined:R"
        patterns.push_back(layer_name + "/" + component);       // "Combined/R"

        // Blender ViewLayer patterns - hierarchical naming
        patterns.push_back("ViewLayer." + layer_name + "." + component);     // "ViewLayer.Combined.R"
        patterns.push_back("viewlayer." + layer_name + "." + component);     // lowercase variant
        patterns.push_back("View Layer." + layer_name + "." + component);    // with space

        // Older Blender RenderLayer naming
        patterns.push_back("RenderLayer." + layer_name + "." + component);
        patterns.push_back("renderlayer." + layer_name + "." + component);

        // Blender with underscore separator
        patterns.push_back("ViewLayer." + layer_name + "_" + component);
        patterns.push_back("viewlayer." + layer_name + "_" + component);

        // Case variations
        std::string layer_lower = layer_name;
        std::transform(layer_lower.begin(), layer_lower.end(), layer_lower.begin(), ::tolower);
        patterns.push_back("ViewLayer." + layer_lower + "." + component);
        patterns.push_back("viewlayer." + layer_lower + "." + component);

        std::string layer_upper = layer_name;
        std::transform(layer_upper.begin(), layer_upper.end(), layer_upper.begin(), ::toupper);
        patterns.push_back(layer_upper + "." + component);
        patterns.push_back(layer_upper + "_" + component);
        patterns.push_back("VIEWLAYER." + layer_upper + "." + component);
        patterns.push_back("RENDERLAYER." + layer_upper + "." + component);

        // Channel-first patterns
        patterns.push_back(component + "." + layer_name);
        patterns.push_back(component + "_" + layer_name);
    }

    // Always try basic RGBA as fallback
    patterns.push_back(component);

    return patterns;
}

std::string DirectEXRCache::FindChannelName(
    const Imf_3_3::ChannelList& channels,
    const std::string& layer_name,
    const std::string& component) const {

    // Generate all possible patterns for this component
    std::vector<std::string> patterns = GenerateChannelPatterns(layer_name, component);

    // Try each pattern (case-insensitive)
    for (const auto& pattern : patterns) {
        for (auto it = channels.begin(); it != channels.end(); ++it) {
            std::string channel_name = it.name();
            std::string pattern_lower = pattern;
            std::string channel_lower = channel_name;

            std::transform(pattern_lower.begin(), pattern_lower.end(), pattern_lower.begin(), ::tolower);
            std::transform(channel_lower.begin(), channel_lower.end(), channel_lower.begin(), ::tolower);

            if (channel_lower == pattern_lower) {
                return channel_name;  // Return actual channel name from file
            }
        }
    }

    return "";  // Not found
}

//=============================================================================
// Static Utility Functions
//=============================================================================

bool DirectEXRCache::GetFrameDimensions(const std::string& file_path, int& width, int& height) {
    try {
        auto stream = std::make_unique<MemoryMappedIStream>(file_path);
        Imf::MultiPartInputFile file(*stream);

        const Imf::Header& header = file.header(0);
        Imath::Box2i dw = header.dataWindow();

        width = dw.max.x - dw.min.x + 1;
        height = dw.max.y - dw.min.y + 1;

        return true;
    } catch (const std::exception& e) {
        Debug::Log("DirectEXRCache::GetFrameDimensions: Exception - " + std::string(e.what()));
        return false;
    }
}

} // namespace ump
