import QtQuick
import QtQuick.Controls

// The button that opens the visualiser picker (VisualiserPicker.qml): a
// waveform, so it reads as "the visualiser" wherever it sits. It is a little
// brighter than the buttons beside it, takes the accent when hovered, and
// stays lit while its picker is open. Used in the large now playing bar,
// the big Now Playing view's top line, the deck and the strip.
Rectangle {
    id: button
    objectName: "visualiserButton"
    // Which visualiser the picker chooses for: "bar", "big" or "player".
    property string where: "player"
    // The colours of what it sits in.
    property color ink: theme.c.foreground
    property color accent: theme.accent
    // Round in the large bar, square-ish elsewhere.
    property bool round: false
    property int size: 28
    property int glyphSize: Math.round(size * 0.55)
    // Words after the glyph (the big view's top line says the style).
    property string label: ""
    property int fontSize: 12
    // What the picker opens beside (above or below), when not the button.
    property Item edgeItem: null
    readonly property bool active: appearance.pickerAnchor === button
    readonly property bool hot: area.containsMouse || active
    signal clicked()
    onClicked: appearance.choosePicker(button, where)

    implicitWidth: label !== "" ? row.implicitWidth + 20 : size
    implicitHeight: size
    radius: round ? height / 2 : Math.min(4, theme.radius)
    color: active ? Qt.alpha(accent, 0.24) : area.containsMouse ? Qt.alpha(accent, 0.16) : Qt.alpha(ink, 0.07)
    border.width: 1
    border.color: hot ? accent : Qt.alpha(ink, 0.18)

    Row {
        id: row
        anchors.centerIn: parent
        spacing: 6
        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: String.fromCodePoint(0xF147D) // waveform
            font.family: "JetBrainsMono Nerd Font"
            font.pixelSize: button.glyphSize
            color: button.hot ? button.accent : button.ink
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            visible: button.label !== ""
            text: button.label
            font.pixelSize: button.fontSize
            color: button.ink
        }
    }

    MouseArea {
        id: area
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: button.clicked()
    }
    ToolTip.visible: area.containsMouse && !active
    ToolTip.delay: 600
    ToolTip.text: qsTr("Visualiser (Shift+V)")
}
