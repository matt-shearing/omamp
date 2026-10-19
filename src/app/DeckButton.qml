import QtQuick

// A chunky transport button with a bevel, like the original deck: a light
// edge on top, a dark one underneath, pressed in when held. Its colours
// come from a deck style (appearance.deck); the default is the plain one.
Item {
    id: button
    property int glyph: 0
    property string tip: ""
    property bool lit: false
    property var skin: appearance.plain
    signal clicked()

    implicitWidth: 38
    implicitHeight: 28
    opacity: enabled ? 1 : 0.4

    Rectangle {
        id: face
        anchors.fill: parent
        radius: button.skin.smallRadius
        color: area.pressed ? button.skin.buttonPressed
             : area.containsMouse ? button.skin.buttonHover : button.skin.buttonTop
        gradient: button.skin.gradient && !area.pressed ? shine : null
        border.width: 1
        border.color: button.lit ? button.skin.buttonLit : button.skin.buttonBorder

        Gradient {
            id: shine
            GradientStop { position: 0.0; color: area.containsMouse ? Qt.lighter(button.skin.buttonTop, 1.04) : button.skin.buttonTop }
            GradientStop { position: 1.0; color: area.containsMouse ? Qt.lighter(button.skin.buttonBottom, 1.06) : button.skin.buttonBottom }
        }

        // The highlight along the top edge.
        Rectangle {
            visible: !area.pressed && !button.skin.bevel
            anchors { left: parent.left; right: parent.right; top: parent.top; margins: 1 }
            height: 1
            color: button.skin.buttonShine
        }
        Bevel {
            anchors.fill: parent
            anchors.margins: 1
            skin: button.skin
            visible: button.skin.bevel
            sunken: area.pressed
        }

        Text {
            anchors.centerIn: parent
            anchors.verticalCenterOffset: area.pressed ? 1 : 0
            text: String.fromCodePoint(button.glyph)
            font.family: "JetBrainsMono Nerd Font"
            font.pixelSize: Math.min(17, button.height - 8)
            color: button.lit ? button.skin.buttonLit
                 : area.containsMouse ? button.skin.buttonGlyphHover : button.skin.buttonGlyph
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
