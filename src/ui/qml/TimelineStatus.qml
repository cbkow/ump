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
import QtQuick.Layouts
import Qcv

Rectangle {
    id: root

    color: Theme.bgAlt
    Rectangle {
        anchors.left:  parent.left
        anchors.right: parent.right
        anchors.top:   parent.top
        height: 1
        color:  Theme.divider
    }

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

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: Theme.padding
        anchors.rightMargin: Theme.padding
        spacing: Theme.paddingLoose

        // ---- Frame counter / playlist seconds --------------------
        // Primary readouts sit in recessed wells (aesthetics pass 3)
        // — read-only data vocabulary, and the fixed well anchors the
        // churning digits visually.
        Rectangle {
            Layout.preferredWidth: 160
            Layout.preferredHeight: 18
            radius: Theme.radiusSmall
            color: Theme.surfaceRecess
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
        Rectangle {
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
}
