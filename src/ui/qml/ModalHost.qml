import QtQuick
import QtQuick.Layouts
import Qcv

// ModalHost — reusable in-scene modal frame for the UI-over-viewport
// framework (hazy-weaving-reddy plan).
//
// The native viewport surface composites ABOVE the Qt scene, so a modal
// can't simply z-order over it. Instead, while `opened` is true we drive
// WindowManager.beginModal()/endModal(), which CAPTURES the current frame
// as a frozen backdrop and HIDES the native surface — so this overlay
// (dim scrim + centered panel) becomes the top visible plane, sitting
// over that dimmed still (shown in centerStage). Input to the viewport is
// gated for the duration; chrome behind the scrim is covered by the
// scrim's own click-eater.
//
// Usage:
//   ModalHost {
//       id: myModal
//       title: qsTr("Settings")
//       Text { ... }            // default content goes in the panel body
//   }
//   ... myModal.opened = true
Item {
    id: host
    anchors.fill: parent
    z: 11000                       // above menuClickEater (z:10000)

    // Public API.
    property string title: ""
    property bool   opened: false
    property int    panelWidth: 420
    default property alias content: contentHolder.data
    signal closed()

    visible: opened
    focus: opened

    function close() { host.opened = false; }

    onOpenedChanged: {
        if (opened)
            WindowManager.beginModal();
        else {
            WindowManager.endModal();
            host.closed();
        }
    }

    Keys.onEscapePressed: host.close()

    // Dim scrim — covers (and eats clicks to) everything behind the panel.
    Rectangle {
        anchors.fill: parent
        color: "#000000"
        opacity: 0.55
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.AllButtons
            hoverEnabled: true
            onPressed: (m) => { m.accepted = true; }
        }
    }

    // Centered themed panel.
    Rectangle {
        id: panel
        anchors.centerIn: parent
        width: host.panelWidth
        implicitHeight: body.implicitHeight + 40
        height: implicitHeight
        color: Theme.surface
        border.color: Theme.border
        border.width: 1
        radius: Theme.radius

        // Swallow clicks on the panel so they don't reach the scrim eater.
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.AllButtons
            onPressed: (m) => { m.accepted = true; }
        }

        ColumnLayout {
            id: body
            anchors.fill: parent
            anchors.margins: 20
            spacing: 16

            RowLayout {
                Layout.fillWidth: true
                visible: host.title.length > 0
                spacing: 8
                Text {
                    Layout.fillWidth: true
                    text: host.title
                    color: Theme.textPrimary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontSizeBase
                    font.bold: true
                    elide: Text.ElideRight
                }
                FlatButton {
                    iconName: "x"
                    tooltipText: qsTr("Close")
                    onClicked: host.close()
                }
            }

            // Consumer-provided content.
            Column {
                id: contentHolder
                Layout.fillWidth: true
                spacing: 12
            }
        }
    }
}
