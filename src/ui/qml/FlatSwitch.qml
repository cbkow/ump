import QtQuick
import QtQuick.Controls.Basic
import Qcv

// Flat squared switch — track + sliding square handle.
//   off → track Theme.surface, handle Theme.textMuted
//   on  → track Theme.accentMuted, handle Theme.accent
// Set `attention: true` to flip the OFF state to an accent-blue
// fill — useful for surfacing toggles the user is meant to notice
// (e.g. "Engage OCIO" when the chain isn't applied).
// No animation curve, instant snap.
Switch {
    id: root
    property bool attention: false
    implicitWidth: 32
    implicitHeight: 18
    spacing: 0

    indicator: Rectangle {
        implicitWidth: 32
        implicitHeight: 18
        x: root.leftPadding
        y: parent.height / 2 - height / 2
        radius: 0
        // Track shifts a notch brighter when on; the handle
        // colour is the strong differentiator (bright vs muted).
        // attention mode paints OFF with accent so the toggle
        // visually pulls the eye while disengaged, and ON with
        // success so the engaged state reads as confirmed.
        color: root.attention
               ? (root.checked ? Theme.success : Theme.accent)
               : (root.checked ? Theme.borderStrong : Theme.surface)
        border.color: root.attention
                      ? (root.checked ? Qt.lighter(Theme.success, 1.15)
                                      : Theme.accentHover)
                      : (root.checked ? Theme.textMuted : Theme.border)
        border.width: 1

        Rectangle {
            x: root.checked ? parent.width - width - 2 : 2
            y: 2
            width: parent.height - 4
            height: parent.height - 4
            radius: 0
            color: root.checked
                   ? Theme.textBright
                   : (root.attention ? Theme.textBright : Theme.textMuted)
        }
    }

    contentItem: Text {
        leftPadding: root.indicator.width + Theme.spacing
        text: root.text
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fontSizeSmall
        color: Theme.textPrimary
        verticalAlignment: Text.AlignVCenter
    }
}
