// annotation_thumbnail — see header.

#include "annotation_thumbnail.h"

#include <QBrush>
#include <QColorSpace>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPolygonF>

#include <algorithm>
#include <cmath>

namespace qcv {

QRectF strokeBoundsNorm(const ActiveStroke &s)
{
    if (s.points.empty()) return {};
    // Oval encodes {center, radii} — the true box is center ± radii.
    if (s.tool == DrawingTool::Oval && s.points.size() >= 2) {
        const QPointF c = s.points[0], r = s.points[1];
        return QRectF(c.x() - std::abs(r.x()), c.y() - std::abs(r.y()),
                      2.0 * std::abs(r.x()), 2.0 * std::abs(r.y()));
    }
    // Everything else is a list of actual coordinates (freehand /
    // line / arrow endpoints / rect corners) — bbox of the points.
    double minX = s.points.front().x(), maxX = minX;
    double minY = s.points.front().y(), maxY = minY;
    for (const QPointF &p : s.points) {
        minX = std::min(minX, p.x()); maxX = std::max(maxX, p.x());
        minY = std::min(minY, p.y()); maxY = std::max(maxY, p.y());
    }
    return QRectF(minX, minY, maxX - minX, maxY - minY);
}

namespace {

inline QPointF toPx(const QPointF &norm, double w, double h)
{
    return QPointF(norm.x() * w, norm.y() * h);
}

// Draw one stroke onto the (already un-squeezed) target. Geometry mirrors
// StrokeTessellator::tessellate so the thumbnail matches the live viewport;
// QPainter's round-cap/round-join pen gives the same uniform thickness the
// GPU tessellator produces (a single scalar perpendicular to the path).
void drawStroke(QPainter &p, const ActiveStroke &s, double w, double h)
{
    if (s.points.empty()) return;

    const QColor col = s.color;
    // strokeWidth is in source-pixel units; the GPU capture tessellated at
    // lineWidthScale == 1.0 against the source frame, so we match: pen width
    // == strokeWidth on the (effective-resolution) image.
    const double penW = std::max(1.0, static_cast<double>(s.strokeWidth));

    QPen pen(col);
    pen.setWidthF(penW);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    const QBrush fill(col);

    switch (s.tool) {
    case DrawingTool::Freehand: {
        if (s.points.size() == 1) {
            // Single tap → a dot (matches the tessellator's round cap).
            const QPointF c = toPx(s.points[0], w, h);
            p.setPen(Qt::NoPen);
            p.setBrush(fill);
            p.drawEllipse(c, penW * 0.5, penW * 0.5);
            break;
        }
        QPainterPath path;
        path.moveTo(toPx(s.points[0], w, h));
        for (std::size_t i = 1; i < s.points.size(); ++i) {
            path.lineTo(toPx(s.points[i], w, h));
        }
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);
        break;
    }

    case DrawingTool::Line: {
        if (s.points.size() < 2) break;
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawLine(toPx(s.points[0], w, h), toPx(s.points[1], w, h));
        break;
    }

    case DrawingTool::Rectangle: {
        if (s.points.size() < 4) break;
        // points[0] / points[2] are opposite corners (normalized).
        const QRectF rect =
            QRectF(toPx(s.points[0], w, h), toPx(s.points[2], w, h)).normalized();
        if (s.filled) {
            p.setPen(Qt::NoPen);
            p.setBrush(fill);
        } else {
            p.setPen(pen);
            p.setBrush(Qt::NoBrush);
        }
        p.drawRect(rect);
        break;
    }

    case DrawingTool::Oval: {
        if (s.points.size() < 2) break;
        // points[0] = center; points[1] = radii as fractions of W / H.
        const QPointF center = toPx(s.points[0], w, h);
        const double rx = s.points[1].x() * w;
        const double ry = s.points[1].y() * h;
        if (s.filled) {
            p.setPen(Qt::NoPen);
            p.setBrush(fill);
        } else {
            p.setPen(pen);
            p.setBrush(Qt::NoBrush);
        }
        p.drawEllipse(center, rx, ry);
        break;
    }

    case DrawingTool::Arrow: {
        if (s.points.size() < 2) break;
        const QPointF start = toPx(s.points[0], w, h);
        const QPointF end   = toPx(s.points[1], w, h);
        double dirX = end.x() - start.x();
        double dirY = end.y() - start.y();
        const double length = std::sqrt(dirX * dirX + dirY * dirY);
        if (length < 0.001) break;
        dirX /= length;
        dirY /= length;

        // Mirror the tessellator's arrowhead geometry (25°, size grows
        // with width); shaft stops short of the head by arrowSize*cos.
        const double arrowSize = 12.0 + penW * 2.0;
        const double cosA = std::cos(0.436332);   // 25°
        const double sinA = std::sin(0.436332);
        const double backDist = arrowSize * cosA;
        const QPointF lineEnd(end.x() - dirX * backDist,
                              end.y() - dirY * backDist);

        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawLine(start, lineEnd);

        const QPointF a1(end.x() - arrowSize * (dirX * cosA - dirY * sinA),
                         end.y() - arrowSize * (dirY * cosA + dirX * sinA));
        const QPointF a2(end.x() - arrowSize * (dirX * cosA + dirY * sinA),
                         end.y() - arrowSize * (dirY * cosA - dirX * sinA));
        QPolygonF head;
        head << end << a1 << a2;
        p.setPen(Qt::NoPen);
        p.setBrush(fill);
        p.drawPolygon(head);
        break;
    }

    case DrawingTool::None:
    case DrawingTool::Eraser:
    default:
        break;
    }
}

} // namespace

QImage renderNoteThumbnail(const QImage &cleanSquare,
                           const std::vector<ActiveStroke> &strokes,
                           int parNum, int parDen)
{
    if (cleanSquare.isNull()) return QImage();
    const int srcW = cleanSquare.width();
    const int srcH = cleanSquare.height();
    if (srcW <= 0 || srcH <= 0) return QImage();

    // Effective (un-squeezed) dims — widen the larger-pixel axis up so we
    // never downscale and lose detail. PAR 1/1 (square) is an identity copy.
    int effW = srcW;
    int effH = srcH;
    if (parNum > 0 && parDen > 0 && parNum != parDen) {
        if (parNum > parDen)
            effW = static_cast<int>(std::lround(double(srcW) * parNum / parDen));
        else
            effH = static_cast<int>(std::lround(double(srcH) * parDen / parNum));
    }

    QImage out = (effW == srcW && effH == srcH)
        ? cleanSquare.convertToFormat(QImage::Format_RGBA8888)
        : cleanSquare.scaled(effW, effH, Qt::IgnoreAspectRatio,
                             Qt::SmoothTransformation)
                     .convertToFormat(QImage::Format_RGBA8888);

    if (!strokes.empty()) {
        QPainter p(&out);
        p.setRenderHint(QPainter::Antialiasing, true);
        for (const ActiveStroke &s : strokes) {
            drawStroke(p, s, effW, effH);
        }
        p.end();
    }

    out.setColorSpace(QColorSpace(QColorSpace::SRgb));
    return out;
}

} // namespace qcv
