import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Settings > Appearance: how the deck, the mini player and the big view
// look, and how the library window is laid out (LayoutSettings.qml). The library window always stays in the plain Omarchy theme.
// Everything applies as it is chosen.
ScrollView {
    id: page
    objectName: "appearancePage"
    clip: true
    contentWidth: availableWidth

    component Heading: Text {
        Layout.fillWidth: true
        Layout.topMargin: 10
        font.pixelSize: 14
        font.bold: true
        color: theme.c.bright_foreground
    }
    component Note: Text {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        font.pixelSize: 12
        color: theme.c.muted
    }

    ColumnLayout {
        width: parent.width - 40
        x: 20
        spacing: 4

        Heading { text: qsTr("Deck style") }
        Note { text: qsTr("The deck, the mini player and the big view wear the style. The library stays in your Omarchy theme.") }
        Repeater {
            model: [
                ["omarchy", qsTr("Omarchy: follows the desktop theme")],
                ["classic", qsTr("Classic: Winamp 2 grey, with the green readout on black")],
                ["silver", qsTr("Silver: the brushed silver and blue of 2000s MediaMonkey")]
            ]
            RadioButton {
                required property var modelData
                objectName: "deckStyle_" + modelData[0]
                text: modelData[1]
                font.pixelSize: 13
                checked: appearance.deckStyle === modelData[0]
                onClicked: appearance.deckStyle = modelData[0]
            }
        }
        CheckBox {
            objectName: "coverInDeck"
            text: qsTr("Show the cover on the deck")
            font.pixelSize: 13
            checked: appearance.coverInDeck
            onClicked: appearance.coverInDeck = checked
        }

        Heading { text: qsTr("Visualiser") }
        Note { text: qsTr("On the deck, the strip, the mini player and the big view. Click the visualiser, turn the mouse wheel over it, or press Shift+V, to try the next one; its waveform button shows them all.") }
        Flow {
            Layout.fillWidth: true
            spacing: 4
            Repeater {
                model: appearance.visualisers
                RadioButton {
                    required property string modelData
                    objectName: "visualiser_" + modelData
                    text: appearance.visualiserName(modelData)
                    font.pixelSize: 13
                    checked: appearance.visualiser === modelData
                    onClicked: appearance.visualiser = modelData
                }
            }
        }
        // The large bar's visualiser, the idle animation and Reduce motion.
        BarVisualiserSettings { Layout.fillWidth: true }

        Heading { text: qsTr("Big Now Playing view") }
        Note { text: qsTr("Press N for the big view, F11 for fullscreen, Esc to come back.") }
        RadioButton {
            objectName: "bigBackgroundBlur"
            text: qsTr("Background: the cover, blurred")
            font.pixelSize: 13
            checked: appearance.bigBackground === "blur"
            onClicked: appearance.bigBackground = "blur"
        }
        RadioButton {
            objectName: "bigBackgroundTheme"
            text: qsTr("Background: a plain colour from the style")
            font.pixelSize: 13
            checked: appearance.bigBackground === "theme"
            onClicked: appearance.bigBackground = "theme"
        }
        CheckBox {
            objectName: "bigLyrics"
            text: qsTr("Show lyrics in the big view when the track has them")
            font.pixelSize: 13
            checked: appearance.bigLyrics
            onClicked: appearance.bigLyrics = checked
        }
        CheckBox {
            objectName: "bigVisualiserFade"
            text: qsTr("Run the big view's visualiser edge to edge, fading out at the sides and the top")
            font.pixelSize: 13
            checked: appearance.bigVisualiserFade
            onClicked: appearance.bigVisualiserFade = checked
        }

        Heading { text: qsTr("Mini player") }
        Note {
            text: qsTr("Ctrl+M switches to the mini player and back; Ctrl+Shift+M to the micro player. Wayland lets no window place itself or stay on top, so a Hyprland rule for the titles “Omamp mini” and “Omamp micro” floats and pins them. docs/now-playing.md has the rule.")
        }

        // The library window's layout: the Art column, the sidebar, the bar.
        LayoutSettings { Layout.fillWidth: true }
        Item { Layout.preferredHeight: 16 }
    }
}
