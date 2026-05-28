import QtQuick
import QtQuick.Controls
import Qcv

// Menu separator recolored to the Theme.
//
// Fusion's default draws the rule with Qt.lighter(Fusion.darkShade,
// 1.06) — a fixed shade DARKER than our Theme.surface menu fill, so
// the line reads as a dark groove that's hard to see. Fusion.darkShade
// isn't palette-driven, so ThemedMenu's palette overrides can't reach
// it; the only lever is a custom contentItem.
//
// This draws a 1px rule one step BRIGHTER than the surface (Theme.divider,
// same token as the rail dividers), matching Fusion's padding so the
// menu's vertical rhythm is unchanged.
MenuSeparator {
    padding: 5
    verticalPadding: 1
    contentItem: Rectangle {
        implicitWidth: 188
        implicitHeight: 1
        color: Theme.divider
    }
}
