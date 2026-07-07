// KvRow — the inspector's label/value line (aesthetics pass 1).
//
// One rule for every tag/value pair: fixed label column
// (Theme.labelColWidth) so values align vertically across sections
// and panels; label in tiny sans secondary; value in mono one step
// below the sans size (Theme.fontSizeMono — JetBrainsMono's tall
// x-height otherwise overpowers the labels); fixed row height for a
// consistent vertical rhythm.

import QtQuick
import QtQuick.Layouts
import Qcv

RowLayout {
    id: root

    property string label: ""
    property string value: ""
    property color  valueColor: Theme.textPrimary
    // Trailing tiny sans annotation after the value, e.g. the
    // timecode row's "(frame 86400)".
    property string note: ""
    property int    valueElide: Text.ElideRight
    // Sans opt-out for values that are prose, not data.
    property bool   monoValue: true

    Layout.fillWidth: true
    Layout.preferredHeight: Theme.rowHeightKV
    spacing: Theme.spacingLoose

    Text {
        text: root.label
        color: Theme.textSecondary
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fontSizeTiny
        Layout.preferredWidth: Theme.labelColWidth
        elide: Text.ElideRight
    }
    // Value well — Resolve-style read-only field. surfaceRecess is
    // darker than the card plane, so the value sits in a subtle
    // inset without needing a border; all wells in a card stack
    // into an aligned field column.
    Rectangle {
        Layout.fillWidth: true
        Layout.fillHeight: true
        color: Theme.surfaceRecess
        radius: Theme.radiusSmall

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 6
            anchors.rightMargin: 6
            spacing: Theme.spacing

            Text {
                Layout.fillWidth: true
                text: root.value
                color: root.valueColor
                font.family: root.monoValue ? Theme.monoFamily
                                            : Theme.fontFamily
                font.pixelSize: root.monoValue ? Theme.fontSizeMono
                                               : Theme.fontSizeSmall
                elide: root.valueElide
            }
            Text {
                visible: root.note.length > 0
                text: root.note
                color: Theme.textMuted
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontSizeTiny
            }
        }
    }
}
