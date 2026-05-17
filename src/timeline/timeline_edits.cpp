#include "timeline_edits.h"

#include "timeline_controller.h"

#include <QUuid>
#include <QtLogging>

namespace qcv {

namespace {

// Find the named clip on the named track in a snapshot of the
// timeline. Returns a copy (or default-constructed Clip if not
// found). Used to capture the pre-edit state on command
// construction.
Clip findClipCopy(const Timeline &t,
                   const QString &trackId,
                   const QString &clipId)
{
    for (const Track &track : t.tracks) {
        if (track.id != trackId) continue;
        for (const Clip &c : track.clips) {
            if (c.id == clipId) return c;
        }
    }
    return Clip{};
}

} // namespace

// ---- ClipReplaceEdit -------------------------------------------

ClipReplaceEdit::ClipReplaceEdit(TimelineController *ctl,
                                   const QString &trackId,
                                   const QString &clipId,
                                   const Clip &newState,
                                   const QString &commandText,
                                   QUndoCommand *parent)
    : QUndoCommand(commandText, parent)
    , m_ctl(ctl)
    , m_trackId(trackId)
    , m_clipId(clipId)
    , m_oldState(findClipCopy(ctl ? ctl->timeline() : Timeline{},
                                trackId, clipId))
    , m_newState(newState)
{
}

void ClipReplaceEdit::redo()
{
    if (!m_ctl) return;
    m_ctl->applyClipReplacement(m_trackId, m_clipId, m_newState);
}

void ClipReplaceEdit::undo()
{
    if (!m_ctl) return;
    m_ctl->applyClipReplacement(m_trackId, m_clipId, m_oldState);
}

// ---- TrimClipHeadEdit ------------------------------------------

TrimClipHeadEdit::TrimClipHeadEdit(TimelineController *ctl,
                                     const QString &trackId,
                                     const QString &clipId,
                                     double newStartTimeSec,
                                     double newSourceInSec,
                                     QUndoCommand *parent)
    : ClipReplaceEdit(ctl, trackId, clipId,
                       /* newState = */ Clip{}, /* placeholder */
                       QObject::tr("Trim head"),
                       parent)
{
    // Build new state: keep everything from old, override startTime
    // + sourceIn (and recompute duration so the right edge stays
    // anchored where the user expected). For head trim, the right
    // edge of the clip on the timeline doesn't move — only the left
    // edge slides in.
    m_newState           = m_oldState;
    const double oldEnd  = m_oldState.startTime + m_oldState.duration;
    m_newState.startTime = newStartTimeSec;
    m_newState.sourceIn  = newSourceInSec;
    m_newState.duration  = std::max(0.0, oldEnd - newStartTimeSec);
}

// ---- TrimClipTailEdit ------------------------------------------

TrimClipTailEdit::TrimClipTailEdit(TimelineController *ctl,
                                     const QString &trackId,
                                     const QString &clipId,
                                     double newSourceOutSec,
                                     QUndoCommand *parent)
    : ClipReplaceEdit(ctl, trackId, clipId,
                       Clip{},
                       QObject::tr("Trim tail"),
                       parent)
{
    // Tail trim: startTime stays, sourceOut adjusts, duration
    // follows. (sourceOut - sourceIn) is the new length.
    m_newState           = m_oldState;
    m_newState.sourceOut = newSourceOutSec;
    m_newState.duration  = std::max(0.0, newSourceOutSec - m_oldState.sourceIn);
}

// ---- SlideClipEdit ---------------------------------------------

SlideClipEdit::SlideClipEdit(TimelineController *ctl,
                               const QString &trackId,
                               const QString &clipId,
                               double newStartTimeSec,
                               QUndoCommand *parent)
    : ClipReplaceEdit(ctl, trackId, clipId,
                       Clip{},
                       QObject::tr("Slide clip"),
                       parent)
{
    // Slide: only startTime changes; sourceIn / sourceOut /
    // duration unchanged. The clip's underlying source content is
    // identical; the timeline position shifts.
    m_newState           = m_oldState;
    m_newState.startTime = std::max(0.0, newStartTimeSec);
}

// ---- SlipClipEdit ----------------------------------------------

SlipClipEdit::SlipClipEdit(TimelineController *ctl,
                              const QString &trackId,
                              const QString &clipId,
                              double newSourceInSec,
                              double newSourceOutSec,
                              QUndoCommand *parent)
    : ClipReplaceEdit(ctl, trackId, clipId,
                       Clip{},
                       QObject::tr("Slip clip"),
                       parent)
{
    // Slip: only sourceIn / sourceOut change; startTime + duration
    // stay constant. The clip's underlying media offset shifts
    // while the timeline-side rect is unchanged.
    m_newState           = m_oldState;
    m_newState.sourceIn  = std::max(0.0, newSourceInSec);
    m_newState.sourceOut = std::max(m_newState.sourceIn,
                                       newSourceOutSec);
}

// ---- SplitClipEdit ---------------------------------------------

SplitClipEdit::SplitClipEdit(TimelineController *ctl,
                                const QString &trackId,
                                const QString &clipId,
                                double atSec,
                                QUndoCommand *parent)
    : QUndoCommand(QObject::tr("Cut clip"), parent)
    , m_ctl(ctl)
    , m_trackId(trackId)
    , m_originalClipId(clipId)
{
    // Capture original state up front. Compute the two halves.
    if (!ctl) return;
    for (const Track &track : ctl->timeline().tracks) {
        if (track.id != trackId) continue;
        for (const Clip &c : track.clips) {
            if (c.id != clipId) continue;
            m_originalState = c;
            break;
        }
        break;
    }
    if (m_originalState.id.isEmpty()) return;

    const double splitOffset = atSec - m_originalState.startTime;
    if (splitOffset <= 0.0
        || splitOffset >= m_originalState.duration) {
        // Cut point outside clip — leave halves invalid; redo will
        // no-op.
        return;
    }

    m_firstHalf            = m_originalState;
    m_firstHalf.duration   = splitOffset;
    m_firstHalf.sourceOut  = m_originalState.sourceIn + splitOffset;

    m_secondHalf           = m_originalState;
    m_secondHalf.id        =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_secondHalf.startTime = atSec;
    m_secondHalf.duration  = m_originalState.duration - splitOffset;
    m_secondHalf.sourceIn  = m_originalState.sourceIn + splitOffset;
    // sourceOut is unchanged from original.
}

void SplitClipEdit::redo()
{
    if (!m_ctl) return;
    if (m_secondHalf.id.isEmpty()) return;       // invalid cut, no-op
    m_ctl->applyClipReplacement(m_trackId, m_originalClipId, m_firstHalf);
    m_ctl->applyClipInsertAfter(m_trackId, m_originalClipId, m_secondHalf);
}

void SplitClipEdit::undo()
{
    if (!m_ctl) return;
    if (m_secondHalf.id.isEmpty()) return;
    m_ctl->applyClipRemove(m_trackId, m_secondHalf.id);
    m_ctl->applyClipReplacement(m_trackId, m_originalClipId, m_originalState);
}

// ---- DeleteClipEdit --------------------------------------------

DeleteClipEdit::DeleteClipEdit(TimelineController *ctl,
                                  const QString &trackId,
                                  const QString &clipId,
                                  QUndoCommand *parent)
    : QUndoCommand(QObject::tr("Delete clip"), parent)
    , m_ctl(ctl)
    , m_trackId(trackId)
    , m_clipId(clipId)
    , m_originalIndex(-1)
{
    if (!ctl) return;
    for (const Track &track : ctl->timeline().tracks) {
        if (track.id != trackId) continue;
        for (int i = 0; i < track.clips.size(); ++i) {
            if (track.clips[i].id != clipId) continue;
            m_removedClip = track.clips[i];
            m_originalIndex = i;
            return;
        }
    }
}

void DeleteClipEdit::redo()
{
    if (!m_ctl) return;
    if (m_originalIndex < 0) return;
    m_ctl->applyClipRemove(m_trackId, m_clipId);
}

void DeleteClipEdit::undo()
{
    if (!m_ctl) return;
    if (m_originalIndex < 0) return;
    m_ctl->applyClipInsertAt(m_trackId, m_originalIndex, m_removedClip);
}

// ---- InsertClipEdit (Phase 3.H.4 Stage A) ----------------------

InsertClipEdit::InsertClipEdit(TimelineController *ctl,
                                  const QString &trackId,
                                  int            atIndex,
                                  const Clip    &newClip,
                                  QUndoCommand *parent)
    : QUndoCommand(QObject::tr("Add clip"), parent)
    , m_ctl(ctl)
    , m_trackId(trackId)
    , m_atIndex(atIndex)
    , m_clip(newClip)
{
}

void InsertClipEdit::redo()
{
    if (!m_ctl) return;
    m_ctl->applyClipInsertAt(m_trackId, m_atIndex, m_clip);
    m_ctl->recomputeContiguousStartTimes(m_trackId);
}

void InsertClipEdit::undo()
{
    if (!m_ctl) return;
    m_ctl->applyClipRemove(m_trackId, m_clip.id);
    m_ctl->recomputeContiguousStartTimes(m_trackId);
}

// ---- MoveClipEdit (Phase 3.H.4 Stage B) ------------------------

MoveClipEdit::MoveClipEdit(TimelineController *ctl,
                            const QString &trackId,
                            int            fromIndex,
                            int            toIndex,
                            QUndoCommand *parent)
    : QUndoCommand(QObject::tr("Reorder clip"), parent)
    , m_ctl(ctl)
    , m_trackId(trackId)
    , m_fromIndex(fromIndex)
    , m_toIndex(toIndex)
{
}

namespace {
// Move clip at `from` so it lands at slot `to` (using the pre-
// remove numbering: to == clips.size() means "last"; to == from
// or to == from+1 are no-ops since the clip would end up at the
// same slot in a contiguous arrangement).
void moveClipBySlot(TimelineController *ctl,
                     const QString &trackId,
                     int from, int to)
{
    if (!ctl) return;
    if (from == to || to == from + 1) return;
    Clip removed;
    bool found = false;
    for (const Track &t : ctl->timeline().tracks) {
        if (t.id != trackId) continue;
        if (from < 0 || from >= t.clips.size()) return;
        removed = t.clips[from];
        found = true;
        break;
    }
    if (!found) return;
    ctl->applyClipRemove(trackId, removed.id);
    int adjTo = to;
    if (to > from) adjTo = to - 1;     // pre-remove → post-remove index
    ctl->applyClipInsertAt(trackId, adjTo, removed);
    // applyClipInsertAt auto-ripples in playlist mode.
}
} // namespace

void MoveClipEdit::redo()
{
    moveClipBySlot(m_ctl, m_trackId, m_fromIndex, m_toIndex);
}

void MoveClipEdit::undo()
{
    if (m_fromIndex == m_toIndex || m_toIndex == m_fromIndex + 1) return;
    // Inverse: post-redo the clip sits at (to > from ? to-1 : to).
    // Move it back from there to fromIndex.
    const int postIndex =
        (m_toIndex > m_fromIndex) ? (m_toIndex - 1) : m_toIndex;
    moveClipBySlot(m_ctl, m_trackId, postIndex, m_fromIndex);
}

} // namespace qcv
