// ProjectManager — Phase 7.1.
//
// Owns the project's media_pool (flat list of MediaItems by id) and
// the default ProjectBin that lists them. Phase 7.1 ships in-memory
// only — persistence to .qcvproj JSON lands in 7.6.
//
// The old app's ProjectManager was 7,400 LOC of kitchen-sink. This
// port keeps just the data model + signals; UI rendering lives in
// QML (LeftRail's Bins section), and inter-subsystem wiring goes
// through WindowManager rather than callbacks.

#pragma once

#include "media_item.h"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>

namespace qcv {

// A ProjectBin is one of the four FIXED categories (Guide 07 D4 +
// Guide 16). Items route into the bin matching their MediaType
// automatically; users don't create / rename / delete bins. Each
// bin has its own per-session expand/collapse state (.qcvproj v2
// will persist these alongside .qcvproj save).
struct ProjectBin {
    QString      name;       // user-visible label ("Videos", "Audio", …)
    MediaType    accepts;    // routing key — items of this type land here
    QStringList  itemIds;
    bool         isOpen = true;
};

class ProjectManager : public QObject
{
    Q_OBJECT
    // QVariantList of {name, accepts, isOpen, items: [{id,name,…}]}
    // — one entry per fixed bin (Guide 07 D4). Order is the
    // canonical Videos / Audio / Images / Playlists. ListView /
    // Repeater consumers bind to this; binsChanged fires on add /
    // remove / rename / open-toggle.
    Q_PROPERTY(QVariantList bins READ bins NOTIFY binsChanged)
    // Flat-list view of every item in the pool, in insertion order.
    // The LeftRail Phase 7.1 UI renders this directly with a type
    // glyph per row (Videos / Audio / Images / Playlists). The
    // four-bin internal structure persists for .qcvproj write
    // ordering + future inspector filtering.
    Q_PROPERTY(QVariantList flatItems READ flatItems NOTIFY binsChanged)
    Q_PROPERTY(QString activeItemId READ activeItemId NOTIFY activeItemIdChanged)
    // Phase 7.4.a.1 — full record of the active MediaItem as a map,
    // so QML inspectors can render fps / range / pattern / format
    // without round-tripping through individual Q_PROPERTYs. Returns
    // an empty map when nothing is active.
    Q_PROPERTY(QVariantMap activeItem READ activeItemMap NOTIFY activeItemIdChanged)

    // Phase 7.7 Stage 5 — Source B is a top-level architectural
    // element, not a MediaItem flag. Persisted as a single mediaId
    // ref to a regular MediaItem in `bins`; B drag-drop adds the
    // file to the pool (dedupe-by-path) AND sets this ref. Stays
    // empty in single mode; consumed by the dual playback island
    // when compositorMode flips to SBS or Wipe. Empty mediaId means
    // "B unset" — dual mode renders that side transparent.
    Q_PROPERTY(QString bSourceMediaId READ bSourceMediaId
               NOTIFY bSourceChanged)
    // Convenience: the resolved BSource path. Empty when bSource is
    // unset or its referenced MediaItem was removed.
    Q_PROPERTY(QString bSourcePath READ bSourcePath
               NOTIFY bSourceChanged)
    // Display name (basename) for the B chip's label. Empty when
    // unset.
    Q_PROPERTY(QString bSourceName READ bSourceName
               NOTIFY bSourceChanged)
    // Phase 7.7 Stage 8 — full B-side MediaItem map for the dual
    // Inspector's A/B picker. Same shape as activeItem (file size,
    // video metadata, image-seq fields, Adobe links, etc.) — the
    // QML inspector binds to whichever of activeItem / bSourceItem
    // the user has selected. Empty when bSource is unset.
    Q_PROPERTY(QVariantMap bSourceItem READ bSourceItemMap
               NOTIFY bSourceChanged)

    // Phase 7.6 — `.qcvproj` v2 persistence. UUID, path, name reflect
    // the on-disk file (or generated defaults pre-Save). `dirty`
    // tracks whether the in-memory pool has changes vs. the last
    // saved snapshot — bound by the QML window-title decorator and
    // the close-confirmation guard.
    Q_PROPERTY(QString projectUuid     READ projectUuid
               NOTIFY projectFileChanged)
    Q_PROPERTY(QString projectFilePath READ projectFilePath
               NOTIFY projectFileChanged)
    Q_PROPERTY(QString projectName     READ projectName
               NOTIFY projectFileChanged)
    Q_PROPERTY(bool    projectDirty    READ projectDirty
               NOTIFY projectDirtyChanged)

public:
    explicit ProjectManager(QObject *parent = nullptr);
    ~ProjectManager() override;

    QVariantList bins() const;
    QVariantList flatItems() const;
    QString      activeItemId() const { return m_activeItemId; }
    QVariantMap  activeItemMap() const;
    QVariantMap  bSourceItemMap() const;
    // Underlying helper — returns the QVariantMap for any media id
    // in the pool. Empty map for unknown / empty id. activeItemMap
    // and bSourceItemMap both delegate here. Phase 3.H.4: also
    // exposed to QML so the playlist drop handler can resolve a
    // freshly-added file's metadata for clip-spec construction.
    Q_INVOKABLE QVariantMap mediaItemMap(const QString &id) const;

    // Toggle the expand/collapse state of bin index `idx` (0..3).
    // No-op for out-of-range indices.
    Q_INVOKABLE void setBinOpen(int binIndex, bool open);

    // Add a media file from a file picker / drop. Detects type by
    // extension. For an image extension, probes the directory for
    // siblings — if 2+ files match a numbered pattern, the item
    // becomes a single ImageSequence MediaItem (Phase 7.4); otherwise
    // it's added as a single still Image. Returns the new item's
    // id (or the existing id if a dupe is detected).
    Q_INVOKABLE QString addMediaFile(const QString &path);

    // Phase 7.4 — same as addMediaFile but lets the caller specify
    // the playback fps for the resulting sequence. Useful for
    // future "Open Image Sequence with FPS…" dialogs. Defaults match
    // addMediaFile (24 fps). Falls through to single-image behavior
    // if the file isn't part of a sequence.
    Q_INVOKABLE QString addImageSequenceFromFile(const QString &anyFilePath,
                                                 double fps);

    // Phase 3.H.1 — create a Playlist MediaItem from a list of file
    // paths. Each path goes through addMediaFile (dedupe-by-path)
    // so the dropped files also appear in the standard bins; the
    // playlist references them by id. Pass empty paths to create
    // an empty playlist that can be filled later. The playlist
    // routes to the "Playlists" bin. Returns the new playlist's id.
    // `name` may be empty — defaults to a "Playlist-YYYYMMDD-HHMMSS"
    // timestamp form.
    Q_INVOKABLE QString createPlaylist(const QStringList &paths,
                                       const QString     &name      = QString(),
                                       double             fps       = 24.0,
                                       int                canvasW   = 1920,
                                       int                canvasH   = 1080,
                                       int                gapFrames = 10);

    // Phase 7.4.b — set the active EXR layer for an image-sequence
    // item. Mutates the pool entry's imageSeq.layer and emits
    // activeItemIdChanged so the QML inspector + WindowManager
    // both pick up the new selection. No-op for non-EXR items.
    Q_INVOKABLE bool setSequenceLayer(const QString &itemId,
                                      const QString &layer);

    // Persist a retuned playback fps onto an image-sequence MediaItem
    // so the choice survives a media switch and project save/reload.
    // TimelineController::setFrameRate only retunes the live timeline;
    // without this, the inspector's fps chip reverts to the import
    // value on the next load. Mutates imageSeq.frameRate + recomputes
    // imageSeq.duration / item.duration (frame count is invariant),
    // markDirty, and nudges the inspector rebind (activeItemIdChanged /
    // bSourceChanged) when this id is the displayed A or B side. No-op
    // (false) for unknown id, non-ImageSequence, or an unchanged fps.
    Q_INVOKABLE bool setImageSeqFrameRate(const QString &itemId,
                                          double fps);

    // Write probed dimensions back to an image-sequence MediaItem.
    // Called from WindowManager::startImageSequence once the cache
    // has the actual width / height from the first decoded frame.
    // detectImageSequence at import time only fills the file-list
    // metadata; width / height stay at 0 until the cache opens the
    // first frame and reports them. Without this write-back, the
    // inspector's Resolution row sits at "(probing…)" forever
    // because it binds to item.imageSeq.width. Skips when both
    // values already match (no spurious activeItemIdChanged). Not
    // markDirty — these are probed values, not user state.
    void setSequenceProbedDimensions(const QString &itemId,
                                      int width, int height);

    // Phase 3.G — set the per-clip YUV range override. `range` is a
    // `qcv::VideoRange` cast to int (0 = Auto, 1 = Full, 2 = Limited).
    // Mutates `videoRangeOverride` on the pool entry; emits
    // activeItemIdChanged when this is the active item so the QML
    // inspector re-reads. Decoder wiring (cache invalidate + re-
    // decode) is the QML's responsibility — it calls
    // VideoDecoder::setRangeOverride() in parallel for the loaded
    // source so the override takes effect immediately.
    Q_INVOKABLE bool setVideoRangeOverride(const QString &itemId, int range);

    // Set the per-clip audio routing mode. `mode` is a
    // `qcv::AudioRoutingMode` cast to int (0 = Auto, 1 = Downmix5_1,
    // 2 = Stereo7_8). Mirrors setVideoRangeOverride: mutates the
    // pool entry, emits activeItemIdChanged / bSourceChanged when
    // this id matches A or B so the inspector re-reads. Also emits
    // audioRoutingModeChanged so WindowManager can reconfigure the
    // live AudioDecoder / DualAudioMixer side without waiting for
    // the next media load.
    Q_INVOKABLE bool setAudioRoutingMode(const QString &itemId, int mode);

    // Phase 3.H.3 — rename a MediaItem (Bins inline rename + the
    // empty-playlist creation flow). Sets MediaItem.name; emits
    // binsChanged so the bin row re-renders. No-op for unknown id
    // or empty newName. Returns true on a real change.
    Q_INVOKABLE bool renameMediaItem(const QString &id,
                                     const QString &newName);

    // Phase 3.H.3 — pick the next available "Playlist_NN" auto-name
    // (NN = 01..) by scanning the pool for matching names. Used by
    // the empty-playlist "+" button so the new playlist starts with
    // a sensible default that the user can immediately rename inline.
    Q_INVOKABLE QString nextPlaylistAutoName() const;

    // Phase 3.H.4 — replace the playlist's items list verbatim.
    // `entries` is a QVariantList of {mediaId, inPoint, outPoint}
    // maps. Used by WindowManager after each timeline edit to
    // mirror the runtime track-A clips back into the persistent
    // playlist record so navigating away and back doesn't lose
    // content. Emits binsChanged. No-op for non-Playlist target.
    Q_INVOKABLE bool replacePlaylistItems(const QString      &playlistId,
                                          const QVariantList &entries);

    // Remove an item from media_pool + every bin that references it.
    Q_INVOKABLE bool removeMediaItem(const QString &id);

    // Mark the given item as the active selection. Emits
    // activeItemIdChanged so consumers (WindowManager) can load the
    // associated path into the player.
    Q_INVOKABLE void setActiveItem(const QString &id);

    // Look up a pool entry by id. Returns nullptr if not present.
    const MediaItem *findItem(const QString &id) const;
    // Mutable overload — used by WindowManager to write back per-item
    // session-only playback state (e.g. ImageSequenceData::cacheStride)
    // without requiring a one-off setter per field. Caller must not
    // mutate fields that affect identity (id, path, type) — those
    // have dedicated setters that emit the right signals.
    MediaItem *findItem(const QString &id);

    // Phase 7.8 — create a DualPair MediaItem in the pool + route
    // to the "Dual Views" bin. Caller (WindowManager) populates
    // `dualPair` (mediaIdA, mediaIdB, masterFps, masterDuration,
    // clipsA, clipsB, in/out) before calling. Returns the new
    // MediaItem id.
    QString addDualPairItem(const QString &name,
                              const DualPairData &data);

    // Overwrite an existing DualPair's saved state in place (the
    // "Update" path from the viewport Save bar). Replaces the
    // item's `dualPair` payload, keeping its id + name + bin slot.
    // Emits binsChanged + marks the project dirty. No-op (returns
    // false) if `id` is unknown or not a DualPair.
    bool updateDualPairItem(const QString &id,
                            const DualPairData &data);

    // ---- Phase 7.7 Stage 5 — Source B accessors ----
    QString bSourceMediaId() const { return m_bSourceMediaId; }
    QString bSourcePath() const;
    QString bSourceName() const;

    // Set the B-source mediaId. Emits bSourceChanged. Pass empty
    // to clear. Caller is responsible for the addMediaFile-and-
    // dedupe step (WindowManager wraps the two together via its
    // own setBSource Q_INVOKABLE).
    Q_INVOKABLE void setBSourceMediaId(const QString &id);
    Q_INVOKABLE void clearBSource();

    // ---- Phase 7.6 — `.qcvproj` v2 persistence ---------------------

    QString projectUuid()     const { return m_projectUuid; }
    QString projectFilePath() const { return m_projectFilePath; }
    QString projectName()     const { return m_projectName; }
    QString projectCreatedAt()  const { return m_projectCreatedAt; }
    QString projectModifiedAt() const { return m_projectModifiedAt; }
    bool    projectDirty()    const { return m_projectDirty; }

    // Wipes the media pool, clears each bin's item_ids (preserves
    // bin shells + their accept-types), clears active + B-source,
    // generates a new UUID + timestamps, and marks clean. Emits all
    // affected change signals. Called from the constructor so the
    // manager always has a defined project identity.
    Q_INVOKABLE void newProject();

    // Save to `path`. Empty path returns false (caller routes the
    // FileDialog). On success refreshes modified_at, sets the
    // project file path, clears the dirty bit, and emits the
    // matching signals. On failure emits projectError and returns
    // false.
    Q_INVOKABLE bool saveProject(const QString &path);

    // Replace in-memory state from `path`. On failure emits
    // projectError, leaves the manager in a clean newProject state,
    // and returns false.
    Q_INVOKABLE bool openProject(const QString &path);

    // Promote any mutator's local change to the project's dirty bit.
    // Idempotent (no-op if already dirty). Emits projectDirtyChanged
    // only on the false→true transition.
    void markDirty();

    // Read-only accessors used by ProjectIo::save (kept out of the
    // QML surface — friend declarations would force ProjectIo into
    // the class header).
    const QList<MediaItem>  &mediaPool() const { return m_mediaPool; }
    const QList<ProjectBin> &binsRaw()   const { return m_bins; }

    // Atomic state replacement used by ProjectIo::load. Replaces
    // pool + bins + selection + project-identity fields in one shot,
    // then emits binsChanged + activeItemIdChanged + bSourceChanged
    // + projectFileChanged + projectDirtyChanged(false). Caller is
    // responsible for the contents being internally consistent
    // (every bin item_id resolves into the supplied pool).
    void applyLoadedState(QList<MediaItem>  pool,
                          QList<ProjectBin> bins,
                          QString           activeId,
                          QString           bSourceId,
                          QString           uuid,
                          QString           name,
                          QString           createdAt,
                          QString           modifiedAt);

signals:
    void binsChanged();
    void activeItemIdChanged();
    void bSourceChanged();

    // Per-clip audio routing changed. WindowManager listens and
    // reconfigures the live decoder (AudioPlayer for single mode;
    // DualAudioMixer per-side for dual) so the new routing applies
    // mid-playback without re-opening the file. Decoupled from
    // activeItemIdChanged so we don't trigger full inspector
    // rebinds for what is just an audio reroute.
    void audioRoutingModeChanged(const QString &itemId, int mode);

    // Per-clip videoRangeOverride changed. Same pattern as the
    // audio variant: WindowManager hooks this to push the new value
    // into whichever live decoder owns this item — single-flow's
    // VideoDecoder, or DualPlaybackController's per-side range
    // atomics in dual mode. Without this, the QML pill click only
    // updates the persisted MediaItem field; the live render-side
    // YUV→RGB compute stays on its old setting until next reopen.
    void videoRangeOverrideChanged(const QString &itemId, int range);

    // Phase 7.6 — `.qcvproj` save/load + dirty tracking.
    void projectFileChanged();
    void projectDirtyChanged();
    // QML routes this into a top-level error dialog.
    void projectError(const QString &message);

    // Emitted when a media file is successfully added (new pool
    // entry OR a dedup hit). WindowManager listens and pushes
    // the path onto the Recent Media list.
    void mediaFileAdded(const QString &path);
    // Emitted on a successful saveProject / openProject. Path is
    // the absolute project file. WindowManager listens and
    // pushes onto Recent Projects.
    void projectSaved(const QString &path);
    void projectOpened(const QString &path);

    // Emitted whenever the entire project is replaced wholesale —
    // newProject() and applyLoadedState(). WindowManager listens and
    // tears down dual view (the old project's B-source is gone) BEFORE
    // any restored-active-item loadRequested fires, so the dual-persist
    // logic in the loadRequested handler doesn't try to re-pair the new
    // project's A with the previous project's B.
    void projectReplaced();

    // Emitted when setActiveItem successfully resolves to a pool
    // item. WindowManager listens and routes the path into
    // VideoDecoder + AudioPlayer. Decoupled from activeItemIdChanged
    // so the UI label updates even on selection-only events that
    // don't trigger a load.
    void loadRequested(const qcv::MediaItem &item);

public:
    // Public so callers (e.g. WindowManager::setBSource) can pre-
    // classify a path before adding to the pool. Returns the type
    // label string ("Video" / "Audio" / "Image" / "ImageSequence")
    // and writes the enum to *outType when non-null.
    static QString detectType(const QString &path, MediaType *outType);

private:
    int findIndexInPool(const QString &id) const;

    // Image-sequence detection. Given any single file path, parses
    // the filename's trailing digits, scans the directory for
    // siblings sharing the same base+separator+extension, and reports
    // back the printf-style pattern + frame range. Returns false
    // if the file's name doesn't fit the pattern or fewer than 2
    // siblings exist.
    static bool detectImageSequence(const QString &filePath,
                                    QString &outDirectory,
                                    QString &outPattern,
                                    QString &outBaseLabel,
                                    int     &outStartFrame,
                                    int     &outEndFrame,
                                    int     &outPadding,
                                    QList<int> *outMissingFrames = nullptr);

    QList<MediaItem>  m_mediaPool;
    QList<ProjectBin> m_bins;
    QString           m_activeItemId;
    // Phase 7.7 Stage 5 — B-source ref. Empty = unset.
    QString           m_bSourceMediaId;

    // Phase 7.6 — `.qcvproj` v2 identity + dirty tracking. UUID +
    // created_at survive save / load; modified_at refreshes on each
    // save; file path is empty until first Save As / Open.
    QString           m_projectUuid;
    QString           m_projectFilePath;
    QString           m_projectName       = QStringLiteral("Untitled");
    QString           m_projectCreatedAt;
    QString           m_projectModifiedAt;
    bool              m_projectDirty      = false;

    // Phase 3.B — extraction owned here so its lifetime is bounded
    // by ProjectManager. addMediaFile schedules a video extraction;
    // when MetadataService::metadataReady fires, we patch the
    // MediaItem and emit binsChanged so the inspector reflows.
    class MetadataService *m_metadataService = nullptr;

    void onVideoMetadataReady(const QString &mediaId,
                              const VideoMetadata &metadata);
    void onAdobeMetadataReady(const QString &mediaId,
                              const AdobeMetadata &metadata);
};

} // namespace qcv
