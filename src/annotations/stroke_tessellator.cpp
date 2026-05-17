// StrokeTessellator — direct lift from old QCView's
// src/annotations/stroke_tessellator.cpp per Guide 19 §2.3.

#include "stroke_tessellator.h"

#include <cmath>

namespace qcv {

namespace {

constexpr int   kMinCircleSegments = 16;   // Minimum stamps per cap
constexpr int   kOvalSegments      = 64;   // Segments for oval approximation
constexpr float kPi                = 3.14159265358979323846f;

// Adaptive segment count for full circle: ~1.5 px arc length per segment.
int circleSegments(float radius)
{
    int segs = static_cast<int>(std::ceil(2.0f * kPi * radius / 1.5f));
    return std::max(segs, kMinCircleSegments);
}

} // namespace

void StrokeTessellator::normalizedToScreen(
    float nx, float ny,
    float dpx, float dpy, float dw, float dh,
    float &ox, float &oy)
{
    ox = dpx + (nx * dw);
    oy = dpy + (ny * dh);
}

TessellatedMesh StrokeTessellator::tessellate(
    const ActiveStroke &stroke,
    float displayPosX, float displayPosY,
    float displayWidth, float displayHeight,
    float lineWidthScale)
{
    TessellatedMesh mesh;

    if (stroke.points.empty()) return mesh;

    const float r = static_cast<float>(stroke.color.redF());
    const float g = static_cast<float>(stroke.color.greenF());
    const float b = static_cast<float>(stroke.color.blueF());
    const float a = static_cast<float>(stroke.color.alphaF());
    const float scaledWidth = stroke.strokeWidth * lineWidthScale;
    const float halfWidth   = scaledWidth * 0.5f;

    auto toScreen = [&](const QPointF &p, float &ox, float &oy) {
        normalizedToScreen(static_cast<float>(p.x()),
                           static_cast<float>(p.y()),
                           displayPosX, displayPosY,
                           displayWidth, displayHeight,
                           ox, oy);
    };

    switch (stroke.tool) {
        case DrawingTool::Freehand: {
            // v2.0 modeled strokes are already smoothed; v1.0 legacy
            // strokes render as raw (the deprecated StrokeSmoother
            // Catmull-Rom path is intentionally not lifted — see
            // header).
            if (stroke.points.empty()) return mesh;

            // Single point (click without drag) → emit one round
            // cap so the dot is visible. Without this the user
            // taps and sees nothing, since tessellatePolyline bails
            // when there are fewer than 2 vertices.
            if (stroke.points.size() == 1) {
                float sx, sy;
                toScreen(stroke.points[0], sx, sy);
                addRoundCap(sx, sy, halfWidth, r, g, b, a, mesh);
                break;
            }

            std::vector<QPointF> screen(stroke.points.size());
            for (std::size_t i = 0; i < stroke.points.size(); ++i) {
                float sx, sy;
                toScreen(stroke.points[i], sx, sy);
                screen[i] = QPointF(sx, sy);
            }
            tessellatePolyline(screen, halfWidth, r, g, b, a, mesh);
            break;
        }

        case DrawingTool::Rectangle: {
            if (stroke.points.size() < 4) return mesh;

            float x1, y1, x2, y2;
            toScreen(stroke.points[0], x1, y1);
            toScreen(stroke.points[2], x2, y2);

            if (stroke.filled) {
                tessellateFilledRect(x1, y1, x2, y2, r, g, b, a, mesh);
            } else {
                std::vector<QPointF> rect = {
                    {x1, y1}, {x2, y1}, {x2, y2}, {x1, y2}, {x1, y1}
                };
                tessellatePolyline(rect, halfWidth, r, g, b, a, mesh);
            }
            break;
        }

        case DrawingTool::Oval: {
            if (stroke.points.size() < 2) return mesh;

            float cx, cy;
            toScreen(stroke.points[0], cx, cy);
            const float rx = static_cast<float>(stroke.points[1].x()) * displayWidth;
            const float ry = static_cast<float>(stroke.points[1].y()) * displayHeight;

            if (stroke.filled) {
                tessellateFilledOval(cx, cy, rx, ry, r, g, b, a, mesh);
            } else {
                std::vector<QPointF> oval(kOvalSegments + 1);
                for (int i = 0; i <= kOvalSegments; ++i) {
                    const float angle = 2.0f * kPi * i / kOvalSegments;
                    oval[i] = QPointF(cx + rx * std::cos(angle),
                                      cy + ry * std::sin(angle));
                }
                tessellatePolyline(oval, halfWidth, r, g, b, a, mesh);
            }
            break;
        }

        case DrawingTool::Arrow: {
            if (stroke.points.size() < 2) return mesh;

            float startX, startY, endX, endY;
            toScreen(stroke.points[0], startX, startY);
            toScreen(stroke.points[1], endX,   endY);

            float dirX = endX - startX;
            float dirY = endY - startY;
            const float length = std::sqrt(dirX * dirX + dirY * dirY);
            if (length < 0.001f) return mesh;

            dirX /= length;
            dirY /= length;

            const float arrowSize     = 12.0f + scaledWidth * 2.0f;
            const float arrowAngleRad = 0.436332f;   // 25 degrees
            const float cosA = std::cos(arrowAngleRad);
            const float sinA = std::sin(arrowAngleRad);

            const float arrowBackDist = arrowSize * cosA;
            const float lineEndX = endX - dirX * arrowBackDist;
            const float lineEndY = endY - dirY * arrowBackDist;

            std::vector<QPointF> shaft = {
                QPointF(startX, startY),
                QPointF(lineEndX, lineEndY)
            };
            tessellatePolyline(shaft, halfWidth, r, g, b, a, mesh);

            const float a1x = endX - arrowSize * (dirX * cosA - dirY * sinA);
            const float a1y = endY - arrowSize * (dirY * cosA + dirX * sinA);
            const float a2x = endX - arrowSize * (dirX * cosA + dirY * sinA);
            const float a2y = endY - arrowSize * (dirY * cosA - dirX * sinA);
            tessellateFilledTriangle(endX, endY, a1x, a1y, a2x, a2y,
                                     r, g, b, a, mesh);
            break;
        }

        case DrawingTool::Line: {
            if (stroke.points.size() < 2) return mesh;

            float startX, startY, endX, endY;
            toScreen(stroke.points[0], startX, startY);
            toScreen(stroke.points[1], endX,   endY);

            std::vector<QPointF> line = {
                QPointF(startX, startY),
                QPointF(endX,   endY)
            };
            tessellatePolyline(line, halfWidth, r, g, b, a, mesh);
            break;
        }

        case DrawingTool::None:
        default:
            break;
    }

    return mesh;
}

void StrokeTessellator::tessellatePolyline(
    const std::vector<QPointF> &screenPoints,
    float halfWidth,
    float r, float g, float b, float a,
    TessellatedMesh &mesh)
{
    if (screenPoints.size() < 2) return;

    const std::size_t n = screenPoints.size();

    struct SegInfo {
        float dx, dy;   // direction
        float nx, ny;   // left-hand perpendicular
        float len;
    };
    std::vector<SegInfo> segs(n - 1);

    for (std::size_t i = 0; i < n - 1; ++i) {
        float dx = static_cast<float>(screenPoints[i + 1].x() - screenPoints[i].x());
        float dy = static_cast<float>(screenPoints[i + 1].y() - screenPoints[i].y());
        float len = std::sqrt(dx * dx + dy * dy);
        if (len < 1e-6f) len = 1e-6f;
        segs[i].dx  = dx / len;
        segs[i].dy  = dy / len;
        segs[i].nx  = -segs[i].dy;
        segs[i].ny  =  segs[i].dx;
        segs[i].len = len;
    }

    // Round-cap stamp at every vertex (handles caps + joins uniformly).
    for (std::size_t i = 0; i < n; ++i) {
        addRoundCap(static_cast<float>(screenPoints[i].x()),
                    static_cast<float>(screenPoints[i].y()),
                    halfWidth, r, g, b, a, mesh);
    }

    // Connecting quads between adjacent stamps.
    for (std::size_t i = 0; i < n - 1; ++i) {
        const float nx = segs[i].nx * halfWidth;
        const float ny = segs[i].ny * halfWidth;

        const QPointF &p0 = screenPoints[i];
        const QPointF &p1 = screenPoints[i + 1];

        const TessVertex v0 = {static_cast<float>(p0.x()) + nx, static_cast<float>(p0.y()) + ny, r, g, b, a};
        const TessVertex v1 = {static_cast<float>(p0.x()) - nx, static_cast<float>(p0.y()) - ny, r, g, b, a};
        const TessVertex v2 = {static_cast<float>(p1.x()) + nx, static_cast<float>(p1.y()) + ny, r, g, b, a};
        const TessVertex v3 = {static_cast<float>(p1.x()) - nx, static_cast<float>(p1.y()) - ny, r, g, b, a};

        mesh.vertices.push_back(v0);
        mesh.vertices.push_back(v1);
        mesh.vertices.push_back(v2);
        mesh.vertices.push_back(v1);
        mesh.vertices.push_back(v3);
        mesh.vertices.push_back(v2);
    }
}

void StrokeTessellator::addRoundCap(
    float cx, float cy,
    float halfWidth,
    float r, float g, float b, float a,
    TessellatedMesh &mesh)
{
    const int segments = circleSegments(halfWidth);

    for (int i = 0; i < segments; ++i) {
        const float a0 = 2.0f * kPi * i       / segments;
        const float a1 = 2.0f * kPi * (i + 1) / segments;

        const TessVertex v0 = {cx, cy, r, g, b, a};
        const TessVertex v1 = {cx + halfWidth * std::cos(a0),
                               cy + halfWidth * std::sin(a0),
                               r, g, b, a};
        const TessVertex v2 = {cx + halfWidth * std::cos(a1),
                               cy + halfWidth * std::sin(a1),
                               r, g, b, a};

        mesh.vertices.push_back(v0);
        mesh.vertices.push_back(v1);
        mesh.vertices.push_back(v2);
    }
}

void StrokeTessellator::tessellateFilledRect(
    float x1, float y1, float x2, float y2,
    float r, float g, float b, float a,
    TessellatedMesh &mesh)
{
    const TessVertex v0 = {x1, y1, r, g, b, a};
    const TessVertex v1 = {x2, y1, r, g, b, a};
    const TessVertex v2 = {x2, y2, r, g, b, a};
    const TessVertex v3 = {x1, y2, r, g, b, a};

    mesh.vertices.push_back(v0);
    mesh.vertices.push_back(v1);
    mesh.vertices.push_back(v2);
    mesh.vertices.push_back(v0);
    mesh.vertices.push_back(v2);
    mesh.vertices.push_back(v3);
}

void StrokeTessellator::tessellateFilledOval(
    float cx, float cy, float rx, float ry,
    float r, float g, float b, float a,
    TessellatedMesh &mesh)
{
    const TessVertex center = {cx, cy, r, g, b, a};

    for (int i = 0; i < kOvalSegments; ++i) {
        const float a0 = 2.0f * kPi * i       / kOvalSegments;
        const float a1 = 2.0f * kPi * (i + 1) / kOvalSegments;

        const TessVertex v1 = {cx + rx * std::cos(a0),
                               cy + ry * std::sin(a0),
                               r, g, b, a};
        const TessVertex v2 = {cx + rx * std::cos(a1),
                               cy + ry * std::sin(a1),
                               r, g, b, a};

        mesh.vertices.push_back(center);
        mesh.vertices.push_back(v1);
        mesh.vertices.push_back(v2);
    }
}

void StrokeTessellator::tessellateFilledTriangle(
    float x0, float y0,
    float x1, float y1,
    float x2, float y2,
    float r, float g, float b, float a,
    TessellatedMesh &mesh)
{
    mesh.vertices.push_back({x0, y0, r, g, b, a});
    mesh.vertices.push_back({x1, y1, r, g, b, a});
    mesh.vertices.push_back({x2, y2, r, g, b, a});
}

} // namespace qcv
