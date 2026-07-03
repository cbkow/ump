// AudioSyncServo — drift → consumption-ratio controller for the
// continuous A/V sync servo.
//
// Replaces the old bang-bang model (re-seek the audio decoder when
// |drift| > 150 ms) inside the servo band: the caller measures drift
// each update tick and this PI controller returns a consumption ratio
// in [1 - kMaxTrim, 1 + kMaxTrim] that the render-callback-side
// FractionalResampler applies when draining the decoder ring. A trim
// of ±0.2 % is ~3.5 cents of pitch — inaudible — and sample-continuous,
// so drift converges to ~0 with no clicks or gaps.
//
// The caller keeps the escape tiers (this class never seeks):
//   |drift| <= servo band (~40 ms)  -> servo only
//   band .. 1 s                     -> soft re-seek (existing cooldown)
//   > 1 s                           -> immediate re-seek (discontinuity)
//
// update() runs on the UI thread at an irregular cadence (per video
// frame in video mode, ~30 Hz pump otherwise) — hence dt-aware terms.
// The returned ratio is published to the render callback through an
// atomic owned by the caller. Not a QObject; no locks.

#pragma once

namespace qcv {

class AudioSyncServo
{
public:
    // driftSeconds: master-clock target minus audio playout position
    // (positive = audio is behind, needs to consume faster).
    // dtSeconds: time since the previous update() call.
    double update(double driftSeconds, double dtSeconds)
    {
        if (dtSeconds <= 0.0 || dtSeconds > 0.5) dtSeconds = 1.0 / 30.0;

        // Integral with anti-windup: clamped to the trim range so a
        // long one-sided drift can't wind past what the output can
        // ever apply.
        m_integral += driftSeconds * dtSeconds * kI;
        m_integral  = clampTrim(m_integral);

        const double trim = clampTrim(kP * driftSeconds + m_integral);

        // Slew-limit the ratio so corrections ramp smoothly (no
        // zipper artifacts from step changes between callbacks).
        const double target = 1.0 + trim;
        double delta = target - m_ratio;
        if (delta >  kMaxSlewPerUpdate) delta =  kMaxSlewPerUpdate;
        if (delta < -kMaxSlewPerUpdate) delta = -kMaxSlewPerUpdate;
        m_ratio += delta;
        return m_ratio;
    }

    void reset()
    {
        m_integral = 0.0;
        m_ratio    = 1.0;
    }

    double ratio() const { return m_ratio; }

private:
    static double clampTrim(double t)
    {
        if (t >  kMaxTrim) return  kMaxTrim;
        if (t < -kMaxTrim) return -kMaxTrim;
        return t;
    }

    // kP: 40 ms of drift maps to the full ±0.2 % trim.
    // kI: soaks up steady-state clock skew (typical crystal mismatch
    //     is tens of ppm; trim capacity is 2000 ppm — ample).
    static constexpr double kP               = 0.05;   // per second
    static constexpr double kI               = 0.01;   // per second^2
    static constexpr double kMaxTrim         = 0.002;  // ±0.2 %
    static constexpr double kMaxSlewPerUpdate = 0.0005;

    double m_integral = 0.0;
    double m_ratio    = 1.0;
};

} // namespace qcv
