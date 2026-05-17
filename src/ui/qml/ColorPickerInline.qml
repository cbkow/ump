// ColorPickerInline — reusable Photoshop-style inline color
// picker. Drop-in Pane (not Popup); add as a child of any Layout
// to embed an HSV picker.
//
// External API:
//   property color value         — current selection (always opaque)
//   signal valueChanged(color c) — fires on every interactive change
//
// Layout (~160 × 200):
//   ┌──────────┬──┐
//   │  S × V   │ H│
//   │  square  │  │
//   └──────────┴──┘
//   Hex: [######]
//   R: [_]  G: [_]  B: [_]
//
// Alpha is intentionally absent — the consumer (annotation strokes)
// requires fully-opaque colors so partial alpha doesn't smear the
// stroke-width fidelity. Hex is 6 digits (#RRGGBB) for the same
// reason. Internal HSV ground truth keeps the cursor sane even
// when saturation drops to zero.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Qcv

Pane {
    id: root
    padding: 8

    // Transparent — consumers wrap with a styled Pane and put
    // additional controls (e.g. stroke width) in the same
    // container without doubling borders.
    background: Item {}

    property color value: Qt.rgba(1.0, 0.0, 0.0, 1.0)

    // Layout switch.
    //   false (default): fields stack BELOW the SV+hue row. Picks
    //                    a narrow ~170-px footprint suitable for
    //                    cramped panels.
    //   true:            Hex / R / G / B as a vertical column to
    //                    the RIGHT of the SV+hue row. Stretches to
    //                    fill any remaining width — used in the
    //                    safety panel where the rail's ~280 wide.
    property bool wideFields: false

    // ---- HSV ground truth (alpha is always 1.0) -----------------
    property real _h: 0       // 0..1
    property real _s: 1
    property real _v: 1
    property bool _suppressFeedback: false

    function _setHsv(h, s, v) {
        _suppressFeedback = true;
        _h = h; _s = s; _v = v;
        value = Qt.hsva(h, s, v, 1.0);
        _suppressFeedback = false;
    }
    function _applyValueColor() {
        const c = value;
        let h = c.hsvHue;
        if (h < 0) h = _h;     // grayscale → preserve hue
        _suppressFeedback = true;
        _h = h;
        _s = c.hsvSaturation;
        _v = c.hsvValue;
        _suppressFeedback = false;
    }
    onValueChanged: { if (!_suppressFeedback) _applyValueColor(); }
    Component.onCompleted: _applyValueColor()

    function _toHex6(c) {
        const hh = (n) => {
            const v = Math.max(0, Math.min(255, Math.round(n * 255)));
            const s = v.toString(16);
            return s.length < 2 ? "0" + s : s;
        };
        return "#" + hh(c.r) + hh(c.g) + hh(c.b);
    }
    function _fromHex(text) {
        let s = text.trim();
        if (s.startsWith("#")) s = s.slice(1);
        if (s.length === 3) s = s[0]+s[0]+s[1]+s[1]+s[2]+s[2];
        if (s.length === 8) s = s.slice(0, 6);  // ignore legacy alpha
        if (s.length !== 6) return null;
        const r = parseInt(s.slice(0,2),16) / 255;
        const g = parseInt(s.slice(2,4),16) / 255;
        const b = parseInt(s.slice(4,6),16) / 255;
        if (isNaN(r) || isNaN(g) || isNaN(b)) return null;
        return Qt.rgba(r, g, b, 1.0);
    }

    // Hoisted so both the narrow (below) and wide (right column)
    // layouts can use it. QML inline-component declarations don't
    // hoist; declaring it at root scope keeps both code paths
    // valid regardless of which fields panel is visible.
    component ChannelField: TextField {
        id: cf
        Layout.fillWidth: true
        Layout.preferredHeight: 22
        font.family: Theme.monoFamily
        font.pixelSize: Theme.fontSizeSmall
        color: Theme.textPrimary
        selectByMouse: true
        horizontalAlignment: TextInput.AlignLeft
        leftPadding: Theme.spacing
        rightPadding: Theme.spacing
        // Borderless recessed-slot — matches FlatTextField. Rest at
        // Theme.bg, hover lifts to Theme.bgAlt, focus jumps to
        // Theme.surface and adds a 1-px accent bottom rule.
        background: Rectangle {
            color: cf.activeFocus
                   ? Theme.surface
                   : (cf.hovered ? Theme.bgAlt : Theme.bg)
            radius: 0
            Rectangle {
                anchors.left:   parent.left
                anchors.right:  parent.right
                anchors.bottom: parent.bottom
                height: 1
                color: cf.activeFocus ? Theme.accent : "transparent"
            }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 6

        // No "Color" header — the picker is unmistakable on its own
        // and the reclaimed vertical space goes to the SV square.

        // ---- SV square + Hue slider (+ wide fields) -------------
        // In wideFields mode the Hex / R / G / B column lives at
        // the right edge of this row and consumes the leftover
        // width; in narrow mode it falls back to the rows below.
        RowLayout {
            Layout.fillWidth: true
            spacing: 6

            Rectangle {
                id: svSquare
                Layout.preferredWidth: 152
                Layout.preferredHeight: 152
                radius: 2
                border.color: "#3a3a3a"
                border.width: 1
                color: "transparent"

                Rectangle {
                    anchors.fill: parent
                    anchors.margins: 1
                    gradient: Gradient {
                        orientation: Gradient.Horizontal
                        GradientStop { position: 0.0; color: "#ffffff" }
                        GradientStop {
                            position: 1.0
                            color: Qt.hsva(root._h, 1, 1, 1)
                        }
                    }
                }
                Rectangle {
                    anchors.fill: parent
                    anchors.margins: 1
                    gradient: Gradient {
                        orientation: Gradient.Vertical
                        GradientStop { position: 0.0; color: "#00000000" }
                        GradientStop { position: 1.0; color: "#ff000000" }
                    }
                }
                Rectangle {
                    width: 12; height: 12; radius: 6
                    border.color: "#ffffff"
                    border.width: 2
                    color: "transparent"
                    x: root._s * (svSquare.width - 2) - 5
                    y: (1 - root._v) * (svSquare.height - 2) - 5
                    Rectangle {
                        anchors.centerIn: parent
                        width: 8; height: 8; radius: 4
                        border.color: "#000000"
                        border.width: 1
                        color: "transparent"
                    }
                }
                MouseArea {
                    anchors.fill: parent
                    preventStealing: true
                    function update(mx, my) {
                        const w = svSquare.width  - 2;
                        const h = svSquare.height - 2;
                        const s = Math.max(0, Math.min(1, mx / w));
                        const v = 1 - Math.max(0, Math.min(1, my / h));
                        root._setHsv(root._h, s, v);
                    }
                    onPressed: (m) => update(m.x, m.y)
                    onPositionChanged: (m) => { if (pressed) update(m.x, m.y); }
                }
            }

            Rectangle {
                id: hueBar
                Layout.preferredWidth: root.wideFields ? 28 : 14
                Layout.preferredHeight: 152
                radius: 0
                border.color: Theme.border
                border.width: 1
                gradient: Gradient {
                    orientation: Gradient.Vertical
                    GradientStop { position: 0.0;     color: "#ff0000" }
                    GradientStop { position: 0.16667; color: "#ffff00" }
                    GradientStop { position: 0.33333; color: "#00ff00" }
                    GradientStop { position: 0.5;     color: "#00ffff" }
                    GradientStop { position: 0.66667; color: "#0000ff" }
                    GradientStop { position: 0.83333; color: "#ff00ff" }
                    GradientStop { position: 1.0;     color: "#ff0000" }
                }
                Rectangle {
                    width: parent.width + 4
                    height: 3
                    x: -2
                    y: root._h * (hueBar.height - 1) - 1
                    color: "#ffffff"
                    border.color: "#000000"
                    border.width: 1
                }
                MouseArea {
                    anchors.fill: parent
                    preventStealing: true
                    function update(my) {
                        const h = Math.max(0, Math.min(1,
                            my / (hueBar.height - 1)));
                        root._setHsv(h, root._s, root._v);
                    }
                    onPressed: (m) => update(m.y)
                    onPositionChanged: (m) => { if (pressed) update(m.y); }
                }
            }

            // Wide-mode fields column. Same TextFields as the
            // narrow-mode rows below; we instantiate one or the
            // other (visible-gated) so only one set is in the
            // focus chain at a time.
            ColumnLayout {
                visible: root.wideFields
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 2

                Text {
                    text: qsTr("Hex")
                    color: Theme.textMuted
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontSizeTiny
                }
                TextField {
                    id: hexFieldWide
                    Layout.fillWidth: true
                    Layout.preferredHeight: 22
                    font.family: Theme.monoFamily
                    font.pixelSize: Theme.fontSizeSmall
                    color: Theme.textPrimary
                    selectByMouse: true
                    horizontalAlignment: TextInput.AlignLeft
                    leftPadding: Theme.spacing
                    rightPadding: Theme.spacing
                    text: root._toHex6(root.value)
                    onAccepted: {
                        const c = root._fromHex(text);
                        if (c) {
                            root._setHsv(c.hsvHue >= 0 ? c.hsvHue : root._h,
                                          c.hsvSaturation, c.hsvValue);
                        }
                        text = root._toHex6(root.value);
                    }
                    background: Rectangle {
                        color: hexFieldWide.activeFocus
                               ? Theme.surface
                               : (hexFieldWide.hovered ? Theme.bgAlt : Theme.bg)
                        radius: 0
                        Rectangle {
                            anchors.left:   parent.left
                            anchors.right:  parent.right
                            anchors.bottom: parent.bottom
                            height: 1
                            color: hexFieldWide.activeFocus ? Theme.accent : "transparent"
                        }
                    }
                }
                Text {
                    text: qsTr("R")
                    color: Theme.textMuted
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontSizeTiny
                }
                ChannelField {
                    text: Math.round(root.value.r * 255)
                    onAccepted: {
                        const v = Math.max(0, Math.min(255, parseInt(text)));
                        if (!isNaN(v)) {
                            const c = Qt.rgba(v/255, root.value.g,
                                                root.value.b, 1.0);
                            const hue = c.hsvHue >= 0 ? c.hsvHue : root._h;
                            root._setHsv(hue, c.hsvSaturation, c.hsvValue);
                        }
                        text = Math.round(root.value.r * 255);
                    }
                }
                Text {
                    text: qsTr("G")
                    color: Theme.textMuted
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontSizeTiny
                }
                ChannelField {
                    text: Math.round(root.value.g * 255)
                    onAccepted: {
                        const v = Math.max(0, Math.min(255, parseInt(text)));
                        if (!isNaN(v)) {
                            const c = Qt.rgba(root.value.r, v/255,
                                                root.value.b, 1.0);
                            const hue = c.hsvHue >= 0 ? c.hsvHue : root._h;
                            root._setHsv(hue, c.hsvSaturation, c.hsvValue);
                        }
                        text = Math.round(root.value.g * 255);
                    }
                }
                Text {
                    text: qsTr("B")
                    color: Theme.textMuted
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontSizeTiny
                }
                ChannelField {
                    text: Math.round(root.value.b * 255)
                    onAccepted: {
                        const v = Math.max(0, Math.min(255, parseInt(text)));
                        if (!isNaN(v)) {
                            const c = Qt.rgba(root.value.r, root.value.g,
                                                v/255, 1.0);
                            const hue = c.hsvHue >= 0 ? c.hsvHue : root._h;
                            root._setHsv(hue, c.hsvSaturation, c.hsvValue);
                        }
                        text = Math.round(root.value.b * 255);
                    }
                }
                Item { Layout.fillHeight: true }
            }
        }

        // ---- Narrow-mode rows (gated; the wide-mode column above
        //      replaces them when wideFields=true) ---------------
        // ---- Hex field ------------------------------------------
        RowLayout {
            visible: !root.wideFields
            Layout.fillWidth: true
            spacing: Theme.spacing
            Text {
                text: qsTr("Hex")
                color: Theme.textMuted
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontSizeTiny
                Layout.alignment: Qt.AlignVCenter
            }
            TextField {
                id: hexField
                Layout.fillWidth: true
                Layout.preferredHeight: 22
                font.family: Theme.monoFamily
                font.pixelSize: Theme.fontSizeSmall
                color: Theme.textPrimary
                selectByMouse: true
                horizontalAlignment: TextInput.AlignLeft
                leftPadding: Theme.spacing
                rightPadding: Theme.spacing
                text: root._toHex6(root.value)
                onAccepted: {
                    const c = root._fromHex(text);
                    if (c) {
                        root._setHsv(c.hsvHue >= 0 ? c.hsvHue : root._h,
                                      c.hsvSaturation, c.hsvValue);
                    }
                    text = root._toHex6(root.value);
                }
                background: Rectangle {
                    color: hexField.activeFocus
                           ? Theme.surface
                           : (hexField.hovered ? Theme.bgAlt : Theme.bg)
                    radius: 0
                    Rectangle {
                        anchors.left:   parent.left
                        anchors.right:  parent.right
                        anchors.bottom: parent.bottom
                        height: 1
                        color: hexField.activeFocus ? Theme.accent : "transparent"
                    }
                }
            }
        }

        // ---- RGB numeric fields (narrow mode only) -------------
        GridLayout {
            visible: !root.wideFields
            Layout.fillWidth: true
            columns: 3
            columnSpacing: Theme.spacing
            rowSpacing: 2
            Text {
                text: qsTr("R")
                color: Theme.textMuted
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontSizeTiny
            }
            Text {
                text: qsTr("G")
                color: Theme.textMuted
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontSizeTiny
            }
            Text {
                text: qsTr("B")
                color: Theme.textMuted
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontSizeTiny
            }

            ChannelField {
                text: Math.round(root.value.r * 255)
                onAccepted: {
                    const v = Math.max(0, Math.min(255, parseInt(text)));
                    if (!isNaN(v)) {
                        const c = Qt.rgba(v/255, root.value.g,
                                            root.value.b, 1.0);
                        const hue = c.hsvHue >= 0 ? c.hsvHue : root._h;
                        root._setHsv(hue, c.hsvSaturation, c.hsvValue);
                    }
                    text = Math.round(root.value.r * 255);
                }
            }
            ChannelField {
                text: Math.round(root.value.g * 255)
                onAccepted: {
                    const v = Math.max(0, Math.min(255, parseInt(text)));
                    if (!isNaN(v)) {
                        const c = Qt.rgba(root.value.r, v/255,
                                            root.value.b, 1.0);
                        const hue = c.hsvHue >= 0 ? c.hsvHue : root._h;
                        root._setHsv(hue, c.hsvSaturation, c.hsvValue);
                    }
                    text = Math.round(root.value.g * 255);
                }
            }
            ChannelField {
                text: Math.round(root.value.b * 255)
                onAccepted: {
                    const v = Math.max(0, Math.min(255, parseInt(text)));
                    if (!isNaN(v)) {
                        const c = Qt.rgba(root.value.r, root.value.g,
                                            v/255, 1.0);
                        const hue = c.hsvHue >= 0 ? c.hsvHue : root._h;
                        root._setHsv(hue, c.hsvSaturation, c.hsvValue);
                    }
                    text = Math.round(root.value.b * 255);
                }
            }
        }
    }
}
