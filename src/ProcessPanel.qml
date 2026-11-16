pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Montop

Panel {
    id: root

    readonly property var model: Monitor.processes
    // Selection follows the app or process ("g:<app>", "p:<pid>"), not the
    // row, so it stays put while the list re-sorts under it every sample.
    property string selectedKey
    // Bumped on every sample, so bindings that ask the model re-run.
    property int tick: 0
    readonly property int selectedRow: {
        tick // qmllint disable confusing-expression-statement
        return model.rowOf(selectedKey)
    }
    readonly property var selected: {
        tick // qmllint disable confusing-expression-statement
        return selectedRow >= 0 ? model.get(selectedRow) : null
    }

    Connections {
        target: Monitor
        function onSampled() { root.tick++ }
        function onRewindChanged() { root.tick++ }
    }
    Connections {
        target: root.model
        function onSortChanged() { root.tick++ }
        function onFilterChanged() { root.tick++ }
        function onGroupedChanged() { root.tick++ }
        function onCountChanged() { root.tick++ }
    }
    property alias filtering: filterField.activeFocus

    function select(row) {
        if (model.count === 0)
            return
        row = Math.max(0, Math.min(model.count - 1, row))
        selectedKey = model.keyAt(row)
        list.positionViewAtIndex(row, ListView.Contain)
    }
    function move(delta) { select((selectedRow < 0 ? -1 : selectedRow) + delta) }
    function page(direction) { move(direction * Math.max(1, Math.floor(list.height / 22) - 1)) }
    // Apps open and close from the keyboard: right or Enter opens, left
    // closes, and on a process left goes up to its app.
    function expand() {
        if (selected && selected.isGroup)
            model.setExpanded(selected.key.slice(2), true)
    }
    function collapse() {
        if (!selected)
            return
        if (selected.isGroup) {
            model.setExpanded(selected.key.slice(2), false)
            return
        }
        const parent = model.parentKey(selected.key)
        if (parent.length > 0) {
            model.setExpanded(parent.slice(2), false)
            selectedKey = parent
            list.positionViewAtIndex(model.rowOf(parent), ListView.Contain)
        }
    }
    function toggle(key) {
        model.setExpanded(key.slice(2), !model.isExpanded(key.slice(2)))
    }
    // Switching views keeps the selection on the same thing where it can.
    function toggleGrouped() {
        const parent = model.parentKey(selectedKey)
        model.grouped = !model.grouped
        if (model.rowOf(selectedKey) < 0 && parent.length > 0)
            selectedKey = parent
        if (selectedRow >= 0)
            list.positionViewAtIndex(selectedRow, ListView.Contain)
    }
    function kindLabel(kind) {
        switch (kind) {
        case "app": return qsTr("app")
        case "service": return qsTr("your service")
        case "system": return qsTr("system service")
        case "kernel": return qsTr("kernel threads")
        case "session": return qsTr("login session")
        case "process": return qsTr("process")
        }
        return kind
    }

    function groupDetail(kind, count, frozen, limit) {
        const parts = [kindLabel(kind), qsTr("%n processes", "", count)]
        if (frozen)
            parts.unshift(qsTr("paused"))
        if (limit > 0)
            parts.unshift(qsTr("limited to %1%").arg(Math.round(limit)))
        return parts.join("  ·  ")
    }

    function startFilter() { filterField.forceActiveFocus(); filterField.selectAll() }
    function clearFilter() { filterField.text = ""; list.forceActiveFocus() }

    function number(value, decimals) { return value.toLocaleString(Translator.locale, 'f', decimals) }

    readonly property var appWindow: ApplicationWindow.window
    readonly property bool rewound: Monitor.rewoundTo > 0
    title: rewound ? (model.grouped ? qsTr("Apps at %1") : qsTr("Processes at %1")).arg(appWindow.clock(Monitor.rewoundTo))
         : model.grouped ? qsTr("Apps") : qsTr("Processes")
    key: "5"
    detailColor: rewound ? Theme.accent : Theme.colors.dark_foreground
    detail: {
        // The past keeps only what was busy or large.
        if (rewound)
            return qsTr("busy and large apps only")
        if (model.filter.length > 0 && !model.grouped)
            return qsTr("%1 of %2 match").arg(model.count).arg(model.total)
        const parts = [qsTr("%n processes", "", model.total), qsTr("%n threads", "", Monitor.taskCount)]
        if (model.grouped)
            parts.unshift(qsTr("%n apps", "", model.groupCount))
        return parts.join("  ·  ")
    }

    // Columns as [title, sort column, width, alignment]; width 0 fills.
    readonly property var columns: [
        { title: qsTr("PID"), sort: ProcessModel.Pid, width: 56, right: true },
        { title: qsTr("Name"), sort: ProcessModel.Name, width: 130, right: false },
        { title: qsTr("Command"), sort: -1, width: 0, right: false },
        { title: qsTr("User"), sort: ProcessModel.User, width: 80, right: false },
        { title: qsTr("Thr"), sort: ProcessModel.Threads, width: 36, right: true },
        { title: qsTr("CPU%"), sort: ProcessModel.Cpu, width: 56, right: true },
        { title: qsTr("GPU%"), sort: ProcessModel.Gpu, width: 56, right: true },
        { title: qsTr("Memory"), sort: ProcessModel.Memory, width: 76, right: true },
        { title: qsTr("Read/s"), sort: ProcessModel.Read, width: 92, right: true },
        { title: qsTr("Write/s"), sort: ProcessModel.Write, width: 92, right: true },
    ]
    readonly property int spacing: 8
    readonly property int commandMinimum: 120
    // In a narrow window (a half-screen tile, say) the least telling columns
    // give way first, until what's left fits. The command goes first since
    // the footer shows the selected one in full anyway.
    readonly property var shown: {
        const available = list.width - 12
        const dropOrder = [2, 6, 4, 9, 8, 3]
        // GPU% only where a GPU says which app uses it.
        const visible = columns.map((c, i) => i !== 6 || Monitor.gpuPerApp)
        const needed = () => columns.reduce((sum, c, i) => visible[i] ? sum + (c.width > 0 ? c.width : commandMinimum) + spacing : sum, -spacing)
        for (const index of dropOrder) {
            if (needed() <= available)
                break
            visible[index] = false
        }
        return visible
    }
    readonly property int commandWidth: list.width - 12
        - columns.reduce((sum, c, i) => shown[i] && c.width > 0 ? sum + c.width + spacing : sum, 0)
    function columnWidth(index) {
        if (!shown[index])
            return 0
        // Without a command column the name takes its room.
        if (index === 1 && !shown[2])
            return columns[1].width + Math.max(0, commandWidth + spacing)
        const w = columns[index].width
        return w > 0 ? w : commandWidth
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 4

        RowLayout {
            Layout.fillWidth: true
            spacing: 10
            visible: filterField.activeFocus || filterField.text.length > 0

            Label { text: "/"; color: Theme.accent; font.bold: true }
            TextField {
                id: filterField
                Layout.fillWidth: true
                placeholderText: qsTr("Filter by name, command, user, or PID")
                text: root.model.filter
                onTextChanged: root.model.filter = text
                color: Theme.colors.bright_foreground
                background: Rectangle {
                    color: "transparent"
                    border.width: 0
                    Rectangle {
                        anchors.bottom: parent.bottom
                        width: parent.width
                        height: 1
                        color: filterField.activeFocus ? Theme.accent : Theme.colors.muted
                    }
                }
                padding: 2
                Keys.onReturnPressed: list.forceActiveFocus()
                Keys.onEnterPressed: list.forceActiveFocus()
                Keys.onEscapePressed: root.clearFilter()
                Keys.onUpPressed: root.move(-1)
                Keys.onDownPressed: root.move(1)
            }
        }

        // Column headers; click one to sort by it, again to flip the order.
        Row {
            Layout.fillWidth: true
            spacing: root.spacing
            Repeater {
                model: root.columns
                delegate: Label {
                    id: header
                    required property var modelData
                    required property int index
                    readonly property bool sorted: modelData.sort === root.model.sortColumn
                    width: root.columnWidth(index)
                    visible: root.shown[index]
                    horizontalAlignment: modelData.right ? Text.AlignRight : Text.AlignLeft
                    text: modelData.title + (sorted ? (root.model.descending ? " ↓" : " ↑") : "")
                    color: sorted ? Theme.accent : Theme.colors.dark_foreground
                    font.bold: true
                    elide: Text.ElideRight

                    MouseArea {
                        anchors.fill: parent
                        enabled: header.modelData.sort >= 0
                        cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                        onClicked: root.model.sortBy(header.modelData.sort)
                    }
                }
            }
        }

        ListView {
            id: list
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: root.model
            boundsBehavior: Flickable.StopAtBounds
            focus: true
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

            delegate: Rectangle {
                id: row
                required property int index
                required property string key
                required property bool isGroup
                required property int depth
                required property bool expanded
                required property int processCount
                required property string kind
                required property int pid
                required property string name
                required property string command
                required property string user
                required property int threads
                required property real cpu
                required property real gpu
                required property real memory
                required property real readRate
                required property real writeRate
                required property bool frozen
                required property real cpuLimit
                readonly property bool current: key === root.selectedKey

                width: ListView.view.width
                height: 22
                radius: Math.min(3, Theme.radius)
                color: current ? Theme.colors.selection : "transparent"

                readonly property color text: current ? Theme.colors.bright_foreground : Theme.colors.foreground
                readonly property color quiet: current ? Theme.colors.light_foreground : Theme.colors.dark_foreground

                Row {
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: root.spacing

                    Label {
                        width: root.columnWidth(0)
                        visible: root.shown[0]
                        horizontalAlignment: Text.AlignRight
                        text: row.isGroup ? "" : row.pid
                        color: row.quiet
                    }
                    // An app's name with its open/closed marker; its
                    // processes sit indented beneath it.
                    Item {
                        width: root.columnWidth(1)
                        height: nameLabel.height
                        visible: root.shown[1]

                        Label {
                            id: marker
                            visible: row.isGroup
                            width: 14
                            text: row.expanded ? "▾" : "▸"
                            color: row.current ? Theme.accent : Theme.colors.dark_foreground

                            MouseArea {
                                anchors.fill: parent
                                anchors.margins: -4
                                cursorShape: Qt.PointingHandCursor
                                onClicked: {
                                    root.selectedKey = row.key
                                    root.toggle(row.key)
                                }
                            }
                        }
                        Label {
                            id: nameLabel
                            x: row.isGroup ? marker.width : row.depth * 14
                            width: parent.width - x
                            text: row.name
                            color: row.frozen ? Theme.colors.yellow
                                 : row.current ? Theme.accent : row.isGroup || row.depth === 0 ? Theme.colors.bright_foreground : Theme.colors.foreground
                            font.bold: row.current || row.isGroup
                            elide: Text.ElideRight
                        }
                    }
                    Label {
                        width: root.columnWidth(2)
                        visible: root.shown[2]
                        text: row.isGroup ? root.groupDetail(row.kind, row.processCount, row.frozen, row.cpuLimit) : row.command
                        color: row.quiet
                        elide: Text.ElideRight
                    }
                    Label { width: root.columnWidth(3); visible: root.shown[3]; text: row.user; color: row.quiet; elide: Text.ElideRight }
                    Label { width: root.columnWidth(4); visible: root.shown[4]; horizontalAlignment: Text.AlignRight; text: row.threads; color: row.quiet }
                    Label {
                        width: root.columnWidth(5)
                        visible: root.shown[5]
                        horizontalAlignment: Text.AlignRight
                        text: root.number(row.cpu, 1)
                        color: row.cpu >= 80 ? Theme.colors.red : row.cpu >= 25 ? Theme.colors.yellow
                             : row.cpu >= 1 ? row.text : row.quiet
                    }
                    Label {
                        width: root.columnWidth(6)
                        visible: root.shown[6]
                        horizontalAlignment: Text.AlignRight
                        text: root.number(row.gpu, 1)
                        color: row.gpu >= 80 ? Theme.colors.red : row.gpu >= 25 ? Theme.colors.yellow
                             : row.gpu >= 1 ? row.text : row.quiet
                    }
                    Label { width: root.columnWidth(7); visible: root.shown[7]; horizontalAlignment: Text.AlignRight; text: Monitor.bytes(row.memory); color: row.text }
                    Label {
                        width: root.columnWidth(8)
                        visible: root.shown[8]
                        horizontalAlignment: Text.AlignRight
                        // A dash, not zero, where the kernel won't tell us.
                        text: row.readRate < 0 ? "–" : row.readRate === 0 ? "0" : Monitor.rate(row.readRate)
                        color: row.readRate > 0 ? Theme.colors.green : row.quiet
                    }
                    Label {
                        width: root.columnWidth(9)
                        visible: root.shown[9]
                        horizontalAlignment: Text.AlignRight
                        text: row.writeRate < 0 ? "–" : row.writeRate === 0 ? "0" : Monitor.rate(row.writeRate)
                        color: row.writeRate > 0 ? Theme.colors.red : row.quiet
                    }
                }

                MouseArea {
                    anchors.fill: parent
                    z: -1
                    onClicked: {
                        root.selectedKey = row.key
                        list.forceActiveFocus()
                    }
                    onDoubleClicked: if (row.isGroup) root.toggle(row.key)
                }
            }

            Label {
                anchors.centerIn: parent
                visible: list.count === 0
                text: root.model.filter.length > 0 ? qsTr("Nothing matches “%1”").arg(root.model.filter) : ""
                color: Theme.colors.dark_foreground
            }
        }
    }
}
