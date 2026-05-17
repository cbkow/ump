import QtQuick
import QtQuick.Controls.Basic
import Qcv

// Flat slim slider matching ufb's style: 2-px-tall track in
// borderStrong, accent-coloured fill from origin to handle,
// 10×10 squared handle in accent (accentHover while pressed).
//
// Use:
//   FlatSlider { from: 0.0; to: 1.0; value: 0.5; onMoved: ... }
Slider {
    id: root

    background: Rectangle {
        x: root.leftPadding
        y: root.topPadding + root.availableHeight / 2 - height / 2
        width:  root.availableWidth
        height: 2
        // Track stays subtly visible when disabled — its color
        // already reads as low contrast so we leave it.
        color:  Theme.borderStrong
        Rectangle {
            width:  root.visualPosition * parent.width
            height: parent.height
            // Fill dims to textMuted when the slider is disabled
            // (e.g. volume slider with no audio loaded).
            color:  root.enabled ? Theme.textBright : Theme.textMuted
        }
    }
    handle: Rectangle {
        x: root.leftPadding
           + root.visualPosition * (root.availableWidth - width)
        y: root.topPadding + root.availableHeight / 2 - height / 2
        width: 10; height: 10
        color: !root.enabled
                 ? Theme.textMuted
                 : (root.pressed ? Theme.textPrimary : Theme.textBright)
    }
    // Faint overall fade for the whole control when disabled —
    // matches FlatButton's disabled affordance.
    opacity: root.enabled ? 1.0 : 0.55
}
