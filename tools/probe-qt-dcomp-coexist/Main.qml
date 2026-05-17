// Main.qml — Phase F.1.b spike.
//
// Distinctive QML scene so we can visually verify that Qt's render is
// still happening on the HWND after we attach a DComp target to it.
// If Qt's QML disappears, DComp has taken over the HWND and we can't
// do same-HWND coexistence.

import QtQuick
import QtQuick.Controls
import QtQuick.Window

Window {
    width: 1024
    height: 768
    visible: true
    title: "Qt + DComp coexistence spike"
    color: "#3344aa"   // distinctive Qt-window blue (opaque again)

    // Big centered text — sits BEHIND the child-HWND DComp visual.
    // We expect to see this text overwritten by the centered DComp
    // overlay; the corner rectangles below remain visible as Qt UI.
    Text {
        anchors.centerIn: parent
        text: "Qt QML content"
        color: "white"
        font.pixelSize: 64
        font.bold: true
    }

    // Corner markers — should be visible AT THE CORNERS, outside our
    // DComp visual's centered region.
    Rectangle {
        width: 220; height: 80
        anchors.top: parent.top; anchors.left: parent.left
        anchors.margins: 16
        color: "#dd2222"
        radius: 8
        Text { anchors.centerIn: parent; text: "Top-Left (Qt)"; color: "white"; font.pixelSize: 18 }
    }
    Rectangle {
        width: 220; height: 80
        anchors.top: parent.top; anchors.right: parent.right
        anchors.margins: 16
        color: "#22aa22"
        radius: 8
        Text { anchors.centerIn: parent; text: "Top-Right (Qt)"; color: "white"; font.pixelSize: 18 }
    }
    Rectangle {
        width: 220; height: 80
        anchors.bottom: parent.bottom; anchors.left: parent.left
        anchors.margins: 16
        color: "#aaaa22"
        radius: 8
        Text { anchors.centerIn: parent; text: "Bottom-Left (Qt)"; color: "black"; font.pixelSize: 18 }
    }
    Rectangle {
        width: 220; height: 80
        anchors.bottom: parent.bottom; anchors.right: parent.right
        anchors.margins: 16
        color: "#aa22aa"
        radius: 8
        Text { anchors.centerIn: parent; text: "Bottom-Right (Qt)"; color: "white"; font.pixelSize: 18 }
    }

    // Button → popup. Tests whether native Qt popups render above
    // our DComp visual (which sits centered on the window).
    Button {
        text: "Open native popup"
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 120
        onClicked: popup.open()
    }

    // Window-type popup (Qt 6.8+). popupType: Popup.Window makes
    // this its own top-level OS window — DWM z-orders it ABOVE the
    // child HWND that holds our DComp player visual. That's the
    // architecture trade for Option A: any QML overlay that should
    // sit over the player rect must use popupType: Window.
    Popup {
        id: popup
        anchors.centerIn: Overlay.overlay
        width: 480; height: 280
        modal: true
        popupType: Popup.Window
        background: Rectangle { color: "#ffeecc"; border.color: "#aa6622"; border.width: 2; radius: 12 }
        contentItem: Column {
            spacing: 12
            anchors.fill: parent
            anchors.margins: 24
            Text { text: "Qt Window-Popup"; color: "#552200"; font.pixelSize: 28; font.bold: true }
            Text {
                text: "Window-type popup — own OS HWND.\nShould render ABOVE the DComp\nchild-HWND visual."
                color: "#552200"
                font.pixelSize: 16
            }
            Button { text: "Close"; onClicked: popup.close() }
        }
    }
}
