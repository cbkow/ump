#include "stroke_tessellator.h"
#include "stroke_smoother.h"
#include <cmath>

namespace qcview {
namespace Annotations {

static constexpr int kMinCircleSegments = 16;  // Minimum circle segments
static constexpr int kOvalSegments = 64;        // Segments for oval approximation
static constexpr float kPi = 3.14159265358979323846f;

// Adaptive segment count for full circle: ~1.5px arc length per segment
static int CircleSegments(float radius) {
    int segs = static_cast<int>(std::ceil(2.0f * kPi * radius / 1.5f));
    return std::max(segs, kMinCircleSegments);
}

void StrokeTessellator::NormalizedToScreen(
    float nx, float ny,
    float dpx, float dpy, float dw, float dh,
    float& ox, float& oy
) {
    ox = dpx + (nx * dw);
    oy = dpy + (ny * dh);
}

TessellatedMesh StrokeTessellator::Tessellate(
    const ActiveStroke& stroke,
    float display_pos_x, float display_pos_y,
    float display_width, float display_height,
    float line_width_scale,
    bool apply_smoothing
) {
    TessellatedMesh mesh;

    if (stroke.points.empty()) return mesh;

    float r = stroke.color.x;
    float g = stroke.color.y;
    float b = stroke.color.z;
    float a = stroke.color.w;
    float scaled_width = stroke.stroke_width * line_width_scale;
    float half_width = scaled_width * 0.5f;

    switch (stroke.tool) {
        case DrawingTool::FREEHAND: {
            std::vector<ImVec2> points_to_render = stroke.points;

            // Apply Catmull-Rom smoothing only for legacy strokes
            if (apply_smoothing && !stroke.is_modeled && stroke.points.size() >= 4) {
                StrokeSmoother::SmoothingConfig config;
                points_to_render = StrokeSmoother::SmoothStroke(stroke.points, config);
            }

            if (points_to_render.size() < 2) return mesh;

            // Convert to screen coords
            std::vector<ImVec2> screen_pts(points_to_render.size());
            for (size_t i = 0; i < points_to_render.size(); ++i) {
                NormalizedToScreen(points_to_render[i].x, points_to_render[i].y,
                                   display_pos_x, display_pos_y, display_width, display_height,
                                   screen_pts[i].x, screen_pts[i].y);
            }

            TessellatePolyline(screen_pts, half_width, r, g, b, a, mesh);
            break;
        }

        case DrawingTool::RECTANGLE: {
            if (stroke.points.size() < 4) return mesh;

            float x1, y1, x2, y2;
            NormalizedToScreen(stroke.points[0].x, stroke.points[0].y,
                               display_pos_x, display_pos_y, display_width, display_height, x1, y1);
            NormalizedToScreen(stroke.points[2].x, stroke.points[2].y,
                               display_pos_x, display_pos_y, display_width, display_height, x2, y2);

            if (stroke.filled) {
                TessellateFilledRect(x1, y1, x2, y2, r, g, b, a, mesh);
            } else {
                // 4-segment closed polyline
                std::vector<ImVec2> rect_pts = {
                    {x1, y1}, {x2, y1}, {x2, y2}, {x1, y2}, {x1, y1}
                };
                TessellatePolyline(rect_pts, half_width, r, g, b, a, mesh);
            }
            break;
        }

        case DrawingTool::OVAL: {
            if (stroke.points.size() < 2) return mesh;

            float cx, cy;
            NormalizedToScreen(stroke.points[0].x, stroke.points[0].y,
                               display_pos_x, display_pos_y, display_width, display_height, cx, cy);
            float rx = stroke.points[1].x * display_width;
            float ry = stroke.points[1].y * display_height;

            if (stroke.filled) {
                TessellateFilledOval(cx, cy, rx, ry, r, g, b, a, mesh);
            } else {
                // Approximate ellipse as polyline
                std::vector<ImVec2> oval_pts(kOvalSegments + 1);
                for (int i = 0; i <= kOvalSegments; ++i) {
                    float angle = 2.0f * kPi * i / kOvalSegments;
                    oval_pts[i] = {cx + rx * std::cos(angle), cy + ry * std::sin(angle)};
                }
                TessellatePolyline(oval_pts, half_width, r, g, b, a, mesh);
            }
            break;
        }

        case DrawingTool::ARROW: {
            if (stroke.points.size() < 2) return mesh;

            float start_x, start_y, end_x, end_y;
            NormalizedToScreen(stroke.points[0].x, stroke.points[0].y,
                               display_pos_x, display_pos_y, display_width, display_height, start_x, start_y);
            NormalizedToScreen(stroke.points[1].x, stroke.points[1].y,
                               display_pos_x, display_pos_y, display_width, display_height, end_x, end_y);

            float dir_x = end_x - start_x;
            float dir_y = end_y - start_y;
            float length = std::sqrt(dir_x * dir_x + dir_y * dir_y);
            if (length < 0.001f) return mesh;

            dir_x /= length;
            dir_y /= length;

            float arrow_size = 12.0f + scaled_width * 2.0f;
            float arrow_angle = 0.436332f;  // 25 degrees
            float cos_angle = std::cos(arrow_angle);
            float sin_angle = std::sin(arrow_angle);

            float arrow_back_dist = arrow_size * cos_angle;
            float line_end_x = end_x - dir_x * arrow_back_dist;
            float line_end_y = end_y - dir_y * arrow_back_dist;

            // Shaft line
            std::vector<ImVec2> line_pts = {{start_x, start_y}, {line_end_x, line_end_y}};
            TessellatePolyline(line_pts, half_width, r, g, b, a, mesh);

            // Arrowhead triangle
            float a1x = end_x - arrow_size * (dir_x * cos_angle - dir_y * sin_angle);
            float a1y = end_y - arrow_size * (dir_y * cos_angle + dir_x * sin_angle);
            float a2x = end_x - arrow_size * (dir_x * cos_angle + dir_y * sin_angle);
            float a2y = end_y - arrow_size * (dir_y * cos_angle - dir_x * sin_angle);

            TessellateFilledTriangle(end_x, end_y, a1x, a1y, a2x, a2y, r, g, b, a, mesh);
            break;
        }

        case DrawingTool::LINE: {
            if (stroke.points.size() < 2) return mesh;

            float start_x, start_y, end_x, end_y;
            NormalizedToScreen(stroke.points[0].x, stroke.points[0].y,
                               display_pos_x, display_pos_y, display_width, display_height, start_x, start_y);
            NormalizedToScreen(stroke.points[1].x, stroke.points[1].y,
                               display_pos_x, display_pos_y, display_width, display_height, end_x, end_y);

            std::vector<ImVec2> line_pts = {{start_x, start_y}, {end_x, end_y}};
            TessellatePolyline(line_pts, half_width, r, g, b, a, mesh);
            break;
        }

        default:
            break;
    }

    return mesh;
}

void StrokeTessellator::TessellatePolyline(
    const std::vector<ImVec2>& screen_points,
    float half_width,
    float r, float g, float b, float a,
    TessellatedMesh& mesh
) {
    if (screen_points.size() < 2) return;

    // Precompute segment directions and normals
    size_t n = screen_points.size();
    struct SegInfo {
        float dx, dy;  // direction
        float nx, ny;  // normal (left-hand perpendicular)
        float len;
    };
    std::vector<SegInfo> segs(n - 1);

    for (size_t i = 0; i < n - 1; ++i) {
        float dx = screen_points[i + 1].x - screen_points[i].x;
        float dy = screen_points[i + 1].y - screen_points[i].y;
        float len = std::sqrt(dx * dx + dy * dy);
        if (len < 1e-6f) len = 1e-6f;
        segs[i].dx = dx / len;
        segs[i].dy = dy / len;
        segs[i].nx = -segs[i].dy;
        segs[i].ny = segs[i].dx;
        segs[i].len = len;
    }

    // Circle at every vertex (acts as round brush stamp — handles caps and joins)
    for (size_t i = 0; i < n; ++i) {
        AddRoundCap(screen_points[i].x, screen_points[i].y,
                    0, 0, half_width, false, r, g, b, a, mesh);
    }

    // Connecting quads between adjacent circles
    for (size_t i = 0; i < n - 1; ++i) {
        float nx = segs[i].nx * half_width;
        float ny = segs[i].ny * half_width;

        const auto& p0 = screen_points[i];
        const auto& p1 = screen_points[i + 1];

        TessVertex v0 = {p0.x + nx, p0.y + ny, r, g, b, a};
        TessVertex v1 = {p0.x - nx, p0.y - ny, r, g, b, a};
        TessVertex v2 = {p1.x + nx, p1.y + ny, r, g, b, a};
        TessVertex v3 = {p1.x - nx, p1.y - ny, r, g, b, a};

        mesh.vertices.push_back(v0);
        mesh.vertices.push_back(v1);
        mesh.vertices.push_back(v2);

        mesh.vertices.push_back(v1);
        mesh.vertices.push_back(v3);
        mesh.vertices.push_back(v2);
    }
}

void StrokeTessellator::AddRoundCap(
    float cx, float cy,
    float /*dx*/, float /*dy*/,
    float half_width,
    bool /*is_start*/,
    float r, float g, float b, float a,
    TessellatedMesh& mesh
) {
    // Full circle — direction-independent, overlap with line body is invisible
    int segments = CircleSegments(half_width);

    for (int i = 0; i < segments; ++i) {
        float a0 = 2.0f * kPi * i / segments;
        float a1 = 2.0f * kPi * (i + 1) / segments;

        TessVertex v0 = {cx, cy, r, g, b, a};
        TessVertex v1 = {cx + half_width * std::cos(a0), cy + half_width * std::sin(a0), r, g, b, a};
        TessVertex v2 = {cx + half_width * std::cos(a1), cy + half_width * std::sin(a1), r, g, b, a};

        mesh.vertices.push_back(v0);
        mesh.vertices.push_back(v1);
        mesh.vertices.push_back(v2);
    }
}

void StrokeTessellator::TessellateFilledRect(
    float x1, float y1, float x2, float y2,
    float r, float g, float b, float a,
    TessellatedMesh& mesh
) {
    TessVertex v0 = {x1, y1, r, g, b, a};
    TessVertex v1 = {x2, y1, r, g, b, a};
    TessVertex v2 = {x2, y2, r, g, b, a};
    TessVertex v3 = {x1, y2, r, g, b, a};

    mesh.vertices.push_back(v0);
    mesh.vertices.push_back(v1);
    mesh.vertices.push_back(v2);

    mesh.vertices.push_back(v0);
    mesh.vertices.push_back(v2);
    mesh.vertices.push_back(v3);
}

void StrokeTessellator::TessellateFilledOval(
    float cx, float cy, float rx, float ry,
    float r, float g, float b, float a,
    TessellatedMesh& mesh
) {
    TessVertex center = {cx, cy, r, g, b, a};

    for (int i = 0; i < kOvalSegments; ++i) {
        float a0 = 2.0f * kPi * i / kOvalSegments;
        float a1 = 2.0f * kPi * (i + 1) / kOvalSegments;

        TessVertex v1 = {cx + rx * std::cos(a0), cy + ry * std::sin(a0), r, g, b, a};
        TessVertex v2 = {cx + rx * std::cos(a1), cy + ry * std::sin(a1), r, g, b, a};

        mesh.vertices.push_back(center);
        mesh.vertices.push_back(v1);
        mesh.vertices.push_back(v2);
    }
}

void StrokeTessellator::TessellateFilledTriangle(
    float x0, float y0,
    float x1, float y1,
    float x2, float y2,
    float r, float g, float b, float a,
    TessellatedMesh& mesh
) {
    mesh.vertices.push_back({x0, y0, r, g, b, a});
    mesh.vertices.push_back({x1, y1, r, g, b, a});
    mesh.vertices.push_back({x2, y2, r, g, b, a});
}

} // namespace Annotations
} // namespace qcview
