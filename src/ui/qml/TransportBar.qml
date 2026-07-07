// TransportBar — Phase 7.6.b transport row.
//
// Single-row layout per Guide 03 §5. All transport buttons route
// through WindowManager's unified methods (stepFrames /
// seekToStart / seekToEnd / togglePlayback / seekToFrame) so the
// same controls drive video and image-seq without QML branching
// on currentMediaKind.
//
// Polish pass: flat toolstrip aesthetic — Phosphor icons,
// FlatButton primitive, Theme tokens. Detach UI dropped per
// product direction; viewport stays embedded.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Qcv

Pane {
    id: root
    padding: 0

    property bool colorPanelVisible: false
    property bool notesPanelVisible: false
    signal toggleColorPanel()
    signal toggleNotesPanel()
    signal toggleFullscreenRequested()
    signal openVideoRequested()
    // Right-rail / left-rail section reveals — Main.qml expands
    // the named section + force-opens the rail.
    signal showRightRailSection(string name)
    signal showLeftRailSection(string name)

    background: Rectangle {
        color: Theme.toolbar
        Rectangle {
            anchors.left:   parent.left
            anchors.right:  parent.right
            anchors.bottom: parent.bottom
            height: Theme.dividerWidth
            color: Theme.divider
        }
    }

    // ---- Helpers --------------------------------------------------
    readonly property bool hasMedia:
        WindowManager.dualController
        || WindowManager.imageSeqActive
        || WindowManager.audioActive
        || (WindowManager.videoDecoder
            && WindowManager.videoDecoder.state !== 0)

    readonly property bool isPlaying: {
        if (WindowManager.dualController) {
            return WindowManager.dualController.isPlaying;
        }
        const t = WindowManager.timeline
                  ? WindowManager.timeline.timer : null;
        if (WindowManager.imageSeqActive) return t ? t.playing : false;
        // Audio-only: timer drives the playhead, AudioPlayer is
        // the audible side. Either one being "playing" implies
        // the other is too (transport branches drive both
        // together) — read the timer for the same WallClock
        // semantics as image-seq.
        if (WindowManager.audioActive)    return t ? t.playing : false;
        return WindowManager.videoDecoder
               && WindowManager.videoDecoder.isPlaying;
    }

    readonly property bool inOutSet: WindowManager.inPoint >= 0
                                  || WindowManager.outPoint >= 0

    // Playlist mode: loop in/out is non-functional in playlist mode
    // (the timer-level wrap is disabled because the orchestrator
    // handles playlist-level wrap instead). Grey out the In/Out
    // controls + hide the Clear button so users aren't misled into
    // thinking they can set loop bounds inside a playlist.
    readonly property bool inPlaylist:
        WindowManager.timeline
        && WindowManager.timeline.sourceMode === 1

    RowLayout {
        anchors.fill: parent
        // Flush left/right — the row sits flat top-to-bottom in
        // the bottom band, so the buttons run all the way to the
        // panel edges with no inset.
        anchors.leftMargin:  0
        anchors.rightMargin: 0
        // Tiny inter-button gap so filled buttons (the danger ✕
        // clear, the accent-checked In/Out) don't visually merge
        // with their neighbors.
        spacing: Theme.spacingTight

        // ---- Open (left edge) -----------------------------------
        // The destructive close-media (✕) button used to live here
        // but was removed — closing media is reachable via File menu
        // and shouldn't sit one accidental click away in the
        // transport row.
        FlatButton {
            iconName: "folder-simple"
            tooltipText: qsTr("Open media…")
            onClicked: root.openVideoRequested()
        }

        // Vertical separator before the transport cluster.
        Rectangle {
            Layout.preferredWidth: 1
            Layout.fillHeight: true
            Layout.topMargin: Theme.padding
            Layout.bottomMargin: Theme.padding
            Layout.leftMargin: Theme.spacingLoose
            Layout.rightMargin: Theme.spacingLoose
            color: Theme.divider
        }

        // ---- Transport cluster -----------------------------------
        // Playlist clip-step (prev) — visible only in playlist mode.
        // Jumps to the in-point of the previous clip on the timeline
        // (or to the current clip's in-point first if mid-clip).
        FlatButton {
            iconName: "arrow-line-left"
            visible: WindowManager.timeline
                     && WindowManager.timeline.sourceMode === 1
            enabled: root.hasMedia
            tooltipText: qsTr("Previous clip")
            onClicked: WindowManager.seekToPrevClipStart()
        }
        FlatButton {
            iconName: "skip-back"
            enabled: root.hasMedia
            tooltipText: qsTr("Skip to start")
            onClicked: WindowManager.seekToStart()
        }
        // Press-and-hold rewind: ramps 2× → 32× per second.
        FlatButton {
            iconName: "rewind"
            enabled: root.hasMedia
            tooltipText: qsTr("Rewind (hold)")
            onPressedChanged: {
                if (pressed) WindowManager.startFastSeek(-1);
                else         WindowManager.stopFastSeek();
            }
            onHoveredChanged: if (!hovered && pressed) WindowManager.stopFastSeek()
        }
        FlatButton {
            iconName: "caret-left"
            enabled: root.hasMedia
            tooltipText: qsTr("Step back 1 frame")
            onClicked: WindowManager.stepFrames(-1)
        }
        FlatButton {
            iconName: root.isPlaying ? "pause" : "play"
            iconColor: Theme.textBright
            enabled: root.hasMedia
            tooltipText: root.isPlaying ? qsTr("Pause") : qsTr("Play")
            onClicked: WindowManager.togglePlayback()
        }
        FlatButton {
            iconName: "caret-right"
            enabled: root.hasMedia
            tooltipText: qsTr("Step forward 1 frame")
            onClicked: WindowManager.stepFrames(1)
        }
        FlatButton {
            iconName: "fast-forward"
            enabled: root.hasMedia
            tooltipText: qsTr("Fast-forward (hold)")
            onPressedChanged: {
                if (pressed) WindowManager.startFastSeek(1);
                else         WindowManager.stopFastSeek();
            }
            onHoveredChanged: if (!hovered && pressed) WindowManager.stopFastSeek()
        }
        FlatButton {
            iconName: "skip-forward"
            enabled: root.hasMedia
            tooltipText: qsTr("Skip to end")
            onClicked: WindowManager.seekToEnd()
        }
        // Playlist clip-step (next) — visible only in playlist mode.
        // Jumps to the in-point of the next clip on the timeline.
        FlatButton {
            iconName: "arrow-line-right"
            visible: WindowManager.timeline
                     && WindowManager.timeline.sourceMode === 1
            enabled: root.hasMedia
            tooltipText: qsTr("Next clip")
            onClicked: WindowManager.seekToNextClipStart()
        }

        // Review-speed readout — visible only when playback rate is
        // not 1x (R cycles presets, Shift+R resets). Click to cycle,
        // matching the key. Recessed readout chip (shared slider-
        // readout treatment); accent text keeps the "off-1x" alert.
        Rectangle {
            visible: Math.abs(WindowManager.reviewSpeed - 1.0) > 0.001
            Layout.preferredWidth: speedText.implicitWidth + 12
            Layout.preferredHeight: 16
            Layout.leftMargin: Theme.spacingTight
            radius: Theme.radiusSmall
            color: speedMa.containsMouse
                   ? Theme.surfaceHover : Theme.surfaceRecess
            Text {
                id: speedText
                anchors.centerIn: parent
                text: WindowManager.reviewSpeed.toFixed(2)
                          .replace(/\.?0+$/, "") + "×"
                color: Theme.accent
                font.family: Theme.monoFamily
                font.pixelSize: Theme.fontSizeMono
                font.bold: true
            }
            MouseArea {
                id: speedMa
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: WindowManager.cycleReviewSpeed()
                FlatToolTip {
                    visible: speedMa.containsMouse
                    text: qsTr("Review speed — click to cycle (R)")
                }
            }
        }

        // Vertical separator after the transport cluster.
        Rectangle {
            Layout.preferredWidth: 1
            Layout.fillHeight: true
            Layout.topMargin: Theme.padding
            Layout.bottomMargin: Theme.padding
            Layout.leftMargin: Theme.spacingLoose
            Layout.rightMargin: Theme.spacingLoose
            color: Theme.divider
        }

        // ---- In / Out / Clear ------------------------------------
        FlatButton {
            iconName: "sign-in"
            enabled: root.hasMedia && !root.inPlaylist
            checkable: true
            checked: WindowManager.inPoint >= 0 && !root.inPlaylist
            tooltipText: root.inPlaylist
                ? qsTr("In point not available in playlist mode")
                : qsTr("Set in point (I)")
            onClicked: {
                if (WindowManager.inPoint >= 0
                    && WindowManager.inPoint === WindowManager.currentFrameUnified()) {
                    WindowManager.clearInOutPoints();
                } else {
                    WindowManager.setInPointAtCurrent();
                }
            }
        }
        FlatButton {
            iconName: "sign-out"
            enabled: root.hasMedia && !root.inPlaylist
            checkable: true
            checked: WindowManager.outPoint >= 0 && !root.inPlaylist
            tooltipText: root.inPlaylist
                ? qsTr("Out point not available in playlist mode")
                : qsTr("Set out point (O)")
            onClicked: {
                if (WindowManager.outPoint >= 0
                    && WindowManager.outPoint === WindowManager.currentFrameUnified()) {
                    WindowManager.clearInOutPoints();
                } else {
                    WindowManager.setOutPointAtCurrent();
                }
            }
        }
        FlatButton {
            iconName: "x-circle"
            variant: "danger"
            visible: root.inOutSet && !root.inPlaylist
            tooltipText: qsTr("Clear in/out points (Shift+I)")
            onClicked: WindowManager.clearInOutPoints()
        }

        // Vertical separator after the in/out cluster.
        Rectangle {
            Layout.preferredWidth: 1
            Layout.fillHeight: true
            Layout.leftMargin: Theme.spacingLoose
            Layout.rightMargin: Theme.spacingLoose
            color: Theme.divider
        }

        // ---- Loop / Fullscreen -----------------------------------
        FlatButton {
            iconName: "repeat"
            checkable: true
            checked: WindowManager.loopEnabled
            tooltipText: qsTr("Loop (L)")
            onClicked: WindowManager.loopEnabled = !WindowManager.loopEnabled
        }
        FlatButton {
            iconName: "frame-corners"
            tooltipText: qsTr("Fullscreen (F)")
            onClicked: root.toggleFullscreenRequested()
        }

        // Vertical separator after the fullscreen button.
        Rectangle {
            Layout.preferredWidth: 1
            Layout.fillHeight: true
            Layout.leftMargin: Theme.spacingLoose
            Layout.rightMargin: Theme.spacingLoose
            color: Theme.divider
        }

        // ---- Screenshots -----------------------------------------
        FlatButton {
            iconName: "file-image"
            enabled: root.hasMedia
            tooltipText: qsTr("Screenshot to clipboard (⌥T)")
            onClicked: WindowManager.screenshotToClipboard()
        }
        FlatButton {
            iconName: "file-arrow-down"
            enabled: root.hasMedia
            tooltipText: qsTr("Screenshot to Desktop (T)")
            onClicked: WindowManager.screenshotToFile()
        }

        // ---- Section reveals (left rail) -------------------------
        FlatButton {
            iconName: "rectangle-dashed"
            tooltipText: qsTr("Safety guides")
            onClicked: root.showLeftRailSection("safety")
        }
        FlatButton {
            iconName: "checkerboard"
            tooltipText: qsTr("Background")
            onClicked: root.showLeftRailSection("background")
        }
        // Timeline hover thumbnails toggle — accent fill when on.
        // Mirrors the Settings panel toggle via the Q_PROPERTY on
        // WindowManager, so flipping either side updates the other
        // live without a relaunch.
        FlatButton {
            iconName: "image-square"
            checkable: true
            checked: WindowManager.timelineHoverThumbsEnabled
            tooltipText: checked
                         ? qsTr("Timeline hover thumbnails (on)")
                         : qsTr("Timeline hover thumbnails (off)")
            // Read fresh from the property, not the (already auto-
            // toggled by Qt's Button) local `checked` — same pattern
            // as the Loop button above.
            onClicked: WindowManager.timelineHoverThumbsEnabled =
                       !WindowManager.timelineHoverThumbsEnabled
        }

        // Vertical separator after the background button.
        Rectangle {
            Layout.preferredWidth: 1
            Layout.fillHeight: true
            Layout.leftMargin: Theme.spacingLoose
            Layout.rightMargin: Theme.spacingLoose
            color: Theme.divider
        }

        // ---- Color / Notes panel toggles -------------------------
        FlatButton {
            iconName: "palette"
            checkable: true
            checked: root.colorPanelVisible
            tooltipText: qsTr("Color panel (⌃3)")
            onClicked: root.toggleColorPanel()
        }
        FlatButton {
            iconName: "note-pencil"
            checkable: true
            enabled: WindowManager.annotationsAllowed
            checked: root.notesPanelVisible
            tooltipText: qsTr("Notes panel (⌃4)")
            onClicked: root.toggleNotesPanel()
        }

        // Vertical separator after the notes button.
        Rectangle {
            Layout.preferredWidth: 1
            Layout.fillHeight: true
            Layout.leftMargin: Theme.spacingLoose
            Layout.rightMargin: Theme.spacingLoose
            color: Theme.divider
        }

        // ---- Audio -----------------------------------------------
        FlatButton {
            id: muteBtn
            // When muted: red-fill "danger" variant matches the rest
            // of the destructive / alert iconography (clear in/out
            // ✕). Untoggled returns to the default neutral look.
            readonly property bool isMuted:
                WindowManager.audio && WindowManager.audio.muted
            iconName: isMuted ? "speaker-x" : "speaker-high"
            variant: isMuted ? "danger" : "default"
            enabled: WindowManager.audio && WindowManager.audio.hasAudio
            tooltipText: isMuted ? qsTr("Unmute (M)") : qsTr("Mute (M)")
            onClicked: {
                if (WindowManager.audio) {
                    WindowManager.audio.muted = !WindowManager.audio.muted;
                }
            }
        }
        FlatSlider {
            id: volSlider
            Layout.preferredWidth: 90
            Layout.preferredHeight: Theme.toolStripHeight
            Layout.leftMargin: Theme.paddingTight
            Layout.rightMargin: Theme.paddingTight
            from: 0.0
            to: 1.0
            value: WindowManager.audio ? WindowManager.audio.volume : 1.0
            enabled: WindowManager.audio && WindowManager.audio.hasAudio
                     && !(WindowManager.audio && WindowManager.audio.muted)
            onMoved: WindowManager.audio.volume = value
            FlatToolTip {
                visible: volSlider.hovered
                text: qsTr("Volume — %1%")
                          .arg(Math.round((WindowManager.audio
                              ? WindowManager.audio.volume : 1.0) * 100))
            }
        }

        // Push-to-right spacer.
        Item { Layout.fillWidth: true }
    }
}
