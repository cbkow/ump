// ViewportOverlay — Phase 2.4. The "source bar."
//
// Always-visible thin strip at the top of the center stage, above the
// player child window. Hosts ONLY single/dual view controls per
// Guide 16's design refinement: Source A indicator, Source B
// indicator, compositor mode toggles (Single / SBS / Split-Wipe),
// and the split-position slider.
//
// Source A is the current videoDecoder (Phase 1.8.x). Source B is
// presently the static test pattern; the chip says "(test pattern)"
// until the dual-source phase lands a second VideoDecoder + load-
// into-B path. Compositor mode toggles work today against the static
// Source B — useful for verifying the rendering path.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Qcv
import Qcv.Render

Pane {
    id: root
    padding: 0

    background: Rectangle {
        color: Theme.toolbar
        Rectangle {
            anchors.left:   parent.left
            anchors.right:  parent.right
            anchors.bottom: parent.bottom
            height: Theme.dividerWidth
            color:  Theme.divider
        }
    }

    // Phase 7.8 Stage F — inline "Save Dual View" mode. When true, the
    // mode-toggle buttons + split slider hide and a name field + Save /
    // Cancel buttons take their slot. Avoids a popup entirely (popups
    // anchored to this overlay would extend into the native player
    // QWindow region and get occluded).
    property bool saveMode: false

    // Rail open buttons — shown in this bar's left/right gutters ONLY
    // when the corresponding rail is fully closed (rails close to 0
    // width; the rail header owns the close button while open). Main.qml
    // binds the closed state and handles the open signals. Placing the
    // open affordance here keeps it where the rail edge used to be.
    property bool leftRailClosed:  false
    property bool rightRailClosed: false
    signal openLeftRail()
    signal openRightRail()

    // Phase 3.H.4 — playlist mode hides everything except the A chip.
    // Side-by-side / Split-Wipe / Save Dual View / B-source aren't
    // meaningful for a sequential playlist.
    readonly property bool playlistActive:
        WindowManager.timeline
        && WindowManager.timeline.sourceMode === 1

    function autoSaveName() {
        const proj = WindowManager.project;
        if (!proj) return qsTr("Untitled Dual View");
        const aName = proj.activeItem
                      ? (proj.activeItem.name || qsTr("(none)"))
                      : qsTr("(none)");
        const bName = proj.bSourceName || qsTr("(none)");
        return aName + " vs " + bName;
    }

    function enterSaveMode() {
        saveNameField.text = autoSaveName();
        root.saveMode = true;
        saveNameField.selectAll();
        saveNameField.forceActiveFocus();
    }

    function commitSave() {
        const name = saveNameField.text.trim();
        if (name.length === 0) return;
        const id = WindowManager.saveCurrentDualView(name);
        if (id && id.length > 0) {
            console.log("DualView saved:", id);
            root.saveMode = false;
        } else {
            console.warn("DualView save failed");
        }
    }

    // True when the live dual session was loaded from (or already
    // saved to) a DualPair item — drives the "Update" affordance.
    readonly property bool isSavedDualView:
        WindowManager.activeDualViewId
        && WindowManager.activeDualViewId.length > 0

    // Overwrite the bound saved view in place (keeps its name). No
    // name prompt — that's the whole point of Update vs Save As New.
    function commitUpdate() {
        if (!root.isSavedDualView) return;
        if (WindowManager.updateDualView(WindowManager.activeDualViewId))
            console.log("DualView updated:",
                        WindowManager.activeDualViewId);
        else
            console.warn("DualView update failed");
    }

    // A/B side-marker color — matches Track A / B accentBorder
    // colors in TimelinePanel so the chip identifier reads as the
    // same source-color across the inspector, the dual-view bar,
    // and the timeline track.
    readonly property color sideAColor: "#446a90"
    readonly property color sideBColor: "#a0664a"

    RowLayout {
        anchors.fill: parent
        // The open buttons sit in the gutterWidth gutters; when one is
        // showing, add a little breathing room so its inner divider
        // isn't flush against the neighbour (A chip / mode toggles).
        anchors.leftMargin:  root.leftRailClosed
            ? Theme.gutterWidth + Theme.spacing
            : Theme.gutterWidth
        anchors.rightMargin: root.rightRailClosed
            ? Theme.gutterWidth + Theme.spacing
            : Theme.padding
        // Toolstrip flow — chips and buttons butt up. Tiny gap so
        // filled buttons (danger / primary save form) don't visually
        // merge with their neighbors.
        spacing: Theme.spacingTight

        // ---- Source slot indicators
        Rectangle {
            id: aChip
            Layout.preferredWidth: 240
            Layout.preferredHeight: 24
            readonly property bool aLoaded:
                (WindowManager.project
                 && WindowManager.project.activeItemId
                 && WindowManager.project.activeItemId.length > 0)
                || (WindowManager.videoDecoder
                    && WindowManager.videoDecoder.sourcePath
                    && WindowManager.videoDecoder.sourcePath.length > 0)
                || WindowManager.imageSeqActive
            // Flat chip — the colored "A" letter inside is the
            // source-marker. Drag-over uses the standard 2-px
            // Theme.accent border via the overlay below (matches
            // the Media bin + PlayerWindow viewport pattern);
            // hover lifts to surfaceHover.
            color: aChipMa.containsMouse
                   ? Theme.surfaceHover : "transparent"
            radius: 0

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.spacing
                anchors.rightMargin: Theme.spacing
                spacing: Theme.spacing

                Text {
                    text: qsTr("A")
                    color: root.sideAColor
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontSizeSmall
                    font.bold: true
                }
                Text {
                    Layout.fillWidth: true
                    text: {
                        if (WindowManager.project
                            && WindowManager.project.activeItem
                            && WindowManager.project.activeItem.name) {
                            return WindowManager.project.activeItem.name;
                        }
                        if (WindowManager.videoDecoder
                            && WindowManager.videoDecoder.sourcePath) {
                            return sourceFilenameOf(
                                WindowManager.videoDecoder.sourcePath);
                        }
                        return qsTr("(drop a file…)");
                    }
                    color: aChip.aLoaded ? Theme.textPrimary : Theme.textMuted
                    font.italic: !aChip.aLoaded
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontSizeSmall
                    elide: Text.ElideMiddle
                }

                // Per-side mute (A). Phosphor speaker icon.
                Item {
                    visible: WindowManager.compositorMode !== 0
                             && WindowManager.dualController
                             && WindowManager.dualController.audio
                             && WindowManager.dualController.audio.hasAudioA
                    Layout.preferredWidth: 18
                    Layout.preferredHeight: 18
                    Icon {
                        anchors.centerIn: parent
                        name: (WindowManager.dualController
                               && WindowManager.dualController.audio
                               && WindowManager.dualController.audio.mutedA)
                              ? "speaker-x" : "speaker-high"
                        size: Theme.iconSizeSmall
                        color: aMuteMa.containsMouse
                               ? Theme.textBright : Theme.textPrimary
                    }
                    MouseArea {
                        id: aMuteMa
                        anchors.fill: parent
                        anchors.margins: -2
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            const a = WindowManager.dualController
                                      && WindowManager.dualController.audio;
                            if (a) a.mutedA = !a.mutedA;
                        }
                        FlatToolTip {
                            visible: aMuteMa.containsMouse
                            text: qsTr("Mute / unmute A audio")
                        }
                    }
                }
            }

            // Drop zone — accepts any media type ProjectManager
            // recognizes (video, image, image sequence).
            DropArea {
                id: aDrop
                anchors.fill: parent
                onEntered: (drag) => {
                    console.log("aDrop: entered, hasUrls=", drag.hasUrls);
                }
                onDropped: (drop) => {
                    console.log("aDrop: dropped, hasUrls=", drop.hasUrls,
                                "project=", !!WindowManager.project);
                    if (!drop.hasUrls || !WindowManager.project) return;
                    const u = WindowManager.urlToOsPath(drop.urls[0]);
                    if (!u) return;
                    console.log("aDrop: addMediaFile(", u, ")");
                    const id = WindowManager.project.addMediaFile(u);
                    console.log("aDrop: -> id=", id);
                    if (id) WindowManager.project.setActiveItem(id);
                    drop.accept();
                }
            }
            // Hover-only — drag is the supported load path. The
            // bin/Open-Video menu still works when the user wants
            // a file dialog.
            MouseArea {
                id: aChipMa
                anchors.fill: parent
                hoverEnabled: true
                acceptedButtons: Qt.NoButton
            }

            // Drop highlight overlay. Drawn ON TOP of the chip's
            // RowLayout so the 2-px accent border isn't covered
            // by the inner text/icons. Matches the Media bin
            // pattern in LeftRail.qml.
            Rectangle {
                anchors.fill: parent
                color: "transparent"
                border.color: Theme.accent
                border.width: 2
                visible: aDrop.containsDrag
                z: 10
            }
        }

        // Small gap between the A and B chips so they read as
        // distinct slots rather than a single continuous bar.
        Item { width: Theme.spacingLoose }

        Rectangle {
            id: bChip
            visible: !root.playlistActive
            Layout.preferredWidth: visible ? 240 : 0
            Layout.preferredHeight: 24
            readonly property bool bLoaded:
                (WindowManager.project
                 && WindowManager.project.bSourcePath
                 && WindowManager.project.bSourcePath.length > 0)
                || (WindowManager.videoDecoderB
                    && WindowManager.videoDecoderB.sourcePath
                    && WindowManager.videoDecoderB.sourcePath.length > 0)
            readonly property string bDisplayPath:
                (WindowManager.project && WindowManager.project.bSourcePath)
                ? WindowManager.project.bSourcePath
                : (WindowManager.videoDecoderB
                   ? WindowManager.videoDecoderB.sourcePath
                   : "")
            // Flat chip — the colored "B" letter inside is the
            // source-marker. Drag-over uses the standard 2-px
            // Theme.accent border via the overlay below; hover
            // lifts to surfaceHover. Matches A chip + Media bin.
            color: bChipMa.containsMouse
                   ? Theme.surfaceHover : "transparent"
            radius: 0

            // Drop zone — currently video-only. Source B
            // image-sequence support is a separate piece of work
            // (its own cache, pacing, playhead). Filter by ext so
            // EXR/PNG/etc. don't get fed into VideoDecoder where
            // ffmpeg's image decoders crash on multi-layer files.
            // The renderer's hybrid mode works the other way:
            // image-seq on A + video on B is fine.
            // Phase 7.7 Stage 5 — B drop accepts the same content
            // types as A (video + image sequences). Routes through
            // WindowManager.setBSource which adds to the project pool
            // (dedupe-by-path) and updates Project::bSource.
            DropArea {
                id: bDrop
                anchors.fill: parent
                onDropped: (drop) => {
                    if (!drop.hasUrls) return;
                    const u = WindowManager.urlToOsPath(drop.urls[0]);
                    if (!u) return;
                    if (!WindowManager.setBSource(u)) {
                        console.warn("ViewportOverlay: setBSource failed:", u);
                    }
                    drop.accept();
                }
            }

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.spacing
                anchors.rightMargin: Theme.spacing
                spacing: Theme.spacing

                Text {
                    text: qsTr("B")
                    color: root.sideBColor
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontSizeSmall
                    font.bold: true
                }
                Text {
                    Layout.fillWidth: true
                    text: bChip.bLoaded
                          ? sourceFilenameOf(bChip.bDisplayPath)
                          : qsTr("(drop a video or sequence…)")
                    color: bChip.bLoaded ? Theme.textPrimary : Theme.textMuted
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontSizeSmall
                    font.italic: !bChip.bLoaded
                    elide: Text.ElideMiddle
                }

                // Per-side mute (B).
                Item {
                    visible: WindowManager.compositorMode !== 0
                             && WindowManager.dualController
                             && WindowManager.dualController.audio
                             && WindowManager.dualController.audio.hasAudioB
                    Layout.preferredWidth: 18
                    Layout.preferredHeight: 18
                    Icon {
                        anchors.centerIn: parent
                        name: (WindowManager.dualController
                               && WindowManager.dualController.audio
                               && WindowManager.dualController.audio.mutedB)
                              ? "speaker-x" : "speaker-high"
                        size: Theme.iconSizeSmall
                        color: bMuteMa.containsMouse
                               ? Theme.textBright : Theme.textPrimary
                    }
                    MouseArea {
                        id: bMuteMa
                        anchors.fill: parent
                        anchors.margins: -2
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            const a = WindowManager.dualController
                                      && WindowManager.dualController.audio;
                            if (a) a.mutedB = !a.mutedB;
                        }
                        FlatToolTip {
                            visible: bMuteMa.containsMouse
                            text: qsTr("Mute / unmute B audio")
                        }
                    }
                }

                // Close — visible only when B is loaded. Phosphor x.
                Item {
                    visible: bChip.bLoaded
                    Layout.preferredWidth: 16
                    Layout.preferredHeight: 16
                    Icon {
                        anchors.centerIn: parent
                        name: "x"
                        size: Theme.iconSizeSmall
                        color: bCloseMa.containsMouse
                               ? Theme.error : Theme.textMuted
                    }
                    MouseArea {
                        id: bCloseMa
                        anchors.fill: parent
                        anchors.margins: -2
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            WindowManager.clearBSource();
                            WindowManager.closeSourceB();
                        }
                        FlatToolTip {
                            visible: bCloseMa.containsMouse
                            text: qsTr("Close source B")
                        }
                    }
                }
            }
            MouseArea {
                id: bChipMa
                anchors.fill: parent
                hoverEnabled: true
                acceptedButtons: Qt.NoButton
            }

            // Drop highlight overlay — same pattern as A chip.
            Rectangle {
                anchors.fill: parent
                color: "transparent"
                border.color: Theme.accent
                border.width: 2
                visible: bDrop.containsDrag
                z: 10
            }
        }

        Item { Layout.fillWidth: true }

        // ---- Compositor mode toggles — icon-only, checkable.
        FlatButton {
            visible: !root.saveMode && !root.playlistActive
            iconName: "square"
            tooltipText: qsTr("Single view")
            checkable: true
            checked: WindowManager.compositorMode === 0
            onClicked: WindowManager.compositorMode = 0
        }
        FlatButton {
            visible: !root.saveMode && !root.playlistActive
            iconName: "square-split-horizontal"
            tooltipText: qsTr("Side-by-side")
            checkable: true
            checked: WindowManager.compositorMode === 1
            onClicked: WindowManager.compositorMode = 1
        }
        FlatButton {
            visible: !root.saveMode && !root.playlistActive
            iconName: "split-horizontal"
            tooltipText: qsTr("Split wipe")
            checkable: true
            checked: WindowManager.compositorMode === 2
            onClicked: WindowManager.compositorMode = 2
        }
        FlatButton {
            visible: !root.saveMode && !root.playlistActive
            iconName: "exclude"
            tooltipText: qsTr("Difference")
            checkable: true
            checked: WindowManager.compositorMode === 3
            onClicked: WindowManager.compositorMode = 3
        }

        // "Update" — overwrite the saved Dual View the session is
        // bound to, in place (keeps its name). Only shown when we're
        // actually in a saved view; otherwise the single Save button
        // below is the entry point. Primary variant so it reads as
        // the main action while editing an existing saved view.
        FlatButton {
            id: updateDualBtn
            visible: !root.saveMode && !root.playlistActive
                     && WindowManager.compositorMode !== 0
                     && WindowManager.dualController
                     && root.isSavedDualView
            iconName: "floppy-disk"
            tooltipText: qsTr("Update this saved dual view")
            // Subtle grey fill rather than accent-blue: in this row
            // blue is reserved for dual-view state (the active mode
            // toggle). Update is an action, not a state.
            variant: "subtle"
            onClicked: root.commitUpdate()
        }

        // "Save as Dual View" / "Save As New" — visible whenever dual
        // mode is active AND we're not already in saveMode. Click
        // swaps the bar contents to the inline name-field + Save /
        // Cancel form below. When the session is already a saved
        // view, this becomes the "save a separate copy" action (and
        // the Update button to its left handles overwrite).
        FlatButton {
            id: saveDualBtn
            visible: !root.saveMode && !root.playlistActive
                     && WindowManager.compositorMode !== 0
                     && WindowManager.dualController
            iconName: root.isSavedDualView ? "file-plus" : "floppy-disk"
            tooltipText: root.isSavedDualView ? qsTr("Save as a new dual view")
                                              : qsTr("Save dual view")
            onClicked: root.enterSaveMode()
        }

        // Phase 7.5 B.6.6: annotation + safety controls moved to the
        // right rail (RightRail.qml). The native QWindow player sits
        // above the UI's centerStage in z-order, so any popup
        // (ComboBox, etc.) on this overlay would be occluded by the
        // player surface.

        // ---- Split slider (Split-Wipe only).
        FlatSlider {
            visible: !root.saveMode && !root.playlistActive
                     && WindowManager.compositorMode === 2
            Layout.preferredWidth: 160
            Layout.preferredHeight: 26
            from: 0
            to: 1
            value: WindowManager.splitPos
            onValueChanged: WindowManager.splitPos = value
        }
        Text {
            visible: !root.saveMode && !root.playlistActive
                     && WindowManager.compositorMode === 2
            text: WindowManager.splitPos.toFixed(2)
            color: Theme.textSecondary
            font.family: Theme.monoFamily
            font.pixelSize: Theme.fontSizeTiny
            Layout.preferredWidth: 36
        }

        // ---- Gain slider (Difference only) — shares the slot with the
        // split slider; the two modes are mutually exclusive. 1.0 = raw
        // Adobe-style abs(A−B); higher amplifies subtle diffs (stays
        // black where aligned).
        FlatSlider {
            visible: !root.saveMode && !root.playlistActive
                     && WindowManager.compositorMode === 3
            Layout.preferredWidth: 160
            Layout.preferredHeight: 26
            from: 1
            to: 16
            value: WindowManager.diffGain
            onValueChanged: WindowManager.diffGain = value
        }
        Text {
            visible: !root.saveMode && !root.playlistActive
                     && WindowManager.compositorMode === 3
            text: "×" + WindowManager.diffGain.toFixed(1)
            color: Theme.textSecondary
            font.family: Theme.monoFamily
            font.pixelSize: Theme.fontSizeTiny
            Layout.preferredWidth: 36
        }

        // ---- Inline Save Dual View form (visible only in saveMode)
        Text {
            visible: root.saveMode
            text: qsTr("Name:")
            color: Theme.textPrimary
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontSizeSmall
        }
        FlatTextField {
            id: saveNameField
            visible: root.saveMode
            Layout.fillWidth: true
            // Match the dual-view bar's 26-px button height —
            // FlatTextField's default implicitHeight is 22.
            Layout.preferredHeight: 26
            placeholderText: qsTr("e.g. ‘US30 v001 vs v002’")
            onAccepted: root.commitSave()
            Keys.onEscapePressed: root.saveMode = false
        }
        FlatButton {
            visible: root.saveMode
            iconName: "check"
            tooltipText: qsTr("Save")
            variant: "primary"
            enabled: saveNameField.text.trim().length > 0
            onClicked: root.commitSave()
        }
        FlatButton {
            visible: root.saveMode
            iconName: "x"
            tooltipText: qsTr("Cancel")
            variant: "danger"
            onClicked: root.saveMode = false
        }
    }

    // ---- Rail open buttons, in the bar's gutters ----------------------
    // Visible only when the matching rail is closed. The arrow points
    // in the direction the rail expands (left rail opens rightward, right
    // rail opens leftward) — single arrows, mirroring the rail header's
    // close arrows. They sit exactly where the rail edge was, so opening
    // reads as the rail sliding back into the same position.
    // Rail open buttons sit in the bar's gutters; the divider on the
    // inner edge matches the border the rail used to show against the
    // viewport.
    Item {
        visible: root.leftRailClosed
        z: 5
        anchors.left:   parent.left
        anchors.top:    parent.top
        anchors.bottom: parent.bottom
        width: Theme.gutterWidth
        Rectangle {
            anchors.fill: parent
            color: leftOpenMa.containsMouse ? Theme.affordanceHover
                                            : Theme.affordanceIdle
        }
        Rectangle {
            anchors.right:  parent.right
            anchors.top:    parent.top
            anchors.bottom: parent.bottom
            width: Theme.dividerWidth
            color: Theme.divider
        }
        Icon {
            anchors.centerIn: parent
            name: "arrow-right"
            size: Theme.iconSizeSmall
            color: leftOpenMa.containsMouse ? Theme.textPrimary : Theme.textSecondary
        }
        MouseArea {
            id: leftOpenMa
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            hoverEnabled: true
            onClicked: root.openLeftRail()
            FlatToolTip {
                visible: leftOpenMa.containsMouse
                text: qsTr("Show project panel")
            }
        }
    }
    Item {
        visible: root.rightRailClosed
        z: 5
        anchors.right:  parent.right
        anchors.top:    parent.top
        anchors.bottom: parent.bottom
        width: Theme.gutterWidth
        Rectangle {
            anchors.fill: parent
            color: rightOpenMa.containsMouse ? Theme.affordanceHover
                                             : Theme.affordanceIdle
        }
        Rectangle {
            anchors.left:   parent.left
            anchors.top:    parent.top
            anchors.bottom: parent.bottom
            width: Theme.dividerWidth
            color: Theme.divider
        }
        Icon {
            anchors.centerIn: parent
            name: "arrow-left"
            size: Theme.iconSizeSmall
            color: rightOpenMa.containsMouse ? Theme.textPrimary : Theme.textSecondary
        }
        MouseArea {
            id: rightOpenMa
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            hoverEnabled: true
            onClicked: root.openRightRail()
            FlatToolTip {
                visible: rightOpenMa.containsMouse
                text: qsTr("Show inspector panel")
            }
        }
    }

    function sourceFilenameOf(path) {
        const idx = Math.max(path.lastIndexOf("/"), path.lastIndexOf("\\"));
        return idx >= 0 ? path.substring(idx + 1) : path;
    }

    // If the user leaves dual mode while in saveMode, snap back to
    // the normal toggle bar so they don't end up with an inline form
    // that has no dual session to save.
    Connections {
        target: WindowManager
        function onCompositorModeChanged() {
            if (WindowManager.compositorMode === 0 && root.saveMode) {
                root.saveMode = false;
            }
        }
    }
}
