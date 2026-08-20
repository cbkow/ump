// MetadataService — see header.

#include "metadata_service.h"

#include "adobe_metadata_extractor.h"
#include "ffmpeg_metadata_extractor.h"

#include <QMetaObject>
#include <QPointer>
#include <QThreadPool>

namespace qcv {

// VideoMetadata isn't a Qt type; register it so it can travel
// through queued connections + Q_INVOKABLE bridges. Done once on
// first MetadataService construction.
static void registerMetaTypes()
{
    static bool registered = false;
    if (registered) return;
    qRegisterMetaType<qcv::VideoMetadata>("qcv::VideoMetadata");
    qRegisterMetaType<qcv::AdobeMetadata>("qcv::AdobeMetadata");
    registered = true;
}

MetadataService::MetadataService(QObject *parent)
    : QObject(parent)
{
    registerMetaTypes();
}

MetadataService::~MetadataService() = default;

void MetadataService::requestVideoExtraction(const QString &mediaId,
                                             const QString &path)
{
    if (mediaId.isEmpty() || path.isEmpty()) return;

    // Capture by value so the runnable owns its inputs; QPointer
    // protects against the service being destroyed mid-extract.
    QPointer<MetadataService> guard(this);
    QThreadPool::globalInstance()->start([guard, mediaId, path]() {
        const VideoMetadata m = FFmpegMetadataExtractor::extract(path);
        if (!guard) return;   // service destroyed during extract; drop
        QMetaObject::invokeMethod(guard.data(), [guard, mediaId, m]() {
            if (!guard) return;
            Q_EMIT guard->metadataReady(mediaId, m);
        }, Qt::QueuedConnection);
    });
}

void MetadataService::requestRotationProbe(const QString &mediaId,
                                           const QString &path)
{
    if (mediaId.isEmpty() || path.isEmpty()) return;

    QPointer<MetadataService> guard(this);
    QThreadPool::globalInstance()->start([guard, mediaId, path]() {
        const int deg = FFmpegMetadataExtractor::probeRotation(path);
        if (!guard) return;
        QMetaObject::invokeMethod(guard.data(), [guard, mediaId, deg]() {
            if (!guard) return;
            Q_EMIT guard->rotationProbed(mediaId, deg);
        }, Qt::QueuedConnection);
    });
}

void MetadataService::requestContainerProbe(const QString &mediaId,
                                            const QString &path)
{
    if (mediaId.isEmpty() || path.isEmpty()) return;

    QPointer<MetadataService> guard(this);
    QThreadPool::globalInstance()->start([guard, mediaId, path]() {
        const VideoMetadata probe =
            FFmpegMetadataExtractor::probeContainer(path);
        if (!guard) return;
        QMetaObject::invokeMethod(guard.data(), [guard, mediaId, probe]() {
            if (!guard) return;
            Q_EMIT guard->containerProbed(mediaId, probe);
        }, Qt::QueuedConnection);
    });
}

void MetadataService::requestAdobeExtraction(const QString &mediaId,
                                             const QString &path)
{
    if (mediaId.isEmpty() || path.isEmpty()) return;

    QPointer<MetadataService> guard(this);
    QThreadPool::globalInstance()->start([guard, mediaId, path]() {
        const AdobeMetadata m = AdobeMetadataExtractor::extract(path);
        if (!guard) return;
        QMetaObject::invokeMethod(guard.data(), [guard, mediaId, m]() {
            if (!guard) return;
            Q_EMIT guard->adobeMetadataReady(mediaId, m);
        }, Qt::QueuedConnection);
    });
}

} // namespace qcv
