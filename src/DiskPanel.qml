import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Montop

Panel {
    id: root

    title: qsTr("Disks")
    key: "3"

    ColumnLayout {
        anchors.fill: parent
        spacing: 6

        // Reads above the line, writes below, on one scale per device. The
        // model is a count so delegates live on between samples.
        Repeater {
            model: Monitor.disks.length
            delegate: ColumnLayout {
                id: disk
                required property int index
                readonly property var info: Monitor.disks[index] || ({})

                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 2

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 12
                    RowLayout {
                        Layout.fillWidth: true
                        Layout.preferredWidth: 0
                        // The model gives way first; in a very narrow column
                        // the name is cut short rather than run under the rates.
                        clip: true
                        spacing: 8
                        Label { text: disk.info.name || ""; color: Theme.colors.bright_foreground }
                        Label {
                            // Gives way first when the row runs out of room.
                            Layout.fillWidth: true
                            Layout.preferredWidth: 0
                            text: disk.info.model || ""
                            color: Theme.colors.dark_foreground
                            elide: Text.ElideRight
                        }
                    }
                    Label { text: "▲ " + Monitor.rate(disk.info.readRate || 0); color: Theme.colors.green }
                    Label { text: "▼ " + Monitor.rate(disk.info.writeRate || 0); color: Theme.colors.red }
                    Label {
                        // Gives way, with an ellipsis, when the rates need the room.
                        Layout.minimumWidth: 0
                        Layout.preferredWidth: implicitWidth
                        text: qsTr("%1% busy").arg(Math.round((disk.info.busy || 0) * 100))
                        color: Theme.colors.dark_foreground
                        elide: Text.ElideRight
                    }
                }
                Graph {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.minimumHeight: 28
                    series: "disk/" + (disk.info.name || "") + "/read"
                    mirrorSeries: "disk/" + (disk.info.name || "") + "/write"
                    blame: "io"
                    color: Theme.colors.green
                    mirrorColor: Theme.colors.red
                }
            }
        }

        Repeater {
            model: Monitor.filesystems.length
            delegate: RowLayout {
                id: filesystem
                required property int index
                readonly property var info: Monitor.filesystems[index] || ({})

                Layout.fillWidth: true
                spacing: 10

                Label {
                    Layout.preferredWidth: 100
                    text: filesystem.info.path || ""
                    color: Theme.colors.dark_foreground
                    elide: Text.ElideMiddle
                }
                Meter {
                    Layout.fillWidth: true
                    value: filesystem.info.share || 0
                }
                Label {
                    text: qsTr("%1 of %2").arg(Monitor.bytes(filesystem.info.used || 0)).arg(Monitor.bytes(filesystem.info.total || 0))
                    color: Theme.colors.foreground
                }
            }
        }
    }
}
