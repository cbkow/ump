// RightRail — Phase 3.H.6 polish.
//
// Inspector + Image Sequence sections only. Safety Guides and
// Background moved to the LEFT rail next to Playlists; tools
// (Annotation + Color picker) live in the bottom Notes panel.
// What's left here is "what's loaded" — the Inspector showing
// the active item's metadata, and the Image Sequence panel
// surfacing layer / range / fps for EXR/PNG/TIFF/JPEG seqs.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Qcv

Pane {
    id: root
    padding: 0

    implicitWidth: 320

    property bool collapsed: false
    signal toggleCollapsed()

    // Expand + scroll the named section into view. Currently
    // wired for "shortcuts" (the keyboard-shortcuts reference);
    // future sections plug in here too.
    function revealSection(name) {
        if (typeof keyboardShortcutsSection !== "undefined") {
            keyboardShortcutsSection.expanded = (name === "shortcuts");
        }
        Qt.callLater(function() {
            var target = null;
            if (name === "shortcuts"
                && typeof keyboardShortcutsSection !== "undefined")
                target = keyboardShortcutsSection;
            if (!target) return;
            var flick = scroll.contentItem;
            if (!flick) return;
            var pos  = target.mapToItem(flick.contentItem, 0, 0);
            var maxY = Math.max(0, flick.contentHeight - flick.height);
            flick.contentY = Math.max(0, Math.min(maxY, pos.y - 8));
        });
    }

    background: Rectangle {
        // Darker well (aesthetics pass 1) — the InspectorCard planes
        // inside read as raised against this; was the flat #1c1c1c
        // shared with the left rail.
        color: Theme.bg
        // Left-edge seam — COLLAPSED state only; when expanded the
        // HResizeHandle beside the rail is the (single) 1-px line.
        // See LeftRail's matching comment.
        Rectangle {
            visible: root.collapsed
            anchors.left:   parent.left
            anchors.top:    parent.top
            anchors.bottom: parent.bottom
            // One device pixel — see LeftRail's matching seam.
            width:  1 / Screen.devicePixelRatio
            color:  Theme.divider
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // Header strip with chevron. Mirrors LeftRail's chevron
        // behavior but the chevron lives in the right-edge
        // gutter slot since the rail collapses toward the right.
        Rectangle {
            Layout.fillWidth: true
            // Match the ViewportOverlay row (36 px) so the
            // rail's collapse button lines up across the seam.
            Layout.preferredHeight: 36
            color: Theme.toolbar
            RowLayout {
                anchors.fill: parent
                spacing: 0

                Item {
                    visible: !root.collapsed
                    Layout.preferredWidth: Theme.padding
                }
                Icon {
                    visible: !root.collapsed
                    Layout.preferredWidth: Theme.iconSizeToolbar
                    Layout.preferredHeight: Theme.iconSizeToolbar
                    Layout.rightMargin: Theme.spacing
                    name: "info"
                    size: Theme.iconSizeToolbar
                    color: Theme.textSecondary
                }
                Text {
                    Layout.fillWidth: true
                    visible: !root.collapsed
                    text: qsTr("Inspector")
                    color: Theme.textPrimary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontSizeBase
                    font.bold: true
                    elide: Text.ElideRight
                }
                // When collapsed, fillWidth is gone with the label;
                // a stretching spacer keeps the chevron pinned
                // to the right gutter.
                Item { visible: root.collapsed; Layout.fillWidth: true }
                Item {
                    // Shown only while open — the re-open affordance is
                    // in the ViewportOverlay top bar when closed. Hiding
                    // it on collapse stops the arrow flashing through the
                    // gutter width during the close animation.
                    visible: !root.collapsed
                    Layout.preferredWidth: Theme.gutterWidth
                    Layout.fillHeight: true
                    // Subtle raised-gray idle fill so the toggle reads
                    // as a button at rest; hover steps up for clear
                    // click feedback. See Theme.affordanceIdle/Hover.
                    Rectangle {
                        anchors.fill: parent
                        color: railToggleMa.containsMouse
                               ? Theme.affordanceHover
                               : Theme.affordanceIdle
                    }
                    Icon {
                        anchors.centerIn: parent
                        name: root.collapsed ? "arrow-left" : "arrow-right"
                        size: Theme.iconSizeSmall
                        color: railToggleMa.containsMouse
                               ? Theme.textPrimary : Theme.textSecondary
                    }
                    MouseArea {
                        id: railToggleMa
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        hoverEnabled: true
                        onClicked: root.toggleCollapsed()
                        // Tooltip now renders over the native player
                        // surface (FlatToolTip is popupType:
                        // Popup.Window), so the rail-edge toggle can
                        // carry one without stacking behind the
                        // viewport.
                        FlatToolTip {
                            visible: railToggleMa.containsMouse
                            text: root.collapsed ? qsTr("Expand panel")
                                                 : qsTr("Collapse panel")
                        }
                    }
                }
            }
            // No bottom divider — the toolbar strip's tone against
            // the rail well below is the separation.
        }

        // ---- Inspector + Image Sequence -------------------------
        // ScrollView so tall metadata blocks (lots of timecodes,
        // multi-layer EXR pickers) stay reachable when the rail
        // can't fit everything at once.
        ScrollView {
            id: scroll
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: !root.collapsed
            clip: true
            // availableWidth excludes the vertical scrollbar's
            // reserved space — bind contentWidth to it so the
            // inner column doesn't extend underneath the bar and
            // get its right edge clipped.
            contentWidth: availableWidth
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
            ScrollBar.vertical.policy:   ScrollBar.AsNeeded

            ColumnLayout {
                width: scroll.availableWidth
                spacing: 0

                InspectorPanel {
                    id: inspectorPanel
                    Layout.fillWidth: true
                }

                ImageSequenceInspector {
                    id: imageSeqPanel
                    Layout.fillWidth: true
                    // Inset to match the InspectorCards above (the
                    // panel is itself a card plane now).
                    Layout.leftMargin: Theme.paddingTight
                    Layout.rightMargin: Theme.paddingTight
                    Layout.bottomMargin: visible ? Theme.paddingTight : 0
                    item: inspectorPanel.displayedItem
                    side: inspectorPanel.dualActive
                          ? inspectorPanel.inspectedSide
                          : ""
                }

                // ---- Keyboard Shortcuts (read-only reference) ----
                // Maintenance: bindings here MUST be kept in sync
                // with their actual implementations in
                // window_manager.cpp (eventFilter / transport keys),
                // Main.qml (file/view menu accelerators),
                // TimelinePanel.qml (edit-mode shortcuts), and
                // LeftRail.qml (Delete/F2 for bins).
                //
                // Card plane with its own collapse header (same
                // pattern as ImageSequenceInspector) instead of the
                // shared CollapsibleSection — that component still
                // carries the old flat style and gets restyled in
                // the left-rail pass.
                Rectangle {
                    id: keyboardShortcutsSection
                    Layout.fillWidth: true
                    Layout.leftMargin: Theme.paddingTight
                    Layout.rightMargin: Theme.paddingTight
                    Layout.bottomMargin: Theme.paddingTight
                    color: Theme.card
                    radius: Theme.radiusBase
                    clip: true
                    property bool expanded: false
                    implicitHeight: expanded
                        ? shortcutsBody.y + shortcutsBody.implicitHeight
                          + Theme.padding
                        : shortcutsHeader.height

                    Rectangle {
                        id: shortcutsHeader
                        anchors.top: parent.top
                        anchors.left: parent.left
                        anchors.right: parent.right
                        height: Theme.headerHeight
                        radius: Theme.radiusBase
                        color: shortcutsHeaderMa.containsMouse
                               ? Theme.surfaceHover : "transparent"

                        MouseArea {
                            id: shortcutsHeaderMa
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: keyboardShortcutsSection.expanded =
                                       !keyboardShortcutsSection.expanded
                        }
                        RowLayout {
                            anchors.fill: parent
                            spacing: 0
                            Item {
                                Layout.preferredWidth: Theme.gutterWidth
                                Layout.fillHeight: true
                                Icon {
                                    anchors.centerIn: parent
                                    name: keyboardShortcutsSection.expanded
                                          ? "caret-down" : "caret-right"
                                    size: Theme.iconSizeSmall
                                    color: Theme.textSecondary
                                }
                            }
                            // Card-voice title (tiny caps, muted) —
                            // matches the InspectorCard titles above.
                            Text {
                                Layout.fillWidth: true
                                text: qsTr("Keyboard Shortcuts")
                                color: Theme.textMuted
                                font.family: Theme.fontFamily
                                font.pixelSize: Theme.fontSizeTiny
                                font.bold: true
                                font.capitalization: Font.AllUppercase
                                font.letterSpacing: 0.8
                            }
                        }
                    }

                    // Each item is either:
                    //   { label, keys }                         — same on all platforms (single keys, F-keys, etc.)
                    //   { label, keysWin, keysMac }             — modifier-bearing combos that read differently per OS
                    // Display delegate picks keysWin on Windows, keysMac elsewhere; falls back to `keys` when present.
                    // Why per-entry instead of a render-time symbol substitution: the View block originally used ⌃ for
                    // Qt::ControlModifier shortcuts, which is wrong (Qt remaps Ctrl→Cmd on Mac so ⌘ is the correct Mac
                    // glyph). A blanket symbol swap would carry that error across; per-entry forces us to write each
                    // binding correctly for both OSes.
                    readonly property var categories: [
                        {
                            heading: "File",
                            items: [
                                { label: "Open Media…",         keysWin: "Ctrl+O",       keysMac: "⌘O"   },
                                { label: "Open Project…",       keysWin: "Ctrl+Shift+O", keysMac: "⇧⌘O"  },
                                { label: "Save Project",        keysWin: "Ctrl+S",       keysMac: "⌘S"   },
                                { label: "Save Project As…",    keysWin: "Ctrl+Shift+S", keysMac: "⇧⌘S"  },
                            ]
                        },
                        {
                            heading: "View",
                            items: [
                                { label: "Minimal Mode",        keysWin: "Ctrl+0",       keysMac: "⌘0"     },
                                { label: "Default View",        keysWin: "Ctrl+R",       keysMac: "⌘R"     },
                                { label: "Show All Panels",     keysWin: "Ctrl+9",       keysMac: "⌘9"     },
                                { label: "Left Rail",           keysWin: "Ctrl+1",       keysMac: "⌘1"     },
                                { label: "Right Rail",          keysWin: "Ctrl+2",       keysMac: "⌘2"     },
                                { label: "Color Panel",         keysWin: "Ctrl+3",       keysMac: "⌘3"     },
                                { label: "Notes Panel",         keysWin: "Ctrl+4",       keysMac: "⌘4"     },
                                { label: "Status Bar",          keysWin: "Ctrl+5",       keysMac: "⌘5"     },
                                { label: "Keyboard Shortcuts",  keysWin: "Ctrl+/",       keysMac: "⌘/"     },
                                { label: "Settings",            keysWin: "Ctrl+,",       keysMac: "⌘,"     },
                                { label: "Fullscreen",          keys: "F"   },
                                { label: "Exit Fullscreen",     keys: "Esc" },
                            ]
                        },
                        {
                            heading: "Transport",
                            items: [
                                { label: "Play / Pause",            keys: "Space, W, S, K"  },
                                { label: "Previous Frame",          keys: "Q"               },
                                { label: "Next Frame",              keys: "E"               },
                                { label: "Rewind (hold)",           keys: "A, J"            },
                                { label: "Fast Forward (hold)",     keys: "D, L"            },
                                { label: "Seek to Start",           keys: "Home"            },
                                { label: "Seek to End",             keys: "End"             },
                                { label: "Toggle Loop",             keysWin: "V, Ctrl+L", keysMac: "V, ⌘L" },
                                { label: "Set In Point",            keys: "I"               },
                                { label: "Set Out Point",           keys: "O"               },
                                { label: "Clear In/Out Points",     keysWin: "Shift+I", keysMac: "⇧I" },
                                { label: "Volume Up",               keys: "↑"               },
                                { label: "Volume Down",             keys: "↓"               },
                                { label: "Mute / Unmute",           keys: "M"               },
                                { label: "Cycle Background",        keys: "B"               },
                            ]
                        },
                        {
                            heading: "Screenshots",
                            items: [
                                { label: "Screenshot to Desktop",   keys: "T"   },
                                { label: "Screenshot to Clipboard", keysWin: "Ctrl+T", keysMac: "⌘T" },
                            ]
                        },
                        {
                            heading: "Annotations (in annotation mode)",
                            items: [
                                { label: "Undo Stroke",             keysWin: "Ctrl+Z",       keysMac: "⌘Z"   },
                                { label: "Redo Stroke",             keysWin: "Ctrl+Shift+Z", keysMac: "⇧⌘Z"  },
                                { label: "Cancel In-Flight Stroke", keys: "Esc" },
                            ]
                        },
                        {
                            heading: "Bins (when item selected)",
                            items: [
                                { label: "Delete Item",             keys: "Delete, Backspace"  },
                                { label: "Rename Item",             keys: "F2"                 },
                            ]
                        },
                        {
                            heading: "Timeline (edit mode)",
                            items: [
                                { label: "Undo Edit",               keysWin: "Ctrl+Z",       keysMac: "⌘Z"   },
                                { label: "Redo Edit",               keysWin: "Ctrl+Shift+Z", keysMac: "⇧⌘Z"  },
                                { label: "Split Clip at Playhead",  keysWin: "Ctrl+K",       keysMac: "⌘K"   },
                                { label: "Delete Selected Clip",    keys: "Delete, Backspace" },
                                { label: "Exit Edit Mode",          keys: "Esc" },
                            ]
                        },
                        {
                            heading: "Timeline (zoom + pan)",
                            items: [
                                { label: "Zoom In",                  keysWin: "Ctrl+=", keysMac: "⌘=" },
                                { label: "Zoom Out",                 keysWin: "Ctrl+−", keysMac: "⌘−" },
                                { label: "Zoom (cursor anchored)",   keys: "Scroll" },
                                { label: "Pan horizontally",         keysWin: "Alt+Scroll", keysMac: "⌥Scroll" },
                                { label: "Drag viewport (overview)", keys: "Drag middle" },
                                { label: "Resize viewport (zoom)",   keys: "Drag edge" },
                            ]
                        },
                    ]

                    ColumnLayout {
                        id: shortcutsBody
                        anchors.top: shortcutsHeader.bottom
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.leftMargin: Theme.paddingTight
                        anchors.rightMargin: Theme.paddingTight
                        visible: keyboardShortcutsSection.expanded
                        spacing: 0

                        Repeater {
                            model: keyboardShortcutsSection.categories
                            delegate: ColumnLayout {
                                Layout.fillWidth: true
                                Layout.topMargin: Theme.padding
                                spacing: Theme.spacingTight

                                required property var modelData

                                // Category heading — caps voice but a
                                // notch brighter than the card title;
                                // group separation is spacing now, no
                                // hairline rule.
                                Text {
                                    Layout.fillWidth: true
                                    text: modelData.heading
                                    color: Theme.textSecondary
                                    font.family: Theme.fontFamily
                                    font.pixelSize: Theme.fontSizeTiny
                                    font.bold: true
                                    font.capitalization: Font.AllUppercase
                                    font.letterSpacing: 0.5
                                }
                                Repeater {
                                    model: modelData.items
                                    delegate: RowLayout {
                                        Layout.fillWidth: true
                                        Layout.preferredHeight: Theme.rowHeightKV + 2
                                        spacing: Theme.spacingLoose
                                        required property var modelData

                                        Text {
                                            Layout.fillWidth: true
                                            text: modelData.label
                                            color: Theme.textPrimary
                                            font.family: Theme.fontFamily
                                            font.pixelSize: Theme.fontSizeSmall
                                            elide: Text.ElideRight
                                        }
                                        // Keycaps — each comma-separated
                                        // binding renders as its own
                                        // recessed cap (same well tone
                                        // the KvRow values use).
                                        Row {
                                            Layout.alignment: Qt.AlignVCenter
                                            spacing: Theme.spacing
                                            Repeater {
                                                // Per-platform pick. `keys` is the all-platforms fallback used
                                                // when an entry is identical on Windows + Mac (single letters,
                                                // F-keys, etc.). Modifier-bearing combos provide separate
                                                // keysWin / keysMac strings instead.
                                                model: {
                                                    const s = modelData.keys
                                                          ? modelData.keys
                                                          : (Qt.platform.os === "windows"
                                                             ? modelData.keysWin
                                                             : modelData.keysMac);
                                                    return s ? s.split(", ") : [];
                                                }
                                                delegate: Rectangle {
                                                    required property var modelData
                                                    width: capText.implicitWidth + 10
                                                    height: 16
                                                    radius: Theme.radiusSmall
                                                    color: Theme.surfaceRecess
                                                    Text {
                                                        id: capText
                                                        anchors.centerIn: parent
                                                        text: modelData
                                                        color: Theme.textSecondary
                                                        font.family: Theme.monoFamily
                                                        font.pixelSize: Theme.fontSizeMono
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
