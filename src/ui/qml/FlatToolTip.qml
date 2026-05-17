import QtQuick
import QtQuick.Controls.Basic
import Qcv

// Universal flat tooltip — opaque dark grey background, squared
// corners, single-line subtle border, Inter at fontSizeSmall.
//
// Use as a child of a hover target:
//
//   FlatSlider {
//       ...
//       ToolTip.delay: 600
//       FlatToolTip { visible: parent.hovered; text: "..." }
//   }
//
// FlatButton has this tooltip baked in via its `tooltipText`
// property; FlatToolTip is the freestanding form for sliders or
// any custom Item that wants the same look.
ToolTip {
    id: root

    delay: 600
    timeout: 5000

    background: Rectangle {
        color:  "#0e0e0e"
        border.color: Theme.border
        border.width: Theme.borderWidth
        radius: 0
    }
    contentItem: Text {
        text:  root.text
        color: Theme.textPrimary
        font.family:    Theme.fontFamily
        font.pixelSize: Theme.fontSizeSmall
    }
}
