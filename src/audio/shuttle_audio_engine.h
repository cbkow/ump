// ShuttleAudioEngine — grain scheduler + varispeed player for the
// FF/RW hold gesture (A/J = rewind, D/L = fast-forward).
//
// The gesture integrates a position at 2x→32x on a 33 ms QTimer and
// issues per-tick seeks; video chases by sampling. Audio gets this
// engine instead: a grain thread follows the integrated position,
// decoding short chunks FORWARD from a synchronous AudioChunkReader,
// varispeed-resampling them by the current signed speed (pitch
// follows speed — the tape/deck sound), and feeding a small ring the
// device callback drains. Direction only affects where the next
// grain comes from, so RW needs no backward-decode machinery.
//
// Grain geometry: each grain produces kGrainOutFrames (80 ms) from
// kGrainOutFrames × |speed| source frames — contiguous varispeed
// segments. Forward grains chain through the reader's continuation
// fast path AND through the resampler's carried phase, so FF is one
// continuous pitched stream with no crossfades. Reverse grains are
// individually reversed (below kReverseGrainMaxSpeed) and edged with
// short fades to kill boundary clicks.
//
// Re-sync: the grain cursor free-runs; each grain it compares against
// the transport's target and snaps only when the divergence exceeds
// max(0.25 s, 0.25 × |speed|) — decoupling grain cadence from the
// 33 ms tick and absorbing tick jitter. A grain that can't decode in
// time is skipped (ring underrun = silence), never allowed to lag
// the picture.
//
// Threads: transport (UI) calls begin/updateTarget/setInGap/end; the
// device render callback calls read(); the internal grain thread does
// all decoding. One engine per output side (AudioPlayer owns 1,
// DualAudioMixer owns 2).

#pragma once

#include "audio_ring_buffer.h"
#include "fractional_resampler.h"

#include <QString>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace qcv {

class AudioChunkReader;

class ShuttleAudioEngine
{
public:
    ShuttleAudioEngine();
    ~ShuttleAudioEngine();

    ShuttleAudioEngine(const ShuttleAudioEngine &) = delete;
    ShuttleAudioEngine &operator=(const ShuttleAudioEngine &) = delete;

    // Start the grain thread following (path, startSrcSec) at
    // signedSpeed. Empty path = silent side until updateTarget hands
    // over a real one. Idempotent-ish: calling begin while active
    // re-targets without tearing the thread down.
    void begin(const QString &path, double startSrcSec,
               double signedSpeed, int routingMode);

    // Per-gesture-tick update. Path changes (playlist boundary
    // crossings) are handed to the grain thread via generation bump;
    // the reader LRU keeps the previous file's reader warm.
    void updateTarget(const QString &path, double srcSec,
                      double signedSpeed);

    // Dual: this side has no clip under the master playhead. Grain
    // production pauses (silence) but the cursor keeps tracking the
    // target so clip re-entry is seamless.
    void setInGap(bool inGap);

    // Stop + join the grain thread, clear the ring.
    void end();

    bool active() const { return m_active.load(); }

    // Device render callback: drain up to `frames` stereo f32 frames.
    // Returns frames delivered; caller handles/pads the remainder
    // (or use the padded semantics below which memset the tail).
    std::size_t read(float *out, std::size_t frames);

private:
    void grainThreadFn();

    // ---- Transport-side state (atomics / mutex) ----
    std::atomic<bool>     m_running{false};
    std::atomic<bool>     m_active{false};
    std::atomic<bool>     m_inGap{false};
    std::atomic<double>   m_targetSrcSec{0.0};
    std::atomic<double>   m_signedSpeed{1.0};
    // Wall time (steady ms) of the last updateTarget that actually
    // moved the target — hold detection for drag-scrub (a stationary
    // mouse stops producing move events) and edge-clamped shuttles.
    std::atomic<int64_t>  m_lastTargetMoveMs{0};
    std::mutex            m_reqMutex;      // guards the two below
    QString               m_reqPath;
    int                   m_reqRouting = 0;
    std::atomic<uint64_t> m_reqGeneration{0};

    std::thread           m_thread;

    // ---- Callback-drained output ----
    // ~170 ms at 48 kHz stereo f32; grain thread back-pressures at
    // ~120 ms so speed changes stay audible within ~a grain.
    AudioRingBuffer       m_ring{65536};

    // ---- Grain-thread-only state ----
    // Reader LRU of 2, keyed by path — survives playlist boundary
    // crossings without re-opening the file the gesture just left.
    struct ReaderSlot {
        QString path;
        int     routing = 0;
        std::unique_ptr<AudioChunkReader> reader;
        uint64_t lastUse = 0;
    };
    ReaderSlot            m_slots[2];
    uint64_t              m_useCounter = 0;
    FractionalResampler   m_resampler;
    std::vector<float>    m_srcBuf;
    std::vector<float>    m_outBuf;
};

} // namespace qcv
