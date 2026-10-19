import QtQuick

// SHUFFLE and REPEAT: a word with a small lamp that glows when it's on.
Item {
    id: lamp
    property string label: ""
    property bool on: false
    signal toggled()

    implicitWidth: row.implicitWidth + 14
    implicitHeight: 22

    Rectangle {
        anchors.fill: parent
        radius: Math.min(4, theme.radius)
        color: area.containsMouse ? theme.c.selection : theme.c.darker_background
        border.width: 1
        border.color: lamp.on ? theme.accent : theme.c.lighter_background
    }

    Row {
        id: row
        anchors.centerIn: parent
        spacing: 6
        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            width: 6; height: 6; radius: 3
            color: lamp.on ? theme.accent : theme.c.lighter_background
        }
        Text {
            text: lamp.label
            font.family: "JetBrainsMono Nerd Font"
            font.pixelSize: 9
            font.bold: true
            font.letterSpacing: 1.5
            color: lamp.on ? theme.c.bright_foreground : theme.c.muted
        }
    }

    MouseArea {
        id: area
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: lamp.toggled()
    }
}
