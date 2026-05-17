// ActiveStroke + DrawingTool — lifted from old QCView's
// src/annotations/viewport_annotator.h:30-65 per Guide 19 §2.3.
//
// This file holds only the transient drawing-state types — the stroke
// the user is currently drawing, plus the tool enum. The full data
// model (committed Stroke, AnnotationNote, AnnotationManager, JSON
// serde) lives in separate files in this directory and ships in
// Phase A.2.4.
//
// The structures are CPU-only; consumed by StrokeTessellator
// (A.2.2 — produces vertex meshes) and ViewportAnnotator
// (A.2.5 — owns input handling + the active stroke).

#pragma once

#include <QColor>
#include <QPointF>
#include <QtTypes>
#include <vector>

namespace qcv {

enum class DrawingTool {
    None,
    Freehand,
    Rectangle,
    Oval,
    Arrow,
    Line,
    // Phase 3.H.6 Stage E — not a stroke creator. Pointer events
    // in this mode fire an erase callback against stored strokes
    // instead of building a new ActiveStroke. Hit detection +
    // mutation live in WindowManager (it has the AnnotationManager
    // reference); ViewportAnnotator just routes the event.
    Eraser,
};

// A drawing stroke or shape being created. Lives on the CPU side
// (owned by ViewportAnnotator); read each render frame by the
// stroke pass when populating the active-stroke portion of the
// shared vertex buffer.
struct ActiveStroke {
    DrawingTool          tool = DrawingTool::None;
    std::vector<QPointF> points;        // Normalized [0..1] coords
    std::vector<qint64>  timestamps;    // ms since stroke start, per point

    QColor color       = QColor(255, 0, 0, 255);  // sRGB; render-time srgbToWorking maps to HDR
    float  strokeWidth = 4.0f;          // logical pixels
    bool   filled      = false;         // Rectangle / Oval only
    bool   isComplete  = false;
    bool   isModeled   = false;         // true once ink-stroke-modeler has smoothed the points

    void clear() {
        tool = DrawingTool::None;
        points.clear();
        timestamps.clear();
        filled = false;
        isComplete = false;
        isModeled = false;
    }

    bool empty() const { return points.empty(); }
};

} // namespace qcv
