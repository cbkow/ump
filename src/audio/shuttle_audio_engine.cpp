#include "shuttle_audio_engine.h"

#include "audio_chunk_reader.h"

#include <QtLogging>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace qcv {

namespace {
constexpr int         kOutRate           = 48000;
constexpr std::size_t kGrainOutFrames    = 3840;   // 80 ms per grain
constexpr std::size_t kEdgeFadeFrames    = 240;    // 5 ms grain-edge fade
// Back-pressure: hold ~40 ms buffered. Queued audio is committed at
// the speed it was produced with, so queue depth IS the speed-change
// latency; with the 80 ms grain the queue sawtooths 40–120 ms.
constexpr std::size_t kMaxBufferedBytes  = 15360;
// Post-flush fade-in (render callback): a direction-flip flush cuts
// the output mid-waveform; ramp the next ~2.7 ms in to soften it.
constexpr std::size_t kFlushFadeFrames   = 128;
// Varispeed clamp for the GRAIN math (pitch + source span). Below 1x
// pitch follows speed — a slow drag-scrub plays deeply pitched-down
// grains (the tape sound, useful for finding a beat or a word by
// ear; Pro Tools-style). Above 1x pitch does NOT rise: grains keep
// playing at natural pitch while the transport target runs ahead at
// the real speed, and the snap rule below jumps the cursor forward —
// fast shuttles/drags become natural-pitch fragments sampled sparsely
// along the path (FCP-skim / Avid digital-scrub sound). The previous
// 4x cap read as a constant squeal: at fit-to-width zoom nearly any
// drag exceeds 4x real time, so every gesture pinned the cap.
constexpr double      kMinSpeed  = 0.05;
constexpr double      kPitchCap  = 1.0;   // natural pitch is the ceiling
// Hold detection: below this |speed|, or when the transport target
// hasn't moved for the timeout, the engine idles silent (cursor
// keeps tracking). Covers a stationary drag-scrub mouse (no move
// events arrive to decay the velocity estimate) and a shuttle
// clamped at the media edge.
constexpr double      kHoldSpeedThreshold = 0.05;
constexpr int64_t     kHoldTimeoutMs      = 120;
// Target moves smaller than this don't count as "movement" (jitter
// around a held position).
constexpr double      kTargetMoveEpsilon  = 0.004;

int64_t nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
}

std::atomic<bool> ShuttleAudioEngine::s_globalMute{false};

void ShuttleAudioEngine::setGlobalMute(bool muted)
{
    s_globalMute.store(muted, std::memory_order_relaxed);
}

ShuttleAudioEngine::ShuttleAudioEngine() = default;

ShuttleAudioEngine::~ShuttleAudioEngine() { end(); }

void ShuttleAudioEngine::begin(const QString &path, double startSrcSec,
                               double signedSpeed, int routingMode)
{
    {
        std::lock_guard<std::mutex> lock(m_reqMutex);
        m_reqPath    = path;
        m_reqRouting = routingMode;
    }
    m_reqGeneration.fetch_add(1, std::memory_order_release);
    m_targetSrcSec.store(startSrcSec, std::memory_order_relaxed);
    const double prevSpeed =
        m_signedSpeed.exchange(signedSpeed, std::memory_order_relaxed);
    // Direction flip while live (J→L etc. re-begins without a thread
    // teardown): the queued audio is wrong-way — flush it.
    if (prevSpeed * signedSpeed < 0.0) {
        m_flushRing.store(true, std::memory_order_release);
    }
    m_inGap.store(false, std::memory_order_relaxed);
    m_lastTargetMoveMs.store(nowMs(), std::memory_order_relaxed);

    if (!m_running.load()) {
        m_ring.clear();
        m_flushRing.store(false, std::memory_order_relaxed);
        m_postFlushFadeLeft = 0;
        m_running.store(true);
        m_active.store(true);
        m_thread = std::thread(&ShuttleAudioEngine::grainThreadFn, this);
    }
}

void ShuttleAudioEngine::updateTarget(const QString &path, double srcSec,
                                      double signedSpeed)
{
    if (!m_running.load()) return;
    const double prevTarget =
        m_targetSrcSec.exchange(srcSec, std::memory_order_relaxed);
    if (std::abs(srcSec - prevTarget) > kTargetMoveEpsilon) {
        m_lastTargetMoveMs.store(nowMs(), std::memory_order_relaxed);
    }
    const double prevSpeed =
        m_signedSpeed.exchange(signedSpeed, std::memory_order_relaxed);
    // Direction flip (drag reversal, shuttle J↔L): everything queued
    // in the ring is wrong-way audio — have the render callback
    // discard it rather than play up to ~120 ms of it.
    if (prevSpeed * signedSpeed < 0.0) {
        m_flushRing.store(true, std::memory_order_release);
    }
    // Path handoff only when it actually changed (playlist boundary).
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(m_reqMutex);
        if (path != m_reqPath) {
            m_reqPath = path;
            changed = true;
        }
    }
    if (changed) m_reqGeneration.fetch_add(1, std::memory_order_release);
}

void ShuttleAudioEngine::setInGap(bool inGap)
{
    m_inGap.store(inGap, std::memory_order_relaxed);
}

void ShuttleAudioEngine::end()
{
    if (m_running.exchange(false)) {
        if (m_thread.joinable()) m_thread.join();
    }
    m_active.store(false);
    m_ring.clear();
    // Release readers so files aren't held open between gestures.
    for (auto &slot : m_slots) {
        slot.reader.reset();
        slot.path.clear();
    }
}

std::size_t ShuttleAudioEngine::read(float *out, std::size_t frames)
{
    if (!out || frames == 0) return 0;
    if (!m_active.load()) {
        std::memset(out, 0, frames * 2 * sizeof(float));
        return 0;
    }
    // Direction flip: everything queued is wrong-way audio. Discard
    // it from the consumer side (SPSC-safe — see discardAll) and
    // fade the next delivered samples in to soften the cut.
    if (m_flushRing.exchange(false, std::memory_order_acquire)) {
        m_ring.discardAll();
        m_postFlushFadeLeft = kFlushFadeFrames;
    }
    const std::size_t bytesWanted = frames * 2 * sizeof(float);
    const std::size_t bytesRead   = m_ring.read(out, bytesWanted);
    const std::size_t framesRead  = bytesRead / (2 * sizeof(float));
    if (m_postFlushFadeLeft > 0 && framesRead > 0) {
        const std::size_t n = std::min(m_postFlushFadeLeft, framesRead);
        for (std::size_t i = 0; i < n; ++i) {
            const float g = static_cast<float>(
                kFlushFadeFrames - m_postFlushFadeLeft + i)
                / static_cast<float>(kFlushFadeFrames);
            out[i * 2 + 0] *= g;
            out[i * 2 + 1] *= g;
        }
        m_postFlushFadeLeft -= n;
    }
    if (framesRead < frames) {
        std::memset(out + framesRead * 2, 0,
                    (frames - framesRead) * 2 * sizeof(float));
    }
    return framesRead;
}

void ShuttleAudioEngine::grainThreadFn()
{
    uint64_t localGen = 0;
    AudioChunkReader *reader = nullptr;
    // Grain cursor free-runs in source seconds; snaps to the
    // transport target when divergence exceeds the threshold.
    double cursor = m_targetSrcSec.load(std::memory_order_relaxed);
    bool needFadeIn = true;   // fade the first grain in after any reset
    int prevDir = 0;          // last grain's direction (0 = none yet)

    while (m_running.load(std::memory_order_relaxed)) {
        // ---- Source handoff (path change / first grain) ----
        const uint64_t gen =
            m_reqGeneration.load(std::memory_order_acquire);
        if (gen != localGen) {
            localGen = gen;
            QString path;
            int routing = 0;
            {
                std::lock_guard<std::mutex> lock(m_reqMutex);
                path    = m_reqPath;
                routing = m_reqRouting;
            }
            reader = nullptr;
            if (!path.isEmpty()) {
                // LRU of 2: reuse a warm reader, else evict the
                // least-recently-used slot.
                ReaderSlot *slot = nullptr;
                for (auto &s : m_slots) {
                    if (s.path == path && s.routing == routing
                        && s.reader && s.reader->isOpen()) {
                        slot = &s;
                        break;
                    }
                }
                if (!slot) {
                    slot = &m_slots[0];
                    for (auto &s : m_slots) {
                        if (s.lastUse < slot->lastUse) slot = &s;
                    }
                    slot->reader = std::make_unique<AudioChunkReader>();
                    slot->path.clear();
                    if (slot->reader->open(path, routing)) {
                        slot->path    = path;
                        slot->routing = routing;
                    } else {
                        slot->reader.reset();
                        slot = nullptr;
                    }
                }
                if (slot) {
                    slot->lastUse = ++m_useCounter;
                    reader = slot->reader.get();
                }
            }
            cursor = m_targetSrcSec.load(std::memory_order_relaxed);
            m_resampler.reset();
            needFadeIn = true;
        }

        // ---- Idle states: muted, gap side, no source, held still ----
        // Hold = |speed| under the threshold OR the target hasn't
        // moved for kHoldTimeoutMs (stationary drag-scrub mouse,
        // shuttle clamped at the media edge). Cursor keeps tracking
        // so resuming motion is seamless. The global mute (Settings
        // "Mute scrub audio") rides the same path: gesture machinery
        // untouched, grains just never produced.
        const bool held =
            std::abs(m_signedSpeed.load(std::memory_order_relaxed))
                < kHoldSpeedThreshold
            || (nowMs() - m_lastTargetMoveMs.load(std::memory_order_relaxed))
                > kHoldTimeoutMs;
        if (s_globalMute.load(std::memory_order_relaxed)
            || m_inGap.load(std::memory_order_relaxed) || !reader || held) {
            cursor = m_targetSrcSec.load(std::memory_order_relaxed);
            needFadeIn = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        // ---- Back-pressure ----
        if (m_ring.availableRead() > kMaxBufferedBytes) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }

        const double signedSpeed =
            m_signedSpeed.load(std::memory_order_relaxed);
        // Pitch-capped grain speed: above kPitchCap the transport
        // target outruns the cursor and the snap rule turns playback
        // into sparse natural-pitch snippets (see kPitchCap comment).
        const double aspeed =
            std::clamp(std::abs(signedSpeed), kMinSpeed, kPitchCap);
        const int dir = (signedSpeed < 0.0) ? -1 : 1;

        // Direction flip: the render callback flushes the queued
        // wrong-way audio; re-anchor on the live target so the first
        // new grain comes from where the user turned, not wherever
        // the old direction had run ahead to.
        if (prevDir != 0 && dir != prevDir) {
            cursor = m_targetSrcSec.load(std::memory_order_relaxed);
            m_resampler.reset();
            needFadeIn = true;
        }
        prevDir = dir;

        // ---- Re-sync to the transport target ----
        const double target =
            m_targetSrcSec.load(std::memory_order_relaxed);
        const double snapThreshold = std::max(0.25, 0.25 * aspeed);
        if (std::abs(cursor - target) > snapThreshold) {
            cursor = target;
            m_resampler.reset();
            needFadeIn = true;
        }

        // ---- Decode one grain ----
        // Ask the resampler what it needs for kGrainOutFrames at this
        // ratio (phase-exact), decode exactly that many source frames.
        const std::size_t srcFrames =
            m_resampler.sourceFramesNeeded(kGrainOutFrames, aspeed);
        const double srcSpanSec =
            static_cast<double>(srcFrames) / kOutRate;
        double grainStart = (dir > 0) ? cursor : cursor - srcSpanSec;
        if (grainStart < 0.0) grainStart = 0.0;

        if (m_srcBuf.size() < srcFrames * 2) m_srcBuf.resize(srcFrames * 2);
        const std::size_t got =
            reader->readAt(grainStart, m_srcBuf.data(), srcFrames);

        // Cursor advances by the full span regardless of short reads
        // (clip edge / EOF ⇒ silence remainder, position keeps
        // tracking the gesture). Clamp into the file.
        cursor += dir * srcSpanSec;
        const double dur = reader->duration();
        if (cursor < 0.0) cursor = 0.0;
        if (dur > 0.0 && cursor > dur) cursor = dur;

        if (got == 0) {
            // Nothing decodable here (edge / error) — brief nap so a
            // clamped-at-edge gesture doesn't spin.
            needFadeIn = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        // Zero-pad a short read to the full span.
        if (got < srcFrames) {
            std::memset(m_srcBuf.data() + got * 2, 0,
                        (srcFrames - got) * 2 * sizeof(float));
        }

        // Grain spans never exceed 1x source time (kPitchCap), so
        // reversal is always audible — reverse every backward grain.
        const bool reversed = (dir < 0);
        if (reversed) {
            // Reverse whole stereo frames in place.
            float *p = m_srcBuf.data();
            for (std::size_t i = 0, j = srcFrames - 1; i < j; ++i, --j) {
                std::swap(p[i * 2 + 0], p[j * 2 + 0]);
                std::swap(p[i * 2 + 1], p[j * 2 + 1]);
            }
            // Each reverse grain is a waveform discontinuity at both
            // edges; reset phase so grains don't smear into each
            // other, and fade the edges below.
            m_resampler.reset();
        }

        if (m_outBuf.size() < kGrainOutFrames * 2) {
            m_outBuf.resize(kGrainOutFrames * 2);
        }
        m_resampler.process(m_srcBuf.data(), srcFrames,
                            m_outBuf.data(), kGrainOutFrames, aspeed);

        // ---- Edge fades ----
        // Fade in after any reset/discontinuity; fade both edges of
        // reverse grains (their boundaries never join up).
        const std::size_t fadeN =
            std::min(kEdgeFadeFrames, kGrainOutFrames / 2);
        if (needFadeIn || reversed) {
            for (std::size_t i = 0; i < fadeN; ++i) {
                const float g = static_cast<float>(i)
                              / static_cast<float>(fadeN);
                m_outBuf[i * 2 + 0] *= g;
                m_outBuf[i * 2 + 1] *= g;
            }
            needFadeIn = false;
        }
        if (reversed) {
            for (std::size_t i = 0; i < fadeN; ++i) {
                const float g = static_cast<float>(i)
                              / static_cast<float>(fadeN);
                const std::size_t k = kGrainOutFrames - 1 - i;
                m_outBuf[k * 2 + 0] *= g;
                m_outBuf[k * 2 + 1] *= g;
            }
        }

        // ---- Ship it ----
        const auto *bytes =
            reinterpret_cast<const uint8_t *>(m_outBuf.data());
        std::size_t total = 0;
        const std::size_t want = kGrainOutFrames * 2 * sizeof(float);
        while (total < want && m_running.load(std::memory_order_relaxed)) {
            // Direction flipped while shipping: the rest of this
            // grain is wrong-way audio — drop it (the callback is
            // flushing the queue anyway).
            const double liveSpeed =
                m_signedSpeed.load(std::memory_order_relaxed);
            if (((liveSpeed < 0.0) ? -1 : 1) != dir) break;
            const std::size_t n = m_ring.write(bytes + total, want - total);
            total += n;
            if (total < want) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }
    }
}

} // namespace qcv
