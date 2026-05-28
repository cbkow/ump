// ColorPanel — Phase 2.5 (D18 redesign).
//
// Per Guide 05 §2 + D18: ComboBox dropdowns are unusable above a
// native QQuickWindow child (the player), so each column is an
// always-visible vertical scroll list ("reel"). The panel takes
// substantial vertical space — Guide 03 sets ~360 px when open.
//
// Layout:
//   ┌─────────────────────────────────────────────────────────────────┐
//   │ [Preset row: ◀ Standard ▶]   [Save] [Save as…] [Manage…]        │  preset bar
//   ├─────────────────────────────────────────────────────────────────┤
//   │ ┌Preset─┐┌Config─┐┌Input──┐┌Look───┐┌SceneLUT┐ → ┌Output┐┌View┐┌DispLUT┐│  reel grid
//   │ │ list  ││ list  ││ list  ││ list  ││ tile   │   │ list ││list││ tile  ││
//   │ │       ││       ││       ││       ││        │   │      ││    ││       ││
//   │ │       ││       ││       ││       ││        │   │      ││    ││       ││
//   │ └───────┘└───────┘└───────┘└───────┘└────────┘   └──────┘└────┘└───────┘│
//   ├─────────────────────────────────────────────────────────────────┤
//   │ [⏼ Bypass OCIO]                                  [Export LUT…]  │  bottom row
//   └─────────────────────────────────────────────────────────────────┘
//
// Phase 2.5 wiring:
//   ✓ Input / Look / Output / View — live reels
//   ✓ Scene LUT / Display LUT — file-picker tiles (native dialog,
//     stacks above the player correctly)
//   ✓ Bypass — toggle button
//   ◌ Preset reel — only "None (Passthrough)" entry until presets land
//   ◌ Config reel — current config name only until config-switching
//     lands (Phase 2.5 follow-up)
//   ◌ Save / Save as… / Manage… / Export LUT — visible-but-disabled
//     placeholders so the layout reads complete

import QtCore
import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import QtQuick.Window
import Qcv

Pane {
    id: root
    padding: 0

    background: Rectangle {
        // Surface is one shade up from Theme.bg so the inner reels
        // (which use Theme.bg) read as recessed against the panel.
        // No top divider — the VResizeHandle above is itself a 1-px
        // Theme.divider line; doubling it would render as 2 px.
        color: Theme.surface
    }

    // Persistent expand/collapse for LUT tile columns. The two LUT
    // pickers eat horizontal space but are used less often than the
    // OCIO reels; default both to collapsed so the OCIO chain gets
    // the room. The collapsed strip is still a clear affordance —
    // rotated title + identity icon + status border.
    Settings {
        id: lutTileSettings
        category: "color"
        property bool sceneLutExpanded: false
        property bool displayLutExpanded: false
        property bool lookExpanded: false        // Phase 2.5 polish: Look collapsed by default
    }

    // Shared LUT picker, target-routed.
    property string lutPickerTarget: ""

    FileDialog {
        id: lutPicker
        title: qsTr("Choose LUT")
        nameFilters: [
            qsTr("LUT files (*.cube *.3dl *.csp)"),
            qsTr("All files (*)")
        ]
        fileMode: FileDialog.OpenFile
        onAccepted: {
            const path = WindowManager.urlToOsPath(selectedFile);
            if (root.lutPickerTarget === "scene") {
                WindowManager.ocio.activeSceneLutPath = path;
            } else if (root.lutPickerTarget === "display") {
                WindowManager.ocio.activeDisplayLutPath = path;
            }
        }
    }

    function pickLut(target) {
        root.lutPickerTarget = target;
        lutPicker.open();
    }

    // Save-as for "Export LUT…" — bakes the active OCIO chain into
    // a 65³ .cube via OCIOConfigManager::exportLut. Independent of
    // engaged state; only requires a configured chain.
    FileDialog {
        id: exportLutDialog
        title: qsTr("Export LUT…")
        fileMode: FileDialog.SaveFile
        defaultSuffix: "cube"
        nameFilters: [qsTr("3D LUT (*.cube)")]
        onAccepted: {
            const path = WindowManager.urlToOsPath(selectedFile);
            const err = WindowManager.ocio.exportLut(path, 65);
            if (err && err.length > 0) {
                console.warn("Export LUT failed:", err);
            } else {
                console.log("Export LUT: wrote", path);
            }
        }
    }
    function basenameOf(path) {
        if (!path) return "";
        const idx = Math.max(path.lastIndexOf("/"), path.lastIndexOf("\\"));
        return idx >= 0 ? path.substring(idx + 1) : path;
    }


    // Phase 2.5e.2 — Save as… as a top-level Window, not a Popup.
    // QML Popups live inside the UI window's scene graph and end up
    // BEHIND the native player child window on macOS (same root
    // cause as Guide 05 D18 — child windows stack above QML popups).
    // A Window element is its own top-level OS window and stacks
    // correctly above the player.
    Window {
        id: saveAsDialog
        flags: Qt.Dialog
        modality: Qt.ApplicationModal
        title: qsTr("Save preset as…")
        width: 380
        height: 160
        color: Theme.bg

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 16
            spacing: 10

            Text {
                text: qsTr("Save preset as…")
                color: Theme.textPrimary
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontSizeMedium
                font.bold: true
            }
            TextField {
                id: saveAsField
                Layout.fillWidth: true
                placeholderText: qsTr("Preset name")
                onAccepted: saveAsDialog.commit()
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spacingLoose
                Item { Layout.fillWidth: true }
                FlatButton {
                    text: qsTr("Cancel")
                    onClicked: saveAsDialog.close()
                }
                FlatButton {
                    text: qsTr("Save")
                    variant: "primary"
                    iconName: "floppy-disk"
                    enabled: saveAsField.text.trim().length > 0
                    onClicked: saveAsDialog.commit()
                }
            }
        }

        function open() {
            saveAsField.text = WindowManager.presets
                && WindowManager.presets.activePresetName.length > 0
                ? WindowManager.presets.activePresetName + " (copy)"
                : "";
            show();
            requestActivate();
            saveAsField.forceActiveFocus();
        }
        function commit() {
            const name = saveAsField.text.trim();
            if (!name) return;
            if (WindowManager.presets.saveAs(name)) {
                close();
            }
            // saveAs logs a warning on failure; user can pick another
            // name. Inline error surface is a polish step.
        }
    }

    ColumnLayout {
        anchors.fill: parent
        // Toolbars flush to the panel top/bottom edges (no gap
        // above the preset bar or below the engage row). The reel
        // grid in the middle owns its own vertical breathing via
        // Layout.topMargin / bottomMargin.
        anchors.margins: 0
        spacing: 0

        // ---- Preset bar (top)
        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: Theme.spacingLoose
            Layout.rightMargin: 0
            spacing: Theme.spacingLoose

            Text {
                text: qsTr("Color")
                color: Theme.textPrimary
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontSizeMedium
                font.bold: true
            }
            Text {
                text: WindowManager.ocio
                      ? WindowManager.ocio.configDescription : ""
                color: Theme.textMuted
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontSizeTiny
                elide: Text.ElideRight
                Layout.fillWidth: true
            }
            // Preset stepper — wired to PresetManager (Phase 2.5e.1).
            FlatButton {
                iconName: "caret-left"
                tooltipText: qsTr("Previous preset")
                enabled: WindowManager.presets
                onClicked: WindowManager.presets.stepPreset(-1)
            }
            Rectangle {
                Layout.preferredWidth: 280
                Layout.preferredHeight: Theme.toolStripHeight
                color: Theme.surface
                radius: Theme.radiusSmall
                Text {
                    anchors.fill: parent
                    anchors.leftMargin: Theme.spacingLoose
                    anchors.rightMargin: Theme.spacingLoose
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    text: {
                        if (!WindowManager.presets) return qsTr("(no preset)");
                        const name = WindowManager.presets.activePresetName;
                        if (!name) return qsTr("(no preset)");
                        return WindowManager.presets.modified
                               ? name + qsTr(" — modified")
                               : name;
                    }
                    color: WindowManager.presets
                           && WindowManager.presets.activePresetName.length > 0
                           ? (WindowManager.presets.modified
                              ? Theme.warn : Theme.textPrimary)
                           : Theme.textMuted
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontSizeSmall
                    font.italic: !WindowManager.presets
                                 || WindowManager.presets.activePresetName.length === 0
                                 || WindowManager.presets.modified
                    elide: Text.ElideMiddle
                }
            }
            FlatButton {
                iconName: "caret-right"
                tooltipText: qsTr("Next preset")
                enabled: WindowManager.presets
                onClicked: WindowManager.presets.stepPreset(1)
            }
            Item { width: Theme.spacingLoose }
            FlatButton {
                text: qsTr("Save")
                iconName: "floppy-disk"
                tooltipText: qsTr("Overwrite the active user preset")
                // Save overwrites the active user preset's slot tuple.
                // Built-ins are read-only; an attempt routes the user
                // to Save as instead. Save is also disabled when the
                // active preset isn't modified (no work to do).
                enabled: WindowManager.presets
                         && WindowManager.presets.activePresetName.length > 0
                         && !WindowManager.presets.activeIsBuiltIn()
                         && WindowManager.presets.modified
                onClicked: WindowManager.presets.saveCurrent()
            }
            FlatButton {
                text: qsTr("Save as…")
                iconName: "floppy-disk-back"
                enabled: WindowManager.presets
                onClicked: saveAsDialog.open()
            }
            // Delete the active user preset. Built-ins (and the
            // empty/no-preset state) keep this disabled. The
            // alert-red fill is reserved for an actually-actionable
            // destructive operation — when disabled, the button
            // falls back to the default greyed-out look so a quiet
            // "no user preset selected" state isn't shouting in red.
            FlatButton {
                text: qsTr("Delete")
                iconName: "trash"
                enabled: WindowManager.presets
                         && WindowManager.presets.activePresetName.length > 0
                         && !WindowManager.presets.activeIsBuiltIn()
                variant: enabled ? "danger" : "default"
                tooltipText: enabled
                             ? qsTr("Delete the active user preset")
                             : qsTr("Select a user preset to delete")
                onClicked: WindowManager.presets.deleteCurrent()
            }
        }

        // ---- Reel grid (middle, fills available height)
        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.leftMargin:   Theme.spacingLoose
            Layout.rightMargin:  Theme.spacingLoose
            Layout.topMargin:    Theme.spacingLoose
            Layout.bottomMargin: Theme.spacingLoose
            spacing: Theme.spacingLoose

            // Input-referred group
            //
            // Preset reel is a custom column (not the generic ReelColumn)
            // because its ListView uses section.property to render
            // grouped headers (Built-in / Blender 5.1 / ACES 2.0 / ACES
            // 1.3 / Custom). The model is QVariantList of
            // {name, section} maps from PresetManager.
            // Phase 2.5 polish: the Presets column is the entry point
            // into the chain. The body fills with the app's default
            // background but draws a 1-px border so the list reads as
            // a contained surface against the reels on its right.
            ColumnLayout {
                id: presetColumn
                property string filterText: ""
                Layout.minimumWidth: 130
                Layout.preferredWidth: 170
                Layout.fillHeight: true
                spacing: Theme.spacing

                Text {
                    text: qsTr("Preset")
                    color: Theme.textSecondary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontSizeSmall
                    font.bold: true
                }

                FlatTextField {
                    id: presetFilterField
                    Layout.fillWidth: true
                    placeholderText: qsTr("Filter…")
                    onTextChanged: presetColumn.filterText = text
                    // Subtle border so the filter slot reads as a
                    // defined input within the column, matching the
                    // bordered list body below. Hover + focus lift
                    // the fill; border turns accent on focus.
                    background: Rectangle {
                        color: presetFilterField.activeFocus
                               ? Theme.surfaceHover
                               : (presetFilterField.hovered
                                  ? Theme.surfaceAlt : Theme.surface)
                        radius: Theme.radiusSmall
                        border.width: Theme.borderWidth
                        border.color: presetFilterField.activeFocus
                                      ? Theme.accent : Theme.border
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    // One shade lighter than the app bg so the list
                    // surface visibly lifts off the panel — sells the
                    // Preset column as the entry point.
                    color: Theme.surface
                    radius: Theme.radiusSmall
                    border.width: Theme.borderWidth
                    border.color: Theme.border
                    clip: true

                    ListView {
                        id: presetList
                        anchors.fill: parent
                        // Inset by the border width so the sticky
                        // section headers (opaque Theme.surface bands)
                        // don't paint over the panel's border.
                        anchors.margins: Theme.borderWidth
                        clip: true
                        boundsBehavior: Flickable.StopAtBounds
                        model: WindowManager.presets
                               ? WindowManager.presets.availablePresetEntries : []
                        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                        section.property: "section"
                        section.criteria: ViewSection.FullString
                        section.delegate: Rectangle {
                            width: ListView.view.width
                            height: 18
                            // Match the list bg so the section header
                            // reads as a label on the same surface as
                            // the items, not a contrasting stripe.
                            color: Theme.surface
                            Text {
                                anchors.fill: parent
                                anchors.leftMargin: 6
                                anchors.rightMargin: 6
                                verticalAlignment: Text.AlignVCenter
                                text: section
                                color: Theme.textSecondary
                                font.family: Theme.fontFamily
                                font.pixelSize: Theme.fontSizeTiny
                                font.bold: true
                            }
                        }

                        delegate: Item {
                            id: presetRow
                            width: ListView.view.width
                            height: visible ? 22 : 0
                            property string entryName: modelData.name
                            // PresetManager::Preset::Kind:
                            //   0 Universal, 1 SdrSrgb, 2 HdrEdrSrgb,
                            //   3 HdrPq, 4 HdrEdrP3, 5 SdrP3.
                            property int entryKind: modelData.kind || 0
                            property bool isCurrent:
                                WindowManager.presets
                                && entryName === WindowManager.presets.activePresetName

                            readonly property bool isMacOS:
                                Qt.platform.os === "osx"
                                || Qt.platform.os === "macos"
                            readonly property bool isWindows:
                                Qt.platform.os === "windows"
                            readonly property int activeMode:
                                WindowManager ? WindowManager.hdrMode : 0
                            // Enable rule:
                            //   Universal  — always
                            //   SdrSrgb    — in SDR sRGB (mode 0), both OSes
                            //   SdrP3      — in SDR Display P3 (mode 1), both OSes
                            //   HdrEdrSrgb — cross-platform: in EDR
                            //                modes (2/3) on macOS OR
                            //                in Windows scRGB (mode 2)
                            //   HdrPq      — in HDR10 PQ mode (4),
                            //                Windows/Linux only
                            //   HdrEdrP3   — in EDR modes (2/3), macOS only
                            readonly property bool entryEnabled: {
                                if (entryKind === 0) return true;
                                if (entryKind === 1) return activeMode === 0;
                                if (entryKind === 5) return activeMode === 1;
                                if (entryKind === 2) {
                                    if (presetRow.isMacOS)
                                        return activeMode === 2 || activeMode === 3;
                                    if (presetRow.isWindows)
                                        return activeMode === 2;
                                    return false;
                                }
                                if (entryKind === 3)
                                    return activeMode === 4
                                           && !presetRow.isMacOS;
                                if (entryKind === 4)
                                    return (activeMode === 2 || activeMode === 3)
                                           && presetRow.isMacOS;
                                return true;
                            }
                            // Badge text — soft grey, right-aligned.
                            readonly property string badgeText: {
                                if (entryKind === 1 || entryKind === 5)
                                    return qsTr("SDR");
                                if (entryKind === 2 || entryKind === 3
                                    || entryKind === 4)
                                    return qsTr("HDR");
                                return "";
                            }
                            // Platform-logo decoration. HdrEdrSrgb is
                            // cross-platform so it shows BOTH logos.
                            readonly property bool showAppleIcon:
                                entryKind === 2 || entryKind === 4
                            readonly property bool showWindowsIcon:
                                entryKind === 2 || entryKind === 3

                            visible: presetColumn.filterText.length === 0
                                     || entryName.toLowerCase().indexOf(
                                          presetColumn.filterText.toLowerCase()) >= 0

                            Rectangle {
                                anchors.fill: parent
                                color: presetRow.isCurrent ? Theme.selection
                                     : (presetMa.containsMouse && presetRow.entryEnabled
                                        ? Theme.surfaceHover : "transparent")
                            }
                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 6
                                anchors.rightMargin: 6
                                spacing: Theme.spacing
                                opacity: presetRow.entryEnabled ? 1.0 : 0.45

                                Text {
                                    text: presetRow.isCurrent ? "★" : ""
                                    color: Theme.success
                                    font.family: Theme.fontFamily
                                    font.pixelSize: Theme.fontSizeSmall
                                    Layout.preferredWidth: 10
                                }
                                Text {
                                    Layout.fillWidth: true
                                    text: presetRow.entryName
                                    color: presetRow.isCurrent
                                           ? Theme.textBright : Theme.textPrimary
                                    font.family: Theme.fontFamily
                                    font.pixelSize: Theme.fontSizeSmall
                                    font.bold: presetRow.isCurrent
                                    elide: Text.ElideRight
                                }
                                // Platform-logo decoration. Cross-
                                // platform HdrEdrSrgb presets render
                                // BOTH icons side by side.
                                Icon {
                                    visible: presetRow.showAppleIcon
                                    name: "apple-logo"
                                    size: Theme.iconSizeSmall
                                    color: Theme.textSecondary
                                    Layout.preferredWidth: visible
                                                           ? Theme.iconSizeSmall : 0
                                }
                                Icon {
                                    visible: presetRow.showWindowsIcon
                                    name: "windows-logo"
                                    size: Theme.iconSizeSmall
                                    color: Theme.textSecondary
                                    Layout.preferredWidth: visible
                                                           ? Theme.iconSizeSmall : 0
                                }
                                // SDR / HDR badge in soft grey.
                                Text {
                                    visible: presetRow.badgeText.length > 0
                                    text: presetRow.badgeText
                                    color: Theme.textMuted
                                    font.family: Theme.fontFamily
                                    font.pixelSize: Theme.fontSizeTiny
                                    font.bold: true
                                    Layout.preferredWidth: visible ? 22 : 0
                                    horizontalAlignment: Text.AlignRight
                                }
                            }
                            MouseArea {
                                id: presetMa
                                anchors.fill: parent
                                hoverEnabled: true
                                enabled: presetRow.entryEnabled
                                cursorShape: presetRow.entryEnabled
                                              ? Qt.PointingHandCursor
                                              : Qt.ArrowCursor
                                onClicked: WindowManager.presets.applyPreset(entryName)
                            }
                        }
                    }
                }
            }
            ReelColumn {
                title: qsTr("Config")
                model: WindowManager.ocio
                       ? WindowManager.ocio.availableConfigs : []
                currentText: WindowManager.ocio
                             ? WindowManager.ocio.activeConfigName : ""
                onSelected: (entry) => WindowManager.ocio.setActiveConfig(entry)
            }
            ReelColumn {
                title: qsTr("Input")
                model: WindowManager.ocio ? WindowManager.ocio.colorspaces : []
                currentText: WindowManager.ocio
                             ? WindowManager.ocio.activeInput : ""
                onSelected: (entry) => WindowManager.ocio.activeInput = entry
            }
            ReelColumn {
                title: qsTr("Look")
                expandable: true
                expanded: lutTileSettings.lookExpanded
                onExpandedChanged: lutTileSettings.lookExpanded = expanded
                collapsedIconName: "magic-wand"
                model: WindowManager.ocio
                       ? [qsTr("(none)")].concat(WindowManager.ocio.looks)
                       : [qsTr("(none)")]
                currentText: WindowManager.ocio
                             && WindowManager.ocio.activeLook.length > 0
                             ? WindowManager.ocio.activeLook
                             : qsTr("(none)")
                onSelected: (entry) => WindowManager.ocio.activeLook =
                    (entry === qsTr("(none)") ? "" : entry)
            }

            // Scene LUT — picker tile, full column height
            LutTileColumn {
                title: qsTr("Scene LUT")
                iconName: "cube"
                path: WindowManager.ocio
                      ? WindowManager.ocio.activeSceneLutPath : ""
                expanded: lutTileSettings.sceneLutExpanded
                onExpandedChanged: lutTileSettings.sceneLutExpanded = expanded
                onPickRequested: root.pickLut("scene")
                onClearRequested: WindowManager.ocio.activeSceneLutPath = ""
            }

            // Group divider
            Text {
                text: "→"
                color: Theme.textMuted
                font.family: Theme.fontFamily
                font.pixelSize: 22
                Layout.alignment: Qt.AlignVCenter
            }

            // Display-referred group
            ReelColumn {
                title: qsTr("Output")
                model: WindowManager.ocio ? WindowManager.ocio.displays : []
                currentText: WindowManager.ocio
                             ? WindowManager.ocio.activeDisplay : ""
                onSelected: (entry) => WindowManager.ocio.activeDisplay = entry
            }
            ReelColumn {
                title: qsTr("View")
                // Recompute when display changes — bind to activeDisplay
                // so the view list refreshes after Output selection.
                model: WindowManager.ocio
                    ? WindowManager.ocio.viewsForDisplay(
                        WindowManager.ocio.activeDisplay) : []
                currentText: WindowManager.ocio
                             ? WindowManager.ocio.activeView : ""
                onSelected: (entry) => WindowManager.ocio.activeView = entry
            }

            // Display LUT — picker tile, full column height
            LutTileColumn {
                title: qsTr("Display LUT")
                iconName: "monitor"
                path: WindowManager.ocio
                      ? WindowManager.ocio.activeDisplayLutPath : ""
                expanded: lutTileSettings.displayLutExpanded
                onExpandedChanged: lutTileSettings.displayLutExpanded = expanded
                onPickRequested: root.pickLut("display")
                onClearRequested: WindowManager.ocio.activeDisplayLutPath = ""
            }
        }

        // ---- Bottom row: Engage | Export LUT
        // Engage is the explicit "apply this chain" action. Default
        // off, so the app boots in passthrough. Once engaged, slot
        // changes update live (the chain rebuild is cheap — typically
        // < a few ms). Disengage to return to raw without losing
        // slot selections. See Guide 05 D9 (and the engage-vs-bypass
        // pivot in Phase 2.5b).
        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: Theme.spacingLoose
            Layout.rightMargin: 0
            spacing: Theme.spacingLoose

            RowLayout {
                spacing: Theme.spacingLoose
                FlatSwitch {
                    id: engageSwitch
                    // Attention styling on both states — blue "engage
                    // me" while off, green "engaged" when on. Was
                    // `!checked` previously which dropped attention
                    // mode the moment the user toggled on, missing
                    // the green confirmation.
                    attention: true
                    checked: WindowManager.ocio
                             ? WindowManager.ocio.engaged : false
                    onToggled: WindowManager.ocio.engaged = checked
                }
                Item { Layout.preferredWidth: Theme.padding }
                Text {
                    text: engageSwitch.checked
                          ? qsTr("OCIO Engaged — applying chain live")
                          : qsTr("Engage OCIO")
                    color: engageSwitch.checked ? Theme.success : Theme.textPrimary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontSizeSmall
                    font.bold: engageSwitch.checked
                }
            }

            Item { Layout.fillWidth: true }

            // Phase F.2.8 follow-up — uniform post-OCIO brightness
            // multiplier. Lives just left of the Display picker
            // (clusters all display-side controls together). Linear
            // scale 0.5×–3.0×, default 1.0×. Persisted in QSettings
            // under "display/brightness". WILL clip HDR highlights
            // above the display's headroom — user-responsible.
            Text {
                text: qsTr("Brightness:")
                color: Theme.textSecondary
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontSizeSmall
            }
            FlatSlider {
                id: brightnessSlider
                from: 0.5
                to: 3.0
                stepSize: 0.05
                value: WindowManager ? WindowManager.brightness : 1.0
                implicitWidth: 120
                onMoved: WindowManager.brightness = value
                // Double-click anywhere on the slider resets to 1.0×
                // (no-multiplier identity). Uses propagateComposedEvents
                // so the slider still receives the press half of the
                // double-click for normal drag interactions.
                MouseArea {
                    anchors.fill: parent
                    acceptedButtons: Qt.LeftButton
                    propagateComposedEvents: true
                    onPressed: (mouse) => { mouse.accepted = false }
                    onDoubleClicked: WindowManager.brightness = 1.0
                }
            }
            Text {
                text: (WindowManager
                        ? WindowManager.brightness
                        : 1.0).toFixed(2) + "×"
                color: Theme.textSecondary
                font.family: Theme.monoFamily
                font.pixelSize: Theme.fontSizeSmall
                Layout.preferredWidth: 40
                horizontalAlignment: Text.AlignRight
            }
            FlatButton {
                iconName: "arrow-counter-clockwise"
                tooltipText: qsTr("Reset brightness to 1.0×")
                enabled: WindowManager
                         && Math.abs(WindowManager.brightness - 1.0) > 1e-3
                onClicked: WindowManager.brightness = 1.0
            }

            // Phase 2.6.1: HDR mode picker. Lives in the panel's
            // bottom row temporarily; Guide 06 D3 puts the final
            // chip in the menu bar. The dropdown items map to
            // WindowManager.HdrMode enum values.
            Text {
                text: qsTr("Display:")
                color: Theme.textSecondary
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontSizeSmall
            }
            FlatComboBox {
                id: hdrModeCombo
                implicitHeight: 26
                // Pin a width that fits the longest entry so the
                // shorter "SDR" selection doesn't shrink the box and
                // hide the menu's other items behind a narrow header.
                implicitWidth: 200

                // Per Guide 06 D2 + Phase F.2.9: macOS gets the EDR
                // variants via CAMetalLayer extendedDynamicRange;
                // Windows D3D11 exposes scRGB-linear AND HDR10 PQ —
                // both are first-class HDR options. PQ uses an
                // R10G10B10A2_UNORM swapchain tagged G2084_NONE_P2020
                // (BT.2020 + ST.2084) with HDR10 mastering metadata;
                // scRGB uses R16G16B16A16_FLOAT + G10_NONE_P709 and
                // leans on the OS compositor to tone-map. Linux falls
                // through to PQ on Vulkan.
                //
                // Show ALL modes always; flag platform-unavailable
                // ones as `supported: false` so they grey out
                // instead of vanishing.
                readonly property bool isMacOS:
                    Qt.platform.os === "osx" || Qt.platform.os === "macos"
                readonly property bool isWindows:
                    Qt.platform.os === "windows"
                readonly property var entries: [
                    { name: qsTr("SDR — sRGB"),           value: 0, supported: true },
                    { name: qsTr("SDR — Display P3"),     value: 1, supported: true },
                    { name: qsTr("EDR — Linear sRGB"),    value: 2, supported: isMacOS },
                    { name: qsTr("EDR — Linear P3"),      value: 3, supported: isMacOS },
                    { name: qsTr("HDR10 PQ"),             value: 4, supported: !isMacOS },
                    { name: qsTr("Extended Linear sRGB"), value: 2, supported: !isMacOS },
                ]
                textRole: "name"
                valueRole: "value"
                model: entries
                currentIndex: {
                    // Tie-break on `supported` so value-2 picks the
                    // correctly-labeled row for the active platform.
                    for (let i = 0; i < entries.length; ++i) {
                        if (entries[i].value === WindowManager.hdrMode
                            && entries[i].supported) return i;
                    }
                    for (let i = 0; i < entries.length; ++i) {
                        if (entries[i].value === WindowManager.hdrMode) return i;
                    }
                    return 0;
                }
                onActivated: (index) => {
                    if (!entries[index].supported) return;
                    WindowManager.hdrMode = entries[index].value;
                }

                // Custom delegate so unsupported rows render greyed
                // out and reject clicks — defaults from
                // FlatComboBox use string `modelData` and don't know
                // about per-item enabled.
                delegate: ItemDelegate {
                    width: hdrModeCombo.width
                    height: 24
                    enabled: modelData.supported
                    contentItem: Text {
                        text: modelData.name
                        color: modelData.supported
                               ? Theme.textPrimary : Theme.textMuted
                        font: hdrModeCombo.font
                        verticalAlignment: Text.AlignVCenter
                        leftPadding: Theme.spacing
                    }
                    background: Rectangle {
                        color: (parent.hovered || parent.highlighted)
                               && modelData.supported
                               ? Theme.surfaceHover : "transparent"
                    }
                }
            }

            FlatButton {
                text: qsTr("Export LUT…")
                iconName: "export"
                // Bakes the configured OCIO chain to a .cube — works
                // whether or not the chain is engaged (engage is for
                // live viewport apply; export is independent).
                enabled: WindowManager.ocio
                         && WindowManager.ocio.activeInput.length > 0
                         && WindowManager.ocio.activeDisplay.length > 0
                         && WindowManager.ocio.activeView.length > 0
                tooltipText: enabled
                             ? qsTr("Bake the active OCIO chain to a 65³ .cube LUT")
                             : qsTr("Configure input / display / view first")
                onClicked: exportLutDialog.open()
            }
        }
    }

    // ---- Reusable: a vertical scroll-list reel.
    // Always-visible options list with current-selection star and
    // optional filter field. Click a row to select.
    //
    // Phase 2.5 polish: optional collapsible mode (mirrors LutTileColumn).
    // Set expandable=true and the parent owns `expanded`. Collapsed
    // state renders a slim strip with rotated title — saves horizontal
    // space for reels that aren't typically active (e.g. Look).
    component ReelColumn: ColumnLayout {
        id: reel
        property string title: ""
        property var    model: []
        property string currentText: ""
        property bool   showFilter: true
        property string filterText: ""
        property bool   expandable: false
        property bool   expanded: true
        property string collapsedIconName: "list-bullets"
        signal selected(string entry)

        Layout.minimumWidth: (expandable && !expanded) ? 32 : 130
        Layout.preferredWidth: (expandable && !expanded) ? 32 : 160
        Layout.fillHeight: true
        spacing: Theme.spacing

        // Title slot — always reserves its height (matches LutTileColumn)
        // so collapsed and expanded columns line up at the same body Y.
        // The inner Text + chevron are what toggle on collapse.
        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: titleProbe.implicitHeight

            Text {
                id: titleProbe
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                visible: !reel.expandable || reel.expanded
                text: reel.title
                color: Theme.textSecondary
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontSizeSmall
                font.bold: true
            }
            Icon {
                visible: reel.expandable && reel.expanded
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                name: "caret-double-left"
                size: Theme.iconSizeSmall
                color: reelCollapseMa.containsMouse
                       ? Theme.textPrimary : Theme.textSecondary
            }
            MouseArea {
                id: reelCollapseMa
                visible: reel.expandable && reel.expanded
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                width: 22
                height: 22
                cursorShape: Qt.PointingHandCursor
                hoverEnabled: true
                onClicked: reel.expanded = false
                FlatToolTip {
                    visible: reelCollapseMa.containsMouse
                    text: qsTr("Collapse")
                }
            }
        }

        // Optional filter (shown when list could be long)
        FlatTextField {
            id: reelFilterField
            Layout.fillWidth: true
            visible: reel.showFilter && (!reel.expandable || reel.expanded)
            placeholderText: qsTr("Filter…")
            onTextChanged: reel.filterText = text
            // Subtle border so the filter slot reads as a defined
            // input within the column, matching the bordered list
            // body below it. Border turns accent on focus.
            background: Rectangle {
                color: reelFilterField.activeFocus ? Theme.surface
                       : (reelFilterField.hovered ? Theme.bgAlt : Theme.bg)
                radius: Theme.radiusSmall
                border.width: Theme.borderWidth
                border.color: reelFilterField.activeFocus
                              ? Theme.accent : Theme.border
            }
        }

        // Expanded list body.
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: !reel.expandable || reel.expanded
            color: reel.enabled ? Theme.bg : Theme.bgAlt
            radius: Theme.radiusSmall
            border.width: Theme.borderWidth
            border.color: Theme.border
            clip: true
            opacity: reel.enabled ? 1.0 : 0.55

            ListView {
                id: list
                anchors.fill: parent
                model: reel.model
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                // Filter is a simple substring match on the original
                // model. If filtering causes complexity later (grouped
                // entries / categories), promote model to a real
                // QSortFilterProxyModel.
                delegate: Item {
                    width: ListView.view.width
                    height: visible ? 22 : 0
                    visible: reel.filterText.length === 0
                             || modelData.toLowerCase().indexOf(
                                  reel.filterText.toLowerCase()) >= 0

                    Rectangle {
                        anchors.fill: parent
                        color: modelData === reel.currentText ? Theme.selection
                             : (mouseArea.containsMouse ? Theme.surfaceHover : "transparent")
                    }
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 6
                        anchors.rightMargin: 6
                        spacing: Theme.spacing
                        Text {
                            text: modelData === reel.currentText ? "★" : ""
                            color: Theme.success
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.fontSizeSmall
                            Layout.preferredWidth: 10
                        }
                        Text {
                            Layout.fillWidth: true
                            text: modelData
                            color: modelData === reel.currentText
                                   ? Theme.textBright : Theme.textPrimary
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.fontSizeSmall
                            font.bold: modelData === reel.currentText
                            elide: Text.ElideRight
                        }
                    }
                    MouseArea {
                        id: mouseArea
                        anchors.fill: parent
                        hoverEnabled: true
                        enabled: reel.enabled
                        onClicked: reel.selected(modelData)
                    }
                }
            }
        }

        // Collapsed strip — same shape as LutTileColumn's collapsed body.
        Rectangle {
            id: reelCollapsedStrip
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: reel.expandable && !reel.expanded
            color: reelExpandMa.containsMouse ? Theme.surfaceHover : Theme.bg
            border.width: Theme.borderWidth
            border.color: Theme.border
            radius: Theme.radiusSmall

            MouseArea {
                id: reelExpandMa
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: reel.expanded = true
                FlatToolTip {
                    visible: reelExpandMa.containsMouse
                    text: qsTr("%1 — click to expand").arg(reel.title)
                }
            }

            ColumnLayout {
                anchors.fill: parent
                anchors.topMargin: Theme.spacingLoose
                anchors.bottomMargin: Theme.spacingLoose
                spacing: Theme.spacingLoose

                Item {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Text {
                        anchors.centerIn: parent
                        rotation: -90
                        text: reel.title
                        // Always-soft grey for the rotated label — the
                        // collapsed strip's job is "here's a column you
                        // can expand," not a loaded-state signal.
                        color: Theme.textSecondary
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontSizeSmall
                        font.bold: true
                    }
                }

                Icon {
                    Layout.alignment: Qt.AlignHCenter
                    name: reel.collapsedIconName
                    size: 20
                    // Grey icon (no green/success cue) — keeps the
                    // collapsed strip neutral.
                    color: reelExpandMa.containsMouse
                           ? Theme.accent : Theme.textMuted
                }

                Icon {
                    Layout.alignment: Qt.AlignHCenter
                    name: "caret-double-right"
                    size: Theme.iconSizeSmall
                    color: Theme.textMuted
                }
            }
        }
    }

    // ---- Reusable: a LUT picker tile shaped like a reel column.
    //
    // Two states:
    //   - Expanded: full ~160-px column with a big centered CTA
    //     tile (cube/monitor identity icon, "+ Add LUT" / "Replace
    //     LUT" title, basename subtitle, corner ✕ to clear).
    //   - Collapsed (default): slim ~32-px strip with a rotated
    //     vertical title, the identity icon, and a caret-right
    //     hint. Border / icon color still signal whether a LUT is
    //     loaded (Theme.success when present).
    //
    // Clicking the section header chevron toggles the state. In
    // collapsed state, clicking anywhere on the strip expands.
    component LutTileColumn: ColumnLayout {
        id: tile
        property string title: ""
        property string iconName: "cube"
        property string path: ""
        property bool   expanded: false
        signal pickRequested()
        signal clearRequested()

        Layout.minimumWidth:   tile.expanded ? 130 : 32
        Layout.preferredWidth: tile.expanded ? 160 : 32
        Layout.fillHeight: true
        spacing: Theme.spacing

        // Title slot — same line height as ReelColumn's title text,
        // so list bodies / tile bodies / collapsed strips all start
        // at the same Y. Section header (title + collapse chevron)
        // is rendered inside when expanded; collapsed leaves this
        // bar empty.
        Item {
            id: titleSlot
            Layout.fillWidth: true
            Layout.preferredHeight: titleProbe.implicitHeight

            Text {
                id: titleProbe
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                visible: tile.expanded
                text: tile.title
                color: Theme.textSecondary
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontSizeSmall
                font.bold: true
            }
            Icon {
                visible: tile.expanded
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                // Matches the Look reel's collapse direction — the
                // column shrinks horizontally into a left-side strip,
                // so left-pointing carets read as "collapse" and
                // right-pointing as "expand" on the collapsed body.
                name: "caret-double-left"
                size: Theme.iconSizeSmall
                color: collapseMa.containsMouse
                       ? Theme.textPrimary : Theme.textSecondary
            }
            MouseArea {
                id: collapseMa
                visible: tile.expanded
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                width: 22
                height: 22
                cursorShape: Qt.PointingHandCursor
                hoverEnabled: true
                onClicked: tile.expanded = false
                FlatToolTip {
                    visible: collapseMa.containsMouse
                    text: qsTr("Collapse")
                }
            }
        }

        // ---- Expanded body — big CTA tile.
        // No filter slot here — the LUT body (big tile / collapsed
        // strip) extends up into the filter zone for extra room.
        // The title slot above remains the only reserved header
        // area, and stays empty in collapsed mode.
        Rectangle {
            id: tileBg
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: tile.expanded
            // Subtle border defines the tile against the panel.
            // Hover lifts the fill; loaded-state cue lives on the
            // identity icon (Theme.success) inside.
            color: tileMa.containsMouse ? Theme.surfaceHover : Theme.bg
            border.width: Theme.borderWidth
            border.color: Theme.border
            radius: Theme.radiusSmall

            // Body click target. Declared first so the corner ✕
            // (declared later) wins when the click lands on it.
            MouseArea {
                id: tileMa
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: tile.pickRequested()
            }

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: Theme.spacingLoose
                spacing: Theme.spacing

                Item { Layout.fillHeight: true }

                Icon {
                    Layout.alignment: Qt.AlignHCenter
                    name: tile.iconName
                    size: 36
                    color: tile.path
                           ? Theme.success
                           : (tileMa.containsMouse
                              ? Theme.accent : Theme.textMuted)
                }

                Item { Layout.preferredHeight: Theme.spacing }

                Text {
                    Layout.alignment: Qt.AlignHCenter
                    Layout.fillWidth: true
                    text: tile.path
                          ? qsTr("Replace LUT")
                          : qsTr("+ Add LUT")
                    color: tileMa.containsMouse
                           ? Theme.textBright : Theme.textPrimary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontSizeBase
                    font.bold: true
                    horizontalAlignment: Text.AlignHCenter
                }

                Text {
                    Layout.alignment: Qt.AlignHCenter
                    Layout.fillWidth: true
                    Layout.leftMargin: Theme.padding
                    Layout.rightMargin: Theme.padding
                    visible: tile.path.length > 0
                    text: root.basenameOf(tile.path)
                    color: Theme.textSecondary
                    font.family: Theme.monoFamily
                    font.pixelSize: Theme.fontSizeSmall
                    horizontalAlignment: Text.AlignHCenter
                    elide: Text.ElideMiddle
                }
                Text {
                    Layout.alignment: Qt.AlignHCenter
                    Layout.fillWidth: true
                    visible: tile.path.length === 0
                    text: qsTr("click to choose")
                    color: Theme.textMuted
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontSizeSmall
                    font.italic: true
                    horizontalAlignment: Text.AlignHCenter
                }

                Item { Layout.fillHeight: true }
            }

            // Top-right clear button when a LUT is loaded. Declared
            // AFTER tileMa so this MouseArea catches clicks within
            // its bounds before the body MA does.
            Rectangle {
                anchors.top: parent.top
                anchors.right: parent.right
                anchors.margins: Theme.spacing
                width: 22
                height: 22
                color: clearMa.containsMouse ? Theme.error : "transparent"
                radius: Theme.radiusSmall
                visible: tile.path.length > 0
                Icon {
                    anchors.centerIn: parent
                    name: "x"
                    size: Theme.iconSizeSmall
                    color: clearMa.containsMouse
                           ? Theme.textBright : Theme.textMuted
                }
                MouseArea {
                    id: clearMa
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: tile.clearRequested()
                    FlatToolTip {
                        visible: clearMa.containsMouse
                        text: qsTr("Clear LUT")
                    }
                }
            }
        }

        // ---- Collapsed body — slim vertical strip.
        Rectangle {
            id: collapsedStrip
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: !tile.expanded
            // Subtle border defines the strip against the panel.
            // Hover lifts the fill; loaded-state cue lives on the
            // identity icon (Theme.success).
            color: collapsedMa.containsMouse ? Theme.surfaceHover : Theme.bg
            border.width: Theme.borderWidth
            border.color: Theme.border
            radius: Theme.radiusSmall

            MouseArea {
                id: collapsedMa
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: tile.expanded = true
                FlatToolTip {
                    visible: collapsedMa.containsMouse
                    text: tile.path
                          ? qsTr("%1 — %2").arg(tile.title)
                                            .arg(root.basenameOf(tile.path))
                          : qsTr("%1 — click to expand").arg(tile.title)
                }
            }

            ColumnLayout {
                anchors.fill: parent
                anchors.topMargin: Theme.spacingLoose
                anchors.bottomMargin: Theme.spacingLoose
                spacing: Theme.spacingLoose

                // Rotated vertical title — fills the upper portion
                // of the strip. Wrapped in an Item so the rotated
                // bounding box doesn't fight the layout.
                Item {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Text {
                        anchors.centerIn: parent
                        rotation: -90
                        text: tile.title
                        color: tile.path ? Theme.textPrimary
                                          : Theme.textSecondary
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontSizeSmall
                        font.bold: true
                    }
                }

                Icon {
                    Layout.alignment: Qt.AlignHCenter
                    name: tile.iconName
                    size: 20
                    color: tile.path
                           ? Theme.success
                           : (collapsedMa.containsMouse
                              ? Theme.accent : Theme.textMuted)
                }

                Icon {
                    Layout.alignment: Qt.AlignHCenter
                    name: "caret-double-right"
                    size: Theme.iconSizeSmall
                    color: Theme.textMuted
                }
            }
        }
    }
}
