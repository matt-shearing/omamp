import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Asks before doing something that can't be taken back from inside Omamp:
// a question, what will happen, and two buttons. Enter confirms, Esc
// cancels. ask() takes the action to run.
ToolSheet {
    id: sheet
    objectName: "confirmSheet"
    preferredWidth: 560
    preferredHeight: 280
    property string text: ""
    property string confirmText: qsTr("OK")
    property var action: null

    function ask(title, text, confirmText, action) {
        sheet.title = title
        sheet.text = text
        sheet.confirmText = confirmText
        sheet.action = action
        open()
        confirm.forceActiveFocus()
    }
    function run() {
        const a = action
        action = null
        close()
        if (a) a()
    }
    onAccepted: run()

    Text {
        anchors.fill: parent
        anchors.margins: 20
        wrapMode: Text.Wrap
        text: sheet.text
        font.pixelSize: 14
        color: theme.c.foreground
    }

    buttons: [
        Button { text: qsTr("Cancel"); flat: true; onClicked: sheet.close() },
        PrimaryButton {
            id: confirm
            objectName: "confirmButton"
            text: sheet.confirmText
            highlighted: true
            Keys.onReturnPressed: sheet.run()
            Keys.onEnterPressed: sheet.run()
            onClicked: sheet.run()
        }
    ]
}
