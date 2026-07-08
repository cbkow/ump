#include "image_sequence_cache.h"

#include "exr_image_loader.h"
#include "jpeg_image_loader.h"
#include "png_image_loader.h"
#include "tiff_image_loader.h"

#include <QSettings>
#include <QtLogging>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <utility>

namespace qcv {

namespace {

// Lower-case the pattern's extension. Mirrors the dispatch shape
// in old QCView's timeline_cache.cpp:384-415 — the cache itself
// holds an IImageLoader and never cares about format internals.
std::string extensionFromPattern(const std::string &pattern)
{
    const auto pos = pattern.rfind('.');
    if (pos == std::string::npos) return ".png";
    std::string ext = pattern.substr(pos);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return ext;
}

// Per-task loader factory. Loaders are constructed inside each
// std::async task (thread-safety contract for libpng/libjpeg/
// OpenEXR — fresh state per worker).
std::unique_ptr<IImageLoader>
createLoaderForExtension(const std::string &ext)
{
    if (ext == ".png")  return std::make_unique<PNGImageLoader>();
    if (ext == ".tif" || ext == ".tiff")
        return std::make_unique<TIFFImageLoader>();
    if (ext == ".jpg" || ext == ".jpeg")
        return std::make_unique<JPEGImageLoader>();
    if (ext == ".exr")  return std::make_unique<EXRImageLoader>();
    return nullptr;
}

} // namespace

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

ImageSequenceCache::ImageSequenceCache(std::string directory,
                                       std::string pattern)
    : m_directory(std::move(directory))
    , m_pattern(std::move(pattern))
{
    m_extension = extensionFromPattern(m_pattern);

    // Phase 7.6 — apply user tunables (Settings panel) on top of
    // the struct defaults. Cache window + thread count are
    // worth exposing for the MacBook-Air → Threadripper range;
    // read-behind auto-derives from read-ahead with the
    // canonical 6:1 ratio (capped to 120 to match old app's
    // validation envelope).
    QSettings s;
    const int threads = s.value(
        QStringLiteral("performance/imageSeqThreads"),
        m_config.threadCount).toInt();
    const int ahead   = s.value(
        QStringLiteral("performance/imageSeqReadAhead"),
        m_config.readAheadFrames).toInt();
    m_config.threadCount      = std::clamp(threads, 1, 64);
    m_config.readAheadFrames  = std::clamp(ahead,  24, 240);
    m_config.readBehindFrames = std::clamp(
        std::max(12, m_config.readAheadFrames / 6), 0, 120);
}

ImageSequenceCache::~ImageSequenceCache()
{
    shutdown();
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bool ImageSequenceCache::initialize(int start_frame, int end_frame, double fps,
                                    PipelineMode mode,
                                    const std::string &exrLayer)
{
    if (m_initialized) return true;

    m_startFrame   = start_frame;
    m_endFrame     = end_frame;
    m_frameCount   = end_frame - start_frame + 1;
    m_fps          = fps;
    m_pipelineMode = mode;
    m_exrLayer     = exrLayer;

    // Probe first frame for dimensions. Format-aware via the loader
    // factory — EXR / TIFF / PNG / JPEG all support cheap getDimensions().
    auto probe = createLoaderForExtension(m_extension);
    if (!probe) {
        qWarning("ImageSequenceCache: no loader for extension '%s'",
                 m_extension.c_str());
        return false;
    }
    if (loaderIsExr()) {
        // EXR-specific layer setup before probing — `getDimensions`
        // on EXR routes through the layer-aware MultiPartInputFile
        // path so multi-part files probe correctly.
        static_cast<EXRImageLoader*>(probe.get())->setLayer(m_exrLayer);
    }
    if (!probe->getDimensions(framePath(0), m_width, m_height)) {
        qWarning("ImageSequenceCache: probe failed for '%s'",
                 framePath(0).c_str());
        return false;
    }

    // Initial byte budget — old app uses (readAhead + readBehind +
    // 50) × 64 MB as a 4K-ish estimate. We refine it once we have
    // the actual width × height (recomputeByteBudget) but seed it
    // here for the first few frames.
    recomputeByteBudget(m_width, m_height);

    m_playheadFrame = 0;
    m_lastSeekFrame = 0;
    m_requestGeneration = 0;
    m_lastWasSyncLoad = false;
    m_overrunMode = false;
    m_failedFrames.clear();

    m_running = true;
    m_cacheThread     = std::thread(&ImageSequenceCache::cacheLoop, this);
    m_ioWorkerThread  = std::thread(&ImageSequenceCache::ioWorkerLoop, this);

    m_initialized = true;
    return true;
}

void ImageSequenceCache::shutdown()
{
    if (!m_initialized) return;

    m_running = false;
    m_cacheCv.notify_all();
    m_ioCv.notify_all();

    if (m_cacheThread.joinable())    m_cacheThread.join();
    if (m_ioWorkerThread.joinable()) m_ioWorkerThread.join();

    // Wait for any remaining in-flight tasks to complete before
    // releasing the cache. Mirrors old DirectEXRCache shutdown
    // (DON'T clear in-flight while waiting — futures wait first).
    {
        std::unique_lock<std::mutex> lk(m_ioMutex);
        for (auto &kv : m_inFlight) {
            if (kv.second.future.valid()) kv.second.future.wait();
        }
        m_inFlight.clear();
        m_videoRequests.clear();
    }

    m_pixelCache.clear();
    m_failedFrames.clear();
    m_lastGoodFrame.reset();
    m_bufferAheadCount  = 0;
    m_bufferBehindCount = 0;
    m_bufferSize        = 0;
    m_initialized = false;
}

// ---------------------------------------------------------------------------
// Frame access
// ---------------------------------------------------------------------------

std::shared_ptr<PixelData> ImageSequenceCache::getFrame(int frame_number)
{
    // Display path uses peek (does NOT touch LRU) so already-shown
    // frames age out naturally — load-bearing per old app's
    // direct_exr_cache.h:213 comment.
    std::shared_ptr<PixelData> pd;
    const bool hit = m_pixelCache.peek(frame_number, pd);
    if (hit && pd) {
        // Cache hit → exit overrun if we were in it.
        m_lastWasSyncLoad.store(false, std::memory_order_release);
        // Stash for future overrun fallback.
        {
            std::lock_guard<std::mutex> lk(m_lastGoodMutex);
            m_lastGoodFrame = pd;
        }
        return pd;
    }

    // Miss — engage overrun semantics. Note: despite the old app's
    // misleading "GetFrameOrLoad" name, this does NOT do a sync
    // load. The display caller should fall through to
    // getClosestFrame; the cache thread is already prefetching.
    m_lastWasSyncLoad.store(true, std::memory_order_release);
    if (kOverrunThreshold == 1) {
        m_overrunMode.store(true, std::memory_order_release);
    }
    return nullptr;
}

std::shared_ptr<PixelData> ImageSequenceCache::getClosestFrame(
    int frame_number, int *actual_frame)
{
    // Walk the cache keys, find the closest by absolute distance
    // to frame_number. Linear search; for typical 80-frame
    // resident counts this is negligible vs the upload work it
    // saves. (Old app does the same.)
    auto keys = m_pixelCache.keys();
    if (keys.empty()) {
        if (actual_frame) *actual_frame = -1;
        return nullptr;
    }
    int best = keys.front();
    int bestDist = std::abs(best - frame_number);
    for (int k : keys) {
        const int d = std::abs(k - frame_number);
        if (d < bestDist) { best = k; bestDist = d; }
    }
    std::shared_ptr<PixelData> pd;
    if (m_pixelCache.peek(best, pd) && pd) {
        if (actual_frame) *actual_frame = best;
        return pd;
    }
    if (actual_frame) *actual_frame = -1;
    return nullptr;
}

// ---------------------------------------------------------------------------
// Playhead
// ---------------------------------------------------------------------------

void ImageSequenceCache::updatePlayhead(int frame_number, bool force_seek)
{
    frame_number = std::max(0, std::min(frame_number, m_frameCount - 1));
    const int prev = m_playheadFrame.exchange(frame_number);
    const int delta = frame_number - prev;
    bool bigJump = (delta < kBigJumpBackward || delta > kBigJumpForward);

    // Loop-wrap suppression — old app direct_exr_cache.cpp:644-665.
    // When playback rolls over the seam (prev near end → new near
    // start), the raw delta looks like a huge negative seek. With
    // wrap-around caching the in-point's pixels are already in the
    // window, so we don't want to bump the generation, clear the
    // queue, or evict — that throws away valid prefetch.
    if (bigJump && m_isLooping.load() && m_frameCount > 0) {
        const int slack = kLoopWrapSlack;
        const bool endToStart =
            prev         >= m_frameCount - 1 - slack &&
            frame_number <= slack;
        const bool startToEnd =
            prev         <= slack &&
            frame_number >= m_frameCount - 1 - slack;
        if (endToStart || startToEnd) {
            bigJump = false;
        }
    }

    if (force_seek || bigJump) {
        // Bump generation so any in-flight tasks discard their
        // results when they complete. DON'T clear m_inFlight —
        // that would block on future.wait(). Old app pattern:
        // direct_exr_cache.cpp:854-863.
        m_requestGeneration.fetch_add(1, std::memory_order_acq_rel);
        m_lastSeekFrame.store(frame_number, std::memory_order_release);
        // Clear pending queue (safe — futures aren't tracked here).
        {
            std::lock_guard<std::mutex> lk(m_ioMutex);
            m_videoRequests.clear();
        }
        // Wake CacheThread to re-evaluate window + queue.
        m_cacheCv.notify_one();
    }

    // Re-push the new playhead's pixels through the GPU sink, even
    // when nothing changed at the cache level. Without this, seeking
    // to a frame whose pixels are already in the pixel cache (e.g.,
    // small scrubs within the readahead window, or a paused
    // re-seek) never re-triggers the upload thread — the IO worker
    // only fires the push callback when it *promotes a fresh load*
    // into the cache. Result was: progress strip showed the frame
    // as buffered, but the upload thread had no MTLTexture for it,
    // so the renderer fell back to last-good and never painted the
    // user's actual seek target until playback nudged the cache
    // worker into pushing the next frame. Upload thread dedupes
    // via its own textureMap, so calling this on every playhead
    // update is cheap.
    if (delta != 0 || force_seek) {
        if (frame_number >= 0 && frame_number < m_frameCount) {
            std::shared_ptr<PixelData> pd;
            const bool inPixelCache =
                m_pixelCache.peek(frame_number, pd) && pd;
            if (inPixelCache && !pd->is_sentinel) {
                GpuPushCallback cb;
                {
                    std::lock_guard<std::mutex> lk(m_gpuPushCallbackMutex);
                    cb = m_gpuPushCallback;
                }
                if (cb) cb(frame_number, pd);
            }

            // Reactive priority request — only on explicit seeks
            // (force_seek) or big jumps. Firing this on every small
            // delta during drag-scrub queued stale frames faster
            // than the I/O worker pool could drain them; with 16
            // worker threads tied up on already-passed scrub
            // mid-points, the actual seek target had to wait for a
            // thread to free, making the perceived latency worse,
            // not better. Big jumps already bumped the generation
            // above and cleared m_videoRequests, so this push_front
            // lands into a freshly empty queue.
            if (!inPixelCache && (force_seek || bigJump)) {
                std::lock_guard<std::mutex> lk(m_ioMutex);
                if (!m_inFlight.count(frame_number)) {
                    bool failed = false;
                    {
                        std::lock_guard<std::mutex> fl(m_failedMutex);
                        failed = m_failedFrames.count(frame_number) > 0;
                    }
                    if (!failed) {
                        auto it = std::find(m_videoRequests.begin(),
                                            m_videoRequests.end(),
                                            frame_number);
                        if (it != m_videoRequests.end()) {
                            m_videoRequests.erase(it);
                        }
                        m_videoRequests.push_front(frame_number);
                        m_ioCv.notify_one();
                    }
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Cache stride
// ---------------------------------------------------------------------------

void ImageSequenceCache::setCacheStride(int stride)
{
    stride = std::clamp(stride, 1, 4);
    if (stride == m_cacheStride.load()) return;
    m_cacheStride.store(stride);
    // Wake cache thread to re-build queue with new stride filter.
    m_cacheCv.notify_one();
}

void ImageSequenceCache::setLoopRange(int inFrame, int outFrame)
{
    if (inFrame < 0 || outFrame <= inFrame ||
        outFrame >= m_frameCount) {
        clearLoopRange();
        return;
    }
    m_loopInFrame.store(inFrame);
    m_loopOutFrame.store(outFrame);
    m_hasLoopRange.store(true);
    // Wake cache thread to re-evaluate eviction/fill against the
    // new range. Existing in-flight loads outside the range will
    // be evicted on the next tick.
    m_cacheCv.notify_one();
}

void ImageSequenceCache::clearLoopRange()
{
    if (!m_hasLoopRange.load()) return;
    m_hasLoopRange.store(false);
    m_loopInFrame.store(-1);
    m_loopOutFrame.store(-1);
    m_cacheCv.notify_one();
}

// ---------------------------------------------------------------------------
// EXR layer
// ---------------------------------------------------------------------------

void ImageSequenceCache::setExrLayer(const std::string &layer)
{
    // No-op for non-EXR sequences — TIFF/PNG/JPEG don't have layers.
    if (!loaderIsExr()) return;
    if (layer == m_exrLayer) return;
    m_exrLayer = layer;
    // Old app: switching layers = re-Initialize (clears cache).
    // We approximate by clearing the pixel cache + generation bump
    // so in-flight tasks discard. Cache thread will re-prefetch
    // on the new layer at the current playhead.
    m_requestGeneration.fetch_add(1, std::memory_order_acq_rel);

    // Fire eviction callback for every currently-cached frame BEFORE
    // clearing — without this the upload thread's textureMap keeps
    // every stale-content texture from the previous layer, and the
    // renderer happily samples them as the new layer's frames load.
    // Snapshot the eviction sink first; call it outside the
    // pixel-cache clear so the callbacks see a consistent state.
    FrameEvictedCallback evictCb;
    {
        std::lock_guard<std::mutex> lk(m_frameEvictedCallbackMutex);
        evictCb = m_frameEvictedCallback;
    }
    if (evictCb) {
        for (int k : m_pixelCache.keys()) evictCb(k);
    }

    m_pixelCache.clear();
    {
        std::lock_guard<std::mutex> lk(m_failedMutex);
        m_failedFrames.clear();
    }
    {
        std::lock_guard<std::mutex> lk(m_ioMutex);
        m_videoRequests.clear();
    }
    {
        std::lock_guard<std::mutex> lk(m_lastGoodMutex);
        m_lastGoodFrame.reset();
    }
    m_bufferAheadCount     = 0;
    m_bufferBehindCount    = 0;
    m_bufferSize           = 0;
    m_bufferAheadCoverage  = 0;
    m_bufferBehindCoverage = 0;
    m_cacheCv.notify_one();
}

void ImageSequenceCache::setGpuPushCallback(GpuPushCallback cb)
{
    std::lock_guard<std::mutex> lk(m_gpuPushCallbackMutex);
    m_gpuPushCallback = std::move(cb);
}

void ImageSequenceCache::setFrameEvictedCallback(FrameEvictedCallback cb)
{
    std::lock_guard<std::mutex> lk(m_frameEvictedCallbackMutex);
    m_frameEvictedCallback = std::move(cb);
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

void ImageSequenceCache::setKnownMissingFrames(const std::vector<int> &frames)
{
    std::lock_guard<std::mutex> lk(m_failedMutex);
    for (int f : frames) {
        m_failedFrames.insert(f);
    }
}

std::vector<int> ImageSequenceCache::failedFrames() const
{
    std::lock_guard<std::mutex> lk(m_failedMutex);
    std::vector<int> out;
    out.reserve(m_failedFrames.size());
    for (int f : m_failedFrames) out.push_back(f);
    return out;
}

int ImageSequenceCache::failedFrameCount() const
{
    std::lock_guard<std::mutex> lk(m_failedMutex);
    return static_cast<int>(m_failedFrames.size());
}

std::string ImageSequenceCache::framePath(int frame_number) const
{
    const int fileFrame = frame_number + m_startFrame;
    char buf[1024];
    std::snprintf(buf, sizeof(buf), m_pattern.c_str(), fileFrame);
    std::filesystem::path dir(m_directory);
    std::filesystem::path file(buf);
    return (dir / file).string();
}

void ImageSequenceCache::recomputeByteBudget(int width, int height)
{
    // RGBA half-float = 8 bytes/pixel (EXR). 8-bit formats over-
    // estimate by ~4×; cheap insurance.
    const std::size_t perFrame =
        static_cast<std::size_t>(width) * height * 8;
    const std::size_t windowFrames =
        m_config.readAheadFrames + m_config.readBehindFrames + kSlackFrames;
    const std::size_t budget = perFrame
        ? perFrame * windowFrames
        : static_cast<std::size_t>(kEstFrameBytes4K) * windowFrames;
    m_pixelCache.setMaxBytes(budget);
}

// Linear distance helpers — Phase 7.4.b.4 fix. The 7.4.b.3 version
// always wrapped modulo m_frameCount, which is wrong for non-looping
// playback: e.g. on a 60-frame seq with current=50, frame=5, modulo
// said "15 ahead" when frame 5 is actually 45 frames BEHIND. Old
// app at direct_exr_cache.cpp:1191-1198 uses the linear range check
// `frame < current - readBehind || frame > current + readAhead`
// for the linear path; modulo is reserved for loop-mode wrap, which
// this port doesn't ship until Phase 7.7+.
int ImageSequenceCache::forwardDistance(int frame, int current) const
{
    if (m_isLooping.load() && m_frameCount > 0) {
        // Modulo distance — old app direct_exr_cache.cpp:1109,1184.
        // Result is in [0, m_frameCount - 1]; "0 = same frame, then
        // increasing as you walk forward through the ring."
        const int n = m_frameCount;
        return ((frame - current) % n + n) % n;
    }
    return frame - current;   // signed; negative means frame is behind
}

int ImageSequenceCache::backwardDistance(int frame, int current) const
{
    if (m_isLooping.load() && m_frameCount > 0) {
        const int n = m_frameCount;
        return ((current - frame) % n + n) % n;
    }
    return current - frame;   // signed; negative means frame is ahead
}

void ImageSequenceCache::evictOutsideWindowLocked(int currentFrame)
{
    // Caller does NOT need to hold any lock — SimpleLRU is internally
    // thread-safe. This is a CacheThread-only entry.
    //
    // Eviction window: keep frames within [readBehind, readAhead]
    // of the current frame. When looping, distances wrap around the
    // ring of length m_frameCount — so frame 5 in a 100-frame seq
    // is "15 ahead" of frame 90, not "85 behind." Old app:
    //   linear path: direct_exr_cache.cpp:1191-1198
    //   wrap path:   direct_exr_cache.cpp:1182-1190
    const int stride = std::max(1, m_cacheStride.load());
    const int readBehind = (stride > 1) ? 0 : m_config.readBehindFrames;
    const int readAhead = m_config.readAheadFrames;
    const bool looping = m_isLooping.load() && m_frameCount > 0;

    // Loop-range mode: pretend the sequence is the [in, out]
    // interval. Frames outside that interval evict unconditionally;
    // frames inside use modulo distance over the loop length so
    // the cache wraps in→out instead of around the full sequence.
    const bool hasRange    = m_hasLoopRange.load();
    const int  loopIn      = m_loopInFrame.load();
    const int  loopOut     = m_loopOutFrame.load();
    const int  loopSize    = (hasRange && loopOut > loopIn)
                             ? (loopOut - loopIn + 1) : 0;

    auto keys = m_pixelCache.keys();
    int aheadCount = 0, behindCount = 0;
    int aheadCoverage = 0, behindCoverage = 0;
    FrameEvictedCallback evictCb;
    {
        std::lock_guard<std::mutex> lk(m_frameEvictedCallbackMutex);
        evictCb = m_frameEvictedCallback;
    }
    for (int k : keys) {
        bool evict = false;
        int  fwd = 0, bwd = 0;
        if (hasRange && loopSize > 0) {
            // Outside the loop zone: always evict.
            if (k < loopIn || k > loopOut) {
                evict = true;
            } else {
                // Inside the zone — modulo distances over the loop
                // size so we wrap loop_in ↔ loop_out instead of
                // around the whole sequence. Use a clamped
                // currentFrame so out-of-zone playhead values
                // don't push the math off-grid.
                const int curInLoop = std::max(loopIn,
                                               std::min(loopOut, currentFrame))
                                       - loopIn;
                const int kInLoop   = k - loopIn;
                fwd = ((kInLoop - curInLoop) % loopSize + loopSize) % loopSize;
                bwd = ((curInLoop - kInLoop) % loopSize + loopSize) % loopSize;
                evict = (bwd > readBehind && fwd > readAhead);
            }
        } else {
            fwd = forwardDistance(k, currentFrame);
            bwd = backwardDistance(k, currentFrame);
            if (looping) {
                evict = (bwd > readBehind && fwd > readAhead);
            } else {
                evict = (fwd > readAhead) || (bwd > readBehind);
            }
        }
        if (evict) {
            m_pixelCache.remove(k);
            if (evictCb) evictCb(k);
            continue;
        }
        // Bucket count for the inspector strip — "ahead" if the
        // shortest path to this frame is forward (fwd <= bwd in
        // wrap mode; k >= current in linear). Coverage tracks the
        // farthest-out cached frame in each direction; the strip
        // uses coverage instead of count so a sparser stride > 1
        // window still draws the correct span.
        const bool isAhead = (hasRange && loopSize > 0)
                              ? (fwd <= bwd)
                              : looping ? (fwd <= bwd)
                                        : (k >= currentFrame);
        if (isAhead) {
            ++aheadCount;
            if (fwd > aheadCoverage) aheadCoverage = fwd;
        } else {
            ++behindCount;
            if (bwd > behindCoverage) behindCoverage = bwd;
        }
    }
    m_bufferAheadCount     = aheadCount;
    m_bufferBehindCount    = behindCount;
    m_bufferSize           = aheadCount + behindCount;
    m_bufferAheadCoverage  = aheadCoverage;
    m_bufferBehindCoverage = behindCoverage;
}

void ImageSequenceCache::buildRequestQueueLocked(int currentFrame)
{
    // Caller holds m_ioMutex.
    const int stride = std::max(1, m_cacheStride.load());

    const int totalPending = static_cast<int>(m_videoRequests.size())
                           + static_cast<int>(m_inFlight.size());
    if (totalPending >= kMaxConcurrentReqs) return;

    const int batchTarget = std::max(4, m_config.readAheadFrames / 2);
    int budget = std::min(batchTarget,
                          kMaxConcurrentReqs - totalPending);

    auto already = [this](int frame) {
        // Already cached, in-flight, queued, or known-failed?
        if (m_pixelCache.contains(frame)) return true;
        if (m_inFlight.count(frame)) return true;
        for (int q : m_videoRequests) if (q == frame) return true;
        std::lock_guard<std::mutex> lk(m_failedMutex);
        return m_failedFrames.count(frame) > 0;
    };

    const bool looping = m_isLooping.load() && m_frameCount > 0;

    auto inRange = [this](int frame) {
        return frame >= 0 && frame < m_frameCount;
    };

    auto onGrid = [stride](int absFrame) {
        return (stride <= 1) || (absFrame % stride == 0);
    };

    // Loop-range mode: when set, fill stays inside [in, out] and
    // wraps within that interval. The base playhead used for wrap
    // math gets clamped into the zone — stops a playhead parked
    // outside the range from producing nonsense frame numbers.
    const bool hasRange = m_hasLoopRange.load();
    const int  loopIn   = m_loopInFrame.load();
    const int  loopOut  = m_loopOutFrame.load();
    const bool useRange = hasRange && loopOut > loopIn;
    const int  rangeBase = useRange
                            ? std::max(loopIn, std::min(loopOut, currentFrame))
                            : currentFrame;
    const int  rangeSize = useRange ? (loopOut - loopIn + 1) : 0;

    // Wrap a raw forward/backward offset to a valid frame index.
    // Returns -1 when neither looping nor range mode applies and
    // the offset falls off the sequence (caller breaks out).
    auto wrapForward = [&](int offset) -> int {
        if (useRange) {
            const int idx = ((rangeBase - loopIn) + offset) % rangeSize;
            return loopIn + ((idx % rangeSize) + rangeSize) % rangeSize;
        }
        const int raw = currentFrame + offset;
        if (looping) return ((raw % m_frameCount) + m_frameCount) % m_frameCount;
        return inRange(raw) ? raw : -1;
    };
    auto wrapBackward = [&](int offset) -> int {
        if (useRange) {
            const int idx = ((rangeBase - loopIn) - offset) % rangeSize;
            return loopIn + ((idx % rangeSize) + rangeSize) % rangeSize;
        }
        const int raw = currentFrame - offset;
        if (looping) return ((raw % m_frameCount) + m_frameCount) % m_frameCount;
        return inRange(raw) ? raw : -1;
    };

    // Always queue the current frame FIRST when stride <= 1.
    // "Fixes first-frame delay on high-res files" — old app
    // direct_exr_cache.cpp:1365-1366. In range mode, prime with
    // the clamped rangeBase so a parked-out-of-zone playhead
    // doesn't request out-of-zone pixels.
    if (stride <= 1 && budget > 0) {
        const int primer = useRange ? rangeBase : currentFrame;
        if (inRange(primer) && onGrid(primer) && !already(primer)) {
            m_videoRequests.push_back(primer);
            --budget;
        }
    }

    // Cap forward / behind windows by rangeSize-1 when in range
    // mode — otherwise a 24-frame range with readAhead=72 would
    // wrap around the loop 3x and request the same frames over.
    const int aheadCap  = useRange
                          ? std::min(m_config.readAheadFrames,  rangeSize - 1)
                          : m_config.readAheadFrames;
    const int behindCap = useRange
                          ? std::min(m_config.readBehindFrames, rangeSize - 1)
                          : m_config.readBehindFrames;

    // Forward read-ahead.
    for (int offset = 1; offset <= aheadCap && budget > 0; ++offset) {
        const int absFrame = wrapForward(offset);
        if (absFrame < 0) break;            // off end, non-looping
        if (!onGrid(absFrame)) continue;
        if (!already(absFrame)) {
            m_videoRequests.push_back(absFrame);
            --budget;
        }
    }

    // Backward read-behind ONLY when stride <= 1. Stride > 1
    // collapses the behind window to 0 to give all budget forward.
    if (stride <= 1) {
        for (int offset = 1; offset <= behindCap && budget > 0; ++offset) {
            const int absFrame = wrapBackward(offset);
            if (absFrame < 0) break;        // off start, non-looping
            if (!already(absFrame)) {
                m_videoRequests.push_back(absFrame);
                --budget;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// CacheThread — proactive scheduling + eviction
// ---------------------------------------------------------------------------

void ImageSequenceCache::cacheLoop()
{
    while (m_running.load(std::memory_order_acquire)) {
        // 10 ms tick. Old app: "100 ticks/sec for fast response."
        {
            std::unique_lock<std::mutex> lk(m_cacheSleepMutex);
            m_cacheCv.wait_for(lk,
                std::chrono::milliseconds(kCacheTickMs));
        }
        if (!m_running.load(std::memory_order_acquire)) break;

        const int current = m_playheadFrame.load();

        // Eviction pass.
        evictOutsideWindowLocked(current);

        // (Old app's LRU touch-reverse pass intentionally OMITTED
        // here. The old impl did 80+ individual unique_lock acquires
        // per cache tick to push closest-to-playhead to MRU. On
        // macOS std::shared_mutex (pthread_rwlock) is writer-
        // preferring, so the render thread's peek() starves under
        // that pattern — Qt Quick's GUI thread blocks at sync, whole
        // app freezes within ~1 sec of playback. Eviction by linear
        // window range — without LRU re-touching — gives the same
        // correctness for our pull-model. Old app needed the touch
        // loop because byte-budget eviction would otherwise drop
        // near-playhead frames; we get the same protection from the
        // window-range filter in evictOutsideWindowLocked.)

        // Build request queue + wake IOWorker.
        bool queued = false;
        {
            std::lock_guard<std::mutex> lk(m_ioMutex);
            const std::size_t before = m_videoRequests.size();
            buildRequestQueueLocked(current);
            queued = (m_videoRequests.size() > before);
        }
        if (queued) m_ioCv.notify_one();
    }
}

// ---------------------------------------------------------------------------
// IOWorkerThread — parallel decode dispatch
// ---------------------------------------------------------------------------

void ImageSequenceCache::ioWorkerLoop()
{
    while (m_running.load(std::memory_order_acquire)) {
        // Wait for work. Predicate intentionally does NOT include
        // `!m_inFlight.empty()` — that would tight-loop while tasks
        // are in flight (predicate true → wait_for returns instantly →
        // loop body re-runs). The 10ms cv timeout already provides
        // the periodic poll for in-flight completions. Old app's
        // direct_exr_cache.cpp:1535 same shape.
        {
            std::unique_lock<std::mutex> lk(m_ioMutex);
            m_ioCv.wait_for(lk,
                std::chrono::milliseconds(kIoWaitMs),
                [this] {
                    return !m_running.load() ||
                           !m_videoRequests.empty();
                });
        }
        if (!m_running.load(std::memory_order_acquire)) break;

        // -------- Spawn new tasks --------
        // Build the to-spawn batch under the lock; spawn outside.
        // Cap at `threadCount` (default 16, old app direct_exr_cache.h:72)
        // — NOT `readAheadFrames` which the 7.4.b.3 port wrongly used,
        // pinning fill rate at ~16 fps via 8-thread saturation.
        struct SpawnItem {
            int frame; uint64_t generation; std::string path;
            bool express = false;
        };
        std::vector<SpawnItem> spawnBatch;
        const std::size_t threadCap =
            static_cast<std::size_t>(std::max(1, m_config.threadCount));
        // EXR perf audit 2026-07-08: the frame the user just seeked
        // to gets an EXPRESS LANE — it may exceed the worker cap by
        // one. Without it, a seek that lands while all workers are
        // mid-decode on now-stale frames (~250 ms each on 4K DWAB)
        // waits for the first one to finish before its own decode
        // even STARTS. Results of the stale decodes still discard
        // via the generation check; this only stops them from
        // gatekeeping the one frame the user is looking at.
        const int seekFrame =
            m_lastSeekFrame.load(std::memory_order_acquire);
        {
            std::unique_lock<std::mutex> lk(m_ioMutex);
            while (!m_videoRequests.empty() &&
                   m_inFlight.size() + spawnBatch.size()
                       < static_cast<std::size_t>(kMaxConcurrentReqs)) {
                const int frame = m_videoRequests.front();
                const bool express = (frame == seekFrame);
                const std::size_t cap = express ? threadCap + 1
                                                : threadCap;
                if (m_inFlight.size() + spawnBatch.size() >= cap) break;
                m_videoRequests.pop_front();
                if (m_inFlight.count(frame)) continue;
                if (m_pixelCache.contains(frame)) continue;
                SpawnItem item;
                item.frame      = frame;
                item.generation = m_requestGeneration.load();
                item.path       = framePath(frame);
                item.express    = express;
                spawnBatch.push_back(std::move(item));
            }
        }

        // Spawn outside lock.
        const std::string ext = m_extension;
        for (auto &item : spawnBatch) {
            const std::string layer = m_exrLayer;
            const PipelineMode mode = m_pipelineMode;
            const bool express = item.express;
            std::future<std::shared_ptr<PixelData>> fut =
                std::async(std::launch::async,
                    [path = item.path, layer, mode, ext, express]()
                    -> std::shared_ptr<PixelData> {
                        try {
                            std::error_code ec;
                            if (!std::filesystem::exists(path, ec) || ec) {
                                return MakeGapSentinel();
                            }
                            auto loader = createLoaderForExtension(ext);
                            if (!loader) return nullptr;
                            // Express (seek-target) frames decode
                            // with OpenEXR's internal pool — 4K DWAB
                            // drops ~225→~56 ms. Read-ahead stays
                            // synchronous: the worker pool already
                            // saturates cores frame-parallel.
                            if (express) loader->setDecodeThreads(8);
                            return loader->loadFrame(path, layer, mode);
                        } catch (...) {
                            return nullptr;
                        }
                    });

            std::lock_guard<std::mutex> lk(m_ioMutex);
            PendingTask pt;
            pt.frame      = item.frame;
            pt.generation = item.generation;
            pt.future     = std::move(fut);
            m_inFlight.emplace(item.frame, std::move(pt));
        }

        // -------- Poll completed futures --------
        struct CompletedResult {
            int frame;
            uint64_t generation;
            std::shared_ptr<PixelData> pixels;
        };
        std::vector<CompletedResult> completed;
        {
            std::unique_lock<std::mutex> lk(m_ioMutex);
            for (auto it = m_inFlight.begin(); it != m_inFlight.end();) {
                if (it->second.future.valid() &&
                    it->second.future.wait_for(std::chrono::seconds(0))
                        == std::future_status::ready) {
                    CompletedResult r;
                    r.frame = it->second.frame;
                    r.generation = it->second.generation;
                    try {
                        r.pixels = it->second.future.get();
                    } catch (...) {
                        r.pixels.reset();
                    }
                    completed.push_back(std::move(r));
                    it = m_inFlight.erase(it);
                } else {
                    ++it;
                }
            }
        }

        // Process completions outside lock.
        const uint64_t curGen = m_requestGeneration.load();
        for (auto &r : completed) {
            // Stale (generation mismatch) — discard.
            if (r.generation != curGen) continue;

            std::shared_ptr<PixelData> pd = r.pixels;
            if (!pd) {
                pd = MakeBrokenSentinel(m_width, m_height);
                std::lock_guard<std::mutex> lk(m_failedMutex);
                m_failedFrames.insert(r.frame);
            } else if (IsSentinel(pd)) {
                std::lock_guard<std::mutex> lk(m_failedMutex);
                m_failedFrames.insert(r.frame);
            }

            // Refine byte budget once we have actual pixel dimensions
            // — first real frame triggers a re-size of the LRU.
            if (!pd->is_sentinel && pd->width > 0 &&
                pd->width * pd->height * 8 != m_width * m_height * 8 &&
                m_pixelCache.maxBytes() == 0) {
                recomputeByteBudget(pd->width, pd->height);
            }

            const std::size_t bytes =
                (pd && !pd->is_sentinel)
                    ? static_cast<std::size_t>(pd->byteSize())
                    : 4;  // sentinels are tiny
            m_pixelCache.add(r.frame, pd, bytes);

            // Phase 7.5 B.6.2 — push freshly-loaded pixels to the GPU
            // upload sink (the native player's MetalGpuUploadThread or
            // VulkanGpuUploadThread). Skip sentinels — broken/gap
            // frames render via the renderer's clear color, no need to
            // burn a GPU texture on them.
            if (pd && !pd->is_sentinel) {
                GpuPushCallback cb;
                {
                    std::lock_guard<std::mutex> lk2(m_gpuPushCallbackMutex);
                    cb = m_gpuPushCallback;
                }
                if (cb) {
                    cb(r.frame, pd);
                }
            }
        }
    }

    // Drain on shutdown.
    std::unique_lock<std::mutex> lk(m_ioMutex);
    for (auto &kv : m_inFlight) {
        if (kv.second.future.valid()) kv.second.future.wait();
    }
}

} // namespace qcv
