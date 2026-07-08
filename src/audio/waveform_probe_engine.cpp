#include "waveform_probe_engine.h"

#include "audio_chunk_reader.h"

#include <QElapsedTimer>
#include <QtLogging>

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <vector>

namespace qcv {

namespace {

// Finest slot = 5 ms — one pixel at the long-clip zoom ceiling
// (200 pps). Short clips zoomed beyond this show flat 5 ms steps;
// accepted for the experiment.
constexpr double kBaseSlotSec = 0.005;
constexpr int    kMaxRung     = 24;      // 5ms·2^24 ≈ 23 h — plenty

// Requests whose visible span fits under this decode linearly
// (sample-exact, no seeks). Above it, sparse 20 ms probes.
constexpr double kLinearSpanMaxSec = 60.0;
constexpr int    kSparseProbeFrames = 960;   // 20 ms @ 48 kHz

// Keep the peak hashes for at most this many sources (LRU) — the
// active media + dual B + a couple of recently-closed ones.
constexpr int kMaxSources = 4;

// Fold min/max over interleaved stereo f32.
inline void foldPeaks(const float *samples, std::size_t frames,
                      float &mn, float &mx)
{
    for (std::size_t i = 0; i < frames * 2; ++i) {
        const float s = samples[i];
        if (s < mn) mn = s;
        if (s > mx) mx = s;
    }
}

} // namespace

WaveformProbeEngine *WaveformProbeEngine::instance()
{
    static WaveformProbeEngine engine;
    return &engine;
}

WaveformProbeEngine::WaveformProbeEngine()
{
    m_thread = std::thread([this]() { workerLoop(); });
}

WaveformProbeEngine::~WaveformProbeEngine()
{
    m_quit.store(true);
    m_cv.notify_all();
    if (m_thread.joinable()) m_thread.join();
}

int WaveformProbeEngine::rungFor(double columnSpan) const
{
    if (columnSpan <= kBaseSlotSec) return 0;
    const int r = static_cast<int>(
        std::ceil(std::log2(columnSpan / kBaseSlotSec)));
    return std::clamp(r, 0, kMaxRung);
}

void WaveformProbeEngine::requestWindow(const QString &path, double t0,
                                        double t1, int columns)
{
    if (path.isEmpty() || columns <= 0 || t1 <= t0) return;
    {
        std::lock_guard<std::mutex> lock(m_mx);
        Request &req = m_pending[path];
        req.t0 = t0;
        req.t1 = t1;
        req.columns = columns;
        req.gen = ++m_genCounter;
    }
    m_cv.notify_all();
}

void WaveformProbeEngine::setPlaybackActive(bool active)
{
    m_paused.store(active);
    if (!active) m_cv.notify_all();
}

QVector<WaveformProbeEngine::ColumnPeak>
WaveformProbeEngine::sampleColumns(const QString &path, double t0,
                                   double t1, int columns)
{
    QVector<ColumnPeak> out(std::max(0, columns));
    if (columns <= 0 || t1 <= t0) return out;

    std::lock_guard<std::mutex> lock(m_mx);
    const auto srcIt = m_sources.constFind(path);
    if (srcIt == m_sources.constEnd()) return out;
    const RungMap &rungs = srcIt->rungs;

    const double columnSpan = (t1 - t0) / columns;
    const int rung = rungFor(columnSpan);

    for (int x = 0; x < columns; ++x) {
        const double tc = t0 + (x + 0.5) * columnSpan;
        if (tc < 0.0) continue;
        // Exact rung, then coarser (an estimate is better than a
        // hole while the fine pass fills), then slightly finer.
        for (int dr : {0, 1, 2, 3, 4, 5, 6, -1, -2}) {
            const int r = rung + dr;
            if (r < 0 || r > kMaxRung) continue;
            const auto rIt = rungs.constFind(r);
            if (rIt == rungs.constEnd()) continue;
            const double stride = kBaseSlotSec * std::pow(2.0, r);
            const auto sIt = rIt->constFind(
                static_cast<qint64>(std::floor(tc / stride)));
            if (sIt == rIt->constEnd()) continue;
            out[x].mn = sIt->first;
            out[x].mx = sIt->second;
            out[x].valid = true;
            break;
        }
    }
    return out;
}

void WaveformProbeEngine::workerLoop()
{
    // One persistent reader per source, kept open across requests —
    // container reopen costs real time on network volumes. Small
    // LRU; thread-confined to this worker (AudioChunkReader is
    // single-thread by contract).
    struct OpenReader {
        std::unique_ptr<AudioChunkReader> reader;
        quint64 touched = 0;
    };
    // std::map (not QHash): values hold a unique_ptr, and QHash
    // requires copyable value types.
    std::map<QString, OpenReader> readers;
    quint64 readerTouch = 0;

    std::vector<float> buf;
    QElapsedTimer emitTimer;
    emitTimer.start();

    while (true) {
        QString path;
        Request req;
        {
            std::unique_lock<std::mutex> lock(m_mx);
            m_cv.wait(lock, [this]() {
                return m_quit.load()
                    || (!m_pending.isEmpty() && !m_paused.load());
            });
            if (m_quit.load()) return;
            // Take any pending request (A and B strips at most).
            auto it = m_pending.begin();
            path = it.key();
            req = it.value();
            m_pending.erase(it);

            // Touch/create the source state + LRU-evict extras.
            SourceState &st = m_sources[path];
            st.touched = ++m_touchCounter;
            while (m_sources.size() > kMaxSources) {
                auto oldest = m_sources.begin();
                for (auto sit = m_sources.begin();
                     sit != m_sources.end(); ++sit) {
                    if (sit->touched < oldest->touched) oldest = sit;
                }
                m_sources.erase(oldest);
            }
        }

        // Reader for this path (open outside the lock — can block).
        OpenReader &entry = readers[path];
        entry.touched = ++readerTouch;
        while (readers.size() > 2) {
            auto oldest = readers.begin();
            for (auto rit = readers.begin(); rit != readers.end(); ++rit) {
                if (rit->second.touched < oldest->second.touched
                    && rit->first != path) {
                    oldest = rit;
                }
            }
            if (oldest->first == path) break;   // only survivors left
            readers.erase(oldest);
        }
        if (!entry.reader) {
            entry.reader = std::make_unique<AudioChunkReader>();
            if (!entry.reader->open(path, /*routingMode=*/0)) {
                qWarning("WaveformProbeEngine: no decodable audio in "
                         "'%s'", qPrintable(path));
                readers.erase(path);
                continue;
            }
        }
        AudioChunkReader *reader = entry.reader.get();

        // Probe plan: slots at the request's rung across [t0,t1].
        const double columnSpan = (req.t1 - req.t0) / req.columns;
        const int    rung   = rungFor(columnSpan);
        const double stride = kBaseSlotSec * std::pow(2.0, rung);
        const bool   linear = (req.t1 - req.t0) <= kLinearSpanMaxSec;
        const double srcDuration = reader->duration();

        const qint64 i0 = static_cast<qint64>(
            std::floor(std::max(0.0, req.t0) / stride));
        const qint64 i1 = static_cast<qint64>(
            std::floor(std::max(0.0, req.t1) / stride));

        // Plan the pass. Three tiers, all under one lock:
        //   1. FOLD-DOWN — a missing slot whose children at a finer
        //      rung are mostly probed is synthesized by min/max-
        //      combining them. Zero decode, zero network: zoom-OUT
        //      recalculates instantly from prior exploration.
        //   2. UNCOVERED — no data at this rung NOR any nearby
        //      coarser one: genuinely unexplored time. Probed FIRST
        //      (ascending, near-sequential IO) so work always goes
        //      where the strip is blank, not re-plowing old ground.
        //   3. REFINE — a coarser estimate already paints there
        //      (sampleColumns falls back up-rung); probed last.
        std::vector<qint64> uncovered;
        std::vector<qint64> refine;
        bool synthesized = false;
        {
            std::lock_guard<std::mutex> lock(m_mx);
            RungMap &rungs = m_sources[path].rungs;
            // ("slots" is a Qt macro — hence slotMap.)
            auto &slotMap = rungs[rung];
            for (qint64 i = i0; i <= i1; ++i) {
                if (i * stride >= srcDuration && srcDuration > 0.0)
                    break;
                if (slotMap.contains(i)) continue;

                // Tier 1: synthesize from children (1-3 rungs finer).
                bool made = false;
                for (int dr = 1; dr <= 3 && !made; ++dr) {
                    const auto fIt = rungs.constFind(rung - dr);
                    if (fIt == rungs.constEnd()) continue;
                    const qint64 kids  = qint64(1) << dr;
                    const qint64 first = i * kids;
                    float mn = 0.0f, mx = 0.0f;
                    qint64 found = 0;
                    for (qint64 k = 0; k < kids; ++k) {
                        const auto kIt = fIt->constFind(first + k);
                        if (kIt == fIt->constEnd()) continue;
                        mn = std::min(mn, kIt->first);
                        mx = std::max(mx, kIt->second);
                        ++found;
                    }
                    if (found * 2 >= kids) {   // ≥ half → good enough
                        slotMap.insert(i, qMakePair(mn, mx));
                        made = synthesized = true;
                    }
                }
                if (made) continue;

                // Tier 2 vs 3: any coarser slot covering this time?
                bool covered = false;
                for (int dr = 1; dr <= 6 && !covered; ++dr) {
                    const auto cIt = rungs.constFind(rung + dr);
                    if (cIt != rungs.constEnd()
                        && cIt->contains(i >> dr)) {
                        covered = true;
                    }
                }
                (covered ? refine : uncovered).push_back(i);
            }
        }
        if (synthesized) emit dataArrived(path);
        if (uncovered.empty() && refine.empty()) {
            emit dataArrived(path);
            continue;
        }

        const std::size_t strideFrames = static_cast<std::size_t>(
            std::max(1.0, std::round(stride * 48000.0)));
        const std::size_t probeFrames = linear
            ? strideFrames
            : std::min<std::size_t>(strideFrames, kSparseProbeFrames);
        buf.resize(probeFrames * 2);

        // Uncovered first, refinements after — each tier ascending.
        std::vector<qint64> plan = std::move(uncovered);
        plan.insert(plan.end(), refine.begin(), refine.end());

        bool aborted = false;
        for (const qint64 slotIdx : plan) {
            // Cancellation / pause / quit — EVERY probe. On slow
            // (network) sources a probe can take 100 ms+; the old
            // every-16 cadence made new vantages feel ignored.
            {
                std::lock_guard<std::mutex> lock(m_mx);
                if (m_quit.load()) return;
                const auto pIt = m_pending.constFind(path);
                if ((pIt != m_pending.constEnd() && pIt->gen != req.gen)
                    || m_paused.load()) {
                    aborted = true;   // newer vantage or playback —
                    break;            // requeued naturally
                }
            }

            const double t = slotIdx * stride;
            const std::size_t got =
                reader->readAt(t, buf.data(), probeFrames);

            float mn = 0.0f, mx = 0.0f;
            if (got > 0) foldPeaks(buf.data(), got, mn, mx);

            {
                std::lock_guard<std::mutex> lock(m_mx);
                m_sources[path].rungs[rung].insert(
                    slotIdx, qMakePair(mn, mx));
            }

            if (emitTimer.elapsed() > 80) {
                emitTimer.restart();
                emit dataArrived(path);
            }
        }

        emit dataArrived(path);
        if (aborted && m_paused.load()) {
            // Pause interrupted this vantage — re-pend it so probing
            // resumes where it left off when playback stops.
            std::lock_guard<std::mutex> lock(m_mx);
            if (!m_pending.contains(path)) m_pending.insert(path, req);
        }
    }
}

} // namespace qcv
