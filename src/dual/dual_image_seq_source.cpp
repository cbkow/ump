#include "dual_image_seq_source.h"

#include "decode/exr_image_loader.h"
#include "decode/image_loader.h"

#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QRegularExpression>
#include <QtLogging>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace qcv::dual {

namespace {

// Tick interval for the cache thread — same as old QCView's 10 ms.
constexpr int kCacheTickMs = 10;

// Image extensions DualImageSeqSource recognizes. PNG/JPEG/TIFF/BMP/GIF
// load via Qt's built-in QImageReader; EXR loads via qcv_decode's
// EXRImageLoader (RGBA16F → clamped RGBA8 for the dual ring's Cpu
// kind). Single-flow EXR path is unaffected.
bool looksLikeImageFile(const QString &path)
{
    static const QStringList kExts = {
        "png", "jpg", "jpeg", "tif", "tiff", "bmp", "gif", "exr"
    };
    const QString ext = QFileInfo(path).suffix().toLower();
    return kExts.contains(ext);
}

bool isExrFile(const QString &path)
{
    return QFileInfo(path).suffix().toLower() == QLatin1String("exr");
}

// Wrap a PixelData::RGBA16F buffer as a QImage with
// Format_RGBA16FPx4 — same byte layout (4 × half-float per pixel,
// 8 B/px). The QImage owns a copy so the source buffer's lifetime
// is independent. Avoids the precision loss of an 8-bit clamp; the
// dual compositor uploads as RGBA16Float and OCIO operates on the
// linear FP16 values directly.
QImage rgba16fToFloatQImage(const PixelData &pd)
{
    QImage out(pd.width, pd.height, QImage::Format_RGBA16FPx4);
    const std::size_t expected =
        static_cast<std::size_t>(pd.width) *
        static_cast<std::size_t>(pd.height) * 8u;
    if (pd.pixels.size() < expected) {
        qWarning("DualImageSeqSource: EXR pixel buffer smaller than "
                 "expected (%zu vs %zu)",
                 pd.pixels.size(), expected);
        return QImage();
    }
    const std::size_t srcRow = static_cast<std::size_t>(pd.width) * 8u;
    const std::size_t dstRow = static_cast<std::size_t>(out.bytesPerLine());
    const std::size_t copyRow = std::min(srcRow, dstRow);
    for (int y = 0; y < pd.height; ++y) {
        std::memcpy(out.scanLine(y),
                    pd.pixels.data() + static_cast<std::size_t>(y) * srcRow,
                    copyRow);
    }
    return out;
}

// Extract the trailing run of digits from a basename. Returns the
// numeric value or -1 if no digits found. Also returns the digit
// run's start/length so the caller can reconstruct the pattern.
//
// Examples (basename):
//   "frame.0001"     -> 1, start=6, len=4
//   "shot_0042_v01"  -> 1, start=10, len=2  (last digit run, NOT v01)
//      ^^ note: takes the LAST run only — matches typical sequence
//         naming where the version tag comes AFTER the frame number?
//         Actually no — typical naming has frame number AS the last
//         numeric run before extension. We take the LAST digit run.
int extractTrailingNumber(const QString &basename, int &startOut, int &lenOut)
{
    int end = basename.size() - 1;
    while (end >= 0 && !basename[end].isDigit()) --end;
    if (end < 0) {
        startOut = lenOut = 0;
        return -1;
    }
    int start = end;
    while (start > 0 && basename[start - 1].isDigit()) --start;
    startOut = start;
    lenOut   = end - start + 1;
    bool ok = false;
    int n = basename.mid(start, lenOut).toInt(&ok);
    return ok ? n : -1;
}

} // namespace

DualImageSeqSource::DualImageSeqSource() = default;

DualImageSeqSource::~DualImageSeqSource()
{
    close();
}

bool DualImageSeqSource::open(const QString &path)
{
    close();

    if (!QFileInfo::exists(path)) {
        qWarning("DualImageSeqSource::open: path not found: %s", qPrintable(path));
        return false;
    }

    m_path = path;
    if (!discoverSequence(path)) {
        m_path.clear();
        return false;
    }
    if (!probeFirstFrame()) {
        m_files.clear();
        m_path.clear();
        return false;
    }

    m_frameCount = m_files.size();
    m_decodeTarget.store(0, std::memory_order_release);
    m_pendingSeekTarget.store(-1, std::memory_order_release);
    m_stopRequested.store(false, std::memory_order_release);

    {
        std::lock_guard<std::mutex> lk(m_cacheMutex);
        m_cache.clear();
        m_lru.clear();
        m_lruPos.clear();
    }
    {
        std::lock_guard<std::mutex> lk(m_workMutex);
        m_workQueue.clear();
        m_inFlight.clear();
    }

    m_open.store(true, std::memory_order_release);

    const int workerCount = std::max(1, m_threadCount);
    m_workers.reserve(workerCount);
    for (int i = 0; i < workerCount; ++i) {
        m_workers.emplace_back([this] { ioWorkerLoop(); });
    }
    m_cacheThread = std::thread([this] { cacheLoop(); });

    qInfo("DualImageSeqSource: opened %s (%dx%d, %d frames, %d workers)",
          qPrintable(QFileInfo(path).fileName()),
          m_width, m_height, m_frameCount, workerCount);
    return true;
}

void DualImageSeqSource::close()
{
    if (!m_open.exchange(false, std::memory_order_acq_rel) &&
        !m_cacheThread.joinable() && m_workers.empty()) {
        return;
    }
    m_stopRequested.store(true, std::memory_order_release);

    {
        std::lock_guard<std::mutex> lk(m_cacheCvMutex);
    }
    m_cacheCv.notify_all();
    {
        std::lock_guard<std::mutex> lk(m_workMutex);
    }
    m_workCv.notify_all();

    if (m_cacheThread.joinable()) m_cacheThread.join();
    for (auto &w : m_workers) {
        if (w.joinable()) w.join();
    }
    m_workers.clear();

    {
        std::lock_guard<std::mutex> lk(m_cacheMutex);
        m_cache.clear();
        m_lru.clear();
        m_lruPos.clear();
    }
    {
        std::lock_guard<std::mutex> lk(m_workMutex);
        m_workQueue.clear();
        m_inFlight.clear();
    }

    m_files.clear();
    m_path.clear();
    m_width = m_height = m_frameCount = 0;
}

void DualImageSeqSource::setDecodeTarget(int frameNumber)
{
    if (!m_open.load(std::memory_order_acquire)) return;
    if (frameNumber < 0) frameNumber = 0;
    if (m_frameCount > 0 && frameNumber >= m_frameCount) {
        frameNumber = m_frameCount - 1;
    }
    const int prev = m_decodeTarget.exchange(frameNumber, std::memory_order_acq_rel);
    if (prev != frameNumber) {
        std::lock_guard<std::mutex> lk(m_cacheCvMutex);
        m_cacheCv.notify_one();
    }
}

void DualImageSeqSource::seekTo(int frameNumber)
{
    if (!m_open.load(std::memory_order_acquire)) return;
    if (frameNumber < 0) frameNumber = 0;
    if (m_frameCount > 0 && frameNumber >= m_frameCount) {
        frameNumber = m_frameCount - 1;
    }
    m_pendingSeekTarget.store(frameNumber, std::memory_order_release);
    m_decodeTarget.store(frameNumber, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lk(m_cacheCvMutex);
    }
    m_cacheCv.notify_one();
}

std::shared_ptr<DualFrame> DualImageSeqSource::getBufferedFrame(int frameNumber) const
{
    std::lock_guard<std::mutex> lk(m_cacheMutex);
    auto it = m_cache.find(frameNumber);
    if (it == m_cache.end()) return nullptr;
    return it->second;
}

std::shared_ptr<DualFrame> DualImageSeqSource::getClosestFrame(int frameNumber) const
{
    std::lock_guard<std::mutex> lk(m_cacheMutex);
    // Exact hit first (the common case at stride 1).
    auto exact = m_cache.find(frameNumber);
    if (exact != m_cache.end()) return exact->second;
    // Otherwise the nearest cached frame by absolute distance — lets a
    // sparse stride > 1 window show an approximate frame during fast
    // drags instead of going blank. Linear scan; resident count is
    // small (<= kCacheCapacityFrames).
    std::shared_ptr<DualFrame> best;
    int bestDist = m_frameCount + 1;   // larger than any in-range distance
    for (const auto &kv : m_cache) {
        const int d = std::abs(kv.first - frameNumber);
        if (d < bestDist) {
            bestDist = d;
            best     = kv.second;
        }
    }
    return best;
}

bool DualImageSeqSource::hasFrame(int frameNumber) const
{
    std::lock_guard<std::mutex> lk(m_cacheMutex);
    return m_cache.count(frameNumber) > 0;
}

int DualImageSeqSource::bufferedAhead() const
{
    // Returns COVERAGE — the forward distance to the FARTHEST cached
    // frame, not a raw count. The timeline cache strip draws an ahead
    // band this many frames wide; reporting coverage (span) instead of
    // count means a sparse cache-stride > 1 window still draws the full
    // reach instead of a band truncated to ~span/stride. At stride 1
    // (contiguous fill) coverage equals the old count, so unchanged.
    const int target = m_decodeTarget.load(std::memory_order_acquire);
    const int loIn   = m_loopIn.load(std::memory_order_acquire);
    const int loOut  = m_loopOut.load(std::memory_order_acquire);
    const bool hasRange = (loIn >= 0 && loOut > loIn);
    std::lock_guard<std::mutex> lk(m_cacheMutex);
    int cov = 0;
    if (hasRange) {
        // Loop-aware MODULAR forward distance so the strip's ahead band
        // reflects wrap-around prefetch (loop-start frames read as
        // "ahead via wrap" when target is near loOut). fwd > 0 excludes
        // the current frame, which sits AT the playhead.
        const int loopSize = loOut - loIn + 1;
        const int curIn = std::max(loIn, std::min(loOut, target)) - loIn;
        for (const auto &kv : m_cache) {
            const int k = kv.first;
            if (k < loIn || k > loOut) continue;
            const int kIn = k - loIn;
            const int fwd = ((kIn - curIn) % loopSize + loopSize) % loopSize;
            if (fwd > 0 && fwd <= kReadAheadFrames && fwd > cov) cov = fwd;
        }
        return cov;
    }
    for (const auto &kv : m_cache) {
        const int d = kv.first - target;
        if (d > 0 && d > cov) cov = d;   // farthest cached frame ahead
    }
    return cov;
}

int DualImageSeqSource::bufferedBehind() const
{
    // COVERAGE (see bufferedAhead): backward distance to the FARTHEST
    // cached frame. At stride > 1 the read-behind window is 0, so this
    // is naturally 0 (forward-only sparse caching).
    const int target = m_decodeTarget.load(std::memory_order_acquire);
    const int loIn   = m_loopIn.load(std::memory_order_acquire);
    const int loOut  = m_loopOut.load(std::memory_order_acquire);
    const bool hasRange = (loIn >= 0 && loOut > loIn);
    std::lock_guard<std::mutex> lk(m_cacheMutex);
    int cov = 0;
    if (hasRange) {
        const int loopSize = loOut - loIn + 1;
        const int curIn = std::max(loIn, std::min(loOut, target)) - loIn;
        for (const auto &kv : m_cache) {
            const int k = kv.first;
            if (k < loIn || k > loOut) continue;
            const int kIn = k - loIn;
            const int bwd = ((curIn - kIn) % loopSize + loopSize) % loopSize;
            if (bwd > 0 && bwd <= kReadBehindFrames && bwd > cov) cov = bwd;
        }
        return cov;
    }
    for (const auto &kv : m_cache) {
        const int d = target - kv.first;
        if (d > 0 && d > cov) cov = d;   // farthest cached frame behind
    }
    return cov;
}

int DualImageSeqSource::bufferCount() const
{
    std::lock_guard<std::mutex> lk(m_cacheMutex);
    return static_cast<int>(m_cache.size());
}

void DualImageSeqSource::getBufferedRange(int &startFrame, int &endFrame) const
{
    std::lock_guard<std::mutex> lk(m_cacheMutex);
    if (m_cache.empty()) {
        startFrame = endFrame = -1;
        return;
    }
    int lo = std::numeric_limits<int>::max();
    int hi = std::numeric_limits<int>::min();
    for (const auto &kv : m_cache) {
        lo = std::min(lo, kv.first);
        hi = std::max(hi, kv.first);
    }
    startFrame = lo;
    endFrame   = hi;
}

void DualImageSeqSource::setThreadCount(int n)
{
    if (m_open.load(std::memory_order_acquire)) {
        qWarning("DualImageSeqSource::setThreadCount: ignored — already open");
        return;
    }
    m_threadCount = std::clamp(n, 1, 32);
}

void DualImageSeqSource::setLayer(const QString &layer)
{
    if (m_layer == layer) return;
    m_layer = layer;
    // Mid-session layer changes flush the cache so the next frame
    // pull goes through the loader with the new layer name. Cheap
    // — workers refill on next decode tick.
    if (m_open.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lk(m_cacheMutex);
        m_cache.clear();
        m_lru.clear();
        m_lruPos.clear();
    }
}

void DualImageSeqSource::setLoopRange(int loIn, int hiOut)
{
    if (loIn < 0 || hiOut < loIn) {
        clearLoopRange();
        return;
    }
    m_loopIn.store(loIn, std::memory_order_release);
    m_loopOut.store(hiOut, std::memory_order_release);
    // Wake the cache thread so it can re-evict + re-prefetch
    // around the new loop window immediately rather than waiting
    // for the next setDecodeTarget tick.
    m_cacheCv.notify_all();
}

void DualImageSeqSource::clearLoopRange()
{
    m_loopIn.store(-1, std::memory_order_release);
    m_loopOut.store(-1, std::memory_order_release);
    m_cacheCv.notify_all();
}

void DualImageSeqSource::setCacheStride(int stride)
{
    stride = std::clamp(stride, 1, 4);
    if (stride == m_cacheStride.load(std::memory_order_acquire)) return;
    m_cacheStride.store(stride, std::memory_order_release);
    // Wake the cache thread to rebuild the prefetch window with the
    // new stride filter (and re-evict the now-off-grid frames).
    m_cacheCv.notify_all();
}

void DualImageSeqSource::setFps(double fps)
{
    if (fps > 0.0) m_fps = fps;
}

void DualImageSeqSource::setFrameAvailableCallback(FrameAvailableCallback cb)
{
    std::lock_guard<std::mutex> lk(m_callbackMutex);
    m_onFrameAvailable = std::move(cb);
}

bool DualImageSeqSource::discoverSequence(const QString &path)
{
    QFileInfo info(path);
    QDir directory;
    QString prefix;
    QString suffix;
    int     digitLen = 0;
    bool    havePattern = false;

    if (info.isDir()) {
        directory = QDir(path);
    } else if (info.isFile()) {
        directory = info.dir();
        const QString basename = info.completeBaseName();
        int start = 0, len = 0;
        if (extractTrailingNumber(basename, start, len) >= 0) {
            prefix   = basename.left(start);
            digitLen = len;
            suffix   = info.suffix();
            havePattern = true;
        }
    } else {
        qWarning("DualImageSeqSource::discoverSequence: not a file or dir: %s",
                 qPrintable(path));
        return false;
    }

    QStringList nameFilters;
    if (havePattern) {
        // Match only files with the same prefix + suffix as the dropped frame.
        // Use a glob: <prefix>*<suffix> — we'll filter further by digit count below.
        nameFilters << QString("%1*%2%3").arg(prefix,
            suffix.isEmpty() ? QString() : QStringLiteral("."),
            suffix);
    } else {
        for (const QString &ext : { "png", "PNG", "jpg", "JPG", "jpeg", "JPEG",
                                     "tif", "TIF", "tiff", "TIFF",
                                     "bmp", "BMP",
                                     "exr", "EXR" }) {
            nameFilters << QString("*.%1").arg(ext);
        }
    }

    QFileInfoList entries = directory.entryInfoList(
        nameFilters, QDir::Files | QDir::Readable, QDir::Name);

    // Build a (frame_number, path) list, then sort + index.
    struct Entry { int frame; QString path; };
    QList<Entry> frames;
    frames.reserve(entries.size());

    for (const QFileInfo &e : entries) {
        if (!looksLikeImageFile(e.fileName())) continue;
        int start = 0, len = 0;
        const int n = extractTrailingNumber(e.completeBaseName(), start, len);
        if (n < 0) continue;
        if (havePattern && len != digitLen) continue;
        frames.push_back({ n, e.absoluteFilePath() });
    }
    std::sort(frames.begin(), frames.end(),
              [](const Entry &a, const Entry &b) { return a.frame < b.frame; });

    if (frames.isEmpty()) {
        qWarning("DualImageSeqSource: no image frames in %s", qPrintable(path));
        return false;
    }

    // Index by 0..N-1 (drop original numeric values; caller treats it
    // as a contiguous sequence for the dual flow).
    m_files.clear();
    m_files.reserve(frames.size());
    for (const Entry &e : frames) {
        m_files.append(e.path);
    }
    return true;
}

bool DualImageSeqSource::probeFirstFrame()
{
    if (m_files.isEmpty()) return false;
    const QString first = m_files.first();

    // EXR — Qt's QImageReader doesn't recognize it; ask qcv_decode's
    // EXR loader for fast metadata-only dimensions.
    if (isExrFile(first)) {
        EXRImageLoader exr;
        int w = 0, h = 0;
        if (!exr.getDimensions(first.toStdString(), w, h)) {
            qWarning("DualImageSeqSource: EXR probe failed on %s",
                     qPrintable(first));
            return false;
        }
        m_width  = w;
        m_height = h;
        return true;
    }

    QImageReader reader(first);
    const QSize sz = reader.size();
    if (!sz.isValid()) {
        // Fall back to an actual load — some formats don't report size cheaply.
        QImage img;
        if (!img.load(first)) {
            qWarning("DualImageSeqSource: probe failed on %s",
                     qPrintable(first));
            return false;
        }
        m_width  = img.width();
        m_height = img.height();
    } else {
        m_width  = sz.width();
        m_height = sz.height();
    }
    return true;
}

int DualImageSeqSource::distanceFromTarget(int frame, int target) const
{
    return frame - target;   // signed; consumers branch on >= / <
}

std::shared_ptr<DualFrame> DualImageSeqSource::loadFrameBlocking(int frameNumber)
{
    if (frameNumber < 0 || frameNumber >= m_files.size()) return nullptr;
    const QString &filePath = m_files[frameNumber];

    QImage img;

    if (isExrFile(filePath)) {
        // EXR — load via qcv_decode's EXRImageLoader as RGBA16F and
        // wrap the linear FP16 pixels in a Format_RGBA16FPx4 QImage.
        // The dual compositor uploads as MTLPixelFormatRGBA16Float
        // and OCIO operates on the FP16 values directly — no 8-bit
        // clamp before the color transform.
        EXRImageLoader exr;
        auto pd = exr.loadFrame(filePath.toStdString(),
                                  m_layer.toStdString(),
                                  PipelineMode::ULTRA_HIGH_RES);
        if (!pd || pd->is_sentinel || pd->pixels.empty()) {
            qWarning("DualImageSeqSource: EXR load failed on frame %d (%s)",
                     frameNumber, qPrintable(filePath));
            return nullptr;
        }
        if (pd->pixel_format == PixelFormat::RGBA16F) {
            img = rgba16fToFloatQImage(*pd);
            if (img.isNull()) return nullptr;
        } else {
            qWarning("DualImageSeqSource: unexpected EXR pixel format %d",
                     static_cast<int>(pd->pixel_format));
            return nullptr;
        }
    } else {
        if (!img.load(filePath)) {
            qWarning("DualImageSeqSource: load failed on frame %d (%s)",
                     frameNumber, qPrintable(filePath));
            return nullptr;
        }
        if (img.format() != QImage::Format_RGBA8888) {
            img = img.convertToFormat(QImage::Format_RGBA8888);
        }
    }

    auto frame = std::make_shared<DualFrame>();
    frame->frameNumber = frameNumber;
    frame->width       = img.width();
    frame->height      = img.height();
    frame->kind        = DualFrame::Kind::Cpu;
    frame->rgba        = std::make_shared<QImage>(std::move(img));
    return frame;
}

void DualImageSeqSource::insertCacheLocked(int frameNumber,
                                            std::shared_ptr<DualFrame> frame)
{
    auto existing = m_cache.find(frameNumber);
    if (existing != m_cache.end()) {
        existing->second = std::move(frame);
        touchLruLocked(frameNumber);
        return;
    }

    if (static_cast<int>(m_cache.size()) >= kCacheCapacityFrames) {
        // Evict LRU front.
        if (!m_lru.empty()) {
            const int evict = m_lru.front();
            m_lru.pop_front();
            m_lruPos.erase(evict);
            m_cache.erase(evict);
        }
    }

    m_cache.emplace(frameNumber, std::move(frame));
    m_lru.push_back(frameNumber);
    auto it = m_lru.end();
    --it;
    m_lruPos[frameNumber] = it;
}

void DualImageSeqSource::touchLruLocked(int frameNumber)
{
    auto it = m_lruPos.find(frameNumber);
    if (it == m_lruPos.end()) return;
    m_lru.splice(m_lru.end(), m_lru, it->second);
    // splice keeps iterator valid; no need to re-store.
}

void DualImageSeqSource::evictOutsideWindowLocked(int target)
{
    // Loop-range path: keep frames near the start of the loop
    // alive when target is near the end (and vice versa) by using
    // modular distances over the loop length. Without this, the
    // forward-only window evicts loIn frames long before the wrap
    // and playback stalls on disk re-read at the loop point.
    const int loIn  = m_loopIn.load(std::memory_order_acquire);
    const int loOut = m_loopOut.load(std::memory_order_acquire);
    const bool hasRange = (loIn >= 0 && loOut >= loIn);
    const int  loopSize = hasRange ? (loOut - loIn + 1) : 0;
    // Stride > 1 collapses the read-behind window to 0 (forward-only),
    // matching ImageSequenceCache and the enqueue path above.
    const int stride     = std::max(1, m_cacheStride.load(std::memory_order_acquire));
    const int readBehind = (stride > 1) ? 0 : kReadBehindFrames;

    auto it = m_cache.begin();
    while (it != m_cache.end()) {
        const int k = it->first;
        bool evict = false;
        if (hasRange && loopSize > 0) {
            if (k < loIn || k > loOut) {
                evict = true;          // outside the loop zone
            } else {
                // Modular distance, mirroring single's
                // ImageSequenceCache. Clamp target into the loop
                // so out-of-zone playhead doesn't push the math.
                const int curIn = std::max(loIn,
                                  std::min(loOut, target)) - loIn;
                const int kIn   = k - loIn;
                const int fwd = ((kIn - curIn) % loopSize
                                 + loopSize) % loopSize;
                const int bwd = ((curIn - kIn) % loopSize
                                 + loopSize) % loopSize;
                evict = (bwd > readBehind
                         && fwd > kReadAheadFrames);
            }
        } else {
            // Linear (no-loop) eviction — the original behavior.
            const int loBound = target - readBehind;
            const int hiBound = target + kReadAheadFrames;
            evict = (k < loBound || k > hiBound);
        }
        if (evict) {
            auto pos = m_lruPos.find(k);
            if (pos != m_lruPos.end()) {
                m_lru.erase(pos->second);
                m_lruPos.erase(pos);
            }
            it = m_cache.erase(it);
        } else {
            ++it;
        }
    }
}

void DualImageSeqSource::enqueueMissingFramesLocked(int target)
{
    // Forward-first ordering — the renderer will ask for the current
    // frame next, then ahead. Only queue what's not already cached
    // and not in flight.
    std::lock_guard<std::mutex> wlk(m_workMutex);

    auto considerFrame = [&](int f) {
        if (f < 0 || f >= m_frameCount) return;
        if (m_cache.count(f) > 0) return;
        if (m_inFlight.count(f) > 0) return;
        // Already in queue?
        for (int q : m_workQueue) {
            if (q == f) return;
        }
        m_workQueue.push_back(f);
    };

    // When a loop range is set, wrap the offset frame numbers
    // through the loop so prefetch flows past the loop end into
    // the start. Without this the prefetcher tries to grab frames
    // beyond the sequence end (clamped to nothing) and never
    // pre-warms loIn before the wrap.
    const int loIn  = m_loopIn.load(std::memory_order_acquire);
    const int loOut = m_loopOut.load(std::memory_order_acquire);
    const bool hasRange = (loIn >= 0 && loOut >= loIn);
    const int  loopSize = hasRange ? (loOut - loIn + 1) : 0;

    auto wrapInLoop = [&](int f) -> int {
        if (!hasRange || loopSize <= 0) return f;
        if (f >= loIn && f <= loOut) return f;
        const int rel = ((f - loIn) % loopSize + loopSize) % loopSize;
        return loIn + rel;
    };

    // Cache-stride filter — at stride N > 1 only frames on the grid
    // (frameNumber % N == 0) are fetched, forward-only, for sparse
    // prefetch during fast drags. Mirrors ImageSequenceCache.
    const int stride = std::max(1, m_cacheStride.load(std::memory_order_acquire));
    auto onGrid = [stride](int absFrame) {
        return (stride <= 1) || (absFrame % stride == 0);
    };

    // Current frame first — only prime it at stride 1. At stride > 1
    // the exact target is often off-grid; the pull path shows the
    // nearest on-grid frame via getClosestFrame instead.
    if (stride <= 1) {
        considerFrame(target);
    }
    // Forward read-ahead — wraps to start when target is near end
    // of loop range. Skip off-grid frames at stride > 1.
    for (int d = 1; d <= kReadAheadFrames; ++d) {
        const int f = wrapInLoop(target + d);
        if (onGrid(f)) considerFrame(f);
    }
    // Backward read-behind — wraps to end when target is near start of
    // loop range. ONLY at stride 1; stride > 1 is forward-only so the
    // budget all goes to the sparse look-ahead.
    if (stride <= 1) {
        for (int d = 1; d <= kReadBehindFrames; ++d) {
            considerFrame(wrapInLoop(target - d));
        }
    }

    if (!m_workQueue.empty()) {
        m_workCv.notify_all();
    }
}

void DualImageSeqSource::cacheLoop()
{
    while (!m_stopRequested.load(std::memory_order_acquire)) {
        // Honor pending seek (just clears state — workers naturally
        // pick up new target on next pass).
        const int seekTarget = m_pendingSeekTarget.exchange(
            -1, std::memory_order_acq_rel);
        if (seekTarget >= 0) {
            std::lock_guard<std::mutex> lk(m_cacheMutex);
            // Hard seek: drop everything outside the new window.
            evictOutsideWindowLocked(seekTarget);
        }

        const int target = m_decodeTarget.load(std::memory_order_acquire);

        {
            std::lock_guard<std::mutex> lk(m_cacheMutex);
            evictOutsideWindowLocked(target);
            enqueueMissingFramesLocked(target);
        }

        // H.8 (2026-05-14) — prune the worker queue of items outside
        // the (loop-aware) modular window. Stale entries accumulate
        // when the playhead moves faster than workers can drain:
        // pre-wrap entries linger, get processed AFTER the wrap, and
        // the H.7 modular insert-check drops them. The wasted decode
        // time is what made the second loop pass stay transparent
        // until the cache caught up. Items already in flight aren't
        // here — they live in m_inFlight; their results are filtered
        // at insert time anyway.
        {
            std::lock_guard<std::mutex> wlk(m_workMutex);
            const int loIn   = m_loopIn.load(std::memory_order_acquire);
            const int loOut  = m_loopOut.load(std::memory_order_acquire);
            const bool hasRange = (loIn >= 0 && loOut >= loIn);
            auto isInWindow = [&](int f) -> bool {
                if (f < 0 || f >= m_frameCount) return false;
                if (hasRange && loOut > loIn) {
                    const int loopSize = loOut - loIn + 1;
                    if (f < loIn || f > loOut) return false;
                    const int curIn = std::max(loIn,
                                      std::min(loOut, target)) - loIn;
                    const int kIn   = f - loIn;
                    const int fwd = ((kIn - curIn) % loopSize
                                     + loopSize) % loopSize;
                    const int bwd = ((curIn - kIn) % loopSize
                                     + loopSize) % loopSize;
                    return (fwd <= kReadAheadFrames) ||
                           (bwd <= kReadBehindFrames);
                }
                return (f >= target - kReadBehindFrames &&
                        f <= target + kReadAheadFrames);
            };
            auto newEnd = std::remove_if(
                m_workQueue.begin(), m_workQueue.end(),
                [&](int f) { return !isInWindow(f); });
            m_workQueue.erase(newEnd, m_workQueue.end());
        }

        std::unique_lock<std::mutex> lk(m_cacheCvMutex);
        m_cacheCv.wait_for(lk, std::chrono::milliseconds(kCacheTickMs),
            [this] {
                return m_stopRequested.load(std::memory_order_acquire) ||
                       m_pendingSeekTarget.load(std::memory_order_acquire) >= 0;
            });
    }
}

void DualImageSeqSource::ioWorkerLoop()
{
    while (!m_stopRequested.load(std::memory_order_acquire)) {
        int frame = -1;
        {
            std::unique_lock<std::mutex> lk(m_workMutex);
            m_workCv.wait(lk, [this] {
                return m_stopRequested.load(std::memory_order_acquire) ||
                       !m_workQueue.empty();
            });
            if (m_stopRequested.load(std::memory_order_acquire)) return;
            if (m_workQueue.empty()) continue;
            frame = m_workQueue.front();
            m_workQueue.pop_front();
            m_inFlight.insert(frame);
        }

        // Load outside the lock — disk I/O + decode.
        std::shared_ptr<DualFrame> loaded = loadFrameBlocking(frame);

        // Stash in cache, drop in-flight marker.
        bool inserted = false;
        if (loaded) {
            std::lock_guard<std::mutex> clk(m_cacheMutex);
            // Bail if we've drifted way out of the window since we started.
            // H.7 (2026-05-14) — match the eviction logic's loop-aware
            // modular distance. Without this, a worker that was queued
            // BEFORE a loop wrap via wrapInLoop (e.g. frame 0 enqueued
            // when target was near loopOut) finishes AFTER the wrap and
            // the linear-window check drops the just-loaded frame.
            // Result: blank frames after every wrap until the cache
            // thread re-enqueues + workers re-load (the lag the user
            // worked around by stop+wait+play).
            const int target = m_decodeTarget.load(std::memory_order_acquire);
            const int loIn   = m_loopIn.load(std::memory_order_acquire);
            const int loOut  = m_loopOut.load(std::memory_order_acquire);
            const bool hasRange = (loIn >= 0 && loOut >= loIn);
            bool inWindow;
            if (hasRange && loOut > loIn) {
                const int loopSize = loOut - loIn + 1;
                const int curIn = std::max(loIn,
                                  std::min(loOut, target)) - loIn;
                const int kIn   = frame - loIn;
                if (frame < loIn || frame > loOut) {
                    inWindow = false;
                } else {
                    const int fwd = ((kIn - curIn) % loopSize
                                     + loopSize) % loopSize;
                    const int bwd = ((curIn - kIn) % loopSize
                                     + loopSize) % loopSize;
                    inWindow = (fwd <= kReadAheadFrames) ||
                               (bwd <= kReadBehindFrames);
                }
            } else {
                inWindow = (frame >= target - kReadBehindFrames &&
                            frame <= target + kReadAheadFrames);
            }
            if (inWindow) {
                insertCacheLocked(frame, std::move(loaded));
                inserted = true;
            }
        }
        {
            std::lock_guard<std::mutex> wlk(m_workMutex);
            m_inFlight.erase(frame);
        }
        // H.4 — wake the renderer outside the cache + work mutexes
        // so the callback (which may call into render-thread wake
        // primitives) can't deadlock against this thread's locks.
        if (inserted) {
            FrameAvailableCallback cb;
            {
                std::lock_guard<std::mutex> lk(m_callbackMutex);
                cb = m_onFrameAvailable;
            }
            if (cb) cb();
        }
    }
}

} // namespace qcv::dual
