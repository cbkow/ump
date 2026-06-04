// SafetyOverlay — Phase 7.5 B.6.6 / Guide 19 §2.6.
//
// Reads a safety-overlay SVG via SvgPathParser, then on each frame
// generates a TessellatedMesh of thick lines fitted into the viewport
// rect. The native player's stroke render pass (MetalAnnotationRenderer
// for now — the pipeline is generic per-vertex-color triangles) draws
// the mesh on top of the annotation strokes.
//
// CPU-only; reusable across Metal + Vulkan.
//
// Per Guide 19 §2.6, the SVG-parsing side is already lifted
// (src/render/svg_path_parser); SafetyOverlay is the geometry +
// styling layer on top. Old QCView's SVGOverlayRenderer mixed parsing
// with ImGui rendering; the new port separates them.

#pragma once

#include "annotations/stroke_tessellator.h"
#include "svg_path_parser.h"

#include <QColor>
#include <QPointF>
#include <QSizeF>
#include <QString>

#include <atomic>
#include <mutex>

namespace qcv {

class SafetyOverlay {
public:
    SafetyOverlay();
    ~SafetyOverlay();

    // Load a safety SVG. Returns false if the file can't be parsed.
    // Call from the GUI thread; render thread reads via mesh().
    bool loadFile(const QString &svgPath);

    // Drop any loaded SVG.
    void clear();

    bool isLoaded() const;

    // Display settings (GUI thread).
    void setOpacity(float opacity);
    void setColor(const QColor &color);
    void setLineWidth(float pixels);

    QColor color() const;
    float  opacity() const;
    float  lineWidth() const;

    // Render-thread call. Generates the line-list mesh aspect-fit
    // into the viewport rect. Returns an empty mesh if not loaded.
    TessellatedMesh mesh(QPointF displayPos, QSizeF displaySize) const;

    // Render-thread call for the on-image overlay. Aspect-fits the SVG
    // into the SOURCE pixel frame (srcW × srcH, square pixels), then
    // maps that through the displayed image rect (imgPos/imgSize, which
    // is pixel-aspect-corrected) — so the guide stretches with the
    // picture under anamorphic un-squeeze exactly as the image does.
    // Reduces to mesh()'s uniform fit when imgSize matches the source
    // aspect (i.e. pixel aspect 1:1). Returns empty if not loaded or
    // dims are non-positive.
    TessellatedMesh meshForImage(QPointF imgPos, QSizeF imgSize,
                                 double srcW, double srcH) const;

private:
    // SVG-parse output is small (~hundreds of segments per file); a
    // mutex on read is fine.
    mutable std::mutex m_mutex;
    ParsedSvg          m_parsed;
    bool               m_loaded = false;
    QColor             m_color  = QColor(255, 255, 255, 255);
    float              m_opacity = 0.7f;
    float              m_lineWidth = 2.0f;
};

} // namespace qcv
