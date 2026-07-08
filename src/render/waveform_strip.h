// WaveformStrip — viewport-sized audio waveform overlay for a
// timeline track row (EXPERIMENT, 2026-07-08 — the codebase's first
// QQuickPaintedItem).
//
// One strip covers one track row's visible width. Every paint asks
// WaveformProbeEngine::sampleColumns for one min/max pair per pixel
// column of the CURRENT vantage — so repaints are pure memo lookups
// (microseconds) and happen freely per wheel tick, while actual
// audio probing is only *requested* after the vantage settles for
// ~150 ms. Columns with no probe yet paint transparent and fill in
// as the worker's dataArrived pings land.
//
// The strip maps viewport x → timeline time → clip source time via
// the clip's (clipStart, clipDuration, sourceIn) window, so dual
// trim/slip edits stay honest; columns outside the clip are blank.
// Gate `active` on the clip's hasAudio and feed `playing` from the
// panel's playbackActive so probing pauses during playback.

#pragma once

#include <QColor>
#include <QQuickPaintedItem>
#include <QTimer>
#include <QtQmlIntegration>

namespace qcv {

class WaveformStrip : public QQuickPaintedItem {
    Q_OBJECT
    // Media file whose audio to sample (clip's mediaPath).
    Q_PROPERTY(QString sourcePath READ sourcePath WRITE setSourcePath
               NOTIFY sourcePathChanged FINAL)
    // Visible timeline window (seconds) — scrollX/pps at the edges.
    Q_PROPERTY(double windowStart READ windowStart WRITE setWindowStart
               NOTIFY vantageChanged FINAL)
    Q_PROPERTY(double windowEnd READ windowEnd WRITE setWindowEnd
               NOTIFY vantageChanged FINAL)
    // Clip placement: timeline start, duration, and source in-point.
    Q_PROPERTY(double clipStart READ clipStart WRITE setClipStart
               NOTIFY vantageChanged FINAL)
    Q_PROPERTY(double clipDuration READ clipDuration WRITE setClipDuration
               NOTIFY vantageChanged FINAL)
    Q_PROPERTY(double sourceIn READ sourceIn WRITE setSourceIn
               NOTIFY vantageChanged FINAL)
    // Master gate (hasAudio && loaded). Inactive strips never probe.
    Q_PROPERTY(bool active READ active WRITE setActive
               NOTIFY activeChanged FINAL)
    // Transport running — suspends probing engine-wide.
    Q_PROPERTY(bool playing READ playing WRITE setPlaying
               NOTIFY playingChanged FINAL)
    Q_PROPERTY(QColor waveColor READ waveColor WRITE setWaveColor
               NOTIFY waveColorChanged FINAL)
    QML_ELEMENT

public:
    explicit WaveformStrip(QQuickItem *parent = nullptr);

    void paint(QPainter *painter) override;

    QString sourcePath() const { return m_sourcePath; }
    void setSourcePath(const QString &path);
    double windowStart() const { return m_windowStart; }
    void setWindowStart(double v);
    double windowEnd() const { return m_windowEnd; }
    void setWindowEnd(double v);
    double clipStart() const { return m_clipStart; }
    void setClipStart(double v);
    double clipDuration() const { return m_clipDuration; }
    void setClipDuration(double v);
    double sourceIn() const { return m_sourceIn; }
    void setSourceIn(double v);
    bool active() const { return m_active; }
    void setActive(bool v);
    bool playing() const { return m_playing; }
    void setPlaying(bool v);
    QColor waveColor() const { return m_waveColor; }
    void setWaveColor(const QColor &c);

signals:
    void sourcePathChanged();
    void vantageChanged();
    void activeChanged();
    void playingChanged();
    void waveColorChanged();

private:
    void onVantageChanged();
    void issueRequest();

    QString m_sourcePath;
    double m_windowStart = 0.0;
    double m_windowEnd = 0.0;
    double m_clipStart = 0.0;
    double m_clipDuration = 0.0;
    double m_sourceIn = 0.0;
    bool m_active = false;
    bool m_playing = false;
    QColor m_waveColor = QColor(255, 255, 255, 70);

    // Probe-request settle timer: repaint-from-memo is free and
    // immediate; decoding only chases the vantage once it rests.
    QTimer m_settle;
};

} // namespace qcv
