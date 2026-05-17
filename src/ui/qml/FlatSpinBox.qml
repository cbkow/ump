import QtQuick
import QtQuick.Controls.Basic
import Qcv

// Flat squared SpinBox — matches the toolstrip vocabulary.
// Squared corners, Theme.border at rest, Theme.accent on focus.
// Up / down arrows use Phosphor carets, hover to accent.
SpinBox {
    id: root

    implicitHeight: 22
    font.family: Theme.monoFamily
    font.pixelSize: Theme.fontSizeSmall

    // Borderless recessed-slot styling matching FlatTextField.
    background: Rectangle {
        color: root.activeFocus
               ? Theme.surface
               : (root.hovered ? Theme.bgAlt : Theme.bg)
        radius: 0
        Rectangle {
            anchors.left:   parent.left
            anchors.right:  parent.right
            anchors.bottom: parent.bottom
            height: 1
            color: root.activeFocus ? Theme.accent : "transparent"
        }
    }

    contentItem: TextInput {
        text: root.displayText
        font: root.font
        color: Theme.textPrimary
        horizontalAlignment: Qt.AlignLeft
        verticalAlignment: Qt.AlignVCenter
        readOnly: !root.editable
        validator: root.validator
        inputMethodHints: Qt.ImhFormattedNumbersOnly
        leftPadding: Theme.spacing
        rightPadding: Theme.spacing
        selectByMouse: true
    }

    up.indicator: Rectangle {
        x: root.mirrored ? 0 : root.width - width
        height: root.height / 2
        implicitWidth: 18
        color: root.up.pressed
                ? Theme.accentMuted
                : (upMa.containsMouse ? Theme.surfaceHover : "transparent")
        border.width: 0
        Icon {
            anchors.centerIn: parent
            name: "caret-up"
            size: 10
            color: upMa.containsMouse ? Theme.accent : Theme.textSecondary
        }
        MouseArea {
            id: upMa
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.NoButton  // SpinBox handles clicks internally
        }
    }

    down.indicator: Rectangle {
        x: root.mirrored ? 0 : root.width - width
        y: root.height / 2
        height: root.height / 2
        implicitWidth: 18
        color: root.down.pressed
                ? Theme.accentMuted
                : (downMa.containsMouse ? Theme.surfaceHover : "transparent")
        border.width: 0
        Icon {
            anchors.centerIn: parent
            name: "caret-down"
            size: 10
            color: downMa.containsMouse ? Theme.accent : Theme.textSecondary
        }
        MouseArea {
            id: downMa
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.NoButton
        }
    }
}
