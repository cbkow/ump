#include "safety_overlay.h"

#include <QtLogging>

#include <cmath>

namespace qcv {

SafetyOverlay::SafetyOverlay()  = default;
SafetyOverlay::~SafetyOverlay() = default;

bool SafetyOverlay::loadFile(const QString &svgPath)
{
    ParsedSvg parsed = SvgPathParser::parseFile(svgPath);
    if (parsed.empty()) {
        qWarning("SafetyOverlay: parseFile returned empty for %s",
                 qPrintable(svgPath));
        std::lock_guard<std::mutex> lk(m_mutex);
        m_parsed.clear();
        m_loaded = false;
        return false;
    }

    std::lock_guard<std::mutex> lk(m_mutex);
    m_parsed = std::move(parsed);
    m_loaded = true;
    qInfo("SafetyOverlay: loaded %s — %zu line segments, viewBox %.0fx%.0f",
          qPrintable(svgPath),
          m_parsed.lines.size(),
          m_parsed.viewBox.width(), m_parsed.viewBox.height());
    return true;
}

void SafetyOverlay::clear()
{
    std::lock_guard<std::mutex> lk(m_mutex);
    m_parsed.clear();
    m_loaded = false;
}

bool SafetyOverlay::isLoaded() const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_loaded;
}

void SafetyOverlay::setOpacity(float opacity)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    m_opacity = std::clamp(opacity, 0.0f, 1.0f);
}

void SafetyOverlay::setColor(const QColor &color)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    m_color = color;
}

void SafetyOverlay::setLineWidth(float pixels)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    m_lineWidth = std::max(0.5f, pixels);
}

QColor SafetyOverlay::color() const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_color;
}

float SafetyOverlay::opacity() const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_opacity;
}

float SafetyOverlay::lineWidth() const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_lineWidth;
}

TessellatedMesh SafetyOverlay::mesh(QPointF displayPos, QSizeF displaySize) const
{
    TessellatedMesh out;

    std::lock_guard<std::mutex> lk(m_mutex);
    if (!m_loaded || m_parsed.lines.empty()) return out;
    if (displaySize.width() <= 0.0 || displaySize.height() <= 0.0) return out;

    // Aspect-fit the viewBox into the display rect (matches old app's
    // SVGToScreen logic).
    const double svgAspect = m_parsed.viewBox.width() / m_parsed.viewBox.height();
    const double dstAspect = displaySize.width()     / displaySize.height();

    double scale  = 1.0;
    double offX   = displayPos.x();
    double offY   = displayPos.y();
    if (svgAspect > dstAspect) {
        // SVG wider — fit width, center vertically.
        scale = displaySize.width() / m_parsed.viewBox.width();
        offY  += (displaySize.height() - m_parsed.viewBox.height() * scale) * 0.5;
    } else {
        // SVG taller — fit height, center horizontally.
        scale = displaySize.height() / m_parsed.viewBox.height();
        offX  += (displaySize.width() - m_parsed.viewBox.width() * scale) * 0.5;
    }

    auto toScreen = [&](const QPointF &svg) {
        return QPointF(offX + svg.x() * scale, offY + svg.y() * scale);
    };

    const float r = static_cast<float>(m_color.redF());
    const float g = static_cast<float>(m_color.greenF());
    const float b = static_cast<float>(m_color.blueF());
    const float a = static_cast<float>(m_color.alphaF()) * m_opacity;
    const float halfW = m_lineWidth * 0.5f;

    out.vertices.reserve(m_parsed.lines.size() * 6);

    for (const SvgLineSegment &seg : m_parsed.lines) {
        const QPointF p1 = toScreen(seg.a);
        const QPointF p2 = toScreen(seg.b);

        double dx = p2.x() - p1.x();
        double dy = p2.y() - p1.y();
        double len = std::sqrt(dx * dx + dy * dy);
        if (len < 1e-6) continue;
        dx /= len; dy /= len;

        // Perpendicular, scaled to half the line width (pixels).
        const double nx = -dy * halfW;
        const double ny =  dx * halfW;

        const TessVertex v0 = { static_cast<float>(p1.x() + nx),
                                static_cast<float>(p1.y() + ny),
                                r, g, b, a };
        const TessVertex v1 = { static_cast<float>(p1.x() - nx),
                                static_cast<float>(p1.y() - ny),
                                r, g, b, a };
        const TessVertex v2 = { static_cast<float>(p2.x() + nx),
                                static_cast<float>(p2.y() + ny),
                                r, g, b, a };
        const TessVertex v3 = { static_cast<float>(p2.x() - nx),
                                static_cast<float>(p2.y() - ny),
                                r, g, b, a };

        // Two triangles: (v0,v1,v2), (v1,v3,v2)
        out.vertices.push_back(v0);
        out.vertices.push_back(v1);
        out.vertices.push_back(v2);
        out.vertices.push_back(v1);
        out.vertices.push_back(v3);
        out.vertices.push_back(v2);
    }

    return out;
}

} // namespace qcv
