pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Montop

Panel {
    id: root

    // The ids the window hides; Main keeps them in the settings.
    property var hiddenDisks: []
    property var hiddenFilesystems: []
    signal hide(string kind, string id, string label)
    signal choose()

    readonly property var disks: Monitor.disks.filter(d => root.hiddenDisks.indexOf(d.id) < 0)
    readonly property var filesystems: Monitor.filesystems.filter(f => root.hiddenFilesystems.indexOf(f.id) < 0)
    readonly property int hiddenCount: Monitor.disks.length - disks.length + Monitor.filesystems.length - filesystems.length

    title: qsTr("Disks")
    key: "3"
    detail: hiddenCount > 0 ? qsTr("%n hidden", "", hiddenCount) : ""
    detailClickable: true
    onDetailClicked: root.choose()

    Label {
        anchors.fill: parent
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        wrapMode: Text.Wrap
        visible: root.disks.length === 0 && root.filesystems.length === 0 && root.hiddenCount > 0
        text: qsTr("Everything here is hidden. Press d to choose what to show.")
        color: Theme.colors.dark_foreground
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 6

        // Reads above the line, writes below, on one scale per device. The
        // model is a count so delegates live on between samples.
        Repeater {
            model: root.disks.length
            delegate: ColumnLayout {
                id: disk
                required property int index
                readonly property var info: root.disks[index] || ({})

                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 2

                TapHandler {
                    acceptedButtons: Qt.RightButton
                    onTapped: root.hide("disk", disk.info.id || "", disk.info.name || "")
                }

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
            model: root.filesystems.length
            delegate: RowLayout {
                id: filesystem
                required property int index
                readonly property var info: root.filesystems[index] || ({})

                Layout.fillWidth: true
                spacing: 10

                TapHandler {
                    acceptedButtons: Qt.RightButton
                    onTapped: root.hide("filesystem", filesystem.info.id || "", filesystem.info.path || "")
                }

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

        // Without disks, nothing fills the height: keep the filesystems on top.
        Item {
            Layout.fillHeight: true
            visible: root.disks.length === 0
        }
    }
}
