import QtQuick

// A chunky transport button with a bevel, like the original deck: a light
// edge on top, a dark one underneath, pressed in when held.
Item {
    id: button
    property int glyph: 0
    property string tip: ""
    property bool lit: false
    signal clicked()

    implicitWidth: 38
    implicitHeight: 28

    Rectangle {
        anchors.fill: parent
        radius: Math.min(4, theme.radius)
        color: area.pressed ? theme.c.darker_background
             : area.containsMouse ? theme.c.selection : theme.c.lighter_background
        border.width: 1
        border.color: button.lit ? theme.accent : theme.c.darker_background

        Rectangle {
            visible: !area.pressed
            anchors { left: parent.left; right: parent.right; top: parent.top; margins: 1 }
            height: 1
            color: theme.c.light_foreground
            opacity: 0.25
        }

        Text {
            anchors.centerIn: parent
            anchors.verticalCenterOffset: area.pressed ? 1 : 0
            text: String.fromCodePoint(button.glyph)
            font.family: "JetBrainsMono Nerd Font"
            font.pixelSize: 17
            color: button.lit ? theme.accent : area.containsMouse ? theme.c.bright_foreground : theme.c.foreground
        }
    }

    MouseArea {
        id: area
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: button.clicked()
    }
}
