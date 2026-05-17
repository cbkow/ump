// SvgPathParser — adapted from old QCView's SVGOverlayRenderer
// parsing logic per Guide 19 §2.6 (overlay/svg_overlay_renderer.cpp,
// the parsing portion).
//
// Reads a safety-overlay SVG and returns a list of line segments in
// SVG-coordinate space, plus the viewBox dimensions for scaling. The
// rendering portion of the old SVGOverlayRenderer becomes a per-
// platform render pass in Phase B.6.6 / C.7.6 — this parser is the
// one CPU-side input shared across both.
//
// Adaptation notes vs old app:
//   - Namespace: bare → qcv
//   - Output struct ParsedSvg replaces SafetyGuides
//   - QXmlStreamReader instead of std::regex (more robust to attribute
//     order; old regex required a specific x/y/width/height order)
//   - Path command parser preserved verbatim (M/L/H/V/h/v/Z/z) —
//     hand-rolled, not regex
//   - Handles <path>, <rect>, <polygon>, <polyline>, <line>
//
// Limitations preserved from old app:
//   - Only the SVG subset shipping safety SVGs use is supported.
//   - No transforms, no stroke-style attributes, no curves (C/Q
//     commands), no relative L/l, no S/T smooths. Adding those breaks
//     compatibility with the regex-style old parser; we lift the same
//     subset.
//   - viewBox required for accurate aspect; default 1920x1080 if
//     missing.

#pragma once

#include <QPointF>
#include <QSizeF>
#include <QString>

#include <vector>

namespace qcv {

struct SvgLineSegment {
    QPointF a;
    QPointF b;
};

struct ParsedSvg {
    QSizeF                       viewBox = QSizeF(1920.0, 1080.0);
    std::vector<SvgLineSegment>  lines;

    bool empty() const { return lines.empty(); }
    void clear() { lines.clear(); viewBox = QSizeF(1920.0, 1080.0); }
};

class SvgPathParser {
public:
    // Parse SVG content (file body, not a path). Returns a populated
    // ParsedSvg on success; on parse failure returns an empty
    // ParsedSvg (caller can detect via empty()).
    static ParsedSvg parseContent(const QString &svgContent);

    // Convenience: read the file at `path` and parse its content.
    // Returns empty ParsedSvg if the file can't be read.
    static ParsedSvg parseFile(const QString &path);
};

} // namespace qcv
