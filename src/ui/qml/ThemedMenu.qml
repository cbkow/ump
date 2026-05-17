import QtQuick
import QtQuick.Controls
import Qcv

// Project-themed Menu wrapper.
//
// Two jobs:
//   1. Force popupType: Popup.Window so the popup z-orders above
//      the player's child HWND on Windows (F.2.1 invariant). All
//      menu sites in QCView need this; centralising avoids the
//      previous duplication of the inline assignment + comment.
//   2. Recolor the popup to our Theme so the parent menu and its
//      submenus read as the same surface. Fusion's defaults gave
//      a noticeably different fill in the top-level popup window
//      vs the MenuBar's in-scene parent.
//
// Item-level colors flow via `palette` — Fusion respects palette
// roles for menu items, so we don't need a custom contentItem or
// per-item delegate to recolor the rows.
Menu {
    id: themedMenu

    // popupType: Popup.Window is REQUIRED so the popup z-orders
    // above the player's child HWND on Windows (F.2.1 invariant).
    // Popup.Item renders in-scene, which tucks the menu behind the
    // player HWND whenever the menu geometrically overlaps the
    // centerStage area.
    //
    // Known issue (deferred 2026-05-14): with Popup.Window, Qt 6.11
    // menus opened via MenuBar leak click events through to QML
    // items underneath the popup (e.g. LeftRail rows). modal:true
    // does NOT prevent this on Window-type popups — the modal
    // overlay isn't created in the parent scene. Revisit if/when
    // Qt fixes this, or build a manual scene-overlay that's enabled
    // while any menu is open.
    popupType: Popup.Window

    background: Rectangle {
        implicitWidth:  200
        implicitHeight: 24
        color:        Theme.surface
        border.color: Theme.border
        border.width: 1
    }

    palette.window:           Theme.surface
    palette.windowText:       Theme.textPrimary
    palette.base:             Theme.surface
    palette.text:             Theme.textPrimary
    palette.button:           Theme.surface
    palette.buttonText:       Theme.textPrimary
    palette.highlight:        Theme.accentMuted
    palette.highlightedText:  Theme.textBright
    palette.mid:              Theme.border
    palette.midlight:         Theme.surfaceHover
    palette.dark:             Theme.bg
    palette.shadow:           Theme.bg

    // Disabled-state group — greyed-out menu items.
    palette.disabled.text:       Theme.textMuted
    palette.disabled.windowText: Theme.textMuted
    palette.disabled.buttonText: Theme.textMuted
}
