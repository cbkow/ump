// TempoStage — SoundTouch WSOLA wrapper for constant-pitch tempo
// (the review-speeds feature: 0.5x–2x playback with natural pitch).
//
// Producer-side stage: lives on each decoder's decode thread between
// decoded/filtered PCM and the ring write. WSOLA needs ~60–100 ms
// analysis windows and allocates internally, so it is NOT render-
// callback-safe — which is exactly why it sits here and not at the
// consumer drain (the sync servo's FractionalResampler covers that
// side).
//
// tempo semantics match SoundTouch: output duration = input / tempo,
// so tempo 2.0 consumes 2 source seconds per output second. AudioPlayer
// and DualAudioMixer scale their consumption-based position estimates
// by tempo() accordingly.
//
// At tempo == 1.0 the stage reports bypassed() and the decoders skip
// it entirely — the 1x path stays byte-identical to the pre-tempo
// code. Tempo changes are applied lazily on the decode thread at the
// next put(), clearing internal WSOLA state; the owning player
// performs a re-anchor seek on change anyway, so stale-tempo ring
// residue is dropped rather than played.

#pragma once

#include <atomic>
#include <cstddef>
#include <memory>

namespace soundtouch { class SoundTouch; }

namespace qcv {

class TempoStage
{
public:
    TempoStage();
    ~TempoStage();

    TempoStage(const TempoStage &) = delete;
    TempoStage &operator=(const TempoStage &) = delete;

    // Any thread. Clamped to [0.25, 4.0]; exactly 1.0 -> bypass.
    void   setTempo(double tempo);
    double tempo()    const { return m_tempo.load(); }
    bool   bypassed() const { return m_bypassed.load(); }

    // Decode thread only. put() applies a pending tempo change
    // (clearing WSOLA state) before feeding `frames` of interleaved
    // stereo f32.
    void        put(const float *in, std::size_t frames);
    // Drain up to maxFrames of stretched output; returns frames
    // delivered (0 when the stretcher needs more input).
    std::size_t receive(float *out, std::size_t maxFrames);
    // Discard buffered input/output (on seek).
    void        flush();

private:
    std::unique_ptr<soundtouch::SoundTouch> m_st;
    std::atomic<double> m_tempo{1.0};
    std::atomic<bool>   m_bypassed{true};
    std::atomic<bool>   m_dirty{false};
};

} // namespace qcv
