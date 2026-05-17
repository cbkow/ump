// TimelinePanel — Phase 7.6.a (timeline view scaffold).
//
// First visible delta of the Phase 7.2 backbone refactor. Reads
// from WindowManager.timeline.* directly — the controller's
// PlaybackTimer is the canonical position, the loaded clip is
// shown as a rectangle on a track row, the ruler ticks at
// timecode boundaries.
//
// Layout per Guide 02 §8:
//
//   ┌──────────────────────────────────────────────────────────┐
//   │  ruler  0:00         0:05         0:10         0:15      │  20 px
//   ├──────────────────────────────────────────────────────────┤
//   │ V1      [─────── shot_010_v3.mov ────────]               │  44 px
//   │                          ▲ playhead                       │
//   └──────────────────────────────────────────────────────────┘
//
// Phase 7.6.a ships the SingleMedia degenerate case: one locked
// track, one clip spanning the whole duration, click-to-seek.
// Drag/trim handles, hover thumbnails, in/out markers, multi-clip
// support land in 7.6.b+ alongside 7.4 (image sequences) and 7.5
// (playlist source mode).

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Qcv

Pane {
    id: root
    padding: 0

    background: Rectangle {
        color: "#181818"
        // No top divider — the TransportBar above already draws
        // its own bottom divider, which sits exactly here. Two
        // adjacent 1-px lines read as a double border.
    }

    // ---- Pull from the controller -------------------------------------------

    // Fired by the playlist drop-area's onDropped — Main.qml uses
    // this to auto-commit any in-progress playlist-rename in the
    // left rail, so the user doesn't have to press Enter before
    // dragging media in.
    signal playlistContentDropped()

    readonly property var ctl:   WindowManager.timeline
    readonly property var timer: ctl ? ctl.timer : null

    readonly property double duration:  ctl ? ctl.duration : 0.0
    readonly property double frameRate: ctl ? ctl.frameRate : 24.0
    readonly property double position:  timer ? timer.position : 0.0
    readonly property string clipName:
        ctl && ctl.activeMediaPath
        ? ctl.activeMediaPath.split("/").pop() : ""
    readonly property bool   loaded: duration > 0.0

    // Follow-playhead during playback. When the playhead crosses the
    // viewport's left or right margin, page-turn so it lands at the
    // left margin (matches old QCView's panel_timeline.cpp:441-475).
    // Skipped while the user is scrubbing — scrubArea.pressed gates
    // the playhead's x binding too, so trying to follow then would
    // fight the cursor.
    onPositionChanged: maybeFollowPlayhead()
    // Unified play state — drives follow-playhead. Dual mode runs
    // its own controller (timer.playing stays false there); for
    // every other mode (single video, image sequence, playlist,
    // mixed playlist), the timer.playing flag is the single source
    // of truth — it's mirrored from VideoDecoder for video and
    // played directly for image sequences.
    readonly property bool playbackActive: {
        if (WindowManager.dualController)
            return WindowManager.dualController.isPlaying;
        const t = WindowManager.timeline
                  ? WindowManager.timeline.timer : null;
        return t ? t.playing : false;
    }

    function maybeFollowPlayhead() {
        if (!playbackActive) return;
        if (pps <= 0 || trackWidth <= 0) return;
        if (typeof scrubArea !== "undefined" && scrubArea.pressed) return;
        const playheadViewX = timeToX(position) - scrollX;
        if (playheadViewX < kFollowMargin
            || playheadViewX > trackWidth - kFollowMargin) {
            scrollX = Math.max(0, Math.min(maxScrollX,
                                  timeToX(position) - kFollowMargin));
        }
    }

    // Shared wheel logic so multiple MouseAreas (track-area, overview
    // bar, the panel-root catch-all) can route to the same zoom/pan
    // implementation. `viewportX` is the cursor's x in trackArea
    // (= ruler / overview) coordinate space — they all share trackWidth.
    function applyWheel(viewportX, dy, ctrlPressed) {
        if (!loaded || pps <= 0 || duration <= 0 || dy === 0) return;
        if (ctrlPressed) {
            scrollX = Math.max(0, Math.min(maxScrollX, scrollX - dy));
            return;
        }
        const factor = Math.pow(1.0015, dy);
        // Floor at fit-to-width so we never expose empty space past
        // the timeline. Long content (feature films) puts fitPps well
        // below kPpsMin (0.5) — a 2-hour movie at 1600 px is ~0.22
        // px/sec — so clamping to kPpsMin would cap zoom-out at
        // ~53 min visible. Using fitPps directly lets the user always
        // see the whole timeline. Ceiling scales with fit-to-width
        // too — for short content (e.g. a 2.5-sec EXR sequence) fit
        // is already > kPpsMax, so a hard cap there would lock zoom
        // completely. 50× fit allows ample frame-level zoom.
        const fitPps = trackWidth / duration;
        const minPps = fitPps;
        const maxPps = Math.max(kPpsMax, fitPps * 50);
        const newPps = Math.max(minPps, Math.min(maxPps, pps * factor));
        if (newPps === pps) return;
        const cursorTime = (viewportX + scrollX) / pps;
        zoomLevel = newPps;
        const newMaxScroll = Math.max(0, duration * newPps - trackWidth);
        scrollX = Math.max(0, Math.min(newMaxScroll,
                              cursorTime * newPps - viewportX));
    }

    // Two-track view. Reactive on timelineChanged via the
    // controller's QVariantMap properties. Single-source case
    // collapses to track A only — trackB is empty + hidden.
    readonly property var    trackAData: ctl ? ctl.trackA : null
    readonly property var    trackBData: ctl ? ctl.trackB : null
    readonly property bool   hasTrackB:  ctl ? ctl.hasTrackB : false

    // Phase 7.8 Stage B — per-track edit mode. A and B can be in
    // edit mode independently. Drag handles + cursor changes (Stage
    // C) gate on these flags. Re-clearing on dual mode exit happens
    // via the dualController binding below.
    property bool editingA: false
    property bool editingB: false

    // Phase 3.H.4 Stage A — drop-to-add gesture state. Updated by
    // the DropArea on trackArea while a drag hovers over the
    // playlist timeline. Drives the dashed drop-ghost outline +
    // accent insertion-bar visuals. Cleared on exit / drop.
    //   active        : true while a uri-list drag is hovering
    //   cursorX       : raw cursor x in trackArea coords
    //   snapIndex     : track-clip index where a drop would land
    //                   (-1 if outside snap tolerance, in which
    //                   case ghost follows cursor exactly)
    //   snapEdgeX     : pixel x of the snap edge (or cursorX when
    //                   no snap)
    //   ghostWidthPx  : pixel width of the placeholder clip outline
    property var dropState: ({})
    readonly property bool dropActive:
        dropState && dropState.active === true

    // Phase 3.H.4 Stage B — reorder ghost state. QML's `var`
    // property binding re-evaluation only fires on identity
    // change, not deep-field mutations. The editState's nested
    // .active / .cursorX writes don't trigger dependent bindings,
    // so we mirror them into top-level reactive properties.
    property bool   reorderActive: false
    property string reorderTrackId: ""
    property real   reorderCursorX: 0
    property real   reorderGhostW:  0
    // Phase 3.H.4 Stage B — destination indicator. The dragged
    // clip's left-edge time in the post-arrangement timeline.
    // Drives the vertical insertion bar so the user sees exactly
    // where the clip will land on release.
    property real   reorderDropSec: 0

    // Phase 7.8 Stage C — active edit gesture state.
    // Populated on mousedown, set to .active=true once the drag
    // threshold is crossed, cleared on release / cancel. The
    // panel-level ghost + badge bind to this.
    //
    // Fields:
    //   trackId, clipId, op ("trimHead"|"trimTail"|"slide")
    //   pending: bool — between mousedown and threshold cross
    //   active: bool — currently dragging
    //   startMouseX: x at mousedown (in trackArea coords)
    //   originalSnapshot: { startTime, duration, sourceIn, sourceOut }
    //   ghostX, ghostW: pixel rect of the original clip (for the
    //     dashed-outline ghost)
    //   badgeText: current proposed-value label
    //   cursorX: current cursor x (for badge positioning)
    property var editState: ({})
    readonly property bool editActive:
        editState && editState.active === true
    readonly property bool editPending:
        editState && editState.pending === true
    // Dual mode is required to enter edit. When dualController goes
    // null, force edit mode off so single-mode timelines never
    // accidentally show editable handles.
    readonly property bool dualActive:
        WindowManager.dualController !== null
        && WindowManager.dualController !== undefined
    // Phase 3.H.1 — playlist mode also needs edit gating: clicking
    // a clip body should be able to select / drag, not just scrub.
    // The track is the same Track A; we don't have a track B in
    // playlist mode (yet), so editingB stays false here.
    readonly property bool playlistActive:
        ctl !== null && ctl !== undefined && ctl.sourceMode === 1
    // Either compositor mode that supports timeline editing — drives
    // EditToggle visibility, gesture catchers, and Cut / Delete
    // shortcut enablement. Dual + playlist both qualify.
    readonly property bool editableMode: dualActive || playlistActive
    onEditableModeChanged: {
        if (!editableMode) {
            editingA = false;
            editingB = false;
            editState = ({});
        }
    }

    // Stage C edit constants.
    readonly property int kHandleW: 8
    readonly property int kDragThresholdPx: 5
    readonly property string kHandleHoverColor: "#ffc864"

    // Stage D — selected clip (for Delete shortcut). Single
    // selection across both tracks. Click on a clip body in edit
    // mode (without drag) selects it; Esc / clicking empty area
    // clears.
    property string selectedTrackId: ""
    property string selectedClipId:  ""
    function clearSelection() {
        selectedTrackId = "";
        selectedClipId  = "";
    }

    // ---- Helpers used by edit gestures + ghost / badge --------------------

    function clipById(trackId, clipId) {
        const td = trackId === "A" ? trackAData : trackBData;
        if (!td || !td.clips) return null;
        for (let i = 0; i < td.clips.length; ++i) {
            if (td.clips[i].id === clipId) return td.clips[i];
        }
        return null;
    }

    function snapshotClip(clip) {
        return {
            startTime: clip.startTime || 0,
            duration:  clip.duration  || 0,
            sourceIn:  clip.sourceIn  || 0,
            sourceOut: clip.sourceOut || 0,
        };
    }

    function tcLabel(seconds) {
        // Same format as ruler — H:MM:SS or M:SS depending on length.
        return formatTimecode(seconds);
    }

    // ---- Hover-zone tracking for edit handle highlights ---------
    // Updated by the per-track edit MAs as the user moves the
    // cursor. Lets ClipDelegate's handle Rectangles tint amber
    // on hover without the delegate owning a hover MouseArea.
    property string editHoverTrack: ""
    property string editHoverClipId: ""
    property string editHoverZone: ""    // "trimHead"|"trimTail"|"slide"|"slip"|""

    // Find the clip on the named track containing trackArea X-coord
    // `xPx`. Returns the clipMap or null.
    function clipAtTrackX(trackId, xPx) {
        const td = trackId === "A" ? trackAData : trackBData;
        if (!td || !td.clips || pps <= 0) return null;
        const sec = (xPx - 1) / pps;
        for (let i = 0; i < td.clips.length; ++i) {
            const c = td.clips[i];
            if (!c || c.isGap) continue;
            if (sec >= c.startTime && sec < c.startTime + c.duration) return c;
        }
        return null;
    }

    // Pick which edit op a press at (trackId, xPx) with these
    // modifiers should fire. Returns { op, clip } or null.
    // Phase 3.H.4 Stage B (lean-in) — body drag in playlist mode
    // is the slot-based "reorder" op (with live ripple preview).
    // Dual mode keeps the free-shift "slide" since there are no
    // neighbors to ripple against.
    function pickEditOp(trackId, xPx, modifiers) {
        const c = clipAtTrackX(trackId, xPx);
        if (!c) return null;
        const startPx = 1 + timeToX(c.startTime || 0);
        const endPx   = startPx
                        + Math.max(2, timeToX(c.duration || 0) - 2);
        if (xPx < startPx + kHandleW) return { op: "trimHead", clip: c };
        if (xPx > endPx - kHandleW)   return { op: "trimTail", clip: c };
        if ((modifiers & Qt.ControlModifier) !== 0)
            return { op: "slip", clip: c };
        if (root.playlistActive) return { op: "reorder", clip: c };
        return { op: "slide", clip: c };
    }

    // ---- Edit gesture handlers (root scope) ---------------------
    // Operate on `editState` independently of any clip delegate.
    // Survive Repeater rebuilds because they're not bound to any
    // recreated item.

    function beginEdit(trackId, clipId, op, mouseXTrack) {
        const c = clipById(trackId, clipId);
        if (!c) return;
        const snap = snapshotClip(c);
        const ghostStartPx = 1 + timeToX(snap.startTime);
        const ghostWidthPx = Math.max(2, timeToX(snap.duration) - 2);
        const state = {
            trackId: trackId,
            clipId:  clipId,
            op:      op,
            pending: true,
            active:  false,
            startMouseX: mouseXTrack,
            originalSnapshot: snap,
            ghostX: ghostStartPx,
            ghostW: ghostWidthPx,
            badgeText: "",
            cursorX: mouseXTrack,
        };
        // Phase 3.H.4 Stage B — reorder needs a snapshot of EVERY
        // track-A clip so the live ripple preview can mutate them
        // and we can restore the originals on Esc / release.
        if (op === "reorder") {
            const td = trackId === "A" ? trackAData : trackBData;
            const all = [];
            let fromIndex = -1;
            if (td && td.clips) {
                for (let i = 0; i < td.clips.length; ++i) {
                    const tc = td.clips[i];
                    all.push({
                        id:        tc.id,
                        startTime: tc.startTime || 0,
                        duration:  tc.duration  || 0,
                        sourceIn:  tc.sourceIn  || 0,
                        sourceOut: tc.sourceOut || 0,
                    });
                    if (tc.id === clipId) fromIndex = i;
                }
            }
            state.allClipsSnapshot = all;
            state.fromIndex = fromIndex;
            state.targetSlot = fromIndex;
            // Phase 3.H.4 — top-level reactive ghost props.
            // reorderActive flips true once threshold is crossed
            // (in updateEditAtTrackX); width is known at press
            // time, so set it now. Track id determines y position.
            root.reorderTrackId = trackId;
            root.reorderGhostW  = ghostWidthPx;
            root.reorderCursorX = mouseXTrack;
        }
        editState = state;
    }

    function updateEditAtTrackX(trackX) {
        if (!editState || !editState.clipId) return;
        if (editState.pending) {
            if (Math.abs(trackX - editState.startMouseX) < kDragThresholdPx)
                return;
            editState.pending = false;
            editState.active  = true;
            // Phase 3.H.4 — flip the top-level reactive flag for
            // the reorder ghost (the editState.active mutation
            // won't propagate through QML var-property bindings).
            if (editState.op === "reorder") {
                root.reorderActive = true;
            }
        }
        if (editState.op === "reorder") {
            root.reorderCursorX = trackX;
        }

        const deltaPx = trackX - editState.startMouseX;
        const deltaSec = pps > 0 ? deltaPx / pps : 0;
        const orig = editState.originalSnapshot;
        const ctl  = WindowManager.timeline;
        if (!ctl) return;

        let badge = "";

        if (editState.op === "trimHead") {
            const minDur = 1.0 / Math.max(1.0, frameRate);
            const newSourceIn = Math.max(0, orig.sourceIn + deltaSec);
            const newStartTime = Math.max(
                0, Math.min(orig.startTime + (newSourceIn - orig.sourceIn),
                            orig.startTime + orig.duration - minDur));
            const actualSourceIn =
                orig.sourceIn + (newStartTime - orig.startTime);
            ctl.previewTrimHead(editState.trackId, editState.clipId,
                                  newStartTime, actualSourceIn);
            badge = qsTr("head ") + tcLabel(actualSourceIn);
        } else if (editState.op === "trimTail") {
            const minDur = 1.0 / Math.max(1.0, frameRate);
            const newSourceOut =
                Math.max(orig.sourceIn + minDur,
                         orig.sourceOut + deltaSec);
            ctl.previewTrimTail(editState.trackId, editState.clipId,
                                  newSourceOut);
            badge = qsTr("tail ") + tcLabel(newSourceOut);
        } else if (editState.op === "slide") {
            const newStartTime = Math.max(0, orig.startTime + deltaSec);
            ctl.previewSlide(editState.trackId, editState.clipId,
                              newStartTime);
            badge = qsTr("start ") + tcLabel(newStartTime);
        } else if (editState.op === "slip") {
            let dIn  = orig.sourceIn  + deltaSec;
            let dOut = orig.sourceOut + deltaSec;
            if (dIn < 0) {
                dIn  = 0;
                dOut = orig.sourceOut - orig.sourceIn;
            }
            ctl.previewSlip(editState.trackId, editState.clipId,
                             dIn, dOut);
            badge = qsTr("slip ") + tcLabel(dIn);
        } else if (editState.op === "reorder") {
            // Slot-based reorder with live ripple preview. Walk the
            // "without dragged" arrangement (all other clips
            // recompacted contiguously) and locate the slot whose
            // midpoint the cursor crosses. Then apply the new
            // arrangement to every clip via previewSlide.
            const all = editState.allClipsSnapshot || [];
            const fromIdx = editState.fromIndex;
            const dragged = all[fromIdx];
            if (!dragged) return;
            // 1. Build the without-dragged list contiguously.
            const without = [];
            let cursor0 = 0;
            for (let i = 0; i < all.length; ++i) {
                if (i === fromIdx) continue;
                without.push({
                    id:        all[i].id,
                    duration:  all[i].duration,
                    startTime: cursor0,
                });
                cursor0 += all[i].duration;
            }
            // 2. Find target slot from cursor (midpoint rule).
            const cursorTime = root.xToTime(trackX);
            let targetSlot = without.length;
            for (let i = 0; i < without.length; ++i) {
                const c = without[i];
                const mid = c.startTime + (c.duration / 2);
                if (cursorTime < mid) { targetSlot = i; break; }
            }
            // 3. Build the with-dragged-back-in arrangement.
            //    Each clip's new startTime = sum of preceding
            //    durations, with dragged inserted at targetSlot.
            const newOrder = [];
            for (let i = 0; i < without.length; ++i) {
                if (i === targetSlot) newOrder.push(dragged);
                newOrder.push(without[i]);
            }
            if (targetSlot >= without.length) newOrder.push(dragged);
            // 4. Apply via previewSlide for each clip whose
            //    startTime changed. Capture the dragged clip's
            //    new startTime so the insertion bar can render
            //    at the drop location.
            let cursor1 = 0;
            let droppedAtSec = 0;
            for (let i = 0; i < newOrder.length; ++i) {
                const target = newOrder[i];
                if (target.id === editState.clipId) {
                    droppedAtSec = cursor1;
                }
                if (target.startTime !== cursor1) {
                    ctl.previewSlide(editState.trackId, target.id, cursor1);
                }
                target.startTime = cursor1;
                cursor1 += target.duration;
            }
            root.reorderDropSec = droppedAtSec;
            // 5. Stash target slot for commit. The SLOT we use for
            //    MoveClipEdit is in pre-remove numbering — so if
            //    targetSlot in the without-list equals fromIdx in
            //    the original, that's a no-op (drop in own slot);
            //    if targetSlot >= fromIdx in the without-list, the
            //    pre-remove slot is targetSlot + 1.
            const preRemoveSlot = (targetSlot < fromIdx)
                                  ? targetSlot
                                  : targetSlot + 1;
            editState.targetSlot = preRemoveSlot;
            badge = qsTr("→ slot ") + (targetSlot + 1) + "/"
                    + newOrder.length;
        }
        editState.badgeText = badge;
        editState.cursorX   = trackX;
        editState = editState;   // force re-bind
    }

    function commitCurrentEdit() {
        if (!editState || !editState.clipId) return;
        // Pure click (no drag past threshold) → set selection.
        if (editState.pending) {
            root.selectedTrackId = editState.trackId;
            root.selectedClipId  = editState.clipId;
            editState = ({});
            return;
        }
        const ctl = WindowManager.timeline;
        const orig = editState.originalSnapshot;
        const cur  = clipById(editState.trackId, editState.clipId);
        if (!ctl || !cur) {
            editState = ({});
            return;
        }
        const finalState = snapshotClip(cur);
        // Phase 3.H.4 Stage B (lean-in) — reorder restores the FULL
        // track snapshot (the live ripple preview mutated multiple
        // clips), then pushes one MoveClipEdit. Other ops touch
        // only the dragged clip.
        if (editState.op === "reorder") {
            const snap = editState.allClipsSnapshot || [];
            for (let i = 0; i < snap.length; ++i) {
                const s = snap[i];
                ctl.previewSlide(editState.trackId, s.id, s.startTime);
            }
            const fromIdx = editState.fromIndex;
            const target  = editState.targetSlot;
            if (target !== fromIdx && target !== fromIdx + 1) {
                ctl.moveClipToIndex(
                    editState.trackId, editState.clipId, target);
            }
            // Phase 3.H.4 — clear the ghost.
            root.reorderActive  = false;
            root.reorderTrackId = "";
            root.reorderGhostW  = 0;
            root.reorderDropSec = 0;
            editState = ({});
            return;
        }
        ctl.restoreClipFromMap(editState.trackId, editState.clipId, orig);
        if (editState.op === "trimHead") {
            ctl.trimClipHead(editState.trackId, editState.clipId,
                               finalState.startTime, finalState.sourceIn);
        } else if (editState.op === "trimTail") {
            ctl.trimClipTail(editState.trackId, editState.clipId,
                               finalState.sourceOut);
        } else if (editState.op === "slide") {
            ctl.slideClip(editState.trackId, editState.clipId,
                           finalState.startTime);
        } else if (editState.op === "slip") {
            ctl.slipClip(editState.trackId, editState.clipId,
                          finalState.sourceIn, finalState.sourceOut);
        }
        editState = ({});
    }

    // EditToggle — flat per-track edit-mode toggle. Lives in
    // the trackHeaderGutter on the left of the timeline; the
    // pencil icon goes accent-blue when active. Squared corners,
    // no fill, just a hover affordance + tooltip.
    component EditToggle: Item {
        id: editToggle
        property bool editing: false
        signal toggled

        // Flat squared fill — no radius. Caller sizes the Item to
        // the full row (gutter width × kRowH) so the toggle butts
        // up flush against the track area's left edge with no
        // margin and aligns with the track row's top/bottom edges.
        Rectangle {
            anchors.fill: parent
            radius: 0
            color: editToggle.editing
                   ? (toggleMa.containsMouse ? Theme.accentHover : Theme.accent)
                   : (toggleMa.containsMouse ? Theme.surfaceHover : "transparent")
        }
        Icon {
            anchors.centerIn: parent
            name: "pencil-simple"
            size: Theme.iconSizeToolbar
            color: Theme.textBright
        }
        MouseArea {
            id: toggleMa
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            hoverEnabled: true
            onClicked: editToggle.toggled()
            FlatToolTip {
                visible: toggleMa.containsMouse
                text: editToggle.editing
                      ? qsTr("Edit mode ON — click to disable")
                      : qsTr("Edit this track")
            }
        }
    }

    // EditTrackMa — the gesture catcher that survives Repeater
    // rebuilds. One per editable track row.
    component EditTrackMa: MouseArea {
        property string trackId: ""
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton
        propagateComposedEvents: false
        // Must sit above playlistDropArea (z:24) — when the
        // DropArea is enabled in playlist mode it shadows mouse
        // presses below it, so click-to-select on a clip never
        // reaches the gesture catcher (the bug that didn't repro
        // in dual because the DropArea is disabled there). 27
        // also clears the DnD ghost (z:25) and insertion bar (z:26).
        z: 27
        cursorShape: {
            if (!containsMouse) return Qt.ArrowCursor;
            const z = root.editHoverZone;
            if (z === "trimHead" || z === "trimTail") return Qt.SizeHorCursor;
            if (pressed || z === "slide" || z === "slip")
                return Qt.ClosedHandCursor;
            return Qt.OpenHandCursor;
        }
        onPositionChanged: (m) => {
            // pickEditOp compares against timeToX-derived clip
            // positions in absolute content space, so we feed it
            // content-x (= viewport-x + scrollX). beginEdit /
            // updateEditAtTrackX keep receiving viewport-x because
            // editState.cursorX feeds the badge (rendered in
            // viewport coords) and the delta calc inside
            // updateEditAtTrackX is coord-system invariant.
            if (pressed) {
                root.updateEditAtTrackX(m.x);
                return;
            }
            // Hover state: pick the zone under the cursor so the
            // ClipDelegate's handle highlights light up.
            const pick = root.pickEditOp(trackId,
                                          m.x + root.scrollX,
                                          m.modifiers);
            if (pick) {
                root.editHoverTrack = trackId;
                root.editHoverClipId = pick.clip.id;
                root.editHoverZone   = pick.op;
            } else {
                root.editHoverTrack  = "";
                root.editHoverClipId = "";
                root.editHoverZone   = "";
            }
        }
        onExited: {
            root.editHoverTrack  = "";
            root.editHoverClipId = "";
            root.editHoverZone   = "";
        }
        onPressed: (m) => {
            const pick = root.pickEditOp(trackId,
                                          m.x + root.scrollX,
                                          m.modifiers);
            if (!pick) {
                m.accepted = false;
                return;
            }
            root.beginEdit(trackId, pick.clip.id, pick.op, m.x);
        }
        onReleased: (m) => root.commitCurrentEdit()
        onCanceled: root.commitCurrentEdit()
    }

    // Esc — cancel an in-flight edit gesture (Stage C) OR clear
    // the current selection (Stage D). Edit cancel takes
    // precedence; selection-clear runs only when nothing is
    // mid-drag.
    Shortcut {
        sequence: "Escape"
        enabled: root.editableMode
                 && (root.editActive || root.editPending
                     || root.selectedClipId.length > 0)
        onActivated: {
            if (root.editActive || root.editPending) {
                if (!editState) return;
                const ctl = WindowManager.timeline;
                // Reorder mutates many clips during preview; restore
                // each from the per-clip snapshot taken on press.
                if (ctl && editState.op === "reorder"
                    && editState.allClipsSnapshot) {
                    const snap = editState.allClipsSnapshot;
                    for (let i = 0; i < snap.length; ++i) {
                        ctl.previewSlide(editState.trackId,
                                         snap[i].id,
                                         snap[i].startTime);
                    }
                    root.reorderActive  = false;
                    root.reorderTrackId = "";
                    root.reorderGhostW  = 0;
                    root.reorderDropSec = 0;
                } else if (ctl && editState.originalSnapshot) {
                    ctl.restoreClipFromMap(editState.trackId,
                                              editState.clipId,
                                              editState.originalSnapshot);
                }
                editState = ({});
            } else {
                root.clearSelection();
            }
        }
    }
    // Cmd+Z / Cmd+Shift+Z — undo / redo. Only when in dual mode
    // with a wired controller.
    Shortcut {
        sequence: StandardKey.Undo
        enabled: root.editableMode && WindowManager.timeline
        onActivated: WindowManager.timeline.undoEdit()
    }
    Shortcut {
        sequence: StandardKey.Redo
        enabled: root.editableMode && WindowManager.timeline
        onActivated: WindowManager.timeline.redoEdit()
    }
    // Cmd+K — cut at the playhead on whichever track is currently
    // in edit mode. If both A and B are in edit mode, cut both;
    // if neither, no-op.
    Shortcut {
        sequence: "Ctrl+K"
        enabled: root.editableMode && WindowManager.timeline
                 && (root.editingA || root.editingB)
        onActivated: {
            const ctl = WindowManager.timeline;
            if (!ctl || !timer) return;
            const atSec = timer.position;
            if (root.editingA) ctl.splitClipAt("A", atSec);
            if (root.editingB) ctl.splitClipAt("B", atSec);
        }
    }
    // Delete / Backspace — delete the selected clip. Only fires
    // when the selected clip's track is in edit mode (prevents
    // accidental deletion when read-only browsing).
    // ApplicationShortcut so the player surface holding focus on
    // Windows doesn't swallow the keystroke (same fix LeftRail's
    // delete shortcut needs). The track-edit gates above already
    // resolve the LeftRail-vs-Timeline ownership conflict; promoting
    // to app context doesn't change that resolution.
    Shortcut {
        sequence: "Delete"
        context: Qt.ApplicationShortcut
        enabled: root.editableMode && WindowManager.timeline
                 && root.selectedClipId.length > 0
                 && ((root.selectedTrackId === "A" && root.editingA)
                     || (root.selectedTrackId === "B" && root.editingB))
        onActivated: {
            WindowManager.timeline.deleteClip(root.selectedTrackId,
                                                root.selectedClipId);
            root.clearSelection();
        }
    }
    Shortcut {
        sequence: "Backspace"
        context: Qt.ApplicationShortcut
        enabled: root.editableMode && WindowManager.timeline
                 && root.selectedClipId.length > 0
                 && ((root.selectedTrackId === "A" && root.editingA)
                     || (root.selectedTrackId === "B" && root.editingB))
        onActivated: {
            WindowManager.timeline.deleteClip(root.selectedTrackId,
                                                root.selectedClipId);
            root.clearSelection();
        }
    }

    // ---- ClipDelegate component ------------------------------------------

    component ClipDelegate: Item {
        id: clipItem
        property string trackId
        property bool   isEdited: false
        property color  accentBorder: "#446a90"
        property color  bodyColor: "#284560"
        property color  textColor: "#ffffff"
        property int    yPos: 4
        property int    rowHeight: 44
        property bool   visibleGate: true
        property var    clipModel       // QVariantMap from track.clips[i]

        readonly property double clipStart: clipModel ? (clipModel.startTime || 0) : 0
        readonly property double clipDur:   clipModel ? (clipModel.duration  || 0) : 0
        readonly property string clipId:    clipModel ? (clipModel.id || "") : ""

        readonly property bool isBeingEdited:
            editState && editState.active === true
            && editState.trackId === trackId
            && editState.clipId === clipId

        visible: visibleGate && loaded && clipModel && !clipModel.isGap
        x: 1 + timeToX(clipStart) - root.scrollX
        y: yPos
        width: Math.max(2, timeToX(clipDur) - 2)
        height: rowHeight - (yPos === 4 ? 8 : 6)
        // Phase 7.8 — when this clip's track is in edit mode, raise
        // it above scrubArea (z=0, declared later in trackArea so it
        // would otherwise win by declaration order). Without this
        // bump, clicks on edit handles fall through to scrub-to-seek.
        // Selected clips also pop above so the click-on-body select
        // gesture wins over scrubArea even when handles aren't shown.
        z: clipItem.isEdited
            ? 20
            : (clipItem.isSelected ? 18 : 0)

        // Clip body — fills clipItem entirely. Visible whether or
        // not we're editing.
        readonly property bool isSelected:
            root.selectedTrackId === clipItem.trackId
            && root.selectedClipId === clipItem.clipId
        Rectangle {
            anchors.fill: parent
            // Squared corners — matches the flat aesthetic of the
            // toolstrip + tooltip + sliders. Clips read as crisp
            // blocks rather than rounded chips.
            radius: 0
            // Edited clips brighten their fill slightly so the
            // active drag target stands out without needing a
            // chunky border. The currently-selected clip in edit
            // mode brightens much more, so it's distinguishable
            // from its same-track neighbours at a glance.
            color: clipItem.isEdited
                   ? Qt.lighter(clipItem.bodyColor,
                                clipItem.isSelected ? 1.5 : 1.18)
                   : clipItem.bodyColor
            // Border: warn-amber (matches the edit-mode handles)
            // for unselected edit-mode clips; bright white for the
            // selected one so it pops from the amber row; per-clip
            // accent colour otherwise.
            border.color:
                clipItem.isEdited
                ? (clipItem.isSelected ? Theme.textBright : Theme.warn)
                : clipItem.accentBorder
            border.width: 1
            // Selected clip → 2-px accent left rule (no full
            // border swap — the MinRender "primary row" pattern).
            // Distinguishes the focused clip from any others
            // visible in the same track without making the whole
            // outline scream.
            Rectangle {
                visible: clipItem.isSelected
                anchors.left:   parent.left
                anchors.top:    parent.top
                anchors.bottom: parent.bottom
                width: 2
                color: Theme.accent
            }
            Text {
                anchors.fill: parent
                anchors.leftMargin: clipItem.isSelected ? 8 : 6
                anchors.rightMargin: 6
                verticalAlignment: Text.AlignVCenter
                text: clipItem.clipModel ? (clipItem.clipModel.name || "") : ""
                color: clipItem.textColor
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontSizeSmall
                elide: Text.ElideMiddle
            }
        }

        // ClipDelegate is pure visual now. The mouse gesture
        // handler lives at the panel level (per-track EditTrackMa
        // below) — Repeater destroys + recreates these delegates
        // on every timelineChanged emission, which would otherwise
        // lose the press grab mid-drag and leave editState stuck.
        // Hover-only edge highlights piggy-back on the panel's
        // editHover state for visual continuity.
        Rectangle {
            id: handleL
            visible: clipItem.isEdited
            x: 0
            y: 0
            width: kHandleW
            height: parent.height
            color: (root.editHoverTrack === clipItem.trackId
                     && root.editHoverClipId === clipItem.clipId
                     && root.editHoverZone === "trimHead")
                   ? Qt.rgba(1.0, 0.78, 0.39, 0.55)
                   : "transparent"
            border.color: clipItem.isEdited ? kHandleHoverColor : "transparent"
            border.width: 1
            radius: 2
            z: 1
        }
        Rectangle {
            id: handleR
            visible: clipItem.isEdited
            x: parent.width - kHandleW
            y: 0
            width: kHandleW
            height: parent.height
            color: (root.editHoverTrack === clipItem.trackId
                     && root.editHoverClipId === clipItem.clipId
                     && root.editHoverZone === "trimTail")
                   ? Qt.rgba(1.0, 0.78, 0.39, 0.55)
                   : "transparent"
            border.color: clipItem.isEdited ? kHandleHoverColor : "transparent"
            border.width: 1
            radius: 2
            z: 1
        }
    }

    // Per-row heights. Both tracks at the original 44 px so single
    // and dual sources read at the same density. Dual = 88 px stack.
    readonly property int kRowHA: 44
    readonly property int kRowHB: 44
    // Track-header gutter on the left of the timeline. Hosts
    // edit-mode toggles per track. Width comes from Theme so
    // the TransportBar can match it for visual alignment.
    readonly property int kGutterW: Theme.gutterWidth
    // Overview / zoom constants. The overview strip lives below the
    // track row and lets the user pan + zoom long content (~90-min
    // movies, long playlists) where fit-to-width makes clips 1-px
    // wide. Mirrors the old ImGui app's overview-with-viewport-rect
    // pattern (panel_timeline.cpp:1300-1500).
    readonly property int    kOverviewH:    14
    readonly property double kPpsMin:        0.5
    readonly property double kPpsMax:      200.0
    readonly property int    kEdgeZone:      6   // overview-edge hit width
    readonly property int    kMinVpW:       12   // min viewport-rect width
    readonly property int    kFollowMargin: 20   // playhead margin during playback

    // Total panel height for outer splitter sizing. Ruler (20) +
    // trackArea + overview (kOverviewH) + borders ≈ 4 px. Exposed
    // so Main.qml grows / shrinks the timeline strip dynamically.
    readonly property int desiredHeight:
        20 + (hasTrackB ? (kRowHA + kRowHB) : kRowHA) + kOverviewH + 4

    // Pixels-per-second derived from the user's zoom level. -1 means
    // "fit-to-width" — fall back to trackWidth / duration so short
    // content always fills the row. Otherwise pps is the explicit
    // zoom level the user set via the overview bar.
    property double zoomLevel: -1
    property double scrollX:   0
    readonly property double trackWidth: trackArea.width
    readonly property double pps: {
        if (!loaded || trackWidth <= 0 || duration <= 0) return 0.0;
        if (zoomLevel < 0) return trackWidth / duration;
        return zoomLevel;
    }
    readonly property double maxScrollX:
        Math.max(0, duration * pps - trackWidth)

    // Clamp scrollX whenever pps or trackWidth change (window
    // resize, dual-mode toggle, media change, fit-to-width recompute).
    onPpsChanged: scrollX = Math.max(0, Math.min(scrollX, maxScrollX))

    // Window resize handling: when the user has zoomed in (zoomLevel
    // > 0), rescale pps + scrollX proportionally to the new track
    // width so the visible time range stays constant. Without this
    // a resize would EXPAND the timeline (more time visible at the
    // same pps) which feels wrong — the user picked a specific zoom
    // level and resizing the window shouldn't change what's on
    // screen. At fit-to-width (zoomLevel < 0) the recompute is the
    // intended behavior — pps tracks trackWidth/duration so the
    // whole timeline always fits.
    property double _lastTrackWidth: 0
    onTrackWidthChanged: {
        if (_lastTrackWidth > 0 && trackWidth > 0
            && zoomLevel > 0 && _lastTrackWidth !== trackWidth) {
            const ratio = trackWidth / _lastTrackWidth;
            zoomLevel = zoomLevel * ratio;
            scrollX   = scrollX * ratio;
        }
        _lastTrackWidth = trackWidth;
        scrollX = Math.max(0, Math.min(scrollX, maxScrollX));
    }
    // New media → reset to fit-to-width. duration is reactive on
    // controller's timeline change.
    onDurationChanged: { zoomLevel = -1; scrollX = 0; }

    // timeToX returns ABSOLUTE content-pixel position (i.e., the x
    // inside scrollContent, NOT viewport space). Children of
    // scrollContent use this directly; the wrapper's `x: -scrollX`
    // shifts everything visually.
    function timeToX(seconds) { return seconds * pps; }
    // xToTime converts a VIEWPORT-space mouse x to time. Centralizes
    // the scroll offset — every site that takes mouse.x and wants
    // seconds keeps working unchanged.
    function xToTime(x)       { return pps > 0 ? (x + scrollX) / pps : 0.0; }

    function formatTimecode(seconds) {
        // Guide 02's ruler uses adaptive timecode; for the 7.6.a
        // scaffold we render M:SS at coarse zooms and SS.f at
        // dense zooms. The pps threshold (~30 px/s) keeps labels
        // readable at typical clip widths.
        if (pps > 30.0) {
            return seconds.toFixed(1) + "s";
        }
        const m = Math.floor(seconds / 60);
        const s = Math.floor(seconds - m * 60);
        return m + ":" + (s < 10 ? "0" : "") + s;
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

        // ---- Ruler row ------------------------------------------------------

        Rectangle {
            id: rulerRow
            Layout.fillWidth: true
            Layout.preferredHeight: 20
            color: "#1c1c1c"
            clip: true

            // Tick spacing — adaptive. Pick the largest interval
            // (in seconds) whose pixel spacing is at least ~80 px,
            // so labels don't overlap. Defaults at 1 / 5 / 10 /
            // 30 / 60 seconds; refine in 7.6.b.
            readonly property double tickSeconds: {
                if (pps <= 0) return 0;
                const candidates = [1, 5, 10, 30, 60, 300];
                for (let i = 0; i < candidates.length; ++i) {
                    if (candidates[i] * pps >= 80.0) return candidates[i];
                }
                return 600;
            }

            // Scroll-content wrapper for ruler ticks. timeToX()
            // returns absolute content-pixel positions; the wrapper's
            // x = -scrollX shifts ticks visually with the track area.
            // Stage 2 may clamp the Repeater model to visible-range
            // ticks for very long content + extreme zoom.
            Item {
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                x: -root.scrollX
                width: Math.max(parent.width, duration * pps)

                Repeater {
                    model: rulerRow.tickSeconds > 0 && duration > 0
                           ? Math.ceil(duration / rulerRow.tickSeconds) + 1 : 0
                    Item {
                        width: 0
                        height: 20
                        x: timeToX(index * rulerRow.tickSeconds)
                        Rectangle {
                            x: -0.5; y: 14
                            width: 1; height: 6
                            color: Theme.textMuted
                        }
                        Text {
                            x: 4; y: 2
                            text: formatTimecode(index * rulerRow.tickSeconds)
                            color: Theme.textSecondary
                            font.family: Theme.monoFamily
                            font.pixelSize: Theme.fontSizeTiny
                        }
                    }
                }
            }
            // Single-edge bottom divider rather than a 4-sided
            // border to keep the ruler/track seam at 1 px.
            Rectangle {
                anchors.left:   parent.left
                anchors.right:  parent.right
                anchors.bottom: parent.bottom
                height: 1
                color: Theme.divider
            }

            // Ruler playhead — always visible when loaded so the
            // (now-interactive) ticker row carries a position marker
            // in every mode. In playlist / dual edit mode it stands
            // alone; in single + non-edit playlist/dual it pairs with
            // the track playhead below to draw a continuous line.
            Rectangle {
                id: rulerPlayhead
                visible: loaded
                width: 1
                height: parent.height
                color: Theme.success
                z: 10
                x: scrubArea.pressed
                   ? Math.max(0, Math.min(parent.width - 1, scrubArea.mouseX))
                   : (rulerScrubArea.pressed
                      ? Math.max(0, Math.min(parent.width - 1, rulerScrubArea.mouseX))
                      : (timeToX(position) - root.scrollX))
            }

            // Scrub catcher on the ruler. Active in every mode so the
            // ticker row is always seek-able; no hover-thumbnail logic
            // here — the trackArea scrubArea owns that and the ruler
            // is too narrow for the corner overlay to feel useful.
            MouseArea {
                id: rulerScrubArea
                anchors.fill: parent
                hoverEnabled: false
                cursorShape: loaded ? Qt.PointingHandCursor
                                    : Qt.ArrowCursor
                z: 5    // below the rulerPlayhead (z:10) but above ticks
                onPressed: (m) => scrubArea.doPress(m.x)
                onPositionChanged: (m) => {
                    if (!pressed) return;
                    scrubArea.doMove(m.x);
                }
                onReleased: (m) => scrubArea.doRelease(m.x)
            }
        }

        // ---- Track row ------------------------------------------------------

        Rectangle {
            id: trackArea
            Layout.fillWidth: true
            // Heights stack — single source = one row, dual source
            // = two stacked rows. Total height drives both the
            // overlay (in/out, playhead, scrub) and the clipRects'
            // y-positions inside.
            Layout.preferredHeight: hasTrackB ? (kRowHA + kRowHB) : kRowHA
            Behavior on Layout.preferredHeight {
                NumberAnimation { duration: 120; easing.type: Easing.OutQuad }
            }
            color: Theme.bg
            clip: true

            // Mouse-wheel zoom + Ctrl+wheel pan. MouseArea (rather
            // than WheelHandler) — Qt's WheelHandler refused to fire
            // through the sibling MouseAreas (scrubArea / DropArea /
            // EditTrackMa) covering trackArea. Accept LeftButton so
            // Qt actually delivers pointer events to this MouseArea
            // (NoButton makes the MA inert for *all* pointer events
            // including wheel), then explicitly reject the press so
            // the click falls through to scrubArea underneath.
            MouseArea {
                id: timelineWheelMa
                anchors.fill: parent
                // hoverEnabled: false — otherwise this MA at z:100
                // captures hover events that scrubArea below needs
                // for the timeline-hover-thumbnail throttle. Wheel
                // events fire independently of hover tracking.
                hoverEnabled: false
                propagateComposedEvents: true
                z: 100
                onPressed: (mouse) => { mouse.accepted = false; }
                onWheel: (wheel) => {
                    root.applyWheel(wheel.x,
                                    wheel.angleDelta.y,
                                    (wheel.modifiers
                                     & Qt.AltModifier) !== 0);
                    wheel.accepted = true;
                }
            }

            // Empty-state hint until media loads. In playlist mode an
            // empty playlist needs different copy so the user knows
            // the timeline IS active — they just have nowhere to
            // play yet.
            Text {
                anchors.centerIn: parent
                visible: !loaded
                text: root.playlistActive
                      ? qsTr("Drop media here to add to the playlist")
                      : qsTr("Open a video to enable the timeline.")
                color: "#555555"
                font.pixelSize: 11
                font.italic: true
            }

            // Track A — primary source. Phase 7.8 Stage B: render
            // clips via Repeater so a multi-clip track (post-cut /
            // -split) shows all of its segments. Stage C: each
            // clip becomes edit-aware when track A is in edit mode.
            Repeater {
                model: trackAData && trackAData.clips ? trackAData.clips : []
                delegate: ClipDelegate {
                    trackId: "A"
                    isEdited: editingA
                    accentBorder: "#446a90"
                    bodyColor: "#284560"
                    textColor: "#cce0ff"
                    yPos: 4
                    rowHeight: kRowHA
                    clipModel: modelData
                }
            }

            // Track separator — thin line between A and B rows in
            // the dual case.
            Rectangle {
                visible: hasTrackB
                x: 0
                y: kRowHA
                width: parent.width
                height: 1
                color: "#222222"
            }

            // Track B — secondary source. Stage C: same edit-aware
            // delegate as A.
            Repeater {
                model: trackBData && trackBData.clips && hasTrackB
                       ? trackBData.clips : []
                delegate: ClipDelegate {
                    trackId: "B"
                    isEdited: editingB
                    accentBorder: "#a0664a"
                    bodyColor: "#5a3a28"
                    textColor: "#ffd9bf"
                    yPos: kRowHA + 3
                    rowHeight: kRowHB
                    visibleGate: loaded && hasTrackB
                    clipModel: modelData
                }
            }

            // ---- Phase 3.H.4 Stage B — reorder drag ghost ----
            // A translucent rectangle of the dragged clip's
            // dimensions that follows the cursor while reordering.
            // The underlying ClipDelegate has already snapped to
            // its target slot via live ripple preview; the ghost
            // shows "this is the clip I'm dragging" so the user
            // can see what they're holding.
            Rectangle {
                id: reorderGhost
                visible: root.reorderActive
                z: 30
                color: Theme.accent
                opacity: 0.55
                radius: 3
                border.color: "#aaccff"
                border.width: 1
                width: Math.max(40, root.reorderGhostW || 40)
                height: kRowHA - 8
                x: Math.max(0,
                       Math.min(trackArea.width - width,
                                (root.reorderCursorX || 0) - width / 2))
                y: root.reorderTrackId === "B" ? (kRowHA + 4) : 4
                Text {
                    anchors.centerIn: parent
                    text: qsTr("⇄")
                    color: "#ffffff"
                    font.pixelSize: 14
                    font.bold: true
                }
            }
            // Insertion indicator — vertical bar at the dragged
            // clip's drop location (its post-arrangement left edge).
            // Helps the user see EXACTLY where the clip will land
            // since the cursor-following ghost is centered on the
            // cursor and may not visually match the drop slot.
            Rectangle {
                id: reorderInsertionBar
                visible: root.reorderActive
                z: 31
                color: "#ffd54a"
                width: 3
                height: kRowHA
                x: root.timeToX(root.reorderDropSec || 0) - root.scrollX - width / 2
                y: root.reorderTrackId === "B" ? kRowHA : 0
                // Small triangle "drop arrow" pointing down at the bar.
                Rectangle {
                    width: 9
                    height: 9
                    color: "#ffd54a"
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.bottom: parent.top
                    anchors.bottomMargin: -2
                    rotation: 45
                }
            }

            // ---- Phase 3.H.4 Stage A — drop-to-add visuals ----
            // Dashed ghost outline at the proposed insertion point.
            // 5-second placeholder width (drag-time we don't yet
            // know the dragged file's duration).
            Rectangle {
                id: dropGhost
                visible: root.dropActive
                color: "transparent"
                border.color: Theme.accent
                border.width: 2
                radius: 2
                z: 25
                readonly property real placeholderSec: 5.0
                readonly property real ghostX:
                    root.dropActive ? (root.dropState.snapEdgeX || 0) : 0
                readonly property real ghostW:
                    Math.max(20, dropGhost.placeholderSec * pps)
                // Subtract scrollX to convert content-x → viewport-x.
                // Clamp to trackArea bounds so the ghost stays visible
                // even if the snap edge is outside the visible window.
                x: Math.max(0, Math.min(trackArea.width - ghostW,
                                          ghostX - root.scrollX))
                y: 4
                width: ghostW
                height: kRowHA - 8
                opacity: 0.55
                Rectangle {
                    anchors.fill: parent
                    anchors.margins: 2
                    color: Theme.accent
                    opacity: 0.10
                    radius: 1
                }
            }
            // Insertion bar — vertical accent line at the snap
            // edge. Only renders when we've snapped (snapIndex>=0);
            // when dragging in free space, the ghost outline alone
            // is enough.
            Rectangle {
                id: dropInsertionBar
                visible: root.dropActive
                         && root.dropState.snapIndex !== undefined
                         && root.dropState.snapIndex >= 0
                color: Theme.accent
                width: 2
                height: kRowHA
                z: 26
                x: root.dropActive
                   ? ((root.dropState.snapEdgeX || 0) - root.scrollX)
                   : 0
                y: 0
            }
            // The DropArea itself. Covers the whole trackArea and
            // accepts text/uri-list (Finder + bin sources). Computes
            // the snap point on every onPositionChanged tick and
            // updates dropState; on drop, addMediaFile per URL +
            // insertClipAtIndex on the active playlist.
            DropArea {
                id: playlistDropArea
                anchors.fill: parent
                enabled: root.playlistActive
                z: 24
                // No `keys:` filter — Qt's keys list filters by the
                // drag's internal `Drag.keys` array, NOT mime types.
                // Both Finder drops and the bin row's drag source
                // leave `Drag.keys` empty, so a keys filter would
                // reject every drag. Runtime `drag.hasUrls` is the
                // discriminator.

                // Snap helpers — duplicate the C++ snap logic so we
                // can drive the visuals on every drag tick without
                // a round-trip. Tolerance is screen-space px → time.
                readonly property real snapTolPx: 12

                function recompute(x) {
                    if (!ctl || pps <= 0) {
                        root.dropState = ({});
                        return;
                    }
                    const trackId = "A";
                    const tSec = root.xToTime(x);
                    const tolSec = playlistDropArea.snapTolPx / pps;
                    const idx = ctl.snapEdgeIndexAtTime(trackId, tSec, tolSec);
                    let edgeX = x;
                    let snappedIdx = -1;
                    if (idx >= 0 && trackAData && trackAData.clips) {
                        const clips = trackAData.clips;
                        if (idx === 0) {
                            edgeX = root.timeToX(0);
                        } else if (idx >= clips.length) {
                            edgeX = root.timeToX(duration);
                        } else {
                            edgeX = root.timeToX(clips[idx].startTime || 0);
                        }
                        snappedIdx = idx;
                    }
                    root.dropState = {
                        active: true,
                        cursorX: x,
                        snapIndex: snappedIdx,
                        snapEdgeX: edgeX,
                    };
                }

                onEntered: (drag) => {
                    if (!drag.hasUrls) {
                        drag.accepted = false;
                        return;
                    }
                    drag.accepted = true;
                    recompute(drag.x);
                }
                onPositionChanged: (drag) => {
                    if (!drag.hasUrls) return;
                    recompute(drag.x);
                }
                onExited: {
                    root.dropState = ({});
                }
                onDropped: (drop) => {
                    const dropX = drop.x;
                    root.dropState = ({});
                    if (!drop.hasUrls
                        || !ctl
                        || !WindowManager.project) {
                        return;
                    }

                    // Resolve drop time + snap edge → starting
                    // insert index. Subsequent files insert one
                    // after another at successive indices.
                    const trackId = "A";
                    const tSec = root.xToTime(dropX);
                    const tolSec = playlistDropArea.snapTolPx / pps;
                    let idx = ctl.snapEdgeIndexAtTime(trackId, tSec, tolSec);
                    if (idx < 0) {
                        // No snap — append to end.
                        idx = trackAData && trackAData.clips
                              ? trackAData.clips.length : 0;
                    }

                    for (let i = 0; i < drop.urls.length; ++i) {
                        // Centralized URL→OS-path on WindowManager.
                        // Handles file:///C:/… (Win), file:///path
                        // (POSIX), and the file://server/… UNC form.
                        const path = WindowManager.urlToOsPath(drop.urls[i]);
                        if (!path) continue;
                        const mediaId = WindowManager.project.addMediaFile(path);
                        if (!mediaId) continue;

                        // Pull duration / fps / kind from the resolved
                        // pool entry. Freshly-imported files may have
                        // 0 duration until metadata extraction lands;
                        // fall back to a 5s placeholder.
                        const m = WindowManager.project.mediaItemMap(mediaId);
                        // Reject audio in playlists for v2.0 — the
                        // playlist orchestrator runs all clips through
                        // VideoDecoder and has no audio-only path.
                        // The audio item still lands in the Audio bin
                        // (addMediaFile completed); just don't put it
                        // on the timeline.
                        if ((m.type || 0) === 1) {
                            console.warn("playlist drop: skipping audio media:",
                                         path);
                            continue;
                        }
                        const isSeq = (m.type || 0) === 3;
                        let dur = m.duration || 0;
                        if (isSeq && m.imageSeq && m.imageSeq.duration) {
                            dur = m.imageSeq.duration;
                        }
                        if (!(dur > 0)) dur = 5.0;
                        const srcFps = isSeq && m.imageSeq
                                       ? m.imageSeq.frameRate
                                       : (m.video ? m.video.frameRate : 0);

                        const spec = {
                            "name":           m.name || path,
                            "mediaItemId":    mediaId,
                            "mediaPath":      m.path || path,
                            "mediaKind":      isSeq ? 1 : ((m.type || 0) === 1 ? 2 : 0),
                            "sourceFps":      srcFps > 0 ? srcFps : frameRate,
                            "sourceDuration": dur,
                            "sourceIn":       0.0,
                            "sourceOut":      dur,
                            "hasAudio":       !!m.hasAudio,
                        };
                        if (isSeq && m.imageSeq) {
                            spec.sequencePattern    = m.imageSeq.pattern || "";
                            spec.sequenceStartFrame = m.imageSeq.startFrame || 1;
                            spec.sequenceEndFrame   = m.imageSeq.endFrame   || 1;
                        }
                        ctl.insertClipAtIndex(trackId, idx, spec);
                        idx += 1;
                    }
                    drop.accept();
                    root.playlistContentDropped();
                }

                // Tint the trackArea border while dragging over.
                Rectangle {
                    anchors.fill: parent
                    visible: parent.containsDrag
                    color: "transparent"
                    border.color: Theme.accent
                    border.width: 2
                    radius: 2
                    z: 23
                }
            }

            // Image-sequence cache indicator — a thin strip along the
            // bottom of the track row showing the ring-buffer fill
            // relative to the playhead. Visible only when the active
            // clip is an image sequence; bound to WindowManager's
            // imageSeqBuffered* properties (kept fresh by the cache
            // thread's tick + the timer pump). Behind/ahead are drawn
            // on either side of the playhead so users can see the
            // prefetch direction at a glance.
            Item {
                id: cacheIndicator
                // Reference the controller property directly rather
                // than scrubArea (defined later in this file) — QML
                // resolves IDs lazily, but the explicit binding is
                // more legible.
                visible: loaded && ctl && ctl.currentMediaKind === 1
                // x: -scrollX + width: full timeline length so all
                // band properties (centerX, behindMainX, etc.) can
                // remain in absolute content-pixel space; the offset
                // shifts them visually with the rest of the timeline.
                x: -root.scrollX
                width: Math.max(parent.width, duration * pps)
                height: 3
                // Anchor to track A's bottom edge specifically, so
                // the strip stays where it was (against the A clip)
                // when the dual-source view extends the panel
                // downward with track B.
                y: kRowHA - 3 - 1
                z: 1                       // above the clip rect, below the playhead

                // Convert "frames" to pixels using the timeline's
                // pps. ahead/behind extents only mean something
                // relative to the current playhead, so center the
                // bar at timeToX(position). When wrap-around caching
                // is active, a band can extend past the clip rect's
                // edge — we split the overflow back to the opposite
                // edge so the user sees the wrapped portion.
                readonly property int  fps2: Math.max(1, Math.round(frameRate))
                readonly property int  fc:   WindowManager.imageSeqFrameCount
                // Use coverage (max cached-frame distance) rather
                // than count: at stride > 1 the count under-reports
                // the actual span (every Nth frame); coverage gives
                // the true buffered band even with sparse density.
                readonly property int  ahCov: WindowManager.imageSeqBufferedAheadCoverage
                readonly property int  bhCov: WindowManager.imageSeqBufferedBehindCoverage
                readonly property real frameWidth:
                    fps2 > 0 && pps > 0 ? pps / fps2 : 0
                readonly property real centerX: timeToX(position)
                readonly property real aheadW:
                    Math.max(0, ahCov * frameWidth)
                readonly property real behindW:
                    Math.max(0, bhCov * frameWidth)

                // Track-row paintable bounds — usually the clipRect
                // (x=1..trackWidth-1) but when the cache has a loop
                // range set, the strip wraps at the in/out markers
                // instead. With a range, the cache only fills inside
                // [in, out], so wrap-around bands should land back
                // at `in` rather than across the whole timeline.
                // Playlist mode forces no-range (in/out isn't
                // functional there; the strip should span the full
                // playlist width even if stale in/out state persists).
                readonly property bool hasRange:
                    WindowManager.hasInOutRange
                    && !(WindowManager.timeline
                         && WindowManager.timeline.sourceMode === 1)
                readonly property real inEdgeX: {
                    if (!hasRange) return 1;
                    if (fps2 <= 0) return 1;
                    return timeToX(WindowManager.inPoint / fps2);
                }
                readonly property real outEdgeX: {
                    // No-range fallback: full content width (in
                    // scrollContent coords). Was trackWidth - 1 when
                    // the timeline was always fit-to-width.
                    const fullW = duration * pps;
                    if (!hasRange) return Math.max(0, fullW - 1);
                    if (fps2 <= 0) return Math.max(0, fullW - 1);
                    // out is inclusive frame; +1 so the right edge
                    // sits *after* the last frame to match the wrap
                    // semantics on the playback side.
                    return timeToX((WindowManager.outPoint + 1) / fps2);
                }
                readonly property real leftEdge:  inEdgeX
                readonly property real rightEdge: outEdgeX

                // Behind: from centerX leftward by behindW. If it
                // would underflow leftEdge, split the underflow to
                // the right side.
                readonly property real behindStart:
                    parent !== null ? centerX - behindW : 0
                readonly property real behindOverflow:
                    Math.max(0, leftEdge - behindStart)
                readonly property real behindMainX:
                    Math.max(leftEdge, centerX - behindW)
                readonly property real behindMainW:
                    centerX - behindMainX
                readonly property real behindWrapX:
                    rightEdge - behindOverflow
                readonly property real behindWrapW:
                    behindOverflow

                // Ahead: from centerX rightward by aheadW. Overflow
                // past rightEdge wraps to the left side.
                readonly property real aheadEnd: centerX + aheadW
                readonly property real aheadOverflow:
                    Math.max(0, aheadEnd - rightEdge)
                readonly property real aheadMainW:
                    Math.min(aheadEnd, rightEdge) - centerX
                readonly property real aheadWrapX: leftEdge
                readonly property real aheadWrapW: aheadOverflow

                // Behavior smoothing — at the tip of the band each
                // frame of playback is "current advanced by 1, but
                // I/O worker hasn't yet pushed a new frame at the
                // far edge", so coverage oscillates by ±1 frame.
                // Honest data + a short ease-out smooths the
                // perceptual ripple without misrepresenting the
                // buffer state.
                readonly property int kBandAnimMs: 90

                // Behind — main piece.
                Rectangle {
                    visible: parent.behindMainW > 0
                    x: parent.behindMainX
                    y: 0
                    width: parent.behindMainW
                    height: 3
                    color: "#3a8c3a"
                    opacity: 0.65
                    Behavior on width  { NumberAnimation { duration: cacheIndicator.kBandAnimMs; easing.type: Easing.OutQuad } }
                    Behavior on x      { NumberAnimation { duration: cacheIndicator.kBandAnimMs; easing.type: Easing.OutQuad } }
                }
                // Behind — wrapped piece on the right side.
                Rectangle {
                    visible: parent.behindWrapW > 0
                    x: parent.behindWrapX
                    y: 0
                    width: parent.behindWrapW
                    height: 3
                    color: "#3a8c3a"
                    opacity: 0.65
                    Behavior on width  { NumberAnimation { duration: cacheIndicator.kBandAnimMs; easing.type: Easing.OutQuad } }
                    Behavior on x      { NumberAnimation { duration: cacheIndicator.kBandAnimMs; easing.type: Easing.OutQuad } }
                }
                // Ahead — main piece.
                Rectangle {
                    visible: parent.aheadMainW > 0
                    x: parent.centerX
                    y: 0
                    width: parent.aheadMainW
                    height: 3
                    color: Theme.success
                    opacity: 0.85
                    Behavior on width  { NumberAnimation { duration: cacheIndicator.kBandAnimMs; easing.type: Easing.OutQuad } }
                }
                // Ahead — wrapped piece on the left side.
                Rectangle {
                    visible: parent.aheadWrapW > 0
                    x: parent.aheadWrapX
                    y: 0
                    width: parent.aheadWrapW
                    height: 3
                    color: Theme.success
                    opacity: 0.85
                    Behavior on width  { NumberAnimation { duration: cacheIndicator.kBandAnimMs; easing.type: Easing.OutQuad } }
                }

                // Failed-frame ticks — declared LAST and given an
                // explicit z so the moving green ahead/behind bands
                // never paint over them. Each tick spans the full
                // duration of one frame on the timeline (frameWidth
                // pixels) so the gap is unmistakable at any zoom.
                // At extreme zoom-out where frameWidth drops below
                // 1 px, clamp to a 1 px minimum so the tick stays
                // visible. modelData is the 0-based frame index
                // relative to the sequence's startFrame.
                Repeater {
                    model: WindowManager.imageSeqFailedFrames
                    Rectangle {
                        required property int modelData
                        x: modelData * cacheIndicator.frameWidth
                        // Same height as the cache band (3 px)
                        // stacked just above it — y=-4 leaves a
                        // 1 px gap between the warning lane and
                        // the prefetch lane so they read as two
                        // parallel rails rather than a shared
                        // strip.
                        y: -4
                        width:  Math.max(1, cacheIndicator.frameWidth)
                        height: 3
                        color:  Theme.error
                        opacity: 1.0
                        z: 5   // above the green bands' implicit z;
                               // playhead is a sibling of cacheIndicator
                               // declared later, so it still draws above
                    }
                }
            }

            // ---- In/Out markers ---------------------------------
            // Frame numbers from WindowManager; -1 = unset. The
            // timeline ruler's frame rate comes from the active
            // timer; convert to seconds before timeToX.
            // Hidden in playlist mode — in/out loop isn't functional
            // there (the orchestrator handles playlist-level wrap
            // and timer-level looping is forced off in playlist), so
            // surfacing the markers would mislead the user. Matches
            // the disabled transport In/Out buttons.
            Item {
                id: inOutMarkers
                visible: !(WindowManager.timeline
                           && WindowManager.timeline.sourceMode === 1)
                // x: -scrollX shifts the absolute-content-x marker
                // children (inX, outX from timeToX) into viewport
                // space along with the rest of the timeline.
                x: -root.scrollX
                y: 0
                width: Math.max(parent.width, duration * pps)
                height: parent.height
                z: 1   // above clip rect, below playhead
                readonly property int  inFr:  WindowManager.inPoint
                readonly property int  outFr: WindowManager.outPoint
                readonly property real fpsLocal: {
                    const t = WindowManager.timeline
                              ? WindowManager.timeline.timer : null;
                    return t ? t.frameRate : frameRate;
                }
                readonly property real inX:
                    inFr  >= 0 && fpsLocal > 0
                        ? timeToX(inFr  / fpsLocal) : -1
                readonly property real outX:
                    outFr >= 0 && fpsLocal > 0
                        ? timeToX((outFr + 1) / fpsLocal) : -1

                // Tinted band between in and out — only when both
                // are set. Faint blue overlay across the track row.
                Rectangle {
                    visible: inOutMarkers.inX >= 0 && inOutMarkers.outX >= 0
                             && inOutMarkers.outX > inOutMarkers.inX
                    x: inOutMarkers.inX
                    y: 0
                    width: Math.max(0, inOutMarkers.outX - inOutMarkers.inX)
                    height: parent.height
                    color: Theme.accentMuted
                    opacity: 0.10
                }
                // In marker: vertical line + downward triangle flag
                // at top, anchored to the In x position.
                Rectangle {
                    visible: inOutMarkers.inX >= 0
                    x: inOutMarkers.inX
                    y: 0
                    width: 1
                    height: parent.height
                    color: Theme.success
                }
                Canvas {
                    visible: inOutMarkers.inX >= 0
                    x: inOutMarkers.inX - 5
                    y: 0
                    width: 11; height: 7
                    onPaint: {
                        const ctx = getContext("2d");
                        ctx.reset();
                        ctx.fillStyle = Theme.success;
                        ctx.beginPath();
                        ctx.moveTo(0, 0);
                        ctx.lineTo(11, 0);
                        ctx.lineTo(6, 7);
                        ctx.closePath();
                        ctx.fill();
                    }
                }
                // Out marker: vertical line + flag pointing the
                // other direction (top-right corner of the wedge
                // sits at outX so the visual width is left of the
                // line).
                Rectangle {
                    visible: inOutMarkers.outX >= 0
                    x: inOutMarkers.outX - 1
                    y: 0
                    width: 1
                    height: parent.height
                    color: Theme.warn
                }
                Canvas {
                    visible: inOutMarkers.outX >= 0
                    x: inOutMarkers.outX - 6
                    y: 0
                    width: 11; height: 7
                    onPaint: {
                        const ctx = getContext("2d");
                        ctx.reset();
                        ctx.fillStyle = Theme.warn;
                        ctx.beginPath();
                        ctx.moveTo(0, 0);
                        ctx.lineTo(11, 0);
                        ctx.lineTo(5, 7);
                        ctx.closePath();
                        ctx.fill();
                    }
                }
            }

            // Playhead — a vertical line tracking timer.position.
            // Drawn on top of the clip. While scrubArea is pressed,
            // the playhead follows the cursor directly: the frame
            // catches up asynchronously through ScrubDecoder, but
            // visual feedback is instant. Without this decoupling
            // the playhead lags the cursor by one decode round-trip
            // (~16–50 ms per frame for inter-frame codecs) and feels
            // "a step removed" from the gesture. Old QCView's
            // timeline_view.cpp does the same trick — see comment
            // in scrub_decoder.cpp:279-288 about the worker's
            // latest-pending-target throttle.
            Rectangle {
                id: playhead
                // Hidden when the user is actively editing clips on a
                // track (playlist edit, or dual A/B edit) — the ruler
                // playhead alone marks position so the track row is
                // free for trim / slide / reorder gestures. In every
                // other mode the track playhead pairs with the ruler
                // playhead to draw a continuous line top-to-bottom.
                visible: loaded
                         && !(root.editableMode
                              && (root.editingA || root.editingB))
                width: 1
                height: parent.height
                color: Theme.success
                // Above cacheIndicator (z:1) and its children
                // (failed-frame ticks at z:5 inside it). Below the
                // drop ghost (z:31) which only appears during DnD.
                z: 10
                // While scrubbing, follow the cursor in viewport
                // space (mouseX is trackArea-local). Otherwise place
                // by absolute time, then offset to viewport coords.
                x: scrubArea.pressed
                   ? Math.max(0, Math.min(parent.width - 1, scrubArea.mouseX))
                   : (rulerScrubArea.pressed
                      ? Math.max(0, Math.min(parent.width - 1, rulerScrubArea.mouseX))
                      : (timeToX(position) - root.scrollX))
            }

            // Click + drag to scrub. Mirrors the old slider stub:
            // press pauses + remembers playing state, drag routes
            // through ScrubDecoder for fast frame preview, release
            // commits the final position via VideoDecoder.seekToFrame
            // and resumes playback if it was playing before. The
            // controller's timer follows VideoDecoder.currentFrame
            // automatically via the 7.2.d mirror, so the playhead
            // tracks the scrub in real time.
            MouseArea {
                id: scrubArea
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: loaded ? Qt.PointingHandCursor
                                    : Qt.ArrowCursor
                property bool wasPlayingBeforeScrub: false

                // Phase 3.H.5 — hover thumbnail. Throttle position
                // updates to ~30 Hz; QML mouse-move events can fire
                // every input tick (~125 Hz on Mac trackpads) and
                // we don't want to spam the cache with redundant
                // misses. The cache itself dedups, but we save the
                // overhead of the QML→C++ trip.
                Timer {
                    id: hoverThumbRequestThrottle
                    interval: 16
                    repeat: false
                    property real pendingX: 0
                    onTriggered: {
                        if (!loaded) return;
                        // Annotation mode: skip the request — the
                        // user is crossing the timeline to reach
                        // the notes panel, the corner-overlay
                        // preview is just visual noise here.
                        if (WindowManager.isAnnotationActive()) return;
                        // Settings: user can disable hover thumbs
                        // entirely (low-RAM systems / focus mode).
                        if (!WindowManager.timelineHoverThumbsEnabled) return;
                        const t = root.xToTime(
                            Math.max(0, Math.min(scrubArea.width, pendingX)));
                        WindowManager.requestHoverThumbnail(t);
                    }
                }
                onExited: {
                    // Stop any pending throttle fire — it would
                    // otherwise call requestHoverThumbnail() ~16 ms
                    // after we left, restoring the overlay.
                    hoverThumbRequestThrottle.stop();
                    WindowManager.clearHoverThumbnail();
                }

                function frameAtX(px) {
                    const clampedX = Math.max(0, Math.min(width, px));
                    return Math.round(xToTime(clampedX) * frameRate);
                }

                // ClipMediaKind enum values (mirrors timeline_types.h):
                //   0 = Video, 1 = ImageSequence, 2 = Audio
                // Image sequences have no FFmpeg context to seek
                // and no separate ScrubDecoder — pixels live in
                // ImageSequenceCache, the renderer pulls per
                // present, and the timer is the master clock. So
                // scrub gestures route directly to the timer for
                // image-seq sources.
                readonly property int activeMediaKind:
                    ctl ? ctl.currentMediaKind : 0
                readonly property bool isImageSequence:
                    activeMediaKind === 1
                readonly property bool isAudio:
                    activeMediaKind === 2

                readonly property bool isDual:
                    WindowManager.dualController !== null
                    && WindowManager.dualController !== undefined

                function commitFrame(frame) {
                    if (isDual) {
                        // Phase 7.7 — dual mode routes through the
                        // controller via WindowManager.seekToFrame.
                        WindowManager.seekToFrame(frame);
                    } else if (root.playlistActive) {
                        // Phase 3.H.2 — playlist routes through
                        // WindowManager so cross-clip scrubs swap
                        // the active decoder before seeking.
                        WindowManager.seekToFrame(frame);
                    } else if (isImageSequence) {
                        if (frameRate > 0)
                            timer.seek(frame / frameRate);
                    } else if (isAudio) {
                        // Audio-only: WM.seekToFrame seeks both the
                        // timer + AudioPlayer in lockstep so play-
                        // after-scrub stays in sync.
                        WindowManager.seekToFrame(frame);
                    } else if (WindowManager.videoDecoder) {
                        WindowManager.videoDecoder.seekToFrame(frame);
                    }
                }

                // Scrub state machine. Both this MouseArea (over the
                // track row) and rulerScrubArea (over the time-ticker
                // row above) call these so the user can scrub from
                // either surface — useful when the playhead lives on
                // the ruler in dual / playlist mode.
                function doPress(viewX) {
                    if (!loaded) return;
                    // Phase 3.H.5 — drag = scrubbing, not hovering.
                    // Drop the corner thumb so it doesn't sit there
                    // showing a stale frame on top of the live scrub
                    // preview.
                    hoverThumbRequestThrottle.stop();
                    WindowManager.clearHoverThumbnail();
                    const f = frameAtX(viewX);
                    if (isDual) {
                        wasPlayingBeforeScrub = WindowManager.dualController
                            && WindowManager.dualController.isPlaying;
                        WindowManager.pause();
                        commitFrame(f);
                        return;
                    }
                    if (root.playlistActive) {
                        // Phase 3.H.2 — playlist scrub: lightweight
                        // preview during drag (ScrubDecoder + clip
                        // swap); heavy commit on release.
                        wasPlayingBeforeScrub =
                            timer && timer.playing;
                        if (timer) timer.pause();
                        if (WindowManager.videoDecoder) {
                            WindowManager.videoDecoder.pause();
                        }
                        WindowManager.scrubToTimelineFrame(f);
                        return;
                    }
                    if (isImageSequence) {
                        wasPlayingBeforeScrub = timer && timer.playing;
                        if (timer) timer.pause();
                        commitFrame(f);
                        return;
                    }
                    if (isAudio) {
                        // Audio: pause via the unified path so timer
                        // + AudioPlayer stop together, then seek.
                        wasPlayingBeforeScrub = timer && timer.playing;
                        if (wasPlayingBeforeScrub) WindowManager.pause();
                        commitFrame(f);
                        return;
                    }
                    if (!WindowManager.videoDecoder) return;
                    wasPlayingBeforeScrub = WindowManager.videoDecoder.isPlaying;
                    WindowManager.videoDecoder.pause();
                    // Video: lightweight preview through ScrubDecoder;
                    // heavy VideoDecoder seek waits for onReleased.
                    if (WindowManager.scrubDecoder) {
                        WindowManager.scrubDecoder.requestFrame(f);
                    }
                    // Source B has no ScrubDecoder (Phase B follow-up)
                    // — full seekToFrame each scrub move so it tracks.
                    // Slower than A's preview but keeps the two
                    // sources visually together during the scrub.
                    if (WindowManager.videoDecoderB
                        && WindowManager.videoDecoderB.sourcePath) {
                        WindowManager.videoDecoderB.pause();
                        WindowManager.videoDecoderB.seekToFrame(f);
                    }
                }

                function doMove(viewX) {
                    if (!loaded) return;
                    const f = frameAtX(viewX);
                    if (isDual) {
                        commitFrame(f);
                        return;
                    }
                    if (root.playlistActive) {
                        WindowManager.scrubToTimelineFrame(f);
                        return;
                    }
                    if (isImageSequence) {
                        // Drive the timer directly. The driver tick
                        // picks up the new currentFrame within ~16 ms
                        // and publishes the matching ring-buffer entry.
                        commitFrame(f);
                        return;
                    }
                    if (isAudio) {
                        // Audio: same pattern as image-seq — direct
                        // timer + AudioPlayer seek through commitFrame.
                        commitFrame(f);
                        return;
                    }
                    if (WindowManager.scrubDecoder) {
                        WindowManager.scrubDecoder.requestFrame(f);
                    }
                    if (WindowManager.videoDecoderB
                        && WindowManager.videoDecoderB.sourcePath) {
                        WindowManager.videoDecoderB.seekToFrame(f);
                    }
                }

                function doRelease(viewX) {
                    if (!loaded) return;
                    const f = frameAtX(viewX);
                    if (isDual) {
                        commitFrame(f);
                        if (wasPlayingBeforeScrub) WindowManager.play();
                        return;
                    }
                    if (root.playlistActive) {
                        // Full commit: WindowManager.seekToFrame
                        // routes the cross-clip swap (if any) and
                        // seeks the active decoder to the exact
                        // source frame.
                        WindowManager.seekToFrame(f);
                        if (wasPlayingBeforeScrub
                            && WindowManager.videoDecoder) {
                            WindowManager.videoDecoder.play();
                        }
                        return;
                    }
                    if (isImageSequence) {
                        commitFrame(f);
                        if (wasPlayingBeforeScrub && timer) timer.play();
                        return;
                    }
                    if (isAudio) {
                        commitFrame(f);
                        if (wasPlayingBeforeScrub) WindowManager.play();
                        return;
                    }
                    if (!WindowManager.videoDecoder) return;
                    // Commit the final position to the streaming
                    // decoder so playback resumes from the exact
                    // frame the user dropped on (and so audio/clock
                    // re-sync to it). The playhead's pressed binding
                    // releases here, falling back to timeToX(position).
                    WindowManager.videoDecoder.seekToFrame(f);
                    if (WindowManager.videoDecoderB
                        && WindowManager.videoDecoderB.sourcePath) {
                        WindowManager.videoDecoderB.seekToFrame(f);
                    }
                    if (wasPlayingBeforeScrub) {
                        WindowManager.videoDecoder.play();
                        if (WindowManager.videoDecoderB
                            && WindowManager.videoDecoderB.sourcePath) {
                            WindowManager.videoDecoderB.play();
                        }
                    }
                }

                onPressed: (m) => doPress(m.x)

                onPositionChanged: (m) => {
                    if (!loaded) return;
                    // Phase 3.H.5 — hover-thumbnail (no press, not
                    // playing). Throttled via the timer above so we
                    // don't burn QML→C++ trips on every input tick.
                    if (!pressed) {
                        if (timer && timer.playing) return;
                        if (WindowManager.isAnnotationActive()) return;
                        if (!WindowManager.timelineHoverThumbsEnabled) return;
                        hoverThumbRequestThrottle.pendingX = m.x;
                        if (!hoverThumbRequestThrottle.running) {
                            hoverThumbRequestThrottle.start();
                        }
                        return;
                    }
                    doMove(m.x);
                }

                onReleased: (m) => doRelease(m.x)
            }

            // Phase 7.8 Stage B — per-track Edit toggle buttons.
            // Overlaid on the upper-left of each track row, with z
            // higher than scrubArea so click-to-toggle wins over
            // click-to-seek. Only visible in dual mode; the user
            // explicitly enters edit mode per track. Stage C wires
            // this flag to clip drag handles.
            // (EditToggle component hoisted to panel root; see
            //  the flat version near the top of the file. The
            //  in-track instances live in the gutter on the left,
            //  not overlaid on the track area any more.)

            // (EditToggle instances moved to trackHeaderGutter
            //  on the left of the timeline so they're flat
            //  inline rather than overlaying the track area.
            //  EditToggle component definition is hoisted above
            //  to root scope so the gutter can reference it.)

            // Phase 7.8 — gesture catchers per track. Live at the
            // panel level so the press-grab survives clip Repeater
            // rebuilds caused by previewTrim/Slide/Slip emitting
            // timelineChanged. Without these, drag would get
            // stuck after the first timelineChanged because the
            // per-clip MA holding the grab would be destroyed and
            // onReleased would never fire.
            EditTrackMa {
                visible: loaded && root.editingA
                trackId: "A"
                x: 0
                y: 0
                width: parent.width
                height: kRowHA
            }
            EditTrackMa {
                visible: loaded && hasTrackB && root.editingB
                trackId: "B"
                x: 0
                y: kRowHA
                width: parent.width
                height: kRowHB
            }

            // Phase 7.8 Stage C — pre-drag ghost outline. Shows
            // where the clip USED to be while you're dragging it.
            // Dashed ~40% opacity stroke; no fill. Disappears on
            // mouse release.
            Rectangle {
                visible: root.editActive
                // ghostX is content-x (set in beginEdit via timeToX
                // from the snapshot's startTime). Subtract scrollX
                // to convert to viewport-x for direct rendering.
                x: editState && editState.ghostX !== undefined
                    ? editState.ghostX - root.scrollX : 0
                y: editState && editState.trackId === "B"
                    ? (kRowHA + 3) : 4
                width: editState && editState.ghostW !== undefined
                    ? editState.ghostW : 0
                height: editState && editState.trackId === "B"
                    ? (kRowHB - 6) : (kRowHA - 8)
                color: "transparent"
                border.color: "#ffc864"
                border.width: 2
                opacity: 0.4
                radius: 3
                z: 14
                // Dashed look via marching ants (overlay of small
                // alternating segments). Cheap approximation.
                Rectangle {
                    anchors.fill: parent
                    color: "transparent"
                    border.color: "#181818"
                    border.width: 0
                }
            }

            // Floating timecode badge — follows the cursor during
            // drag, shows the proposed value (head 00:00:05:12).
            Rectangle {
                visible: root.editActive
                         && editState && editState.badgeText
                         && editState.badgeText.length > 0
                x: editState && editState.cursorX !== undefined
                    ? Math.min(trackArea.width - width - 6,
                               Math.max(6, editState.cursorX + 12))
                    : 0
                y: editState && editState.trackId === "B"
                    ? (kRowHA + 3 + (kRowHB - 6) + 4)
                    : (4 + (kRowHA - 8) + 4)
                width: badgeText.implicitWidth + 12
                height: badgeText.implicitHeight + 6
                color: "#1a1a1a"
                border.color: "#ffc864"
                border.width: 1
                radius: 3
                z: 15
                Text {
                    id: badgeText
                    anchors.centerIn: parent
                    text: editState && editState.badgeText
                          ? editState.badgeText : ""
                    color: "#ffe6b6"
                    font.pixelSize: 11
                    font.family: "Menlo, monospace"
                }
            }
        }

        // ---- Overview bar -----------------------------------------
        // Mini-strip below the track row showing the entire timeline
        // scaled to fit. Translucent accent rectangle marks the
        // currently-visible portion. Drag the middle to pan; drag
        // the left or right edge to zoom (left edge anchors right
        // side, right edge anchors left side). Most useful for
        // long content where fit-to-width makes clips 1-px wide.
        Rectangle {
            id: overviewBar
            Layout.fillWidth: true
            Layout.preferredHeight: kOverviewH
            color: Theme.bgAlt
            visible: loaded && duration > 0

            Rectangle {
                id: vpIndicator
                readonly property double totalContentW: duration * pps
                readonly property double startFrac:
                    totalContentW > 0
                        ? Math.max(0, Math.min(1, scrollX / totalContentW))
                        : 0
                readonly property double endFrac:
                    totalContentW > 0
                        ? Math.max(0, Math.min(1,
                            (scrollX + trackWidth) / totalContentW))
                        : 1
                readonly property double rawWidthPx:
                    parent.width * Math.max(0, endFrac - startFrac)
                x: parent.width * startFrac
                width: Math.max(kMinVpW, rawWidthPx)
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                // Scrollbar-handle tone, darkened ~55 % so it sits
                // back from the foreground without disappearing
                // against Theme.bgAlt. Qt.darker(c, 2.2) ≈ 46 %
                // brightness ≈ #2F2F2F (15 % brighter than the
                // earlier 2.5-darken value).
                color: Qt.darker(Theme.textMuted, 2.2)
            }

            MouseArea {
                id: ovMa
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: {
                    if (!loaded || duration <= 0) return Qt.ArrowCursor;
                    const m = ovMa.mouseX;
                    const startX = vpIndicator.x;
                    const endX   = startX + vpIndicator.width;
                    if (Math.abs(m - startX) < kEdgeZone
                        || Math.abs(m - endX) < kEdgeZone)
                        return Qt.SizeHorCursor;
                    if (m >= startX && m <= endX) return Qt.OpenHandCursor;
                    return Qt.ArrowCursor;
                }

                property string dragMode: ""
                property double dragStartX: 0
                property double dragStartScrollX: 0
                property double dragStartLeftTime: 0
                property double dragStartRightTime: 0

                function clamp(v, lo, hi) {
                    return Math.max(lo, Math.min(hi, v));
                }
                function pickMode(mx) {
                    const startX = vpIndicator.x;
                    const endX   = startX + vpIndicator.width;
                    if (Math.abs(mx - startX) < kEdgeZone) return "zoomLeft";
                    if (Math.abs(mx - endX)   < kEdgeZone) return "zoomRight";
                    if (mx >= startX && mx <= endX)        return "pan";
                    return "";
                }

                onPressed: (m) => {
                    if (!loaded || duration <= 0 || pps <= 0) return;
                    dragMode         = pickMode(m.x);
                    dragStartX       = m.x;
                    dragStartScrollX = scrollX;
                    dragStartLeftTime  = scrollX / pps;
                    dragStartRightTime = (scrollX + trackWidth) / pps;
                }
                onPositionChanged: (m) => {
                    if (!pressed || !loaded || pps <= 0) return;
                    if (dragMode === "pan") {
                        const totalContentW = duration * pps;
                        if (totalContentW <= 0) return;
                        const delta = m.x - dragStartX;
                        const scrollDeltaPx =
                            (delta / overviewBar.width) * totalContentW;
                        scrollX = clamp(dragStartScrollX + scrollDeltaPx,
                                         0, maxScrollX);
                    } else if (dragMode === "zoomLeft") {
                        // Right edge anchored. New left time computed
                        // from the cursor's frac across the overview.
                        const newLeftFrac =
                            clamp(m.x / overviewBar.width, 0, 1);
                        const newLeftTime = newLeftFrac * duration;
                        const newVisible  = dragStartRightTime - newLeftTime;
                        if (newVisible <= 0.01) return;
                        // Floor at fit-to-width — see applyWheel for the
                        // long-content rationale (kPpsMin caps the
                        // visible window at ~25–53 min for feature films).
                        const fitPpsLocal = trackWidth / duration;
                        const newPps = clamp(trackWidth / newVisible,
                                              fitPpsLocal,
                                              Math.max(kPpsMax, fitPpsLocal * 50));
                        zoomLevel = newPps;
                        scrollX   = clamp(newLeftTime * newPps,
                                          0, Math.max(0, duration * newPps - trackWidth));
                    } else if (dragMode === "zoomRight") {
                        // Left edge anchored.
                        const newRightFrac =
                            clamp(m.x / overviewBar.width, 0, 1);
                        const newRightTime = newRightFrac * duration;
                        const newVisible   = newRightTime - dragStartLeftTime;
                        if (newVisible <= 0.01) return;
                        const fitPpsLocal = trackWidth / duration;
                        const newPps = clamp(trackWidth / newVisible,
                                              fitPpsLocal,
                                              Math.max(kPpsMax, fitPpsLocal * 50));
                        zoomLevel = newPps;
                        scrollX   = clamp(dragStartLeftTime * newPps,
                                          0, Math.max(0, duration * newPps - trackWidth));
                    }
                }
                onReleased: {
                    dragMode = "";
                    // Drop focus the drag may have grabbed so the
                    // window's eventFilter (held-key fast-seek) and
                    // app-wide Shortcuts don't see a stale focus
                    // target after release. forceActiveFocus on a
                    // Rectangle (overviewBar) is safe — it isn't a
                    // text-class object so the eventFilter's
                    // "is-text-item" bail-out doesn't trip.
                    overviewBar.forceActiveFocus();
                }
                onCanceled: {
                    dragMode = "";
                    overviewBar.forceActiveFocus();
                }
                // Forward wheel to the shared zoom/pan implementation.
                // The overview MouseArea otherwise consumes wheel
                // events (it covers overviewBar entirely), so without
                // this the user can't zoom while hovering the strip.
                onWheel: (wheel) => {
                    // Wheel x is overview-local; scale the proportion
                    // back to track-area coordinates.
                    const trackX = (overviewBar.width > 0)
                        ? (wheel.x / overviewBar.width) * trackWidth
                        : wheel.x;
                    root.applyWheel(trackX,
                                    wheel.angleDelta.y,
                                    (wheel.modifiers
                                     & Qt.AltModifier) !== 0);
                    wheel.accepted = true;
                }
            }
        }

        // (Status row moved to TimelineStatus.qml — sits above
        //  TransportBar in Main.qml so transport tooltips have a
        //  buffer instead of overflowing into the viewport.)
        }   // close inner ColumnLayout

        // ---- Track-header gutter (edit toggles) — right side
        // Holds per-track edit-mode toggles. Sits on the right of
        // the track area; the divider hugs its left edge against
        // the track area.
        Item {
            id: trackHeaderGutter
            Layout.preferredWidth: kGutterW
            Layout.fillHeight: true

            // Empty space above tracks (matches ruler height).
            Item {
                id: gutterRulerSpacer
                anchors.left:  parent.left
                anchors.right: parent.right
                anchors.top:   parent.top
                height: 20
            }
            // Left-edge divider against the track area.
            Rectangle {
                anchors.left:   parent.left
                anchors.top:    parent.top
                anchors.bottom: parent.bottom
                width: 1
                color: Theme.divider
            }

            // Match ClipDelegate's y/height — clips inset 4 top/
            // bottom on track A, 3 top/bottom on track B (see line
            // 696-698). Toggle aligns with the visible clip body,
            // not the full row.
            // Hardcoded y values — push the toggle below the ruler
            // (20 px) plus the per-track inset (4 for A, 3 for B)
            // so it lines up with the visible clip body. The
            // gutterRulerSpacer.height binding wasn't resolving in
            // time, leaving the toggles at the gutter top edge.
            EditToggle {
                visible: loaded && root.editableMode
                anchors.left:  parent.left
                anchors.right: parent.right
                y: 20 + 4 + 2
                height: kRowHA - 8
                editing: editingA
                onToggled: {
                    editingA = !editingA;
                    if (!editingA && root.selectedTrackId === "A") {
                        root.clearSelection();
                    }
                }
            }
            EditToggle {
                visible: loaded && hasTrackB && root.dualActive
                anchors.left:  parent.left
                anchors.right: parent.right
                y: 20 + kRowHA + 3 + 2
                height: kRowHB - 6
                editing: editingB
                onToggled: {
                    editingB = !editingB;
                    if (!editingB && root.selectedTrackId === "B") {
                        root.clearSelection();
                    }
                }
            }
        }
    }       // close outer RowLayout
}           // close Pane
