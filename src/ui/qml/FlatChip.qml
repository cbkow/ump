// FlatChip — the inspector's toggle-chip primitive (aesthetics
// pass 1). One home for the pill vocabulary that used to be
// re-implemented inline six times (PAR mode, Range, TC Origin,
// Audio Mix, FPS presets, cache stride) at two different heights:
// solid accent fill when active, transparent at rest, surfaceHover
// lift on hover, mono tiny label, uniform Theme.chipHeight.

import QtQuick
import Qcv

Rectangle {
    id: root

    property string label: ""
    property bool   active: false
    // Non-pickable chips render dimmed and ignore clicks (e.g. the
    // Range pills when the inspected item isn't the loaded source).
    property bool   interactive: true
    // Optional floor so numeric chip sets (fps presets, stride)
    // present as equal-width cells while text chips auto-fit.
    property int    minWidth: 0
    property string tooltip: ""

    signal clicked()

    implicitWidth: Math.max(minWidth, chipLabel.implicitWidth + 14)
    implicitHeight: Theme.chipHeight
    radius: Theme.radiusBase
    color: active
           ? Theme.accent
           : (chipMa.containsMouse && interactive
              ? Theme.surfaceHover : "transparent")
    opacity: interactive ? 1.0 : 0.55

    Text {
        id: chipLabel
        anchors.centerIn: parent
        text: root.label
        color: root.active ? Theme.textBright : Theme.textPrimary
        font.family: Theme.monoFamily
        font.pixelSize: Theme.fontSizeTiny
    }
    MouseArea {
        id: chipMa
        anchors.fill: parent
        hoverEnabled: true
        enabled: root.interactive
        cursorShape: root.interactive ? Qt.PointingHandCursor
                                      : Qt.ArrowCursor
        onClicked: root.clicked()
        FlatToolTip {
            visible: chipMa.containsMouse && root.tooltip.length > 0
            text: root.tooltip
        }
    }
}
