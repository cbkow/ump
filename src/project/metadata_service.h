// MetadataService — Phase 3.B / Guide 07 §1 D1.
//
// QObject orchestrator: receives extract requests from ProjectManager,
// dispatches to FFmpegMetadataExtractor on a background QThreadPool
// runnable, and emits `metadataReady(mediaId, metadata)` back on the
// owning thread once each extraction finishes.
//
// Phase 3.B scope: video / audio container metadata via FFmpeg only.
// Phase 3.C adds EXR / image-sequence detectors; Phase 3.D adds
// Adobe via exiftool. Each extractor type plugs into this same
// service so the consumer (ProjectManager) sees a single signal
// surface regardless of file type.
//
// Threading: extraction runs on QThreadPool::globalInstance(); the
// completion signal is queued back to the service's owning thread
// via Qt::QueuedConnection so callers don't need to lock.

#pragma once

#include "media_item.h"

#include <QObject>
#include <QString>

namespace qcv {

class MetadataService : public QObject {
    Q_OBJECT

public:
    explicit MetadataService(QObject *parent = nullptr);
    ~MetadataService() override;

    // Schedule an async FFmpeg-based extraction for `mediaId` /
    // `path`. Returns immediately; result lands on metadataReady().
    // Re-issuing for the same mediaId is safe; the latest result
    // wins (per-id generation counter discards stale callbacks on
    // path change).
    void requestVideoExtraction(const QString &mediaId, const QString &path);

    // Phase 3.D — Adobe links + extended timecodes via the bundled
    // exiftool. Slower than FFmpeg (Perl startup-bound) so we run
    // it in parallel and emit a separate signal.
    void requestAdobeExtraction(const QString &mediaId, const QString &path);

    // Header-only display-matrix probe for project caches that
    // predate VideoMetadata::rotationDeg. Patches a single field
    // rather than re-running the full extraction so an offline
    // volume can't clobber good cached metadata. Result lands on
    // rotationProbed(); deg is -1 when the file couldn't be opened.
    void requestRotationProbe(const QString &mediaId, const QString &path);

Q_SIGNALS:
    // Fired on the owning thread when an extraction completes.
    // `metadata.loaded` is true on success; false means the file
    // couldn't be opened / probed.
    void metadataReady(const QString &mediaId, const VideoMetadata &metadata);
    void adobeMetadataReady(const QString &mediaId, const AdobeMetadata &metadata);
    void rotationProbed(const QString &mediaId, int rotationDeg);

private:
    Q_DISABLE_COPY(MetadataService)
};

} // namespace qcv
