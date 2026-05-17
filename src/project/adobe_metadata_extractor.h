// AdobeMetadataExtractor — Phase 3.D / Guide 07 §6.
//
// Reads Adobe project links + XMP / QuickTime / MXF / UserData
// timecodes by shelling out to the bundled exiftool. Returns an
// AdobeMetadata populated with whatever fields were present.
//
// exiftool is bundled into qcview.app/Contents/Resources/assets/
// exiftool/ (Phase 3.D — see src/app/CMakeLists.txt). Resolution
// order:
//   1. Bundled (next to the executable) — primary.
//   2. PATH (Homebrew install) — fallback for dev builds.
//
// Adapted from old QCView's
// src/metadata/adobe_metadata.{h,cpp} (~600 LoC). Old uses raw
// CreateProcess / fork+exec; we use QProcess for portability.

#pragma once

#include "media_item.h"

#include <QString>

namespace qcv {

class AdobeMetadataExtractor {
public:
    // Synchronous extraction. Spawns exiftool, parses `-s`-style
    // key:value output. Typical run is ~200–500ms (exiftool
    // startup-bound on macOS — Perl warm-up).
    static AdobeMetadata extract(const QString &filePath);

    // Resolved exiftool binary path; empty if not found anywhere.
    static QString resolveExiftoolPath();
};

} // namespace qcv
