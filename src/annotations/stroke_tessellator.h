// StrokeTessellator — direct lift from old QCView's
// src/annotations/stroke_tessellator.{h,cpp} per Guide 19 §2.3.
//
// Tessellates an ActiveStroke (or committed Stroke) into a triangle
// mesh suitable for direct upload to a vertex buffer. The resulting
// mesh is rendered by the player's stroke pass (Phase B.6.5 / C —
// per-platform shader).
//
// Pure CPU; no GPU dependencies. Reusable across Metal + Vulkan.
//
// Adaptation notes vs old app:
//   - Namespace qcview::Annotations → qcv
//   - ImVec2 → QPointF (Active stroke + screen coords)
//   - ImVec4 → QColor (stroke color; we extract RGBA floats)
//   - StrokeSmoother (deprecated upstream) NOT lifted; legacy v1.0
//     non-modeled strokes render as raw points. v2.0 modeled strokes
//     are already smoothed by ink-stroke-modeler (A.2.3).

#pragma once

#include "active_stroke.h"

#include <QPointF>
#include <vector>

namespace qcv {

struct TessVertex {
    float x, y;       // Position (pixel coordinates)
    float r, g, b, a; // Color (linear or sRGB; the stroke pass shader maps to working space)
};

struct TessellatedMesh {
    std::vector<TessVertex> vertices;  // Triangle list (3 vertices per triangle)
};

class StrokeTessellator {
public:
    // Tessellate a single stroke into a triangle mesh.
    //
    // displayPos / displaySize define the viewport rectangle in pixels.
    // lineWidthScale scales stroke width (typically viewport_size /
    // source_image_size — keeps stroke thickness perceptually constant
    // across zoom).
    static TessellatedMesh tessellate(
        const ActiveStroke &stroke,
        float displayPosX, float displayPosY,
        float displayWidth, float displayHeight,
        float lineWidthScale = 1.0f);

private:
    static void normalizedToScreen(
        float nx, float ny,
        float dpx, float dpy, float dw, float dh,
        float &ox, float &oy);

    // Expand a polyline into a triangle list with round caps + joins
    // (caps drawn as full-circle stamps at every vertex; cheap and
    // robust against angle artifacts).
    static void tessellatePolyline(
        const std::vector<QPointF> &screenPoints,
        float halfWidth,
        float r, float g, float b, float a,
        TessellatedMesh &mesh);

    static void addRoundCap(
        float cx, float cy,
        float halfWidth,
        float r, float g, float b, float a,
        TessellatedMesh &mesh);

    static void tessellateFilledRect(
        float x1, float y1, float x2, float y2,
        float r, float g, float b, float a,
        TessellatedMesh &mesh);

    static void tessellateFilledOval(
        float cx, float cy, float rx, float ry,
        float r, float g, float b, float a,
        TessellatedMesh &mesh);

    static void tessellateFilledTriangle(
        float x0, float y0,
        float x1, float y1,
        float x2, float y2,
        float r, float g, float b, float a,
        TessellatedMesh &mesh);
};

} // namespace qcv
