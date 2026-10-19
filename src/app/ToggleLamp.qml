import QtQuick

// SHUFFLE and REPEAT: a word with a small lamp that glows when it's on.
Item {
    id: lamp
    property string label: ""
    property bool on: false
    property var skin: appearance.plain
    signal toggled()

    implicitWidth: row.implicitWidth + 14
    implicitHeight: 22
    opacity: enabled ? 1 : 0.4

    Rectangle {
        anchors.fill: parent
        radius: lamp.skin.smallRadius
        color: area.containsMouse ? lamp.skin.lampHover : lamp.skin.lamp
        border.width: 1
        border.color: lamp.on ? lamp.skin.lampOn : lamp.skin.lampBorder
    }

    Row {
        id: row
        anchors.centerIn: parent
        spacing: 6
        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            width: 6; height: 6; radius: 3
            color: lamp.on ? lamp.skin.lampOn : lamp.skin.lampBorder
        }
        Text {
            text: lamp.label
            font.family: "JetBrainsMono Nerd Font"
            font.pixelSize: 9
            font.bold: true
            font.letterSpacing: 1.5
            color: lamp.on ? lamp.skin.lampTextOn : lamp.skin.lampText
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
