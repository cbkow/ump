// Phase 1.7: helpers to generate the two test source textures sampled
// by the compositor shader. Source A and Source B exist so we can
// visually distinguish the two inputs of SIDE_BY_SIDE / SPLIT_WIPE
// modes without needing a video decoder yet.
//
// Both images are RGBA8 and intended to be uploaded once at init via
// QRhiResourceUpdateBatch::uploadTexture — the same path that real
// video frames will travel in Phase 2+.

#pragma once

#include <QImage>
#include <QSize>

namespace qcv {

// SMPTE-ish 75% color bars + thick blue border + bold "A" badge.
QImage generateSourceA(QSize size);

// 4-quadrant pattern (orange / teal / magenta / lime) + thick yellow
// border + bold "B" badge — visually unmistakable from Source A.
QImage generateSourceB(QSize size);

} // namespace qcv
