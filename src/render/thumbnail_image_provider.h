// ThumbnailImageProvider — QML-facing media thumbnails
// (image://thumb/...). Serves the Inspector's File-card hero image
// and the per-EXR-layer picker tiles by running the decode layer's
// IImageLoader::loadThumbnail fast paths off the GUI thread.
//
// URL grammar (the id is everything after "image://thumb/"):
//   <absolute file path>[?layer=<exr layer>][&frame=<N|mid>]
// - `layer` routes multi-layer EXRs (same convention as the
//   TimelineThumbnailCache's "?layer=" cache-key suffix).
// - `frame` marks the source as a *video container*: the path is
//   opened with VideoImageLoader and that frame is extracted
//   ("mid" = frameCount/2, the poster frame — first frames are
//   routinely black leaders). Absent = still image, routed by
//   extension (EXR/PNG/TIFF/JPEG loaders; anything else falls back
//   to QImageReader so plain formats keep working).
//
// Concurrency (modeled on sister-app ufb's UfbImageProviders):
// this is a QQuickAsyncImageProvider running requests on its OWN
// 4-thread pool. The first cut was a sync provider with
// ForceAsynchronousImageLoading — which serializes every request
// onto Qt Quick's single pixmap-reader thread, so a 20-layer EXR
// contact sheet decoded one tile at a time. Responses support
// cancel() so tiles torn down mid-decode don't burn queued work.
//
// Requests decode a fixed-size MASTER (480 long edge — the largest
// size the inspector asks for) that lands in a byte-bounded LRU;
// each response then downscales the master to its requestedSize.
// Hero (480) and layer tiles (240) of the same source share one
// decode. Single-part multi-layer EXRs go one better: all channels
// live interleaved in the same compression blocks, so the first
// request runs EXRImageLoader::loadThumbnailsAllLayers — ONE
// scanline sweep that fills every layer's master — while
// concurrent requests for sibling layers wait on the in-flight
// batch instead of re-decoding (N decompress passes → 1).

#pragma once

#include <QCache>
#include <QImage>
#include <QMutex>
#include <QQuickAsyncImageProvider>
#include <QSet>
#include <QThreadPool>
#include <QWaitCondition>

namespace qcv {

class ThumbnailImageProvider : public QQuickAsyncImageProvider {
public:
    ThumbnailImageProvider();
    ~ThumbnailImageProvider() override;

    QQuickImageResponse *requestImageResponse(
        const QString &id, const QSize &requestedSize) override;

    // Worker-thread entry (called by the response runnable): returns
    // the master image for the parsed request, consulting/filling
    // the LRU and joining any in-flight EXR batch.
    QImage masterFor(const QString &path, const QString &layer,
                     const QString &frame);

private:
    // Decode one master (no cache, no batch). Empty QImage on failure.
    static QImage decodeMaster(const QString &path, const QString &layer,
                               const QString &frame);

    QImage cacheGet(const QString &key);
    void   cachePut(const QString &key, const QImage &img);

    QMutex m_cacheMutex;
    QCache<QString, QImage> m_cache;

    // In-flight EXR batch dedup — one batch per path at a time;
    // late requesters block on the condition then re-read the LRU.
    QMutex m_batchMutex;
    QWaitCondition m_batchDone;
    QSet<QString> m_batchInFlight;

    QThreadPool m_pool;
};

} // namespace qcv
