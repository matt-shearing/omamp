import QtQuick

// A short note near the bottom of the window ("Undone: tag edit of 2
// tracks"), gone after a few seconds or on a click.
Rectangle {
    id: toast
    objectName: "toast"
    property string text: ""
    function show(message) {
        text = message
        visible = true
        hide.restart()
    }

    visible: false
    width: Math.min(label.implicitWidth + 36, parent ? parent.width - 80 : 600)
    height: label.implicitHeight + 18
    radius: Math.max(4, theme.radius)
    color: theme.c.dark_background
    border.width: 1
    border.color: theme.accent
    z: 1000

    Text {
        id: label
        anchors.centerIn: parent
        width: parent.width - 36
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.Wrap
        text: toast.text
        font.pixelSize: 13
        color: theme.c.bright_foreground
    }
    MouseArea { anchors.fill: parent; onClicked: toast.visible = false }
    Timer { id: hide; interval: 4500; onTriggered: toast.visible = false }
}
