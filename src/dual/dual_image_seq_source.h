// DualImageSeqSource — Phase 7.7 Stage 2.
//
// Image-sequence source for the dual playback island. Mirrors the
// architecture of qcv::ImageSequenceCache (the single-flow cache)
// but is fully isolated under qcv::dual:: — own worker threads, own
// cache, own discovery. Per Phase 7.7 design, two image-sequence
// sources cannot share a cache (key collisions when both reference
// the same on-disk sequence with different ranges/layers). Each
// DualImageSeqSource owns its own pool of I/O threads.
//
// Adaptive threading: DualPlaybackController halves the per-side
// I/O thread budget when both sides are image sequences (so total
// concurrency stays bounded). Set via setThreadCount() before open().
//
// Current scope:
//   - Discovery from a directory path (lists frames numerically).
//   - QImage::load() for image I/O — Qt's built-in PNG / JPEG / TIFF
//     decoders. EXR support comes later when we link qcv_decode's
//     ExrImageLoader.
//   - LRU cache, frame-count budget.
//   - Read-ahead window around decode_target.
//   - Standalone log-only verification via test_dual_image_seq.
//
// KNOWN GAPS (tracked as real tasks in the project task list):
//   - "DualPool — zero-copy MTLTexture handles": replaces shared_ptr
//     <QImage> with frame-keyed MTLTexture handles; same pool used by
//     DualVideoDecoder.
//   - EXR / multi-layer support via qcv_decode's ExrImageLoader.
//   - "DualPlaybackController — pre-open thread halving for image-seq
//     dual": when both A and B are sequences, halve each side's
//     worker count so dual doesn't oversubscribe at 2N threads.

#pragma once

#include "i_dual_source.h"

#include <QStringList>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <list>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace qcv::dual {

class DualImageSeqSource : public IDualSource {
public:
    // Default tunables — match old QCView's "feels good" numbers
    // scaled down for the simpler Stage 2 cache.
    static constexpr int kDefaultThreadCount  = 4;
    static constexpr int kReadAheadFrames     = 16;
    static constexpr int kReadBehindFrames    = 4;
    // Cache must be at least readAhead + readBehind + small slack.
    static constexpr int kCacheCapacityFrames = 32;

    DualImageSeqSource();
    ~DualImageSeqSource() override;

    DualImageSeqSource(const DualImageSeqSource &)            = delete;
    DualImageSeqSource &operator=(const DualImageSeqSource &) = delete;

    // ---- IDualSource ----
    // `path` may be:
    //   - A directory containing a numbered image sequence.
    //   - A path to one frame in a sequence (siblings auto-discovered).
    bool open(const QString &path) override;
    void close() override;
    bool isOpen() const override { return m_open.load(std::memory_order_acquire); }

    void setDecodeTarget(int frameNumber) override;
    void seekTo(int frameNumber) override;

    std::shared_ptr<DualFrame> getBufferedFrame(int frameNumber) const override;
    // Nearest cached frame to `frameNumber` (linear search). Used by
    // the pull path so a sparse (cache-stride > 1) window still shows a
    // frame when the exact target isn't on the cache grid. Returns the
    // exact frame when it's cached; nullptr only when nothing is cached.
    std::shared_ptr<DualFrame> getClosestFrame(int frameNumber) const override;
    bool hasFrame(int frameNumber) const override;

    int  bufferedAhead() const override;
    int  bufferedBehind() const override;
    int  bufferCount() const override;
    void getBufferedRange(int &startFrame, int &endFrame) const override;

    int    width()  const override { return m_width; }
    int    height() const override { return m_height; }
    double fps()    const override { return m_fps; }
    int    frameCount() const override { return m_frameCount; }
    QString path() const override { return m_path; }

    // Phase 3.H.5 — per-frame file path lookup. Used by
    // WindowManager's hover-thumbnail resolver to build a cache key
    // for a specific frame in this sequence (dual mode side B).
    // Returns empty for out-of-range frames.
    QString frameFilePath(int frameNumber) const {
        if (frameNumber < 0
            || frameNumber >= m_files.size()) {
            return QString();
        }
        return m_files.at(frameNumber);
    }

    // ---- Stage 2 specific ----
    // Adaptive thread count knob. Must be called before open(); has no
    // effect after the worker pool is spawned. DualPlaybackController
    // sets this to kDefaultThreadCount/2 when the other side is also
    // an image sequence.
    void setThreadCount(int n);
    int  threadCount() const { return m_threadCount; }

    // EXR layer / part selector. Set before open() (or between
    // open() calls) to pick a non-default part / channel-prefix.
    // Empty string = first part / unprefixed RGBA channels.
    // Ignored for non-EXR formats. Mirrors single-flow's
    // ImageSequenceCache layer plumbing.
    void setLayer(const QString &layer);
    QString layer() const { return m_layer; }

    // Loop-range hint for the cache + worker pool. When set,
    // eviction uses modular distances within [loIn, hiOut] —
    // frames near loIn are treated as "ahead" of target when
    // target is near hiOut, so they stay cached / get prefetched
    // before the loop wraps. Without this hint, the LRU window
    // around target evicts loIn frames long before the wrap and
    // playback stalls on disk re-read at the loop point.
    void setLoopRange(int loIn, int hiOut);
    void clearLoopRange();

    // Cache stride — 1 (default) caches every frame; N > 1 caches only
    // frames where (frameNumber % N == 0), forward-only (no read-behind),
    // for sparse prefetch during fast drags on huge sequences. Mirrors
    // ImageSequenceCache::setCacheStride. Clamped to [1,4]. The pull path
    // uses getClosestFrame so off-grid targets still display.
    void setCacheStride(int stride);
    int  cacheStride() const { return m_cacheStride.load(std::memory_order_acquire); }

    // Tunable fps used for downstream timecode display. Image
    // sequences don't carry an intrinsic fps — caller supplies it.
    void setFps(double fps);

    // Wake callback fired (on a worker thread) after a newly loaded
    // frame is inserted into the cache. See IDualSource for the full
    // rationale — the render-on-demand loop needs a wake once an
    // async load completes, otherwise first-frame-after-open /
    // first-frame-after-loop-wrap / seek-while-paused stay blank.
    void setFrameAvailableCallback(FrameAvailableCallback cb) override;

private:
    bool discoverSequence(const QString &path);
    bool probeFirstFrame();   // sets m_width/m_height from first file

    void cacheLoop();
    void ioWorkerLoop();

    std::shared_ptr<DualFrame> loadFrameBlocking(int frameNumber);

    int distanceFromTarget(int frame, int target) const;
    void evictOutsideWindowLocked(int target);
    void enqueueMissingFramesLocked(int target);

    void touchLruLocked(int frameNumber);
    void insertCacheLocked(int frameNumber, std::shared_ptr<DualFrame> frame);

    // ---- Sequence metadata ----
    QString m_path;          // user-supplied path (file or dir)
    QStringList m_files;     // frame N → file path (index = frameNumber)
    int     m_width  = 0;
    int     m_height = 0;
    int     m_frameCount = 0;
    double  m_fps    = 24.0;

    // ---- Adaptive threading knob ----
    int m_threadCount = kDefaultThreadCount;

    // EXR layer / part name. Empty = first part. Set via setLayer().
    QString m_layer;

    // Loop range, frame numbers. -1 = no loop range; eviction +
    // enqueue use linear distances. When both >= 0 with hiOut >=
    // loIn, both use modular distances over the loop length.
    std::atomic<int>  m_loopIn{-1};
    std::atomic<int>  m_loopOut{-1};

    // Cache stride — see setCacheStride(). 1 = every frame.
    std::atomic<int>  m_cacheStride{1};

    // ---- Cache (LRU by frame count) ----
    mutable std::mutex m_cacheMutex;
    std::unordered_map<int, std::shared_ptr<DualFrame>> m_cache;
    std::list<int>                                       m_lru;        // back = most-recent
    std::unordered_map<int, std::list<int>::iterator>    m_lruPos;

    // ---- Decode-ahead state ----
    std::atomic<int>  m_decodeTarget{0};
    std::atomic<int>  m_pendingSeekTarget{-1};

    // ---- Threading ----
    std::atomic<bool>  m_open{false};
    std::atomic<bool>  m_stopRequested{false};

    // Cache thread (pumps the window, queues missing frames, evicts).
    std::thread             m_cacheThread;
    std::condition_variable m_cacheCv;
    std::mutex              m_cacheCvMutex;

    // Worker pool (loads queued frames).
    std::vector<std::thread>     m_workers;
    std::deque<int>              m_workQueue;
    std::unordered_set<int>      m_inFlight;
    std::condition_variable      m_workCv;
    mutable std::mutex           m_workMutex;

    // H.4 — wake-renderer-on-frame-loaded callback. Fired by worker
    // threads after a successful insertCacheLocked. Protect the setter
    // (called from UI thread) and the reader (worker threads) with
    // a short mutex.
    mutable std::mutex      m_callbackMutex;
    FrameAvailableCallback  m_onFrameAvailable;
};

} // namespace qcv::dual
