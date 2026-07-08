// Toast — transient outcome feedback (product pass, 2026-07-07).
//
// The app's single feedback voice for operations that previously
// succeeded or failed silently (screenshots, LUT export, dual-view
// save, rejected drops…). One toast at a time — a new show()
// replaces the current message and restarts the clock. Click
// dismisses early.
//
// Placement: top-right of the viewport (Main.qml parents it to
// centerStage). That region is the native player HWND, which
// composites ABOVE the QML scene on Windows — so this is a
// popupType: Popup.Window OS popup, the same trick FlatToolTip and
// ThemedMenu use to float over the player surface. Non-modal,
// non-focusable: it never interrupts the review.
//
// Kinds: 0 = success (green check), 1 = warning (amber),
//        2 = error (red).

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Qcv

Popup {
    id: root

    function show(message, kind) {
        toastText.text = message;
        root.kind = (kind === undefined) ? 0 : kind;
        root.actionLabel = "";
        root.actionId = "";
        hideTimer.interval = 3400;
        root.open();
        hideTimer.restart();
    }

    // Toast with one action button (e.g. "Undo" on the delete
    // toasts). actionId is opaque — clicking the button emits
    // actionTriggered(actionId) and the host routes it (Main.qml →
    // WindowManager.invokeToastAction). Lingers longer than a
    // passive toast so there's time to react.
    function showAction(message, kind, actionLabel, actionId) {
        toastText.text = message;
        root.kind = (kind === undefined) ? 0 : kind;
        root.actionLabel = actionLabel || "";
        root.actionId = actionId || "";
        hideTimer.interval = 5600;
        root.open();
        hideTimer.restart();
    }

    property int kind: 0
    property string actionLabel: ""
    property string actionId: ""
    signal actionTriggered(string actionId)

    readonly property string iconName:
        kind === 2 ? "x-circle"
      : kind === 1 ? "warning"
                   : "check-circle"
    readonly property color iconColor:
        kind === 2 ? Theme.error
      : kind === 1 ? Theme.warn
                   : Theme.success

    popupType: Popup.Window
    // The auto-dismiss timer owns the lifetime — outside clicks and
    // Esc must keep reaching the app, not this popup.
    closePolicy: Popup.NoAutoClose
    modal: false
    focus: false
    dim: false
    padding: 0

    enter: Transition {
        NumberAnimation {
            property: "opacity"; from: 0; to: 1
            duration: 120; easing.type: Easing.OutQuad
        }
    }
    exit: Transition {
        NumberAnimation {
            property: "opacity"; from: 1; to: 0
            duration: 120; easing.type: Easing.OutQuad
        }
    }

    Timer {
        id: hideTimer
        interval: 3400
        onTriggered: root.close()
    }

    background: Rectangle {
        radius: Theme.radiusBase
        color: Theme.card
        border.color: Theme.borderStrong
        border.width: 1
    }

    contentItem: Item {
        implicitWidth: row.implicitWidth + 2 * Theme.paddingLoose
        implicitHeight: 32

        // Dismiss-on-click. Declared BEFORE the content row so the
        // action button (when present) stacks above it and wins the
        // click; text/icon don't accept mouse, so clicks anywhere
        // else still fall through here.
        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            onClicked: root.close()
        }

        RowLayout {
            id: row
            anchors.centerIn: parent
            spacing: Theme.spacingLoose
            Icon {
                name: root.iconName
                size: Theme.iconSizeToolbar
                color: root.iconColor
            }
            Text {
                id: toastText
                color: Theme.textPrimary
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontSizeBase
            }
            // Action button (undo toasts). Raised + rounded — it
            // carries a text label, so it keeps radiusBase per the
            // corner rule (only icon-only inline buttons square off).
            FlatButton {
                visible: root.actionLabel.length > 0
                variant: "raised"
                text: root.actionLabel
                Layout.preferredHeight: 22
                onClicked: {
                    root.actionTriggered(root.actionId);
                    root.close();
                }
            }
        }
    }
}
