// ViewportAnnotator — adapted from old QCView's
// src/annotations/viewport_annotator.{h,cpp} per Guide 19 §2.5.
//
// Manages annotation drawing state for one viewport. The old app
// polled ImGui::GetIO() for mouse state inside ProcessInput(); this
// port flips that to a push-based API: the caller (PlayerWindow's
// mousePress/Move/Release event handlers) calls onPointerEvent() and
// the annotator drives its stroke state machine off those events.
//
// Adaptation notes vs old app:
//   - ViewportMode::PLAYBACK / ANNOTATION → ::Playback / Annotation
//   - DrawingTool / ActiveStroke already in active_stroke.h (A.2.2)
//   - ImVec2 → QPointF, ImVec4 → QColor (both already adapted)
//   - ImGui::GetIO() polling → onPointerEvent(Phase, pos, t) push API
//   - chrono::steady_clock for stroke-relative timestamps → caller
//     supplies qint64 ms timestamp (QInputEvent::timestamp())
//   - allow_input_in_popup_ + IsPopupOpen suppression dropped: Qt
//     popups consume events natively before they reach this class
//   - Per-platform default stroke width preserved (#ifdef Q_OS_MACOS)

#pragma once

#include "active_stroke.h"

#include <QColor>
#include <QPointF>
#include <QSizeF>

#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace qcv {

class InkStrokeModelerWrapper;

enum class ViewportMode {
    Playback,    // Default: events pass through to playback / scrub.
    Annotation,  // Drawing tools active; events become stroke input.
};

enum class PointerPhase {
    Press,
    Move,
    Release,
};

class ViewportAnnotator {
public:
    ViewportAnnotator();
    ~ViewportAnnotator();   // out-of-line: InkStrokeModelerWrapper is incomplete here

    ViewportAnnotator(const ViewportAnnotator&)            = delete;
    ViewportAnnotator& operator=(const ViewportAnnotator&) = delete;

    // ---- Mode ----
    ViewportMode mode() const                  { return mode_; }
    void         setMode(ViewportMode m);
    ViewportMode toggleMode();
    bool         isAnnotationMode() const      { return mode_ == ViewportMode::Annotation; }

    // ---- Tool / style ----
    DrawingTool activeTool() const             { return active_tool_; }
    void        setActiveTool(DrawingTool t)   { active_tool_ = t; }

    QColor drawingColor() const                { return drawing_color_; }
    void   setDrawingColor(const QColor &c)    { drawing_color_ = c; }

    float strokeWidth() const                  { return stroke_width_; }
    void  setStrokeWidth(float w)              { stroke_width_ = w; }

    bool fillEnabled() const                   { return fill_enabled_; }
    void setFillEnabled(bool e)                { fill_enabled_ = e; }

    // ---- Viewport rect (display area in screen coordinates) ----
    // Update on every viewport resize / aspect-fit recompute.
    void setViewportRect(QPointF displayPos, QSizeF displaySize);

    // ---- Input ----
    // Returns true if the event was consumed by the annotation system.
    // `screenPos` is in viewport-window pixels (same coord system as
    // the display rect). `timestampMs` should monotonically increase;
    // QInputEvent::timestamp() satisfies that.
    bool onPointerEvent(PointerPhase phase, QPointF screenPos, qint64 timestampMs);

    // ---- Active stroke ----
    const ActiveStroke *activeStroke() const {
        return active_stroke_.tool != DrawingTool::None ? &active_stroke_ : nullptr;
    }
    void clearActiveStroke();

    // Thread-safe copy of the active stroke under active_stroke_mutex_.
    // Returns false if no stroke. Use this from the render thread —
    // dereferencing activeStroke() while the GUI thread mutates points
    // via onPointerEvent() trips Debug CRT's iterator-validity check.
    // Metal renderer can stay on the raw-pointer path (libc++'s
    // iterator debug is more permissive on macOS); D3D11 must
    // snapshot under lock.
    bool snapshotActiveStroke(ActiveStroke &out) const;

    // Pop the in-progress stroke off; caller takes ownership.
    // Returns nullptr if no stroke or stroke has no points.
    std::unique_ptr<ActiveStroke> finalizeStroke();

    // Set a callback fired right after a stroke completes (mouse
    // release with at least one point). The annotator hands the
    // finalized ActiveStroke to the callback; callee owns it. Pass
    // nullptr to detach. Used by WindowManager to bridge the
    // finished stroke into AnnotationManager (frame + JSON
    // serialization + sidecar save).
    using StrokeFinalizedCb =
        std::function<void(std::unique_ptr<ActiveStroke>)>;
    void setStrokeFinalizedCallback(StrokeFinalizedCb cb) {
        stroke_finalized_cb_ = std::move(cb);
    }

    // Fired on the Press of any drawing tool (Freehand, Rectangle,
    // Oval, Arrow, Line). NOT fired for Eraser. WindowManager uses
    // it to cancel the debounced annotated-thumbnail save so a
    // pending capture can't fire mid-stroke and stall the GUI
    // thread when stroke B starts right after stroke A.
    using StrokeStartedCb = std::function<void()>;
    void setStrokeStartedCallback(StrokeStartedCb cb) {
        stroke_started_cb_ = std::move(cb);
    }

    // Phase 3.H.6 Stage E — fired on Press / Move events while
    // the active tool is Eraser. The callback receives a normalized
    // (0..1) point in the viewport's coordinate space; WindowManager
    // hit-tests against stored strokes at the current frame and
    // removes the first match.
    using EraseAtCb = std::function<void(QPointF normalizedPos)>;
    void setEraseAtCallback(EraseAtCb cb) {
        erase_at_cb_ = std::move(cb);
    }

    // Cancel the in-flight stroke (Esc during a drag). Drops
    // active_stroke_ + clears is_drawing_; the finalize callback
    // is NOT fired.
    void cancelActiveStroke();

    // ---- Coord helpers (static, useful elsewhere) ----
    static QPointF screenToNormalized(QPointF screenPos,
                                      QPointF displayPos,
                                      QSizeF displaySize);
    static QPointF normalizedToScreen(QPointF normalizedPos,
                                      QPointF displayPos,
                                      QSizeF displaySize);
    static bool    isInsideDisplayArea(QPointF screenPos,
                                       QPointF displayPos,
                                       QSizeF displaySize);

private:
    void processFreehand (PointerPhase phase, QPointF screenPos, qint64 timestampMs);
    void processRectangle(PointerPhase phase, QPointF screenPos, qint64 timestampMs);
    void processOval     (PointerPhase phase, QPointF screenPos, qint64 timestampMs);
    void processArrow    (PointerPhase phase, QPointF screenPos, qint64 timestampMs);
    void processLine     (PointerPhase phase, QPointF screenPos, qint64 timestampMs);

    ViewportMode mode_         = ViewportMode::Playback;
    DrawingTool  active_tool_  = DrawingTool::None;
    QColor       drawing_color_ = QColor(255, 0, 0, 255);
#ifdef Q_OS_MACOS
    float        stroke_width_ = 6.0f;
#else
    float        stroke_width_ = 4.0f;
#endif
    bool         fill_enabled_ = false;

    QPointF display_pos_;
    QSizeF  display_size_;

    ActiveStroke active_stroke_;
    // Protects active_stroke_'s vectors from concurrent reads on the
    // render thread while onPointerEvent() (GUI thread) mutates them.
    // Locking is fine-grained: pointer event path takes it for the
    // duration of one event, snapshotActiveStroke() takes it for one
    // copy. No call into Qt or modeler is made while held.
    mutable std::mutex active_stroke_mutex_;
    bool         is_drawing_ = false;

    QPointF drag_start_norm_;       // normalized 0..1 anchor for shape tools

    // Stroke-relative time base (so the modeler sees t=0 at first
    // point regardless of QInputEvent::timestamp() epoch).
    qint64 stroke_t0_ms_ = 0;

    std::unique_ptr<InkStrokeModelerWrapper> stroke_modeler_;

    StrokeFinalizedCb stroke_finalized_cb_;
    StrokeStartedCb   stroke_started_cb_;
    EraseAtCb         erase_at_cb_;
};

} // namespace qcv
