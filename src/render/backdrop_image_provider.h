// BackdropImageProvider — serves the frozen modal backdrop frame to QML.
//
// The UI-over-viewport framework (hazy-weaving-reddy plan) shows a
// modal over a dimmed, frozen still of the viewport. We capture that
// still as a QImage (via the renderer's screenshot path) and hand it to
// QML through this provider instead of writing a temp PNG to disk — no
// encode, no I/O, no SMB mid-write race. WindowManager::captureBackdrop
// calls setImage() then publishes an image://qcv/backdrop/<n> URL whose
// monotonic suffix busts QML's image cache.
//
// Thread note: setImage() runs on the GUI thread (the capture already
// deep-copied the frame); requestImage() runs on the QML image-loader
// thread. The mutex guards the swap; QImage is implicitly shared so the
// returned copy is cheap and detaches on write.

#pragma once

#include <QImage>
#include <QMutex>
#include <QQuickImageProvider>

namespace qcv {

class BackdropImageProvider : public QQuickImageProvider {
public:
    BackdropImageProvider()
        : QQuickImageProvider(QQuickImageProvider::Image) {}

    void setImage(const QImage &img);

    QImage requestImage(const QString &id, QSize *size,
                        const QSize &requestedSize) override;

private:
    QMutex m_mutex;
    QImage m_image;
};

} // namespace qcv
