import QtQuick
import QtQuick.Controls

// A small button in a panel's title bar, like Winamp's windowshade and
// minimise buttons, with a tooltip that names it and its key.
Rectangle {
    id: button
    property int glyph: 0
    // Words beside the glyph, for the buttons that switch views, so they
    // say what they do rather than leave it to an icon.
    property string label: ""
    property string tip: ""
    property var skin: appearance.plain
    signal clicked()

    implicitWidth: label !== "" ? labelRow.implicitWidth + 12 : 20
    implicitHeight: label !== "" ? 18 : 14
    radius: Math.min(2, skin.radius)
    color: area.pressed ? skin.buttonPressed : area.containsMouse ? skin.buttonHover : skin.lamp
    border.width: 1
    border.color: area.containsMouse ? skin.accent : skin.lampBorder

    Row {
        id: labelRow
        anchors.centerIn: parent
        spacing: 4
        Text {
            anchors.verticalCenter: parent.verticalCenter
            visible: button.glyph !== 0
            text: String.fromCodePoint(button.glyph)
            font.family: "JetBrainsMono Nerd Font"
            font.pixelSize: 11
            color: area.containsMouse ? button.skin.buttonGlyphHover : button.skin.label
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            visible: button.label !== ""
            text: button.label
            font.pixelSize: 11
            font.bold: true
            color: area.containsMouse ? button.skin.buttonGlyphHover : button.skin.label
        }
    }

    MouseArea {
        id: area
        anchors.fill: parent
        anchors.margins: -2
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: button.clicked()
    }

    ToolTip.visible: area.containsMouse && tip !== ""
    ToolTip.delay: 600
    ToolTip.text: tip
}
