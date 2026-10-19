import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Asks for one name: a new playlist or folder, a rename, Save Now Playing
// as a playlist. Enter (or Ctrl+Enter) saves, Esc cancels. ask() takes the
// function that gets the name; it returns true when the name was taken.
ToolSheet {
    id: sheet
    objectName: "nameSheet"
    preferredWidth: 520
    preferredHeight: 230
    property string label: qsTr("Name")
    property string confirmText: qsTr("Save")
    property var action: null

    function ask(title, label, initial, confirmText, action) {
        sheet.title = title
        sheet.label = label
        sheet.confirmText = confirmText
        sheet.action = action
        field.text = initial
        open()
        field.forceActiveFocus()
        field.selectAll()
    }
    function run() {
        const name = field.text.trim()
        if (name === "" || !action) return
        if (action(name) !== false) close()
    }
    onAccepted: run()

    TextField {
        id: field
        objectName: "nameField"
        anchors { left: parent.left; right: parent.right; top: parent.top; margins: 20 }
        placeholderText: sheet.label
        font.pixelSize: 14
        Keys.onReturnPressed: sheet.run()
        Keys.onEnterPressed: sheet.run()
    }

    buttons: [
        Button { text: qsTr("Cancel"); flat: true; onClicked: sheet.close() },
        PrimaryButton {
            objectName: "nameConfirm"
            text: sheet.confirmText
            enabled: field.text.trim() !== ""
            onClicked: sheet.run()
        }
    ]
}
