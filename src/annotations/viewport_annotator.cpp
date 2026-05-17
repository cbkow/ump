// ViewportAnnotator — see header for adaptation notes.

#include "viewport_annotator.h"

#include "ink_stroke_modeler_wrapper.h"

#include <algorithm>
#include <cmath>

namespace qcv {

namespace {

// Clamp helper for QPointF (QPointF doesn't ship one).
inline qreal clamp01(qreal v) { return std::clamp<qreal>(v, 0.0, 1.0); }

} // namespace

ViewportAnnotator::ViewportAnnotator()
    : stroke_modeler_(std::make_unique<InkStrokeModelerWrapper>())
{
}

ViewportAnnotator::~ViewportAnnotator() = default;

void ViewportAnnotator::setMode(ViewportMode m)
{
    if (mode_ == m) return;

    // Discard any in-progress stroke when leaving annotation mode.
    if (mode_ == ViewportMode::Annotation && m == ViewportMode::Playback) {
        clearActiveStroke();
    }
    mode_ = m;
}

ViewportMode ViewportAnnotator::toggleMode()
{
    setMode(mode_ == ViewportMode::Playback
                ? ViewportMode::Annotation
                : ViewportMode::Playback);
    return mode_;
}

void ViewportAnnotator::setViewportRect(QPointF displayPos, QSizeF displaySize)
{
    display_pos_  = displayPos;
    display_size_ = displaySize;
}

void ViewportAnnotator::clearActiveStroke()
{
    std::lock_guard lock(active_stroke_mutex_);
    active_stroke_.clear();
    is_drawing_ = false;
}

void ViewportAnnotator::cancelActiveStroke()
{
    // Esc-mid-drag: drop the in-flight stroke without firing the
    // finalize callback. Caller (WindowManager event filter)
    // gates this on isAnnotationMode + is_drawing_ so we don't
    // no-op the user's regular Esc. Modeler state is left as-is
    // — the next stroke's StartStroke resets it.
    if (!is_drawing_) return;
    std::lock_guard lock(active_stroke_mutex_);
    active_stroke_.clear();
    is_drawing_ = false;
}

bool ViewportAnnotator::snapshotActiveStroke(ActiveStroke &out) const
{
    std::lock_guard lock(active_stroke_mutex_);
    if (active_stroke_.tool == DrawingTool::None) return false;
    out = active_stroke_;   // deep copy of vectors under the lock
    return true;
}

std::unique_ptr<ActiveStroke> ViewportAnnotator::finalizeStroke()
{
    std::lock_guard lock(active_stroke_mutex_);
    if (active_stroke_.tool == DrawingTool::None || active_stroke_.points.empty()) {
        return nullptr;
    }
    auto out = std::make_unique<ActiveStroke>(active_stroke_);
    out->isComplete = true;
    // NB: don't clearActiveStroke() here — the caller is responsible
    // for clearing AFTER the stroke-finalized callback has pushed
    // the new stored stroke into the renderer. Clearing first opens
    // a 1–2 frame window where the renderer sees neither the old
    // active nor the new stored stroke (visible flash on release).
    return out;
}

bool ViewportAnnotator::onPointerEvent(PointerPhase phase,
                                       QPointF screenPos,
                                       qint64 timestampMs)
{
    if (mode_ != ViewportMode::Annotation) return false;
    if (active_tool_ == DrawingTool::None)  return false;

    // Stroke-started hook. Fires on the Press of any drawing tool;
    // WindowManager listens so a pending debounced annotated-thumb
    // capture can be cancelled before it fires mid-stroke and
    // stalls the GUI thread.
    if (phase == PointerPhase::Press
        && active_tool_ != DrawingTool::Eraser
        && stroke_started_cb_) {
        stroke_started_cb_();
    }

    // Lock just around the dispatch so the render thread's
    // snapshotActiveStroke() observes a coherent active_stroke_ — the
    // process* helpers mutate points + timestamps vectors, and a
    // concurrent read of a half-grown vector trips Debug CRT's
    // iterator validity checks (and is UB in any case). The lock is
    // released before the Release-block below because finalizeStroke
    // and clearActiveStroke both reacquire the same mutex; std::mutex
    // is non-recursive and reentering deadlocks on Windows.
    // onPointerEvent runs on the GUI thread (Qt event loop), so
    // single-thread ordering guarantees nothing mutates active_stroke_
    // between this lock release and the post-switch reads.
    {
    std::lock_guard active_stroke_lock(active_stroke_mutex_);

    switch (active_tool_) {
        case DrawingTool::Freehand:  processFreehand (phase, screenPos, timestampMs); break;
        case DrawingTool::Rectangle: processRectangle(phase, screenPos, timestampMs); break;
        case DrawingTool::Oval:      processOval     (phase, screenPos, timestampMs); break;
        case DrawingTool::Arrow:     processArrow    (phase, screenPos, timestampMs); break;
        case DrawingTool::Line:      processLine     (phase, screenPos, timestampMs); break;
        case DrawingTool::Eraser: {
            // Erase only while the user is actively pressing —
            // press to begin, drag to continue, release to stop.
            // Hover (Move without a prior Press) is ignored; the
            // gesture catcher sends Move events any time the
            // cursor is over the viewport, and we don't want the
            // user to lose strokes just by passing the mouse over
            // them.
            if (phase == PointerPhase::Press) {
                is_drawing_ = true;
            } else if (phase == PointerPhase::Release) {
                is_drawing_ = false;
                return true;
            } else if (!is_drawing_) {
                return true;   // hover / pre-press Move → no-op
            }
            if (!isInsideDisplayArea(screenPos, display_pos_,
                                       display_size_)) return true;
            const QPointF norm = screenToNormalized(screenPos,
                                                     display_pos_,
                                                     display_size_);
            if (erase_at_cb_) erase_at_cb_(norm);
            return true;
        }
        case DrawingTool::None:
        default:                     return false;
    }
    }  // active_stroke_lock released — finalize/clear below reacquire it

    // Phase 3.H.6 — single hook point for stroke completion. Each
    // processX method sets active_stroke_.isComplete on Release.
    // Pop the stroke + hand it to the callback (WindowManager
    // bridges into AnnotationManager: addNote with current frame +
    // serialized JSON, async sidecar save).
    //
    // Order matters: callback first, clearActiveStroke() second.
    // The callback runs synchronously and updates the renderer's
    // stored-strokes list; only after that's done is it safe to
    // clear the active stroke. Otherwise the renderer briefly sees
    // neither (visible flash on release).
    if (phase == PointerPhase::Release
        && active_stroke_.isComplete
        && stroke_finalized_cb_) {
        if (auto stroke = finalizeStroke()) {
            stroke_finalized_cb_(std::move(stroke));
            clearActiveStroke();
        }
    }
    return true;
}

QPointF ViewportAnnotator::screenToNormalized(QPointF screenPos,
                                              QPointF displayPos,
                                              QSizeF displaySize)
{
    if (displaySize.width() <= 0.0 || displaySize.height() <= 0.0) {
        return {};
    }
    return QPointF(
        clamp01((screenPos.x() - displayPos.x()) / displaySize.width()),
        clamp01((screenPos.y() - displayPos.y()) / displaySize.height()));
}

QPointF ViewportAnnotator::normalizedToScreen(QPointF normalizedPos,
                                              QPointF displayPos,
                                              QSizeF displaySize)
{
    return QPointF(
        displayPos.x() + normalizedPos.x() * displaySize.width(),
        displayPos.y() + normalizedPos.y() * displaySize.height());
}

bool ViewportAnnotator::isInsideDisplayArea(QPointF screenPos,
                                            QPointF displayPos,
                                            QSizeF displaySize)
{
    return screenPos.x() >= displayPos.x() &&
           screenPos.x() <= displayPos.x() + displaySize.width() &&
           screenPos.y() >= displayPos.y() &&
           screenPos.y() <= displayPos.y() + displaySize.height();
}

// ---------------------------------------------------------------------
// Freehand
// ---------------------------------------------------------------------

void ViewportAnnotator::processFreehand(PointerPhase phase,
                                        QPointF screenPos,
                                        qint64 timestampMs)
{
    const bool inside = isInsideDisplayArea(screenPos, display_pos_, display_size_);

    // Mouse left the display area mid-stroke → finalize (matches old
    // app behavior; only Freehand auto-finalizes on leave).
    if (is_drawing_ && phase == PointerPhase::Move && !inside) {
        if (stroke_modeler_ && stroke_modeler_->IsActive()) {
            const QPointF lastNorm = active_stroke_.points.empty()
                ? screenToNormalized(screenPos, display_pos_, display_size_)
                : active_stroke_.points.back();
            const double t = (timestampMs - stroke_t0_ms_) / 1000.0;
            for (const QPointF &pt : stroke_modeler_->EndStroke(lastNorm, t)) {
                active_stroke_.points.push_back(pt);
                active_stroke_.timestamps.push_back(static_cast<qint64>(t * 1000.0));
            }
        }
        active_stroke_.isComplete = true;
        is_drawing_ = false;
        return;
    }

    if (!inside && phase != PointerPhase::Release) return;

    if (phase == PointerPhase::Press) {
        is_drawing_ = true;
        active_stroke_.clear();
        active_stroke_.tool        = DrawingTool::Freehand;
        active_stroke_.color       = drawing_color_;
        active_stroke_.strokeWidth = stroke_width_;
        active_stroke_.isModeled   = true;

        stroke_t0_ms_ = timestampMs;

        if (stroke_modeler_) {
            InkStrokeModelerWrapper::Config cfg;
            stroke_modeler_->BeginStroke(cfg);
        }

        const QPointF norm = screenToNormalized(screenPos, display_pos_, display_size_);
        if (stroke_modeler_ && stroke_modeler_->IsActive()) {
            for (const QPointF &pt : stroke_modeler_->AddPoint(norm, 0.0, true)) {
                active_stroke_.points.push_back(pt);
                active_stroke_.timestamps.push_back(0);
            }
        } else {
            active_stroke_.points.push_back(norm);
            active_stroke_.timestamps.push_back(0);
            active_stroke_.isModeled = false;
        }
        return;
    }

    if (!is_drawing_) return;

    if (phase == PointerPhase::Move) {
        const QPointF norm = screenToNormalized(screenPos, display_pos_, display_size_);
        const double  t    = (timestampMs - stroke_t0_ms_) / 1000.0;

        if (stroke_modeler_ && stroke_modeler_->IsActive()) {
            for (const QPointF &pt : stroke_modeler_->AddPoint(norm, t, false)) {
                active_stroke_.points.push_back(pt);
                active_stroke_.timestamps.push_back(static_cast<qint64>(t * 1000.0));
            }
        } else if (active_stroke_.points.empty() ||
                   active_stroke_.points.back() != norm) {
            active_stroke_.points.push_back(norm);
            active_stroke_.timestamps.push_back(static_cast<qint64>(t * 1000.0));
        }
        return;
    }

    if (phase == PointerPhase::Release) {
        const QPointF norm = screenToNormalized(screenPos, display_pos_, display_size_);
        const double  t    = (timestampMs - stroke_t0_ms_) / 1000.0;

        if (stroke_modeler_ && stroke_modeler_->IsActive()) {
            for (const QPointF &pt : stroke_modeler_->EndStroke(norm, t)) {
                active_stroke_.points.push_back(pt);
                active_stroke_.timestamps.push_back(static_cast<qint64>(t * 1000.0));
            }
        }
        active_stroke_.isComplete = true;
        is_drawing_ = false;
    }
}

// ---------------------------------------------------------------------
// Rectangle
// ---------------------------------------------------------------------

void ViewportAnnotator::processRectangle(PointerPhase phase,
                                         QPointF screenPos,
                                         qint64 /*timestampMs*/)
{
    if (!isInsideDisplayArea(screenPos, display_pos_, display_size_) &&
        phase != PointerPhase::Release) {
        return;
    }

    if (phase == PointerPhase::Press) {
        is_drawing_ = true;
        active_stroke_.clear();
        active_stroke_.tool        = DrawingTool::Rectangle;
        active_stroke_.color       = drawing_color_;
        active_stroke_.strokeWidth = stroke_width_;
        active_stroke_.filled      = fill_enabled_;
        drag_start_norm_ = screenToNormalized(screenPos, display_pos_, display_size_);
        return;
    }

    if (!is_drawing_) return;

    if (phase == PointerPhase::Move) {
        const QPointF cur = screenToNormalized(screenPos, display_pos_, display_size_);
        active_stroke_.points = {
            QPointF(drag_start_norm_.x(), drag_start_norm_.y()),  // TL
            QPointF(cur.x(),              drag_start_norm_.y()),  // TR
            QPointF(cur.x(),              cur.y()),               // BR
            QPointF(drag_start_norm_.x(), cur.y()),               // BL
        };
        return;
    }

    if (phase == PointerPhase::Release) {
        active_stroke_.isComplete = true;
        is_drawing_ = false;
    }
}

// ---------------------------------------------------------------------
// Oval (stored as center + radii)
// ---------------------------------------------------------------------

void ViewportAnnotator::processOval(PointerPhase phase,
                                    QPointF screenPos,
                                    qint64 /*timestampMs*/)
{
    if (!isInsideDisplayArea(screenPos, display_pos_, display_size_) &&
        phase != PointerPhase::Release) {
        return;
    }

    if (phase == PointerPhase::Press) {
        is_drawing_ = true;
        active_stroke_.clear();
        active_stroke_.tool        = DrawingTool::Oval;
        active_stroke_.color       = drawing_color_;
        active_stroke_.strokeWidth = stroke_width_;
        active_stroke_.filled      = fill_enabled_;
        drag_start_norm_ = screenToNormalized(screenPos, display_pos_, display_size_);
        return;
    }

    if (!is_drawing_) return;

    if (phase == PointerPhase::Move) {
        const QPointF cur = screenToNormalized(screenPos, display_pos_, display_size_);
        const QPointF center((drag_start_norm_.x() + cur.x()) * 0.5,
                             (drag_start_norm_.y() + cur.y()) * 0.5);
        const QPointF radii(std::abs(cur.x() - drag_start_norm_.x()) * 0.5,
                            std::abs(cur.y() - drag_start_norm_.y()) * 0.5);
        active_stroke_.points = { center, radii };
        return;
    }

    if (phase == PointerPhase::Release) {
        active_stroke_.isComplete = true;
        is_drawing_ = false;
    }
}

// ---------------------------------------------------------------------
// Arrow / Line — same shape (start, end), different rendering
// ---------------------------------------------------------------------

void ViewportAnnotator::processArrow(PointerPhase phase,
                                     QPointF screenPos,
                                     qint64 /*timestampMs*/)
{
    if (!isInsideDisplayArea(screenPos, display_pos_, display_size_) &&
        phase != PointerPhase::Release) {
        return;
    }

    if (phase == PointerPhase::Press) {
        is_drawing_ = true;
        active_stroke_.clear();
        active_stroke_.tool        = DrawingTool::Arrow;
        active_stroke_.color       = drawing_color_;
        active_stroke_.strokeWidth = stroke_width_;
        drag_start_norm_ = screenToNormalized(screenPos, display_pos_, display_size_);
        active_stroke_.points.push_back(drag_start_norm_);
        return;
    }

    if (!is_drawing_) return;

    if (phase == PointerPhase::Move) {
        const QPointF cur = screenToNormalized(screenPos, display_pos_, display_size_);
        if (active_stroke_.points.size() < 2) active_stroke_.points.push_back(cur);
        else                                  active_stroke_.points[1] = cur;
        return;
    }

    if (phase == PointerPhase::Release) {
        active_stroke_.isComplete = true;
        is_drawing_ = false;
    }
}

void ViewportAnnotator::processLine(PointerPhase phase,
                                    QPointF screenPos,
                                    qint64 /*timestampMs*/)
{
    if (!isInsideDisplayArea(screenPos, display_pos_, display_size_) &&
        phase != PointerPhase::Release) {
        return;
    }

    if (phase == PointerPhase::Press) {
        is_drawing_ = true;
        active_stroke_.clear();
        active_stroke_.tool        = DrawingTool::Line;
        active_stroke_.color       = drawing_color_;
        active_stroke_.strokeWidth = stroke_width_;
        drag_start_norm_ = screenToNormalized(screenPos, display_pos_, display_size_);
        active_stroke_.points.push_back(drag_start_norm_);
        return;
    }

    if (!is_drawing_) return;

    if (phase == PointerPhase::Move) {
        const QPointF cur = screenToNormalized(screenPos, display_pos_, display_size_);
        if (active_stroke_.points.size() < 2) active_stroke_.points.push_back(cur);
        else                                  active_stroke_.points[1] = cur;
        return;
    }

    if (phase == PointerPhase::Release) {
        active_stroke_.isComplete = true;
        is_drawing_ = false;
    }
}

} // namespace qcv
