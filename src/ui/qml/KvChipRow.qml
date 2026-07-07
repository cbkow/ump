// KvChipRow — label column + wrapping chip Flow (aesthetics pass 1).
//
// The chip-picker counterpart to KvRow: same fixed label column
// (Theme.labelColWidth) so the chips start where values start.
// Children (usually a Repeater of FlatChips) land in the Flow via
// the default `chips` slot and wrap when the rail is narrow.

import QtQuick
import QtQuick.Layouts
import Qcv

RowLayout {
    id: root

    property string label: ""
    default property alias chips: flow.data

    Layout.fillWidth: true
    spacing: Theme.spacingLoose

    Text {
        text: root.label
        color: Theme.textSecondary
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fontSizeTiny
        Layout.preferredWidth: Theme.labelColWidth
        Layout.alignment: Qt.AlignTop
        // Optically center the 10-px label against the first
        // Theme.chipHeight chip row even when the Flow wraps.
        topPadding: Math.max(0, (Theme.chipHeight - implicitHeight) / 2)
        elide: Text.ElideRight
    }
    Flow {
        id: flow
        Layout.fillWidth: true
        Layout.alignment: Qt.AlignTop
        // Flow doesn't propagate childrenRect to implicitHeight by
        // default (Qt 6 quirk) — without this the parent layout
        // thinks the Flow needs zero height once it wraps.
        Layout.preferredHeight: childrenRect.height
        spacing: Theme.spacing
    }
}
