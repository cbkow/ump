#pragma once

#include <vector>
#include <imgui.h>
#include "viewport_annotator.h"

namespace qcview {
namespace Annotations {

struct TessVertex {
    float x, y;       // Position (pixel coordinates)
    float r, g, b, a; // Color
};

struct TessellatedMesh {
    std::vector<TessVertex> vertices;  // Triangle list
};

class StrokeTessellator {
public:
    // Tessellate a single stroke into a triangle mesh.
    // display_pos/display_size define the viewport rectangle.
    // line_width_scale scales stroke width (viewport_size / video_size).
    // apply_smoothing: apply Catmull-Rom to legacy (non-modeled) freehand strokes.
    static TessellatedMesh Tessellate(
        const ActiveStroke& stroke,
        float display_pos_x, float display_pos_y,
        float display_width, float display_height,
        float line_width_scale = 1.0f,
        bool apply_smoothing = true
    );

private:
    // Convert normalized coords to screen coords
    static void NormalizedToScreen(
        float nx, float ny,
        float dpx, float dpy, float dw, float dh,
        float& ox, float& oy
    );

    // Expand a polyline into a triangle strip with round caps and joins
    static void TessellatePolyline(
        const std::vector<ImVec2>& screen_points,
        float half_width,
        float r, float g, float b, float a,
        TessellatedMesh& mesh
    );

    // Add a round cap (semicircle fan) at a point
    static void AddRoundCap(
        float cx, float cy,
        float dx, float dy,  // direction of the line at the cap
        float half_width,
        bool is_start,
        float r, float g, float b, float a,
        TessellatedMesh& mesh
    );

    // Tessellate a filled rectangle (2 triangles)
    static void TessellateFilledRect(
        float x1, float y1, float x2, float y2,
        float r, float g, float b, float a,
        TessellatedMesh& mesh
    );

    // Tessellate a filled ellipse (triangle fan)
    static void TessellateFilledOval(
        float cx, float cy, float rx, float ry,
        float r, float g, float b, float a,
        TessellatedMesh& mesh
    );

    // Tessellate a filled triangle (arrowhead)
    static void TessellateFilledTriangle(
        float x0, float y0,
        float x1, float y1,
        float x2, float y2,
        float r, float g, float b, float a,
        TessellatedMesh& mesh
    );
};

} // namespace Annotations
} // namespace qcview
