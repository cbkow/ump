// test_dual_decoder — Phase 7.7 Stage 1 verification harness.
//
// Standalone executable that exercises DualVideoDecoder's ring buffer
// + decode-ahead gate. Confirms two things:
//
//   1. After opening a video and setting decode_target=0, the decoder
//      fills its ring buffer >= kRingSize/2 frames ahead, then idles.
//   2. As decode_target advances at 30 Hz, the decoder keeps the
//      buffer half-full ahead. getBufferedFrame(target) returns a
//      valid frame consistently (no closest-frame fallback needed).
//
// Build with:
//   cmake --build build --target test_dual_decoder
//
// Run with:
//   build/src/dual/test_dual_decoder /path/to/video.mov
//
// Excluded from the default `cmake --build build` to keep release
// builds clean — opt-in target only.

#include "dual_video_decoder.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QString>
#include <QtLogging>

#include <chrono>
#include <thread>

using namespace qcv::dual;

namespace {

void logBufferState(const DualVideoDecoder &dec, int target, const char *label)
{
    int lo = -1, hi = -1;
    dec.getBufferedRange(lo, hi);
    qInfo("  [%s] target=%d  count=%d  ahead=%d  behind=%d  range=[%d..%d]  "
          "hasTarget=%d",
          label, target,
          dec.bufferCount(),
          dec.bufferedAhead(),
          dec.bufferedBehind(),
          lo, hi,
          dec.hasFrame(target) ? 1 : 0);
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    if (argc < 2) {
        qWarning("Usage: %s /path/to/video.mov", argv[0]);
        return 1;
    }

    const QString path = QString::fromUtf8(argv[1]);
    qInfo("DualVideoDecoder verification — opening %s",
          qPrintable(QFileInfo(path).fileName()));

    DualVideoDecoder dec;
    if (!dec.open(path)) {
        qWarning("Open failed");
        return 2;
    }

    qInfo("Opened: %dx%d @ %.2f fps, %d frames",
          dec.width(), dec.height(), dec.fps(), dec.frameCount());

    // ---- Phase 1: confirm decode-ahead fills past kRingSize/2.
    qInfo("\nPhase 1 — decode_target=0, waiting up to 2 s for ring to fill...");
    dec.setDecodeTarget(0);

    QElapsedTimer fillTimer;
    fillTimer.start();
    while (fillTimer.elapsed() < 2000) {
        if (dec.bufferedAhead() >= DualVideoDecoder::kRingSize / 2) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    logBufferState(dec, 0, "fill");

    if (dec.bufferedAhead() < DualVideoDecoder::kRingSize / 2) {
        qWarning("FAIL: ring did not reach half-full ahead within 2 s "
                 "(got %d, need >= %d)",
                 dec.bufferedAhead(), DualVideoDecoder::kRingSize / 2);
        dec.close();
        return 3;
    }
    qInfo("  PASS: ring has %d frames ahead of target=0",
          dec.bufferedAhead());

    // ---- Phase 2: advance target at 30 Hz, confirm getBufferedFrame
    //              keeps returning valid frames.
    qInfo("\nPhase 2 — advancing decode_target at 30 Hz for 2 s...");
    int hits = 0, misses = 0;
    const int maxFrame = std::max(0, dec.frameCount() - 1);
    QElapsedTimer playTimer;
    playTimer.start();
    int target = 0;
    while (playTimer.elapsed() < 2000) {
        dec.setDecodeTarget(target);
        std::this_thread::sleep_for(std::chrono::milliseconds(33));
        auto frame = dec.getBufferedFrame(target);
        if (frame && frame->valid()) {
            ++hits;
        } else {
            ++misses;
            qInfo("  miss at target=%d  ahead=%d  count=%d",
                  target, dec.bufferedAhead(), dec.bufferCount());
        }
        if (target < maxFrame) ++target;
    }

    qInfo("  result: %d hits, %d misses (%.1f%% hit rate)",
          hits, misses,
          (hits + misses > 0) ? 100.0 * hits / (hits + misses) : 0.0);

    if (misses > hits / 4) {
        qWarning("FAIL: more than 25%% misses — decode-ahead is not keeping "
                 "up with target advance");
        dec.close();
        return 4;
    }

    // ---- Phase 3: seek mid-file, verify buffer rebuilds.
    if (dec.frameCount() > 100) {
        qInfo("\nPhase 3 — seek to frame %d, verify ring rebuilds...",
              dec.frameCount() / 2);
        const int seekTarget = dec.frameCount() / 2;
        dec.seekTo(seekTarget);

        QElapsedTimer seekTimer;
        seekTimer.start();
        while (seekTimer.elapsed() < 2000) {
            if (dec.hasFrame(seekTarget) &&
                dec.bufferedAhead() >= DualVideoDecoder::kRingSize / 2) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        logBufferState(dec, seekTarget, "seek");
        if (!dec.hasFrame(seekTarget)) {
            qWarning("FAIL: seek target frame %d not in buffer after 2 s",
                     seekTarget);
            dec.close();
            return 5;
        }
        qInfo("  PASS: ring rebuilt around seek target");
    }

    qInfo("\nAll checks passed. Closing...");
    dec.close();
    qInfo("Closed cleanly.");
    return 0;
}
