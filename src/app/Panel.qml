import QtQuick

// A framed section with Winamp's striped title bar: the label sits between
// two pairs of thin accent lines.
Rectangle {
    id: panel
    property string label: ""
    property bool active: true
    default property alias content: body.data

    color: theme.c.dark_background
    border.color: theme.c.lighter_background
    border.width: 1
    radius: theme.radius

    Item {
        id: strip
        anchors { left: parent.left; right: parent.right; top: parent.top; margins: 6 }
        height: 14

        Text {
            id: caption
            anchors.centerIn: parent
            text: panel.label
            font.family: "JetBrainsMono Nerd Font"
            font.pixelSize: 10
            font.bold: true
            font.letterSpacing: 3
            color: panel.active ? theme.c.foreground : theme.c.muted
        }

        Repeater {
            model: 2
            delegate: Column {
                required property int index
                spacing: 2
                anchors.verticalCenter: parent.verticalCenter
                x: index === 0 ? 0 : caption.x + caption.width + 8
                width: index === 0 ? caption.x - 8 : strip.width - x
                Repeater {
                    model: 2
                    Rectangle {
                        width: parent.width
                        height: 1
                        color: panel.active ? theme.accent : theme.c.muted
                        opacity: 0.55
                    }
                }
            }
        }
    }

    Item {
        id: body
        anchors { left: parent.left; right: parent.right; top: strip.bottom; bottom: parent.bottom; margins: 8; topMargin: 6 }
    }
}
