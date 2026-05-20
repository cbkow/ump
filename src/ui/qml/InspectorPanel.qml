// InspectorPanel — Phase 3.A.
//
// Per Guide 07 §3, the Inspector lives below the Bins panel in the
// left rail and reads from the active selection. Phase 3.A ships the
// shell + the always-on File section. Type-specific sub-sections
// appear/disappear based on the active item's type.
//
// Currently active sections:
//   - File           (always — name, path, size, type, copy/reveal)
//   - ImageSequence  (when type == ImageSequence)
//
// Stub placeholders for sections that need the MetadataService
// (Video / Audio / Color / Timecodes / Linked Projects). They light
// up in Phases 3.B – 3.F.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Qcv

Rectangle {
    id: root
    color: Theme.bgAlt
    border.width: 0

    property bool expanded: true

    // Phase 7.7 Stage 8 — Inspector side picker for dual mode. In
    // single mode this is always "A" and the picker is hidden; in
    // dual mode the user can flip between A and B to inspect either
    // side's MediaItem (file metadata, video metadata, image-seq
    // section + EXR layer chooser, Adobe links, etc.). Reset to "A"
    // on every dual entry so leaving and re-entering doesn't strand
    // the user on a stale side.
    property string inspectedSide: "A"
    readonly property bool dualActive: WindowManager.compositorMode !== 0

    readonly property var activeItem:
        WindowManager.project ? WindowManager.project.activeItem : null
    readonly property var bSourceItem:
        WindowManager.project ? WindowManager.project.bSourceItem : null

    // The MediaItem the inspector renders metadata for. In single
    // mode it's always activeItem (A side). In dual mode it follows
    // inspectedSide. Falls back to activeItem if B is unset.
    readonly property var displayedItem: {
        if (dualActive && inspectedSide === "B"
            && bSourceItem && bSourceItem.id) {
            return bSourceItem;
        }
        return activeItem;
    }

    readonly property bool hasActive:
        displayedItem && displayedItem.id !== undefined
        && displayedItem.id !== ""
    readonly property int itemType:
        displayedItem ? (displayedItem.type || 0) : -1

    // Snap back to side A when leaving dual mode so the next dual
    // entry starts on the canonical side.
    Connections {
        target: WindowManager
        function onCompositorModeChanged() {
            if (WindowManager.compositorMode === 0) root.inspectedSide = "A";
        }
    }

    // Revision counter for inspector pill bindings that re-fetch a
    // MediaItem map via WindowManager.project.mediaItemMap(id).
    // mediaItemMap is a Q_INVOKABLE method without its own NOTIFY,
    // so the bindings can't observe per-item-property mutations on
    // their own. The QML binding-engine optimizer also short-circuits
    // when an upstream binding (e.g. routingTargetItemId) re-evaluates
    // to the same string — id-based bindings stop propagating even
    // though the underlying MediaItem's audioRoutingMode /
    // videoRangeOverride changed. Bumping this counter from each
    // relevant ProjectManager signal forces dependent bindings to
    // re-evaluate. Pills reference `inspectorPillRev` inside their
    // target-item bindings purely to opt into this dependency.
    property int inspectorPillRev: 0
    Connections {
        target: WindowManager.project
        function onAudioRoutingModeChanged(itemId, mode) { root.inspectorPillRev++ }
        function onVideoRangeOverrideChanged(itemId, range) { root.inspectorPillRev++ }
    }

    // Hide entirely when nothing is selected — the bin's empty-state
    // hint already tells the user to add media.
    visible: hasActive

    // Inspector is now always expanded — the rail's own header
    // already says "Inspector", so the inner accordion shell was
    // redundant. Height is just whatever the body needs.
    implicitHeight: visible
        ? (content.y + content.implicitHeight + 8)
        : 0

    function typeLabel(t) {
        switch (t) {
        case 0: return qsTr("Video");
        case 1: return qsTr("Audio");
        case 2: return qsTr("Image");
        case 3: return qsTr("Image Sequence");
        case 4: return qsTr("Playlist");
        }
        return qsTr("Media");
    }

    function basename(path) {
        if (!path) return "";
        const i = Math.max(path.lastIndexOf("/"), path.lastIndexOf("\\"));
        return i >= 0 ? path.substring(i + 1) : path;
    }

    function formatSize(bytes) {
        if (!bytes || bytes < 0) return "";
        const KB = 1024;
        const MB = KB * 1024;
        const GB = MB * 1024;
        if (bytes >= GB) return (bytes / GB).toFixed(2) + " GB";
        if (bytes >= MB) return (bytes / MB).toFixed(1) + " MB";
        if (bytes >= KB) return (bytes / KB).toFixed(0) + " KB";
        return bytes + " B";
    }

    function formatDuration(seconds) {
        if (!seconds || seconds <= 0) return "";
        const h  = Math.floor(seconds / 3600);
        const m  = Math.floor((seconds % 3600) / 60);
        const s  = Math.floor(seconds % 60);
        if (h > 0) {
            return ("00" + h).slice(-2) + ":" +
                   ("00" + m).slice(-2) + ":" +
                   ("00" + s).slice(-2);
        }
        return ("00" + m).slice(-2) + ":" +
               ("00" + s).slice(-2);
    }

    // Reusable section header — uppercase letter-spaced label
    // followed by a 1-px Theme.divider rule. Used by each major
    // group below (FILE / VIDEO / HDR / COLOR / AUDIO / TIMECODE
    // / LINKED PROJECTS / PLAYLIST CONTENTS) so the section
    // separation is consistent.
    component SectionHeader: ColumnLayout {
        property string label: ""
        Layout.fillWidth: true
        spacing: 2
        Text {
            Layout.fillWidth: true
            text: parent.label
            color: Theme.textMuted
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontSizeTiny
            font.bold: true
            font.letterSpacing: 0.5
        }
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color: Theme.divider
        }
    }

    // Slim pill row at the top of the panel with the A/B side
    // picker (dual mode only) and the active item's type label.
    // The full styled "Inspector" header was redundant with the
    // right rail's own header — dropped in Stage C polish.
    Item {
        id: headerStrip
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: Theme.padding
        anchors.rightMargin: Theme.padding
        height: hasContent ? 22 : 0
        readonly property bool hasContent:
            root.dualActive || root.hasActive
        visible: hasContent

        RowLayout {
            anchors.fill: parent
            spacing: 6
            // Phase 7.7 Stage 8 — A/B side picker. Only in dual.
            // A's color matches Track A's accentBorder (#446a90) in
            // TimelinePanel; B's matches Track B's (#a0664a). Flat
            // chip — solid color fill when active, transparent at
            // rest with surfaceHover on hover. No border.
            Row {
                visible: root.dualActive
                spacing: 0
                Repeater {
                    model: [
                        { side: "A", color: "#446a90" },
                        { side: "B", color: "#a0664a" }
                    ]
                    delegate: Rectangle {
                        required property var modelData
                        readonly property bool isActive:
                            root.inspectedSide === modelData.side
                        width: 22; height: 18
                        radius: Theme.radius
                        color: isActive
                               ? modelData.color
                               : (sideMa.containsMouse ? Theme.surfaceHover
                                                       : "transparent")
                        Text {
                            anchors.centerIn: parent
                            text: modelData.side
                            color: parent.isActive ? Theme.textBright
                                                   : Theme.textSecondary
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.fontSizeTiny
                            font.bold: parent.isActive
                        }
                        MouseArea {
                            id: sideMa
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                if (modelData.side === "B"
                                    && (!root.bSourceItem
                                        || !root.bSourceItem.id)) {
                                    return;
                                }
                                root.inspectedSide = modelData.side;
                            }
                        }
                    }
                }
            }
            Item { Layout.fillWidth: true }
            Text {
                text: root.hasActive ? root.typeLabel(root.itemType) : ""
                color: Theme.textSecondary
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontSizeTiny
            }
        }
    }

    // ---- Body ----
    ColumnLayout {
        id: content
        anchors.top: headerStrip.bottom
        anchors.topMargin: headerStrip.visible ? 4 : 0
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.margins: Theme.padding
        spacing: Theme.paddingLoose

        // ---- File section ----
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 2

            SectionHeader { label: qsTr("FILE") }

            // Name (editable later in 3.D when MediaModel::update lands)
            Text {
                Layout.fillWidth: true
                text: root.hasActive ? root.displayedItem.name : ""
                color: Theme.textPrimary
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontSizeBase
                elide: Text.ElideMiddle
            }

            // Path. Hidden for media types that don't reference an
            // on-disk file — currently Playlist (4) and DualPair (5);
            // both are pure pool aggregates with no path.
            readonly property bool hasOnDiskPath:
                root.itemType !== 4 && root.itemType !== 5

            Text {
                Layout.fillWidth: true
                visible: parent.hasOnDiskPath
                text: root.hasActive
                    ? WindowManager.toNativeSeparators(root.displayedItem.path)
                    : ""
                color: Theme.textSecondary
                font.family: Theme.monoFamily
                font.pixelSize: Theme.fontSizeTiny
                elide: Text.ElideMiddle
                wrapMode: Text.NoWrap
            }

            // Path action buttons — same visibility gate.
            // Top divider for the action row.
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 1
                Layout.topMargin: Theme.spacing
                color: Theme.divider
                visible: parent.hasOnDiskPath
            }
            RowLayout {
                Layout.fillWidth: true
                // Negate the column's 2-px spacing so the buttons
                // touch the divider above.
                Layout.topMargin: -2
                spacing: 4
                visible: parent.hasOnDiskPath

                FlatButton {
                    iconName: "copy"
                    text: qsTr("Copy path")
                    onClicked: {
                        if (root.displayedItem && root.displayedItem.path) {
                            WindowManager.copyTextToClipboard(
                                WindowManager.toNativeSeparators(
                                    root.displayedItem.path))
                        }
                    }
                }
                FlatButton {
                    iconName: "folder-simple"
                    text: qsTr("Reveal")
                    onClicked: {
                        if (root.displayedItem && root.displayedItem.path) {
                            WindowManager.revealInFileManager(root.displayedItem.path)
                        }
                    }
                }
                Item { Layout.fillWidth: true }
            }
            // Bottom divider for the action row.
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 1
                Layout.topMargin: -2
                color: Theme.divider
                visible: parent.hasOnDiskPath
            }

            // Stat row: size, duration
            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: 4
                spacing: Theme.paddingLoose
                Text {
                    text: root.hasActive
                        ? root.formatSize(root.displayedItem.sizeBytes || 0)
                        : ""
                    color: Theme.textSecondary
                    font.family: Theme.monoFamily
                    font.pixelSize: Theme.fontSizeTiny
                }
                Text {
                    text: root.hasActive
                        ? root.formatDuration(root.displayedItem.duration || 0)
                        : ""
                    color: Theme.textSecondary
                    font.family: Theme.monoFamily
                    font.pixelSize: Theme.fontSizeTiny
                }
                Item { Layout.fillWidth: true }
            }
        }

        // ---- Video / Audio / Color sections ----
        // Driven by the MetadataService-extracted MediaItem.video
        // (Phase 3.B). Visible the moment extraction completes —
        // doesn't require the item to be the loaded source.
        readonly property var vmeta:
            root.displayedItem && root.displayedItem.video
                ? root.displayedItem.video : null
        readonly property bool videoLoaded:
            root.hasActive && root.displayedItem.videoLoaded === true

        // Loading sentinel for video / audio types whose extraction
        // is still in flight.
        Text {
            visible: (root.itemType === 0 || root.itemType === 1)
                     && !content.videoLoaded
            Layout.fillWidth: true
            text: qsTr("Reading metadata…")
            color: Theme.textMuted
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontSizeTiny
            font.italic: true
            Layout.topMargin: 4
        }

        // ---- VIDEO ----
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 2
            visible: content.videoLoaded
                     && content.vmeta
                     && content.vmeta.width > 0

            SectionHeader { label: qsTr("VIDEO") }

            GridLayout {
                Layout.fillWidth: true
                columns: 2
                columnSpacing: Theme.paddingLoose
                rowSpacing: 2

                Text { text: qsTr("Resolution"); color: Theme.textSecondary;
                       font.family: Theme.fontFamily; font.pixelSize: Theme.fontSizeTiny }
                Text {
                    Layout.fillWidth: true
                    text: content.vmeta
                        ? content.vmeta.width + " × " + content.vmeta.height
                        : ""
                    color: Theme.textPrimary
                    font.family: Theme.monoFamily
                    font.pixelSize: Theme.fontSizeSmall
                }

                Text { text: qsTr("Frame rate"); color: Theme.textSecondary;
                       font.family: Theme.fontFamily; font.pixelSize: Theme.fontSizeTiny }
                Text {
                    Layout.fillWidth: true
                    text: content.vmeta && content.vmeta.frameRate > 0
                        ? content.vmeta.frameRate.toFixed(3) + " fps"
                        : ""
                    color: Theme.textPrimary
                    font.family: Theme.monoFamily
                    font.pixelSize: Theme.fontSizeSmall
                }

                Text { text: qsTr("Total frames"); color: Theme.textSecondary;
                       font.family: Theme.fontFamily; font.pixelSize: Theme.fontSizeTiny }
                Text {
                    Layout.fillWidth: true
                    text: content.vmeta ? content.vmeta.totalFrames : ""
                    color: Theme.textPrimary
                    font.family: Theme.monoFamily
                    font.pixelSize: Theme.fontSizeSmall
                }

                Text { text: qsTr("Codec"); color: Theme.textSecondary;
                       font.family: Theme.fontFamily; font.pixelSize: Theme.fontSizeTiny }
                Text {
                    Layout.fillWidth: true
                    text: content.vmeta ? content.vmeta.videoCodec : ""
                    color: Theme.textPrimary
                    font.family: Theme.monoFamily
                    font.pixelSize: Theme.fontSizeSmall
                    elide: Text.ElideRight
                }

                Text { text: qsTr("Pixel format"); color: Theme.textSecondary;
                       font.family: Theme.fontFamily; font.pixelSize: Theme.fontSizeTiny }
                Text {
                    Layout.fillWidth: true
                    text: content.vmeta ? content.vmeta.pixelFormat : ""
                    color: Theme.textPrimary
                    font.family: Theme.monoFamily
                    font.pixelSize: Theme.fontSizeSmall
                }

                Text { text: qsTr("Bit depth"); color: Theme.textSecondary;
                       font.family: Theme.fontFamily; font.pixelSize: Theme.fontSizeTiny }
                Text {
                    Layout.fillWidth: true
                    text: content.vmeta
                        ? content.vmeta.bitDepth + qsTr(" bit") +
                          (content.vmeta.hasAlpha ? qsTr(" + alpha") : "")
                        : ""
                    color: Theme.textPrimary
                    font.family: Theme.monoFamily
                    font.pixelSize: Theme.fontSizeSmall
                }

                Text {
                    text: qsTr("HDR")
                    color: Theme.textSecondary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontSizeTiny
                    visible: content.vmeta && content.vmeta.isHdrContent
                }
                Text {
                    Layout.fillWidth: true
                    text: content.vmeta && content.vmeta.isHdrContent
                        ? qsTr("Yes — ") + content.vmeta.colorTransfer
                        : ""
                    color: Theme.warn
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontSizeSmall
                    visible: content.vmeta && content.vmeta.isHdrContent
                }
            }
        }

        // ---- COLOR ----
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 2
            visible: content.videoLoaded
                     && content.vmeta
                     && (content.vmeta.colorspace.length > 0
                         || content.vmeta.colorPrimaries.length > 0
                         || content.vmeta.colorTransfer.length > 0
                         || content.vmeta.colorRange.length > 0)

            SectionHeader { label: qsTr("COLOR") }

            GridLayout {
                Layout.fillWidth: true
                columns: 2
                columnSpacing: Theme.paddingLoose
                rowSpacing: 2

                Text { text: qsTr("Colorspace"); color: Theme.textSecondary;
                       font.family: Theme.fontFamily; font.pixelSize: Theme.fontSizeTiny
                       visible: content.vmeta && content.vmeta.colorspace.length > 0 }
                Text {
                    Layout.fillWidth: true
                    text: content.vmeta ? content.vmeta.colorspace : ""
                    color: Theme.textPrimary
                    font.family: Theme.monoFamily
                    font.pixelSize: Theme.fontSizeSmall
                    visible: content.vmeta && content.vmeta.colorspace.length > 0
                }
                Text { text: qsTr("Primaries"); color: Theme.textSecondary;
                       font.family: Theme.fontFamily; font.pixelSize: Theme.fontSizeTiny
                       visible: content.vmeta && content.vmeta.colorPrimaries.length > 0 }
                Text {
                    Layout.fillWidth: true
                    text: content.vmeta ? content.vmeta.colorPrimaries : ""
                    color: Theme.textPrimary
                    font.family: Theme.monoFamily
                    font.pixelSize: Theme.fontSizeSmall
                    visible: content.vmeta && content.vmeta.colorPrimaries.length > 0
                }
                Text { text: qsTr("Transfer"); color: Theme.textSecondary;
                       font.family: Theme.fontFamily; font.pixelSize: Theme.fontSizeTiny
                       visible: content.vmeta && content.vmeta.colorTransfer.length > 0 }
                Text {
                    Layout.fillWidth: true
                    text: content.vmeta ? content.vmeta.colorTransfer : ""
                    color: Theme.textPrimary
                    font.family: Theme.monoFamily
                    font.pixelSize: Theme.fontSizeSmall
                    visible: content.vmeta && content.vmeta.colorTransfer.length > 0
                }
                Text { text: qsTr("NCLC tag"); color: Theme.textSecondary;
                       font.family: Theme.fontFamily; font.pixelSize: Theme.fontSizeTiny
                       visible: content.vmeta && content.vmeta.nclcTag.length > 0 }
                Text {
                    Layout.fillWidth: true
                    text: content.vmeta ? content.vmeta.nclcTag : ""
                    color: Theme.textPrimary
                    font.family: Theme.monoFamily
                    font.pixelSize: Theme.fontSizeSmall
                    visible: content.vmeta && content.vmeta.nclcTag.length > 0
                }
            }

            // ---- Range override picker (Phase 3.G) ----
            // Per Guide 07 D10, range is per-clip-editable. Auto
            // follows FFmpeg's detected `colorRange`; Full / Limited
            // override the YUV→RGB conversion. Only enabled for the
            // loaded source — same UX rule as the Phase 3.F Origin
            // picker (changing it on a non-loaded item just sets a
            // flag with no visible effect until the user loads it).
            RowLayout {
                id: rangeRow
                Layout.fillWidth: true
                Layout.topMargin: 4
                spacing: 4
                visible: root.itemType === 0  // Video only

                readonly property bool rangePickable:
                    root.itemType === 0
                    && WindowManager.videoDecoder
                    && root.activeItem
                    && root.displayedItem
                    && root.activeItem.id === root.displayedItem.id
                // The MediaItem the pill actually edits + reads. Same
                // playlist-vs-single shape the audio routing pill uses
                // (see audioSection.routingTargetItemId): in playlist
                // mode displayedItem IS the playlist, but the per-clip
                // videoRangeOverride lives on the underlying source
                // MediaItem. The audioRoutingScopeMediaItemId helper
                // resolves to the right item regardless of mode — it's
                // a generic "currently-playing clip" id, not specific
                // to audio.
                readonly property string rangeTargetItemId: {
                    if (root.dualActive) {
                        return root.displayedItem ? root.displayedItem.id : "";
                    }
                    return WindowManager
                        ? WindowManager.audioRoutingScopeMediaItemId : "";
                }
                readonly property var rangeTargetItem: {
                    root.inspectorPillRev;   // dependency tag — see InspectorPanel root
                    if (!rangeTargetItemId || !WindowManager.project) return null;
                    return WindowManager.project
                        .mediaItemMap(rangeTargetItemId);
                }
                readonly property int activeRange:
                    rangeTargetItem
                        && rangeTargetItem.videoRangeOverride !== undefined
                        ? rangeTargetItem.videoRangeOverride : 0
                readonly property string detectedLabel: {
                    if (!content.vmeta || !content.vmeta.colorRange) return qsTr("Auto");
                    const r = content.vmeta.colorRange;
                    if (r === "full")    return qsTr("Auto (Full)");
                    if (r === "limited") return qsTr("Auto (Limited)");
                    return qsTr("Auto");
                }

                Text {
                    text: qsTr("Range")
                    color: Theme.textSecondary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontSizeTiny
                    Layout.preferredWidth: 80
                }
                Flow {
                    Layout.fillWidth: true
                    spacing: 4

                    Repeater {
                        model: [
                            { key: 0, useDetected: true,  label: qsTr("Auto") },
                            { key: 1, useDetected: false, label: qsTr("Full") },
                            { key: 2, useDetected: false, label: qsTr("Limited") },
                        ]

                        Rectangle {
                            id: rangeChip
                            required property var modelData
                            readonly property bool isActive:
                                modelData.key === rangeRow.activeRange
                            readonly property string buttonLabel:
                                modelData.useDetected ? rangeRow.detectedLabel
                                                      : modelData.label
                            width: btnLabel.implicitWidth + 14
                            height: 18
                            radius: Theme.radius
                            // Flat toggle vocabulary — solid accent
                            // fill when active, transparent at rest;
                            // hover lifts the inactive cells. No
                            // border.
                            color: isActive
                                   ? Theme.accent
                                   : (chipMa.containsMouse
                                      ? Theme.surfaceHover : "transparent")
                            opacity: rangeRow.rangePickable ? 1.0 : 0.55
                            Text {
                                id: btnLabel
                                anchors.centerIn: parent
                                text: parent.buttonLabel
                                color: parent.isActive ? Theme.textBright : Theme.textPrimary
                                font.family: Theme.fontFamily
                                font.pixelSize: Theme.fontSizeTiny
                                font.bold: parent.isActive
                            }
                            MouseArea {
                                id: chipMa
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: rangeRow.rangePickable
                                    ? Qt.PointingHandCursor
                                    : Qt.ArrowCursor
                                enabled: rangeRow.rangePickable
                                onClicked: {
                                    // Mutate the resolved target —
                                    // active clip's source MediaItem
                                    // in playlist mode, displayedItem
                                    // otherwise. C++ side's
                                    // videoRangeOverrideChanged signal
                                    // pushes the value into whichever
                                    // live decoder owns it (single-flow
                                    // VideoDecoder OR DualPlaybackController
                                    // per-side range atomic), so we
                                    // don't call setRangeOverride here
                                    // anymore — that previously double-
                                    // applied for single-flow and missed
                                    // entirely for dual.
                                    const targetId =
                                        rangeRow.rangeTargetItemId;
                                    if (targetId && WindowManager.project) {
                                        WindowManager.project
                                            .setVideoRangeOverride(
                                                targetId, modelData.key);
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        // ---- AUDIO ----
        // Metadata readout + per-clip routing-mode pills + per-source-
        // channel level meters. Mode pills only show choices the
        // source has channels to support (5.1 Downmix needs ≥6,
        // Stereo 7-8 needs ≥8). For mono/stereo sources only the
        // metadata + meter rows render — there's nothing meaningful
        // to choose.
        ColumnLayout {
            id: audioSection
            Layout.fillWidth: true
            spacing: 2
            visible: content.videoLoaded
                     && content.vmeta
                     && content.vmeta.audioCodec.length > 0

            SectionHeader { label: qsTr("AUDIO") }

            GridLayout {
                Layout.fillWidth: true
                columns: 2
                columnSpacing: Theme.paddingLoose
                rowSpacing: 2

                Text { text: qsTr("Codec"); color: Theme.textSecondary;
                       font.family: Theme.fontFamily; font.pixelSize: Theme.fontSizeTiny }
                Text {
                    Layout.fillWidth: true
                    text: content.vmeta ? content.vmeta.audioCodec : ""
                    color: Theme.textPrimary
                    font.family: Theme.monoFamily
                    font.pixelSize: Theme.fontSizeSmall
                }
                Text { text: qsTr("Sample rate"); color: Theme.textSecondary;
                       font.family: Theme.fontFamily; font.pixelSize: Theme.fontSizeTiny }
                Text {
                    Layout.fillWidth: true
                    text: content.vmeta && content.vmeta.audioSampleRate > 0
                        ? content.vmeta.audioSampleRate + qsTr(" Hz") : ""
                    color: Theme.textPrimary
                    font.family: Theme.monoFamily
                    font.pixelSize: Theme.fontSizeSmall
                }
                Text { text: qsTr("Channels"); color: Theme.textSecondary;
                       font.family: Theme.fontFamily; font.pixelSize: Theme.fontSizeTiny }
                Text {
                    Layout.fillWidth: true
                    text: content.vmeta && content.vmeta.audioChannels > 0
                        ? content.vmeta.audioChannels +
                          (content.vmeta.audioChannels === 2 ? qsTr(" (stereo)")
                          : content.vmeta.audioChannels === 1 ? qsTr(" (mono)") : "")
                        : ""
                    color: Theme.textPrimary
                    font.family: Theme.monoFamily
                    font.pixelSize: Theme.fontSizeSmall
                }
                Text { text: qsTr("Layout"); color: Theme.textSecondary;
                       font.family: Theme.fontFamily; font.pixelSize: Theme.fontSizeTiny;
                       visible: content.vmeta && content.vmeta.audioChannelLayoutName
                                && content.vmeta.audioChannelLayoutName.length > 0 }
                Text {
                    Layout.fillWidth: true
                    visible: content.vmeta && content.vmeta.audioChannelLayoutName
                             && content.vmeta.audioChannelLayoutName.length > 0
                    text: content.vmeta ? content.vmeta.audioChannelLayoutName : ""
                    color: Theme.textPrimary
                    font.family: Theme.monoFamily
                    font.pixelSize: Theme.fontSizeSmall
                }
            }

            // ---- Routing mode pill row -----------------------------
            // Mirrors the cache-stride pill UI in
            // ImageSequenceInspector. Only renders when the source
            // has enough channels for at least one non-Auto choice
            // (≥6 for 5.1 Downmix, ≥8 for Stereo 7-8).
            readonly property int audioCh:
                content.vmeta ? (content.vmeta.audioChannels || 0) : 0
            readonly property bool routingPicker:
                audioCh >= 6
            // The MediaItem the pill actually edits + displays. In
            // single-mode playback of a bin item this is displayedItem
            // (== activeItem). In playlist mode it's the currently-
            // playing clip's underlying source MediaItem, NOT the
            // playlist's own audioRoutingMode field — same logic the
            // C++ side uses at decoder open. Dual mode keeps using
            // displayedItem (which already resolves to activeItem on
            // side A or bSourceItem on side B via the existing
            // inspectedSide chain).
            readonly property string routingTargetItemId: {
                if (root.dualActive) {
                    return root.displayedItem ? root.displayedItem.id : "";
                }
                return WindowManager
                    ? WindowManager.audioRoutingScopeMediaItemId : "";
            }
            // The MediaItem map for the routing target; we re-fetch
            // here (not via displayedItem) because in playlist mode
            // displayedItem is the PLAYLIST and its audioRoutingMode
            // is unrelated to whatever clip is currently playing.
            // The `root.inspectorPillRev` reference is the dependency
            // that forces re-evaluation when the underlying item's
            // audioRoutingMode mutates — without it, the binding
            // optimizer skips re-eval since routingTargetItemId
            // stays the same string across a same-clip mode change.
            readonly property var routingTargetItem: {
                root.inspectorPillRev;   // dependency tag — see InspectorPanel root
                if (!routingTargetItemId || !WindowManager.project) return null;
                return WindowManager.project
                    .mediaItemMap(routingTargetItemId);
            }
            readonly property int currentMode:
                routingTargetItem
                    ? (routingTargetItem.audioRoutingMode || 0) : 0

            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: Theme.spacing
                spacing: Theme.spacingLoose
                visible: parent.routingPicker
                Text {
                    text: qsTr("Mix")
                    color: Theme.textSecondary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontSizeSmall
                    Layout.preferredWidth: 80
                }
                // Auto pill — always present.
                Repeater {
                    model: {
                        // Build the visible mode list per source. Auto
                        // is unconditional; Downmix5_1 needs ≥6 channels;
                        // Stereo7_8 needs ≥8.
                        const ch = parent.parent.audioCh;
                        const list = [{ mode: 0, label: "Auto" }];
                        if (ch >= 6) list.push({ mode: 1, label: "5.1 → 2" });
                        if (ch >= 8) list.push({ mode: 2, label: "7-8" });
                        return list;
                    }
                    Rectangle {
                        Layout.preferredWidth: 50
                        Layout.preferredHeight: 18
                        radius: Theme.radius
                        // Reference the audioSection ColumnLayout's
                        // resolved routing target via its id rather
                        // than walking parent.parent — Repeater
                        // delegate parent semantics are fragile and
                        // the earlier draft of this pill already
                        // tripped on a miscounted parent walk.
                        readonly property bool isCurrent:
                            audioSection.currentMode === modelData.mode
                        color: isCurrent
                            ? Theme.accent
                            : (modeMa.containsMouse ? Theme.surfaceHover : "transparent")
                        Text {
                            anchors.centerIn: parent
                            text: modelData.label
                            color: parent.isCurrent ? Theme.textBright : Theme.textSecondary
                            font.family: Theme.monoFamily
                            font.pixelSize: Theme.fontSizeTiny
                        }
                        MouseArea {
                            id: modeMa
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                // Click mutates the routing TARGET
                                // (the active clip's source MediaItem
                                // in playlist mode, NOT the playlist
                                // item). Without this, the inspector
                                // pill silently edited the playlist's
                                // own audioRoutingMode field which
                                // wasn't the field the decoder reads
                                // on clip transition.
                                const targetId =
                                    audioSection.routingTargetItemId;
                                if (targetId && WindowManager.project) {
                                    WindowManager.project.setAudioRoutingMode(
                                        targetId, modelData.mode);
                                }
                            }
                            FlatToolTip {
                                visible: modeMa.containsMouse
                                text: modelData.mode === 0
                                    ? qsTr("Auto — picks the right mix per source")
                                    : modelData.mode === 1
                                    ? qsTr("5.1 → Stereo (BS.775 downmix from channels 1-6)")
                                    : qsTr("Channels 7-8 (broadcast stereo bounce)")
                            }
                        }
                    }
                }
            }

            // ---- Per-source-channel level meters ------------------
            // Vertical bars per detected source channel, height
            // proportional to peak amplitude. Always shown when any
            // audio is loaded so reviewers can see which channels
            // actually carry signal. Channel name underneath each
            // bar; chosen meter side follows the inspector A/B
            // toggle in dual mode.
            //
            // All bindings reach into the WindowManager Q_PROPERTYs
            // via this section's two readonly properties so the
            // delegate's parent-chain stays shallow + comprehensible.
            // (Earlier draft used parent.parent.parent.parent and
            // mis-counted by one — yielded "of null" TypeErrors on
            // every poll.)
            readonly property var meterPeaks: {
                if (!WindowManager) return [];
                return (root.dualActive && root.inspectedSide === "B")
                    ? (WindowManager.audioChannelPeaksB || [])
                    : (WindowManager.audioChannelPeaks || []);
            }
            readonly property var meterNames: {
                if (!WindowManager) return [];
                return (root.dualActive && root.inspectedSide === "B")
                    ? (WindowManager.audioChannelNamesB || [])
                    : (WindowManager.audioChannelNames || []);
            }
            readonly property color meterBarBaseColor: Theme.success
            readonly property color meterBarWarnColor: "#d4a64a"   // amber
            readonly property color meterBarPeakColor: Theme.error

            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: Theme.spacing
                spacing: 4
                visible: audioSection.meterPeaks.length > 0

                Repeater {
                    model: audioSection.meterPeaks.length

                    delegate: ColumnLayout {
                        Layout.preferredWidth: 18
                        spacing: 2
                        required property int index

                        Item {
                            Layout.preferredHeight: 40
                            Layout.preferredWidth: 12
                            Layout.alignment: Qt.AlignHCenter
                            Rectangle {
                                anchors.bottom: parent.bottom
                                anchors.horizontalCenter: parent.horizontalCenter
                                width: 10
                                readonly property real peak: {
                                    const peaks = audioSection.meterPeaks;
                                    if (!peaks || index >= peaks.length) return 0.0;
                                    return Number(peaks[index]) || 0.0;
                                }
                                height: Math.max(1, Math.min(40, peak * 40))
                                radius: 1
                                color: peak > 0.95 ? audioSection.meterBarPeakColor
                                     : peak > 0.7  ? audioSection.meterBarWarnColor
                                     :               audioSection.meterBarBaseColor
                            }
                        }
                        Text {
                            Layout.alignment: Qt.AlignHCenter
                            text: {
                                const names = audioSection.meterNames;
                                return (names && index < names.length)
                                       ? names[index] : (index + 1).toString();
                            }
                            color: Theme.textMuted
                            font.family: Theme.monoFamily
                            font.pixelSize: Theme.fontSizeTiny
                        }
                    }
                }
            }
        }

        // ---- TIMECODE ----
        // Combines FFmpeg's embedded timecode (Phase 3.B) and the
        // exiftool-derived QT/MXF/XMP variants (Phase 3.D). Each
        // line shows source label, value, optional Copy button.
        readonly property var ameta:
            root.displayedItem && root.displayedItem.adobe
                ? root.displayedItem.adobe : null
        readonly property bool hasAnyTcLine:
            (content.vmeta && content.vmeta.hasEmbeddedTimecode) ||
            (content.ameta && content.ameta.hasAnyTimecode)

        ColumnLayout {
            id: timecodeSection
            Layout.fillWidth: true
            spacing: 2
            visible: content.hasAnyTcLine

            SectionHeader { label: qsTr("TIMECODE") }

            // ---- Origin picker (Phase 3.F) ----
            // Row of toggle buttons: which TC source feeds the
            // playhead readout in TimelineStatus. "From start" leaves
            // the playhead at 00:00:00:00 with no offset; the others
            // re-parse their TC string and use it as the origin so
            // frame 0 reads as the file's natural start TC.
            //
            // Only enabled when the inspected item is the loaded
            // source — picking origin for an item you're just
            // browsing wouldn't do anything until you load it.
            readonly property bool tcPickable:
                root.itemType === 0
                && WindowManager.videoDecoder
                && root.activeItem
                && root.displayedItem
                && root.activeItem.id === root.displayedItem.id
            readonly property string activeTc:
                WindowManager.videoDecoder
                    ? WindowManager.videoDecoder.startTimecode : ""

            function tcButtonModel() {
                const out = [{ key: "", label: qsTr("From start") }];
                if (content.vmeta && content.vmeta.hasEmbeddedTimecode
                    && content.vmeta.embeddedTimecode) {
                    out.push({
                        key:   content.vmeta.embeddedTimecode,
                        label: qsTr("Embedded"),
                    });
                }
                if (content.ameta && content.ameta.qtStartTimecode) {
                    out.push({
                        key:   content.ameta.qtStartTimecode,
                        label: qsTr("QT Start"),
                    });
                }
                if (content.ameta && content.ameta.qtTimecode
                    && content.ameta.qtTimecode
                       !== content.ameta.qtStartTimecode) {
                    out.push({
                        key:   content.ameta.qtTimecode,
                        label: qsTr("QT TC"),
                    });
                }
                if (content.ameta && content.ameta.xmpAltTimecode
                    && content.ameta.xmpAltTimecode
                       !== content.ameta.qtStartTimecode) {
                    out.push({
                        key:   content.ameta.xmpAltTimecode,
                        label: qsTr("XMP Alt"),
                    });
                }
                return out;
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 4
                // tcPickable / tcButtonModel / activeTc live on
                // timecodeSection (the inner ColumnLayout above).
                // The earlier `content.tcPickable` was an outer-id
                // miss that QML6 strict-scope reports as "Unable to
                // assign [undefined] to bool" — visible defaulted
                // to true so the row stayed shown but the Repeater
                // model was undefined and the picker was empty.
                visible: timecodeSection.tcPickable

                Text {
                    text: qsTr("Origin")
                    color: Theme.textSecondary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontSizeTiny
                    Layout.preferredWidth: 80
                }
                Flow {
                    Layout.fillWidth: true
                    spacing: 4

                    Repeater {
                        model: timecodeSection.tcButtonModel()

                        Rectangle {
                            readonly property bool isActive:
                                modelData.key === timecodeSection.activeTc
                            // Labels vary in length ("From start" vs
                            // "QT TC") so width auto-fits; height +
                            // radius match the Mix / Range pill style.
                            width: btnLabel.implicitWidth + 14
                            Layout.preferredHeight: 18
                            height: 18
                            radius: Theme.radius
                            color: isActive
                                ? Theme.accent
                                : (originMa.containsMouse
                                    ? Theme.surfaceHover : "transparent")
                            Text {
                                id: btnLabel
                                anchors.centerIn: parent
                                text: modelData.label
                                color: parent.isActive ? Theme.textBright : Theme.textSecondary
                                font.family: Theme.monoFamily
                                font.pixelSize: Theme.fontSizeTiny
                            }
                            MouseArea {
                                id: originMa
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: {
                                    if (WindowManager.videoDecoder) {
                                        WindowManager.videoDecoder
                                            .setStartTimecode(modelData.key);
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // FFmpeg-extracted: container "timecode" tag. Almost
            // always matches QT StartTimecode for .mov files but
            // quicker (no exiftool fork).
            RowLayout {
                visible: content.vmeta && content.vmeta.hasEmbeddedTimecode
                Layout.fillWidth: true
                spacing: Theme.spacingLoose
                Text {
                    text: qsTr("Source")
                    color: Theme.textSecondary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontSizeTiny
                    Layout.preferredWidth: 80
                }
                Text {
                    text: content.vmeta ? content.vmeta.embeddedTimecode : ""
                    color: Theme.textPrimary
                    font.pixelSize: Theme.fontSizeSmall
                    font.family: Theme.monoFamily
                }
                Text {
                    visible: content.vmeta && content.vmeta.startFrame !== undefined
                    text: content.vmeta && content.vmeta.startFrame !== undefined
                        ? qsTr("(frame %1)").arg(content.vmeta.startFrame)
                        : ""
                    color: Theme.textSecondary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontSizeTiny
                }
                Item { Layout.fillWidth: true }
            }

            // Adobe-extended QuickTime variants (when present).
            Repeater {
                model: [
                    { label: qsTr("QT Start"),     getter: function() {
                        return content.ameta ? content.ameta.qtStartTimecode : "" } },
                    { label: qsTr("QT TimeCode"),  getter: function() {
                        return content.ameta ? content.ameta.qtTimecode : "" } },
                    { label: qsTr("XMP Alt"),      getter: function() {
                        return content.ameta ? content.ameta.xmpAltTimecode : "" } },
                ]
                RowLayout {
                    visible: modelData.getter().length > 0
                    Layout.fillWidth: true
                    spacing: Theme.spacingLoose
                    Text {
                        text: modelData.label
                        color: Theme.textSecondary
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontSizeTiny
                        Layout.preferredWidth: 80
                    }
                    Text {
                        text: modelData.getter()
                        color: Theme.textPrimary
                        font.pixelSize: Theme.fontSizeSmall
                        font.family: Theme.monoFamily
                    }
                    Item { Layout.fillWidth: true }
                }
            }

            // Creation dates for context.
            Repeater {
                model: [
                    { label: qsTr("Created"),       getter: function() {
                        return content.ameta ? content.ameta.qtCreationDate : "" } },
                    { label: qsTr("Media Created"), getter: function() {
                        return content.ameta ? content.ameta.qtMediaCreateDate : "" } },
                ]
                RowLayout {
                    visible: modelData.getter().length > 0
                    Layout.fillWidth: true
                    spacing: Theme.spacingLoose
                    Text {
                        text: modelData.label
                        color: Theme.textSecondary
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontSizeTiny
                        Layout.preferredWidth: 80
                    }
                    Text {
                        Layout.fillWidth: true
                        text: modelData.getter()
                        color: Theme.textSecondary
                        font.family: Theme.monoFamily
                        font.pixelSize: Theme.fontSizeTiny
                        elide: Text.ElideRight
                    }
                }
            }
        }

        // ---- LINKED PROJECTS ----
        // Adobe project links — pulled from XMP via exiftool. Open
        // launches the linked .aep / .prproj in its native app
        // (After Effects / Premiere); Copy puts the path on the
        // clipboard.
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 4
            visible: content.ameta && content.ameta.hasAnyProject

            SectionHeader { label: qsTr("LINKED PROJECTS") }

            Repeater {
                model: [
                    { label: qsTr("After Effects"),     pathFn: function() {
                        return content.ameta ? content.ameta.aeProjectPath : "" } },
                    { label: qsTr("Premiere (Mac)"),    pathFn: function() {
                        return content.ameta ? content.ameta.premiereMacPath : "" } },
                    { label: qsTr("Premiere (Windows)"),pathFn: function() {
                        return content.ameta ? content.ameta.premiereWinPath : "" } },
                ]
                ColumnLayout {
                    visible: modelData.pathFn().length > 0
                    Layout.fillWidth: true
                    spacing: 1

                    Text {
                        text: modelData.label
                        color: Theme.textSecondary
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontSizeTiny
                    }
                    Text {
                        Layout.fillWidth: true
                        text: WindowManager.toNativeSeparators(modelData.pathFn())
                        color: Theme.textPrimary
                        font.family: Theme.monoFamily
                        font.pixelSize: Theme.fontSizeTiny
                        elide: Text.ElideMiddle
                    }
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 1
                        Layout.topMargin: Theme.spacing
                        color: Theme.divider
                    }
                    RowLayout {
                        spacing: Theme.spacing
                        Layout.topMargin: -1
                        FlatButton {
                            iconName: "arrow-square-out"
                            text: qsTr("Open")
                            onClicked: {
                                const p = modelData.pathFn()
                                if (p) WindowManager.openExternalPath(p)
                            }
                        }
                        FlatButton {
                            iconName: "copy"
                            text: qsTr("Copy")
                            onClicked: WindowManager.copyTextToClipboard(
                                WindowManager.toNativeSeparators(modelData.pathFn()))
                        }
                        FlatButton {
                            iconName: "folder-simple"
                            text: qsTr("Reveal")
                            onClicked: WindowManager.revealInFileManager(modelData.pathFn())
                        }
                        Item { Layout.fillWidth: true }
                    }
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 1
                        Layout.topMargin: -1
                        color: Theme.divider
                    }
                }
            }
        }

        // ---- PLAYLIST CONTENTS (Phase 3.H.1) ----
        // Only visible for MediaType::Playlist (== 4). Lists each
        // entry with name + source duration. Read-only in this stage;
        // editing is the timeline's job. Clicking a row reveals the
        // source MediaItem in the bins.
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 4
            visible: root.itemType === 4
                     && root.displayedItem
                     && root.displayedItem.playlist

            readonly property var pl:
                root.displayedItem ? root.displayedItem.playlist : null

            SectionHeader { label: qsTr("PLAYLIST CONTENTS") }

            // Summary row: clip count + canvas. fps is intentionally
            // hidden — the playlist clock is auto-derived from the
            // first clip's native fps and clips play at their own
            // native fps regardless. Surfacing it would be misleading.
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.paddingLoose
                Text {
                    text: parent.parent.pl
                          ? qsTr("%1 clips").arg(parent.parent.pl.clipCount || 0)
                          : ""
                    color: Theme.textSecondary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontSizeTiny
                }
                Text {
                    text: parent.parent.pl
                          ? parent.parent.pl.canvasWidth + " × "
                            + parent.parent.pl.canvasHeight
                          : ""
                    color: Theme.textSecondary
                    font.family: Theme.monoFamily
                    font.pixelSize: Theme.fontSizeTiny
                }
                Item { Layout.fillWidth: true }
            }

            // Clip list. ProjectManager surfaces each entry resolved
            // to {mediaId, name, path, type, duration, inPoint, outPoint}.
            Repeater {
                model: parent.pl ? parent.pl.items : []

                Rectangle {
                    required property var modelData
                    required property int index
                    Layout.fillWidth: true
                    Layout.preferredHeight: 26
                    color: rowMa.containsMouse ? Theme.surfaceHover : Theme.surface
                    border.color: Theme.border
                    border.width: 1
                    radius: 2

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: Theme.padding
                        anchors.rightMargin: 6
                        spacing: 6

                        // Position number
                        Text {
                            text: (parent.parent.index + 1) + "."
                            color: Theme.textMuted
                            font.family: Theme.monoFamily
                            font.pixelSize: Theme.fontSizeTiny
                            Layout.preferredWidth: 18
                        }
                        Text {
                            Layout.fillWidth: true
                            text: parent.parent.modelData.name || qsTr("(missing)")
                            color: parent.parent.modelData.name
                                   ? Theme.textPrimary : Theme.error
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.fontSizeSmall
                            elide: Text.ElideMiddle
                        }
                        Text {
                            text: {
                                const d = parent.parent.modelData.duration || 0;
                                if (!d) return "";
                                const m = Math.floor(d / 60);
                                const s = Math.floor(d % 60);
                                return ("0" + m).slice(-2) + ":"
                                     + ("0" + s).slice(-2);
                            }
                            color: Theme.textSecondary
                            font.family: Theme.monoFamily
                            font.pixelSize: Theme.fontSizeTiny
                        }
                    }

                    MouseArea {
                        id: rowMa
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: parent.modelData.mediaId
                                     ? Qt.PointingHandCursor : Qt.ArrowCursor
                        onClicked: {
                            // Reveal the source MediaItem in bins —
                            // makes "where did this come from?" a
                            // one-click answer.
                            if (parent.modelData.mediaId
                                && WindowManager.project) {
                                WindowManager.project.setActiveItem(
                                    parent.modelData.mediaId);
                            }
                        }
                    }
                }
            }
        }

    }

}
