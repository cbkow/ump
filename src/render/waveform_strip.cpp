#include "waveform_strip.h"

#include "audio/waveform_probe_engine.h"

#include <QPainter>

#include <algorithm>
#include <cmath>

namespace qcv {

WaveformStrip::WaveformStrip(QQuickItem *parent)
    : QQuickPaintedItem(parent)
{
    m_settle.setSingleShot(true);
    m_settle.setInterval(150);
    connect(&m_settle, &QTimer::timeout,
            this, &WaveformStrip::issueRequest);

    connect(WaveformProbeEngine::instance(),
            &WaveformProbeEngine::dataArrived,
            this, [this](const QString &path) {
                if (m_active && path == m_sourcePath) update();
            });

    connect(this, &QQuickItem::widthChanged,
            this, [this]() { onVantageChanged(); });
}

void WaveformStrip::setSourcePath(const QString &path)
{
    if (m_sourcePath == path) return;
    m_sourcePath = path;
    emit sourcePathChanged();
    onVantageChanged();
}

void WaveformStrip::setWindowStart(double v)
{
    if (qFuzzyCompare(m_windowStart, v)) return;
    m_windowStart = v;
    emit vantageChanged();
    onVantageChanged();
}

void WaveformStrip::setWindowEnd(double v)
{
    if (qFuzzyCompare(m_windowEnd, v)) return;
    m_windowEnd = v;
    emit vantageChanged();
    onVantageChanged();
}

void WaveformStrip::setClipStart(double v)
{
    if (qFuzzyCompare(m_clipStart, v)) return;
    m_clipStart = v;
    emit vantageChanged();
    onVantageChanged();
}

void WaveformStrip::setClipDuration(double v)
{
    if (qFuzzyCompare(m_clipDuration, v)) return;
    m_clipDuration = v;
    emit vantageChanged();
    onVantageChanged();
}

void WaveformStrip::setSourceIn(double v)
{
    if (qFuzzyCompare(m_sourceIn, v)) return;
    m_sourceIn = v;
    emit vantageChanged();
    onVantageChanged();
}

void WaveformStrip::setActive(bool v)
{
    if (m_active == v) return;
    m_active = v;
    emit activeChanged();
    onVantageChanged();
}

void WaveformStrip::setPlaying(bool v)
{
    if (m_playing == v) return;
    m_playing = v;
    emit playingChanged();
    WaveformProbeEngine::instance()->setPlaybackActive(v);
    // Playback stopped → chase the current vantage again.
    if (!v) onVantageChanged();
}

void WaveformStrip::setWaveColor(const QColor &c)
{
    if (m_waveColor == c) return;
    m_waveColor = c;
    emit waveColorChanged();
    update();
}

void WaveformStrip::onVantageChanged()
{
    // Paint immediately from whatever the memo holds (free), decode
    // only once the vantage rests.
    update();
    if (m_active && !m_sourcePath.isEmpty()) m_settle.start();
}

void WaveformStrip::issueRequest()
{
    if (!m_active || m_sourcePath.isEmpty() || m_playing) return;
    const int columns = static_cast<int>(width());
    if (columns <= 0 || m_windowEnd <= m_windowStart) return;

    // Probe only the intersection of the viewport and the clip,
    // mapped into source time.
    const double visStart = std::max(m_windowStart, m_clipStart);
    const double visEnd =
        std::min(m_windowEnd, m_clipStart + m_clipDuration);
    if (visEnd <= visStart) return;
    const double srcT0 = m_sourceIn + (visStart - m_clipStart);
    const double srcT1 = m_sourceIn + (visEnd - m_clipStart);

    // Keep column density: probe resolution matches on-screen pixels
    // covering the intersection.
    const double winSpan = m_windowEnd - m_windowStart;
    const int cols = std::max(1, static_cast<int>(
        columns * (visEnd - visStart) / winSpan));

    WaveformProbeEngine::instance()->requestWindow(
        m_sourcePath, srcT0, srcT1, cols);
}

void WaveformStrip::paint(QPainter *painter)
{
    if (!m_active || m_sourcePath.isEmpty()) return;
    const int w = static_cast<int>(width());
    const int h = static_cast<int>(height());
    if (w <= 0 || h <= 0 || m_windowEnd <= m_windowStart) return;

    const double winSpan = m_windowEnd - m_windowStart;
    const double colSpan = winSpan / w;

    // Column x covers timeline time [wS + x·colSpan, +colSpan);
    // sample the engine over the full window and let clip bounds
    // blank the outside columns.
    const double srcT0 = m_sourceIn + (m_windowStart - m_clipStart);
    const double srcT1 = m_sourceIn + (m_windowEnd - m_clipStart);
    const auto peaks = WaveformProbeEngine::instance()->sampleColumns(
        m_sourcePath, srcT0, srcT1, w);

    const double clipEnd = m_clipStart + m_clipDuration;
    const qreal mid = h / 2.0;
    // Slight headroom so full-scale audio doesn't kiss the clip
    // border; min visible tick of 1px so quiet-but-present audio
    // still reads as a line.
    const qreal amp = (h / 2.0) - 1.0;

    painter->setPen(Qt::NoPen);
    painter->setBrush(m_waveColor);

    for (int x = 0; x < w && x < peaks.size(); ++x) {
        const auto &p = peaks[x];
        if (!p.valid) continue;
        const double t = m_windowStart + (x + 0.5) * colSpan;
        if (t < m_clipStart || t >= clipEnd) continue;

        const qreal top = mid - std::clamp<qreal>(p.mx, -1.0, 1.0) * amp;
        const qreal bot = mid - std::clamp<qreal>(p.mn, -1.0, 1.0) * amp;
        painter->drawRect(
            QRectF(x, top, 1.0, std::max<qreal>(1.0, bot - top)));
    }
}

} // namespace qcv
