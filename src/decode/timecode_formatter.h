// TimecodeFormatter — Phase 1.8.7.
//
// SMPTE timecode <-> frame-number conversion, drop-frame aware.
//
// Old QCView Player only handled drop-frame syntactically — it
// stripped the `;` separator and ran the same arithmetic as for
// non-drop, which silently misreports timecodes on 29.97 / 59.94
// NTSC content (cumulatively 18 frames per 10 minutes off). The
// new port implements proper DF math from day one because real
// post-production footage routinely arrives with DF tmcd tracks.
//
// Drop-frame rules (29.97 fps NTSC, double for 59.94):
//   - 2 frame numbers are dropped at the start of every minute
//     (frames 00 and 01 don't appear),
//   - EXCEPT every 10th minute (00, 10, 20, 30, 40, 50) — those
//     keep all frame numbers.
//   Net: 18 dropped frame numbers per 10 minutes, 108 per hour.
//   Real frame-rate stays 29.97; only the DISPLAY timecode skips.
//
// All conversions are pure functions; the class is stateless apart
// from the (fps_num, fps_den, isDropFrame) it's constructed with.

#pragma once

#include <QString>

namespace qcv {

class TimecodeFormatter
{
public:
    TimecodeFormatter() = default;
    TimecodeFormatter(int fpsNum, int fpsDen, bool dropFrame);

    bool isValid() const { return m_fpsNum > 0 && m_fpsDen > 0; }
    bool isDropFrame() const { return m_dropFrame; }

    // The integer frame rate that timecode-display rolls. For 29.97
    // (29970/1000 or 30000/1001) this is 30; for 23.976 it's 24.
    // SMPTE timecode always counts in whole frames per second and
    // uses drop-frame to compensate for the fractional offset.
    int displayFps() const { return m_displayFps; }

    // Format an absolute frame number (relative to a 00:00:00:00
    // origin) as a SMPTE timecode string.
    // Example: 86400 frames @ 24 fps non-drop → "01:00:00:00".
    // Example: 53954 frames @ 29.97 DF       → "00:30:00;00".
    QString format(int frameNo) const;

    // Inverse: parse a SMPTE timecode string into an absolute frame
    // number. Accepts both `:` and `;` for the frames separator
    // (the parser respects whichever was passed; if the formatter is
    // configured DF, DF math is applied regardless of separator).
    // Returns -1 if the string cannot be parsed.
    int parse(const QString &tc) const;

private:
    int  m_fpsNum     = 0;
    int  m_fpsDen     = 1;
    bool m_dropFrame  = false;
    int  m_displayFps = 0;   // round(fpsNum / fpsDen)
};

} // namespace qcv
