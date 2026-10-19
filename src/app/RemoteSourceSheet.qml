import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Add a Navidrome (or other Subsonic) server: its address, how to sign in,
// a test, and how to stream. Adding signs in, keeps the password or key in
// the desktop keyring, and copies the server's library in as one job.
ToolSheet {
    id: sheet
    objectName: "remoteSourceSheet"
    readonly property var nas: library.nas
    title: qsTr("Add a Navidrome or Subsonic server")
    preferredWidth: 760
    preferredHeight: 660
    property string testText: ""
    property bool testOk: false
    property string errorText: ""
    property bool adding: false
    dirty: address.text !== "" || secret.text !== ""
    onAccepted: if (addButton.enabled) add()

    function openSheet() {
        library.activate()
        address.text = ""
        user.text = ""
        secret.text = ""
        name.text = ""
        passwordMode.checked = true
        quality.currentIndex = 0
        testText = ""
        errorText = ""
        adding = false
        open()
        address.forceActiveFocus()
    }
    function add() {
        errorText = ""
        const q = qualities[quality.currentIndex]
        adding = nas.addServer(address.text, user.text, secret.text, keyMode.checked, name.text, q.bitrate, q.format)
    }
    readonly property var qualities: [
        { label: qsTr("Original file"), bitrate: 0, format: "" },
        { label: qsTr("Opus, 192 kbit/s"), bitrate: 192, format: "opus" },
        { label: qsTr("Opus, 128 kbit/s"), bitrate: 128, format: "opus" },
        { label: qsTr("MP3, 320 kbit/s"), bitrate: 320, format: "mp3" },
        { label: qsTr("MP3, 192 kbit/s"), bitrate: 192, format: "mp3" }
    ]

    Connections {
        target: sheet.nas
        function onTested(ok, text) { sheet.testOk = ok; sheet.testText = text }
        function onServerAdded(ok, error) {
            sheet.adding = false
            if (ok) sheet.close()
            else sheet.errorText = error
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 20
        spacing: 8

        TextField {
            id: address
            objectName: "serverAddress"
            Layout.fillWidth: true
            placeholderText: qsTr("Server address, for example http://nas:4533")
            font.pixelSize: 13
            selectByMouse: true
        }
        RowLayout {
            spacing: 16
            Text { text: qsTr("Sign in with"); font.pixelSize: 13; color: theme.c.light_foreground }
            RadioButton { id: passwordMode; objectName: "passwordMode"; text: qsTr("User name and password"); font.pixelSize: 13; checked: true }
            RadioButton { id: keyMode; objectName: "keyMode"; text: qsTr("API key"); font.pixelSize: 13 }
        }
        TextField {
            id: user
            objectName: "serverUser"
            Layout.fillWidth: true
            visible: passwordMode.checked
            placeholderText: qsTr("User name")
            font.pixelSize: 13
            selectByMouse: true
        }
        TextField {
            id: secret
            objectName: "serverSecret"
            Layout.fillWidth: true
            placeholderText: keyMode.checked ? qsTr("API key") : qsTr("Password")
            echoMode: TextInput.Password
            font.pixelSize: 13
        }
        Text {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: qsTr("The password or key is kept in your desktop keyring (or a private file when there is none), never in the library. Requests are signed with it; the password itself is never sent.")
            font.pixelSize: 12
            color: theme.c.muted
        }
        RowLayout {
            spacing: 10
            Button {
                objectName: "testServer"
                text: sheet.nas.testing ? qsTr("Testing…") : qsTr("Test the connection")
                font.pixelSize: 13
                enabled: !sheet.nas.testing && address.text.trim() !== "" && secret.text !== ""
                onClicked: { sheet.testText = ""; sheet.nas.testServer(address.text, user.text, secret.text, keyMode.checked) }
            }
            Text {
                objectName: "testResult"
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: sheet.testText
                font.pixelSize: 13
                color: sheet.testOk ? theme.c.green : theme.c.red
            }
        }
        TextField {
            id: name
            Layout.fillWidth: true
            Layout.topMargin: 6
            placeholderText: qsTr("Name in the library (optional; the server's address otherwise)")
            font.pixelSize: 13
            selectByMouse: true
        }
        RowLayout {
            spacing: 12
            Text { text: qsTr("Stream as"); font.pixelSize: 13; color: theme.c.light_foreground }
            ComboBox {
                id: quality
                Layout.preferredWidth: 240
                implicitHeight: 34; topInset: 0; bottomInset: 0
                font.pixelSize: 13
                model: sheet.qualities.map(q => q.label)
            }
            Text {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: qsTr("Smaller saves data away from home; the original sounds best and seeks best.")
                font.pixelSize: 12
                color: theme.c.muted
            }
        }
        Item { Layout.fillHeight: true }
        Text {
            objectName: "addError"
            Layout.fillWidth: true
            visible: sheet.errorText !== ""
            wrapMode: Text.Wrap
            text: sheet.errorText
            font.pixelSize: 13
            color: theme.c.red
        }
    }

    footerLeft: [
        Text {
            anchors.verticalCenter: parent.verticalCenter
            width: parent.width
            elide: Text.ElideRight
            text: sheet.adding ? library.jobText : ""
            font.pixelSize: 12
            color: theme.c.light_foreground
        }
    ]
    buttons: [
        Button { text: qsTr("Cancel"); flat: true; onClicked: sheet.askToClose() },
        PrimaryButton {
            id: addButton
            objectName: "addServerButton"
            text: sheet.adding ? qsTr("Adding…") : qsTr("Add and sync")
            enabled: !sheet.adding && address.text.trim() !== "" && secret.text !== "" && (keyMode.checked || user.text.trim() !== "")
            onClicked: sheet.add()
        }
    ]
}
