import QtQuick
import QtQuick.Controls
import Montop

// A bordered box with its title set into the top edge, the way btop and the
// Omarchy terminal tools frame their sections. The number is the key that
// shows or hides it.
Item {
    id: panel

    property string title
    property string key
    property string detail
    property color detailColor: Theme.colors.dark_foreground
    default property alias content: body.data

    Rectangle {
        id: frame
        anchors.fill: parent
        anchors.topMargin: 7
        color: "transparent"
        border.width: 1
        border.color: Theme.colors.muted
        radius: Theme.radius
    }

    Rectangle {
        x: 12
        height: titleRow.height
        width: titleRow.width + 12
        color: Theme.colors.background

        Row {
            id: titleRow
            x: 6
            spacing: 6

            Label {
                text: panel.key
                color: Theme.accent
                font.bold: true
                visible: panel.key.length > 0
            }
            Label {
                text: panel.title
                color: Theme.colors.bright_foreground
                font.bold: true
            }
        }
    }

    Rectangle {
        anchors.right: parent.right
        anchors.rightMargin: 12
        height: detailLabel.height
        width: detailLabel.width + 12
        color: Theme.colors.background
        visible: panel.detail.length > 0

        Label {
            id: detailLabel
            x: 6
            // Never over the title: a long detail gives way with an ellipsis.
            width: Math.max(0, Math.min(implicitWidth, panel.width - titleRow.width - 66))
            text: panel.detail
            color: panel.detailColor
            elide: Text.ElideRight
        }
    }

    Item {
        id: body
        anchors.fill: frame
        anchors.margins: 12
        anchors.topMargin: 14
        clip: true
    }
}
