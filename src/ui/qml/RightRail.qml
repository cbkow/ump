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
        color: "#1c1c1c"
        // Polish — single-edge divider on the left rather than a
        // 4-sided frame.
        Rectangle {
            anchors.left:   parent.left
            anchors.top:    parent.top
            anchors.bottom: parent.bottom
            width:  1
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
                    Layout.preferredWidth: Theme.gutterWidth
                    Layout.fillHeight: true
                    // No idle background — the chevron icon alone
                    // reads as the target; hover still brightens
                    // the gutter so there's clear click feedback.
                    Rectangle {
                        anchors.fill: parent
                        color: railToggleMa.containsMouse
                               ? Theme.surfaceHover
                               : "transparent"
                    }
                    Icon {
                        anchors.centerIn: parent
                        name: root.collapsed ? "caret-double-left" : "caret-double-right"
                        size: Theme.iconSizeSmall
                        color: railToggleMa.containsMouse
                               ? Theme.textPrimary : Theme.textSecondary
                    }
                    // No tooltip — sits next to the native player
                    // child window; QML tooltips stack below it.
                    MouseArea {
                        id: railToggleMa
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        hoverEnabled: true
                        onClicked: root.toggleCollapsed()
                    }
                }
            }
            Rectangle {
                anchors.left:   parent.left
                anchors.right:  parent.right
                anchors.bottom: parent.bottom
                height: Theme.dividerWidth
                color:  Theme.divider
            }
        }

        // Spacer keeps the header pinned to the top when the
        // body ScrollView is hidden. (Without it, the
        // ColumnLayout drifts the lone header item toward the
        // rail's vertical center.)
        Item {
            visible: root.collapsed
            Layout.fillWidth:  true
            Layout.fillHeight: true
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
                CollapsibleSection {
                    id: keyboardShortcutsSection
                    Layout.fillWidth: true
                    title: qsTr("Keyboard Shortcuts")
                    iconName: "keyboard"
                    expanded: false

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
                                { label: "Minimal Mode",        keysWin: "Ctrl+−",       keysMac: "⌘−"     },
                                { label: "Default View",        keysWin: "Ctrl+0, Ctrl+R", keysMac: "⌘0, ⌘R" },
                                { label: "Show All Panels",     keysWin: "Ctrl+9",       keysMac: "⌘9"     },
                                { label: "Left Rail",           keysWin: "Ctrl+1",       keysMac: "⌘1"     },
                                { label: "Right Rail",          keysWin: "Ctrl+2",       keysMac: "⌘2"     },
                                { label: "Color Panel",         keysWin: "Ctrl+3",       keysMac: "⌘3"     },
                                { label: "Notes Panel",         keysWin: "Ctrl+4",       keysMac: "⌘4"     },
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
                                { label: "Zoom (cursor anchored)",   keys: "Scroll" },
                                { label: "Pan horizontally",         keysWin: "Alt+Scroll", keysMac: "⌥Scroll" },
                                { label: "Drag viewport (overview)", keys: "Drag middle" },
                                { label: "Resize viewport (zoom)",   keys: "Drag edge" },
                            ]
                        },
                    ]

                    ColumnLayout {
                        x: 10
                        width: parent ? parent.width - 20 : 0
                        spacing: 0

                        Repeater {
                            model: keyboardShortcutsSection.categories
                            delegate: ColumnLayout {
                                Layout.fillWidth: true
                                Layout.topMargin: 8
                                spacing: 2

                                required property var modelData

                                Text {
                                    Layout.fillWidth: true
                                    text: modelData.heading
                                    color: "#888"
                                    font.pixelSize: 10
                                    font.bold: true
                                    font.capitalization: Font.AllUppercase
                                    font.letterSpacing: 0.5
                                }
                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 1
                                    Layout.bottomMargin: 2
                                    color: "#262626"
                                }
                                Repeater {
                                    model: modelData.items
                                    delegate: RowLayout {
                                        Layout.fillWidth: true
                                        spacing: 8
                                        required property var modelData

                                        Text {
                                            Layout.fillWidth: true
                                            text: modelData.label
                                            color: "#cccccc"
                                            font.pixelSize: 11
                                            elide: Text.ElideRight
                                        }
                                        Text {
                                            // Per-platform pick. `keys` is the all-platforms fallback used
                                            // when an entry is identical on Windows + Mac (single letters,
                                            // F-keys, etc.). Modifier-bearing combos provide separate
                                            // keysWin / keysMac strings instead.
                                            text: modelData.keys
                                                  ? modelData.keys
                                                  : (Qt.platform.os === "windows"
                                                     ? modelData.keysWin
                                                     : modelData.keysMac)
                                            color: "#888"
                                            font.pixelSize: 11
                                            font.family: "Menlo, Monospace"
                                            horizontalAlignment: Text.AlignRight
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
