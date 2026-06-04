// annotation_thumbnail — CPU recomposite of a note's display thumbnail.
//
// Notes store a durable, square (raw storage-resolution) "clean" PNG of the
// frame plus their strokes normalized [0,1] in notes.json. Both are pixel-
// aspect-independent. The displayed / exported thumbnail is DERIVED from
// those: un-squeeze the clean frame to the clip's effective (pixel-aspect-
// corrected) dimensions, then draw the strokes on top at uniform thickness.
//
// Deriving (rather than baking a fixed annotated capture) means a later
// change to the clip's pixel aspect just re-derives — nothing goes stale —
// and stroke line-weight stays uniform instead of distorting with the
// horizontal un-squeeze (the trap that stretching a baked PNG would hit).
//
// Pure CPU (QPainter); reused by the live regen path and the notes exporter.

#pragma once

#include "active_stroke.h"

#include <QImage>

#include <vector>

namespace qcv {

// Recomposite a note's display thumbnail.
//   cleanSquare : the durable square clean frame (raw storage pixels).
//   strokes     : the note's strokes (normalized [0,1] coords).
//   parNum/parDen : the clip's pixel aspect (1/1 = square → identity).
// Returns a null QImage if cleanSquare is null/empty. The result is in the
// effective (un-squeezed) display aspect with strokes drawn at uniform
// thickness, matching the live viewport.
QImage renderNoteThumbnail(const QImage &cleanSquare,
                           const std::vector<ActiveStroke> &strokes,
                           int parNum, int parDen);

} // namespace qcv
