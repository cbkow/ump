#include "dual_playback_timer.h"

#include <QtLogging>
#include <algorithm>
#include <cmath>

namespace qcv::dual {

DualPlaybackTimer::DualPlaybackTimer() = default;

void DualPlaybackTimer::setFps(double fps)
{
    if (fps < 0.0) fps = 0.0;
    m_fps.store(fps, std::memory_order_release);
    resetBaseline();
}

void DualPlaybackTimer::setLoopRange(int loIn, int hiOut)
{
    if (loIn < 0 || hiOut < loIn) {
        clearLoopRange();
        return;
    }
    m_loopIn.store(loIn, std::memory_order_release);
    m_loopOut.store(hiOut, std::memory_order_release);
    m_hasLoopRange.store(true, std::memory_order_release);
}

void DualPlaybackTimer::clearLoopRange()
{
    m_hasLoopRange.store(false, std::memory_order_release);
}

void DualPlaybackTimer::play()
{
    m_playing.store(true, std::memory_order_release);
    resetBaseline();
}

void DualPlaybackTimer::pause()
{
    // Latch current position before flipping playing — update() may
    // be called concurrently. Calling update() once before flip
    // ensures the position reflects elapsed wall-time up to now.
    update();
    m_playing.store(false, std::memory_order_release);
}

void DualPlaybackTimer::seekToFrame(int frameNumber)
{
    if (frameNumber < 0) frameNumber = 0;
    m_positionFrames.store(static_cast<double>(frameNumber),
                            std::memory_order_release);
    resetBaseline();
}

void DualPlaybackTimer::setSpeed(double speed)
{
    // Clamp to non-negative for Stage 3 (reverse playback is later).
    if (speed < 0.0) speed = 0.0;
    m_speed.store(speed, std::memory_order_release);
    resetBaseline();
}

int DualPlaybackTimer::update()
{
    const double fps = m_fps.load(std::memory_order_acquire);
    if (fps <= 0.0) {
        return currentFrame();
    }

    const auto now = std::chrono::steady_clock::now();
    if (!m_baselineSet.load(std::memory_order_acquire)) {
        m_lastWall = now;
        m_baselineSet.store(true, std::memory_order_release);
        return currentFrame();
    }

    if (m_playing.load(std::memory_order_acquire)) {
        const auto delta = std::chrono::duration<double>(now - m_lastWall).count();
        const double speed = m_speed.load(std::memory_order_acquire);
        const double advanceFrames = delta * fps * speed;

        double pos = m_positionFrames.load(std::memory_order_acquire);
        pos += advanceFrames;

        if (m_hasLoopRange.load(std::memory_order_acquire)) {
            const int loIn  = m_loopIn.load(std::memory_order_acquire);
            const int loOut = m_loopOut.load(std::memory_order_acquire);
            const double rangeLen = static_cast<double>(loOut - loIn + 1);
            if (rangeLen > 0.0) {
                const double prePos = pos;
                while (pos > static_cast<double>(loOut)) {
                    pos -= rangeLen;
                }
                if (pos < static_cast<double>(loIn)) {
                    pos = static_cast<double>(loIn);
                }
                if (pos != prePos) {
                    qInfo("[dual-wrap] pre=%.2f post=%.2f loop=[%d,%d]",
                          prePos, pos, loIn, loOut);
                }
            }
        }

        m_positionFrames.store(pos, std::memory_order_release);
    }

    m_lastWall = now;
    return currentFrame();
}

int DualPlaybackTimer::currentFrame() const
{
    const double pos = m_positionFrames.load(std::memory_order_acquire);
    return static_cast<int>(std::floor(pos));
}

double DualPlaybackTimer::positionSeconds() const
{
    const double fps = m_fps.load(std::memory_order_acquire);
    if (fps <= 0.0) return 0.0;
    return m_positionFrames.load(std::memory_order_acquire) / fps;
}

void DualPlaybackTimer::resetBaseline()
{
    m_baselineSet.store(false, std::memory_order_release);
}

} // namespace qcv::dual
