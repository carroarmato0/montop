pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Montop

// Every shortcut, one keypress away (?), as in every Omarchy app.
Rectangle {
    id: overlay
    signal closed

    color: Qt.rgba(0, 0, 0, Theme.dark ? 0.6 : 0.35)

    MouseArea {
        anchors.fill: parent
        onClicked: overlay.closed()
    }

    readonly property var shortcuts: [
        ["?", qsTr("Show or hide shortcuts")],
        ["Space", qsTr("Pause or resume updates")],
        ["+ / −", qsTr("Update less or more often")],
        ["1 – 6", qsTr("Show or hide CPU, memory, disks, network, processes, GPU")],
        ["a", qsTr("Show apps or every process")],
        ["↑ ↓  j k", qsTr("Select an app or process")],
        ["→ ←  l h  Enter", qsTr("Open or close an app")],
        ["PgUp PgDn", qsTr("Move a page")],
        ["g  G", qsTr("First or last process")],
        ["/", qsTr("Filter processes")],
        ["Esc", qsTr("Return to now, clear the filter, close this")],
        ["s", qsTr("Sort by the next column")],
        ["r", qsTr("Reverse the sort")],
        ["Shift K", qsTr("Show or hide kernel threads")],
        ["[  ]  {  }", qsTr("Rewind or go forward 10 s, or a minute")],
        ["p", qsTr("Pause or resume the selected app")],
        ["c", qsTr("Limit the selected app's CPU")],
        ["x", qsTr("End the selected app or process (SIGTERM)")],
        ["Shift X", qsTr("Kill the selected app or process (SIGKILL)")],
        ["q", qsTr("Quit")],
    ]

    Rectangle {
        anchors.centerIn: parent
        width: grid.implicitWidth + 48
        height: grid.implicitHeight + heading.height + 56
        color: Theme.colors.lighter_background
        border.width: 1
        border.color: Theme.accent
        radius: Theme.radius

        Label {
            id: heading
            x: 24
            y: 20
            text: qsTr("Shortcuts")
            color: Theme.accent
            font.bold: true
            font.pixelSize: 15
        }

        GridLayout {
            id: grid
            x: 24
            anchors.top: heading.bottom
            anchors.topMargin: 14
            columns: 2
            columnSpacing: 24
            rowSpacing: 6

            Repeater {
                model: overlay.shortcuts.length * 2
                delegate: Label {
                    required property int index
                    readonly property var entry: overlay.shortcuts[Math.floor(index / 2)]
                    text: entry[index % 2]
                    color: index % 2 === 0 ? Theme.colors.bright_foreground : Theme.colors.foreground
                    font.bold: index % 2 === 0
                }
            }
        }
    }
}
