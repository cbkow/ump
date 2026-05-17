// Player window — child of UI window, hosts the QRhi render pipeline.
//
// Phase 1.3: PlayerRhiItem fills the window and clears to its
// configured color each frame. Future phases extend the renderer to
// perform compositor / OCIO / stroke / safety / display passes
// (Guide 01 §5).

import QtQuick
import QtQuick.Window
import Qcv.Render

Window {
    id: playerWindow

    flags: Qt.FramelessWindowHint
    color: "#000000"   // shouldn't be visible — RhiItem fills the window

    PlayerRhiItem {
        id: rhi
        objectName: "playerRhiItem"   // looked up by WindowManager
        anchors.fill: parent
        clearColor: "#0a1a2a"

        // Compositor settings driven by Main.qml via WindowManager.
        compositorMode: WindowManager.compositorMode
        splitPos: WindowManager.splitPos

        // Phase 1.8.1: live video frames into Source A.
        videoDecoder: WindowManager.videoDecoder
        // Phase 2.3: OCIO display pass replaces passthrough.
        ocio: WindowManager.ocio
        // Phase 7.4.b.4 pull-model: when this property is non-null
        // the renderer takes the image-seq cache-pull path (gating
        // the video fetchLatest path); when null, video path runs.
        // The cache pointer is the gate — no separate active flag.
        imageSeqCache: WindowManager.imageSeqCache
    }

    // Pull-model wake: when the playhead crosses a frame boundary,
    // nudge the renderer to pull. Renderer reads the playhead from
    // the cache; this signal carries no payload, only the trigger.
    Connections {
        target: WindowManager
        function onImageSeqFrameAdvanced(frame) {
            rhi.update();
        }
    }

    // Phase 7.1.1: drop-onto-viewport intake. The player is a
    // native child QQuickWindow that sits above the UI window's
    // centerStage Item, so a DropArea placed on the UI side is
    // occluded — drops have to be received by this window directly.
    // On drop: addMediaFile + setActiveItem on the last URL, which
    // routes through ProjectManager.loadRequested → VideoDecoder.
    DropArea {
        id: viewportDrop
        anchors.fill: parent
        onDropped: (drop) => {
            if (!WindowManager.project) return;
            if (!drop.hasUrls) return;
            let lastId = "";
            for (let i = 0; i < drop.urls.length; ++i) {
                // Centralized URL→OS-path on WindowManager. Handles
                // file:///C:/… (Win), file:///path (POSIX), and the
                // file://server/… UNC form that earlier inline
                // conversions missed (the substring(7) shortcut
                // stripped both leading slashes, leaving "server/path"
                // which QFile rejects with "does not exist").
                const path = WindowManager.urlToOsPath(drop.urls[i]);
                if (!path) continue;
                const id = WindowManager.project.addMediaFile(path);
                if (id) lastId = id;
            }
            if (lastId) WindowManager.project.setActiveItem(lastId);
            drop.accept();
        }
    }

    // Drop-overlay tint while dragging — drawn on top of the
    // QRhiItem so the user has visual feedback even though the
    // actual drop target is invisible.
    Rectangle {
        anchors.fill: parent
        color: "transparent"
        border.color: "#88aaff"
        border.width: 4
        opacity: viewportDrop.containsDrag ? 0.7 : 0.0
        Behavior on opacity { NumberAnimation { duration: 100 } }
    }

    // Lightweight overlay for Phase 1.7 status. Removed once we have
    // the real transport / playback HUD (Guide 02 §3).
    Column {
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 8
        anchors.horizontalCenter: parent.horizontalCenter
        spacing: 2

        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: qsTr("Player — Phase 1.7 (compositor: %1)").arg(modeName(rhi.compositorMode))
            font.pixelSize: 11
            color: "#7090b0"
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: qsTr("Window: %1×%2").arg(playerWindow.width).arg(playerWindow.height)
            font.pixelSize: 10
            color: "#506070"
        }
    }

    function modeName(mode) {
        switch (mode) {
            case PlayerRhiItem.Single:     return "Single";
            case PlayerRhiItem.SideBySide: return "Side-by-Side";
            case PlayerRhiItem.SplitWipe:  return "Split-Wipe";
            default:                       return "?";
        }
    }
}
