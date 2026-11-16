pragma ComponentBehavior: Bound

import QtCore
import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import Montop

ApplicationWindow {
    id: win
    width: 1280
    height: 860
    minimumWidth: 760
    minimumHeight: 520
    visible: true
    title: "Montop"
    color: Theme.colors.background

    Material.theme: Theme.dark ? Material.Dark : Material.Light
    Material.accent: Theme.accent
    Material.background: Theme.colors.background
    Material.foreground: Theme.colors.foreground
    Material.roundedScale: Theme.radius

    property bool helpVisible: false
    property alias showCpu: settings.showCpu
    property alias showMemory: settings.showMemory
    property alias showDisks: settings.showDisks
    property alias showNetwork: settings.showNetwork
    property alias showProcesses: settings.showProcesses
    property alias showGpu: settings.showGpu

    // What you set stays set between runs, in ~/.config/montop/settings.conf:
    // the panels shown, the sort, apps or every process, kernel threads, and
    // how often to update. A pause, a filter, or a rewind is for the moment.
    Settings {
        id: settings
        location: StandardPaths.writableLocation(StandardPaths.ConfigLocation) + "/montop/settings.conf"
        category: "window"
        property bool showCpu: true
        property bool showMemory: true
        property bool showDisks: true
        property bool showNetwork: true
        property bool showProcesses: true
        property bool showGpu: true
        property int sortColumn: ProcessModel.Cpu
        property bool descending: true
        property bool grouped: true
        property bool kernelThreads: false
        property int interval: 250
    }
    Component.onCompleted: {
        const model = Monitor.processes
        // Sorting by GPU% means nothing where no GPU reports it per app.
        if (settings.sortColumn !== ProcessModel.Gpu || Monitor.gpuPerApp)
            model.sortColumn = settings.sortColumn
        model.descending = settings.descending
        model.grouped = settings.grouped
        model.showKernelThreads = settings.kernelThreads
        Monitor.interval = settings.interval
    }
    Connections {
        target: Monitor.processes
        function onSortChanged() {
            settings.sortColumn = Monitor.processes.sortColumn
            settings.descending = Monitor.processes.descending
        }
        function onGroupedChanged() { settings.grouped = Monitor.processes.grouped }
        function onFilterChanged() { settings.kernelThreads = Monitor.processes.showKernelThreads }
    }
    Connections {
        target: Monitor
        function onIntervalChanged() { settings.interval = Monitor.interval }
    }
    readonly property bool gpuShown: showGpu && Monitor.gpus.length > 0
    // Single-letter shortcuts stand down while the filter has the keyboard
    // or a question is open, so typing doesn't kill processes.
    readonly property bool typing: processes.filtering || confirm.visible || limiter.visible
    readonly property bool leftColumn: showMemory || showDisks || showNetwork

    // The frame clock every graph shares: 0 when a sample lands, 1 when the
    // next is due. Advanced once per frame while there's motion to show and
    // the window is visible; idle otherwise, so a paused or hidden window
    // costs nothing.
    property real phase: 1
    property double sampledAt: 0
    Connections {
        target: Monitor
        function onSampled() {
            win.sampledAt = Date.now()
            win.phase = 0
        }
        function onPausedChanged() {
            if (Monitor.paused)
                win.phase = 1
        }
    }
    // Graphs move 2 pixels a sample, so the clock ticks about as often as
    // they cross a device pixel, and each tick that moves nothing draws
    // nothing. A timer rather than a FrameAnimation: a running animation
    // keeps Qt redrawing at the display's full rate even when nothing changed.
    Timer {
        interval: Math.max(16, Math.floor(Monitor.interval / (2 * Screen.devicePixelRatio) / 2))
        repeat: true
        running: win.phase < 1 && !Monitor.paused && win.visibility !== Window.Minimized
        onTriggered: win.phase = Math.min(1, (Date.now() - win.sampledAt) / Monitor.interval)
    }

    property string message
    property bool messageIsError: false
    function say(text, error) {
        message = text
        messageIsError = !!error
        messageTimer.restart()
    }
    Timer { id: messageTimer; interval: 4000; onTriggered: win.message = "" }

    // Times on the graphs and while rewound, in the language's own format.
    function clock(time) {
        return new Date(time).toLocaleTimeString(Translator.locale, "HH:mm:ss")
    }
    function ago(time) {
        const seconds = Math.max(0, Math.round((Date.now() - time) / 1000))
        if (seconds < 60)
            return qsTr("%1 s ago").arg(seconds)
        return qsTr("%1 min %2 s ago").arg(Math.floor(seconds / 60)).arg(seconds % 60)
    }
    // A list from the past can't be acted on: its PIDs may belong to other
    // processes by now.
    function live() {
        if (Monitor.rewoundTo === 0)
            return true
        win.say(qsTr("This is how things were at %1. Press Esc to return to now first.").arg(clock(Monitor.rewoundTo)), true)
        return false
    }

    // Pausing and limiting act on the whole app, also from a process row.
    function selectedApp() {
        if (!live())
            return null
        const s = processes.selected
        const app = s ? Monitor.app(s.groupKey) : null
        if (s && !app.key)
            win.say(qsTr("%1 has no cgroup of its own, so it can't be paused or limited.").arg(s.name), true)
        return app && app.key ? app : null
    }
    function togglePause() {
        const app = selectedApp()
        if (!app)
            return
        const error = Monitor.setAppPaused(app.key, !app.frozen)
        if (error.length > 0)
            win.say(error, true)
        else
            win.say(app.frozen ? qsTr("Resumed %1").arg(app.name)
                               : qsTr("Paused %1. Press p to resume it; it also resumes when Montop closes.").arg(app.name))
    }
    function askToLimit() {
        const app = selectedApp()
        if (!app)
            return
        if (app.control.length > 0) {
            // Same sentence the monitor gives, without a pointless dialog.
            win.say(Monitor.setAppCpuLimit(app.key, app.cpuLimit), true)
            return
        }
        limiter.key = app.key
        limiter.name = app.name
        limiter.current = app.cpuLimit
        limiter.open()
    }

    function askToEnd(force) {
        const p = processes.selected
        if (!p || !live())
            return
        confirm.key = p.groupKey
        confirm.pid = p.pid
        confirm.name = p.name
        confirm.pids = p.isGroup ? p.pids : []
        confirm.kind = p.isGroup ? p.kind : ""
        confirm.force = force
        confirm.open()
    }

    function duration(seconds) {
        const d = Math.floor(seconds / 86400), h = Math.floor(seconds % 86400 / 3600), m = Math.floor(seconds % 3600 / 60)
        if (d > 0)
            return qsTr("%1d %2h").arg(d).arg(h)
        if (h > 0)
            return qsTr("%1h %2m").arg(h).arg(m)
        return qsTr("%1m").arg(m)
    }

    Shortcut { sequence: "?"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: win.helpVisible = !win.helpVisible }
    Shortcut { sequences: ["Q", "Ctrl+Q"]; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: Qt.quit() }
    Shortcut { sequence: "Space"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: Monitor.paused = !Monitor.paused }
    Shortcut {
        sequence: "Escape"
        context: Qt.ApplicationShortcut
        enabled: !win.typing
        onActivated: {
            if (win.helpVisible)
                win.helpVisible = false
            else if (Monitor.rewoundTo > 0)
                Monitor.rewindTo(0)
            else if (Monitor.processes.filter.length > 0)
                processes.clearFilter()
        }
    }
    Shortcut { sequence: "/"; context: Qt.ApplicationShortcut; enabled: !win.typing && win.showProcesses; onActivated: processes.startFilter() }
    Shortcut { sequences: ["Down", "J"]; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: processes.move(1) }
    Shortcut { sequences: ["Up", "K"]; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: processes.move(-1) }
    Shortcut { sequence: "PgDown"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: processes.page(1) }
    Shortcut { sequence: "PgUp"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: processes.page(-1) }
    Shortcut { sequences: ["Home", "G"]; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: processes.select(0) }
    Shortcut { sequences: ["End", "Shift+G"]; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: processes.select(Monitor.processes.count - 1) }
    Shortcut { sequence: "S"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: Monitor.processes.cycleSort() }
    Shortcut { sequence: "R"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: Monitor.processes.descending = !Monitor.processes.descending }
    Shortcut {
        sequence: "Shift+K"
        context: Qt.ApplicationShortcut
        enabled: !win.typing
        onActivated: {
            Monitor.processes.showKernelThreads = !Monitor.processes.showKernelThreads
            win.say(Monitor.processes.showKernelThreads ? qsTr("Showing kernel threads") : qsTr("Hiding kernel threads"))
        }
    }
    Shortcut { sequence: "A"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: processes.toggleGrouped() }
    Shortcut { sequences: ["Right", "L", "Return", "Enter"]; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: processes.expand() }
    Shortcut { sequences: ["Left", "H"]; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: processes.collapse() }
    Shortcut { sequence: "["; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: Monitor.rewindBy(-10) }
    Shortcut { sequence: "]"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: Monitor.rewindBy(10) }
    Shortcut { sequence: "{"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: Monitor.rewindBy(-60) }
    Shortcut { sequence: "}"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: Monitor.rewindBy(60) }
    Shortcut { sequence: "P"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: win.togglePause() }
    Shortcut { sequence: "C"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: win.askToLimit() }
    Shortcut { sequence: "X"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: win.askToEnd(false) }
    Shortcut { sequence: "Shift+X"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: win.askToEnd(true) }
    Shortcut {
        sequences: ["+", "="]
        context: Qt.ApplicationShortcut
        enabled: !win.typing
        onActivated: { Monitor.slower(); win.say(qsTr("Updating every %1 s").arg((Monitor.interval / 1000).toLocaleString(Translator.locale, 'f', 3).replace(/[.,]?0+$/, ""))) }
    }
    Shortcut {
        sequence: "-"
        context: Qt.ApplicationShortcut
        enabled: !win.typing
        onActivated: { Monitor.faster(); win.say(qsTr("Updating every %1 s").arg((Monitor.interval / 1000).toLocaleString(Translator.locale, 'f', 3).replace(/[.,]?0+$/, ""))) }
    }
    Shortcut { sequence: "1"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: win.showCpu = !win.showCpu }
    Shortcut { sequence: "2"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: win.showMemory = !win.showMemory }
    Shortcut { sequence: "3"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: win.showDisks = !win.showDisks }
    Shortcut { sequence: "4"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: win.showNetwork = !win.showNetwork }
    Shortcut { sequence: "5"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: win.showProcesses = !win.showProcesses }
    Shortcut { sequence: "6"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: win.showGpu = !win.showGpu }

    // Right-to-left languages mirror the window; the Translator sets the
    // application's layout direction.
    Item {
        anchors.fill: parent
        LayoutMirroring.enabled: Application.layoutDirection === Qt.RightToLeft
        LayoutMirroring.childrenInherit: true

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 12
            spacing: 8

            // Who and how long, in one quiet line.
            RowLayout {
                Layout.fillWidth: true
                spacing: 16
                Label { text: Monitor.hostname; color: Theme.accent; font.bold: true }
                Label { text: Monitor.kernel; color: Theme.colors.dark_foreground }
                Label {
                    text: { return qsTr("up %1").arg(win.duration(Monitor.uptime)) }
                    color: Theme.colors.dark_foreground
                }
                Item { Layout.fillWidth: true }
                Label {
                    visible: Monitor.rewoundTo > 0
                    text: { win.sampledAt; return qsTr("Showing %1, %2  ·  Esc returns to now").arg(win.clock(Monitor.rewoundTo)).arg(win.ago(Monitor.rewoundTo)) } // qmllint disable confusing-expression-statement
                    color: Theme.accent
                    font.bold: true
                }
                Label {
                    visible: Monitor.paused
                    text: qsTr("Updates paused")
                    color: Theme.colors.yellow
                    font.bold: true
                }
                Label {
                    text: { return qsTr("every %1 s").arg((Monitor.interval / 1000).toLocaleString(Translator.locale, 'f', 3).replace(/[.,]?0+$/, "")) }
                    color: Theme.colors.dark_foreground
                }
            }

            Label {
                Layout.fillWidth: true
                visible: Monitor.error.length > 0
                text: Monitor.error
                color: Theme.colors.red
                wrapMode: Text.Wrap
            }

            // The two processors side by side: the CPU, and the GPU where
            // the machine has one.
            RowLayout {
                visible: win.showCpu || win.gpuShown
                Layout.fillWidth: true
                Layout.preferredHeight: Math.max(150, win.height * 0.26)
                Layout.fillHeight: !win.leftColumn && !win.showProcesses
                spacing: 10

                CpuPanel {
                    visible: win.showCpu
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                }
                GpuPanel {
                    visible: win.gpuShown
                    Layout.fillWidth: !win.showCpu
                    Layout.fillHeight: true
                    Layout.preferredWidth: Math.max(330, win.width * 0.3)
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 10
                visible: win.leftColumn || win.showProcesses

                ColumnLayout {
                    visible: win.leftColumn
                    Layout.fillHeight: true
                    Layout.fillWidth: !win.showProcesses
                    Layout.preferredWidth: Math.max(340, win.width * 0.34)
                    spacing: 8

                    MemoryPanel {
                        visible: win.showMemory
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        Layout.minimumHeight: 150
                    }
                    DiskPanel {
                        visible: win.showDisks
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        Layout.minimumHeight: 150
                    }
                    NetworkPanel {
                        visible: win.showNetwork
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        Layout.minimumHeight: 120
                    }
                }

                ProcessPanel {
                    id: processes
                    visible: win.showProcesses
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                }
            }

            // The selected process's full command line, or what just happened.
            RowLayout {
                Layout.fillWidth: true
                spacing: 16

                Label {
                    Layout.fillWidth: true
                    text: win.message.length > 0 ? win.message
                        : !processes.selected ? ""
                        : [processes.selected.isGroup ? processes.selected.name : processes.selected.pid + ""]
                            .concat(processes.selected.frozen ? [qsTr("paused")] : [])
                            .concat(processes.selected.cpuLimit > 0 ? [qsTr("limited to %1%").arg(Math.round(processes.selected.cpuLimit))] : [])
                            .concat([processes.selected.command]).join("  ·  ")
                    color: win.message.length > 0 ? (win.messageIsError ? Theme.colors.red : Theme.accent) : Theme.colors.dark_foreground
                    elide: Text.ElideRight
                }
                Label {
                    text: qsTr("? shortcuts")
                    color: Theme.colors.dark_foreground
                }
            }
        }

        HelpOverlay {
            anchors.fill: parent
            visible: win.helpVisible
            onClosed: win.helpVisible = false
        }
    }

    Popup {
        id: confirm

        property int pid
        property string key
        property string name
        property bool force
        // Every process of an app, when the selection is an app.
        property var pids: []
        property string kind
        readonly property bool group: pids.length > 0
        // Services and the session are not apps people opened; ending one can
        // take the desktop with it (the compositor is a service, too).
        readonly property bool critical: group && (kind === "service" || kind === "system" || kind === "session")

        anchors.centerIn: parent
        modal: true
        focus: true
        padding: 20
        closePolicy: Popup.CloseOnEscape
        // Material's own dimmer greys a dark window out; darken it instead.
        Overlay.modal: Rectangle { color: Qt.rgba(0, 0, 0, Theme.dark ? 0.55 : 0.3) }
        onOpened: confirmButton.forceActiveFocus()

        background: Rectangle {
            color: Theme.colors.lighter_background
            border.width: 1
            border.color: confirm.force ? Theme.colors.red : Theme.accent
            radius: Theme.radius
        }

        function accept() {
            // A frozen process can't act on SIGTERM; let it go first.
            if (group && Monitor.app(key).frozen)
                Monitor.setAppPaused(key, false)
            if (group) {
                const result = Monitor.signalProcesses(pids, force)
                close()
                if (result.failed === 0)
                    win.say(force ? qsTr("Killed %1").arg(name) : qsTr("Asked %1 to end").arg(name))
                else if (result.error === 1)
                    win.say(qsTr("Not allowed to signal %n of %1's processes: they belong to another user", "", result.failed).arg(name), true)
                else
                    win.say(qsTr("Couldn't signal %n of %1's processes, error %2", "", result.failed).arg(name).arg(result.error), true)
                return
            }
            const error = Monitor.signalProcess(pid, force)
            close()
            if (error === 0)
                win.say(force ? qsTr("Killed %1 (%2)").arg(name).arg(pid) : qsTr("Asked %1 (%2) to end").arg(name).arg(pid))
            else if (error === 1)
                win.say(qsTr("Not allowed to signal %1 (%2): it belongs to another user").arg(name).arg(pid), true)
            else if (error === 3)
                win.say(qsTr("%1 (%2) has already exited").arg(name).arg(pid), true)
            else
                win.say(qsTr("Couldn't signal %1 (%2), error %3").arg(name).arg(pid).arg(error), true)
        }

        contentItem: ColumnLayout {
            spacing: 14
            Keys.onReturnPressed: confirm.accept()
            Keys.onEnterPressed: confirm.accept()

            Label {
                text: confirm.group
                    ? (confirm.force ? qsTr("Kill %1 and its %n processes?", "", confirm.pids.length).arg(confirm.name)
                                     : qsTr("End %1 and its %n processes?", "", confirm.pids.length).arg(confirm.name))
                    : (confirm.force ? qsTr("Kill %1 (%2)?").arg(confirm.name).arg(confirm.pid)
                                     : qsTr("End %1 (%2)?").arg(confirm.name).arg(confirm.pid))
                color: Theme.colors.bright_foreground
                font.bold: true
                font.pixelSize: 15
            }
            Label {
                Layout.maximumWidth: 420
                wrapMode: Text.Wrap
                text: confirm.force
                    ? qsTr("SIGKILL stops it immediately. Unsaved work in it is lost.")
                    : qsTr("SIGTERM asks it to close, so it can save and clean up first.")
                color: Theme.colors.foreground
            }
            Label {
                visible: confirm.critical
                Layout.maximumWidth: 420
                wrapMode: Text.Wrap
                text: qsTr("%1 is part of your system, not an app you opened. Ending it can close your session or break what depends on it.").arg(confirm.name)
                color: Theme.colors.red
            }
            RowLayout {
                Layout.alignment: Qt.AlignRight
                spacing: 8
                Button {
                    text: qsTr("Cancel")
                    flat: true
                    onClicked: confirm.close()
                }
                Button {
                    id: confirmButton
                    text: confirm.force ? qsTr("Kill") : qsTr("End")
                    highlighted: true
                    Material.accent: confirm.force ? Theme.colors.red : Theme.accent
                    onClicked: confirm.accept()
                    Keys.onReturnPressed: confirm.accept()
                    Keys.onEnterPressed: confirm.accept()
                }
            }
        }
    }

    // Caps an app's CPU through systemd, in the CPU column's own unit:
    // percent of one core.
    Popup {
        id: limiter

        property string key
        property string name
        property real current
        readonly property var choices: {
            const list = [0, 25, 50, 100]
            for (let n = 200; n < Monitor.coreCount * 100; n *= 2)
                list.push(n)
            return list
        }
        function describe(percent) {
            if (percent === 25)
                return qsTr("a quarter of a core")
            if (percent === 50)
                return qsTr("half a core")
            return percent >= 100 ? qsTr("%n cores", "", percent / 100) : ""
        }
        function apply(percent) {
            const error = Monitor.setAppCpuLimit(key, percent)
            close()
            if (error.length > 0)
                win.say(error, true)
            else if (percent === 0)
                win.say(qsTr("Lifted the CPU limit on %1").arg(name))
            else
                win.say(qsTr("Limited %1 to %2% CPU. It lifts when Montop closes.").arg(name).arg(percent))
        }

        anchors.centerIn: parent
        modal: true
        focus: true
        padding: 20
        closePolicy: Popup.CloseOnEscape
        Overlay.modal: Rectangle { color: Qt.rgba(0, 0, 0, Theme.dark ? 0.55 : 0.3) }
        onOpened: {
            const at = choices.indexOf(Math.round(current))
            choiceList.currentIndex = at >= 0 ? at : 0
            choiceList.forceActiveFocus()
        }

        background: Rectangle {
            color: Theme.colors.lighter_background
            border.width: 1
            border.color: Theme.accent
            radius: Theme.radius
        }

        contentItem: ColumnLayout {
            spacing: 12

            Label {
                text: qsTr("Limit %1's CPU").arg(limiter.name)
                color: Theme.colors.bright_foreground
                font.bold: true
                font.pixelSize: 15
            }
            Label {
                Layout.maximumWidth: 380
                wrapMode: Text.Wrap
                text: qsTr("It keeps running, just slower. Percent of one core, as in the CPU column.")
                color: Theme.colors.foreground
            }
            ListView {
                id: choiceList
                Layout.fillWidth: true
                Layout.preferredWidth: 340
                Layout.preferredHeight: count * 26
                interactive: false
                model: limiter.choices
                keyNavigationEnabled: true
                Keys.onReturnPressed: limiter.apply(limiter.choices[currentIndex])
                Keys.onEnterPressed: limiter.apply(limiter.choices[currentIndex])
                Keys.onPressed: event => {
                    if (event.key === Qt.Key_J) { incrementCurrentIndex(); event.accepted = true }
                    else if (event.key === Qt.Key_K) { decrementCurrentIndex(); event.accepted = true }
                }
                delegate: Rectangle {
                    id: choice
                    required property int index
                    required property int modelData
                    readonly property bool current: ListView.isCurrentItem
                    width: ListView.view.width
                    height: 26
                    radius: Math.min(3, Theme.radius)
                    color: current ? Theme.colors.selection : "transparent"

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 8
                        anchors.rightMargin: 8
                        spacing: 12
                        Label {
                            text: choice.modelData === 0 ? qsTr("No limit") : qsTr("Up to %1%").arg(choice.modelData)
                            color: choice.current ? Theme.accent : Theme.colors.bright_foreground
                            font.bold: choice.current
                        }
                        Label {
                            Layout.fillWidth: true
                            text: limiter.describe(choice.modelData)
                            color: Theme.colors.dark_foreground
                            elide: Text.ElideRight
                        }
                        Label {
                            visible: Math.round(limiter.current) === choice.modelData
                            text: qsTr("now")
                            color: Theme.accent
                        }
                    }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: limiter.apply(choice.modelData)
                    }
                }
            }
        }
    }
}
