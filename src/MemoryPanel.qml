pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Montop

Panel {
    id: root

    function amount(kind) {
        switch (kind) {
        case "used": return Monitor.memoryUsed
        case "cached": return Monitor.memoryCached
        case "available": return Monitor.memoryAvailable
        case "swap": return Monitor.swapUsed
        }
        return 0
    }

    title: qsTr("Memory")
    key: "2"
    detail: Monitor.bytes(Monitor.memoryTotal)

    ColumnLayout {
        anchors.fill: parent
        spacing: 6

        Graph {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: 24
            series: "memory"
            blame: "memory"
            maximum: 1
            color: Theme.colors.magenta
        }

        // The rows are fixed and read their numbers live, so they aren't
        // rebuilt on every sample and their gauges can glide.
        Repeater {
            model: ["used", "cached", "available", "swap"]
            delegate: RowLayout {
                id: memoryRow
                required property string modelData
                readonly property real total: modelData === "swap" ? Monitor.swapTotal : Monitor.memoryTotal
                readonly property real amount: root.amount(modelData)

                Layout.fillWidth: true
                spacing: 10
                visible: total > 0

                Label {
                    Layout.preferredWidth: 100
                    text: {
                        switch (memoryRow.modelData) {
                        case "used": return qsTr("Used")
                        case "cached": return qsTr("Cached")
                        case "available": return qsTr("Available")
                        }
                        return qsTr("Swap")
                    }
                    color: Theme.colors.dark_foreground
                    elide: Text.ElideRight
                }
                Meter {
                    Layout.fillWidth: true
                    value: memoryRow.total > 0 ? memoryRow.amount / memoryRow.total : 0
                    color: memoryRow.modelData === "cached" ? Theme.colors.blue
                         : memoryRow.modelData === "available" ? Theme.colors.cyan
                         : value < 0.5 ? Theme.colors.green : value < 0.8 ? Theme.colors.yellow : Theme.colors.red
                }
                Label {
                    Layout.preferredWidth: 80
                    horizontalAlignment: Text.AlignRight
                    text: Monitor.bytes(memoryRow.amount)
                    color: Theme.colors.foreground
                }
            }
        }
    }
}
