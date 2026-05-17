// ImageSequenceCache — Phase 7.4.b.4 (renamed from DirectEXRCache).
//
// Faithful port of the old QCView's `direct_exr_cache.{h,cpp}`
// (~2249 lines, /Users/chris/Documents/GitHub/QCView-Player/src/player/).
// The OLD app uses DirectEXRCache for ALL primary image-sequence
// formats (EXR/TIFF/PNG/JPEG) — see timeline_cache.cpp:330-433. The
// per-format loader is plugged INTO the cache as IImageLoader. We
// follow that pattern; the new-port name reflects the scope.
//
// Architecture:
//
//   CacheThread (10 ms tick)
//     - Reads playhead atomic, detects seeks (>20 frame jumps,
//       allowing -2..-1 small rewinds without flushing).
//     - Bumps `m_requestGeneration` on big-jump seek so any
//       in-flight tasks discard their results when they complete.
//     - Evicts frames outside [current-readBehind, current+readAhead]
//       window with modulo-distance math for loop wrap-around.
//     - Builds the IO request queue: current frame FIRST, then
//       forward read-ahead, then backward read-behind (only when
//       cacheStride <= 1). Stride filters by absolute frame grid.
//     - Touches in-window cached frames in REVERSE distance order
//       so closest-to-playhead stays freshest in the LRU.
//     - Wakes IOWorkerThread via `m_ioCv`.
//
//   IOWorkerThread (cv-driven, 10 ms timeout)
//     - Pops frames from `m_videoRequests` while in-flight count
//       < threadCount. Spawns std::async per frame outside the lock.
//     - Polls completed futures non-blocking; under the lock,
//       collects results, releases lock, then processes:
//       * generation mismatch → discard
//       * pixels valid → m_pixelCache.add(frame, pd, bytes)
//       * sentinel / null → MakeBrokenSentinel + mark in m_failed
//       Failed frames never auto-retry; cleared only on shutdown.
//
// Display path (renderer thread, pull-model):
//
//   getFrame(N)        - Pixel cache peek (does NOT touch LRU).
//                        Returns nullptr on miss; caller falls
//                        through to getClosestFrame.
//   getClosestFrame(N) - Linear search over cached keys; provides
//                        last-good-frame fallback semantics.
//
// Memory budget: byte-based, computed at first frame load using
// actual width × height × 8 bytes (RGBA half). Old app's default
// of `(readAhead + readBehind + 50) * 64 MB` is a 4K-ish estimate;
// we recompute when a real frame lands and resize the LRU.
//
// Reuses (already shipped):
//   - MemoryMappedIStream  (decode/memory_mapped_istream.{h,cpp})
//   - All IImageLoader concretes (decode/{exr,tiff,png,jpeg}_image_loader)
//   - MakeGap/BrokenSentinel (decode/image_loader.h)

#pragma once

#include "image_loader.h"
#include "simple_lru.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace qcv {

// Tunable read-ahead / read-behind window. Defaults match the old
// app's "feels good" numbers: ~3 s lookahead, ~0.5 s behind at
// 24 fps. threadCount 16 matches old DirectEXRCache (validated 1–32
// at direct_exr_cache.h:84). Helps with slow multilayer EXRs (31
// channels, DWAB compression ~900 ms/frame).
struct ImageSeqStreamingConfig {
    int readAheadFrames  = 72;
    int readBehindFrames = 12;
    int threadCount      = 16;
};

class ImageSequenceCache
{
public:
    ImageSequenceCache(std::string directory, std::string pattern);
    ~ImageSequenceCache();

    ImageSequenceCache(const ImageSequenceCache &) = delete;
    ImageSequenceCache &operator=(const ImageSequenceCache &) = delete;

    bool initialize(int start_frame, int end_frame, double fps,
                    PipelineMode mode = PipelineMode::NORMAL,
                    const std::string &exrLayer = "");
    void shutdown();
    bool isInitialized() const { return m_initialized; }

    // Pull-model display API. Cache-thread populates the LRU async;
    // the renderer asks for the playhead's frame each present and
    // gets either a hit, a closest fallback, or nullptr (caller holds
    // the previously bound texture — last-good semantics).
    std::shared_ptr<PixelData> getFrame(int frame_number);
    std::shared_ptr<PixelData> getClosestFrame(
        int frame_number, int *actual_frame = nullptr);

    void updatePlayhead(int frame_number, bool force_seek = false);

    // Render-thread accessor — atomic load of the playhead the
    // CacheThread is prefetching around. PlayerRhiItem reads this
    // each synchronize() to know which frame to pull from the cache.
    int currentPlayhead() const { return m_playheadFrame.load(); }

    int    width() const { return m_width; }
    int    height() const { return m_height; }
    int    frameCount() const { return m_frameCount; }
    double fps() const { return m_fps; }

    int bufferedAhead() const  { return m_bufferAheadCount.load(); }
    int bufferedBehind() const { return m_bufferBehindCount.load(); }
    int bufferSize() const     { return m_bufferSize.load(); }

    // Frame-distance coverage of the cache window. Distinct from
    // the *count* above: at stride > 1 only every Nth frame is
    // cached, so count is sparser than coverage. The strip needs
    // coverage to draw the actual span buffered. Wraps along the
    // ring when looping is on.
    int bufferedAheadCoverage() const  { return m_bufferAheadCoverage.load(); }
    int bufferedBehindCoverage() const { return m_bufferBehindCoverage.load(); }
    int readAheadFrames() const  { return m_config.readAheadFrames; }
    int readBehindFrames() const { return m_config.readBehindFrames; }

    int  cacheStride() const { return m_cacheStride.load(); }
    void setCacheStride(int stride);

    // Wrap-around caching. ON by default — matches the old app's
    // direct_exr_cache `is_looping_=true`. When playback (or
    // prefetch) crosses the seam, modulo-distance math keeps the
    // window contiguous around the playhead instead of evicting
    // every frame and re-loading from disk.
    bool isLooping() const { return m_isLooping.load(); }
    void setLooping(bool enabled) { m_isLooping.store(enabled); }

    // Optional [in, out] loop range — when set, eviction + fill
    // restrict to that frame interval and use modulo distance
    // within it (wraps loop_in ↔ loop_out instead of around the
    // full sequence). Old app's direct_exr_cache::SetLoopRange.
    // Pass in < 0 or out <= in to clear (returns to full-sequence
    // wrap behavior). Distinct from the timer's loop range, which
    // controls *playback* wrap; this controls *caching*.
    void setLoopRange(int inFrame, int outFrame);
    void clearLoopRange();
    bool hasLoopRange() const { return m_hasLoopRange.load(); }
    int  loopInFrame()  const { return m_loopInFrame.load(); }
    int  loopOutFrame() const { return m_loopOutFrame.load(); }

    // Phase 7.5 cleanup — render thread reads this to drop stale GPU
    // textures when the cache bumps its generation (big seek / layer
    // change). Cheap atomic load.
    int  requestGeneration() const {
        return static_cast<int>(m_requestGeneration.load());
    }

    const std::string &exrLayer() const { return m_exrLayer; }
    void setExrLayer(const std::string &layer);

    // Pre-populate the failed-frame skip list. Called once after
    // initialize() with the gap set discovered at sequence-detection
    // time (file numbers missing from disk in [start..end]). The
    // cache's prefetch loop already skips entries in this set, so
    // pre-populating means we never even try to load a known-missing
    // frame. Snapshot getters reflect the seeded list immediately,
    // so the timeline can render red ticks before the cache thread
    // has done any I/O.
    void setKnownMissingFrames(const std::vector<int> &frames);

    // Snapshot of the failed-frame set under m_failedMutex. Includes
    // both pre-seeded (from setKnownMissingFrames) and runtime-
    // discovered failures (broken loaders, post-seed deletions).
    std::vector<int> failedFrames() const;
    int failedFrameCount() const;

    // Tuning override.
    void setStreamingConfig(const ImageSeqStreamingConfig &c) { m_config = c; }
    const ImageSeqStreamingConfig &streamingConfig() const { return m_config; }

    // Phase 7.5 B.6.2 — GPU upload sink. Native player renderers
    // register a callback here; whenever the I/O worker promotes a
    // freshly loaded PixelData into the cache, we invoke this with
    // the frame number + pixels. The upload thread on the other side
    // enqueues for MTLTexture / VkImage creation off the render
    // thread.
    //
    // Called from the I/O worker thread; sink must be thread-safe.
    using GpuPushCallback =
        std::function<void(int frame, std::shared_ptr<const PixelData> pixels)>;
    void setGpuPushCallback(GpuPushCallback cb);

    // Phase 7.5 cleanup — fires when a frame is evicted from the
    // pixel cache (window slid, hard seek, layer change). The
    // renderer drops the corresponding GPU texture so its texture
    // map stays bounded by the cache window. Called from the cache
    // thread; sink must be thread-safe.
    using FrameEvictedCallback = std::function<void(int frame)>;
    void setFrameEvictedCallback(FrameEvictedCallback cb);

private:
    // -------- Threads --------
    void cacheLoop();      // CacheThread entry
    void ioWorkerLoop();   // IOWorkerThread entry

    // -------- Helpers --------
    std::string framePath(int frame_number) const;
    void recomputeByteBudget(int width, int height);
    void evictOutsideWindowLocked(int currentFrame);
    void buildRequestQueueLocked(int currentFrame);
    int  forwardDistance(int frame, int current) const;
    int  backwardDistance(int frame, int current) const;
    bool loaderIsExr() const { return m_extension == ".exr"; }

    // -------- Sequence metadata --------
    bool        m_initialized = false;
    std::string m_directory;
    std::string m_pattern;
    std::string m_extension;   // ".exr" / ".tif" / ".png" / ".jpg" — for loader factory
    int         m_startFrame  = 1;
    int         m_endFrame    = 1;
    int         m_frameCount  = 0;
    double      m_fps         = 24.0;
    int         m_width       = 0;
    int         m_height      = 0;
    PipelineMode m_pipelineMode = PipelineMode::ULTRA_HIGH_RES;
    std::string  m_exrLayer;

    ImageSeqStreamingConfig m_config;

    // Phase 7.5 B.6.2 — GPU upload sink (see setGpuPushCallback).
    GpuPushCallback m_gpuPushCallback;
    mutable std::mutex m_gpuPushCallbackMutex;

    // Phase 7.5 cleanup — eviction sink. Same threading rules as
    // m_gpuPushCallback (called on the cache thread).
    FrameEvictedCallback m_frameEvictedCallback;
    mutable std::mutex m_frameEvictedCallbackMutex;

    // -------- Pixel cache (byte-budget LRU) --------
    SimpleLRU<int, std::shared_ptr<PixelData>> m_pixelCache;

    // Frame-presence accounting for cheap O(1) queries by the
    // CacheThread + display path. Updated under m_pixelCacheMutex.
    std::atomic<int> m_bufferAheadCount{0};
    std::atomic<int> m_bufferBehindCount{0};
    std::atomic<int> m_bufferSize{0};
    std::atomic<int> m_bufferAheadCoverage{0};
    std::atomic<int> m_bufferBehindCoverage{0};

    // -------- Threading --------
    std::thread       m_cacheThread;
    std::thread       m_ioWorkerThread;
    std::atomic<bool> m_running{false};

    // CacheThread sleep/wake.
    std::mutex                m_cacheSleepMutex;
    std::condition_variable   m_cacheCv;

    // IOWorkerThread queue + futures.
    std::mutex                m_ioMutex;
    std::condition_variable   m_ioCv;
    std::deque<int>           m_videoRequests;
    struct PendingTask {
        int                                   frame;
        uint64_t                              generation;
        std::future<std::shared_ptr<PixelData>> future;
    };
    std::unordered_map<int, PendingTask> m_inFlight;

    // -------- Playhead + seek --------
    std::atomic<int>      m_playheadFrame{0};
    std::atomic<int>      m_lastSeekFrame{0};
    std::atomic<uint64_t> m_requestGeneration{0};

    // -------- Cache stride --------
    std::atomic<int>      m_cacheStride{1};

    // -------- Looping --------
    std::atomic<bool>     m_isLooping{true};
    std::atomic<bool>     m_hasLoopRange{false};
    std::atomic<int>      m_loopInFrame{-1};
    std::atomic<int>      m_loopOutFrame{-1};

    // -------- Failure tracking --------
    // Frames that hit a sentinel (broken / gap). Never retry until
    // shutdown() / clear-equivalent. Single-thread access from the
    // IOWorkerThread polling step + CacheThread skip — guarded by
    // m_failedMutex (cheap, rarely contended).
    std::unordered_set<int> m_failedFrames;
    mutable std::mutex      m_failedMutex;

    // -------- Overrun --------
    std::atomic<bool> m_lastWasSyncLoad{false};
    std::atomic<bool> m_overrunMode{false};
    std::shared_ptr<PixelData> m_lastGoodFrame;
    std::mutex              m_lastGoodMutex;

    // -------- Tuning constants (load-bearing — see plan) --------
    static constexpr int kCacheTickMs       = 10;   // 100/sec proactive tick
    static constexpr int kIoWaitMs          = 10;   // cv timeout
    static constexpr int kBigJumpForward    = 20;   // > +20 = user seek
    static constexpr int kBigJumpBackward   = -2;   // < -2 = user seek
    static constexpr int kLoopWrapSlack     = 2;    // frames near loop boundary
    static constexpr int kMaxConcurrentReqs = 32;   // hard request cap
    static constexpr int kOverrunThreshold  = 1;    // immediate
    static constexpr int kEstFrameBytes4K   = 64 * 1024 * 1024; // initial estimate
    static constexpr int kSlackFrames       = 50;   // budget slack
};

} // namespace qcv
