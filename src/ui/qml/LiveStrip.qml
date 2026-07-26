import QtQuick
import QtQuick.Layouts
import Qcv

// LiveStrip — v2.2.3. Replaces the TimelineStatus + TransportBar +
// TimelinePanel rows while a live srt:// stream is active (there is
// nothing to scrub and no frame count that means anything — a grayed
// transport would read as "broken", not "live").
//
// Left→right: status dot + state, stream name, received facts
// (resolution / codec / pix_fmt — the permanent home of the NVENC
// 4:4:4-downgrade verification), measured fps + Mbps (1 s poll),
// elapsed, reconnect count, screenshot buttons (the one transport
// action that works on live).
Rectangle {
    id: root
    color: Theme.surface

    // Top hairline — keeps the bottom-band seam the timeline rows had.
    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: 1 / Screen.devicePixelRatio
        color: Theme.divider
    }

    readonly property var live: WindowManager.liveDecoder
    // Status enum mirror (LiveStreamDecoder::Status).
    readonly property bool isLive:         live && live.status === 2
    readonly property bool isReconnecting: live && live.status === 3
    readonly property bool isConnecting:   live && live.status === 1

    // 1 s poll → fps / Mbps from counter deltas.
    property double _lastFrames: 0
    property double _lastBytes: 0
    property double measuredFps: 0
    property double measuredMbps: 0
    property int    liveSeconds: 0
    Timer {
        interval: 1000
        running: root.visible && !!root.live
        repeat: true
        onTriggered: {
            const f = root.live.statFramesReceived();
            const b = root.live.statBytesReceived();
            root.measuredFps  = Math.max(0, f - root._lastFrames);
            root.measuredMbps = Math.max(0, (b - root._lastBytes) * 8 / 1e6);
            root._lastFrames  = f;
            root._lastBytes   = b;
            root.liveSeconds  = root.live.statLiveSeconds();
        }
    }

    function _elapsedText(secs) {
        const h = Math.floor(secs / 3600);
        const m = Math.floor((secs % 3600) / 60);
        const s = secs % 60;
        const mm = (m < 10 && h > 0) ? "0" + m : "" + m;
        const ss = s < 10 ? "0" + s : "" + s;
        return h > 0 ? h + ":" + mm + ":" + ss : mm + ":" + ss;
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: Theme.spacingLoose
        anchors.rightMargin: Theme.spacingLoose
        spacing: Theme.spacingLoose

        // ---- Status badge ----------------------------------------
        Rectangle {
            width: 8; height: 8; radius: 4
            Layout.alignment: Qt.AlignVCenter
            color: root.isLive ? "#e5484d"
                 : (root.isReconnecting || root.isConnecting)
                   ? "#e6a23c" : Theme.textMuted
            // On-air pulse while live.
            SequentialAnimation on opacity {
                running: root.isLive
                loops: Animation.Infinite
                NumberAnimation { from: 1.0; to: 0.35; duration: 900 }
                NumberAnimation { from: 0.35; to: 1.0; duration: 900 }
            }
            opacity: 1.0
        }
        Text {
            text: root.isLive ? qsTr("LIVE")
                : root.isReconnecting ? qsTr("RECONNECTING")
                : root.isConnecting ? qsTr("CONNECTING")
                : qsTr("OFFLINE")
            color: root.isLive ? Theme.textBright : Theme.textPrimary
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontSizeSmall
            font.bold: true
        }

        Rectangle {
            Layout.preferredWidth: 1
            Layout.fillHeight: true
            Layout.topMargin: 6
            Layout.bottomMargin: 6
            color: Theme.divider
        }

        // ---- Stream identity + received facts --------------------
        Text {
            text: WindowManager.project && WindowManager.project.activeItem
                  ? (WindowManager.project.activeItem.name || "")
                  : ""
            color: Theme.textPrimary
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontSizeSmall
            elide: Text.ElideMiddle
            Layout.maximumWidth: 220
        }
        Text {
            visible: !!root.live && root.live.width > 0
            text: root.live
                  ? root.live.width + "×" + root.live.height
                    + "  ·  " + root.live.codecName
                    + "  ·  " + root.live.pixelFormatName
                  : ""
            color: Theme.textMuted
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontSizeSmall
        }

        Item { Layout.fillWidth: true }

        // ---- Measured rates + elapsed + drops ---------------------
        Text {
            visible: root.isLive
            text: root.measuredFps.toFixed(0) + " fps  ·  "
                  + root.measuredMbps.toFixed(1) + " Mb/s  ·  "
                  + root._elapsedText(root.liveSeconds)
            color: Theme.textMuted
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontSizeSmall
        }
        Text {
            visible: !!root.live && root.live.reconnectCount > 0
            text: qsTr("%1 drop%2")
                  .arg(root.live ? root.live.reconnectCount : 0)
                  .arg(root.live && root.live.reconnectCount === 1 ? "" : "s")
            color: "#e6a23c"
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontSizeSmall
        }

        Rectangle {
            Layout.preferredWidth: 1
            Layout.fillHeight: true
            Layout.topMargin: 6
            Layout.bottomMargin: 6
            color: Theme.divider
        }

        // ---- Screenshots — the transport action that works live --
        FlatButton {
            iconName: "file-image"
            enabled: root.isLive
            tooltipText: qsTr("Screenshot to clipboard (⌥T)")
            onClicked: WindowManager.screenshotToClipboard()
        }
        FlatButton {
            iconName: "file-arrow-down"
            enabled: root.isLive
            tooltipText: qsTr("Screenshot to Desktop (T)")
            onClicked: WindowManager.screenshotToFile()
        }
    }
}
