// MiniSpinner — tiny rotating-arc busy indicator. The stock
// BusyIndicator doesn't render at these sizes under the app's QQC2
// style (see StatusStrip's export chip, where this arc was first
// drawn), so this is the app's one shared spinner. Only animates
// while `running` AND visible — costs nothing at rest.
//
// Usage: MiniSpinner { running: image.status === Image.Loading }

import QtQuick
import Qcv

Item {
    id: root
    property bool running: true
    property color strokeColor: Theme.accent

    implicitWidth: 12
    implicitHeight: 12
    visible: running

    Canvas {
        anchors.fill: parent
        property real phase: 0
        onPaint: {
            const ctx = getContext("2d");
            ctx.reset();
            const cx = width / 2, cy = height / 2;
            const r  = width / 2 - 1.5;
            ctx.lineWidth   = 1.5;
            ctx.lineCap     = "round";
            ctx.strokeStyle = root.strokeColor;
            ctx.beginPath();
            ctx.arc(cx, cy, r, phase, phase + Math.PI * 1.4);
            ctx.stroke();
        }
        onPhaseChanged: requestPaint()
        NumberAnimation on phase {
            running: root.running && root.visible
            loops: Animation.Infinite
            from: 0
            to: 2 * Math.PI
            duration: 750
        }
    }
}
