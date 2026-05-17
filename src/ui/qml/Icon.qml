import QtQuick
import Qcv
import "PhosphorIcons.js" as Phosphor

// Phosphor icon renderer. Use as:
//   Icon { name: "folder" }
//   Icon { name: "gear-six"; size: 14; color: Theme.accent }
//
// The 'name' property is a Phosphor slug (see phosphoricons.com).
// Unknown names render as an empty string — no warning, no
// fallback — so a typo'd name shows as a blank gap obvious in
// review.
Text {
    id: root
    property string name: ""
    property int    size: Theme.iconSizeToolbar

    text: Phosphor.code[name] || ""
    font.family: Theme.iconFamily
    font.pixelSize: size
    color: Theme.textPrimary
    horizontalAlignment: Text.AlignHCenter
    verticalAlignment: Text.AlignVCenter
    width: size
    height: size
}
