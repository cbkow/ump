// FFmpegMetadataExtractor — Phase 3.B / Guide 07 §1.
//
// Adapted from old QCView's
// src/metadata/ffmpeg_metadata_extractor.{h,cpp} (~386 LoC). Reads
// container + stream info via libavformat; no frame decoding. Fast
// (~10–50ms per file).
//
// Pure C++, no Qt-signal plumbing — that lives in MetadataService
// which calls this off-thread. Returns a populated VideoMetadata
// (or `loaded=false` on failure).

#pragma once

#include "media_item.h"

#include <QString>

namespace qcv {

class FFmpegMetadataExtractor {
public:
    static VideoMetadata extract(const QString &filePath);

    // Fast container-only duration probe (~5–20ms, no codec init).
    // Returns 0.0 on failure.
    static double probeDuration(const QString &filePath);

    // Header-only display-matrix rotation probe for project caches
    // that predate VideoMetadata::rotationDeg. Returns 0/90/180/270
    // (clockwise display degrees), or -1 when the file can't be
    // opened (offline volume) so the caller keeps the unknown
    // sentinel and retries next launch.
    static int probeRotation(const QString &filePath);

    // Header-only codec-profile probe for project caches that predate
    // VideoMetadata::codecProfile. Returns the display name ("ProRes
    // 422 HQ"), empty (non-null) when the stream carries no profile,
    // or a null QString when the file can't be opened (offline
    // volume) so the caller skips the patch and retries next launch.
    static QString probeCodecProfile(const QString &filePath);
};

} // namespace qcv
