// test_dual_image_seq — Phase 7.7 Stage 2 verification harness.
//
// Standalone executable that exercises DualImageSeqSource's cache +
// I/O worker pool. Confirms three things:
//
//   1. Sequence discovery: opens a directory or any frame in a
//      sequence and counts the frames.
//   2. Cache fill: setting decode_target=0 populates the cache out
//      to the read-ahead window within a bounded time budget.
//   3. Continuous-play hit rate: advancing decode_target at 30 Hz
//      keeps the hit rate high (workers refill ahead of the playhead).
//   4. Adaptive thread count: setThreadCount(2) before open() halves
//      the worker pool (verified via log message).
//
// Build:  cmake --build build --target test_dual_image_seq
// Run:    build/src/dual/test_dual_image_seq /path/to/sequence/dir
//   Or:   build/src/dual/test_dual_image_seq /path/to/frame.0001.png

#include "dual_image_seq_source.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QString>
#include <QtLogging>

#include <chrono>
#include <thread>

using namespace qcv::dual;

namespace {

void logBufferState(const DualImageSeqSource &src, int target, const char *label)
{
    int lo = -1, hi = -1;
    src.getBufferedRange(lo, hi);
    qInfo("  [%s] target=%d  count=%d  ahead=%d  behind=%d  range=[%d..%d]  "
          "hasTarget=%d",
          label, target,
          src.bufferCount(),
          src.bufferedAhead(),
          src.bufferedBehind(),
          lo, hi,
          src.hasFrame(target) ? 1 : 0);
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    if (argc < 2) {
        qWarning("Usage: %s /path/to/sequence/dir|frame", argv[0]);
        qWarning("Optional: %s <path> --threads N", argv[0]);
        return 1;
    }

    const QString path = QString::fromUtf8(argv[1]);
    int threadCount = DualImageSeqSource::kDefaultThreadCount;
    if (argc >= 4 && QString::fromUtf8(argv[2]) == QStringLiteral("--threads")) {
        threadCount = QString::fromUtf8(argv[3]).toInt();
        if (threadCount <= 0) threadCount = DualImageSeqSource::kDefaultThreadCount;
    }

    qInfo("DualImageSeqSource verification — opening %s (threads=%d)",
          qPrintable(QFileInfo(path).fileName()), threadCount);

    DualImageSeqSource src;
    src.setThreadCount(threadCount);
    if (!src.open(path)) {
        qWarning("Open failed");
        return 2;
    }

    qInfo("Opened: %dx%d, %d frames",
          src.width(), src.height(), src.frameCount());

    if (src.frameCount() < 4) {
        qWarning("Sequence too short for verification (need >= 4 frames; "
                 "got %d)", src.frameCount());
        src.close();
        return 3;
    }

    // ---- Phase 1: cache fill from cold start.
    qInfo("\nPhase 1 — decode_target=0, waiting up to 5 s for cache to fill...");
    src.setDecodeTarget(0);

    QElapsedTimer fillTimer;
    fillTimer.start();
    const int wantAhead = std::min(DualImageSeqSource::kReadAheadFrames,
                                    src.frameCount() - 1);
    while (fillTimer.elapsed() < 5000) {
        if (src.bufferedAhead() >= wantAhead / 2) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    logBufferState(src, 0, "fill");

    if (src.bufferedAhead() < wantAhead / 2) {
        qWarning("FAIL: cache did not reach %d ahead within 5 s (got %d)",
                 wantAhead / 2, src.bufferedAhead());
        src.close();
        return 4;
    }
    qInfo("  PASS: cache has %d frames ahead of target=0",
          src.bufferedAhead());

    // ---- Phase 2: 30 Hz target advance.
    qInfo("\nPhase 2 — advancing decode_target at 30 Hz for 2 s...");
    int hits = 0, misses = 0;
    const int maxFrame = src.frameCount() - 1;
    QElapsedTimer playTimer;
    playTimer.start();
    int target = 0;
    while (playTimer.elapsed() < 2000) {
        src.setDecodeTarget(target);
        std::this_thread::sleep_for(std::chrono::milliseconds(33));
        auto frame = src.getBufferedFrame(target);
        if (frame && frame->valid()) {
            ++hits;
        } else {
            ++misses;
            if (misses < 6) {
                qInfo("  miss at target=%d  ahead=%d  count=%d",
                      target, src.bufferedAhead(), src.bufferCount());
            }
        }
        if (target < maxFrame) ++target;
        else break;
    }

    qInfo("  result: %d hits, %d misses (%.1f%% hit rate)",
          hits, misses,
          (hits + misses > 0) ? 100.0 * hits / (hits + misses) : 0.0);

    if (misses > hits / 4) {
        qWarning("FAIL: more than 25%% misses — cache + workers can't keep up");
        src.close();
        return 5;
    }

    // ---- Phase 3: seek mid-sequence, verify cache rebuilds.
    if (src.frameCount() > 16) {
        const int seekTarget = src.frameCount() / 2;
        qInfo("\nPhase 3 — seek to frame %d, verify cache rebuilds...",
              seekTarget);
        src.seekTo(seekTarget);

        QElapsedTimer seekTimer;
        seekTimer.start();
        while (seekTimer.elapsed() < 5000) {
            if (src.hasFrame(seekTarget)) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        logBufferState(src, seekTarget, "seek");
        if (!src.hasFrame(seekTarget)) {
            qWarning("FAIL: seek target frame %d not in cache after 5 s",
                     seekTarget);
            src.close();
            return 6;
        }
        qInfo("  PASS: cache rebuilt around seek target");
    }

    qInfo("\nAll checks passed. Closing...");
    src.close();
    qInfo("Closed cleanly.");
    return 0;
}
