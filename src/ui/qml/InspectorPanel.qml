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

// Transparent container — the RightRail provides the darker well
// (Theme.bg) and each section below renders as a raised InspectorCard
// plane on it (aesthetics pass 1).
Rectangle {
    id: root
    color: "transparent"
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
        function onPixelAspectChanged(itemId, mode, num, den) { root.inspectorPillRev++ }
    }

    // Hide entirely when nothing is selected — the bin's empty-state
    // hint already tells the user to add media.
    visible: hasActive

    // Inspector is now always expanded — the rail's own header
    // already says "Inspector", so the inner accordion shell was
    // redundant. Height is just whatever the body needs. The tail
    // pad is the gap to the next card below (Image Sequence panel)
    // — paddingTight to match the 4px inter-card rhythm.
    implicitHeight: visible
        ? (content.y + content.implicitHeight + Theme.paddingTight)
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

    // Same slugs as LeftRail's bin rows — the hero thumbnail's type
    // badge and the icon-only placeholder both use it.
    function typeIconName(t) {
        switch (t) {
        case 0: return "video";
        case 1: return "music-notes";
        case 2: return "image";
        case 3: return "film-strip";
        case 4: return "playlist";
        case 5: return "frame-corners";
        }
        return "file";
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

    // Section headers are gone — each group below is an
    // InspectorCard whose title carries the tiny-caps voice and
    // whose card edge replaces the old 1-px divider rule.

    // Slim pill row at the top of the panel with the A/B side
    // picker (dual mode only) and the active item's type label.
    // The full styled "Inspector" header was redundant with the
    // right rail's own header — dropped in Stage C polish.
    Item {
        id: headerStrip
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        // Match the cards' 4px rail inset below.
        anchors.leftMargin: Theme.paddingTight
        anchors.rightMargin: Theme.paddingTight
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
        // Tight float (margins pass 2): 4px rail-edge insets + 4px
        // gaps between cards, matching the left rail.
        anchors.margins: Theme.paddingTight
        spacing: Theme.paddingTight

        // ---- File section ----
        InspectorCard {
            id: fileCard
            title: qsTr("File")

            // Path hidden for media types that don't reference an
            // on-disk file — currently Playlist (4) and DualPair (5);
            // both are pure pool aggregates with no path.
            readonly property bool hasOnDiskPath:
                root.itemType !== 4 && root.itemType !== 5

            // Hero thumbnail — full-width preview of the displayed
            // item (A or B follows the side picker via displayedItem).
            // Decoded off-thread by ThumbnailImageProvider; the tile
            // shows a spinner while pending and falls back to a
            // centered type icon for sources with no picture (audio,
            // playlist, dual pair) or failed decodes.
            Rectangle {
                id: heroThumb
                Layout.fillWidth: true
                Layout.preferredHeight: Math.round(width * 9 / 16)
                Layout.bottomMargin: 2
                visible: root.hasActive
                radius: Theme.radiusSmall
                color: Theme.surfaceRecess
                clip: true

                readonly property string thumbUrl: {
                    if (!root.hasActive) return "";
                    const item = root.displayedItem;
                    if (root.itemType === 0 && item.path) {
                        // Video → mid-clip poster (first frames are
                        // routinely black leaders).
                        return "image://thumb/"
                            + encodeURIComponent(item.path)
                            + "?frame=mid";
                    }
                    if (root.itemType === 2 && item.path) {
                        return "image://thumb/"
                            + encodeURIComponent(item.path);
                    }
                    if (root.itemType === 3 && item.imageSeq
                            && item.imageSeq.firstFramePath) {
                        let u = "image://thumb/" + encodeURIComponent(
                            item.imageSeq.firstFramePath);
                        if (item.imageSeq.layer) {
                            u += "?layer=" + encodeURIComponent(
                                item.imageSeq.layer);
                        }
                        return u;
                    }
                    return "";   // audio / playlist / dual pair
                }
                readonly property bool hasPicture:
                    heroImage.status === Image.Ready

                Image {
                    id: heroImage
                    anchors.fill: parent
                    fillMode: Image.PreserveAspectFit
                    asynchronous: true
                    source: heroThumb.thumbUrl
                    sourceSize.width: 480
                }

                // No-picture placeholder (also covers decode errors).
                Icon {
                    anchors.centerIn: parent
                    visible: !heroThumb.hasPicture
                             && heroImage.status !== Image.Loading
                    name: root.typeIconName(root.itemType)
                    size: Theme.iconSizeLarge
                    color: Theme.textMuted
                }
                MiniSpinner {
                    anchors.centerIn: parent
                    running: heroImage.status === Image.Loading
                }

                // Media-type badge — bottom-right over the picture.
                Rectangle {
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    anchors.margins: 4
                    width: 22; height: 22
                    radius: Theme.radiusSmall
                    color: Qt.rgba(0, 0, 0, 0.55)
                    visible: heroThumb.hasPicture
                    Icon {
                        anchors.centerIn: parent
                        name: root.typeIconName(root.itemType)
                        size: 14
                        color: Theme.textPrimary
                    }
                }
            }

            // Name (editable later in 3.D when MediaModel::update lands)
            Text {
                Layout.fillWidth: true
                text: root.hasActive ? root.displayedItem.name : ""
                color: Theme.textPrimary
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontSizeBase
                elide: Text.ElideMiddle
            }

            Text {
                Layout.fillWidth: true
                visible: fileCard.hasOnDiskPath
                text: root.hasActive
                    ? WindowManager.toNativeSeparators(root.displayedItem.path)
                    : ""
                color: Theme.textSecondary
                font.family: Theme.monoFamily
                font.pixelSize: Theme.fontSizeMono
                elide: Text.ElideMiddle
                wrapMode: Text.NoWrap
            }

            // Stat row: size, duration
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.paddingLoose
                Text {
                    text: root.hasActive
                        ? root.formatSize(root.displayedItem.sizeBytes || 0)
                        : ""
                    color: Theme.textSecondary
                    font.family: Theme.monoFamily
                    font.pixelSize: Theme.fontSizeMono
                }
                Text {
                    text: root.hasActive
                        ? root.formatDuration(root.displayedItem.duration || 0)
                        : ""
                    color: Theme.textSecondary
                    font.family: Theme.monoFamily
                    font.pixelSize: Theme.fontSizeMono
                }
                Item { Layout.fillWidth: true }
            }

            // Path actions — the card edge provides the grouping the
            // old divider sandwich used to.
            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: Theme.spacing
                spacing: 4
                visible: fileCard.hasOnDiskPath

                FlatButton {
                    variant: "raised"
                    Layout.preferredHeight: 24
                    iconName: "copy"
                    iconSize: Theme.iconSizeSmall
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
                    variant: "raised"
                    Layout.preferredHeight: 24
                    iconName: "folder-simple"
                    iconSize: Theme.iconSizeSmall
                    text: qsTr("Reveal")
                    onClicked: {
                        if (root.displayedItem && root.displayedItem.path) {
                            WindowManager.revealInFileManager(root.displayedItem.path)
                        }
                    }
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
        InspectorCard {
            title: qsTr("Video")
            visible: content.videoLoaded
                     && content.vmeta
                     && content.vmeta.width > 0

            KvRow {
                label: qsTr("Resolution")
                value: content.vmeta
                    ? content.vmeta.width + " × " + content.vmeta.height
                    : ""
            }
            KvRow {
                label: qsTr("Frame rate")
                value: content.vmeta && content.vmeta.frameRate > 0
                    ? content.vmeta.frameRate.toFixed(3) + " fps"
                    : ""
            }
            KvRow {
                label: qsTr("Total frames")
                value: content.vmeta ? content.vmeta.totalFrames : ""
            }
            KvRow {
                label: qsTr("Codec")
                value: content.vmeta ? content.vmeta.videoCodec : ""
            }
            KvRow {
                label: qsTr("Pixel format")
                value: content.vmeta ? content.vmeta.pixelFormat : ""
            }
            KvRow {
                label: qsTr("Bit depth")
                value: content.vmeta
                    ? content.vmeta.bitDepth + qsTr(" bit") +
                      (content.vmeta.hasAlpha ? qsTr(" + alpha") : "")
                    : ""
            }
            KvRow {
                visible: content.vmeta && content.vmeta.isHdrContent === true
                label: qsTr("HDR")
                value: content.vmeta && content.vmeta.isHdrContent
                    ? qsTr("Yes — ") + content.vmeta.colorTransfer
                    : ""
                valueColor: Theme.warn
                monoValue: false
            }
        }

        // ---- PIXEL ASPECT ----
        // Per-clip anamorphic / non-square-pixel override. Default
        // Square renders the stored pixels untouched (QC-safe); the
        // probed SAR pre-fills the read-out and Detected applies it;
        // Custom takes a pixel-aspect scalar (AE term) or a display
        // aspect W:H. Mirrors the Range pill's target-resolution +
        // pickable rules; setPixelAspect persists + pushes live.
        InspectorCard {
            id: parSection
            title: qsTr("Pixel Aspect")
            bodySpacing: 4
            visible: content.videoLoaded && content.vmeta
                     && content.vmeta.width > 0
                     && root.itemType === 0   // Video only

            // The MediaItem the pills edit + read (same resolution as
            // the Range pill: dual → displayedItem, else the routing
            // scope id — the active clip's source, playlist-aware).
            readonly property string parTargetItemId: {
                if (root.dualActive)
                    return root.displayedItem ? root.displayedItem.id : "";
                return WindowManager
                    ? WindowManager.audioRoutingScopeMediaItemId : "";
            }
            readonly property var parTargetItem: {
                root.inspectorPillRev;   // dependency tag — see root
                if (!parTargetItemId || !WindowManager.project) return null;
                return WindowManager.project.mediaItemMap(parTargetItemId);
            }
            readonly property bool parPickable:
                root.itemType === 0 && WindowManager.videoDecoder
                && root.activeItem && root.displayedItem
                && root.activeItem.id === root.displayedItem.id

            readonly property int parMode:
                parTargetItem && parTargetItem.pixelAspectMode !== undefined
                ? parTargetItem.pixelAspectMode : 0
            readonly property int storageW: content.vmeta ? content.vmeta.width : 0
            readonly property int storageH: content.vmeta ? content.vmeta.height : 0
            readonly property int detSarNum:
                content.vmeta && content.vmeta.sarNum ? content.vmeta.sarNum : 1
            readonly property int detSarDen:
                content.vmeta && content.vmeta.sarDen ? content.vmeta.sarDen : 1
            readonly property bool isAnamorphic:
                content.vmeta ? (content.vmeta.isAnamorphic === true) : false

            // Currently-applied PAR (num/den) resolved from the mode.
            readonly property int curParNum:
                parMode === 1 ? detSarNum
              : (parMode === 2 && parTargetItem && parTargetItem.customParNum
                 ? parTargetItem.customParNum : 1)
            readonly property int curParDen:
                parMode === 1 ? detSarDen
              : (parMode === 2 && parTargetItem && parTargetItem.customParDen
                 ? parTargetItem.customParDen : 1)
            readonly property real curPar:
                curParDen > 0 ? (curParNum / curParDen) : 1.0
            readonly property real curDar:
                (storageH > 0 && curParDen > 0)
                ? (storageW * curParNum) / (storageH * curParDen) : 0.0
            readonly property int effW:
                Math.round(storageW * (curParDen > 0 ? curParNum / curParDen : 1))

            // Mode pill row — Square / Detected (R:R) / Custom.
            KvChipRow {
                label: qsTr("Aspect")
                Repeater {
                    model: [
                        { key: 0, label: qsTr("Square") },
                        { key: 1, label: qsTr("Detected") },
                        { key: 2, label: qsTr("Custom") },
                    ]
                    FlatChip {
                        id: parChip
                        required property var modelData
                        // Detected is meaningless on square-pixel
                        // content — disable it there.
                        readonly property bool chipDisabled:
                            modelData.key === 1 && !parSection.isAnamorphic
                        active: modelData.key === parSection.parMode
                        interactive: parSection.parPickable && !chipDisabled
                        label: modelData.key === 1 && parSection.isAnamorphic
                            ? qsTr("Detected ") + parSection.detSarNum
                              + ":" + parSection.detSarDen
                            : modelData.label
                        onClicked: {
                            if (!parSection.parTargetItemId
                                || !WindowManager.project) return;
                            // Seed Custom from the detected ratio
                            // (or 1:1) so its fields start sensible.
                            var n = parSection.curParNum;
                            var d = parSection.curParDen;
                            if (parChip.modelData.key === 2
                                && parSection.parMode !== 2) {
                                n = parSection.isAnamorphic
                                    ? parSection.detSarNum : 1;
                                d = parSection.isAnamorphic
                                    ? parSection.detSarDen : 1;
                            }
                            WindowManager.project.setPixelAspect(
                                parSection.parTargetItemId,
                                parChip.modelData.key, n, d);
                        }
                    }
                }
            }

            // Read-out — detected SAR + resulting display aspect / dims.
            KvRow {
                label: qsTr("Detected")
                value: parSection.isAnamorphic
                    ? parSection.detSarNum + ":" + parSection.detSarDen
                      + qsTr(" (non-square)")
                    : qsTr("1:1 (square)")
            }
            KvRow {
                label: qsTr("Display")
                value: parSection.curDar > 0
                    ? parSection.curDar.toFixed(3) + ":1   ("
                      + parSection.effW + " × " + parSection.storageH + ")"
                    : ""
            }

            // Custom entry — pixel-aspect scalar (AE term) and a
            // display-aspect W:H pair. Either commits the same stored
            // PAR rational on Enter / focus-loss; the other reflects it.
            ColumnLayout {
                Layout.fillWidth: true
                Layout.topMargin: 2
                spacing: 4
                visible: parSection.parMode === 2

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.spacingLoose
                    Text {
                        text: qsTr("Pixel aspect")
                        color: Theme.textSecondary
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontSizeTiny
                        Layout.preferredWidth: Theme.labelColWidth
                    }
                    TextField {
                        id: parScalarField
                        Layout.preferredWidth: 90
                        text: parSection.curPar.toFixed(4)
                        font.family: Theme.monoFamily
                        font.pixelSize: Theme.fontSizeMono
                        selectByMouse: true
                        inputMethodHints: Qt.ImhFormattedNumbersOnly
                        onEditingFinished: {
                            var p = parseFloat(text);
                            if (p > 0 && WindowManager.project
                                && parSection.parTargetItemId) {
                                // Clamp to a sane range so a typo can't
                                // band the image; rationalize at 1e4 —
                                // exact enough for any real PAR.
                                p = Math.min(20.0, Math.max(0.05, p));
                                WindowManager.project.setPixelAspect(
                                    parSection.parTargetItemId, 2,
                                    Math.round(p * 10000), 10000);
                            }
                        }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.spacingLoose
                    Text {
                        text: qsTr("Display aspect")
                        color: Theme.textSecondary
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontSizeTiny
                        Layout.preferredWidth: Theme.labelColWidth
                    }
                    // Display aspect as a single decimal (e.g. 2.390 for
                    // a 32:9 frame). A single scalar round-trips to a
                    // fixed point — PAR = DAR*(storageH/storageW), then
                    // DAR re-derives to the same value — so a focus-steal
                    // commit (e.g. clicking the Safety Guides dropdown) is
                    // a harmless no-op. The old W:H pair fed one live-bound
                    // field back into the other and ran the PAR away.
                    TextField {
                        id: darField
                        Layout.preferredWidth: 90
                        text: parSection.curDar > 0
                              ? parSection.curDar.toFixed(4) : ""
                        font.family: Theme.monoFamily
                        font.pixelSize: Theme.fontSizeMono
                        selectByMouse: true
                        inputMethodHints: Qt.ImhFormattedNumbersOnly
                        onEditingFinished: {
                            var d = parseFloat(text);
                            if (!(d > 0) || parSection.storageW <= 0
                                || parSection.storageH <= 0
                                || !WindowManager.project
                                || !parSection.parTargetItemId) return;
                            d = Math.min(20.0, Math.max(0.05, d));
                            // 1e3 scale keeps PAR an exact rational.
                            WindowManager.project.setPixelAspect(
                                parSection.parTargetItemId, 2,
                                Math.round(d * parSection.storageH * 1000),
                                Math.round(parSection.storageW * 1000));
                        }
                    }
                    Text { text: ": 1"; color: Theme.textSecondary
                           font.family: Theme.monoFamily; font.pixelSize: Theme.fontSizeMono }
                }
            }
        }

        // ---- COLOR ----
        InspectorCard {
            title: qsTr("Color")
            visible: content.videoLoaded
                     && content.vmeta
                     && (content.vmeta.colorspace.length > 0
                         || content.vmeta.colorPrimaries.length > 0
                         || content.vmeta.colorTransfer.length > 0
                         || content.vmeta.colorRange.length > 0)

            KvRow {
                visible: content.vmeta && content.vmeta.colorspace.length > 0
                label: qsTr("Colorspace")
                value: content.vmeta ? content.vmeta.colorspace : ""
            }
            KvRow {
                visible: content.vmeta && content.vmeta.colorPrimaries.length > 0
                label: qsTr("Primaries")
                value: content.vmeta ? content.vmeta.colorPrimaries : ""
            }
            KvRow {
                visible: content.vmeta && content.vmeta.colorTransfer.length > 0
                label: qsTr("Transfer")
                value: content.vmeta ? content.vmeta.colorTransfer : ""
            }
            KvRow {
                visible: content.vmeta && content.vmeta.nclcTag.length > 0
                label: qsTr("NCLC tag")
                value: content.vmeta ? content.vmeta.nclcTag : ""
            }

            // ---- Range override picker (Phase 3.G) ----
            // Per Guide 07 D10, range is per-clip-editable. Auto
            // follows FFmpeg's detected `colorRange`; Full / Limited
            // override the YUV→RGB conversion. Only enabled for the
            // loaded source — same UX rule as the Phase 3.F Origin
            // picker (changing it on a non-loaded item just sets a
            // flag with no visible effect until the user loads it).
            KvChipRow {
                id: rangeRow
                label: qsTr("Range")
                Layout.topMargin: 4
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

                Repeater {
                    model: [
                        { key: 0, useDetected: true,  label: qsTr("Auto") },
                        { key: 1, useDetected: false, label: qsTr("Full") },
                        { key: 2, useDetected: false, label: qsTr("Limited") },
                    ]

                    FlatChip {
                        required property var modelData
                        active: modelData.key === rangeRow.activeRange
                        interactive: rangeRow.rangePickable
                        label: modelData.useDetected ? rangeRow.detectedLabel
                                                     : modelData.label
                        onClicked: {
                            // Mutate the resolved target — active
                            // clip's source MediaItem in playlist
                            // mode, displayedItem otherwise. C++
                            // side's videoRangeOverrideChanged signal
                            // pushes the value into whichever live
                            // decoder owns it (single-flow
                            // VideoDecoder OR DualPlaybackController
                            // per-side range atomic), so we don't
                            // call setRangeOverride here anymore —
                            // that previously double-applied for
                            // single-flow and missed entirely for
                            // dual.
                            const targetId = rangeRow.rangeTargetItemId;
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

        // ---- AUDIO ----
        // Metadata readout + per-clip routing-mode pills + per-source-
        // channel level meters. Mode pills only show choices the
        // source has channels to support (5.1 Downmix needs ≥6,
        // Stereo 7-8 needs ≥8). For mono/stereo sources only the
        // metadata + meter rows render — there's nothing meaningful
        // to choose.
        InspectorCard {
            id: audioSection
            title: qsTr("Audio")
            visible: content.videoLoaded
                     && content.vmeta
                     && content.vmeta.audioCodec.length > 0

            KvRow {
                label: qsTr("Codec")
                value: content.vmeta ? content.vmeta.audioCodec : ""
            }
            KvRow {
                label: qsTr("Sample rate")
                value: content.vmeta && content.vmeta.audioSampleRate > 0
                    ? content.vmeta.audioSampleRate + qsTr(" Hz") : ""
            }
            KvRow {
                label: qsTr("Channels")
                value: content.vmeta && content.vmeta.audioChannels > 0
                    ? content.vmeta.audioChannels +
                      (content.vmeta.audioChannels === 2 ? qsTr(" (stereo)")
                      : content.vmeta.audioChannels === 1 ? qsTr(" (mono)") : "")
                    : ""
            }
            KvRow {
                visible: content.vmeta && content.vmeta.audioChannelLayoutName
                         && content.vmeta.audioChannelLayoutName.length > 0
                label: qsTr("Layout")
                value: content.vmeta ? content.vmeta.audioChannelLayoutName : ""
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

            KvChipRow {
                label: qsTr("Mix")
                Layout.topMargin: Theme.spacing
                visible: audioSection.routingPicker
                // Auto pill — always present.
                Repeater {
                    model: {
                        // Build the visible mode list per source. Auto
                        // is unconditional; Downmix5_1 needs ≥6 channels;
                        // Stereo7_8 needs ≥8.
                        const ch = audioSection.audioCh;
                        const list = [{ mode: 0, label: "Auto" }];
                        if (ch >= 6) list.push({ mode: 1, label: "5.1 → 2" });
                        if (ch >= 8) list.push({ mode: 2, label: "7-8" });
                        return list;
                    }
                    FlatChip {
                        required property var modelData
                        minWidth: 50
                        active: audioSection.currentMode === modelData.mode
                        label: modelData.label
                        tooltip: modelData.mode === 0
                            ? qsTr("Auto — picks the right mix per source")
                            : modelData.mode === 1
                            ? qsTr("5.1 → Stereo (BS.775 downmix from channels 1-6)")
                            : qsTr("Channels 7-8 (broadcast stereo bounce)")
                        onClicked: {
                            // Click mutates the routing TARGET (the
                            // active clip's source MediaItem in
                            // playlist mode, NOT the playlist item).
                            // Without this, the inspector pill
                            // silently edited the playlist's own
                            // audioRoutingMode field which wasn't the
                            // field the decoder reads on clip
                            // transition.
                            const targetId =
                                audioSection.routingTargetItemId;
                            if (targetId && WindowManager.project) {
                                WindowManager.project.setAudioRoutingMode(
                                    targetId, modelData.mode);
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

        InspectorCard {
            id: timecodeSection
            title: qsTr("Timecode")
            visible: content.hasAnyTcLine

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

            KvChipRow {
                label: qsTr("Origin")
                // tcPickable / tcButtonModel / activeTc live on
                // timecodeSection (the card above). The earlier
                // `content.tcPickable` was an outer-id miss that
                // QML6 strict-scope reports as "Unable to assign
                // [undefined] to bool" — visible defaulted to true
                // so the row stayed shown but the Repeater model was
                // undefined and the picker was empty.
                visible: timecodeSection.tcPickable

                Repeater {
                    model: timecodeSection.tcButtonModel()

                    FlatChip {
                        required property var modelData
                        active: modelData.key === timecodeSection.activeTc
                        label: modelData.label
                        onClicked: {
                            if (WindowManager.videoDecoder) {
                                WindowManager.videoDecoder
                                    .setStartTimecode(modelData.key);
                            }
                        }
                    }
                }
            }

            // FFmpeg-extracted: container "timecode" tag. Almost
            // always matches QT StartTimecode for .mov files but
            // quicker (no exiftool fork).
            KvRow {
                visible: content.vmeta && content.vmeta.hasEmbeddedTimecode
                label: qsTr("Source")
                value: content.vmeta ? content.vmeta.embeddedTimecode : ""
                note: content.vmeta && content.vmeta.startFrame !== undefined
                    ? qsTr("(frame %1)").arg(content.vmeta.startFrame)
                    : ""
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
                KvRow {
                    required property var modelData
                    visible: modelData.getter().length > 0
                    label: modelData.label
                    value: modelData.getter()
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
                KvRow {
                    required property var modelData
                    visible: modelData.getter().length > 0
                    label: modelData.label
                    value: modelData.getter()
                    valueColor: Theme.textSecondary
                }
            }
        }

        // ---- LINKED PROJECTS ----
        // Adobe project links — pulled from XMP via exiftool. Open
        // launches the linked .aep / .prproj in its native app
        // (After Effects / Premiere); Copy puts the path on the
        // clipboard.
        InspectorCard {
            title: qsTr("Linked Projects")
            bodySpacing: 4
            visible: content.ameta && content.ameta.hasAnyProject

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
                        font.pixelSize: Theme.fontSizeMono
                        elide: Text.ElideMiddle
                    }
                    // Action row — the card groups this block now;
                    // the old divider sandwich is gone.
                    RowLayout {
                        spacing: Theme.spacing
                        Layout.topMargin: 2
                        FlatButton {
                            variant: "raised"
                            Layout.preferredHeight: 24
                            iconName: "arrow-square-out"
                            iconSize: Theme.iconSizeSmall
                            text: qsTr("Open")
                            onClicked: {
                                const p = modelData.pathFn()
                                if (p) WindowManager.openExternalPath(p)
                            }
                        }
                        FlatButton {
                            variant: "raised"
                            Layout.preferredHeight: 24
                            iconName: "copy"
                            iconSize: Theme.iconSizeSmall
                            text: qsTr("Copy")
                            onClicked: WindowManager.copyTextToClipboard(
                                WindowManager.toNativeSeparators(modelData.pathFn()))
                        }
                        FlatButton {
                            variant: "raised"
                            Layout.preferredHeight: 24
                            iconName: "folder-simple"
                            iconSize: Theme.iconSizeSmall
                            text: qsTr("Reveal")
                            onClicked: WindowManager.revealInFileManager(modelData.pathFn())
                        }
                        Item { Layout.fillWidth: true }
                    }
                }
            }
        }

        // ---- PLAYLIST CONTENTS (Phase 3.H.1) ----
        // Only visible for MediaType::Playlist (== 4). Lists each
        // entry with name + source duration, and doubles as a tracker:
        // the clip currently under the playhead is highlighted (bound
        // to WindowManager.playlistCurrentItemIndex). Clicking a row
        // seeks to that clip's first frame in the playlist (play/pause
        // preserved); the open button on the right loads the source in
        // single mode (the old row behavior).
        InspectorCard {
            id: playlistCard
            title: qsTr("Playlist Contents")
            bodySpacing: 4
            visible: root.itemType === 4
                     && root.displayedItem
                     && root.displayedItem.playlist

            readonly property var pl:
                root.displayedItem ? root.displayedItem.playlist : null

            // Summary row: clip count + canvas. fps is intentionally
            // hidden — the playlist clock is auto-derived from the
            // first clip's native fps and clips play at their own
            // native fps regardless. Surfacing it would be misleading.
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.paddingLoose
                Text {
                    text: playlistCard.pl
                          ? qsTr("%1 clips").arg(playlistCard.pl.clipCount || 0)
                          : ""
                    color: Theme.textSecondary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontSizeTiny
                }
                Text {
                    text: playlistCard.pl
                          ? playlistCard.pl.canvasWidth + " × "
                            + playlistCard.pl.canvasHeight
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
                model: playlistCard.pl ? playlistCard.pl.items : []

                Rectangle {
                    id: clipRow
                    required property var modelData
                    required property int index
                    // Active-clip tracker — the playlist clip currently
                    // under the playhead (mapped past timeline gaps in
                    // WindowManager).
                    readonly property bool isCurrentClip:
                        WindowManager.playlistCurrentItemIndex === clipRow.index
                    Layout.fillWidth: true
                    Layout.preferredHeight: 26
                    // Recessed-by-tone rows (surfaceRecess on the
                    // card plane) — the per-row border is gone; only
                    // the playing clip keeps an accent edge.
                    color: clipRow.isCurrentClip
                           ? Theme.rowActive
                           : (rowMa.containsMouse ? Theme.surfaceHover
                                                  : Theme.surfaceRecess)
                    border.color: Theme.accent
                    border.width: clipRow.isCurrentClip ? 1 : 0
                    radius: Theme.radiusSmall

                    // Row click seeks to this clip's first frame in the
                    // playlist (works whether playing or paused).
                    // Declared first so the open button below sits on
                    // top and intercepts its own clicks; the text items
                    // are non-interactive and fall through to here.
                    MouseArea {
                        id: rowMa
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: WindowManager.seekToPlaylistItem(clipRow.index)
                    }

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: Theme.padding
                        // Reserve room on the right for the open button.
                        anchors.rightMargin: 28
                        spacing: 6

                        // Position number
                        Text {
                            text: (clipRow.index + 1) + "."
                            color: clipRow.isCurrentClip ? Theme.textBright
                                                         : Theme.textMuted
                            font.family: Theme.monoFamily
                            font.pixelSize: Theme.fontSizeTiny
                            Layout.preferredWidth: 18
                        }
                        Text {
                            Layout.fillWidth: true
                            text: clipRow.modelData.name || qsTr("(missing)")
                            color: clipRow.modelData.name
                                   ? (clipRow.isCurrentClip ? Theme.textBright
                                                            : Theme.textPrimary)
                                   : Theme.error
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.fontSizeSmall
                            elide: Text.ElideMiddle
                        }
                        Text {
                            text: {
                                const d = clipRow.modelData.duration || 0;
                                if (!d) return "";
                                const m = Math.floor(d / 60);
                                const s = Math.floor(d % 60);
                                return ("0" + m).slice(-2) + ":"
                                     + ("0" + s).slice(-2);
                            }
                            color: clipRow.isCurrentClip ? Theme.info
                                                         : Theme.textSecondary
                            font.family: Theme.monoFamily
                            font.pixelSize: Theme.fontSizeTiny
                        }
                    }

                    // Open-in-single-mode button — mirrors the old row
                    // behavior (the row click now seeks instead).
                    // Declared after rowMa so its MouseArea wins inside
                    // its own bounds.
                    Rectangle {
                        anchors.right: parent.right
                        anchors.rightMargin: 3
                        anchors.verticalCenter: parent.verticalCenter
                        width: 22
                        height: 22
                        radius: Theme.radiusSmall
                        visible: clipRow.modelData.mediaId
                        color: openMa.containsMouse ? Theme.surfaceHover
                                                    : "transparent"
                        Icon {
                            anchors.centerIn: parent
                            name: "arrow-square-out"
                            size: Theme.iconSizeSmall
                            color: openMa.containsMouse
                                   ? Theme.accent
                                   : (clipRow.isCurrentClip ? Theme.textBright
                                                            : Theme.textSecondary)
                        }
                        MouseArea {
                            id: openMa
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                if (clipRow.modelData.mediaId
                                    && WindowManager.project) {
                                    WindowManager.project.setActiveItem(
                                        clipRow.modelData.mediaId);
                                }
                            }
                            FlatToolTip {
                                visible: openMa.containsMouse
                                text: qsTr("Open in single mode")
                            }
                        }
                    }
                }
            }
        }

    }

}
