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
// Below this |speed|, reverse grains get their PCM reversed for the
// authentic backwards sound. Above it a grain spans so much source
// time that reversal is imperceptible — keep forward PCM and let the
// sparse "spooling" character carry the position feedback.
constexpr double      kReverseGrainMaxSpeed = 8.0;
// Back-pressure: hold ~120 ms buffered so speed changes stay audible
// within ~a grain of latency.
constexpr std::size_t kMaxBufferedBytes  = 46080;
// Varispeed clamp for the GRAIN math (pitch + source span). The low
// end covers drag-scrub, where a slow drag plays deeply pitched-down
// grains (classic analog-scrub sound). The high end is the PITCH CAP:
// transport speeds above it (shuttle ramps to 32x, aggressive drags
// hit similar rates) do NOT pitch further — linear varispeed at 32x
// is a 5-octave squeal. Instead the cursor advances at the cap, the
// target runs ahead at the real speed, and the snap rule below jumps
// the cursor forward — the audio degrades into cap-pitched snippets
// sampled sparsely along the path, which is what real decks (and
// Premiere/Avid shuttle) sound like at high speed.
constexpr double      kMinSpeed  = 0.05;
constexpr double      kPitchCap  = 4.0;   // ~2 octaves up, then plateau
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
    m_signedSpeed.store(signedSpeed, std::memory_order_relaxed);
    m_inGap.store(false, std::memory_order_relaxed);
    m_lastTargetMoveMs.store(nowMs(), std::memory_order_relaxed);

    if (!m_running.load()) {
        m_ring.clear();
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
    m_signedSpeed.store(signedSpeed, std::memory_order_relaxed);
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
    const std::size_t bytesWanted = frames * 2 * sizeof(float);
    const std::size_t bytesRead   = m_ring.read(out, bytesWanted);
    const std::size_t framesRead  = bytesRead / (2 * sizeof(float));
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

        // ---- Idle states: gap side, no source, or held still ----
        // Hold = |speed| under the threshold OR the target hasn't
        // moved for kHoldTimeoutMs (stationary drag-scrub mouse,
        // shuttle clamped at the media edge). Cursor keeps tracking
        // so resuming motion is seamless.
        const bool held =
            std::abs(m_signedSpeed.load(std::memory_order_relaxed))
                < kHoldSpeedThreshold
            || (nowMs() - m_lastTargetMoveMs.load(std::memory_order_relaxed))
                > kHoldTimeoutMs;
        if (m_inGap.load(std::memory_order_relaxed) || !reader || held) {
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
        // into sparse cap-pitched snippets (see kPitchCap comment).
        const double aspeed =
            std::clamp(std::abs(signedSpeed), kMinSpeed, kPitchCap);
        const int dir = (signedSpeed < 0.0) ? -1 : 1;

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

        const bool reversed = (dir < 0 && aspeed <= kReverseGrainMaxSpeed);
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
            const std::size_t n = m_ring.write(bytes + total, want - total);
            total += n;
            if (total < want) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }
    }
}

} // namespace qcv
