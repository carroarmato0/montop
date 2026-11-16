pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Montop

// Every GPU with what its driver reports: how busy it is over time, its
// memory, and its clock and sensors. A discrete GPU that's asleep is left
// asleep, and one whose driver reports nothing says so.
Panel {
    id: root

    readonly property var gpus: Monitor.gpus

    function percent(fraction) {
        return Math.round(fraction * 100).toLocaleString(Translator.locale, 'f', 0) + "%"
    }
    // Clock, temperature, power and fan, as far as they're known.
    function facts(gpu) {
        const parts = []
        // Intel reports 0 MHz while the GPU is powered down between frames.
        if (gpu.mhz > 0)
            parts.push(qsTr("%1 GHz").arg((gpu.mhz / 1000).toLocaleString(Translator.locale, 'f', 2)))
        else if (gpu.mhz === 0)
            parts.push(qsTr("idle"))
        if (gpu.temperature >= 0)
            parts.push(Math.round(gpu.temperature) + " °C")
        if (gpu.watts >= 0)
            parts.push(gpu.watts.toLocaleString(Translator.locale, 'f', 1) + " W")
        if (gpu.fan >= 0)
            parts.push(qsTr("fan %1").arg(root.percent(gpu.fan / 100)))
        return parts.join("  ·  ")
    }

    title: qsTr("GPU")
    key: "6"
    detail: gpus.length === 1 ? gpus[0].name : qsTr("%n GPUs", "", gpus.length)

    ColumnLayout {
        anchors.fill: parent
        spacing: 8

        // A count, not the list: a list model would rebuild every graph on
        // every sample.
        Repeater {
            model: root.gpus.length
            delegate: ColumnLayout {
                id: section
                required property int index
                readonly property var gpu: root.gpus[index] || ({})

                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 4

                // With more than one GPU each says which it is.
                Label {
                    visible: root.gpus.length > 1
                    Layout.fillWidth: true
                    text: section.gpu.name || ""
                    color: Theme.colors.foreground
                    font.bold: true
                    elide: Text.ElideRight
                }

                Item {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.minimumHeight: 36

                    Graph {
                        anchors.fill: parent
                        visible: section.gpu.busy >= 0
                        series: "gpu/" + section.index
                        blame: section.gpu.perApp ? "gpu" : ""
                        maximum: 1
                        color: Theme.colors.cyan
                    }
                    Column {
                        x: 8
                        y: 4
                        width: parent.width - 16
                        Label {
                            text: section.gpu.asleep ? qsTr("Asleep")
                                : section.gpu.busy >= 0 ? root.percent(section.gpu.busy) : "–"
                            font.pixelSize: 28
                            font.bold: true
                            color: section.gpu.asleep ? Theme.colors.dark_foreground : Theme.colors.bright_foreground
                        }
                        Label {
                            width: parent.width
                            text: section.gpu.asleep ? qsTr("Powered down until an app needs it.") : section.gpu.note || ""
                            visible: text.length > 0
                            color: Theme.colors.dark_foreground
                            wrapMode: Text.Wrap
                        }
                    }
                }

                // Its own memory where it has some; otherwise, for a GPU that
                // uses the computer's RAM, what the apps' GPU buffers take of it.
                // Rows hold their place, so the graph above never jumps.
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    visible: !section.gpu.asleep && (section.gpu.memoryTotal > 0 || section.gpu.appMemory >= 0)

                    Label {
                        text: qsTr("Memory")
                        color: Theme.colors.dark_foreground
                        elide: Text.ElideRight
                    }
                    Meter {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 24
                        visible: section.gpu.memoryTotal > 0
                        value: section.gpu.memoryTotal > 0 ? section.gpu.memoryUsed / section.gpu.memoryTotal : 0
                    }
                    Item { Layout.fillWidth: true; visible: !(section.gpu.memoryTotal > 0) }
                    Label {
                        horizontalAlignment: Text.AlignRight
                        text: section.gpu.memoryTotal > 0
                              ? qsTr("%1 of %2").arg(Monitor.bytes(section.gpu.memoryUsed)).arg(Monitor.bytes(section.gpu.memoryTotal))
                              : qsTr("%1 in RAM").arg(Monitor.bytes(section.gpu.appMemory))
                        color: Theme.colors.foreground
                    }
                }

                Label {
                    Layout.fillWidth: true
                    visible: !section.gpu.asleep && (section.gpu.mhz >= 0 || section.gpu.temperature >= 0
                                                    || section.gpu.watts >= 0 || section.gpu.fan >= 0)
                    text: root.facts(section.gpu)
                    color: Theme.colors.dark_foreground
                    elide: Text.ElideRight
                }
            }
        }
    }
}
