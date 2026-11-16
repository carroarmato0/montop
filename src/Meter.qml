import QtQuick
import Montop

// A thin horizontal gauge. By default it colors itself by how full it is,
// green to yellow to red from the theme, like btop's meters.
//
// It moves straight to each sample. At four samples a second that already
// reads as live, and easing twenty-odd gauges in QML between samples cost
// more CPU than everything else in the window together.
Item {
    id: meter

    property real value: 0
    property color color: value < 0.5 ? Theme.colors.green : value < 0.8 ? Theme.colors.yellow : Theme.colors.red

    implicitHeight: 6
    implicitWidth: 80

    Rectangle {
        anchors.fill: parent
        radius: Math.min(3, Theme.radius)
        color: Theme.colors.selection
    }

    Rectangle {
        // Snapped to device pixels: an unchanged width asks for no frame.
        height: parent.height
        width: Math.round(Math.max(0, Math.min(1, meter.value)) * parent.width * Screen.devicePixelRatio) / Screen.devicePixelRatio
        radius: Math.min(3, Theme.radius)
        color: meter.color
    }
}
