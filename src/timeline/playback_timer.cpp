#include "playback_timer.h"

#include <QtGlobal>
#include <algorithm>
#include <cmath>

namespace qcv {

PlaybackTimer::PlaybackTimer(QObject *parent)
    : QObject(parent)
{
}

// ---- Per-tick advance ------------------------------------------------------

bool PlaybackTimer::update()
{
    if (!m_playing) {
        // Even when not playing we keep lastUpdate fresh so the
        // resume doesn't accumulate a giant delta. Same as a
        // freshly-seeked state.
        m_lastUpdate  = Clock::now();
        m_firstUpdate = false;
        return false;
    }

    if (m_clockPolicy == FrameLocked) {
        // FrameLocked: ignore wall clock; advance one frame per
        // tick. Caller (typically the render loop) is the metronome.
        advanceByFrame();
        return true;
    }

    const auto now = Clock::now();
    if (m_firstUpdate) {
        m_lastUpdate  = now;
        m_firstUpdate = false;
        return false;
    }

    const double dt = std::chrono::duration<double>(now - m_lastUpdate).count();
    m_lastUpdate = now;

    if (dt <= 0.0) return false;

    const int oldFrame = currentFrame();
    m_position += dt * m_playbackSpeed;
    // Wrap to start when looping. fmod preserves the sub-frame
    // remainder so a 1.5x clip ending at 9.97 wraps to 0.03 and
    // playback stays smooth across the seam.
    // Loop wrap. With an in/out range set, wrap inside [in, out];
    // otherwise wrap on the full clip duration. The range path
    // ignores duration so users can preview wraps that go past
    // the normal clip end.
    if (m_looping && m_loopOutSec > m_loopInSec) {
        const double span = m_loopOutSec - m_loopInSec;
        if (m_position >= m_loopOutSec) {
            m_position = m_loopInSec
                + std::fmod(m_position - m_loopInSec, span);
        } else if (m_position < m_loopInSec) {
            m_position = m_loopOutSec
                - std::fmod(m_loopInSec - m_position, span);
        }
    } else if (m_looping && m_duration > 0.0) {
        if (m_position >= m_duration) {
            m_position = std::fmod(m_position, m_duration);
        } else if (m_position < 0.0) {
            m_position = std::fmod(m_position, m_duration) + m_duration;
        }
    } else {
        clampPosition();
    }
    emit positionChanged();
    notifyFrameCrossed(oldFrame);
    return true;
}

void PlaybackTimer::advanceByFrame()
{
    if (m_frameRate <= 0.0) return;
    const int oldFrame = currentFrame();
    m_position += 1.0 / m_frameRate;
    clampPosition();
    emit positionChanged();
    notifyFrameCrossed(oldFrame);
}

// ---- Transport controls ----------------------------------------------------

void PlaybackTimer::play()
{
    if (m_playing) return;
    m_playing = true;
    m_firstUpdate = true;   // next update() resets the delta integrator
    emit playingChanged();
}

void PlaybackTimer::pause()
{
    if (!m_playing) return;
    m_playing = false;
    emit playingChanged();
}

void PlaybackTimer::togglePlayPause()
{
    if (m_playing) pause(); else play();
}

void PlaybackTimer::seek(double seconds)
{
    const int oldFrame = currentFrame();
    if (seconds < 0.0) seconds = 0.0;
    if (m_duration > 0.0 && seconds > m_duration) seconds = m_duration;

    if (qFuzzyCompare(m_position, seconds)) return;
    m_position = seconds;
    m_firstUpdate = true;   // don't integrate stale wall-clock delta after seek
    emit positionChanged();
    notifyFrameCrossed(oldFrame);
}

void PlaybackTimer::seekRelative(double deltaSeconds)
{
    seek(m_position + deltaSeconds);
}

void PlaybackTimer::seekToStart()  { seek(0.0); }
void PlaybackTimer::seekToEnd()    { seek(m_duration); }

void PlaybackTimer::stepForward(int frames)
{
    if (m_frameRate <= 0.0 || frames == 0) return;
    pause();
    seek(m_position + static_cast<double>(frames) / m_frameRate);
}

void PlaybackTimer::stepBackward(int frames)
{
    stepForward(-frames);
}

// ---- Configuration ---------------------------------------------------------

void PlaybackTimer::setDuration(double seconds)
{
    if (qFuzzyCompare(m_duration, seconds)) return;
    m_duration = std::max(0.0, seconds);
    if (m_position > m_duration) {
        m_position = m_duration;
        emit positionChanged();
    }
    emit durationChanged();
}

void PlaybackTimer::setFrameRate(double fps)
{
    if (fps <= 0.0) return;
    if (qFuzzyCompare(m_frameRate, fps)) return;
    m_frameRate = fps;
    emit frameRateChanged();
}

void PlaybackTimer::setPlaybackSpeed(double speed)
{
    // Clamp to a sane range. Old app caps at 4× — beyond that the
    // decoder can't keep up and frames just skip massively, which
    // isn't useful for review. Users who want true fast-forward
    // should use the StepForward shortcut repeatedly.
    speed = std::clamp(speed, 0.0625, 8.0);
    if (qFuzzyCompare(m_playbackSpeed, speed)) return;
    m_playbackSpeed = speed;
    m_firstUpdate = true;   // re-seat the wall-clock integrator
    emit playbackSpeedChanged();
}

void PlaybackTimer::setClockPolicy(ClockPolicy policy)
{
    if (m_clockPolicy == policy) return;
    m_clockPolicy = policy;
    m_firstUpdate = true;
    emit clockPolicyChanged();
}

void PlaybackTimer::setLoopRange(double inSec, double outSec)
{
    // Empty / inverted range disables the loop window. Negative
    // inSec is treated as "no range" too — guards against
    // accidental sentinel values.
    if (inSec < 0.0 || outSec <= inSec) {
        m_loopInSec  = 0.0;
        m_loopOutSec = 0.0;
        return;
    }
    m_loopInSec  = inSec;
    m_loopOutSec = outSec;
}

// ---- Reads -----------------------------------------------------------------

int PlaybackTimer::currentFrame() const
{
    if (m_frameRate <= 0.0) return 0;
    // Round to nearest. Keeps the displayed frame stable around
    // half-frame boundaries (where wall-clock drift is most
    // visible). The image-sequence loader uses this directly to
    // pick a file from the pattern.
    return static_cast<int>(std::lround(m_position * m_frameRate));
}

int PlaybackTimer::totalFrames() const
{
    if (m_frameRate <= 0.0 || m_duration <= 0.0) return 0;
    return static_cast<int>(std::lround(m_duration * m_frameRate));
}

// ---- Internals -------------------------------------------------------------

void PlaybackTimer::clampPosition()
{
    if (m_position < 0.0) m_position = 0.0;
    if (m_duration > 0.0 && m_position > m_duration) {
        m_position = m_duration;
    }
}

void PlaybackTimer::notifyFrameCrossed(int oldFrame)
{
    const int newFrame = currentFrame();
    if (newFrame != oldFrame) {
        emit frameAdvanced(newFrame);
    }
}

} // namespace qcv
