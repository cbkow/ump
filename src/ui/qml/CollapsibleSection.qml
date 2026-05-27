// CollapsibleSection — accordion-style section for the rails per
// Guide 03 §2 / §4.
//
// Header is a flat row with a Phosphor caret in a fixed-width
// slot matching Theme.gutterWidth, so the carets across multiple
// stacked sections (and the rail's own collapse chevron) all
// share the same x column. Squared corners, no fill border;
// hover paints the surfaceHover background.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Qcv

Pane {
    id: root
    property string title: ""
    // Optional Phosphor icon slug; when non-empty, renders to the
    // left of the title in the header row. Mirrors the transport-row
    // icon vocabulary so a user who clicked a transport button to
    // jump here recognizes the section by its glyph.
    property string iconName: ""
    property bool   expanded: true
    default property alias content: contentArea.data

    padding: 0
    Layout.fillWidth: true

    background: Rectangle {
        color: "transparent"
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // Header
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.headerHeight
            color: headerMouseArea.containsMouse
                   ? Theme.surfaceHover
                   : (root.expanded
                        ? Theme.sectionOpenBg : "transparent")

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

                // Optional section icon — appears between the caret
                // slot and the title when iconName is set.
                Icon {
                    visible: root.iconName.length > 0
                    Layout.preferredWidth: Theme.iconSizeToolbar
                    Layout.preferredHeight: Theme.iconSizeToolbar
                    Layout.rightMargin: Theme.spacing
                    name: root.iconName
                    size: Theme.iconSizeToolbar
                    color: Theme.textSecondary
                }

                Text {
                    Layout.fillWidth: true
                    text: root.title
                    color: Theme.textPrimary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontSizeBase
                    font.bold: true
                    elide: Text.ElideRight
                }
                Item { Layout.preferredWidth: Theme.padding }
            }

            // Bottom divider — single-edge, only visible when
            // expanded so a stack of collapsed sections reads as
            // a clean list of header rows.
            Rectangle {
                visible: root.expanded
                anchors.left:  parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: Theme.dividerWidth
                color: Theme.divider
            }

            MouseArea {
                id: headerMouseArea
                anchors.fill: parent
                hoverEnabled: true
                onClicked: root.expanded = !root.expanded
            }
        }

        // Content
        Item {
            id: contentArea
            Layout.fillWidth: true
            visible: root.expanded
            implicitHeight: childrenRect.height
        }
    }
}
