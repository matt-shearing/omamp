import QtQuick

// A framed section with Winamp's striped title bar: the label sits between
// two pairs of thin lines. Its colours come from a deck style
// (appearance.deck); the default is the plain Omarchy one.
Rectangle {
    id: panel
    property string label: ""
    property bool active: true
    property var skin: appearance.plain
    default property alias content: body.data
    // Small buttons at the right end of the title bar (TitleButton).
    property alias tools: toolRow.data

    color: skin.panel
    gradient: skin.gradient ? fill : null
    border.color: skin.panelBorder
    border.width: 1
    radius: skin.radius

    Gradient {
        id: fill
        GradientStop { position: 0.0; color: panel.skin.panelTop }
        GradientStop { position: 1.0; color: panel.skin.panelBottom }
    }

    // A raised edge: light along the top and left, dark along the bottom
    // and right, for the styles that have bevels.
    Bevel { anchors.fill: parent; anchors.margins: 1; skin: panel.skin; visible: panel.skin.bevel }

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
            color: panel.active ? panel.skin.label : panel.skin.labelDim
        }

        Repeater {
            model: 2
            delegate: Column {
                required property int index
                spacing: 2
                anchors.verticalCenter: parent.verticalCenter
                x: index === 0 ? 0 : caption.x + caption.width + 8
                width: index === 0 ? caption.x - 8
                                   : strip.width - x - (toolRow.width > 0 ? toolRow.width + 6 : 0)
                Repeater {
                    model: 2
                    Rectangle {
                        width: parent.width
                        height: 1
                        color: panel.active ? panel.skin.stripe : Qt.alpha(panel.skin.labelDim, 0.55)
                    }
                }
            }
        }

        Row {
            id: toolRow
            anchors { right: parent.right; verticalCenter: parent.verticalCenter }
            spacing: 3
        }
    }

    Item {
        id: body
        anchors { left: parent.left; right: parent.right; top: strip.bottom; bottom: parent.bottom; margins: 8; topMargin: 6 }
    }
}
