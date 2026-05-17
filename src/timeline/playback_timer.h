// PlaybackTimer — Phase 7.2.b.
//
// The single source of truth for playback time across video,
// audio, scrubbing, and drift correction. Per Guide 02 D3 +
// D8: wall-clock-driven by default with an optional frame-
// locked mode; the audio mixer reads position() from this
// timer to eliminate video↔audio drift.
//
// Two clock policies, both first-class:
//
//   WallClock     update() advances position by
//                 (now - lastUpdate) * playbackSpeed.
//                 If the decoder can't keep up, frames skip
//                 but real-time cadence is preserved.
//
//   FrameLocked   advanceByFrame() advances position by exactly
//                 1/fps per call regardless of wall-clock time.
//                 If the decoder stalls, playback slows down
//                 rather than skipping frames. This is the path
//                 image sequences use (Phase 7.4) since there's
//                 no native decoder pacing them — the timer IS
//                 the only time authority. Also useful for video
//                 review where every frame must be seen.
//
// The owning TimelineController (Phase 7.2.c) decides which
// policy is active for each loaded source; users can toggle.
//
// Loop ranges intentionally NOT on the timer (Guide 02 D7) —
// the timeline view owns in/out points and the controller asks
// the timer to seek-to-in when the playhead reaches out.

#pragma once

#include <QObject>
#include <chrono>

namespace qcv {

class PlaybackTimer : public QObject
{
    Q_OBJECT
    Q_PROPERTY(double position READ position NOTIFY positionChanged)
    Q_PROPERTY(double duration READ duration WRITE setDuration NOTIFY durationChanged)
    Q_PROPERTY(double frameRate READ frameRate WRITE setFrameRate NOTIFY frameRateChanged)
    Q_PROPERTY(double playbackSpeed READ playbackSpeed
               WRITE setPlaybackSpeed NOTIFY playbackSpeedChanged)
    Q_PROPERTY(bool playing READ isPlaying NOTIFY playingChanged)
    Q_PROPERTY(int clockPolicy READ clockPolicyInt
               WRITE setClockPolicyInt NOTIFY clockPolicyChanged)

public:
    enum ClockPolicy : int {
        WallClock   = 0,
        FrameLocked = 1,
    };
    Q_ENUM(ClockPolicy)

    explicit PlaybackTimer(QObject *parent = nullptr);
    ~PlaybackTimer() override = default;

    // ---- Per-tick advance ---------------------------------------------------

    // Wall-clock advance. Call from the render loop. Computes
    // (now - lastUpdate) * speed and adds to position. Returns
    // true if position changed.
    Q_INVOKABLE bool update();

    // Frame-locked advance. Adds exactly 1/frameRate to position
    // regardless of wall-clock time. Used by FrameLocked clock
    // policy + by stepForward/stepBackward internally.
    Q_INVOKABLE void advanceByFrame();

    // ---- Transport controls -------------------------------------------------

    Q_INVOKABLE void play();
    Q_INVOKABLE void pause();
    Q_INVOKABLE void togglePlayPause();

    // Seek to absolute position in seconds. Pauses the wall-clock
    // integrator for one tick (firstUpdate flag) so the next
    // update() doesn't add a stale delta.
    Q_INVOKABLE void seek(double seconds);
    Q_INVOKABLE void seekRelative(double deltaSeconds);
    Q_INVOKABLE void seekToStart();
    Q_INVOKABLE void seekToEnd();

    // Frame-precise stepping. Pauses then seeks by exactly
    // n / frameRate seconds. Drift-free because we go through
    // seek() rather than accumulating advances.
    Q_INVOKABLE void stepForward(int frames = 1);
    Q_INVOKABLE void stepBackward(int frames = 1);

    // ---- Configuration ------------------------------------------------------

    void setDuration(double seconds);
    void setFrameRate(double fps);
    void setPlaybackSpeed(double speed);    // 0.25 .. 4.0 typical
    void setClockPolicy(ClockPolicy policy);

    // Loop is just "wrap to start when end hit" — a proper [in,out]
    // loop range is owned by the timeline view (Guide 02 D7).
    void setLooping(bool enabled) { m_looping = enabled; }
    bool looping() const { return m_looping; }

    // Optional loop range. When set + looping is on, update() wraps
    // from outSec back to inSec (using fmod for sub-frame remainder
    // continuity). Reset by passing inSec >= outSec or 0/0.
    void   setLoopRange(double inSec, double outSec);
    double loopInSec()  const { return m_loopInSec; }
    double loopOutSec() const { return m_loopOutSec; }
    bool   hasLoopRange() const {
        return m_loopOutSec > m_loopInSec && m_loopInSec >= 0.0;
    }

    // ---- Reads --------------------------------------------------------------

    double position() const { return m_position; }
    double duration() const { return m_duration; }
    double frameRate() const { return m_frameRate; }
    double playbackSpeed() const { return m_playbackSpeed; }
    bool   isPlaying() const { return m_playing; }
    ClockPolicy clockPolicy() const { return m_clockPolicy; }

    // Convenience: current frame index (rounded). Useful for the
    // image-sequence loader which works in frame-index space, and
    // for the timecode readout in the transport bar.
    Q_INVOKABLE int currentFrame() const;
    Q_INVOKABLE int totalFrames() const;

signals:
    void positionChanged();
    void durationChanged();
    void frameRateChanged();
    void playbackSpeedChanged();
    void playingChanged();
    void clockPolicyChanged();

    // Convenience for the controller — emits whenever the timer
    // crosses an integer frame boundary. Image-sequence loader
    // listens to this to know "fetch the next frame from disk."
    // For wall-clock mode this can fire multiple times per
    // update() if speed > 1; for frame-locked mode it fires once
    // per advanceByFrame().
    void frameAdvanced(int frame);

private:
    int    clockPolicyInt() const { return static_cast<int>(m_clockPolicy); }
    void   setClockPolicyInt(int p) { setClockPolicy(static_cast<ClockPolicy>(p)); }

    void   notifyFrameCrossed(int oldFrame);
    void   clampPosition();

    double m_position      = 0.0;
    double m_duration      = 0.0;
    double m_frameRate     = 24.0;
    double m_playbackSpeed = 1.0;
    bool   m_playing       = false;
    ClockPolicy m_clockPolicy = WallClock;

    using Clock = std::chrono::steady_clock;
    Clock::time_point m_lastUpdate;
    bool   m_firstUpdate = true;
    bool   m_looping     = false;
    double m_loopInSec   = 0.0;
    double m_loopOutSec  = 0.0;  // out > in marks the range as set
};

} // namespace qcv
