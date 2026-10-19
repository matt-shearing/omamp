import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Party mode: turn it on with what guests may do and an optional PIN, or,
// while it is on, type the PIN to leave it (or to quit).
ToolSheet {
    id: sheet
    objectName: "partySheet"
    title: library.dj.partyOn ? qsTr("Leave party mode") : qsTr("Party mode")
    preferredWidth: 600
    preferredHeight: library.dj.partyOn ? 280 : 470

    // What to do once unlocked: "leave" or "quit".
    property string then: "leave"
    property bool wrongPin: false

    function openSheet(then) {
        sheet.then = then || "leave"
        wrongPin = false
        pinField.text = ""
        if (!library.dj.partyOn) {
            presetBox.currentIndex = 0
            display.checked = false
        }
        open()
        if (library.dj.partyOn && library.dj.partyHasPin) pinField.forceActiveFocus()
        else if (!library.dj.partyOn) presetBox.forceActiveFocus()
    }
    function run() {
        if (library.dj.partyOn) {
            if (!library.dj.disableParty(pinField.text)) {
                wrongPin = true
                pinField.selectAll()
                return
            }
            close()
            if (then === "quit") Qt.quit()
        } else {
            const presets = ["jukebox", "dj", "display"]
            if (library.dj.enableParty(presets[presetBox.currentIndex], pinField.text, display.checked)) close()
        }
    }
    onAccepted: run()

    ColumnLayout {
        anchors { left: parent.left; right: parent.right; top: parent.top; margins: 20 }
        spacing: 8

        Text {
            Layout.fillWidth: true
            visible: !library.dj.partyOn
            wrapMode: Text.Wrap
            text: qsTr("Guests can use the player without changing your library. Editing tags and playlists, organising files, settings and quitting are locked.")
            font.pixelSize: 13
            color: theme.c.foreground
        }
        RowLayout {
            visible: !library.dj.partyOn
            spacing: 12
            Text { text: qsTr("Guests may"); font.pixelSize: 13; color: theme.c.light_foreground }
            ComboBox {
                id: presetBox
                objectName: "partyPreset"
                Layout.preferredWidth: 380
                implicitHeight: 36; topInset: 0; bottomInset: 0
                font.pixelSize: 13
                model: [qsTr("Browse and add tracks to the queue"),
                        qsTr("Also skip, seek, change the volume and the queue"),
                        qsTr("Only watch what is playing")]
            }
        }
        CheckBox {
            id: display
            objectName: "partyDisplay"
            visible: !library.dj.partyOn
            text: qsTr("Show a big now-playing display over the window")
            font.pixelSize: 13
        }
        Text {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            visible: library.dj.partyOn
            text: library.dj.partyHasPin ? qsTr("Type the PIN to leave party mode.")
                                         : qsTr("No PIN was set, so party mode can be left freely.")
            font.pixelSize: 13
            color: theme.c.foreground
        }
        TextField {
            id: pinField
            objectName: "partyPin"
            Layout.preferredWidth: 220
            visible: !library.dj.partyOn || library.dj.partyHasPin
            placeholderText: library.dj.partyOn ? qsTr("PIN") : qsTr("PIN (optional)")
            echoMode: TextInput.Password
            inputMethodHints: Qt.ImhDigitsOnly
            font.pixelSize: 14
            onTextEdited: sheet.wrongPin = false
            Keys.onReturnPressed: sheet.run()
            Keys.onEnterPressed: sheet.run()
        }
        Text {
            visible: sheet.wrongPin
            text: qsTr("That PIN is wrong.")
            font.pixelSize: 13
            color: theme.c.red
        }
    }

    buttons: [
        Button { text: qsTr("Cancel"); flat: true; onClicked: sheet.close() },
        PrimaryButton {
            objectName: "partyConfirm"
            text: library.dj.partyOn ? (sheet.then === "quit" ? qsTr("Unlock and quit") : qsTr("Leave party mode"))
                                     : qsTr("Start party mode")
            onClicked: sheet.run()
        }
    ]
}
