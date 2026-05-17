// DualPlaybackTimer — Phase 7.7 Stage 3.
//
// Wall-clock master clock for the dual playback island. Mirrors the
// pattern of old QCView's PlaybackTimer: position advances against
// std::chrono::steady_clock at playbackSpeed × wall-time, with no
// gating from sources. Both sources receive the same decode_target
// each tick and independently fill their ring buffers ahead.
//
// THIS is the pacing layer. Sources MUST NOT sleep on wall clock
// inside their decode threads — see DualVideoDecoder header for the
// architectural rationale.

#pragma once

#include <atomic>
#include <chrono>

namespace qcv::dual {

class DualPlaybackTimer {
public:
    DualPlaybackTimer();

    // ---- Configuration ----
    // Drives frame-number derivation. Master fps is max(A.fps, B.fps)
    // when sources differ; controller picks. 0 = paused / no clock.
    void setFps(double fps);
    double fps() const { return m_fps.load(std::memory_order_acquire); }

    // ---- Loop / range ----
    // Inclusive frame range. Wraps when playhead crosses out. Pass
    // (-1, -1) to disable looping (clock holds at end).
    void setLoopRange(int loIn, int hiOut);
    void clearLoopRange();
    bool hasLoopRange() const { return m_hasLoopRange.load(std::memory_order_acquire); }
    int  loopIn()  const { return m_loopIn.load(std::memory_order_acquire); }
    int  loopOut() const { return m_loopOut.load(std::memory_order_acquire); }

    // ---- Transport ----
    void play();
    void pause();
    bool isPlaying() const { return m_playing.load(std::memory_order_acquire); }

    // Set the position directly (in frames). Used on transport scrub
    // and seek. Resets the wall-clock baseline so the next update()
    // ticks from this point.
    void seekToFrame(int frameNumber);

    // Playback speed. 1.0 = normal, 0.5 = half, etc. Negative for
    // reverse (not implemented in Stage 3 — clamps to 0).
    void setSpeed(double speed);
    double speed() const { return m_speed.load(std::memory_order_acquire); }

    // ---- Tick ----
    // Advance position by wall-time elapsed since the previous call.
    // Honors loop range. Returns the integer frame number after
    // advancement. Safe to call from any thread (the controller's
    // clock-pump thread).
    int update();

    // Read the current frame without advancing the clock.
    int currentFrame() const;

    // Position in seconds (fractional).
    double positionSeconds() const;

private:
    void resetBaseline();

    std::atomic<double> m_fps{0.0};
    std::atomic<double> m_speed{1.0};
    std::atomic<bool>   m_playing{false};

    // Position tracked as fractional frames so sub-frame wall-clock
    // ticks accumulate without rounding loss.
    std::atomic<double> m_positionFrames{0.0};

    // Wall-clock baseline — reset on play/pause/seek.
    mutable std::chrono::steady_clock::time_point m_lastWall;
    mutable std::atomic<bool> m_baselineSet{false};

    // Loop range (inclusive frame numbers).
    std::atomic<bool> m_hasLoopRange{false};
    std::atomic<int>  m_loopIn{0};
    std::atomic<int>  m_loopOut{0};
};

} // namespace qcv::dual
