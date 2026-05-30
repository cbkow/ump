import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Qcv

// Flat squared-corner button matching the sister app's visual
// vocabulary. Variants:
//   - "default": transparent idle, surfaceHover on hover
//   - "primary": accent-blue fill
//   - "danger":  error-red fill (stays red even when disabled)
//   - "subtle":  persistent grey fill (surfaceHover idle, borderStrong
//                on hover) — marks an actionable button without
//                spending the accent-blue, which some toolstrips
//                reserve for state (e.g. the dual-view mode row).
//
// Use:
//   FlatButton { text: "Save"; variant: "primary"; onClicked: ... }
//   FlatButton { iconName: "x"; tooltipText: qsTr("Close") }
//   FlatButton { iconName: "trash"; text: "Delete"; variant: "danger" }
//
// Hover is an instant color swap (no Transition) — that snappy
// reactivity is what makes the UI feel flat rather than
// skeuomorphic.
//
// Tooltips: set `tooltipText` and the button shows a styled flat
// tooltip on hover. Don't use the attached `ToolTip.text/visible`
// pattern — that uses Qt's default tooltip which doesn't match
// our visual language.
Button {
    id: root

    property string variant: "default"        // "default" | "primary" | "danger" | "subtle"
    property string iconName: ""              // Phosphor icon slug
    property int    iconSize: Theme.iconSizeToolbar
    property color  iconColor: "transparent"  // transparent => track text color
    property string tooltipText: ""

    // When true, the checked state draws no accent border and no
    // fill — only the text/icon color shifts via checkedColor.
    // Use for filter chips that should sit visually flat with the
    // surrounding toolstrip.
    property bool   subtleChecked: false
    property color  checkedColor: Theme.textBright
    property color  uncheckedColor: Theme.textMuted
    // Optional override for the foreground (text/icon) color on
    // hover — useful for tinting a primary action green on hover
    // to match the addTile vocabulary. Transparent (alpha 0) means
    // "don't override; use the variant's normal fg".
    property color  hoverColor: "transparent"

    QtObject {
        id: pal
        // Default-variant "checked" reads as an accent-blue fill
        // — the toggled state is loud enough to pick out from the
        // unchecked siblings without needing a border.
        readonly property color bgIdle: {
            if (root.variant === "primary") return Theme.accent
            if (root.variant === "danger")  return Theme.error
            if (root.variant === "subtle")  return Theme.surfaceHover
            if (root.checked && !root.subtleChecked) return Theme.accent
            return "transparent"
        }
        readonly property color bgHover: {
            if (root.variant === "primary") return Theme.accentHover
            if (root.variant === "danger")  return Qt.lighter(Theme.error, 1.15)
            if (root.variant === "subtle")  return Theme.borderStrong
            if (root.checked && !root.subtleChecked) return Theme.accentHover
            return Theme.surfaceHover
        }
        readonly property color fg: {
            // Danger keeps its red text even when disabled — a
            // close / cancel action should always read as
            // destructive regardless of whether it's actionable
            // right now. Other variants dim to muted text.
            if (!root.enabled) {
                if (root.variant === "danger")  return Theme.textBright
                if (root.variant === "primary") return Theme.textMuted
                return Theme.textMuted
            }
            // Per-instance hover tint — wins over variant default
            // when set (e.g. addTile-style success-on-hover).
            if (root.hovered && root.hoverColor.a > 0) return root.hoverColor
            if (root.subtleChecked)
                return root.checked ? root.checkedColor : root.uncheckedColor
            // Default-variant when checked → bright on accent fill.
            if (root.variant === "default")
                return root.checked ? Theme.textBright : Theme.textPrimary
            return Theme.textBright
        }
    }

    implicitHeight: Theme.toolStripHeight
    leftPadding:  rowContent.implicitWidth > 0 ? Theme.padding : 0
    rightPadding: rowContent.implicitWidth > 0 ? Theme.padding : 0
    topPadding:    0
    bottomPadding: 0

    background: Rectangle {
        radius: Theme.radius
        color: root.hovered ? pal.bgHover : pal.bgIdle
        border.width: 0
    }

    contentItem: Row {
        id: rowContent
        spacing: 6

        Icon {
            visible: root.iconName.length > 0
            name: root.iconName
            size: root.iconSize
            color: root.iconColor.a > 0 ? root.iconColor : pal.fg
            anchors.verticalCenter: parent.verticalCenter
        }
        Label {
            visible: root.text.length > 0
            text: root.text
            color: pal.fg
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontSizeBase
            anchors.verticalCenter: parent.verticalCenter
        }
    }

    FlatToolTip {
        visible: root.hovered && root.tooltipText.length > 0
        text:    root.tooltipText
    }
}
