// PlayerWindow — Phase 7.5 A.3 / Guide 20.
//
// Native QWindow that hosts the player surface (Metal CAMetalLayer on
// macOS, Vulkan VkSurfaceKHR on Win/Linux). Owns its IPlayerRenderer
// instance and the dedicated render thread that drives present.
//
// Phase A scope: skeleton. The window opens with the correct
// SurfaceType, the renderer's init() is called on first expose, but
// nothing gets drawn — the OS-managed black layer-backing is the
// "render". Phase B / C wire in actual rendering.
//
// Pointer/tablet event handlers (mousePressEvent, mouseMoveEvent,
// mouseReleaseEvent) forward to ViewportAnnotator when the window's
// owner has installed one. Phase B.7 wires that up.

#pragma once

#include <QList>
#include <QUrl>
#include <QWindow>

#include <atomic>
#include <memory>

namespace qcv {

class IPlayerRenderer;
class ViewportAnnotator;

class PlayerWindow : public QWindow {
    Q_OBJECT

public:
    explicit PlayerWindow(QWindow *parent = nullptr);
    ~PlayerWindow() override;

    PlayerWindow(const PlayerWindow &)            = delete;
    PlayerWindow &operator=(const PlayerWindow &) = delete;

    IPlayerRenderer *renderer() const { return m_renderer.get(); }

    // Plug in a viewport annotator; when set, pointer events
    // (mousePress/Move/Release) are forwarded to it via
    // onPointerEvent() in addition to being bubbled up the QWindow
    // chain. Set to nullptr to detach.
    void setViewportAnnotator(ViewportAnnotator *annotator);

    // Split-wipe seam drag — macOS-only path mirroring Main.qml's
    // MouseArea (which only works on Windows because the D3D11 child
    // HWND is HTTRANSPARENT). PlayerWindow owns the QWindow on macOS
    // and absorbs every click, so the QML seam strip is occluded.
    // WindowManager pushes mode + splitPos here and we intercept
    // mouse events that land inside a 14-px strip centered on the
    // seam, emitting splitWipeSeamDragged with the new normalized
    // x. Outside the strip (or in non-Wipe modes) events forward to
    // the annotator as before.
    void setCompositorMode(int mode);
    void setSplitPos(float pos);
    // True iff a DualPlaybackController is currently active. The seam
    // strip only makes sense in dual flow — the QML version on Windows
    // gates the equivalent MouseArea on `WindowManager.dualController`
    // for the same reason. Without this, compositorMode lingering at
    // SplitWipe across a dual-exit (or persisted from a prior session)
    // would still produce the SplitHCursor on hover in single flow.
    void setDualActive(bool active);

    // Phase F.2.11.x — Windows D3D11 child HWND IDropTarget callback
    // entry. OLE drag-drop on Windows is HWND-based (RegisterDragDrop)
    // and does NOT fall through HTTRANSPARENT like mouse clicks, so
    // the player's child HWND blocks every QML DropArea behind it.
    // The renderer's IDropTarget calls this on drop to re-emit the
    // same filesDropped signal macOS already wires through QWindow's
    // event() override, keeping WindowManager's handler shared.
    void notifyFilesDropped(const QList<QUrl> &urls);

Q_SIGNALS:
    // Phase 7.5 B.7 — file drag-drop. Native QWindow sits above the
    // UI window's centerStage in z-order, so QML DropArea overlays
    // can't see the drop. WindowManager connects this signal and
    // routes URLs through ProjectManager::addMediaFile (matches the
    // existing QML path).
    void filesDropped(const QList<QUrl> &urls);

    // Fired during a split-wipe seam drag. `normalizedX` is the new
    // splitPos in [0, 1] derived from the cursor location. WindowManager
    // listens and writes back to its splitPos Q_PROPERTY (which then
    // round-trips back via setSplitPos so the next mouse-move sees the
    // committed value).
    void splitWipeSeamDragged(float normalizedX);

protected:
    // QWindow lifecycle hooks. Phase B/C concrete renderers care.
    void exposeEvent(QExposeEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;

    // Pointer events → ViewportAnnotator (when set).
    void mousePressEvent  (QMouseEvent *e) override;
    void mouseMoveEvent   (QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;

    // Drag-and-drop event handling routes through QWindow::event()
    // since QWindow doesn't expose dragEnterEvent / dropEvent
    // virtuals directly (those are QWidget-only).
    bool event(QEvent *e) override;

private:
    // Returns true and emits splitWipeSeamDragged if (mode == 2 && the
    // cursor at `xLogical` is inside the seam grab strip). Caller uses
    // the bool to decide whether to also forward to the annotator.
    bool handleSeamMouse(float xLogical, bool isPress);

    // Lights the split-wipe seam full-white while hovered or dragged
    // (grey at rest), via the renderer's setSplitSeamHighlight. This is
    // the macOS equivalent of Main.qml's setSplitSeamActive(containsMouse
    // || pressed) — the QML MouseArea is occluded by this native window,
    // so the highlight has to be driven here. No-ops unless the state
    // actually changes so we don't re-push + requestUpdate every move.
    void updateSeamHighlight(bool active);

    std::unique_ptr<IPlayerRenderer> m_renderer;
    ViewportAnnotator               *m_annotator      = nullptr;  // not owned
    bool                             m_rendererInited = false;

    // Split-wipe seam state. Updated by setCompositorMode / setSplitPos
    // from the GUI thread; read in mouse event handlers (also GUI
    // thread). Atomic on principle so a future move off-thread doesn't
    // bite.
    std::atomic<int>   m_compositorMode{0};
    std::atomic<float> m_splitPos{0.5f};
    std::atomic<bool>  m_dualActive{false};
    bool               m_seamDragActive = false;
    bool               m_seamHighlightOn = false;
};

} // namespace qcv
