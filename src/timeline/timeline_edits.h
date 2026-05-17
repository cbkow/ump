// Timeline edit commands — Phase 7.8 Stage C onward.
//
// QUndoCommand subclasses that mutate the timeline. Each captures
// the inverse state on construction so undo restores. They route
// through TimelineController::applyClipReplacement() for the
// actual mutation, which emits timelineChanged() to refresh QML.
//
// Design rules from Guide 21:
//   - One command per gesture (drag-to-release = one undo entry).
//   - Drag preview is direct manipulation (the clip body updates
//     live); only on release does the corresponding command get
//     pushed. Mid-drag mutation goes through a separate
//     "live preview" hook on TimelineController, NOT through
//     these commands — so the undo stack stays clean.
//   - Esc cancels mid-drag without pushing a command.
//
// Stage C ships TrimClipHeadEdit, TrimClipTailEdit, SlideClipEdit.
// Stage D adds SplitClipEdit, DeleteClipEdit. Stage E adds
// SlipClipEdit. All share the same applyClipReplacement seam.

#pragma once

#include "timeline_types.h"

#include <QString>
#include <QUndoCommand>

namespace qcv {

class TimelineController;

// Replaces one clip on a track with the given new state. Used by
// every edit command's redo / undo. The undo state is captured on
// construction.
class ClipReplaceEdit : public QUndoCommand
{
public:
    ClipReplaceEdit(TimelineController *ctl,
                     const QString &trackId,
                     const QString &clipId,
                     const Clip &newState,
                     const QString &commandText,
                     QUndoCommand *parent = nullptr);

    void undo() override;
    void redo() override;

protected:
    TimelineController *m_ctl;
    QString             m_trackId;
    QString             m_clipId;
    Clip                m_oldState;
    Clip                m_newState;
};

// Convenience subclass — sets a descriptive name in the undo stack.
class TrimClipHeadEdit : public ClipReplaceEdit
{
public:
    TrimClipHeadEdit(TimelineController *ctl,
                      const QString &trackId,
                      const QString &clipId,
                      double newStartTimeSec,
                      double newSourceInSec,
                      QUndoCommand *parent = nullptr);
};

class TrimClipTailEdit : public ClipReplaceEdit
{
public:
    TrimClipTailEdit(TimelineController *ctl,
                      const QString &trackId,
                      const QString &clipId,
                      double newSourceOutSec,
                      QUndoCommand *parent = nullptr);
};

class SlideClipEdit : public ClipReplaceEdit
{
public:
    SlideClipEdit(TimelineController *ctl,
                   const QString &trackId,
                   const QString &clipId,
                   double newStartTimeSec,
                   QUndoCommand *parent = nullptr);
};

// Stage E — slip the underlying source window without changing
// the clip's timeline position or duration. Both sourceIn and
// sourceOut shift by the same `deltaSec`. Caller is responsible
// for clamping the slip to keep sourceIn >= 0.
class SlipClipEdit : public ClipReplaceEdit
{
public:
    SlipClipEdit(TimelineController *ctl,
                  const QString &trackId,
                  const QString &clipId,
                  double newSourceInSec,
                  double newSourceOutSec,
                  QUndoCommand *parent = nullptr);
};

// Stage D — split one clip into two at master-time `atSec`. The
// original clip's range becomes [origStart, atSec); a new clip
// (with a fresh id) covers [atSec, origStart + origDuration).
// Both halves point at the same source media; sourceIn / sourceOut
// shift to match.
class SplitClipEdit : public QUndoCommand
{
public:
    SplitClipEdit(TimelineController *ctl,
                   const QString &trackId,
                   const QString &clipId,
                   double atSec,
                   QUndoCommand *parent = nullptr);

    void undo() override;
    void redo() override;

private:
    TimelineController *m_ctl;
    QString             m_trackId;
    QString             m_originalClipId;
    Clip                m_originalState;
    Clip                m_firstHalf;     // updates the original in place
    Clip                m_secondHalf;    // new clip with fresh id
};

// Stage D — delete one clip from a track. Surrounding clips don't
// shift (no ripple, per Phase 7.8 design). Undo re-inserts at the
// original index.
class DeleteClipEdit : public QUndoCommand
{
public:
    DeleteClipEdit(TimelineController *ctl,
                    const QString &trackId,
                    const QString &clipId,
                    QUndoCommand *parent = nullptr);

    void undo() override;
    void redo() override;

private:
    TimelineController *m_ctl;
    QString             m_trackId;
    QString             m_clipId;
    Clip                m_removedClip;   // captured pre-removal
    int                 m_originalIndex; // for re-insertion on undo
};

// Phase 3.H.4 Stage A — insert a new clip at a specific index in
// the track's clip array. After insert, the controller recomputes
// contiguous startTimes so the playlist stays gapless. Undo
// removes the inserted clip and recomputes again.
class InsertClipEdit : public QUndoCommand
{
public:
    InsertClipEdit(TimelineController *ctl,
                    const QString &trackId,
                    int            atIndex,
                    const Clip    &newClip,
                    QUndoCommand *parent = nullptr);

    void undo() override;
    void redo() override;

    QString insertedClipId() const { return m_clip.id; }

private:
    TimelineController *m_ctl;
    QString             m_trackId;
    int                 m_atIndex;       // clamped to [0, clips.size()]
    Clip                m_clip;          // includes a stable id
};

// Phase 3.H.4 Stage B — slot-based reorder. Removes the clip at
// `fromIndex` and re-inserts it at `toIndex` (in the pre-remove
// numbering, same convention as snapEdgeIndexAtTime). Auto-ripple
// in playlist mode keeps the timeline contiguous. No-op if from
// == to or to == from + 1 (those are the dragged clip's own
// slots — same final arrangement).
class MoveClipEdit : public QUndoCommand
{
public:
    MoveClipEdit(TimelineController *ctl,
                  const QString &trackId,
                  int            fromIndex,
                  int            toIndex,
                  QUndoCommand *parent = nullptr);

    void undo() override;
    void redo() override;

private:
    TimelineController *m_ctl;
    QString             m_trackId;
    int                 m_fromIndex;
    int                 m_toIndex;
};

} // namespace qcv
