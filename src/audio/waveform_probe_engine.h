// WaveformProbeEngine — lazy, screen-space audio peak sampling for
// the timeline waveform strip (EXPERIMENT, 2026-07-08).
//
// Design (user-driven, "recalculate per vantage"): the strip always
// shows ~viewport-width columns of whatever time window is visible.
// Nothing is scanned up front — the strip asks this engine for the
// visible window, the engine probes ONLY the audio needed to
// estimate those columns on a worker thread, and every probe is
// memoized so revisited vantages (pans, zoom-backs) are warm.
//
// Probe positions are quantized to a STRIDE LADDER: multiples of
// base·2^rung seconds, rung chosen so one probe ≈ one column. The
// power-of-two ladder is what makes the memo compose: coarse-rung
// positions are also fine-rung positions, so zooming in reuses every
// coarse probe, and paint-time lookups fall back to nearby coarser
// rungs while finer ones fill in (progressive sharpening).
//
// Two decode regimes per request (both via AudioChunkReader — the
// synchronous, thread-confined shuttle-grain decoder):
//   - visible span ≤ ~60 s: read every slot's full stride worth of
//     samples sequentially (forward-continuation = no seeks) —
//     sample-exact columns.
//   - longer spans: 20 ms probe at each slot start, issued in
//     ascending order so container seeks stay near-sequential
//     (matters on SMB volumes) — estimate columns. Transients
//     between probes are missed BY DESIGN; the zoomed-out strip is
//     a map, not evidence.
//
// Playback pause: probing suspends entirely while the transport
// runs (the strip feeds playbackActive through) — the same posture
// TimelineThumbnailCache takes, but stricter.

#pragma once

#include <QHash>
#include <QObject>
#include <QPair>
#include <QString>
#include <QVector>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace qcv {

class AudioChunkReader;

class WaveformProbeEngine : public QObject {
    Q_OBJECT
public:
    static WaveformProbeEngine *instance();

    // One painted column's worth of peak data. mn/mx are sample
    // values in [-1, 1]; valid=false → nothing probed there yet
    // (paint transparent).
    struct ColumnPeak {
        float mn = 0.0f;
        float mx = 0.0f;
        bool  valid = false;
    };

    // Ask the worker to cover [t0,t1] (source seconds) at `columns`
    // resolution. Coalesces: a newer request for the same path
    // replaces the pending one and cancels in-flight work at the
    // next check-point. Cheap to call per settle-tick.
    void requestWindow(const QString &path, double t0, double t1,
                       int columns);

    // Paint-side sampling (GUI thread, mutex-guarded hash reads —
    // microseconds for ~2000 columns). For each column: exact-rung
    // slot first, then nearby coarser rungs (progressive estimate),
    // then finer.
    QVector<ColumnPeak> sampleColumns(const QString &path, double t0,
                                      double t1, int columns);

    // True while the transport runs — worker suspends until cleared.
    void setPlaybackActive(bool active);

signals:
    // Throttled (~80 ms) progress ping from the worker; strips
    // repaint the affected path. Queued into the GUI thread by Qt.
    void dataArrived(const QString &path);

private:
    WaveformProbeEngine();
    ~WaveformProbeEngine() override;

    void workerLoop();
    int  rungFor(double columnSpan) const;

    struct Request {
        double  t0 = 0.0, t1 = 0.0;
        int     columns = 0;
        quint64 gen = 0;
    };
    // rung → (slot index → min/max). Slot i at rung r covers
    // [i·stride, (i+1)·stride), stride = kBaseSlotSec·2^r.
    using RungMap = QHash<int, QHash<qint64, QPair<float, float>>>;
    struct SourceState {
        RungMap rungs;
        quint64 touched = 0;   // LRU stamp
    };

    std::mutex              m_mx;
    std::condition_variable m_cv;
    QHash<QString, Request>     m_pending;
    QHash<QString, SourceState> m_sources;
    quint64 m_genCounter   = 0;
    quint64 m_touchCounter = 0;

    std::atomic<bool> m_paused{false};
    std::atomic<bool> m_quit{false};
    std::thread m_thread;
};

} // namespace qcv
