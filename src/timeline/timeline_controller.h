// TimelineController — Phase 7.2.c.
//
// The single QObject the rest of the app talks to about playback.
// Owns:
//   - one qcv::Timeline (the data model)
//   - one PlaybackTimer (the clock authority)
//   - (Phase 7.2.d+) per-clip VideoDecoder instances, lifecycle
//     scoped to the clips currently in the timeline
//   - (Phase 7.4+)   per-sequence ImageSequenceDecoder
//
// Per Guide 02 D11: frame pull, not push. PlayerRhiItem's
// synchronize() asks the controller for "what should I display
// now?" — the controller resolves the timer's current position
// → flattener → clip → decoder → frame. The controller never
// pushes frames; the renderer always pulls.
//
// 7.2.c shape: API + state, no decoder ownership yet (that
// migrates from WindowManager in 7.2.d). loadSingleMedia builds
// a 1-clip locked-track timeline; load* for image sequences /
// playlists land in 7.4 / 7.5 with the same API shape.

#pragma once

#include "timeline_types.h"
#include "project/media_item.h"

#include <QObject>
#include <QString>
#include <QUndoStack>
#include <QVariantList>
#include <QVariantMap>

namespace qcv {

class PlaybackTimer;

class TimelineController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(qcv::PlaybackTimer *timer READ timer CONSTANT)
    Q_PROPERTY(QString activeMediaPath READ activeMediaPath NOTIFY timelineChanged)
    Q_PROPERTY(QString activeMediaItemId READ activeMediaItemId NOTIFY timelineChanged)
    Q_PROPERTY(int sourceMode READ sourceModeInt NOTIFY timelineChanged)
    Q_PROPERTY(double duration READ duration NOTIFY timelineChanged)
    Q_PROPERTY(double frameRate READ frameRate NOTIFY timelineChanged)
    // Phase 7.4 — surfaces the active visible clip's ClipMediaKind
    // (0=Video, 1=ImageSequence, 2=Audio) so QML can dispatch
    // input gestures (scrub, step) to the right decoder. Updates
    // on timelineChanged AND when the playhead crosses into a
    // different clip (relevant once playlists land in 7.5).
    Q_PROPERTY(int currentMediaKind READ currentMediaKindInt
               NOTIFY currentMediaKindChanged)
    // Two-track exposure for QML. Each property re-reads on
    // timelineChanged so a Repeater bound to `tracks` rebuilds
    // automatically when loadSecondarySource / clearSecondarySource
    // (or any future edit op) mutates the model.
    Q_PROPERTY(QVariantList tracks    READ trackList  NOTIFY timelineChanged)
    Q_PROPERTY(QVariantMap  trackA    READ trackA     NOTIFY timelineChanged)
    Q_PROPERTY(QVariantMap  trackB    READ trackB     NOTIFY timelineChanged)
    Q_PROPERTY(bool         hasTrackB READ hasTrackB  NOTIFY timelineChanged)

public:
    explicit TimelineController(QObject *parent = nullptr);
    ~TimelineController() override;

    PlaybackTimer *timer() const { return m_timer; }

    // Build a SingleMedia timeline around `item`. One video
    // track, one locked clip spanning duration. Tears down any
    // previous timeline first. Phase 7.2.d makes WindowManager
    // call this whenever ProjectManager.setActiveItem fires.
    void loadSingleMedia(const MediaItem &item,
                         double durationSeconds,
                         double frameRate,
                         bool   hasAudio);

    // Add (or replace) a secondary source track. Used when Source
    // B is loaded for dual-source comparison. Creates / replaces
    // a track with id "B" carrying a single clip that references
    // the secondary media. The primary track ("A") is left alone.
    //
    // The model upgrade lets QML render two stacked lanes and
    // (later) host real edit ops on each side. For now: one clip
    // per track (dual view); playlist mode lands later as one
    // track with N clips.
    void loadSecondarySource(const QString &mediaPath,
                             const QString &displayName,
                             double durationSeconds,
                             double frameRate,
                             bool   hasAudio);

    // Phase 3.H.1 — build a Playlist timeline. Clips reference the
    // listed MediaItems in order; inter-clip gaps of `gapFrames`
    // are inserted automatically (skipped between adjacent clips
    // when `gapFrames == 0`). `trims` parallels `items`: a -1
    // inPoint means "use full clip duration"; otherwise the clip
    // takes [inPoint, outPoint] seconds out of the source.
    //
    // Caller (WindowManager) resolves the playlist's PlaylistEntry
    // list against ProjectManager and hands the items in. Keeps
    // TimelineController decoupled from ProjectManager.
    void loadPlaylist(const QString          &playlistName,
                      const QList<MediaItem> &items,
                      const QList<InOutRange>&trims,
                      double                  masterFps,
                      int                     gapFrames,
                      int                     canvasW,
                      int                     canvasH,
                      const QString          &playlistMediaItemId);

    // Drop the secondary track. Called from WindowManager.closeSourceB.
    void clearSecondarySource();

    // Tear down the active timeline. Pauses the timer, clears
    // the model. Used when the user closes the active media.
    void clear();

    // Phase 7.4.a.1 — change the active timeline's frame rate
    // without rebuilding it. Recomputes duration from the existing
    // frame count (oldDur * oldFps = frameCount; newDur =
    // frameCount / newFps), updates the timer, emits timelineChanged.
    // Position is rescaled proportionally so the same on-screen
    // frame stays under the playhead. Used by the image-sequence
    // inspector when the user retunes fps after import.
    Q_INVOKABLE void setFrameRate(double fps);

    // ---- Reads --------------------------------------------------------------

    const Timeline &timeline() const { return m_timeline; }

    // Phase 7.8 Stage A — temporary diagnostic-only setter that
    // wholesale replaces the timeline state. Used by
    // WindowManager::testHeadTrim to verify the dual controller's
    // master→source translation. Stage C replaces this with proper
    // QUndoCommand-based edit ops. Do not call from production
    // code paths.
    void _replaceTimelineForTesting(const Timeline &t) {
        m_timeline = t;
        emit timelineChanged();
    }

    // ---- Phase 7.8 Stage C — edit op API + QUndoStack ----
    // Public undo stack — exposed for QML to bind canUndo / canRedo
    // and for the Cmd+Z / Cmd+Shift+Z shortcuts.
    QUndoStack *undoStack() { return &m_undoStack; }

    // Edit ops — each pushes a QUndoCommand onto m_undoStack and
    // applies the change immediately via redo(). The corresponding
    // undo() restores the original state. QML calls these from
    // drag-release handlers (the live preview during drag uses
    // applyClipReplacement directly without pushing a command;
    // only on release does the gesture commit as one entry).
    Q_INVOKABLE void trimClipHead(const QString &trackId,
                                    const QString &clipId,
                                    double newStartSec,
                                    double newSourceInSec);
    Q_INVOKABLE void trimClipTail(const QString &trackId,
                                    const QString &clipId,
                                    double newSourceOutSec);
    Q_INVOKABLE void slideClip(const QString &trackId,
                                 const QString &clipId,
                                 double newStartSec);
    // Stage E — slip clip's source-side offset (sourceIn/Out shift
    // by the same delta; startTime + duration unchanged).
    Q_INVOKABLE void slipClip(const QString &trackId,
                                const QString &clipId,
                                double newSourceInSec,
                                double newSourceOutSec);
    // Stage D — split a clip at master-time `atSec`. No-op if the
    // cut falls outside the clip's range. Pushes a SplitClipEdit
    // QUndoCommand. trackId is "A" or "B"; the controller picks
    // the clip on that track containing atSec.
    Q_INVOKABLE void splitClipAt(const QString &trackId, double atSec);
    Q_INVOKABLE void deleteClip(const QString &trackId,
                                  const QString &clipId);

    // Phase 3.H.4 Stage B — slot-based reorder. Moves the clip
    // identified by `clipId` from its current slot to the slot
    // numbered `newIndex` (insert-AT semantic; same as
    // snapEdgeIndexAtTime). Pushes a MoveClipEdit; no-op when
    // the move would result in the same arrangement (drop in own
    // slot or own slot + 1).
    Q_INVOKABLE void moveClipToIndex(const QString &trackId,
                                       const QString &clipId,
                                       int            newIndex);

    // Phase 3.H.4 Stage A — drop-to-add. Inserts a new clip at the
    // given clip-array index (or appends if `atIndex` is past the
    // end), then recomputes contiguous startTimes for every clip on
    // the track. Pushes an InsertClipEdit so it's undoable.
    // `clipSpec` keys (all optional except mediaPath + duration):
    //   name, mediaItemId, mediaPath, mediaKind (int), sourceFps,
    //   sourceDuration, sourceIn, sourceOut, hasAudio,
    //   sequencePattern, sequenceStartFrame, sequenceEndFrame.
    // Returns the new clip's id, or empty on failure.
    Q_INVOKABLE QString insertClipAtIndex(const QString    &trackId,
                                          int               atIndex,
                                          const QVariantMap &clipSpec);

    // Phase 3.H.4 — find the clip-edge index whose nearest edge
    // (start or end) falls within `tolSec` of `timeSec`. Returns
    // the index a clip would be inserted AT to land at that edge:
    //   - snap to clip[i].startTime → returns i
    //   - snap to clip[i].endTime  → returns i+1
    //   - snap to t=0              → returns 0
    //   - snap to track end        → returns clips.size()
    //   - no edge within tolerance → returns -1
    // Caller converts pixel tolerance → seconds via pps.
    Q_INVOKABLE int snapEdgeIndexAtTime(const QString &trackId,
                                          double timeSec,
                                          double tolSec) const;

    // Phase 3.H.4 — recompute every clip on the track to be
    // contiguous starting at t=0 (each clip's startTime = sum of
    // preceding clip durations). Used after insert / move / delete.
    // Public so the Insert/Move QUndoCommands can call it.
    void recomputeContiguousStartTimes(const QString &trackId);

    // Phase 3.H.4 — patch a single clip's source-side duration in
    // place. Used when a freshly-dropped Finder file's metadata
    // arrives async after the clip was inserted with a placeholder
    // duration. ONLY patches when the clip is currently using the
    // full source range (sourceIn == 0, sourceOut == sourceDuration
    // or sourceOut <= 0); skipped if the user has already trimmed.
    // Recomputes contiguous startTimes after patching. Returns
    // true if the clip was actually mutated.
    Q_INVOKABLE bool patchClipDurationFromSource(
        const QString &trackId,
        const QString &clipId,
        double         newSourceDuration,
        double         newSourceFps);
    // Phase 7.8 Stage F — wholesale-replace a track's clip array
    // from a QVariantList of clip maps (same shape trackA() /
    // trackB() return). Used by WindowManager::loadDualView to
    // apply a saved DualPair's clip layout. Bypasses the undo
    // stack — load is a fresh state, not an editable transition.
    Q_INVOKABLE void replaceTrackClips(const QString &trackId,
                                          const QVariantList &clipMaps);
    // Clear the undo stack — called on dual exit so the next dual
    // session starts fresh.
    Q_INVOKABLE void clearUndoStack() { m_undoStack.clear(); }
    // Live-preview hooks — called from QML during drag (no undo
    // entry). The corresponding trimClipHead/Tail/slideClip pushes
    // a single QUndoCommand on release. Esc cancels by calling
    // previewClipReplacement with the original snapshot.
    Q_INVOKABLE void previewTrimHead(const QString &trackId,
                                       const QString &clipId,
                                       double newStartSec,
                                       double newSourceInSec);
    Q_INVOKABLE void previewTrimTail(const QString &trackId,
                                       const QString &clipId,
                                       double newSourceOutSec);
    Q_INVOKABLE void previewSlide(const QString &trackId,
                                    const QString &clipId,
                                    double newStartSec);
    Q_INVOKABLE void previewSlip(const QString &trackId,
                                   const QString &clipId,
                                   double newSourceInSec,
                                   double newSourceOutSec);
    // Restore via clip-shaped QVariantMap — used by Esc-cancel
    // during a drag (the QML stashes the pre-drag snapshot).
    Q_INVOKABLE void restoreClipFromMap(const QString &trackId,
                                          const QString &clipId,
                                          const QVariantMap &snapshot);
    Q_INVOKABLE void undoEdit() { m_undoStack.undo(); }
    Q_INVOKABLE void redoEdit() { m_undoStack.redo(); }
    Q_INVOKABLE bool canUndo() const { return m_undoStack.canUndo(); }
    Q_INVOKABLE bool canRedo() const { return m_undoStack.canRedo(); }

    // Internal mutators — used by QUndoCommand redo/undo. The QML
    // drag handlers during live-preview also call applyClip-
    // Replacement directly (no undo entry); cut/delete commands
    // use the Insert/Remove variants.
    void applyClipReplacement(const QString &trackId,
                                const QString &clipId,
                                const Clip &newState);
    void applyClipInsertAt(const QString &trackId, int index,
                             const Clip &clip);
    void applyClipInsertAfter(const QString &trackId,
                                 const QString &afterClipId,
                                 const Clip &clip);
    void applyClipRemove(const QString &trackId, const QString &clipId);

    // Auto-extend duration if any clip extends past the current
    // timeline end. Called after every successful edit.
    void recalculateDuration();
    QString activeMediaPath() const;
    QString activeMediaItemId() const;
    int     sourceModeInt() const { return static_cast<int>(m_timeline.mode); }
    double  duration() const { return m_timeline.duration; }
    double  frameRate() const { return m_timeline.frameRate; }

    // Active visible clip's mediaKind as int — Video=0,
    // ImageSequence=1, Audio=2. Returns Video when the timeline is
    // empty so the existing video path stays the default-safe
    // dispatch target.
    int     currentMediaKindInt() const;

    // Ask the flattener for the visible clip at the timer's
    // current position. Used by PlayerRhiItem::synchronize() in
    // the future pull path. Returns a copy because the timeline
    // may mutate before the renderer reads further fields.
    Q_INVOKABLE Clip currentVisibleClip() const;

    // QML-facing view of the timeline. Returns each track as a
    // QVariantMap with id/name/role/isVideo/locked + a "clips"
    // QVariantList (each clip is a map with id/name/startTime/
    // duration/sourceIn/sourceOut/mediaPath/mediaKind/sourceFps/
    // sourceDuration). Stable across timelineChanged emits so
    // QML's Repeater rebuilds when the model mutates. Only returns
    // video tracks for now — the audio track stays internal until
    // the timeline UI grows an audio lane.
    Q_INVOKABLE QVariantList trackList() const;
    // Convenience accessors for the dual-source primary / secondary.
    // Returns an empty map when the requested track is absent (e.g.,
    // trackB() before openSourceB has run). Lets QML bind without
    // null-guarding every reference.
    Q_INVOKABLE QVariantMap trackA() const;
    Q_INVOKABLE QVariantMap trackB() const;
    Q_INVOKABLE bool hasTrackB() const;

    // Phase 3.H.1 — when the timeline was built from a Playlist
    // MediaItem, this returns its id so QML / WindowManager can
    // route Inspector / save-state lookups back to the project's
    // playlist record. Empty for SingleMedia / dual sessions.
    Q_INVOKABLE QString playlistMediaItemId() const {
        return m_playlistMediaItemId;
    }
    Q_INVOKABLE bool isPlaylistMode() const {
        return m_timeline.mode == SourceMode::Playlist;
    }

signals:
    // Whole-timeline rebuild — model replaced, decoders should
    // re-prime. WindowManager listens (in 7.2.d) and reroutes
    // VideoDecoder.open through the controller.
    void timelineChanged();

    // Phase 7.4: distinct from timelineChanged because the kind
    // can flip mid-playback in 7.5 (a playlist crossing from a
    // video clip into an image-sequence clip without a model
    // rebuild). For 7.4.a it fires once per loadSingleMedia.
    void currentMediaKindChanged();

private:
    Timeline       m_timeline;
    PlaybackTimer *m_timer = nullptr;
    // Phase 7.8 Stage C — undo stack for dual-mode edits. Reset
    // (cleared) on dual exit so single-mode never sees stale
    // history. The dual session owns its own undo timeline.
    QUndoStack     m_undoStack;     // owned via QObject parent
    // Phase 3.H.1 — id of the source Playlist MediaItem, set by
    // loadPlaylist(). Empty otherwise.
    QString        m_playlistMediaItemId;
};

} // namespace qcv
