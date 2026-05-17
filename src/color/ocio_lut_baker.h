// OcioLutBaker — bake an OCIO CPU processor to a .cube 3D LUT file.
//
// Stand-alone helper used by OCIOConfigManager::exportLut. Caller
// owns building the chain (see OcioChainBuilder::buildGroupTransform)
// and obtaining the optimized CPU processor; this routine just
// samples on a cubeSize³ grid and writes the standard Iridas .cube
// format that Resolve / Nuke / Premiere / AE / Baselight all consume.
//
// Synchronous. 65³ takes <100 ms on M-series silicon, so a worker
// thread + progress popup is unnecessary at that grid size. If we
// expose 129³ later, lift this onto QtConcurrent::run.

#pragma once

#include <QString>

#include <OpenColorIO/OpenColorIO.h>

namespace qcv::OcioLutBaker {

// Sample `proc` on a cubeSize³ grid and write a .cube file to outPath.
// Returns empty string on success; a human-readable error message on
// failure. On failure, any partial file is removed.
//
// `cubeSize` must be ≥ 2. Caller validates.
QString writeCube(OCIO_NAMESPACE::ConstCPUProcessorRcPtr proc,
                  const QString &outPath,
                  int cubeSize);

} // namespace qcv::OcioLutBaker
