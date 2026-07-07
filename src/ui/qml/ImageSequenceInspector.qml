// ImageSequenceInspector — Phase 7.4.a.1.
//
// Right-rail panel for the active image sequence. Persistent, not
// modal: the user can retune fps after import, see the detected
// frame range / pattern, and (in 7.4.b) pick an EXR layer or wire
// up a companion audio file. Visible only when the active item is
// MediaType::ImageSequence.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Qcv

// Rendered as an InspectorCard-style plane (aesthetics pass 1) —
// same Theme.card fill + radius as the metadata cards above it in
// the rail, but it keeps its own collapsible header (caret + format
// chip) since long EXR layer lists are worth folding away.
Rectangle {
    id: root
    color: Theme.card
    radius: Theme.radiusBase
    border.width: 0
    // Inspector exposes a public collapse flag — LeftRail (or any
    // future host) can wire a user toggle to it. Default open so a
    // newly-opened sequence is fully visible without an extra click.
    property bool expanded: true
    // Bind to actual layout positions, not a guessed pad value.
    // The previous "+16" was off by ~22 px (chrome = 6 top margin
    // + 18 header + 12 gap + 8 bottom margin = 44, not 16), which
    // clipped the bottom of the inspector and broke scroll math
    // upstream because parent layouts saw an under-reported height.
    implicitHeight:
        visible
        ? (expanded
            ? (content.y + content.implicitHeight + 8)
            : (header.y + header.height + 6))
        : 0

    // The MediaItem QVariantMap to display. Defaults to the active
    // item; the dual-mode A/B picker in InspectorPanel binds this
    // to its `displayedItem` so the image-seq panel follows the
    // side selection. Layouts that nest both panels in a left rail
    // wire it explicitly:
    //   ImageSequenceInspector { item: inspectorPanel.displayedItem }
    property var item:
        WindowManager.project ? WindowManager.project.activeItem : null

    // Which dual side this panel represents — "A" or "B". Empty
    // string in single mode (or when caller doesn't bind it).
    // Layer-pick clicks dispatch to setDualImageSeqLayer(side, …)
    // when in dual mode + side is non-empty; otherwise to the
    // single-flow setImageSeqLayer.
    property string side: ""

    readonly property var seq:
        item && item.imageSeq ? item.imageSeq : null
    readonly property bool isImageSequence:
        item && item.type === 3   // MediaType::ImageSequence

    visible: isImageSequence

    // ---- Header strip — matches CollapsibleSection: Phosphor
    // caret in the Theme.gutterWidth slot, transparent fill,
    // surfaceHover on hover, single-edge bottom divider when
    // expanded. Format chip ("EXR" / "JPEG" / ...) sits at the
    // right of the row.
    Rectangle {
        id: header
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: Theme.headerHeight
        color: imgSeqHeaderMa.containsMouse
               ? Theme.surfaceHover : "transparent"

        MouseArea {
            id: imgSeqHeaderMa
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: root.expanded = !root.expanded
        }

        RowLayout {
            anchors.fill: parent
            spacing: 0
            Item {
                Layout.preferredWidth: Theme.gutterWidth
                Layout.fillHeight: true
                Icon {
                    anchors.centerIn: parent
                    name: root.expanded ? "caret-down" : "caret-right"
                    size: Theme.iconSizeSmall
                    color: Theme.textSecondary
                }
            }
            // Title in the shared card voice (tiny caps, muted) so
            // this panel reads as a peer of the InspectorCards above
            // it; the old icon + bold-12 header was a third header
            // species the rail didn't need.
            Text {
                Layout.fillWidth: true
                text: qsTr("Image Sequence")
                color: Theme.textMuted
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontSizeTiny
                font.bold: true
                font.capitalization: Font.AllUppercase
                font.letterSpacing: 0.8
            }
            Text {
                text: seq ? (seq.format || "") : ""
                color: Theme.textSecondary
                font.family: Theme.monoFamily
                font.pixelSize: Theme.fontSizeTiny
                Layout.rightMargin: Theme.padding
            }
        }
        Rectangle {
            visible: root.expanded
            anchors.left:   parent.left
            anchors.right:  parent.right
            anchors.bottom: parent.bottom
            height: Theme.dividerWidth
            color:  Theme.divider
        }
    }

    ColumnLayout {
        id: content
        anchors.left:   parent.left
        anchors.right:  parent.right
        anchors.top:    header.bottom
        anchors.margins: Theme.padding
        anchors.topMargin: Theme.padding
        visible: root.expanded
        spacing: Theme.spacingLoose

        // ---- Frame rate — preset chips. The 8 SMPTE / broadcast
        // standards cover ~95% of source material; outliers (47.952,
        // 119.88, custom 22-fps stop-motion, …) get added when a
        // real customer file shows up. Chips wrap into 2-3 rows
        // depending on rail width via KvChipRow's Flow.
        KvChipRow {
            id: fpsRow
            label: qsTr("Frame rate")

            readonly property double currentFps:
                WindowManager.timeline
                ? WindowManager.timeline.frameRate : 0

            Repeater {
                model: ["23.976", "24", "25", "29.97",
                        "30", "50", "59.94", "60"]
                FlatChip {
                    required property var modelData
                    readonly property double presetFps:
                        parseFloat(modelData)
                    minWidth: 48
                    active: Math.abs(presetFps - fpsRow.currentFps) < 0.01
                    label: modelData
                    onClicked: {
                        if (WindowManager.timeline)
                            WindowManager.timeline.setFrameRate(presetFps);
                        // Persist onto the media item so the choice
                        // survives a media switch + project save/
                        // reload (setFrameRate only retunes the live
                        // timeline). Targets the displayed item, so
                        // the dual A/B side picker persists per-side.
                        if (WindowManager.project && root.item && root.item.id)
                            WindowManager.project.setImageSeqFrameRate(
                                root.item.id, presetFps);
                    }
                }
            }
        }

        // ---- Range readout — shared KvRow rules (fixed label
        // column, mono values one size down).
        ColumnLayout {
            Layout.fillWidth: true
            spacing: Theme.spacingTight

            KvRow {
                label: qsTr("Range")
                value: seq ? qsTr("%1 – %2 (%3 frames)")
                               .arg(seq.startFrame)
                               .arg(seq.endFrame)
                               .arg(seq.frameCount)
                           : ""
            }
            KvRow {
                label: qsTr("Pattern")
                value: seq ? seq.pattern : ""
                valueElide: Text.ElideMiddle
            }
            KvRow {
                label: qsTr("Resolution")
                value: seq && seq.width > 0
                      ? qsTr("%1 × %2").arg(seq.width).arg(seq.height)
                      : qsTr("(probing…)")
            }
            // EXR compression — populated at import via the EXR header
            // probe (EXRImageLoader::compressionName). Empty for non-
            // EXR formats and on probe failure; the row collapses out
            // so it doesn't leave a hole in the layout.
            KvRow {
                visible: seq && seq.compression && seq.compression.length > 0
                label: qsTr("Compression")
                value: seq && seq.compression ? seq.compression : ""
            }
            KvRow {
                label: qsTr("Duration")
                value: WindowManager.timeline
                      ? WindowManager.timeline.duration.toFixed(2) + " s"
                      : ""
            }
        }

        Rectangle {
            Layout.fillWidth: true
            height: 1
            color: Theme.divider
        }

        // ---- isEXR predicate referenced by the layer block at the
        // bottom of this column. Computed once here so QML doesn't
        // re-evaluate the seq.format string for every binding.
        readonly property bool isEXR:
            seq && seq.format === "EXR"

        // ---- Cache stride — for heavy sequences (4K EXR) the user
        // can downshift so only every Nth frame is cached.
        // Tooltips mirror the old app's "Every Nth Frame" labels.
        // Non-cached frames render via the closest-cached fallback
        // (getClosestFrame) — playhead still advances 1 frame/tick,
        // only the cached grid is sparse. Behind-buffer collapses
        // to 0 when stride > 1 so all RAM goes forward.
        KvChipRow {
            id: strideRow
            label: qsTr("Cache stride")
            // Current stride for the highlight — per-side in dual (routes
            // to dualImageSeqStride for this panel's A/B side), single
            // flow otherwise. The leading property read is a dependency
            // tag so the binding re-evaluates on imageSeqCacheStrideChanged
            // (dualImageSeqStride is a plain invokable, not a property).
            readonly property int curStride: {
                WindowManager.imageSeqCacheStride;   // dependency tag
                return root.side !== ""
                    ? WindowManager.dualImageSeqStride(root.side)
                    : WindowManager.imageSeqCacheStride;
            }
            Repeater {
                model: [1, 2, 3, 4]
                FlatChip {
                    required property var modelData
                    // Coerce modelData to number — QML 6 hands the
                    // array element through as a QVariant in some
                    // contexts and `=== int` then fails silently,
                    // leaving every chip un-highlighted.
                    readonly property int v: Number(modelData)
                    minWidth: 32
                    active: strideRow.curStride === v
                    label: v + "x"
                    tooltip: v === 1
                          ? qsTr("Every Frame (Full)")
                          : qsTr("Every %1 Frames").arg(v)
                    onClicked: {
                        if (root.side !== "")
                            WindowManager.setDualImageSeqStride(
                                root.side, v);
                        else
                            WindowManager.setImageSeqCacheStride(v);
                    }
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            height: 1
            color: Theme.divider
        }

        // ---- Cache status — pairs with the visual strip on the
        // timeline track, here as a numeric readout for users who
        // want exact counts (debugging buffer behavior, tuning
        // read-ahead in the future settings panel).
        ColumnLayout {
            Layout.fillWidth: true
            spacing: Theme.spacingTight

            KvRow {
                label: qsTr("Cache ahead")
                value: qsTr("%1 / %2 frames")
                          .arg(WindowManager.imageSeqBufferedAhead)
                          .arg(WindowManager.imageSeqReadAheadFrames)
                valueColor: Theme.success
            }
            KvRow {
                label: qsTr("Cache behind")
                value: qsTr("%1 / %2 frames")
                          .arg(WindowManager.imageSeqBufferedBehind)
                          .arg(WindowManager.imageSeqReadBehindFrames)
                valueColor: Qt.darker(Theme.success, 1.5)
            }
        }

        // ---- EXR layer list — anchored at the bottom of the
        // inspector since the list can be arbitrarily long (multi-
        // AOV renders ship dozens). Cryptomatte layers are filtered
        // upstream in EXRImageLoader::discoverLayers, so what you
        // see here is image-bearing layers only.
        Rectangle {
            Layout.fillWidth: true
            height: 1
            color: Theme.divider
            visible: content.isEXR
        }
        Text {
            text: qsTr("Layer")
            color: Theme.textSecondary
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontSizeTiny
            visible: content.isEXR
        }
        ColumnLayout {
            id: layerList
            Layout.fillWidth: true
            spacing: 1
            visible: content.isEXR

            readonly property var entries:
                seq && seq.availableLayers ? seq.availableLayers : []
            readonly property string currentLayer:
                seq && seq.layer ? seq.layer : ""

            Repeater {
                model: layerList.entries
                Item {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 20
                    readonly property string layerName: String(modelData)
                    readonly property bool isCurrent:
                        layerName === layerList.currentLayer

                    Rectangle {
                        anchors.fill: parent
                        color: layerMa.containsMouse ? Theme.surfaceHover : "transparent"
                        radius: 2
                    }
                    // Bullet — filled circle when selected, hollow ring
                    // otherwise. Sized to align with 11 px label
                    // baselines so the row looks deliberate.
                    Rectangle {
                        id: bullet
                        anchors.left: parent.left
                        anchors.leftMargin: 4
                        anchors.verticalCenter: parent.verticalCenter
                        width: 9; height: 9
                        radius: 5
                        color: parent.isCurrent ? Theme.accent : "transparent"
                        border.color: parent.isCurrent ? Theme.accent : Theme.textMuted
                        border.width: 1
                    }
                    Text {
                        anchors.left: bullet.right
                        anchors.leftMargin: Theme.padding
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        text: parent.layerName
                        color: parent.isCurrent ? Theme.textBright : Theme.textPrimary
                        font.pixelSize: Theme.fontSizeMono
                        font.family: Theme.monoFamily
                        elide: Text.ElideMiddle
                    }
                    MouseArea {
                        id: layerMa
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        // In dual mode dispatch to the per-side
                        // dual setter (which flushes the matching
                        // DualImageSeqSource's cache + bumps the
                        // compositor generation). In single mode
                        // fall through to the original
                        // setImageSeqLayer that drives
                        // ImageSequenceCache.
                        onClicked: {
                            const dualMode =
                                WindowManager.compositorMode !== 0;
                            if (dualMode && root.side.length > 0) {
                                WindowManager.setDualImageSeqLayer(
                                    root.side, parent.layerName);
                            } else {
                                WindowManager.setImageSeqLayer(
                                    parent.layerName);
                            }
                        }
                    }
                }
            }
        }
    }
}
