// InspectorCard — tonal section container for the inspector rail
// (aesthetics pass 1). Each metadata group renders as a raised plane
// (Theme.card, radiusBase corners) on the rail's darker well instead
// of a divider-separated run on one flat sheet — sections read as
// places, not stripes.
//
// Title renders inside the card in the tiny-caps voice the old
// SectionHeader used (the 1-px rule is gone; the card edge is the
// separator now). Body children land in the default `content` slot,
// a ColumnLayout inset by the card's padding.

import QtQuick
import QtQuick.Layouts
import Qcv

Rectangle {
    id: root

    property string title: ""
    default property alias content: body.data
    property alias bodySpacing: body.spacing

    Layout.fillWidth: true
    color: Theme.card
    radius: Theme.radiusBase
    implicitHeight: body.y + body.implicitHeight + Theme.padding

    Text {
        id: titleText
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: Theme.padding
        anchors.rightMargin: Theme.padding
        anchors.topMargin: Theme.padding
        visible: root.title.length > 0
        height: visible ? implicitHeight : 0
        text: root.title
        color: Theme.textMuted
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fontSizeTiny
        font.bold: true
        font.capitalization: Font.AllUppercase
        font.letterSpacing: 0.8
        elide: Text.ElideRight
    }

    ColumnLayout {
        id: body
        anchors.top: titleText.visible ? titleText.bottom : parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: Theme.padding
        anchors.rightMargin: Theme.padding
        anchors.topMargin: titleText.visible ? 6 : Theme.padding
        spacing: Theme.spacingTight
    }
}
