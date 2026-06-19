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

    // popupType: Popup.Window is REQUIRED so the tooltip z-orders
    // above the player's native viewport surface (CAMetalLayer on
    // macOS, child HWND on Windows). The default Popup.Item renders
    // in-scene, which tucks the tooltip BEHIND the player surface
    // whenever it overlaps the centerStage area — which is why
    // viewport-adjacent tooltips used to be disabled. Same lever as
    // ThemedMenu.qml. Safe for tooltips (unlike a separate-window
    // modal): they're non-interactive and auto-dismiss, so they
    // never hit the click-reorder problem.
    popupType: Popup.Window

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
