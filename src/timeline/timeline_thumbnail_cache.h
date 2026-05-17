// TimelineThumbnailCache — port of old QCView's
// timeline_thumbnail_cache.{h,cpp}.
//
// Independent LRU thumbnail cache for timeline hover-preview. Unlike
// the main ImageSequenceCache (window-based eviction around the
// playhead), this cache holds frames from arbitrary positions in
// arbitrary source files — exactly what hover-anywhere preview
// needs.
//
// Architecture (preserved verbatim from old app, the threading and
// I/O posture is finely tuned and worth keeping):
//   - 3 worker threads when scrubbing/idle, 1 worker (worker 0) when
//     playing — bounds I/O contention against the playback decoder.
//   - Per-worker, per-source loaders: each worker owns its own
//     VideoImageLoader / EXRImageLoader / etc. for each source path
//     it's seen. No shared FFmpeg mutex.
//   - Dedup via queued_keys_ (set lookup before push to deque).
//   - `clear_generation_` increments on Clear() — workers in flight
//     drop their result rather than overwriting a fresh state.
//   - LRU bytes-budgeted eviction (default 100 entries × 320×180
//     RGBA8 ≈ 23 MB).
//   - GPU upload via MetalTexturePool::thumbnailInstance() (separate
//     pool from the main playback path so thumbnails can't evict
//     full-resolution main frames).
//
// Adaptations vs old app:
//   - namespace qcview → qcv
//   - GLuint texture handles → MetalTexturePool::Handle (uint64_t)
//   - Vulkan/OpenGL paths gated on QCV_NATIVE_PLAYER (Metal-only on
//     this branch, Vulkan when Phase C ships)
//   - Debug::Log → qInfo / qDebug
//   - SharedMemoryPool dependency dropped; PixelData carries its own
//     vector<uint8_t> in the new app
//   - Method names match the new app's IImageLoader vtable
//     (loadFrame / loadThumbnail / getDimensions / loaderName)

#pragma once

#include "decode/image_loader.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace qcv {

class VideoImageLoader;

// Worker pool state. Single source of truth for "should this worker
// be running right now" — checked in WorkerThread's CV-wait
// predicate.
enum class ThumbnailWorkerState {
    STOPPED,           // pool not running
    ACTIVE,            // all kMaxWorkerThreads workers active
    PAUSED_PLAYBACK    // only worker 0 active; others CV-paused
};

// (source path, source frame). For image-seq sources, the path is
// the actual per-frame file (with `?layer=` suffix for EXR) and the
// frame is 0 — same convention as old app's ruler-hover code so the
// cache key stays unique without sequence pattern parsing.
struct TimelineThumbnailKey {
    std::string source_path;
    int         source_frame = 0;

    bool operator<(const TimelineThumbnailKey &o) const {
        if (source_path != o.source_path)
            return source_path < o.source_path;
        return source_frame < o.source_frame;
    }
    bool operator==(const TimelineThumbnailKey &o) const {
        return source_path == o.source_path
            && source_frame == o.source_frame;
    }
};

struct TimelineThumbnailConfig {
    int  width      = 320;
    int  height     = 180;
    int  cache_size = 100;
    bool enabled    = true;
};

struct TimelineThumbnailEntry {
    std::uint64_t texture_id = 0;   // MetalTexturePool handle
    int           width      = 0;
    int           height     = 0;
};

class TimelineThumbnailCache {
public:
    TimelineThumbnailCache();
    ~TimelineThumbnailCache();

    void initialize(double fps);
    void shutdown();

    // Non-blocking. Returns the texture handle (or 0 if not yet
    // ready). On miss, queues async generation and — if
    // allow_fallback — returns the nearest cached frame for the
    // same source path so the UI has something to show during the
    // request.
    std::uint64_t getThumbnail(const std::string &source_path,
                                int source_frame,
                                int &width, int &height,
                                bool allow_fallback = true);

    // Bulk pre-fetch a clip range. step controls the stride
    // (default 6 = every 6th frame, decent for trim scrub).
    // priority_frame loads first if non-negative.
    void precacheRange(const std::string &source_path,
                       int start_frame, int end_frame,
                       int priority_frame = -1, int step = 6);

    // Main / GPU thread only. Drains pending uploads from worker
    // threads into the texture pool. Call once per frame before the
    // renderer reads thumbnail textures.
    void processPendingUploads();

    // Drop all cached textures. Workers in flight discard their
    // result via the clear_generation_ check (no race with the
    // calling thread).
    void clear();

    void cancelPendingRequests();

    bool isEnabled() const { return m_config.enabled; }

    // Toggle: when playing → 1 worker; when paused → 3 workers.
    void notifyPlaybackState(bool isPlaying);

    struct Stats {
        int total_cached     = 0;
        int cache_hits       = 0;
        int cache_misses     = 0;
        int pending_requests = 0;
    };
    Stats stats() const;

private:
    static constexpr int kMaxWorkerThreads      = 3;
    static constexpr int kPlaybackWorkerThreads = 1;

    // Per-source loader info. is_video selects whether we drive
    // image_loader with a frame-number string (video) or a file path
    // (image-seq).
    struct LoaderInfo {
        std::unique_ptr<IImageLoader> image_loader;
        bool   is_video    = false;
        int    frame_count = 0;
        double fps         = 24.0;
    };

    void workerThread(int worker_id);
    bool shouldWorkerRun(int worker_id) const;
    void requeueCurrentTask(int worker_id);

    std::shared_ptr<PixelData> loadThumbnailPixels(
        const TimelineThumbnailKey &key, int worker_id);

    std::uint64_t createGpuTexture(
        const std::shared_ptr<PixelData> &pixels);

    void evictLru();
    int  findNearestCachedFrame(const std::string &path,
                                 int target_frame) const;
    // Image-seq fallback. Parses `targetPath` as a sequence file
    // (`<prefix><digits><suffix>` with optional `?layer=`), then
    // searches the cache for the entry whose number is closest to
    // the target's. Used when the requested frame's exact path
    // isn't cached yet — keeps the corner overlay populated with a
    // neighbor frame from the same sequence instead of going blank.
    // Returns the matching key, or std::nullopt if no sibling
    // matched. Caller must hold m_cacheMutex.
    bool findNearestSequenceSibling(
        const std::string &targetPath,
        TimelineThumbnailKey &outKey) const;

    std::shared_ptr<LoaderInfo> getOrCreateLoader(
        const std::string &source_path, int worker_id);

    TimelineThumbnailConfig m_config;
    double                  m_fps         = 24.0;
    bool                    m_initialized = false;

    // ---- Cache + LRU
    std::map<TimelineThumbnailKey, TimelineThumbnailEntry> m_cache;
    std::list<TimelineThumbnailKey>                        m_lruOrder;
    mutable std::mutex                                     m_cacheMutex;

    // ---- Per-worker loaders
    // worker_id → { source_path → LoaderInfo }
    std::map<int, std::map<std::string,
                            std::shared_ptr<LoaderInfo>>>  m_workerLoaders;
    mutable std::mutex                                     m_loadersMutex;

    // ---- Request queue
    struct Request {
        TimelineThumbnailKey key;
        bool high_priority = true;
    };
    std::deque<Request>                m_requestQueue;
    std::set<TimelineThumbnailKey>     m_inProgress;
    std::set<TimelineThumbnailKey>     m_queuedKeys;
    mutable std::mutex                 m_queueMutex;
    std::condition_variable            m_queueCv;

    // ---- Pending uploads (worker → main)
    struct PendingUpload {
        TimelineThumbnailKey       key;
        std::shared_ptr<PixelData> pixels;
        std::uint64_t              gpu_texture_id = 0;
        int                        width  = 0;
        int                        height = 0;
    };
    std::deque<PendingUpload> m_pendingUploads;
    mutable std::mutex        m_uploadMutex;

    // ---- Workers
    std::vector<std::thread>            m_workers;
    std::atomic<bool>                   m_running{false};
    std::atomic<std::uint64_t>          m_clearGeneration{0};

    std::atomic<ThumbnailWorkerState>   m_workerState{
        ThumbnailWorkerState::STOPPED};
    std::atomic<int>                    m_activeThreadCount{
        kMaxWorkerThreads};
    std::condition_variable             m_workerCv;

    std::map<int, TimelineThumbnailKey> m_workerCurrentTask;
    std::mutex                          m_workerTaskMutex;

    std::atomic<bool>                   m_isPlaying{false};

    std::atomic<int>                    m_cacheHits{0};
    std::atomic<int>                    m_cacheMisses{0};

    // Set on clear(); next processPendingUploads() flushes pending
    // and clears the flag. Prevents getThumbnail() from briefly
    // returning a stale texture handle after clear() but before the
    // main thread has run the upload-drain.
    std::atomic<bool>                   m_clearPending{false};
};

} // namespace qcv
