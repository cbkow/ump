// TimelineThumbnailCache implementation. Architectural notes in the
// header — this file is the worker / queue / GPU-upload mechanics
// ported from the old app with namespace + API renames.

#include "timeline_thumbnail_cache.h"

#include "decode/exr_image_loader.h"
#include "decode/jpeg_image_loader.h"
#include "decode/png_image_loader.h"
#include "decode/tiff_image_loader.h"
#include "decode/video_image_loader.h"

// QtGlobal must come before the platform gate below — Q_OS_APPLE and
// Q_OS_WIN are defined by Qt's qsystemdetection.h, NOT by the C++
// compiler's predefined macros. Without this, the gate evaluates false
// and the GPU-upload backend is silently stubbed out on both platforms.
#include <QtGlobal>

#if defined(QCV_NATIVE_PLAYER) && defined(Q_OS_APPLE)
#include "render/metal/metal_texture_pool.h"
#elif defined(QCV_NATIVE_PLAYER) && defined(Q_OS_WIN)
#include "render/d3d11/d3d11_texture_pool.h"
#endif
// GPU-upload backend: Metal on Apple, D3D11 on Windows. Both pools
// share API + format enum (see d3d11_texture_pool.h) so the call sites
// below differ only by the pool type.

#include <QtLogging>

#include <algorithm>
#include <climits>
#include <cstdlib>

namespace qcv {

namespace {

// Match the old app's pool format codes:
//   0 = RGBA8Unorm   1 = RGBA16Float   2 = BGRA8Unorm   3 = RGBA16Unorm
int poolFormatFor(PixelFormat fmt) {
    switch (fmt) {
        case PixelFormat::RGBA8:   return 0;
        case PixelFormat::RGBA16:  return 3;
        case PixelFormat::RGBA16F: return 1;
    }
    return 0;
}

// Lower-case ASCII extension. Empty if no dot.
std::string extOf(const std::string &path) {
    const std::size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return {};
    std::string ext = path.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return ext;
}

// Split off `?layer=...` suffix used by EXR keys to identify a part.
void splitLayer(const std::string &keyPath,
                std::string &filePath, std::string &layer) {
    const std::size_t q = keyPath.find("?layer=");
    if (q == std::string::npos) {
        filePath = keyPath;
        layer.clear();
        return;
    }
    filePath = keyPath.substr(0, q);
    layer    = keyPath.substr(q + 7);  // skip "?layer="
}

} // namespace

TimelineThumbnailCache::TimelineThumbnailCache() = default;

TimelineThumbnailCache::~TimelineThumbnailCache()
{
    try {
        shutdown();
    } catch (...) {
        // Static-destruction-time exceptions: nothing safe to log.
    }
}

void TimelineThumbnailCache::initialize(double fps)
{
    if (m_initialized) return;

    m_fps                = fps;
    m_initialized        = true;
    m_running            = true;
    m_workerState        = ThumbnailWorkerState::ACTIVE;
    m_activeThreadCount  = kMaxWorkerThreads;
    m_isPlaying          = false;

    m_workers.reserve(kMaxWorkerThreads);
    for (int i = 0; i < kMaxWorkerThreads; ++i) {
        m_workers.emplace_back(
            &TimelineThumbnailCache::workerThread, this, i);
    }

    qInfo("TimelineThumbnailCache: initialized fps=%.3f workers=%d",
          fps, kMaxWorkerThreads);
}

void TimelineThumbnailCache::shutdown()
{
    if (!m_initialized) return;

    m_running     = false;
    m_workerState = ThumbnailWorkerState::STOPPED;
    m_queueCv.notify_all();
    m_workerCv.notify_all();

    for (auto &t : m_workers) {
        if (t.joinable()) {
            try { t.join(); }
            catch (...) {
                try { t.detach(); } catch (...) {}
            }
        }
    }
    m_workers.clear();

    {
        std::lock_guard<std::mutex> lock(m_cacheMutex);
        for (auto &kv : m_cache) {
            if (kv.second.texture_id != 0) {
#if defined(QCV_NATIVE_PLAYER) && defined(Q_OS_APPLE)
                if (MetalTexturePool::thumbnailInstance().isInitialized()) {
                    MetalTexturePool::thumbnailInstance().queueDelete(
                        kv.second.texture_id);
                }
#elif defined(QCV_NATIVE_PLAYER) && defined(Q_OS_WIN)
                if (D3D11TexturePool::thumbnailInstance().isInitialized()) {
                    D3D11TexturePool::thumbnailInstance().queueDelete(
                        kv.second.texture_id);
                }
#endif
            }
        }
        m_cache.clear();
        m_lruOrder.clear();
    }

    {
        std::lock_guard<std::mutex> lock(m_loadersMutex);
        m_workerLoaders.clear();
    }
    {
        std::lock_guard<std::mutex> lock(m_workerTaskMutex);
        m_workerCurrentTask.clear();
    }

    m_initialized = false;
    qInfo("TimelineThumbnailCache: shutdown complete");
}

std::uint64_t TimelineThumbnailCache::getThumbnail(
    const std::string &source_path, int source_frame,
    int &width, int &height, bool allow_fallback)
{
    if (!m_config.enabled || !m_initialized) {
        width = height = 0;
        return 0;
    }
    // Hold off returning textures during the in-flight clear()
    // until processPendingUploads has run (next main-thread tick).
    if (m_clearPending.load()) {
        width = height = 0;
        return 0;
    }

    TimelineThumbnailKey key{source_path, source_frame};
    std::lock_guard<std::mutex> lock(m_cacheMutex);

    auto it = m_cache.find(key);
    if (it != m_cache.end()) {
        m_cacheHits++;
        m_lruOrder.remove(key);
        m_lruOrder.push_front(key);
        width  = it->second.width;
        height = it->second.height;
        return it->second.texture_id;
    }

    m_cacheMisses++;
    {
        std::lock_guard<std::mutex> qlock(m_queueMutex);
        if (m_inProgress.find(key) == m_inProgress.end()
            && m_queuedKeys.find(key) == m_queuedKeys.end()) {
            m_requestQueue.push_front({key, /*high_priority=*/true});
            m_queuedKeys.insert(key);
            m_queueCv.notify_one();
        }
    }

    if (allow_fallback) {
        // Same-source nearest-frame (video case — many entries, all
        // sharing source_path, varying source_frame).
        const int near = findNearestCachedFrame(source_path,
                                                  source_frame);
        if (near >= 0) {
            TimelineThumbnailKey nk{source_path, near};
            auto nit = m_cache.find(nk);
            if (nit != m_cache.end()) {
                width  = nit->second.width;
                height = nit->second.height;
                return nit->second.texture_id;
            }
        }
        // Image-seq nearest-sibling (each frame is its own
        // source_path; the by-frame match above never fires here).
        // Keeps the corner overlay populated with a neighbor frame
        // from the same sequence while the worker decodes the
        // exact one.
        TimelineThumbnailKey siblingKey;
        if (findNearestSequenceSibling(source_path, siblingKey)) {
            auto sit = m_cache.find(siblingKey);
            if (sit != m_cache.end()) {
                width  = sit->second.width;
                height = sit->second.height;
                return sit->second.texture_id;
            }
        }
    }

    width = height = 0;
    return 0;
}

void TimelineThumbnailCache::precacheRange(
    const std::string &source_path, int start_frame, int end_frame,
    int priority_frame, int step)
{
    if (!m_config.enabled || !m_initialized) return;
    if (step <= 0) step = 1;

    std::lock_guard<std::mutex> qlock(m_queueMutex);

    if (priority_frame >= start_frame
        && priority_frame <= end_frame) {
        TimelineThumbnailKey k{source_path, priority_frame};
        if (m_inProgress.find(k) == m_inProgress.end()
            && m_queuedKeys.find(k) == m_queuedKeys.end()) {
            m_requestQueue.push_front({k, true});
            m_queuedKeys.insert(k);
        }
    }
    for (int f = start_frame; f <= end_frame; f += step) {
        if (f == priority_frame) continue;
        TimelineThumbnailKey k{source_path, f};
        {
            std::lock_guard<std::mutex> clock(m_cacheMutex);
            if (m_cache.find(k) != m_cache.end()) continue;
        }
        if (m_inProgress.find(k) != m_inProgress.end()) continue;
        if (m_queuedKeys.find(k) != m_queuedKeys.end()) continue;
        m_requestQueue.push_back({k, /*high_priority=*/false});
        m_queuedKeys.insert(k);
    }
    m_queueCv.notify_one();
}

void TimelineThumbnailCache::cancelPendingRequests()
{
    std::lock_guard<std::mutex> lock(m_queueMutex);
    m_requestQueue.clear();
    m_queuedKeys.clear();
    // in_progress_ items finish naturally; their result is discarded
    // by the clear-generation check if the caller has since
    // moved on.
}

void TimelineThumbnailCache::processPendingUploads()
{
    if (!m_initialized) return;

    m_clearPending = false;

    std::deque<PendingUpload> uploads;
    {
        std::lock_guard<std::mutex> lock(m_uploadMutex);
        uploads.swap(m_pendingUploads);
    }
    if (uploads.empty()) return;

    // Metal/Vulkan: textures already created on worker thread, this
    // pass is just cache-map insertion. Old app caps at 64 to
    // amortize lock contention; same here.
    constexpr int kMaxUploadsPerFrame = 64;
    int n = 0;
    while (!uploads.empty()) {
        if (n >= kMaxUploadsPerFrame) {
            std::lock_guard<std::mutex> lock(m_uploadMutex);
            while (!uploads.empty()) {
                m_pendingUploads.push_back(std::move(uploads.front()));
                uploads.pop_front();
            }
            break;
        }
        auto &up = uploads.front();
        std::uint64_t tex = up.gpu_texture_id;
        int tw = up.width;
        int th = up.height;
        if (tex != 0) {
            std::lock_guard<std::mutex> lock(m_cacheMutex);
            while (static_cast<int>(m_cache.size())
                   >= m_config.cache_size) {
                evictLru();
            }
            TimelineThumbnailEntry e;
            e.texture_id = tex;
            e.width      = tw;
            e.height     = th;
            m_cache[up.key] = e;
            m_lruOrder.push_front(up.key);
        }
        uploads.pop_front();
        ++n;
    }
}

void TimelineThumbnailCache::clear()
{
    qInfo("TimelineThumbnailCache: clear");

    m_clearGeneration.fetch_add(1, std::memory_order_release);
    m_clearPending = true;

    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_requestQueue.clear();
        m_queuedKeys.clear();
        m_inProgress.clear();
    }
    {
        std::lock_guard<std::mutex> lock(m_uploadMutex);
        m_pendingUploads.clear();
    }
    {
        std::lock_guard<std::mutex> lock(m_cacheMutex);
        for (auto &kv : m_cache) {
            if (kv.second.texture_id != 0) {
#if defined(QCV_NATIVE_PLAYER) && defined(Q_OS_APPLE)
                if (MetalTexturePool::thumbnailInstance().isInitialized()) {
                    MetalTexturePool::thumbnailInstance().queueDelete(
                        kv.second.texture_id);
                }
#elif defined(QCV_NATIVE_PLAYER) && defined(Q_OS_WIN)
                if (D3D11TexturePool::thumbnailInstance().isInitialized()) {
                    D3D11TexturePool::thumbnailInstance().queueDelete(
                        kv.second.texture_id);
                }
#endif
            }
        }
        m_cache.clear();
        m_lruOrder.clear();
    }

    // Worker-loader map intentionally NOT cleared here — workers may
    // be mid-load. Loaders are torn down only in shutdown() after
    // threads are joined. clear_generation_ keeps stale results out
    // of the cache.

    m_cacheHits   = 0;
    m_cacheMisses = 0;
}

TimelineThumbnailCache::Stats
TimelineThumbnailCache::stats() const
{
    Stats s;
    {
        std::lock_guard<std::mutex> lock(m_cacheMutex);
        s.total_cached = static_cast<int>(m_cache.size());
    }
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        s.pending_requests = static_cast<int>(
            m_requestQueue.size() + m_inProgress.size());
    }
    s.cache_hits   = m_cacheHits.load();
    s.cache_misses = m_cacheMisses.load();
    return s;
}

bool TimelineThumbnailCache::shouldWorkerRun(int worker_id) const
{
    return worker_id == 0 || !m_isPlaying.load();
}

void TimelineThumbnailCache::requeueCurrentTask(int worker_id)
{
    // Caller already holds m_queueMutex.
    std::lock_guard<std::mutex> lock(m_workerTaskMutex);
    auto it = m_workerCurrentTask.find(worker_id);
    if (it == m_workerCurrentTask.end()) return;
    TimelineThumbnailKey k = it->second;
    m_workerCurrentTask.erase(it);
    m_inProgress.erase(k);
    m_requestQueue.push_front({k, /*high_priority=*/true});
}

void TimelineThumbnailCache::notifyPlaybackState(bool isPlaying)
{
    bool was = m_isPlaying.exchange(isPlaying);
    if (isPlaying && !was) {
        m_activeThreadCount = kPlaybackWorkerThreads;
        m_workerState       =
            ThumbnailWorkerState::PAUSED_PLAYBACK;
        m_queueCv.notify_all();
        qInfo("TimelineThumbnailCache: playback → %d worker(s)",
              kPlaybackWorkerThreads);
    } else if (!isPlaying && was) {
        m_activeThreadCount = kMaxWorkerThreads;
        m_workerState       = ThumbnailWorkerState::ACTIVE;
        m_workerCv.notify_all();
        m_queueCv.notify_all();
        qInfo("TimelineThumbnailCache: paused → %d workers",
              kMaxWorkerThreads);
    }
}

std::shared_ptr<TimelineThumbnailCache::LoaderInfo>
TimelineThumbnailCache::getOrCreateLoader(
    const std::string &source_path, int worker_id)
{
    std::lock_guard<std::mutex> lock(m_loadersMutex);
    auto &wm = m_workerLoaders[worker_id];
    auto it = wm.find(source_path);
    if (it != wm.end()) return it->second;

    auto info = std::make_shared<LoaderInfo>();

    // EXR keys carry "?layer=Beauty" suffixes. Strip before
    // extension-check.
    std::string filePath, layer;
    splitLayer(source_path, filePath, layer);
    const std::string ext = extOf(filePath);

    const bool isVideo =
        (ext == "mov" || ext == "mp4" || ext == "mxf"
         || ext == "avi" || ext == "mkv" || ext == "m4v");

    if (isVideo) {
        info->is_video = true;
        info->image_loader = std::make_unique<VideoImageLoader>(
            filePath, m_fps, /*durationHint=*/0.0);
    } else {
        info->is_video = false;
        if (ext == "exr") {
            auto loader = std::make_unique<EXRImageLoader>();
            if (!layer.empty()) loader->setLayer(layer);
            info->image_loader = std::move(loader);
        } else if (ext == "tiff" || ext == "tif") {
            info->image_loader = std::make_unique<TIFFImageLoader>();
        } else if (ext == "png") {
            info->image_loader = std::make_unique<PNGImageLoader>();
        } else if (ext == "jpg" || ext == "jpeg") {
            info->image_loader = std::make_unique<JPEGImageLoader>();
        }
    }

    wm[source_path] = info;
    return info;
}

std::shared_ptr<PixelData>
TimelineThumbnailCache::loadThumbnailPixels(
    const TimelineThumbnailKey &key, int worker_id)
{
    auto info = getOrCreateLoader(key.source_path, worker_id);
    if (!info || !info->image_loader) return nullptr;

    const int maxThumb = std::max(m_config.width, m_config.height);

    if (info->is_video) {
        // VideoImageLoader takes the frame number as a string.
        return info->image_loader->loadThumbnail(
            std::to_string(key.source_frame), maxThumb);
    }

    // Image-seq path: source_path is the per-frame file (with
    // optional ?layer= suffix). Strip the suffix for the loader's
    // file open — the layer was already pushed into EXRImageLoader
    // via setLayer() in getOrCreateLoader.
    std::string filePath, layer;
    splitLayer(key.source_path, filePath, layer);
    return info->image_loader->loadThumbnail(filePath, maxThumb);
}

std::uint64_t TimelineThumbnailCache::createGpuTexture(
    const std::shared_ptr<PixelData> &pixels)
{
    if (!pixels || pixels->pixels.empty()) return 0;

#if defined(QCV_NATIVE_PLAYER) && defined(Q_OS_APPLE)
    auto &pool = MetalTexturePool::thumbnailInstance();
    if (!pool.isInitialized()) {
        pool.initialize(/*maxBytes=*/256ULL * 1024 * 1024);
    }
    return pool.createTextureFromPixels(
        pixels->width, pixels->height,
        poolFormatFor(pixels->pixel_format),
        pixels->pixels.data(), pixels->pixels.size());
#elif defined(QCV_NATIVE_PLAYER) && defined(Q_OS_WIN)
    auto &pool = D3D11TexturePool::thumbnailInstance();
    if (!pool.isInitialized()) {
        pool.initialize(/*maxBytes=*/256ULL * 1024 * 1024);
    }
    return pool.createTextureFromPixels(
        pixels->width, pixels->height,
        poolFormatFor(pixels->pixel_format),
        pixels->pixels.data(), pixels->pixels.size());
#else
    (void)pixels;
    return 0;
#endif
}

void TimelineThumbnailCache::evictLru()
{
    if (m_lruOrder.empty()) return;
    TimelineThumbnailKey k = m_lruOrder.back();
    m_lruOrder.pop_back();

    auto it = m_cache.find(k);
    if (it == m_cache.end()) return;
    if (it->second.texture_id != 0) {
#if defined(QCV_NATIVE_PLAYER) && defined(Q_OS_APPLE)
        MetalTexturePool::thumbnailInstance().queueDelete(
            it->second.texture_id);
#elif defined(QCV_NATIVE_PLAYER) && defined(Q_OS_WIN)
        D3D11TexturePool::thumbnailInstance().queueDelete(
            it->second.texture_id);
#endif
    }
    m_cache.erase(it);
}

int TimelineThumbnailCache::findNearestCachedFrame(
    const std::string &path, int target) const
{
    // Caller holds m_cacheMutex.
    if (m_cache.empty()) return -1;
    int best     = -1;
    int bestDist = INT_MAX;
    for (const auto &kv : m_cache) {
        if (kv.first.source_path != path) continue;
        const int d = std::abs(kv.first.source_frame - target);
        if (d < bestDist) {
            bestDist = d;
            best     = kv.first.source_frame;
        }
    }
    return best;
}

namespace {

// Parse a sequence-style path into (prefix, suffix, frameNumber).
// Returns false if the path doesn't have a digit-run before its
// extension. `?layer=...` (cache-key suffix for EXR multi-layer) is
// preserved as part of `suffix` so siblings with the same layer
// match and siblings with different layers don't.
//
// Examples:
//   /a/b/shot_v1_0042.exr
//     prefix="/a/b/shot_v1_", suffix=".exr", number=42
//   /a/b/shot_v1_0042.exr?layer=Beauty
//     prefix="/a/b/shot_v1_", suffix=".exr?layer=Beauty", number=42
struct SequenceParts {
    std::string prefix;
    std::string suffix;
    int         number = 0;
};
bool parseSequenceParts(const std::string &path,
                         SequenceParts &out)
{
    const std::size_t q = path.find("?layer=");
    const std::string filePath =
        (q == std::string::npos) ? path : path.substr(0, q);
    const std::string keySuf =
        (q == std::string::npos) ? std::string() : path.substr(q);

    const std::size_t dot = filePath.find_last_of('.');
    if (dot == std::string::npos) return false;
    const std::string ext  = filePath.substr(dot);
    const std::string stem = filePath.substr(0, dot);

    // Walk back through trailing digits.
    std::size_t end = stem.size();
    std::size_t pos = end;
    while (pos > 0
           && std::isdigit(static_cast<unsigned char>(stem[pos-1]))) {
        --pos;
    }
    if (pos == end) return false;     // no digits at end

    try {
        out.number = std::stoi(stem.substr(pos, end - pos));
    } catch (...) {
        return false;
    }
    out.prefix = stem.substr(0, pos);
    out.suffix = ext + keySuf;
    return true;
}

bool startsWith(const std::string &s, const std::string &p) {
    return s.size() >= p.size()
        && std::memcmp(s.data(), p.data(), p.size()) == 0;
}
bool endsWith(const std::string &s, const std::string &p) {
    return s.size() >= p.size()
        && std::memcmp(s.data() + (s.size() - p.size()),
                       p.data(), p.size()) == 0;
}

} // namespace

bool TimelineThumbnailCache::findNearestSequenceSibling(
    const std::string &targetPath,
    TimelineThumbnailKey &outKey) const
{
    // Caller holds m_cacheMutex.
    if (m_cache.empty()) return false;

    SequenceParts target;
    if (!parseSequenceParts(targetPath, target)) return false;

    int  bestDist  = INT_MAX;
    bool found     = false;
    for (const auto &kv : m_cache) {
        const std::string &p = kv.first.source_path;
        if (p == targetPath) continue;             // exact match handled
        if (!startsWith(p, target.prefix)) continue;
        if (!endsWith(p, target.suffix)) continue;

        // Extract the number between prefix and suffix.
        const std::size_t numStart = target.prefix.size();
        const std::size_t numEnd   = p.size() - target.suffix.size();
        if (numEnd <= numStart) continue;
        bool allDigits = true;
        for (std::size_t i = numStart; i < numEnd; ++i) {
            if (!std::isdigit(static_cast<unsigned char>(p[i]))) {
                allDigits = false;
                break;
            }
        }
        if (!allDigits) continue;

        int candidateNum = 0;
        try {
            candidateNum = std::stoi(
                p.substr(numStart, numEnd - numStart));
        } catch (...) {
            continue;
        }
        const int d = std::abs(candidateNum - target.number);
        if (d < bestDist) {
            bestDist = d;
            outKey   = kv.first;
            found    = true;
        }
    }
    return found;
}

void TimelineThumbnailCache::workerThread(int worker_id)
{
    qInfo("TimelineThumbnailCache: worker %d started", worker_id);

    while (m_running.load()) {
        Request req;
        bool haveRequest = false;

        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            m_queueCv.wait(lock, [this, worker_id]() {
                return !m_running.load()
                    || (!m_requestQueue.empty()
                        && shouldWorkerRun(worker_id));
            });
            if (!m_running.load()) break;

            // If playback started while we were waiting, requeue
            // current and CV-wait for the resume signal.
            if (!shouldWorkerRun(worker_id)) {
                requeueCurrentTask(worker_id);
                m_workerCv.wait(lock, [this, worker_id]() {
                    return !m_running.load()
                        || shouldWorkerRun(worker_id);
                });
                if (!m_running.load()) break;
                continue;
            }

            if (!m_requestQueue.empty()) {
                req = m_requestQueue.front();
                m_requestQueue.pop_front();
                m_queuedKeys.erase(req.key);
                m_inProgress.insert(req.key);
                haveRequest = true;
                std::lock_guard<std::mutex> tl(m_workerTaskMutex);
                m_workerCurrentTask[worker_id] = req.key;
            }
        }

        if (!haveRequest) continue;

        const std::uint64_t genBefore =
            m_clearGeneration.load(std::memory_order_acquire);

        auto pixels = loadThumbnailPixels(req.key, worker_id);

        // Drop result if a clear() raced through during the load.
        if (m_clearGeneration.load(std::memory_order_acquire)
            != genBefore) {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            m_inProgress.erase(req.key);
            continue;
        }

        if (pixels && m_running.load()) {
            // GPU upload happens on the worker (Metal API is
            // thread-safe for texture creation). Main thread will
            // pick the handle out of m_pendingUploads and insert
            // into m_cache.
            const std::uint64_t tex = createGpuTexture(pixels);
            if (tex != 0) {
                std::lock_guard<std::mutex> lock(m_uploadMutex);
                m_pendingUploads.push_back(
                    {req.key, nullptr, tex,
                     pixels->width, pixels->height});
            }
        }

        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            m_inProgress.erase(req.key);
        }
        {
            std::lock_guard<std::mutex> lock(m_workerTaskMutex);
            m_workerCurrentTask.erase(worker_id);
        }
    }
}

} // namespace qcv
