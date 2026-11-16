pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Montop

Panel {
    id: root

    function percent(fraction) {
        return Math.round(fraction * 100).toLocaleString(Translator.locale, 'f', 0) + "%"
    }

    title: qsTr("CPU")
    key: "1"
    detail: {
        const parts = [Monitor.cpuModel]
        if (Monitor.cpuFrequency > 0)
            parts.push(qsTr("%1 GHz").arg((Monitor.cpuFrequency / 1000).toLocaleString(Translator.locale, 'f', 2)))
        if (Monitor.cpuTemperature >= 0)
            parts.push(Math.round(Monitor.cpuTemperature) + " °C")
        return parts.join("  ·  ")
    }

    RowLayout {
        anchors.fill: parent
        spacing: 16

        Item {
            id: total
            Layout.fillWidth: true
            Layout.fillHeight: true

            Graph {
                anchors.fill: parent
                series: "cpu"
                blame: "cpu"
                maximum: 1
                color: Theme.accent
            }


            Column {
                x: 8
                y: 4
                Label {
                    text: root.percent(Monitor.cpuUsage)
                    font.pixelSize: 28
                    font.bold: true
                    color: Theme.colors.bright_foreground
                }
                Label {
                    text: {
                        return qsTr("load %1").arg(Monitor.loadAverage.map(v => v.toLocaleString(Translator.locale, 'f', 2)).join("  "))
                    }
                    color: Theme.colors.dark_foreground
                }
            }
        }

        // Per-core gauges in as many columns as it takes to fit the height.
        Grid {
            id: grid
            readonly property int rowHeight: 18
            readonly property int rowsFit: Math.max(1, Math.floor(root.height / rowHeight) - 2)

            Layout.alignment: Qt.AlignTop
            flow: Grid.TopToBottom
            rows: Math.min(Monitor.coreCount, rowsFit)
            columnSpacing: 18
            rowSpacing: 0

            // A count, not the list itself: a list model would rebuild every
            // gauge on every sample, and their animations with them.
            Repeater {
                model: Monitor.coreCount
                delegate: Row {
                    id: core
                    required property int index
                    readonly property real usage: Monitor.cores[index] ? Monitor.cores[index].usage : 0
                    height: grid.rowHeight
                    spacing: 8

                    Label {
                        width: 28
                        text: "C" + core.index
                        color: Theme.colors.dark_foreground
                        anchors.verticalCenter: parent.verticalCenter
                    }
                    Meter {
                        id: gauge
                        width: 70
                        value: core.usage
                        anchors.verticalCenter: parent.verticalCenter
                    }
                    Label {
                        width: 40
                        horizontalAlignment: Text.AlignRight
                        text: root.percent(core.usage)
                        color: Theme.colors.foreground
                        anchors.verticalCenter: parent.verticalCenter
                    }
                }
            }
        }
    }
}
