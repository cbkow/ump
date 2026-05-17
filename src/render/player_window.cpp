#include "player_window.h"

#include "annotations/viewport_annotator.h"
#include "iplayer_renderer.h"
#include "player_window_factory.h"

#include <QCursor>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QExposeEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QtLogging>

#include <algorithm>

namespace qcv {

PlayerWindow::PlayerWindow(QWindow *parent)
    : QWindow(parent)
    , m_renderer(createPlayerRenderer())
{
    // SurfaceType is set by createPlayerRenderer-side wiring; default
    // here is OpenGL — we fix it per platform via the factory's
    // helper.
    setSurfaceType(playerWindowSurfaceType());
}

PlayerWindow::~PlayerWindow()
{
    if (m_renderer && m_rendererInited) {
        m_renderer->shutdown();
    }
}

void PlayerWindow::setViewportAnnotator(ViewportAnnotator *annotator)
{
    m_annotator = annotator;
}

namespace {
constexpr float kSeamGrabWidth = 14.0f;   // logical points, matches Main.qml
}

void PlayerWindow::setCompositorMode(int mode)
{
    m_compositorMode.store(mode, std::memory_order_release);
    // If we leave Wipe mode mid-drag, clear seam state + restore the
    // default cursor so we don't strand a SplitHCursor on screen.
    if (mode != 2 && m_seamDragActive) {
        m_seamDragActive = false;
        unsetCursor();
    } else if (mode != 2) {
        unsetCursor();
    }
}

void PlayerWindow::setSplitPos(float pos)
{
    if (pos < 0.0f) pos = 0.0f;
    if (pos > 1.0f) pos = 1.0f;
    m_splitPos.store(pos, std::memory_order_release);
}

void PlayerWindow::setDualActive(bool active)
{
    m_dualActive.store(active, std::memory_order_release);
    // Leaving dual flow mid-drag — clear seam state + restore cursor
    // so a stale SplitHCursor doesn't linger over a now-single viewport.
    if (!active) {
        if (m_seamDragActive) m_seamDragActive = false;
        unsetCursor();
    }
}

bool PlayerWindow::handleSeamMouse(float xLogical, bool isPress)
{
    if (m_compositorMode.load(std::memory_order_acquire) != 2) {
        return false;
    }
    if (!m_dualActive.load(std::memory_order_acquire)) {
        return false;
    }
    const float w = static_cast<float>(width());
    if (w <= 0.0f) return false;

    const float seamX = std::clamp(
        m_splitPos.load(std::memory_order_acquire), 0.0f, 1.0f) * w;
    const float half  = kSeamGrabWidth * 0.5f;

    // On a fresh press, only engage if the cursor is on the strip.
    // While already dragging (m_seamDragActive), every move counts —
    // we follow the cursor even past the original strip bounds. This
    // matches Main.qml's MouseArea behavior: the drag captures the
    // mouse on press and tracks until release.
    if (isPress && (xLogical < seamX - half || xLogical > seamX + half)) {
        return false;
    }

    const float clampedX = std::clamp(xLogical, 0.0f, w);
    emit splitWipeSeamDragged(clampedX / w);
    return true;
}

void PlayerWindow::exposeEvent(QExposeEvent *e)
{
    QWindow::exposeEvent(e);

    if (!isExposed()) return;

    // First-expose init: the surface is now backed and ready for the
    // renderer to bind. Phase B/C concrete renderers create their
    // swapchain here.
    if (m_renderer && !m_rendererInited) {
        if (m_renderer->init(this)) {
            m_rendererInited = true;
        } else {
            qWarning("PlayerWindow: renderer init() failed");
        }
    }

    if (m_renderer && m_rendererInited) {
        m_renderer->requestUpdate();
    }
}

void PlayerWindow::resizeEvent(QResizeEvent *e)
{
    QWindow::resizeEvent(e);
    if (m_renderer && m_rendererInited) {
        m_renderer->resize(e->size().width(), e->size().height());
    }
}

void PlayerWindow::mousePressEvent(QMouseEvent *e)
{
    QWindow::mousePressEvent(e);

    // Split-wipe seam grab wins over the annotator when the press
    // lands on the strip — matches the Windows QML order where
    // splitWipeSeamDrag MouseArea sits above the annotation forwarder.
    if (e->button() == Qt::LeftButton
        && handleSeamMouse(static_cast<float>(e->position().x()),
                           /*isPress=*/true)) {
        m_seamDragActive = true;
        setCursor(QCursor(Qt::SplitHCursor));
        return;
    }

    if (m_annotator) {
        // QMouseEvent::position() is in QWindow logical points; the
        // renderer's viewport rect + drawable are in pixels. Scale
        // by devicePixelRatio so the annotator's normalized coords
        // line up with the displayed pixels.
        const qreal dpr = devicePixelRatio();
        m_annotator->onPointerEvent(PointerPhase::Press,
                                    e->position() * dpr,
                                    static_cast<qint64>(e->timestamp()));
        // Wake the render thread — when the player is paused (or no
        // media is loaded yet) the loop is otherwise idle and the
        // newly-pushed active stroke isn't drawn until some other
        // event triggers a frame. Same idea applies on Move / Release.
        if (m_renderer && m_rendererInited) m_renderer->requestUpdate();
    }
}

void PlayerWindow::mouseMoveEvent(QMouseEvent *e)
{
    QWindow::mouseMoveEvent(e);

    if (m_seamDragActive) {
        // Active drag — follow the cursor regardless of strip bounds.
        handleSeamMouse(static_cast<float>(e->position().x()),
                        /*isPress=*/false);
        return;
    }

    // Hover-cursor swap: in Wipe mode with a live dual session, give a
    // SplitHCursor while the pointer hovers the seam strip so the user
    // can see it's grabbable. Outside the strip / not in Wipe / no
    // dual session, restore the default cursor. mouseMoveEvent fires
    // on plain hover too (no button required).
    if (m_compositorMode.load(std::memory_order_acquire) == 2
        && m_dualActive.load(std::memory_order_acquire)) {
        const float w = static_cast<float>(width());
        if (w > 0.0f) {
            const float seamX =
                std::clamp(m_splitPos.load(std::memory_order_acquire),
                           0.0f, 1.0f) * w;
            const float half  = kSeamGrabWidth * 0.5f;
            const float x     = static_cast<float>(e->position().x());
            if (x >= seamX - half && x <= seamX + half) {
                setCursor(QCursor(Qt::SplitHCursor));
            } else {
                unsetCursor();
            }
        }
    } else {
        unsetCursor();
    }

    if (m_annotator) {
        const qreal dpr = devicePixelRatio();
        m_annotator->onPointerEvent(PointerPhase::Move,
                                    e->position() * dpr,
                                    static_cast<qint64>(e->timestamp()));
        if (m_renderer && m_rendererInited) m_renderer->requestUpdate();
    }
}

void PlayerWindow::mouseReleaseEvent(QMouseEvent *e)
{
    QWindow::mouseReleaseEvent(e);

    if (m_seamDragActive && e->button() == Qt::LeftButton) {
        m_seamDragActive = false;
        // Don't unset the cursor here — the pointer is still hovering
        // the seam, the SplitHCursor should persist. mouseMoveEvent's
        // hover branch handles cursor cleanup once the pointer leaves.
        return;
    }

    if (m_annotator) {
        const qreal dpr = devicePixelRatio();
        m_annotator->onPointerEvent(PointerPhase::Release,
                                    e->position() * dpr,
                                    static_cast<qint64>(e->timestamp()));
        if (m_renderer && m_rendererInited) m_renderer->requestUpdate();
    }
}

void PlayerWindow::notifyFilesDropped(const QList<QUrl> &urls)
{
    if (urls.isEmpty()) return;
    emit filesDropped(urls);
}

bool PlayerWindow::event(QEvent *e)
{
    // QWindow doesn't expose dragEnterEvent / dropEvent virtuals
    // (those are QWidget-only); intercept via the generic event()
    // override + QEvent::type dispatch.
    switch (e->type()) {
        case QEvent::DragEnter: {
            auto *de = static_cast<QDragEnterEvent *>(e);
            if (de->mimeData()->hasUrls()) {
                de->acceptProposedAction();
                return true;
            }
            break;
        }
        case QEvent::DragMove: {
            auto *dm = static_cast<QDragMoveEvent *>(e);
            if (dm->mimeData()->hasUrls()) {
                dm->acceptProposedAction();
                return true;
            }
            break;
        }
        case QEvent::Drop: {
            auto *dr = static_cast<QDropEvent *>(e);
            if (dr->mimeData()->hasUrls()) {
                emit filesDropped(dr->mimeData()->urls());
                dr->acceptProposedAction();
                return true;
            }
            break;
        }
        default:
            break;
    }
    return QWindow::event(e);
}

} // namespace qcv
