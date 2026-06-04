#include "project_manager.h"

#include "decode/exr_image_loader.h"
#include "metadata_service.h"
#include "project_io.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QUuid>
#include <QVariantMap>
#include <QtLogging>

#include <climits>
#include <cmath>
#include <cstdio>

namespace qcv {

namespace {

// File extensions that count as still images. Drives the
// image-sequence detection branch in addMediaFile.
bool isImageExtension(const QString &suffixLower)
{
    static const QStringList kImageExts = {
        QStringLiteral("png"),  QStringLiteral("jpg"),
        QStringLiteral("jpeg"), QStringLiteral("tif"),
        QStringLiteral("tiff"), QStringLiteral("exr"),
        QStringLiteral("dpx"),
    };
    return kImageExts.contains(suffixLower);
}

} // namespace

ProjectManager::ProjectManager(QObject *parent)
    : QObject(parent)
{
    // Guide 07 D4 + Guide 16 — bin taxonomy is the FOUR fixed
    // categories (Dual Views was dropped per Guide 16). Order is
    // the canonical display order: Videos / Audio / Images /
    // Playlists. The user can never rename / delete / reorder
    // these; items route automatically by MediaType on add.
    auto seed = [this](const QString &name, MediaType accepts, bool open) {
        ProjectBin b;
        b.name    = name;
        b.accepts = accepts;
        b.isOpen  = open;
        m_bins.append(b);
    };
    seed(QStringLiteral("Videos"),     MediaType::Video,    true);
    seed(QStringLiteral("Audio"),      MediaType::Audio,    false);
    seed(QStringLiteral("Images"),     MediaType::Image,    false);
    seed(QStringLiteral("Playlists"),  MediaType::Playlist, false);
    // Phase 7.8 — Dual Views bin. Holds saved A/B comparison
    // sessions (MediaType::DualPair). Drag-drop from disk doesn't
    // route here; only WindowManager::saveCurrentDualView populates
    // this bin via addDualPairItem().
    seed(QStringLiteral("Dual Views"), MediaType::DualPair, false);

    // Phase 3.B — async metadata extraction. Connects directly into
    // onVideoMetadataReady, which patches the MediaItem and emits
    // binsChanged so the inspector reflows.
    m_metadataService = new MetadataService(this);
    connect(m_metadataService, &MetadataService::metadataReady,
            this, &ProjectManager::onVideoMetadataReady);
    connect(m_metadataService, &MetadataService::adobeMetadataReady,
            this, &ProjectManager::onAdobeMetadataReady);

    // Phase 7.6 — generate identity + timestamps so the manager is
    // never in an undefined "pre-newProject" state. Done after the
    // bin shells are seeded so newProject's clear-loop iterates
    // over the canonical five entries.
    newProject();
}

// ---- Phase 7.6 — `.qcvproj` v2 persistence ---------------------

void ProjectManager::newProject()
{
    // Pool + selection wipe. Bin shells stay (their accept-types
    // are part of the canonical layout); only item_ids reset.
    m_mediaPool.clear();
    for (ProjectBin &bin : m_bins) {
        bin.itemIds.clear();
    }
    const bool hadActive  = !m_activeItemId.isEmpty();
    const bool hadBSource = !m_bSourceMediaId.isEmpty();
    m_activeItemId.clear();
    m_bSourceMediaId.clear();

    m_projectUuid       = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_projectFilePath.clear();
    m_projectName       = QStringLiteral("Untitled");
    m_projectCreatedAt  = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    m_projectModifiedAt = m_projectCreatedAt;

    const bool wasDirty = m_projectDirty;
    m_projectDirty      = false;

    // Drop dual view before the wiped state propagates — see the
    // projectReplaced doc comment in the header.
    emit projectReplaced();
    emit binsChanged();
    if (hadActive)  emit activeItemIdChanged();
    if (hadBSource) emit bSourceChanged();
    emit projectFileChanged();
    if (wasDirty) emit projectDirtyChanged();
}

void ProjectManager::markDirty()
{
    if (m_projectDirty) return;
    m_projectDirty = true;
    emit projectDirtyChanged();
}

bool ProjectManager::saveProject(const QString &path)
{
    if (path.isEmpty()) {
        emit projectError(QStringLiteral(
            "Save failed: no path provided."));
        return false;
    }
    QString err;
    if (!ProjectIo::save(*this, path, &err)) {
        emit projectError(err.isEmpty()
            ? QStringLiteral("Save failed.")
            : err);
        return false;
    }
    m_projectFilePath   = path;
    m_projectModifiedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    const bool wasDirty = m_projectDirty;
    m_projectDirty      = false;
    emit projectFileChanged();
    if (wasDirty) emit projectDirtyChanged();
    emit projectSaved(path);
    return true;
}

bool ProjectManager::openProject(const QString &path)
{
    if (path.isEmpty()) {
        emit projectError(QStringLiteral(
            "Open failed: no path provided."));
        return false;
    }
    QString err;
    if (!ProjectIo::load(*this, path, &err)) {
        emit projectError(err.isEmpty()
            ? QStringLiteral("Open failed.")
            : err);
        return false;
    }
    // applyLoadedState() handles signal emission; saveProject /
    // openProject only adjust the file-path-derived metadata.
    m_projectFilePath = path;
    emit projectFileChanged();
    emit projectOpened(path);
    return true;
}

void ProjectManager::applyLoadedState(QList<MediaItem>  pool,
                                      QList<ProjectBin> bins,
                                      QString           activeId,
                                      QString           bSourceId,
                                      QString           uuid,
                                      QString           name,
                                      QString           createdAt,
                                      QString           modifiedAt)
{
    m_mediaPool      = std::move(pool);
    m_bins           = std::move(bins);
    m_activeItemId   = std::move(activeId);
    m_bSourceMediaId = std::move(bSourceId);

    m_projectUuid       = uuid.isEmpty()
        ? QUuid::createUuid().toString(QUuid::WithoutBraces)
        : std::move(uuid);
    m_projectName       = name.isEmpty()
        ? QStringLiteral("Untitled") : std::move(name);
    m_projectCreatedAt  = createdAt.isEmpty()
        ? QDateTime::currentDateTimeUtc().toString(Qt::ISODate)
        : std::move(createdAt);
    m_projectModifiedAt = modifiedAt.isEmpty()
        ? m_projectCreatedAt : std::move(modifiedAt);

    const bool wasDirty = m_projectDirty;
    m_projectDirty = false;

    // Saved active id may reference a pool item that's no longer
    // present (e.g., the entry was deleted before re-save, or the
    // .qcvproj came from a different machine where the path resolved
    // differently). Clear it now so the UI doesn't show a phantom
    // selection and so the loadRequested emit below doesn't fire on
    // a missing item.
    int activeIdx = -1;
    if (!m_activeItemId.isEmpty()) {
        activeIdx = findIndexInPool(m_activeItemId);
        if (activeIdx < 0) {
            qWarning("ProjectManager::applyLoadedState — saved active id "
                     "'%s' missing from pool; clearing",
                     qPrintable(m_activeItemId));
            m_activeItemId.clear();
        }
    }

    emit binsChanged();
    emit activeItemIdChanged();
    emit bSourceChanged();
    emit projectFileChanged();
    if (wasDirty) emit projectDirtyChanged();

    // Trigger the load chain for the restored active item.
    // applyLoadedState's other consumers (binsChanged etc.) only
    // refresh the UI bin views; loadRequested is what WindowManager
    // listens to in order to open the decoder / start the image-seq /
    // start the playlist. Without this, the row reads as selected but
    // the viewport stays empty until the user clicks a different row
    // and clicks back — setActiveItem emits loadRequested on the
    // toggle. Emit here so cold-open lands in the viewport directly.
    if (activeIdx >= 0) {
        emit loadRequested(m_mediaPool[activeIdx]);
    }
}

ProjectManager::~ProjectManager() = default;

QString ProjectManager::detectType(const QString &path, MediaType *outType)
{
    const QString suffix = QFileInfo(path).suffix().toLower();

    // Phase 7.1 handles single-file video only. Other types are
    // recognized so the enum picks them correctly (for the type
    // chip in the inspector) but loading still routes through the
    // video decoder — sequences and playlists wire in 7.4 / 7.5.
    static const QStringList kVideoExts = {
        QStringLiteral("mov"), QStringLiteral("mp4"), QStringLiteral("m4v"),
        QStringLiteral("mxf"), QStringLiteral("mkv"), QStringLiteral("avi"),
        QStringLiteral("webm"), QStringLiteral("ts"),  QStringLiteral("mts"),
    };
    static const QStringList kAudioExts = {
        QStringLiteral("wav"), QStringLiteral("aif"), QStringLiteral("aiff"),
        QStringLiteral("mp3"), QStringLiteral("flac"), QStringLiteral("m4a"),
    };
    static const QStringList kImageExts = {
        QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg"),
        QStringLiteral("tif"), QStringLiteral("tiff"), QStringLiteral("exr"),
        QStringLiteral("dpx"),
    };

    MediaType t = MediaType::Video;
    if      (kVideoExts.contains(suffix))  t = MediaType::Video;
    else if (kAudioExts.contains(suffix))  t = MediaType::Audio;
    else if (kImageExts.contains(suffix))  t = MediaType::Image;
    if (outType) *outType = t;

    QFileInfo fi(path);
    return fi.fileName();   // default display name
}

int ProjectManager::findIndexInPool(const QString &id) const
{
    for (int i = 0; i < m_mediaPool.size(); ++i) {
        if (m_mediaPool[i].id == id) return i;
    }
    return -1;
}

const MediaItem *ProjectManager::findItem(const QString &id) const
{
    const int idx = findIndexInPool(id);
    return (idx >= 0) ? &m_mediaPool[idx] : nullptr;
}

MediaItem *ProjectManager::findItem(const QString &id)
{
    const int idx = findIndexInPool(id);
    return (idx >= 0) ? &m_mediaPool[idx] : nullptr;
}

// ---- Phase 7.8 — DualPair add ----------------------------------

QString ProjectManager::addDualPairItem(const QString &name,
                                            const DualPairData &data)
{
    MediaItem item;
    item.id   = QUuid::createUuid().toString(QUuid::WithoutBraces);
    item.name = name.isEmpty()
                ? QStringLiteral("Untitled Dual View")
                : name;
    item.path.clear();          // DualPair has no on-disk file
    item.type = MediaType::DualPair;
    item.dualPair = data;
    m_mediaPool.append(item);

    for (ProjectBin &bin : m_bins) {
        if (bin.accepts == MediaType::DualPair) {
            bin.itemIds.append(item.id);
            break;
        }
    }
    markDirty();
    emit binsChanged();
    return item.id;
}

bool ProjectManager::updateDualPairItem(const QString &id,
                                        const DualPairData &data)
{
    const int idx = findIndexInPool(id);
    if (idx < 0) return false;
    if (m_mediaPool[idx].type != MediaType::DualPair) return false;
    m_mediaPool[idx].dualPair = data;
    markDirty();
    emit binsChanged();
    return true;
}

// ---- Phase 3.H.1 — Playlist creation ---------------------------

QString ProjectManager::createPlaylist(const QStringList &paths,
                                       const QString     &name,
                                       double             fps,
                                       int                canvasW,
                                       int                canvasH,
                                       int                gapFrames)
{
    MediaItem item;
    item.id   = QUuid::createUuid().toString(QUuid::WithoutBraces);
    item.type = MediaType::Playlist;
    item.path.clear();   // Playlists have no on-disk path of their own.

    if (name.isEmpty()) {
        const QString stamp = QDateTime::currentDateTime()
                                  .toString(QStringLiteral("yyyyMMdd-HHmmss"));
        item.name = QStringLiteral("Playlist-") + stamp;
    } else {
        item.name = name;
    }

    item.playlist.masterFps        = fps > 0.0 ? fps : 24.0;
    item.playlist.canvasWidth      = canvasW > 0 ? canvasW : 1920;
    item.playlist.canvasHeight     = canvasH > 0 ? canvasH : 1080;
    item.playlist.defaultGapFrames = gapFrames >= 0 ? gapFrames : 10;

    // Each dropped file lands in the standard bins through addMediaFile
    // (dedupe-by-path; existing items return their id). The playlist
    // references them; it does NOT own them.
    for (const QString &p : paths) {
        const QString mediaId = addMediaFile(p);
        if (mediaId.isEmpty()) continue;
        // Reject audio in playlists for v2.0 — the playlist
        // orchestrator runs all clips through VideoDecoder +
        // ScrubDecoder; audio-only files would need a parallel
        // AudioPlayer-driven clip path with its own boundary
        // detection. Audio is still fine standalone (single-media
        // load with the timeline + AudioPlayer wallclock pump).
        if (const MediaItem *added = findItem(mediaId);
            added && added->type == MediaType::Audio) {
            qWarning("createPlaylist: audio media not allowed in "
                     "playlists — skipping '%s'",
                     qPrintable(p));
            continue;
        }
        PlaylistEntry entry;
        entry.mediaId = mediaId;
        // Default trim: -1 = use full clip duration. The decoder /
        // timeline resolves -1 → source's natural in/out at load.
        entry.range.inPoint  = -1.0;
        entry.range.outPoint = -1.0;
        item.playlist.items.append(entry);
    }

    m_mediaPool.append(item);
    for (ProjectBin &bin : m_bins) {
        if (bin.accepts == MediaType::Playlist) {
            bin.itemIds.append(item.id);
            break;
        }
    }
    markDirty();
    emit binsChanged();
    return item.id;
}

// ---- Phase 3.H.4 — playlist items writeback --------------------

bool ProjectManager::replacePlaylistItems(const QString      &playlistId,
                                          const QVariantList &entries)
{
    if (playlistId.isEmpty()) return false;
    const int idx = findIndexInPool(playlistId);
    if (idx < 0) return false;
    MediaItem &it = m_mediaPool[idx];
    if (it.type != MediaType::Playlist) return false;

    QList<PlaylistEntry> next;
    next.reserve(entries.size());
    for (const QVariant &v : entries) {
        const QVariantMap m = v.toMap();
        const QString mediaId = m.value(QStringLiteral("mediaId")).toString();
        if (mediaId.isEmpty()) continue;
        // Same audio rejection as createPlaylist — see comment there.
        if (const MediaItem *referenced = findItem(mediaId);
            referenced && referenced->type == MediaType::Audio) {
            qWarning("replacePlaylistItems: audio media not allowed in "
                     "playlists — skipping id '%s'",
                     qPrintable(mediaId));
            continue;
        }
        PlaylistEntry pe;
        pe.mediaId         = mediaId;
        pe.range.inPoint   = m.value(QStringLiteral("inPoint"),  -1.0).toDouble();
        pe.range.outPoint  = m.value(QStringLiteral("outPoint"), -1.0).toDouble();
        next.append(pe);
    }

    // Skip the binsChanged emit if nothing actually changed —
    // avoids redundant timeline rebuilds in edit hot paths.
    bool same = (next.size() == it.playlist.items.size());
    if (same) {
        for (int i = 0; i < next.size() && same; ++i) {
            const PlaylistEntry &a = next[i];
            const PlaylistEntry &b = it.playlist.items[i];
            if (a.mediaId         != b.mediaId
                || a.range.inPoint  != b.range.inPoint
                || a.range.outPoint != b.range.outPoint) {
                same = false;
            }
        }
        if (same) return true;
    }
    it.playlist.items = next;
    markDirty();
    emit binsChanged();
    if (m_activeItemId == playlistId) emit activeItemIdChanged();
    return true;
}

// ---- Phase 3.H.3 — rename + auto-name --------------------------

bool ProjectManager::renameMediaItem(const QString &id,
                                     const QString &newName)
{
    if (id.isEmpty() || newName.isEmpty()) return false;
    const int idx = findIndexInPool(id);
    if (idx < 0) return false;
    if (m_mediaPool[idx].name == newName) return true;
    m_mediaPool[idx].name = newName;
    markDirty();
    emit binsChanged();
    if (m_activeItemId == id) emit activeItemIdChanged();
    return true;
}

QString ProjectManager::nextPlaylistAutoName() const
{
    static const QRegularExpression rx(
        QStringLiteral("^Playlist_(\\d+)$"));
    int maxN = 0;
    for (const MediaItem &it : m_mediaPool) {
        if (it.type != MediaType::Playlist) continue;
        const auto m = rx.match(it.name);
        if (!m.hasMatch()) continue;
        const int n = m.captured(1).toInt();
        if (n > maxN) maxN = n;
    }
    return QStringLiteral("Playlist_%1")
        .arg(maxN + 1, 2, 10, QChar('0'));
}

// ---- Phase 7.7 Stage 5 — Source B accessors --------------------

QString ProjectManager::bSourcePath() const
{
    if (m_bSourceMediaId.isEmpty()) return {};
    const MediaItem *item = findItem(m_bSourceMediaId);
    return item ? item->path : QString();
}

QString ProjectManager::bSourceName() const
{
    if (m_bSourceMediaId.isEmpty()) return {};
    const MediaItem *item = findItem(m_bSourceMediaId);
    return item ? item->name : QString();
}

void ProjectManager::setBSourceMediaId(const QString &id)
{
    if (id == m_bSourceMediaId) return;
    if (!id.isEmpty()) {
        const int idx = findIndexInPool(id);
        if (idx < 0) {
            qWarning("ProjectManager::setBSourceMediaId — unknown id '%s'",
                     qPrintable(id));
            return;
        }
        // Belt + suspenders against WindowManager::setBSource —
        // catches programmatic / project-load paths that bypass
        // the path-based setter.
        if (m_mediaPool[idx].type == MediaType::Audio) {
            qWarning("ProjectManager::setBSourceMediaId — audio media "
                     "not allowed as B-source (id=%s)",
                     qPrintable(id));
            return;
        }
    }
    m_bSourceMediaId = id;
    markDirty();
    emit bSourceChanged();
}

void ProjectManager::clearBSource()
{
    if (m_bSourceMediaId.isEmpty()) return;
    m_bSourceMediaId.clear();
    markDirty();
    emit bSourceChanged();
}

QString ProjectManager::addMediaFile(const QString &path)
{
    QFileInfo fi(path);
    if (!fi.exists()) {
        qWarning("ProjectManager: addMediaFile — '%s' does not exist",
                 qPrintable(path));
        return {};
    }
    const QString abs = fi.absoluteFilePath();
    // Recents bumps to top whether this is a new add or a dedup
    // hit — mirrors "files the user reached for recently" rather
    // than "files newly added to this project".
    emit mediaFileAdded(abs);

    // Phase 7.4: if this is an image file, probe the directory
    // for a numbered sibling pattern. Two-or-more matching files
    // → it's a sequence; one match → still image. Routes through
    // the same dedupe path so dropping any frame from an already-
    // imported sequence returns the existing id.
    const QString suffixLower = fi.suffix().toLower();
    if (isImageExtension(suffixLower)) {
        QString seqDir, seqPattern, seqBase;
        int seqStart = 0, seqEnd = 0, seqPadding = 0;
        if (detectImageSequence(abs, seqDir, seqPattern, seqBase,
                                seqStart, seqEnd, seqPadding)) {
            // Dedupe by directory + pattern.
            for (const MediaItem &existing : m_mediaPool) {
                if (existing.type == MediaType::ImageSequence &&
                    existing.imageSeq.directory == seqDir &&
                    existing.imageSeq.pattern == seqPattern) {
                    return existing.id;
                }
            }
            return addImageSequenceFromFile(abs, 24.0);
        }
        // Single-image fall-through.
    }

    // Dedupe by absolute path. Multiple entries pointing at the
    // same file is a usability footgun (same name twice in a bin).
    for (const MediaItem &existing : m_mediaPool) {
        if (existing.path == abs) {
            return existing.id;
        }
    }

    MediaItem item;
    item.id   = QUuid::createUuid().toString(QUuid::WithoutBraces);
    item.name = detectType(abs, &item.type);
    item.path = abs;
    // duration / hasAudio populate when the player opens the file
    // (Phase 7.2 inspector pulls them from VideoDecoder).
    m_mediaPool.append(item);

    // Route to the bin whose `accepts` matches the item's type.
    // ImageSequence falls into "Images". Unknown types (none
    // currently) would silently miss a bin — caller would still
    // get the id back, so addMediaFile never fails on routing.
    const MediaType routeType = (item.type == MediaType::ImageSequence)
                                ? MediaType::Image : item.type;
    for (ProjectBin &bin : m_bins) {
        if (bin.accepts == routeType) {
            bin.itemIds.append(item.id);
            break;
        }
    }
    markDirty();
    emit binsChanged();

    // Phase 3.B — schedule async metadata extraction for video / audio
    // single files (image sequences fan out via their own detector in
    // Phase 3.C). Result lands on metadataReady → patches the item +
    // emits binsChanged again so the inspector reflows.
    if ((item.type == MediaType::Video || item.type == MediaType::Audio)
        && m_metadataService) {
        m_metadataService->requestVideoExtraction(item.id, abs);
        // Phase 3.D — Adobe metadata via exiftool runs in parallel.
        // Fast files (~50 KB) won't have Adobe tags but exiftool is
        // forgiving and returns empty for missing keys.
        m_metadataService->requestAdobeExtraction(item.id, abs);
    }
    return item.id;
}

QString ProjectManager::addImageSequenceFromFile(const QString &anyFilePath,
                                                 double fps)
{
    QFileInfo fi(anyFilePath);
    if (!fi.exists()) {
        qWarning("ProjectManager: addImageSequenceFromFile — '%s' does not exist",
                 qPrintable(anyFilePath));
        return {};
    }
    const QString abs = fi.absoluteFilePath();

    QString dir, pattern, baseLabel;
    int start = 0, end = 0, padding = 0;
    QList<int> missing;
    if (!detectImageSequence(abs, dir, pattern, baseLabel,
                             start, end, padding, &missing)) {
        // Fall back to single-image route.
        return addMediaFile(abs);
    }

    // Dedupe.
    for (const MediaItem &existing : m_mediaPool) {
        if (existing.type == MediaType::ImageSequence &&
            existing.imageSeq.directory == dir &&
            existing.imageSeq.pattern == pattern) {
            return existing.id;
        }
    }

    if (fps <= 0.0) fps = 24.0;
    const int frameCount = end - start + 1;

    MediaItem item;
    item.id   = QUuid::createUuid().toString(QUuid::WithoutBraces);
    item.name = QStringLiteral("%1 [%2-%3]")
                    .arg(baseLabel)
                    .arg(start, padding, 10, QLatin1Char('0'))
                    .arg(end,   padding, 10, QLatin1Char('0'));
    // path is the first concrete file — useful for the Inspector
    // hover, drag-back-out flows, and as the dedupe fallback if
    // something later strips imageSeq.directory.
    char firstBuf[1024];
    std::snprintf(firstBuf, sizeof(firstBuf),
                  pattern.toUtf8().constData(), start);
    item.path = QDir(dir).filePath(QString::fromUtf8(firstBuf));
    item.type = MediaType::ImageSequence;
    item.duration = static_cast<double>(frameCount) / fps;
    item.hasAudio = false;

    item.imageSeq.directory     = dir;
    item.imageSeq.pattern       = pattern;
    item.imageSeq.frameCount    = frameCount;
    item.imageSeq.startFrame    = start;
    item.imageSeq.endFrame      = end;
    item.imageSeq.frameRate     = fps;
    item.imageSeq.duration      = item.duration;
    item.imageSeq.format        = fi.suffix().toUpper();
    item.imageSeq.missingFrames = std::move(missing);
    // width/height fill in once the decoder probes (WindowManager).

    // EXR layer discovery — open the first frame's header to
    // enumerate parts / channel-prefix tokens. Done at import time
    // so the inspector's layer dropdown is populated before the
    // user opens the clip.
    if (item.imageSeq.format == QStringLiteral("EXR")) {
        const auto layers = EXRImageLoader::discoverLayers(
            item.path.toStdString());
        QStringList discovered;
        for (const auto &name : layers) {
            discovered.append(QString::fromStdString(name));
        }
        qInfo("ProjectManager: EXR '%s' layers (%lld): [%s]",
              qPrintable(item.path),
              static_cast<long long>(discovered.size()),
              qPrintable(discovered.join(", ")));
        item.imageSeq.availableLayers = discovered;
        if (!discovered.isEmpty()) {
            // Default to the first layer (usually "default" for
            // ffmpeg-generated EXRs, "beauty" or similar for
            // typical VFX renders that name their primary part).
            item.imageSeq.layer = discovered.first();
        }
        // Header-only compression probe — same MultiPartInputFile
        // open as discoverLayers, kept as a separate call so the
        // EXR loader interface stays single-purpose. Cheap (no
        // pixel decode); empty string surfaces nothing in the
        // inspector.
        const auto comp = EXRImageLoader::compressionName(
            item.path.toStdString());
        if (!comp.empty()) {
            item.imageSeq.compression = QString::fromStdString(comp);
        }
    }

    m_mediaPool.append(item);
    for (ProjectBin &bin : m_bins) {
        if (bin.accepts == MediaType::Image) {
            bin.itemIds.append(item.id);
            break;
        }
    }
    markDirty();
    emit binsChanged();
    return item.id;
}

bool ProjectManager::setSequenceLayer(const QString &itemId,
                                      const QString &layer)
{
    const int idx = findIndexInPool(itemId);
    if (idx < 0) return false;
    MediaItem &it = m_mediaPool[idx];
    if (it.type != MediaType::ImageSequence) return false;
    if (it.imageSeq.layer == layer) return true;
    it.imageSeq.layer = layer;
    markDirty();
    // Re-emit activeItemIdChanged when this is the active item so
    // QML bindings (inspector, etc.) re-read the activeItem map.
    if (m_activeItemId == itemId) {
        emit activeItemIdChanged();
    }
    return true;
}

void ProjectManager::setSequenceProbedDimensions(const QString &itemId,
                                                  int width, int height)
{
    if (width <= 0 || height <= 0) return;
    const int idx = findIndexInPool(itemId);
    if (idx < 0) return;
    MediaItem &it = m_mediaPool[idx];
    if (it.type != MediaType::ImageSequence) return;
    if (it.imageSeq.width == width && it.imageSeq.height == height) return;
    it.imageSeq.width  = width;
    it.imageSeq.height = height;
    // Probed values: do NOT markDirty. The user didn't change anything;
    // we're filling in metadata the cache resolved at first-frame open.
    // Bindings nudge via activeItemIdChanged when this is the live item.
    if (m_activeItemId == itemId) {
        emit activeItemIdChanged();
    }
}

bool ProjectManager::setImageSeqFrameRate(const QString &itemId, double fps)
{
    if (fps <= 0.0) return false;
    const int idx = findIndexInPool(itemId);
    if (idx < 0) return false;
    MediaItem &it = m_mediaPool[idx];
    if (it.type != MediaType::ImageSequence) return false;
    if (qFuzzyCompare(it.imageSeq.frameRate, fps)) return true;

    it.imageSeq.frameRate = fps;
    // Frame count is invariant under an fps re-tune — re-derive the
    // durations from it so the stored item matches what
    // TimelineController::setFrameRate computed for the live timeline.
    if (it.imageSeq.frameCount > 0) {
        const double dur =
            static_cast<double>(it.imageSeq.frameCount) / fps;
        it.imageSeq.duration = dur;
        it.duration          = dur;
    }
    markDirty();
    // Nudge the inspector to re-read on whichever side shows this item
    // (mirrors setVideoRangeOverride / setAudioRoutingMode). Avoids
    // binsChanged so we don't trip the playlist-timeline rebuild hook.
    if (m_activeItemId == itemId || m_bSourceMediaId == itemId) {
        emit activeItemIdChanged();
        emit bSourceChanged();
    }
    return true;
}

bool ProjectManager::setVideoRangeOverride(const QString &itemId, int range)
{
    const int idx = findIndexInPool(itemId);
    if (idx < 0) return false;
    MediaItem &it = m_mediaPool[idx];
    if (it.type != MediaType::Video) return false;
    const VideoRange next = (range == 1) ? VideoRange::Full
                          : (range == 2) ? VideoRange::Limited
                          :                VideoRange::Auto;
    if (it.videoRangeOverride == next) return true;
    it.videoRangeOverride = next;
    markDirty();
    // Dedicated change signal — WindowManager hooks it to reconfigure
    // the live decoder (single-flow VideoDecoder OR DualPlaybackController
    // per-side range atomics) without waiting for the next full reopen.
    // Same pattern as audioRoutingModeChanged.
    emit videoRangeOverrideChanged(itemId, static_cast<int>(next));
    if (m_activeItemId == itemId || m_bSourceMediaId == itemId) {
        emit activeItemIdChanged();
        emit bSourceChanged();
    }
    return true;
}

bool ProjectManager::setPixelAspect(const QString &itemId, int mode,
                                    int parNum, int parDen)
{
    const int idx = findIndexInPool(itemId);
    if (idx < 0) return false;
    MediaItem &it = m_mediaPool[idx];
    if (it.type != MediaType::Video) return false;
    const PixelAspectMode nextMode =
        (mode == 1) ? PixelAspectMode::Detected
      : (mode == 2) ? PixelAspectMode::Custom
      :               PixelAspectMode::Square;
    // Custom ratio is only meaningful for Custom mode; clamp to a sane
    // positive rational so a stray 0 from the UI can't produce a NaN
    // aspect downstream.
    int nNum = (parNum > 0) ? parNum : 1;
    int nDen = (parDen > 0) ? parDen : 1;
    if (nextMode != PixelAspectMode::Custom) { nNum = it.customParNum;
                                               nDen = it.customParDen; }
    if (it.pixelAspectMode == nextMode
        && it.customParNum == nNum && it.customParDen == nDen) return true;
    it.pixelAspectMode = nextMode;
    it.customParNum    = nNum;
    it.customParDen    = nDen;
    markDirty();
    // Dedicated change signal — WindowManager hooks it to push the
    // resulting effective ratio into the live renderer (single-flow A
    // OR DualPlaybackController per-side) without a reopen. Mirrors
    // videoRangeOverrideChanged.
    emit pixelAspectChanged(itemId, static_cast<int>(nextMode), nNum, nDen);
    if (m_activeItemId == itemId || m_bSourceMediaId == itemId) {
        emit activeItemIdChanged();
        emit bSourceChanged();
    }
    return true;
}

bool ProjectManager::setAudioRoutingMode(const QString &itemId, int mode)
{
    const int idx = findIndexInPool(itemId);
    if (idx < 0) return false;
    MediaItem &it = m_mediaPool[idx];
    if (it.type != MediaType::Video) return false;
    const AudioRoutingMode next = (mode == 1) ? AudioRoutingMode::Downmix5_1
                                : (mode == 2) ? AudioRoutingMode::Stereo7_8
                                :               AudioRoutingMode::Auto;
    if (it.audioRoutingMode == next) return true;
    it.audioRoutingMode = next;
    markDirty();
    // Always emit the dedicated change signal — WindowManager hooks it
    // to reconfigure the live decoder regardless of whether this item
    // is the currently-displayed inspector side. Then nudge the
    // inspector rebind on whichever side(s) match this id.
    emit audioRoutingModeChanged(itemId, static_cast<int>(next));
    if (m_activeItemId == itemId || m_bSourceMediaId == itemId) {
        emit activeItemIdChanged();
        emit bSourceChanged();
    }
    return true;
}

bool ProjectManager::detectImageSequence(const QString &filePath,
                                         QString &outDirectory,
                                         QString &outPattern,
                                         QString &outBaseLabel,
                                         int     &outStartFrame,
                                         int     &outEndFrame,
                                         int     &outPadding,
                                         QList<int> *outMissingFrames)
{
    QFileInfo fi(filePath);
    const QString stem = fi.completeBaseName();   // "shot_010"
    const QString ext  = fi.suffix();              // "png"
    if (stem.isEmpty() || ext.isEmpty()) return false;

    // Match: (capture-base-non-greedy)(separator . _ -)?(digits)
    // The old app uses two regex variants — one with a separator
    // (shot_010, frame.0001), one without (image0001). We try
    // separator-aware first; fall through to no-separator if no
    // match. The separator is a single character or empty.
    static const QRegularExpression rxWithSep(
        QStringLiteral("^(.+)([_.\\-])(\\d+)$"));
    static const QRegularExpression rxNoSep(
        QStringLiteral("^(.+?)(\\d{3,})$"));

    QString base, sep, digits;
    QRegularExpressionMatch m = rxWithSep.match(stem);
    if (m.hasMatch()) {
        base   = m.captured(1);
        sep    = m.captured(2);
        digits = m.captured(3);
    } else {
        m = rxNoSep.match(stem);
        if (!m.hasMatch()) return false;
        base   = m.captured(1);
        sep    = QString();
        digits = m.captured(2);
    }
    if (digits.size() < 3) {
        // Reject 1-2 digit numbers — too easy to false-positive on
        // file names like "logo_v2.png" or "shot1.png".
        return false;
    }

    const int padding = digits.size();
    QDir dir = fi.absoluteDir();

    // Build a name filter that matches the same base+sep + any
    // numeric suffix + same extension. QDir filters use globs, not
    // regex, so this is approximate; we re-validate each entry
    // with a regex below to extract its frame number.
    const QString glob = QStringLiteral("%1%2*.%3")
                             .arg(base, sep, ext);

    QStringList found = dir.entryList(QStringList() << glob,
                                      QDir::Files);
    if (found.size() < 2) return false;

    QRegularExpression rxEntry(
        QStringLiteral("^%1%2(\\d+)\\.%3$")
            .arg(QRegularExpression::escape(base),
                 QRegularExpression::escape(sep),
                 QRegularExpression::escape(ext)),
        QRegularExpression::CaseInsensitiveOption);

    int minFrame = INT_MAX;
    int maxFrame = INT_MIN;
    int matchCount = 0;
    QSet<int> seenFrames;
    for (const QString &name : found) {
        const auto em = rxEntry.match(name);
        if (!em.hasMatch()) continue;
        // Require consistent padding for all members of the
        // sequence — otherwise "shot_1.png" and "shot_001.png"
        // would both match the same base regex and the printf
        // pattern would be ambiguous.
        if (em.captured(1).size() != padding) continue;
        const int n = em.captured(1).toInt();
        minFrame = std::min(minFrame, n);
        maxFrame = std::max(maxFrame, n);
        seenFrames.insert(n);
        ++matchCount;
    }
    if (matchCount < 2 || minFrame == INT_MAX) return false;

    // Missing-frame discovery — walk the [min..max] span and record
    // any frame numbers that don't appear in the directory listing.
    // Returned as 0-based offsets from minFrame (matches the
    // ImageSequenceCache's internal frame-number semantics).
    QList<int> missing;
    if (outMissingFrames || maxFrame > minFrame) {
        for (int n = minFrame; n <= maxFrame; ++n) {
            if (!seenFrames.contains(n)) {
                missing.append(n - minFrame);
            }
        }
    }
    if (outMissingFrames) *outMissingFrames = missing;

    outDirectory  = dir.absolutePath();
    outBaseLabel  = base + (sep.isEmpty() ? QString() : sep);
    // Build the printf-style pattern by concatenation — QString::arg
    // can't be used here because the literal `%0<padding>d` collides
    // with arg's own %N placeholders. The result for padding=4 is
    // e.g. "shot_%04d.png".
    outPattern    = base + sep
                  + QStringLiteral("%0") + QString::number(padding)
                  + QStringLiteral("d.") + ext;
    outStartFrame = minFrame;
    outEndFrame   = maxFrame;
    outPadding    = padding;
    const int span = maxFrame - minFrame + 1;
    if (missing.isEmpty()) {
        qInfo("detectImageSequence: base='%s' sep='%s' padding=%d "
              "minFrame=%d maxFrame=%d matchCount=%d (span=%d)",
              qPrintable(base), qPrintable(sep), padding,
              minFrame, maxFrame, matchCount, span);
    } else {
        qWarning("detectImageSequence: base='%s' sep='%s' padding=%d "
                 "minFrame=%d maxFrame=%d matchCount=%d (span=%d) — "
                 "%d frame(s) missing on disk",
                 qPrintable(base), qPrintable(sep), padding,
                 minFrame, maxFrame, matchCount, span, missing.size());
    }
    return true;
}

bool ProjectManager::removeMediaItem(const QString &id)
{
    const int idx = findIndexInPool(id);
    if (idx < 0) return false;
    m_mediaPool.removeAt(idx);
    for (ProjectBin &bin : m_bins) {
        bin.itemIds.removeAll(id);
    }
    if (m_activeItemId == id) {
        m_activeItemId.clear();
        emit activeItemIdChanged();
    }
    if (m_bSourceMediaId == id) {
        m_bSourceMediaId.clear();
        emit bSourceChanged();
    }
    markDirty();
    emit binsChanged();
    return true;
}

void ProjectManager::setActiveItem(const QString &id)
{
    if (id == m_activeItemId) {
        // Even if the id is the same, re-emit loadRequested when
        // the user explicitly clicks the row again. Useful for
        // "reload current" flows. Kept conservative — silent on
        // duplicate calls.
        return;
    }
    const int idx = findIndexInPool(id);
    if (idx < 0) {
        qWarning("ProjectManager: setActiveItem — unknown id '%s'",
                 qPrintable(id));
        return;
    }
    m_activeItemId = id;
    emit activeItemIdChanged();
    emit loadRequested(m_mediaPool[idx]);
}

QVariantList ProjectManager::bins() const
{
    QVariantList out;
    out.reserve(m_bins.size());
    for (const ProjectBin &bin : m_bins) {
        QVariantMap binMap;
        binMap[QStringLiteral("name")]    = bin.name;
        binMap[QStringLiteral("accepts")] = static_cast<int>(bin.accepts);
        binMap[QStringLiteral("isOpen")]  = bin.isOpen;
        binMap[QStringLiteral("count")]   = bin.itemIds.size();

        QVariantList items;
        items.reserve(bin.itemIds.size());
        for (const QString &itemId : bin.itemIds) {
            const int idx = findIndexInPool(itemId);
            if (idx < 0) continue;
            const MediaItem &it = m_mediaPool[idx];
            QVariantMap entry;
            entry[QStringLiteral("id")]       = it.id;
            entry[QStringLiteral("name")]     = it.name;
            entry[QStringLiteral("path")]     = it.path;
            entry[QStringLiteral("type")]     = static_cast<int>(it.type);
            entry[QStringLiteral("duration")] = it.duration;
            entry[QStringLiteral("hasAudio")] = it.hasAudio;
            items.append(entry);
        }
        binMap[QStringLiteral("items")] = items;
        out.append(binMap);
    }
    return out;
}

QVariantMap ProjectManager::mediaItemMap(const QString &id) const
{
    QVariantMap out;
    if (id.isEmpty()) return out;
    const int idx = findIndexInPool(id);
    if (idx < 0) return out;
    const MediaItem &it = m_mediaPool[idx];
    out[QStringLiteral("id")]       = it.id;
    out[QStringLiteral("name")]     = it.name;
    out[QStringLiteral("path")]     = it.path;
    out[QStringLiteral("type")]     = static_cast<int>(it.type);
    out[QStringLiteral("duration")] = it.duration;
    out[QStringLiteral("hasAudio")] = it.hasAudio;

    // Phase 3.G — per-clip YUV range override. Default Auto (0)
    // follows detected `video.colorRange`; Full (1) / Limited (2)
    // override the YUV→RGB conversion.
    out[QStringLiteral("videoRangeOverride")] =
        static_cast<int>(it.videoRangeOverride);

    // Per-clip pixel-aspect override (0 = Square, 1 = Detected,
    // 2 = Custom) + the custom rational. Top-level (not under `video`)
    // so the Inspector pill highlights correctly even before metadata
    // loads. The detected SAR lives in the `video` sub-map below.
    out[QStringLiteral("pixelAspectMode")] =
        static_cast<int>(it.pixelAspectMode);
    out[QStringLiteral("customParNum")] = it.customParNum;
    out[QStringLiteral("customParDen")] = it.customParDen;

    // Per-clip audio routing mode (0 = Auto, 1 = 5.1 Downmix,
    // 2 = Stereo 7-8). Inspector pill row binds to this for the
    // active-pill highlight; setAudioRoutingMode mutates and emits.
    out[QStringLiteral("audioRoutingMode")] =
        static_cast<int>(it.audioRoutingMode);

    // Phase 3.B — surface extracted video metadata. `videoLoaded`
    // gates Inspector visibility of the Video / Color / Audio
    // sub-sections; populated once MetadataService completes.
    out[QStringLiteral("videoLoaded")] = it.video.loaded;
    if (it.video.loaded) {
        QVariantMap v;
        v[QStringLiteral("width")]            = it.video.width;
        v[QStringLiteral("height")]           = it.video.height;
        v[QStringLiteral("frameRate")]        = it.video.frameRate;
        v[QStringLiteral("totalFrames")]      = it.video.totalFrames;
        v[QStringLiteral("duration")]         = it.video.duration;
        v[QStringLiteral("videoCodec")]       = it.video.videoCodec;
        v[QStringLiteral("pixelFormat")]      = it.video.pixelFormat;
        v[QStringLiteral("bitDepth")]         = it.video.bitDepth;
        v[QStringLiteral("hasAlpha")]         = it.video.hasAlpha;
        // Detected sample aspect ratio + derived display aspect. The
        // Inspector shows these read-only and pre-fills the Custom
        // fields from them; displayAspect = (W/H)·(sarNum/sarDen).
        v[QStringLiteral("sarNum")]           = it.video.sarNum;
        v[QStringLiteral("sarDen")]           = it.video.sarDen;
        v[QStringLiteral("isAnamorphic")]     = it.video.isAnamorphic;
        if (it.video.height > 0 && it.video.sarDen > 0) {
            v[QStringLiteral("displayAspect")] =
                (static_cast<double>(it.video.width) * it.video.sarNum)
                / (static_cast<double>(it.video.height) * it.video.sarDen);
        }
        v[QStringLiteral("colorspace")]       = it.video.colorspace;
        v[QStringLiteral("colorPrimaries")]   = it.video.colorPrimaries;
        v[QStringLiteral("colorTransfer")]    = it.video.colorTransfer;
        v[QStringLiteral("colorRange")]       = it.video.colorRange;
        v[QStringLiteral("nclcTag")]          = it.video.nclcTag;
        v[QStringLiteral("isHdrContent")]     = it.video.isHdrContent;
        v[QStringLiteral("audioCodec")]       = it.video.audioCodec;
        v[QStringLiteral("audioSampleRate")]  = it.video.audioSampleRate;
        v[QStringLiteral("audioChannels")]    = it.video.audioChannels;
        v[QStringLiteral("audioChannelLayoutName")] = it.video.audioChannelLayoutName;
        v[QStringLiteral("hasEmbeddedTimecode")] = it.video.hasEmbeddedTimecode;
        v[QStringLiteral("embeddedTimecode")] = it.video.embeddedTimecode;

        // Phase 3.D — derive start-frame from the SMPTE TC + fps.
        // "01:00:00:00" @ 23.976 → 86400 frames. Useful when the
        // reviewer wants to cross-reference with the source NLE
        // timeline (which numbers frames from the same start TC).
        if (it.video.hasEmbeddedTimecode && it.video.frameRate > 0.0) {
            int hh = 0, mm = 0, ss = 0, ff = 0;
            if (std::sscanf(it.video.embeddedTimecode.toUtf8().constData(),
                            "%d:%d:%d:%d", &hh, &mm, &ss, &ff) == 4) {
                const int fpsInt = static_cast<int>(
                    std::round(it.video.frameRate));
                const int startFrame = ((hh * 3600) + (mm * 60) + ss) * fpsInt + ff;
                v[QStringLiteral("startFrame")] = startFrame;
            }
        }
        out[QStringLiteral("video")] = v;
    }

    // Phase 3.D — Adobe links + extended timecodes.
    out[QStringLiteral("adobeLoaded")] = it.adobe.loaded;
    if (it.adobe.loaded) {
        QVariantMap a;
        a[QStringLiteral("aeProjectPath")]    = it.adobe.aeProjectPath;
        a[QStringLiteral("premiereWinPath")]  = it.adobe.premiereWinPath;
        a[QStringLiteral("premiereMacPath")]  = it.adobe.premiereMacPath;
        a[QStringLiteral("hasAnyProject")]    = it.adobe.hasAnyAdobeProject();
        a[QStringLiteral("qtStartTimecode")]  = it.adobe.qtStartTimecode;
        a[QStringLiteral("qtTimecode")]       = it.adobe.qtTimecode;
        a[QStringLiteral("qtCreationDate")]   = it.adobe.qtCreationDate;
        a[QStringLiteral("qtMediaCreateDate")]= it.adobe.qtMediaCreateDate;
        a[QStringLiteral("xmpAltTimecode")]   = it.adobe.xmpAltTimecode;
        a[QStringLiteral("hasAnyTimecode")]   = it.adobe.hasAnyTimecode();
        out[QStringLiteral("adobe")] = a;
    }
    // Phase 3.A — File section needs size on disk. Cheap stat, no
    // FFmpeg probe required. -1 if path is missing / unreadable.
    {
        const QFileInfo info(it.path);
        out[QStringLiteral("sizeBytes")] = info.exists()
            ? QVariant::fromValue(info.size())
            : QVariant::fromValue<qint64>(-1);
    }
    if (it.type == MediaType::ImageSequence) {
        QVariantMap seq;
        seq[QStringLiteral("directory")]  = it.imageSeq.directory;
        seq[QStringLiteral("pattern")]    = it.imageSeq.pattern;
        seq[QStringLiteral("frameCount")] = it.imageSeq.frameCount;
        seq[QStringLiteral("startFrame")] = it.imageSeq.startFrame;
        seq[QStringLiteral("endFrame")]   = it.imageSeq.endFrame;
        seq[QStringLiteral("frameRate")]  = it.imageSeq.frameRate;
        seq[QStringLiteral("duration")]   = it.imageSeq.duration;
        seq[QStringLiteral("width")]      = it.imageSeq.width;
        seq[QStringLiteral("height")]     = it.imageSeq.height;
        seq[QStringLiteral("format")]     = it.imageSeq.format;
        seq[QStringLiteral("layer")]      = it.imageSeq.layer;
        seq[QStringLiteral("availableLayers")] =
            QVariant::fromValue(it.imageSeq.availableLayers);
        seq[QStringLiteral("compression")] = it.imageSeq.compression;
        seq[QStringLiteral("audioFile")]  = it.imageSeq.audioFile;
        out[QStringLiteral("imageSeq")]   = seq;
    }
    if (it.type == MediaType::Playlist) {
        QVariantMap pl;
        pl[QStringLiteral("masterFps")]        = it.playlist.masterFps;
        pl[QStringLiteral("canvasWidth")]      = it.playlist.canvasWidth;
        pl[QStringLiteral("canvasHeight")]     = it.playlist.canvasHeight;
        pl[QStringLiteral("defaultGapFrames")] = it.playlist.defaultGapFrames;
        pl[QStringLiteral("loop")]             = it.playlist.loop;

        QVariantList items;
        items.reserve(it.playlist.items.size());
        for (const PlaylistEntry &entry : it.playlist.items) {
            QVariantMap em;
            em[QStringLiteral("mediaId")]  = entry.mediaId;
            em[QStringLiteral("inPoint")]  = entry.range.inPoint;
            em[QStringLiteral("outPoint")] = entry.range.outPoint;
            // Resolve a couple of display fields from the referenced
            // pool entry so the Inspector can render names + paths
            // without doing a second lookup per row.
            const MediaItem *src = findItem(entry.mediaId);
            if (src) {
                em[QStringLiteral("name")]     = src->name;
                em[QStringLiteral("path")]     = src->path;
                em[QStringLiteral("type")]     = static_cast<int>(src->type);
                em[QStringLiteral("duration")] = src->duration;
            }
            items.append(em);
        }
        pl[QStringLiteral("items")] = items;
        pl[QStringLiteral("clipCount")] = items.size();
        out[QStringLiteral("playlist")] = pl;
    }
    return out;
}

QVariantMap ProjectManager::activeItemMap() const
{
    return mediaItemMap(m_activeItemId);
}

QVariantMap ProjectManager::bSourceItemMap() const
{
    return mediaItemMap(m_bSourceMediaId);
}

QVariantList ProjectManager::flatItems() const
{
    // Walk bins in canonical order (Videos / Audio / Images /
    // Playlists) so the flat list naturally groups by type. The
    // QML LeftRail uses ListView.section to insert static type
    // headers between groups — that only renders coherent labels
    // when items are pre-sorted by section property. Within each
    // bin items keep insertion order so newly-added entries
    // append at the bottom of their group.
    QVariantList out;
    out.reserve(m_mediaPool.size());
    for (const ProjectBin &bin : m_bins) {
        for (const QString &itemId : bin.itemIds) {
            const int idx = findIndexInPool(itemId);
            if (idx < 0) continue;
            const MediaItem &it = m_mediaPool[idx];
            QVariantMap entry;
            entry[QStringLiteral("id")]       = it.id;
            entry[QStringLiteral("name")]     = it.name;
            entry[QStringLiteral("path")]     = it.path;
            entry[QStringLiteral("type")]     = static_cast<int>(it.type);
            entry[QStringLiteral("duration")] = it.duration;
            entry[QStringLiteral("hasAudio")] = it.hasAudio;
            out.append(entry);
        }
    }
    return out;
}

void ProjectManager::setBinOpen(int binIndex, bool open)
{
    if (binIndex < 0 || binIndex >= m_bins.size()) return;
    if (m_bins[binIndex].isOpen == open) return;
    m_bins[binIndex].isOpen = open;
    markDirty();
    emit binsChanged();
}

void ProjectManager::onAdobeMetadataReady(const QString &mediaId,
                                          const AdobeMetadata &metadata)
{
    const int idx = findIndexInPool(mediaId);
    if (idx < 0) return;
    m_mediaPool[idx].adobe = metadata;
    emit binsChanged();
    if (m_activeItemId == mediaId) {
        emit activeItemIdChanged();
    }
}

void ProjectManager::onVideoMetadataReady(const QString &mediaId,
                                          const VideoMetadata &metadata)
{
    const int idx = findIndexInPool(mediaId);
    if (idx < 0) return;

    m_mediaPool[idx].video = metadata;

    // Mirror useful top-level fields back to MediaItem so other
    // consumers (timeline, player) see them without round-tripping
    // through `video`. Skip if the existing values are already set
    // (probably populated by a real load via VideoDecoder, which is
    // authoritative once it's open).
    MediaItem &m = m_mediaPool[idx];
    if (m.duration <= 0.0 && metadata.duration > 0.0) {
        m.duration = metadata.duration;
    }
    if (!m.hasAudio && !metadata.audioCodec.isEmpty()) {
        m.hasAudio = true;
    }

    emit binsChanged();
    if (m_activeItemId == mediaId) {
        emit activeItemIdChanged();   // re-fire so QML rebinds activeItem
    }
}

} // namespace qcv
