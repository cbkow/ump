#include "timecode_formatter.h"

#include <QStringList>
#include <cmath>

namespace qcv {

namespace {

// Standard SMPTE drop-frame conversions for 29.97 fps. The formulas
// generalize to any DF rate by scaling to the display fps:
//   For 29.97 (display fps 30): dropPerMinute = 2, displayFps = 30.
//   For 59.94 (display fps 60): dropPerMinute = 4, displayFps = 60.
//
// Reference: SMPTE 12M-1, also documented in many places — the most
// widely-cited implementations are David Heidelberger's
// "Drop Frame Timecode" article and the Avid "DF Math" reference.

int dropFramesPerMinute(int displayFps)
{
    // 30 fps DF → 2 dropped frame numbers per minute.
    // 60 fps DF → 4 dropped per minute.
    return (displayFps / 30) * 2;
}

QString formatDropFrame(int frameNo, int displayFps)
{
    if (frameNo < 0) frameNo = 0;
    const int dropPerMinute   = dropFramesPerMinute(displayFps);
    // Frames per 10-minute "drop cycle": every 10 minutes the count
    // resyncs (no drop on the 0th/10th/... minute).
    const int framesPerMinute = displayFps * 60 - dropPerMinute;        // 1798 @ 30
    const int framesPer10Min  = displayFps * 600 - dropPerMinute * 9;   // 17982 @ 30

    int d = frameNo / framesPer10Min;
    int m = frameNo % framesPer10Min;

    int frames;
    if (m > dropPerMinute) {
        frames = frameNo
               + dropPerMinute * 9 * d
               + dropPerMinute * ((m - dropPerMinute) / framesPerMinute);
    } else {
        frames = frameNo + dropPerMinute * 9 * d;
    }

    const int ff = frames % displayFps;
    const int ss = (frames / displayFps) % 60;
    const int mm = (frames / (displayFps * 60)) % 60;
    const int hh = frames / (displayFps * 3600);

    return QString::asprintf("%02d:%02d:%02d;%02d", hh, mm, ss, ff);
}

QString formatNonDrop(int frameNo, int displayFps)
{
    if (frameNo < 0) frameNo = 0;
    const int ff = frameNo % displayFps;
    const int ss = (frameNo / displayFps) % 60;
    const int mm = (frameNo / (displayFps * 60)) % 60;
    const int hh = frameNo / (displayFps * 3600);
    return QString::asprintf("%02d:%02d:%02d:%02d", hh, mm, ss, ff);
}

int parseDropFrame(int hh, int mm, int ss, int ff, int displayFps)
{
    const int dropPerMinute   = dropFramesPerMinute(displayFps);
    const int totalMinutes    = 60 * hh + mm;
    // Inverse of formatDropFrame: total minutes minus the number of
    // 10-minute boundaries we haven't passed = minutes that DID drop.
    const int frame =
        ((displayFps * 60) * totalMinutes) +
        (displayFps * ss) +
        ff -
        (dropPerMinute * (totalMinutes - totalMinutes / 10));
    return frame;
}

int parseNonDrop(int hh, int mm, int ss, int ff, int displayFps)
{
    return ((hh * 60 + mm) * 60 + ss) * displayFps + ff;
}

} // namespace

TimecodeFormatter::TimecodeFormatter(int fpsNum, int fpsDen, bool dropFrame)
    : m_fpsNum(fpsNum), m_fpsDen(fpsDen), m_dropFrame(dropFrame)
{
    if (fpsDen > 0) {
        // Round to nearest whole-frame display rate. 29.97 → 30,
        // 23.976 → 24, 59.94 → 60. Drop-frame only meaningful at
        // 30, 60 (and the rarer 24/48 — not supported here yet).
        m_displayFps = static_cast<int>(std::lround(
            static_cast<double>(fpsNum) / static_cast<double>(fpsDen)));
    }
    // DF only makes sense on multiples of 30.
    if (m_dropFrame && m_displayFps != 30 && m_displayFps != 60) {
        m_dropFrame = false;
    }
}

QString TimecodeFormatter::format(int frameNo) const
{
    if (!isValid()) return QString();
    return m_dropFrame
        ? formatDropFrame(frameNo, m_displayFps)
        : formatNonDrop(frameNo, m_displayFps);
}

int TimecodeFormatter::parse(const QString &tc) const
{
    if (!isValid()) return -1;
    // Accept either separator before the frames field.
    QString normalized = tc;
    normalized.replace(QLatin1Char(';'), QLatin1Char(':'));
    const QStringList parts = normalized.split(QLatin1Char(':'));
    if (parts.size() != 4) return -1;

    bool ok[4] = { false };
    const int hh = parts[0].toInt(&ok[0]);
    const int mm = parts[1].toInt(&ok[1]);
    const int ss = parts[2].toInt(&ok[2]);
    const int ff = parts[3].toInt(&ok[3]);
    if (!ok[0] || !ok[1] || !ok[2] || !ok[3]) return -1;
    if (hh < 0 || mm < 0 || mm > 59 || ss < 0 || ss > 59 ||
        ff < 0 || ff >= m_displayFps) return -1;

    return m_dropFrame
        ? parseDropFrame(hh, mm, ss, ff, m_displayFps)
        : parseNonDrop(hh, mm, ss, ff, m_displayFps);
}

} // namespace qcv
