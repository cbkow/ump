// CollapsibleSection — accordion-style section for the rails.
//
// Aesthetics pass 2: renders as a raised card plane (Theme.card,
// radiusBase) on the rail well, matching the InspectorCards and the
// left rail's Media/Dual Views/Playlists cards. Header is the shared
// card voice — Phosphor caret in the fixed Theme.gutterWidth slot
// (so carets align across stacked sections) + tiny-caps muted title.
// The old open/closed header fills, section icon and bottom divider
// are gone; tone and spacing do the separation now.
//
// Only the left rail's preference sections (Safety Guides /
// Background / Settings) use this — the other rail sections carry
// bespoke card headers (counts, + buttons, list wells).

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Qcv

Pane {
    id: root
    property string title: ""
    property bool   expanded: true
    default property alias content: contentArea.data

    padding: 0
    Layout.fillWidth: true
    Layout.leftMargin: Theme.padding
    Layout.rightMargin: Theme.padding
    Layout.topMargin: Theme.padding

    background: Rectangle {
        color: Theme.card
        radius: Theme.radiusBase
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // Header
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.headerHeight
            radius: Theme.radiusBase
            color: headerMouseArea.containsMouse
                   ? Theme.surfaceHover : "transparent"

            RowLayout {
                anchors.fill: parent
                spacing: 0

                // Caret slot — fixed Theme.gutterWidth so the
                // glyph column aligns vertically across stacked
                // sections + the rail toggle.
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

                Text {
                    Layout.fillWidth: true
                    text: root.title
                    color: Theme.textMuted
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontSizeTiny
                    font.bold: true
                    font.capitalization: Font.AllUppercase
                    font.letterSpacing: 0.8
                    elide: Text.ElideRight
                }
                Item { Layout.preferredWidth: Theme.padding }
            }

            MouseArea {
                id: headerMouseArea
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: root.expanded = !root.expanded
            }
        }

        // Content — call sites do their own horizontal padding;
        // the trailing spacing keeps the last control off the
        // card's bottom edge even when the content ends flush.
        Item {
            id: contentArea
            Layout.fillWidth: true
            visible: root.expanded
            implicitHeight: childrenRect.height + Theme.spacing
        }
    }
}
