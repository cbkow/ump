// StatusStrip — bottom-most strip below the Color panel.
//
// Holds the chips that previously lived in the Phase 1.8.x
// decoder status row: mode / state / sourcePath / dimensions /
// codec / pixfmt / HW-accel / OCIO / source-TC / decode-error
// count.
//
// Polish pass: mode-aware. Single video keeps the full decoder
// readout; dual mode summarises both slots; playlist mode shows
// clip count + the active clip's codec line.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Qcv

Pane {
    id: root
    padding: 0

    background: Rectangle {
        color: "#141414"
        Rectangle {
            anchors.left:  parent.left
            anchors.right: parent.right
            anchors.top:   parent.top
            height: 1
            color:  Theme.divider
        }
    }

    // ---- Mode detection -----------------------------------------
    readonly property bool isDual:
        WindowManager.dualController !== null
        && WindowManager.dualController !== undefined
    readonly property bool isPlaylist:
        WindowManager.timeline
        && WindowManager.timeline.sourceMode === 1
    // The single-video decoder readout drives the chip rendering
    // for single + image-seq modes; dual / playlist branches
    // build their own chips.
    readonly property var v: WindowManager.videoDecoder
    readonly property var vB: WindowManager.videoDecoderB

    function modeLabel() {
        if (isDual)     return qsTr("DUAL");
        if (isPlaylist) return qsTr("PLAYLIST");
        if (WindowManager.imageSeqActive) return qsTr("IMAGE SEQ");
        return qsTr("SINGLE");
    }

    function stateLabel(d) {
        if (!d) return "";
        switch (d.state) {
            case 0: return qsTr("idle");
            case 1: return qsTr("opening…");
            case 2: return qsTr("decoding");
            case 3: return qsTr("EOF");
            case 4: return qsTr("ERROR");
        }
        return "";
    }

    function dimsLabel(d) {
        if (!d || d.width <= 0) return "";
        return d.width + "×" + d.height;
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin:  Theme.gutterWidth
        anchors.rightMargin: Theme.padding
        spacing: Theme.paddingLoose

        // Mode chip — always visible.
        Text {
            text: root.modeLabel()
            color: Theme.accent
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontSizeTiny
            font.bold: true
            font.letterSpacing: 0.5
        }

        // ---- Single / image-seq path --------------------------
        Text {
            visible: !root.isDual && !root.isPlaylist && root.v
            text: root.stateLabel(root.v)
            color: root.v && root.v.state === 4
                   ? Theme.error : Theme.textSecondary
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontSizeTiny
        }
        Text {
            visible: !root.isDual && !root.isPlaylist
            Layout.fillWidth: true
            text: root.v && root.v.sourcePath ? root.v.sourcePath : ""
            color: Theme.textMuted
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontSizeTiny
            elide: Text.ElideMiddle
        }
        Text {
            visible: !root.isDual && !root.isPlaylist
                     && root.v && root.v.width > 0
            text: root.v
                  ? qsTr("%1  %2  %3")
                        .arg(root.dimsLabel(root.v))
                        .arg(root.v.codecName)
                        .arg(root.v.pixelFormat)
                  : ""
            color: Theme.textSecondary
            font.family: Theme.monoFamily
            font.pixelSize: Theme.fontSizeTiny
        }

        // ---- Dual mode summary --------------------------------
        Text {
            visible: root.isDual
            Layout.fillWidth: true
            text: {
                const a = root.v   ? root.dimsLabel(root.v)  : "";
                const b = root.vB  ? root.dimsLabel(root.vB) : "";
                const aName = root.v
                              ? (root.v.sourcePath || "").split("/").pop()
                              : "";
                const bName = root.vB
                              ? (root.vB.sourcePath || "").split("/").pop()
                              : "";
                let line = "";
                if (aName) line += "A: " + aName + (a ? "  " + a : "");
                if (line.length > 0 && (bName || b)) line += "    ";
                if (bName) line += "B: " + bName + (b ? "  " + b : "");
                return line;
            }
            color: Theme.textSecondary
            font.family: Theme.monoFamily
            font.pixelSize: Theme.fontSizeTiny
            elide: Text.ElideMiddle
        }

        // ---- Playlist mode summary ----------------------------
        Text {
            visible: root.isPlaylist
            Layout.fillWidth: true
            text: {
                const t = WindowManager.timeline;
                const total = t ? t.timer.duration : 0;
                const codec = root.v ? root.v.codecName : "";
                const dims  = root.v ? root.dimsLabel(root.v) : "";
                let s = "";
                if (codec) s += codec;
                if (dims)  s += "  " + dims;
                if (total > 0) {
                    if (s) s += "  • ";
                    s += total.toFixed(1) + " s total";
                }
                return s;
            }
            color: Theme.textSecondary
            font.family: Theme.monoFamily
            font.pixelSize: Theme.fontSizeTiny
            elide: Text.ElideMiddle
        }

        // ---- Shared chips (HW / OCIO / TC / errors) -----------
        Text {
            visible: root.isDual ? true : !!root.v
            text: {
                if (root.isDual) {
                    // Per-side label from DualPlaybackController:
                    // "A: vulkan  |  B: software" etc.
                    return qsTr("HW: %1").arg(
                        WindowManager.dualController.hwAccel);
                }
                return (root.v && root.v.hwAccel)
                    ? qsTr("HW: %1").arg(root.v.hwAccel)
                    : qsTr("HW: software");
            }
            color: {
                if (root.isDual) {
                    // Green if both sides hw, muted otherwise.
                    const t = WindowManager.dualController.hwAccel;
                    return t.indexOf("software") === -1
                        ? Theme.success : Theme.textMuted;
                }
                return (root.v && root.v.hwAccel)
                    ? Theme.success : Theme.textMuted;
            }
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontSizeTiny
            font.bold: true
        }
        Text {
            visible: WindowManager.ocio
                     && WindowManager.ocio.configIdentifier
                     && WindowManager.ocio.configIdentifier.length > 0
            text: WindowManager.ocio
                  ? qsTr("OCIO: %1").arg(WindowManager.ocio.configIdentifier)
                  : ""
            color: Theme.info
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontSizeTiny
        }
        Text {
            visible: !root.isDual && !root.isPlaylist
                     && root.v && root.v.sourceTimecode
                     && root.v.sourceTimecode.length > 0
            text: root.v
                  ? (root.v.isDropFrame
                        ? qsTr("TC: %1 DF") : qsTr("TC: %1"))
                        .arg(root.v.sourceTimecode)
                  : ""
            color: root.v && root.v.isDropFrame
                   ? Theme.warn : Theme.info
            font.family: Theme.monoFamily
            font.pixelSize: Theme.fontSizeTiny
        }
        Text {
            visible: root.v && root.v.decodeErrorCount > 0
            text: root.v
                  ? qsTr("%1 errors").arg(root.v.decodeErrorCount)
                  : ""
            color: Theme.error
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontSizeTiny
        }

        // ---- Transient export status (right-aligned chip) -----
        // Driven by WindowManager.exportStarted / exportFinished.
        // Shows a spinner while a screenshot save is in flight, then
        // a colored result that auto-clears. It lives here rather than
        // over the viewport because on macOS the native Metal surface
        // covers the viewport — StatusStrip chrome is always visible.
        RowLayout {
            id: exportChip
            visible: false
            spacing: Theme.spacing
            // Right margin so the result text clears the rounded
            // bottom-right window corner (macOS Tahoe curvature).
            Layout.rightMargin: Theme.gutterWidth
            property bool busy: false

            // Tiny self-contained spinner — the stock BusyIndicator
            // doesn't render at this size under the app's QQC2 style,
            // so draw a rotating arc directly. Only animates/paints
            // while an export is in flight.
            Item {
                Layout.preferredWidth: 12
                Layout.preferredHeight: 12
                visible: exportChip.busy
                Canvas {
                    id: spinnerCanvas
                    anchors.fill: parent
                    property real phase: 0
                    onPaint: {
                        const ctx = getContext("2d");
                        ctx.reset();
                        const cx = width / 2, cy = height / 2;
                        const r  = width / 2 - 1.5;
                        ctx.lineWidth   = 1.5;
                        ctx.lineCap     = "round";
                        ctx.strokeStyle = Theme.accent;
                        ctx.beginPath();
                        ctx.arc(cx, cy, r, phase, phase + Math.PI * 1.4);
                        ctx.stroke();
                    }
                    onPhaseChanged: requestPaint()
                    NumberAnimation on phase {
                        running: exportChip.busy
                        loops: Animation.Infinite
                        from: 0
                        to: 2 * Math.PI
                        duration: 750
                    }
                }
            }
            Text {
                id: exportStatusText
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontSizeTiny
                font.bold: true
                color: Theme.textSecondary
            }
        }
        Connections {
            target: WindowManager
            function onExportStarted(label) {
                exportStatusText.text  = label;
                exportStatusText.color = Theme.textSecondary;
                exportChip.busy        = true;
                exportChip.visible     = true;
                exportHideTimer.stop();
            }
            function onExportFinished(ok, message) {
                exportStatusText.text  = message;
                exportStatusText.color = ok ? Theme.success : Theme.error;
                exportChip.busy        = false;
                exportChip.visible     = true;
                exportHideTimer.restart();
            }
        }
        Timer {
            id: exportHideTimer
            interval: 3500
            onTriggered: exportChip.visible = false
        }
    }
}
