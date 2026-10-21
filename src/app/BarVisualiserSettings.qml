import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Settings > Appearance, under Visualiser: the large now playing bar's
// visualiser (where it sits and its style), what the visualisers do while
// no sound reaches Thrum, and Reduce motion. Everything applies as it is
// chosen and is kept in window.conf under [appearance].
ColumnLayout {
    id: page
    objectName: "barVisualiserSettings"
    spacing: 4

    component Sub: Text {
        Layout.fillWidth: true
        Layout.topMargin: 6
        font.pixelSize: 13
        font.bold: true
        color: theme.c.foreground
    }
    component Note: Text {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        font.pixelSize: 12
        color: theme.c.muted
    }

    Sub { text: qsTr("In the large now playing bar") }
    Note { text: qsTr("Shown when the library uses the large bar (Layout, below). Click it, or press Shift+V in the library, to try its next style.") }
    Flow {
        Layout.fillWidth: true
        spacing: 4
        Repeater {
            model: appearance.barPlacements
            RadioButton {
                required property string modelData
                objectName: "barVisualiser_" + modelData
                text: appearance.barPlacementName(modelData)
                font.pixelSize: 13
                checked: appearance.barVisualiser === modelData
                onClicked: appearance.barVisualiser = modelData
            }
        }
    }
    RowLayout {
        spacing: 8
        enabled: appearance.barVisualiser !== "off"
        Text { text: qsTr("Style in the bar:"); font.pixelSize: 13; color: theme.c.foreground }
        ComboBox {
            objectName: "barVisualiserStyle"
            Layout.preferredWidth: 240
            implicitHeight: 34; topInset: 0; bottomInset: 0
            font.pixelSize: 13
            // "" first: the same style as the deck and the strip.
            readonly property var styles: [""].concat(appearance.visualisers)
            model: [qsTr("Same as the player")].concat(appearance.visualisers.map(s => appearance.visualiserName(s)))
            currentIndex: Math.max(0, styles.indexOf(appearance.barVisualiserStyle))
            onActivated: (i) => appearance.barVisualiserStyle = styles[i]
        }
    }

    Sub { text: qsTr("Sensitivity") }
    Note { text: qsTr("How readily every visualiser fills. Thrum follows the music's loudness, so loud and quiet recordings both move; High makes the bars taller, Low calmer.") }
    Flow {
        Layout.fillWidth: true
        spacing: 4
        Repeater {
            model: appearance.sensitivities
            RadioButton {
                required property string modelData
                objectName: "visualiserSensitivity_" + modelData
                text: appearance.sensitivityName(modelData)
                font.pixelSize: 13
                checked: appearance.visualiserSensitivity === modelData
                onClicked: appearance.visualiserSensitivity = modelData
            }
        }
    }

    Sub { text: qsTr("While nothing plays through Thrum") }
    Note { text: qsTr("Paused, stopped, or playing on Spotify, whose sound never reaches Thrum's analyser.") }
    RadioButton {
        objectName: "idleVisualiserCalm"
        text: qsTr("Keep a slow, calm animation")
        font.pixelSize: 13
        checked: appearance.idleVisualiser === "calm"
        onClicked: appearance.idleVisualiser = "calm"
    }
    RadioButton {
        objectName: "idleVisualiserHide"
        text: qsTr("Hide the large bar's visualiser")
        font.pixelSize: 13
        checked: appearance.idleVisualiser === "hide"
        onClicked: appearance.idleVisualiser = "hide"
    }
    CheckBox {
        objectName: "reduceMotion"
        text: qsTr("Reduce motion: slower visualisers, no beat flashes, and a still picture while idle")
        font.pixelSize: 13
        checked: appearance.reduceMotion
        onClicked: appearance.reduceMotion = checked
    }
}
