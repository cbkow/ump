// TimelineStatus — frame counter / SMPTE timecode / fps strip.
//
// Phase 7.6.b: extracted from the bottom of TimelinePanel and
// placed above the TransportBar so the digit readouts live close to
// the playback controls that change them, not buried under the
// timeline. (This strip once also served as a "tooltip buffer" so
// in-scene transport tooltips wouldn't fall off the viewport bottom;
// tooltips are now Popup.Window OS popups that the OS clamps to the
// screen, so that's no longer a reason the strip exists.)
//
// Bound through the WindowManager unified accessors so the same
// strip drives image sequences and videos without QML branching
// on currentMediaKind.
//
// Polish pass: every readout uses the mono font; the in/out
// range "chip" is now flat text separated by middle dots rather
// than a coloured bordered rectangle.

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Qcv

Rectangle {
    id: root

    // Tone plus a faint top edge (below) separate the strip from the
    // viewport — the bottom-band framing pass added the edge over
    // the earlier tone-only rule (borders pass, 2026-07-07).
    color: Theme.bgAlt

    // Phase 3.H.3 — playlist mode shows seconds-based readouts;
    // frame counters and SMPTE timecode hide because the playlist
    // clock is derived (master fps != per-clip fps), so any
    // frame-precision number would be a half-truth.
    readonly property bool isPlaylistMode:
        WindowManager.timeline
        && WindowManager.timeline.sourceMode === 1

    function fmtSec(s) {
        if (!isFinite(s) || s < 0) s = 0;
        const ms  = Math.round(s * 1000);
        const totalS = Math.floor(ms / 1000);
        const f3  = (ms % 1000).toString().padStart(3, "0");
        const sec = totalS % 60;
        const min = Math.floor(totalS / 60) % 60;
        const hr  = Math.floor(totalS / 3600);
        const ss  = sec.toString().padStart(2, "0");
        if (hr > 0) {
            const mm = min.toString().padStart(2, "0");
            return hr + ":" + mm + ":" + ss + "." + f3;
        }
        return min + ":" + ss + "." + f3;
    }

    // Faint top edge — encloses the bottom-band unit against the
    // viewport above. Half-opacity divider tone: a full-strength
    // line here outweighed the gutter lines it joins (the tone step
    // used to be the only separation; see borders pass 2026-07-07).
    Rectangle {
        anchors.left:  parent.left
        anchors.right: parent.right
        anchors.top:   parent.top
        height: Theme.dividerWidth
        color: Theme.divider
        opacity: 0.5
    }

    // Gutter dividers — continue the timeline's side-column edges up
    // through this row (the strip is already the timeline's bgAlt
    // tone, so only the 1px lines are needed; see TransportBar's
    // gutter caps for the toolbar-toned row below).
    Rectangle {
        x: Theme.gutterWidth - 1
        anchors.top:    parent.top
        anchors.bottom: parent.bottom
        width: 1
        color: Theme.divider
    }
    Rectangle {
        x: parent.width - Theme.gutterWidth
        anchors.top:    parent.top
        anchors.bottom: parent.bottom
        width: 1
        color: Theme.divider
    }

    RowLayout {
        anchors.fill: parent
        // Inset to the timeline's gutter columns so this row, the
        // transport row, and the timeline share one content edge
        // (see TransportBar's matching margins).
        anchors.leftMargin: Theme.gutterWidth
        anchors.rightMargin: Theme.gutterWidth
        spacing: Theme.paddingLoose

        // ---- Frame counter / playlist seconds --------------------
        // Primary readouts sit in recessed wells (aesthetics pass 3)
        // — the fixed well anchors the churning digits visually.
        // Clickable: opens the go-to popup (frame entry, or time
        // entry in playlist mode). Hover shows a border so the well
        // reads as interactive without breaking the recessed look.
        Rectangle {
            id: frameWell
            Layout.preferredWidth: 160
            Layout.preferredHeight: 18
            radius: Theme.radiusSmall
            color: Theme.surfaceRecess
            border.width: 1
            border.color: frameWellMa.containsMouse
                          ? Theme.divider : "transparent"
            MouseArea {
                id: frameWellMa
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: {
                    if (root.isPlaylistMode) {
                        goToPopup.openFor("time", frameWell);
                    } else if (WindowManager.frameCountUnified() > 0) {
                        goToPopup.openFor("frame", frameWell);
                    }
                }
            }
            Text {
                anchors.fill: parent
                anchors.leftMargin: 6
                anchors.rightMargin: 6
                verticalAlignment: Text.AlignVCenter
                text: {
                    const t = WindowManager.timeline
                              ? WindowManager.timeline.timer : null;
                    const _refresh = t ? t.position : 0;
                    const _src     = WindowManager.imageSeqActive;
                    if (!t) return qsTr("Time —");
                    if (root.isPlaylistMode) {
                        return root.fmtSec(t.position) + " / "
                             + root.fmtSec(t.duration);
                    }
                    const fc = WindowManager.frameCountUnified();
                    if (fc <= 0) return qsTr("Frame —");
                    // Source-of-truth frame number — the active decoder's
                    // own currentFrame (dual master / video source / image-
                    // seq cache), NOT position × timeline.frameRate. The
                    // latter is wrong in dual mode where the timeline's
                    // frameRate is fpsA but the master clock runs masterFps.
                    const cf = WindowManager.currentFrameUnified();
                    return qsTr("Frame %1 / %2").arg(cf).arg(fc - 1);
                }
                color: Theme.textPrimary
                font.family: Theme.monoFamily
                font.pixelSize: Theme.fontSizeSmall
                elide: Text.ElideRight
            }
        }
        // ---- SMPTE timecode --------------------------------------
        // Clickable like the frame well — opens the go-to popup in
        // timecode mode (only visible when an fps clock exists, so
        // no extra enable guard needed).
        Rectangle {
            id: tcWell
            border.width: 1
            border.color: tcWellMa.containsMouse
                          ? Theme.divider : "transparent"
            MouseArea {
                id: tcWellMa
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: goToPopup.openFor("timecode", tcWell)
            }
            visible: !root.isPlaylistMode
                     && (WindowManager.dualController
                         ? (WindowManager.dualController.fps > 0)
                         : (!WindowManager.imageSeqActive
                            && WindowManager.videoDecoder
                            && WindowManager.videoDecoder.fps > 0))
            Layout.preferredWidth: 130
            Layout.preferredHeight: 18
            radius: Theme.radiusSmall
            color: Theme.surfaceRecess
            Text {
                anchors.fill: parent
                anchors.leftMargin: 6
                anchors.rightMargin: 6
                verticalAlignment: Text.AlignVCenter
                text: {
                    if (WindowManager.dualController) {
                        return WindowManager.dualController.formatTimecode(
                            Math.max(0, WindowManager.dualController.currentFrame));
                    }
                    if (WindowManager.videoDecoder) {
                        // formatTimecode is a Q_INVOKABLE without its own
                        // notify — reference startTimecode so the binding
                        // re-evaluates when the user clicks an Origin
                        // pill ("From start" / Embedded / QT Start / ...).
                        // Without this, the playhead readout stays on the
                        // previous origin until the next frame change.
                        const _originTag = WindowManager.videoDecoder.startTimecode;
                        return WindowManager.videoDecoder.formatTimecode(
                            Math.max(0, WindowManager.videoDecoder.currentFrame));
                    }
                    return "";
                }
                // Drop-frame timecodes get the warn color so the
                // user knows the values aren't strictly continuous.
                color: !WindowManager.dualController
                       && WindowManager.videoDecoder
                       && WindowManager.videoDecoder.isDropFrame
                       ? Theme.warn : Theme.textPrimary
                font.family: Theme.monoFamily
                font.pixelSize: Theme.fontSizeSmall
            }
        }

        // ---- In/Out range readout (flat) -------------------------
        // No background, no border — just text separated by middle
        // dots. Same accent color as set in/out points elsewhere.
        // Hidden in playlist mode to match the disabled transport
        // In/Out buttons + hidden timeline markers — loop in/out
        // isn't functional in playlist, so the RANGE chip would
        // mislead even when state is technically still set.
        RowLayout {
            visible: WindowManager.hasInOutRange && !root.isPlaylistMode
            spacing: Theme.spacingLoose
            Text {
                text: qsTr("RANGE")
                color: Theme.accent
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontSizeTiny
                font.bold: true
                font.letterSpacing: 0.5
            }
            Text {
                visible: !root.isPlaylistMode
                text: {
                    const inP  = WindowManager.inPoint;
                    const outP = WindowManager.outPoint;
                    if (inP < 0 || outP <= inP) return "";
                    return qsTr("%1 fr").arg(outP - inP + 1);
                }
                color: Theme.textPrimary
                font.family: Theme.monoFamily
                font.pixelSize: Theme.fontSizeSmall
            }
            Text {
                visible: !root.isPlaylistMode
                         && WindowManager.videoDecoder
                         && WindowManager.videoDecoder.fps > 0
                         && !WindowManager.imageSeqActive
                text: {
                    const inP  = WindowManager.inPoint;
                    const outP = WindowManager.outPoint;
                    if (inP < 0 || outP <= inP) return "";
                    if (!WindowManager.videoDecoder) return "";
                    return WindowManager.videoDecoder.formatTimecode(
                        outP - inP + 1);
                }
                color: Theme.textPrimary
                font.family: Theme.monoFamily
                font.pixelSize: Theme.fontSizeSmall
            }
            Text {
                text: {
                    const inP  = WindowManager.inPoint;
                    const outP = WindowManager.outPoint;
                    // In/out points are frame numbers in the active
                    // clock's space (dual master / video source). Use
                    // THAT clock's fps to convert to seconds — not the
                    // timeline display frameRate, which is fpsA in dual.
                    let fps = 0;
                    if (WindowManager.dualController) {
                        fps = WindowManager.dualController.fps;
                    } else if (WindowManager.videoDecoder
                               && !WindowManager.imageSeqActive
                               && WindowManager.videoDecoder.fps > 0) {
                        fps = WindowManager.videoDecoder.fps;
                    } else {
                        const t = WindowManager.timeline
                                  ? WindowManager.timeline.timer : null;
                        fps = t ? t.frameRate : 0;
                    }
                    if (inP < 0 || outP <= inP || fps <= 0) return "";
                    return qsTr("%1 s")
                        .arg(((outP - inP + 1) / fps).toFixed(2));
                }
                color: Theme.textPrimary
                font.family: Theme.monoFamily
                font.pixelSize: Theme.fontSizeSmall
            }
        }

        Item { Layout.fillWidth: true }

        // ---- fps -------------------------------------------------
        Text {
            // Breathing room against the right gutter divider.
            Layout.rightMargin: Theme.padding
            visible: !root.isPlaylistMode
            text: {
                // Show the active clock's fps. In dual that's the
                // master fps (max of both sides), not the timeline's
                // frameRate which stayed at fpsA.
                let fps = 0;
                if (WindowManager.dualController) {
                    fps = WindowManager.dualController.fps;
                } else if (WindowManager.videoDecoder
                           && !WindowManager.imageSeqActive
                           && WindowManager.videoDecoder.fps > 0) {
                    fps = WindowManager.videoDecoder.fps;
                } else {
                    const t = WindowManager.timeline
                              ? WindowManager.timeline.timer : null;
                    fps = t ? t.frameRate : 0;
                }
                return fps > 0 ? qsTr("%1 fps").arg(fps.toFixed(3)) : "";
            }
            color: Theme.textSecondary
            font.family: Theme.monoFamily
            font.pixelSize: Theme.fontSizeSmall
        }
    }

    // ---- Go-to navigation popup ------------------------------------
    // Opened by clicking the frame or timecode well. Opens DOWNWARD
    // (over the transport row) — an in-scene Popup is safe there
    // because the native player surface only covers the viewport
    // above; opening upward would z-order under it.
    Popup {
        id: goToPopup
        property string mode: "frame"     // "frame" | "timecode" | "time"
        property bool entryError: false
        width: 210
        padding: Theme.padding
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        background: Rectangle {
            color: Theme.surface
            border.width: 1
            border.color: Theme.divider
            radius: Theme.radiusSmall
        }

        function openFor(mode, anchorItem) {
            goToPopup.mode = mode;
            goToPopup.parent = anchorItem;
            goToPopup.x = 0;
            goToPopup.y = anchorItem.height + 6;
            goToPopup.entryError = false;
            goToField.text = "";
            goToPopup.open();
        }
        onOpened: goToField.forceActiveFocus()

        // "90", "1:30", "1:30.5", "01:02:03.250" → seconds; -1 on junk.
        function flexTimeToSeconds(s) {
            const parts = s.split(":");
            if (parts.length < 1 || parts.length > 3) return -1;
            let secs = 0;
            for (let i = 0; i < parts.length; ++i) {
                const p = parts[i].trim();
                if (!p.length || !/^[0-9]+(\.[0-9]+)?$/.test(p)) return -1;
                secs = secs * 60 + parseFloat(p);
            }
            return secs;
        }

        function commit() {
            const s = goToField.text.trim();
            if (!s.length) { goToPopup.close(); return; }
            if (mode === "frame") {
                if (!/^[0-9]+$/.test(s)) { entryError = true; return; }
                const fc = WindowManager.frameCountUnified();
                if (fc <= 0) { goToPopup.close(); return; }
                WindowManager.seekToFrame(
                    Math.min(parseInt(s, 10), fc - 1));
            } else if (mode === "timecode") {
                // Digits-only entry pads right-aligned into TC pairs
                // (Resolve-style): "1000" → "00:00:10:00".
                let entry = s;
                if (/^[0-9]+$/.test(s) && s.length <= 8) {
                    const p = s.padStart(8, "0");
                    entry = p.slice(0, 2) + ":" + p.slice(2, 4) + ":"
                          + p.slice(4, 6) + ":" + p.slice(6, 8);
                }
                let f = -1;
                if (WindowManager.dualController) {
                    f = WindowManager.dualController.parseTimecode(entry);
                } else if (WindowManager.videoDecoder) {
                    f = WindowManager.videoDecoder.parseTimecode(entry);
                }
                if (f < 0) { entryError = true; return; }
                const fc = WindowManager.frameCountUnified();
                if (fc > 0) f = Math.min(f, fc - 1);
                WindowManager.seekToFrame(f);
            } else { // "time" — playlist seconds
                const secs = flexTimeToSeconds(s);
                if (secs < 0) { entryError = true; return; }
                const t = WindowManager.timeline
                          ? WindowManager.timeline.timer : null;
                const dur = t ? t.duration : 0;
                WindowManager.seekToTime(
                    Math.max(0, Math.min(secs, dur)));
            }
            goToPopup.close();
        }

        contentItem: ColumnLayout {
            spacing: Theme.spacing
            Text {
                text: goToPopup.mode === "frame"
                          ? qsTr("GO TO FRAME")
                      : goToPopup.mode === "timecode"
                          ? qsTr("GO TO TIMECODE")
                          : qsTr("GO TO TIME")
                color: Theme.textSecondary
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontSizeTiny
                font.bold: true
                font.letterSpacing: 0.5
            }
            FlatTextField {
                id: goToField
                Layout.fillWidth: true
                font.family: Theme.monoFamily
                // Invalid entry: warn-toned digits until the next edit.
                color: goToPopup.entryError
                       ? Theme.warn : Theme.textPrimary
                placeholderText: goToPopup.mode === "frame"
                                     ? qsTr("frame number")
                                 : goToPopup.mode === "timecode"
                                     ? qsTr("HH:MM:SS:FF")
                                     : qsTr("MM:SS.mmm")
                onTextEdited: goToPopup.entryError = false
                onAccepted: goToPopup.commit()
                // Dismiss on ANY focus loss, not just in-scene
                // presses (CloseOnPressOutside can't see clicks on
                // the native player surface — a separate QWindow —
                // but those clicks move window focus there, which
                // lands here). Entered text is discarded: navigation
                // only ever happens on an explicit Enter.
                onActiveFocusChanged:
                    if (!activeFocus && goToPopup.opened)
                        goToPopup.close()
            }
        }
    }
}
