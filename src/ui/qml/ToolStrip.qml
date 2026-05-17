import QtQuick
import QtQuick.Layouts
import Qcv

// 32-px-tall horizontal action bar. Default-property delegate
// forwards child elements into the inner RowLayout so call sites
// read naturally:
//
//   ToolStrip {
//       FlatButton { iconName: "plus" }
//       FlatButton { iconName: "trash" }
//       Item { Layout.fillWidth: true }     // spacer
//       FlatButton { iconName: "gear-six" }
//   }
//
// spacing: 0 by default — toolstrip buttons butt up against each
// other for a continuous strip. Insert an Item with fillWidth to
// break the strip into left/right groups.
Rectangle {
    id: root

    property bool showBottomDivider: true
    property alias spacing: layout.spacing
    default property alias content: layout.children

    implicitHeight: Theme.toolStripHeight
    color: Theme.toolbar

    Rectangle {
        visible: root.showBottomDivider
        anchors.left:   parent.left
        anchors.right:  parent.right
        anchors.bottom: parent.bottom
        height: Theme.dividerWidth
        color: Theme.divider
    }

    RowLayout {
        id: layout
        anchors.fill: parent
        spacing: 0
    }
}
