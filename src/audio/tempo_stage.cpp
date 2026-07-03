#include "tempo_stage.h"

#include <SoundTouch.h>

namespace qcv {

TempoStage::TempoStage()
    : m_st(std::make_unique<soundtouch::SoundTouch>())
{
    // Fixed pipeline format — both decoder shapes hand this stage
    // 48 kHz stereo f32 (SOUNDTOUCH_FLOAT_SAMPLES is the vendored
    // build's sample type).
    m_st->setSampleRate(48000);
    m_st->setChannels(2);
    m_st->setTempo(1.0);
    // Pitch and rate stay untouched: this stage is tempo-only
    // (constant pitch). Varispeed shuttle uses FractionalResampler.
}

TempoStage::~TempoStage() = default;

void TempoStage::setTempo(double tempo)
{
    if (tempo < 0.25) tempo = 0.25;
    if (tempo > 4.0)  tempo = 4.0;
    if (m_tempo.exchange(tempo) == tempo) return;
    m_bypassed.store(tempo == 1.0);
    m_dirty.store(true);
}

void TempoStage::put(const float *in, std::size_t frames)
{
    if (m_dirty.exchange(false)) {
        // Clear rather than let old-tempo WSOLA state smear into the
        // new rate; the owning player re-anchors with a seek on tempo
        // change, so the dropped tail was never going to play.
        m_st->clear();
        m_st->setTempo(m_tempo.load());
    }
    if (!in || frames == 0) return;
    m_st->putSamples(in, static_cast<unsigned int>(frames));
}

std::size_t TempoStage::receive(float *out, std::size_t maxFrames)
{
    if (!out || maxFrames == 0) return 0;
    return m_st->receiveSamples(out, static_cast<unsigned int>(maxFrames));
}

void TempoStage::flush()
{
    m_st->clear();
}

} // namespace qcv
