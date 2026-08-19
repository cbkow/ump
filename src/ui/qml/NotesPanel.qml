// NotesPanel — Phase 3.H.6 Stage B + polish.
//
// Three-zone layout:
//   ┌────────┬───────────────────────────────────┬─────────────┐
//   │ +Add   │  filmstrip (horizontal scroll)    │  Tools      │
//   │ sticky │  card / card / card / ...         │  sticky     │
//   └────────┴───────────────────────────────────┴─────────────┘
//
// The "+ Add note here" tile and the Annotation Tools panel are
// pinned to the panel's left and right edges; only the cards in
// the middle scroll horizontally.
//
// Cards = thumbnail + timecode/frame + multi-line text +
// addressed checkbox + delete. Sorted by timecode.

import QtCore
import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import Qcv

Pane {
    id: root
    padding: 0

    background: Rectangle {
        color: Theme.bgAlt
        Rectangle {
            anchors.left:  parent.left
            anchors.right: parent.right
            anchors.top:   parent.top
            height: 1
            color:  Theme.divider
        }
    }

    readonly property int kCardWidth:    240
    readonly property int kThumbHeight:  135
    readonly property int kCardPadding:  8
    readonly property int kAddTileWidth: 160
    readonly property int kToolsWidth:   46     // vertical icon column
    readonly property int kPickerWidth:  186
    readonly property int kExportWidth:  220

    // Phase 3.H.6 Stage D — toggled by the Save button at the
    // bottom of the tools column. Auto-clears after a successful
    // export.
    property bool exportPanelVisible: false

    // ---- Annotation tools state ---------------------------------
    // The active tool (0 = None, 1..5 = Freehand/Rectangle/Oval/
    // Arrow/Line). Selecting a tool auto-enters annotation mode;
    // clicking the active tool again deselects (sets None) and
    // exits annotation mode so pointer events fall back to the
    // scrub / playback path.
    property int    activeTool:    0
    property color  drawingColor:  Qt.rgba(1.0, 0.0, 0.0, 1.0)
    property real   strokeWidth:   6

    // When the panel is closed externally (Ctrl+5 / transport
    // toggle / fullscreen), drop the selected tool so the next
    // open is in a clean "no tool" state — keeps the QML
    // ToolBtn highlight aligned with the C++ side, which exits
    // annotation mode whenever the panel hides.
    //
    // Opening the panel arms Freehand right away — the panel's
    // whole point is drawing, so the first drag should ink
    // without a trip to the tool column. (Panel visibility is
    // gated on annotationsAllowed, so arming here can't fight
    // the C++ gate.) Guarded on "no tool yet" so a same-frame
    // visibility bounce can't toggle an armed tool back off.
    onVisibleChanged: {
        if (!visible && root.activeTool !== 0) {
            root.activeTool = 0;
        } else if (visible && root.activeTool === 0) {
            root.applyTool(1);   // Freehand
        }
    }

    function applyTool(t) {
        if (t === 0 || root.activeTool === t) {
            // Deselect tool (toolId 0) OR click-same-tool-deselects:
            // exit annotation mode. The deselect button always
            // lands here; other tools toggle off only on a second
            // click.
            root.activeTool = 0;
            WindowManager.setAnnotationTool(0);
            WindowManager.setAnnotationActive(false);
            return;
        }
        root.activeTool = t;
        WindowManager.setAnnotationTool(t);
        WindowManager.setAnnotationActive(true);
    }
    function applyColor(c) {
        root.drawingColor = c;
        WindowManager.setAnnotationColor(c.r, c.g, c.b, c.a);
    }
    function applyStrokeWidth(w) {
        root.strokeWidth = w;
        WindowManager.setAnnotationStrokeWidth(w);
    }

    // Sticky color preference — persists across sessions via
    // QtCore.Settings. Stored as a 6-digit hex string (alpha is
    // intentionally absent). Bidirectional: drawingColor changes
    // (from the inline picker) write _settingsColorHex, which
    // QSettings auto-saves; on next launch the alias re-reads it
    // and we fan out to drawingColor.
    property string _settingsColorHex: "#ff0000"
    Settings {
        category: "annotation"
        property alias colorHex: root._settingsColorHex
    }
    Component.onCompleted: {
        const c = Qt.color(_settingsColorHex);
        if (c.r >= 0) {
            drawingColor = c;
            applyColor(c);
        }
    }
    onDrawingColorChanged: {
        const hh = (n) => {
            const v = Math.max(0, Math.min(255, Math.round(n * 255)));
            const s = v.toString(16);
            return s.length < 2 ? "0" + s : s;
        };
        _settingsColorHex = "#" + hh(drawingColor.r)
                                 + hh(drawingColor.g)
                                 + hh(drawingColor.b);
    }

    // Refresh trigger. We split notesChanged into two paths so
    // the Repeater stops blowing away every delegate (which
    // caused the all-cards thumbnail flash on every stroke /
    // erase commit):
    //   - structural change (note added / removed / reordered):
    //     reassign _notesModel → Repeater rebuilds.
    //   - content-only change (text / addressed / hasStrokes
    //     flip): bump _notesRevision; existing delegates' _live
    //     property re-evaluates and pulls fresh values from
    //     WindowManager.notesList without delegate destruction.
    property var _notesModel: WindowManager.notesList
    property var _knownTimecodes: WindowManager.notesList.map(
        function(n) { return n.timecode })
    property int _notesRevision: 0
    Connections {
        target: WindowManager
        function onNotesChanged() {
            const fresh = WindowManager.notesList;
            const tcs = fresh.map(function(n) { return n.timecode });
            const same = tcs.length === root._knownTimecodes.length
                      && tcs.every(function(tc, i) {
                            return tc === root._knownTimecodes[i] });
            if (!same) {
                root._notesModel = fresh;
                root._knownTimecodes = tcs;
            }
            root._notesRevision++;
        }
        function onAnnotationsAllowedChanged() {
            root._notesModel = WindowManager.notesList;
            root._knownTimecodes = root._notesModel.map(
                function(n) { return n.timecode });
            root._notesRevision++;
        }
    }

    RowLayout {
        anchors.fill: parent
        // Set each side explicitly — anchors.margins as a shortcut
        // can fight per-side overrides depending on Qt version.
        anchors.topMargin:    Theme.spacing
        anchors.bottomMargin: Theme.spacing
        anchors.leftMargin:   Theme.spacing
        anchors.rightMargin:  Theme.padding
        spacing: Theme.spacing

        // ---- Sticky left: stacked Add (2/3) + Export (1/3) tiles --
        // Add Note Here is the headline action; the Export tile
        // sits beneath at 1/3 height to mimic the same shape and
        // group the two file-producing actions visually.
        Item {
            id: leftStack
            Layout.preferredWidth: root.kAddTileWidth
            Layout.fillHeight: true

            // 2/3 height — primary action.
            Rectangle {
                id: addTile
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                height: parent.height * 2 / 3 - 2
                radius: Theme.radiusSmall
                // Borderless. Hover lifts the fill; the icon and
                // label color shift carry the affordance.
                color: addMa.containsMouse ? Theme.surfaceHover : Theme.bg
                border.width: 0

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: Theme.spacingLoose
                    spacing: Theme.spacing
                    Item { Layout.fillHeight: true }
                    Icon {
                        Layout.alignment: Qt.AlignHCenter
                        name: "plus"
                        size: 32
                        color: addMa.containsMouse ? Theme.success : Theme.textMuted
                    }
                    Text {
                        Layout.alignment: Qt.AlignHCenter
                        Layout.fillWidth: true
                        text: qsTr("Add note here")
                        color: addMa.containsMouse ? Theme.textPrimary : Theme.textSecondary
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontSizeSmall
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.WordWrap
                    }
                    Text {
                        Layout.alignment: Qt.AlignHCenter
                        Layout.fillWidth: true
                        text: qsTr("captures current frame")
                        color: Theme.textMuted
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontSizeTiny
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.WordWrap
                    }
                    Item { Layout.fillHeight: true }
                }
                MouseArea {
                    id: addMa
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: WindowManager.addNoteAtCurrentFrame()
                }
            }

            // 1/3 height — opens the inline Export panel.
            Rectangle {
                id: exportTile
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: addTile.bottom
                anchors.topMargin: 4
                anchors.bottom: parent.bottom
                radius: Theme.radiusSmall
                // Borderless. Active state filled with accentMuted
                // (panel-open indicator); hover lifts the fill.
                color: root.exportPanelVisible
                       ? Theme.accentMuted
                       : (exportMa.containsMouse ? Theme.surfaceHover
                                                  : Theme.bg)
                border.width: 0

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: Theme.spacingLoose
                    spacing: Theme.spacing
                    Item { Layout.fillHeight: true }
                    Icon {
                        Layout.alignment: Qt.AlignHCenter
                        name: "export"
                        size: 22
                        color: exportMa.containsMouse ? Theme.info : Theme.textSecondary
                    }
                    Text {
                        Layout.alignment: Qt.AlignHCenter
                        Layout.fillWidth: true
                        text: qsTr("Export notes")
                        color: exportMa.containsMouse ? Theme.textPrimary : Theme.textSecondary
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontSizeSmall
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.WordWrap
                    }
                    Item { Layout.fillHeight: true }
                }
                MouseArea {
                    id: exportMa
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.exportPanelVisible =
                                  !root.exportPanelVisible
                }
            }
        }

        // ---- Inline Export panel (slides between Add/Export tiles
        // and the filmstrip). We avoid QML Popups because the
        // player surface is a native QWindow and popups composite
        // poorly above it; an inline panel keeps everything in the
        // QML scenegraph.
        Pane {
            id: exportPanel
            visible: root.exportPanelVisible
            Layout.preferredWidth: visible ? root.kExportWidth : 0
            Layout.fillHeight: true
            padding: Theme.spacingLoose
            background: Rectangle {
                color: Theme.surfaceAlt
                radius: Theme.radiusSmall
            }

            FileDialog {
                id: exportFolderDialog
                title: qsTr("Export folder (Markdown)")
                fileMode: FileDialog.SaveFile
                onAccepted: {
                    const path = WindowManager.urlToOsPath(selectedFile);
                    const slash = path.lastIndexOf("/");
                    const dir   = path.substring(0, slash);
                    let base    = path.substring(slash + 1);
                    const dot = base.lastIndexOf(".");
                    if (dot > 0) base = base.substring(0, dot);
                    if (WindowManager.exportAnnotationNotes(
                            "markdown", dir, base)) {
                        root.exportPanelVisible = false;
                    }
                }
            }
            component FormatDialog: FileDialog {
                property string fmt: ""
                fileMode: FileDialog.SaveFile
                onAccepted: {
                    const path = WindowManager.urlToOsPath(selectedFile);
                    if (WindowManager.exportAnnotationNotes(fmt, path)) {
                        root.exportPanelVisible = false;
                    }
                }
            }
            FormatDialog {
                id: exportHtmlDialog
                fmt: "html"
                title: qsTr("Save HTML…")
                defaultSuffix: "html"
                nameFilters: [qsTr("HTML (*.html)")]
            }
            FormatDialog {
                id: exportPdfDialog
                fmt: "pdf"
                title: qsTr("Save PDF…")
                defaultSuffix: "pdf"
                nameFilters: [qsTr("PDF (*.pdf)")]
            }
            FormatDialog {
                id: exportDocxDialog
                fmt: "docx"
                title: qsTr("Save DOCX…")
                defaultSuffix: "docx"
                nameFilters: [qsTr("Word document (*.docx)")]
            }

            ColumnLayout {
                anchors.fill: parent
                spacing: Theme.spacing
                Text {
                    text: qsTr("Export notes")
                    color: Theme.textPrimary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontSizeBase
                    font.bold: true
                    Layout.fillWidth: true
                }
                Text {
                    text: WindowManager.notesList && WindowManager.notesList.length
                          ? qsTr("%1 notes will be exported.").arg(
                              WindowManager.notesList.length)
                          : qsTr("No notes to export yet.")
                    color: Theme.textSecondary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontSizeTiny
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 1
                    color: Theme.divider
                }

                FlatButton {
                    Layout.fillWidth: true
                    text: qsTr("Markdown folder…")
                    hoverColor: Theme.success
                    enabled: WindowManager.notesList
                             && WindowManager.notesList.length > 0
                    onClicked: exportFolderDialog.open()
                    tooltipText: qsTr(
                        "Folder + linked images.  Type any name to "
                        + "set the output dir; the export creates a "
                        + "timestamped subfolder there.")
                }
                FlatButton {
                    Layout.fillWidth: true
                    text: qsTr("HTML (single file)…")
                    hoverColor: Theme.success
                    enabled: WindowManager.notesList
                             && WindowManager.notesList.length > 0
                    onClicked: exportHtmlDialog.open()
                }
                FlatButton {
                    Layout.fillWidth: true
                    text: qsTr("PDF…")
                    hoverColor: Theme.success
                    enabled: WindowManager.notesList
                             && WindowManager.notesList.length > 0
                    onClicked: exportPdfDialog.open()
                }
                FlatButton {
                    Layout.fillWidth: true
                    text: qsTr("DOCX…")
                    hoverColor: Theme.success
                    enabled: WindowManager.notesList
                             && WindowManager.notesList.length > 0
                    onClicked: exportDocxDialog.open()
                }

                Item { Layout.fillHeight: true }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 1
                    color: Theme.divider
                }
                FlatButton {
                    Layout.fillWidth: true
                    text: qsTr("Cancel")
                    variant: "danger"
                    onClicked: root.exportPanelVisible = false
                }
            }
        }

        // ---- Filmstrip (only zone that scrolls) -------------------
        ScrollView {
            id: stripScroll
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentWidth: filmstrip.implicitWidth
            contentHeight: height
            clip: true
            ScrollBar.horizontal.policy: ScrollBar.AsNeeded
            ScrollBar.vertical.policy:   ScrollBar.AlwaysOff

            // Mouse wheel → horizontal scroll. Default
            // ScrollView wheel handling is vertical-only; we
            // intercept the underlying Flickable's contentX.
            // Trackpad horizontal-flick (angleDelta.x) feeds
            // through naturally too.
            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.NoButton
                propagateComposedEvents: true
                onWheel: (wheel) => {
                    const flick = stripScroll.contentItem;
                    if (!flick) return;
                    const dx = wheel.angleDelta.x !== 0
                                ? wheel.angleDelta.x
                                : wheel.angleDelta.y;
                    if (dx === 0) return;
                    // angleDelta in 1/8 of a degree; one
                    // notch = 120. Step ~80 px per notch.
                    const step = -dx / 120 * 80;
                    const max = Math.max(0, flick.contentWidth
                                              - flick.width);
                    flick.contentX = Math.max(0,
                        Math.min(max, flick.contentX + step));
                    wheel.accepted = true;
                }
            }

            Row {
                id: filmstrip
                spacing: Theme.spacingLoose
                anchors.verticalCenter: parent.verticalCenter

                Repeater {
                    model: root._notesModel
                    delegate: Rectangle {
                        id: card
                        width: root.kCardWidth
                        height: root.kThumbHeight + 96
                        radius: Theme.radiusSmall
                        // Addressed cards sit slightly recessed; hover
                        // lifts the fill (no border swap) so the card
                        // feels of a piece with the LUT-tile language.
                        color: cardMa.containsMouse
                                ? Theme.surfaceHover
                                : (_live.addressed ? Theme.bgAlt : Theme.surfaceAlt)
                        opacity: _live.addressed ? 0.7 : 1.0

                        // Live lookup against WindowManager.notesList
                        // by stable timecode. Re-evaluates whenever
                        // root._notesRevision bumps, so addressed /
                        // text / hasStrokes / imagePath stay current
                        // without rebuilding the delegate. Falls back
                        // to modelData if the note has just been
                        // removed (defensive — structural deletes
                        // already destroy this delegate).
                        readonly property var _live: {
                            const _ = root._notesRevision;   // dep
                            const lst = WindowManager.notesList;
                            for (let i = 0; i < lst.length; i++) {
                                if (lst[i].timecode === modelData.timecode)
                                    return lst[i];
                            }
                            return modelData;
                        }

                        // Bumps when the annotated thumbnail file
                        // for this note's timecode is rewritten on
                        // disk. Appended as a "?v=N" cache-bust
                        // suffix on the Image source so QML re-binds
                        // and Image reloads asynchronously — the
                        // existing pixmap stays painted while the
                        // new one decodes, eliminating the flash.
                        property int thumbVersion: 0
                        Connections {
                            target: WindowManager
                            function onAnnotatedThumbReady(tc) {
                                if (tc === modelData.timecode)
                                    card.thumbVersion++
                            }
                        }

                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: root.kCardPadding
                            spacing: Theme.spacing

                            Rectangle {
                                Layout.preferredWidth: root.kCardWidth - 2 * root.kCardPadding
                                Layout.preferredHeight: root.kThumbHeight - root.kCardPadding
                                color: Theme.bg
                                border.color: Theme.toolbarAlt
                                border.width: 1
                                // Two stacked Image elements with a
                                // hold-and-swap pattern: when the
                                // target source URL changes (thumb
                                // file rewritten + cache-bust bump),
                                // the BACK image loads the new URL
                                // async while the FRONT keeps
                                // painting the previously-decoded
                                // pixmap. When the back is Ready, we
                                // flip which is "front" — so the
                                // user never sees a blank Image
                                // element. Source is only ever
                                // assigned to the currently-hidden
                                // image, so the unavoidable
                                // pixmap-clear on source-change is
                                // invisible.
                                Item {
                                    id: thumbHolder
                                    anchors.fill: parent
                                    anchors.margins: 1
                                    visible: card._live.imagePath
                                             ? true : false

                                    readonly property string targetSource: {
                                        if (!card._live.imagePath) return ""
                                        // Windows absolute paths look like
                                        // C:/Users/... — a valid file URL
                                        // needs three slashes after the
                                        // scheme (file:///C:/Users/...).
                                        // POSIX absolute paths look like
                                        // /Users/... and need two
                                        // (file:///Users/...). The
                                        // simplest cross-platform fix is
                                        // file:/// + ensure no leading
                                        // slash duplication.
                                        let p = card._live.imagePath
                                        if (!p.startsWith("/")) p = "/" + p
                                        const suffix = card.thumbVersion > 0
                                            ? "?v=" + card.thumbVersion : ""
                                        return "file://" + p + suffix
                                    }

                                    property bool aIsFront: true

                                    onTargetSourceChanged: {
                                        if (targetSource === "") {
                                            imgA.source = ""
                                            imgB.source = ""
                                            return
                                        }
                                        if (aIsFront) imgB.source = targetSource
                                        else          imgA.source = targetSource
                                    }

                                    Component.onCompleted: {
                                        if (targetSource !== "")
                                            imgA.source = targetSource
                                    }

                                    Image {
                                        id: imgA
                                        anchors.fill: parent
                                        fillMode: Image.PreserveAspectFit
                                        cache: false
                                        asynchronous: true
                                        // Visibility (not opacity) — a
                                        // clean instant swap; opacity
                                        // animations exposed transient
                                        // states between back-loads.
                                        visible: thumbHolder.aIsFront
                                        onStatusChanged: {
                                            if (status === Image.Ready
                                                && !thumbHolder.aIsFront)
                                                thumbHolder.aIsFront = true
                                        }
                                    }
                                    Image {
                                        id: imgB
                                        anchors.fill: parent
                                        fillMode: Image.PreserveAspectFit
                                        cache: false
                                        asynchronous: true
                                        visible: !thumbHolder.aIsFront
                                        onStatusChanged: {
                                            if (status === Image.Ready
                                                && thumbHolder.aIsFront)
                                                thumbHolder.aIsFront = false
                                        }
                                    }
                                }
                                Text {
                                    anchors.centerIn: parent
                                    visible: !card._live.imagePath
                                    text: qsTr("(no thumb)")
                                    color: Theme.textMuted
                                    font.family: Theme.fontFamily
                                    font.pixelSize: Theme.fontSizeTiny
                                }
                                Rectangle {
                                    visible: card._live.hasStrokes
                                    anchors.right: parent.right
                                    anchors.top:   parent.top
                                    anchors.margins: 4
                                    width: 8; height: 8; radius: 4
                                    color: Theme.error
                                    border.color: "#000000"
                                    border.width: 1
                                }
                                MouseArea {
                                    id: cardMa
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: WindowManager.seekToAnnotationNote(
                                        modelData.timecode)
                                }
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: Theme.spacing
                                Text {
                                    text: modelData.timecode
                                    color: Theme.textPrimary
                                    font.family: Theme.monoFamily
                                    font.pixelSize: Theme.fontSizeSmall
                                    MouseArea {
                                        anchors.fill: parent
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked:
                                            WindowManager.seekToAnnotationNote(
                                                modelData.timecode)
                                    }
                                }
                                Text {
                                    text: "f" + modelData.frame
                                    color: Theme.textMuted
                                    font.family: Theme.monoFamily
                                    font.pixelSize: Theme.fontSizeTiny
                                    Layout.fillWidth: true
                                }
                                CheckBox {
                                    id: addressedCB
                                    checked: card._live.addressed
                                    onToggled: WindowManager.updateAnnotationNoteAddressed(
                                        modelData.timecode, checked)
                                    Layout.preferredWidth: 18
                                    Layout.preferredHeight: 18
                                    // Strip the inherited
                                    // AbstractButton padding so the
                                    // indicator centers cleanly via
                                    // its anchors below.
                                    padding: 0
                                    indicator: Rectangle {
                                        anchors.centerIn: parent
                                        implicitWidth: 14
                                        implicitHeight: 14
                                        color: parent.checked ? Theme.success
                                                              : "transparent"
                                        border.color: parent.checked ? Theme.success
                                                                      : Theme.textMuted
                                        border.width: 1
                                        radius: Theme.radiusSmall
                                    }
                                    FlatToolTip {
                                        visible: addressedCB.hovered
                                        text: qsTr("Mark addressed")
                                    }
                                }
                                Rectangle {
                                    // Filled-on-hover delete chip,
                                    // matching the LUT clear button.
                                    Layout.preferredWidth: 18
                                    Layout.preferredHeight: 18
                                    color: delMa.containsMouse ? Theme.error : "transparent"
                                    radius: Theme.radiusSmall
                                    Icon {
                                        anchors.centerIn: parent
                                        name: "x"
                                        size: Theme.iconSizeSmall
                                        color: delMa.containsMouse
                                               ? Theme.textBright
                                               : Theme.textMuted
                                    }
                                    MouseArea {
                                        id: delMa
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: WindowManager.deleteAnnotationNote(
                                            modelData.timecode)
                                        FlatToolTip {
                                            visible: delMa.containsMouse
                                            text: qsTr("Delete note")
                                        }
                                    }
                                }
                            }

                            ScrollView {
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                clip: true
                                TextArea {
                                    id: noteText
                                    placeholderText: qsTr("Note…")
                                    text: card._live.text
                                    color: Theme.textPrimary
                                    placeholderTextColor: Theme.textMuted
                                    font.family: Theme.fontFamily
                                    font.pixelSize: Theme.fontSizeSmall
                                    selectByMouse: true
                                    wrapMode: TextEdit.Wrap
                                    leftPadding: Theme.spacing
                                    rightPadding: Theme.spacing
                                    topPadding: Theme.spacing
                                    bottomPadding: Theme.spacing
                                    // Recessed-slot styling matching
                                    // FlatTextField — Theme.bg fill at
                                    // rest, lifts on hover/focus, accent
                                    // bottom rule on focus. No border.
                                    background: Rectangle {
                                        color: noteText.activeFocus
                                               ? Theme.surface
                                               : (noteText.hovered ? Theme.bgAlt : Theme.bg)
                                        radius: 0
                                        Rectangle {
                                            anchors.left:   parent.left
                                            anchors.right:  parent.right
                                            anchors.bottom: parent.bottom
                                            height: 1
                                            color: noteText.activeFocus ? Theme.accent : "transparent"
                                        }
                                    }
                                    onActiveFocusChanged: {
                                        if (!activeFocus
                                            && text !== card._live.text) {
                                            WindowManager.updateAnnotationNoteText(
                                                modelData.timecode, text);
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        // ---- Sticky right: Annotation Tools -----------------------
        // Photoshop-style palette: tool icon buttons in a grid,
        // color swatch + presets row, stroke-width slider.
        Rectangle {
            Layout.preferredWidth: root.kToolsWidth
            Layout.fillHeight: true
            radius: Theme.radiusSmall
            color: Theme.surfaceAlt

            // Palette button. Toggleable, Phosphor-icon-only,
            // highlights when the corresponding tool is active.
            // Borderless idle / hover; checked-state uses an
            // accentMuted fill so the active tool reads at-a-
            // glance without a separate outline.
            component ToolBtn: Button {
                id: toolBtn
                property int    toolId:   0
                property string iconName: ""
                property string tooltipText: ""
                checkable: true
                checked: root.activeTool === toolId
                Layout.preferredWidth: 36
                Layout.preferredHeight: 32
                padding: 0
                contentItem: Icon {
                    name: toolBtn.iconName
                    size: Theme.iconSizeMedium
                    color: toolBtn.checked
                           ? Theme.textBright
                           : (toolBtn.hovered ? Theme.textPrimary
                                              : Theme.textSecondary)
                }
                background: Rectangle {
                    // Match FlatButton default-checked vocabulary —
                    // full Theme.accent fill so the active tool
                    // reads at-a-glance, hover state brightens to
                    // accentHover. Squared like FlatButton.
                    color: toolBtn.checked
                           ? (toolBtn.hovered ? Theme.accentHover
                                              : Theme.accent)
                           : (toolBtn.hovered ? Theme.surfaceHover
                                              : "transparent")
                    radius: Theme.radius
                }
                onClicked: root.applyTool(toolId)
                FlatToolTip {
                    visible: toolBtn.hovered && toolBtn.tooltipText.length > 0
                    text: toolBtn.tooltipText
                }
            }

            // Vertical icon column. Deselect button at the top
            // (toolId 0 — always exits annotation mode), then the
            // creation tools, then the eraser. Export trigger now
            // lives in the left stack; this column is tools-only.
            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 5
                spacing: Theme.spacing
                // Pointer tool (toolId 7 = DrawingTool::Select):
                // click a stroke to select, drag to move, corner
                // handles scale, Del removes, Esc deselects. There
                // is no separate Deselect button — like every tool,
                // clicking the armed tool again disarms it (toolId 0
                // still exists internally: applyTool's toggle-off
                // path and the panel-close reset both use it).
                ToolBtn { Layout.alignment: Qt.AlignHCenter; toolId: 7; iconName: "cursor"; tooltipText: qsTr("Select (drag to move, corners to scale, Del to remove; click again to exit)") }
                ToolBtn { Layout.alignment: Qt.AlignHCenter; toolId: 1; iconName: "pencil-simple"; tooltipText: qsTr("Freehand") }
                ToolBtn { Layout.alignment: Qt.AlignHCenter; toolId: 2; iconName: "rectangle"; tooltipText: qsTr("Rectangle") }
                ToolBtn { Layout.alignment: Qt.AlignHCenter; toolId: 3; iconName: "circle"; tooltipText: qsTr("Oval") }
                ToolBtn { Layout.alignment: Qt.AlignHCenter; toolId: 4; iconName: "arrow-right"; tooltipText: qsTr("Arrow") }
                ToolBtn { Layout.alignment: Qt.AlignHCenter; toolId: 5; iconName: "line-segment"; tooltipText: qsTr("Line") }
                ToolBtn { Layout.alignment: Qt.AlignHCenter; toolId: 6; iconName: "eraser"; tooltipText: qsTr("Erase stroke (click strokes to remove)") }
                Item { Layout.fillHeight: true }
            }
        }

        // ---- Sticky right-of-tools: inline color picker + width
        // Live updates as you drag. valueChanged feeds back into
        // applyColor → WindowManager.setAnnotationColor. Stroke
        // width slider sits at the bottom — same Pane so the user
        // has every drawing-style control colocated.
        Pane {
            Layout.preferredWidth: root.kPickerWidth
            Layout.fillHeight: true
            padding: 0
            // Breathing room on the right edge of the panel — the
            // picker sits flush against the panel's right border
            // without it.
            rightPadding: Theme.padding
            background: Rectangle {
                color: Theme.surfaceAlt
                radius: Theme.radiusSmall
            }
            ColumnLayout {
                anchors.fill: parent
                spacing: 0
                ColorPickerInline {
                    id: inlinePicker
                    Layout.fillWidth: true
                    value: root.drawingColor
                    onValueChanged: root.applyColor(value)
                }
                // Stroke-width slider — packed against the bottom
                // of the picker so the user has color and width in
                // the same visual unit.
                RowLayout {
                    Layout.fillWidth: true
                    Layout.leftMargin: Theme.spacingLoose
                    Layout.rightMargin: Theme.spacingLoose
                    Layout.bottomMargin: Theme.spacingLoose
                    Layout.topMargin: 0
                    spacing: Theme.spacing
                    Text {
                        text: qsTr("Width:")
                        color: Theme.textSecondary
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontSizeTiny
                    }
                    FlatSlider {
                        id: strokeWidthSlider
                        Layout.fillWidth: true
                        from: 1
                        to: 24
                        value: root.strokeWidth
                        onValueChanged: root.applyStrokeWidth(value)
                    }
                    Text {
                        text: strokeWidthSlider.value.toFixed(0)
                        color: Theme.textSecondary
                        font.family: Theme.monoFamily
                        font.pixelSize: Theme.fontSizeTiny
                        Layout.preferredWidth: 16
                        horizontalAlignment: Text.AlignRight
                    }
                }
            }
        }
    }
}
