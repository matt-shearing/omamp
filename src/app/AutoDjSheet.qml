import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Auto-DJ: where it picks from and how. Saving applies at once; Auto-DJ
// itself is switched with Ctrl+J or the lamp in the deck strip.
ToolSheet {
    id: sheet
    objectName: "autoDjSheet"
    title: qsTr("Auto-DJ")
    subtitle: library.dj.enabled ? qsTr("On, playing from %1").arg(library.dj.sourceLabel) : qsTr("Off")
    preferredWidth: 640
    preferredHeight: 560

    property var choices: []
    property string queryText: ""

    function openSheet() {
        library.activate()
        choices = library.dj.sourceChoices().concat([{ value: "query", label: qsTr("Tracks that match a query") }])
        const s = library.dj.settings
        const source = s.source || "library"
        let index = 0
        queryText = ""
        if (source.startsWith("query:")) {
            index = choices.length - 1
            queryText = source.substring(6)
        } else {
            for (let i = 0; i < choices.length; ++i) if (choices[i].value === source) index = i
        }
        sourceBox.currentIndex = index
        queryField.text = queryText
        upcoming.value = s.upcoming
        separation.value = s.artistSeparation
        recent.value = s.skipPlayedWithinDays
        byRating.checked = s.weightByRating
        boost.value = Math.round(s.neverPlayedBoost * 10)
        open()
        sourceBox.forceActiveFocus()
    }
    function save() {
        const chosen = choices[sourceBox.currentIndex]
        const source = chosen.value === "query" ? "query:" + queryField.text.trim() : chosen.value
        library.dj.saveSettings({ source: source, upcoming: upcoming.value, artistSeparation: separation.value,
                                  skipPlayedWithinDays: recent.value, weightByRating: byRating.checked,
                                  neverPlayedBoost: boost.value / 10 })
        close()
    }
    onAccepted: save()

    GridLayout {
        anchors { left: parent.left; right: parent.right; top: parent.top; margins: 20 }
        columns: 2
        columnSpacing: 16
        rowSpacing: 6

        Text { text: qsTr("Play from"); font.pixelSize: 13; color: theme.c.light_foreground }
        ComboBox {
            id: sourceBox
            objectName: "djSource"
            Layout.fillWidth: true
            implicitHeight: 36; topInset: 0; bottomInset: 0
            font.pixelSize: 13
            model: sheet.choices
            textRole: "label"
        }
        Item { width: 1; visible: queryField.visible }
        TextField {
            id: queryField
            Layout.fillWidth: true
            visible: sourceBox.currentIndex === sheet.choices.length - 1
            placeholderText: qsTr("rating:4.. -genre:christmas")
            font.family: "JetBrainsMono Nerd Font"
            font.pixelSize: 13
        }

        Text { text: qsTr("Keep this many tracks queued"); font.pixelSize: 13; color: theme.c.light_foreground }
        SpinBox { id: upcoming; objectName: "djUpcoming"; from: 1; to: 100; editable: true; font.pixelSize: 13 }

        Text { text: qsTr("Tracks before an artist comes round again"); font.pixelSize: 13; color: theme.c.light_foreground }
        SpinBox { id: separation; from: 0; to: 100; editable: true; font.pixelSize: 13 }

        Text { text: qsTr("Leave out tracks played in the last (days)"); font.pixelSize: 13; color: theme.c.light_foreground }
        SpinBox { id: recent; from: 0; to: 365; editable: true; font.pixelSize: 13 }

        Text { text: qsTr("Favour higher-rated tracks"); font.pixelSize: 13; color: theme.c.light_foreground }
        CheckBox { id: byRating; font.pixelSize: 13 }

        Text { text: qsTr("How much more often never-played tracks come up"); font.pixelSize: 13; color: theme.c.light_foreground }
        RowLayout {
            Slider { id: boost; from: 10; to: 50; stepSize: 1; Layout.fillWidth: true }
            Text {
                Layout.preferredWidth: 44
                text: "×" + (boost.value / 10).toLocaleString(Qt.locale(), 'f', 1)
                font.pixelSize: 13
                color: theme.c.foreground
            }
        }
    }

    footerLeft: [
        Text {
            anchors.verticalCenter: parent.verticalCenter
            width: parent.width
            elide: Text.ElideRight
            text: library.dj.error !== "" ? library.dj.error : qsTr("Audiobooks and podcasts are never picked.")
            font.pixelSize: 12
            color: library.dj.error !== "" ? theme.c.red : theme.c.muted
        }
    ]
    buttons: [
        Button { text: qsTr("Cancel"); flat: true; onClicked: sheet.close() },
        Button {
            text: library.dj.enabled ? qsTr("Turn off") : qsTr("Turn on")
            flat: true
            onClicked: { sheet.save(); library.dj.toggle() }
        },
        PrimaryButton { objectName: "djSave"; text: qsTr("Save"); onClicked: sheet.save() }
    ]
}
