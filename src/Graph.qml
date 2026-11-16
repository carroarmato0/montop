import QtQuick
import QtQuick.Controls
import Montop

// A Sparkline reading from the monitor and wired to the window's frame
// clock, so every graph scrolls in step with the others.
//
// Pointing at it says when that was and which apps were behind it, and a
// click rewinds the process list to that moment. While rewound, a line marks
// the moment on every graph.
Sparkline {
    id: graph

    // What to name the apps by: "cpu", "memory", "io", or "" for nothing
    // (no app's network traffic is visible without root).
    property string blame: ""

    readonly property var win: ApplicationWindow.window

    source: Monitor
    step: 2
    phase: win ? win.phase : 1
    gridColor: Theme.colors.selection

    // Sample k back from the newest sits at x = width + step - (k + phase) * step.
    function samplesAt(x) { return Math.max(0, Math.round((width + step - x) / step - phase)) }
    function xFor(samplesBack) { return width + step - (samplesBack + phase) * step }

    function describe(x) {
        const time = Monitor.sampleTime(samplesAt(x))
        if (!time)
            return ""
        // Named by the app scan that covers it, the moment a click goes to.
        const moment = Monitor.momentTime(time)
        const lines = ["<b>" + win.clock(moment || time) + "</b>  " + win.ago(moment || time)]
        if (blame.length > 0) {
            for (const entry of Monitor.blame(time, blame, 3)) {
                const value = blame === "cpu" ? entry.value.toLocaleString(Translator.locale, 'f', 0) + "%"
                            : blame === "memory" ? Monitor.bytes(entry.value)
                            : Monitor.rate(entry.value)
                lines.push(entry.name + "  " + value)
            }
        }
        lines.push("<i>" + qsTr("Click to see the apps as they were") + "</i>")
        return lines.join("<br>")
    }

    Rectangle {
        // Re-read on every sample, as the moment slides left.
        readonly property int back: { graph.win ? graph.win.sampledAt : 0; return Monitor.samplesSince(Monitor.rewoundTo) } // qmllint disable confusing-expression-statement
        x: Math.round(graph.xFor(back))
        width: 1
        height: graph.height
        visible: Monitor.rewoundTo > 0 && x >= 0 && x <= graph.width
        color: Theme.accent
    }

    Rectangle {
        x: Math.round(pointer.mouseX)
        width: 1
        height: graph.height
        visible: pointer.containsMouse
        color: Theme.colors.dark_foreground
        opacity: 0.6
    }

    MouseArea {
        id: pointer
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.CrossCursor
        onClicked: mouse => Monitor.rewindTo(Monitor.sampleTime(graph.samplesAt(mouse.x)))
    }

    ToolTip {
        id: tip
        visible: pointer.containsMouse && text.length > 0
        // Follows the pointer, and re-reads as samples arrive under it.
        text: { graph.win ? graph.win.sampledAt : 0; return pointer.containsMouse ? graph.describe(pointer.mouseX) : "" } // qmllint disable confusing-expression-statement
        x: Math.min(pointer.mouseX + 14, graph.width - width)
        y: pointer.mouseY + 18
        delay: 0
        padding: 8
        contentItem: Label {
            text: tip.text
            textFormat: Text.StyledText
            color: Theme.colors.foreground
            lineHeight: 1.15
        }
        background: Rectangle {
            color: Theme.colors.lighter_background
            border.width: 1
            border.color: Theme.colors.muted
            radius: Math.min(4, Theme.radius)
        }
        enter: null
        exit: null
    }
}
