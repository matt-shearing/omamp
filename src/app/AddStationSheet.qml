import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Adds a station by its stream address (or a .pls or .m3u address that
// points at one) to My stations, and plays it.
ToolSheet {
    id: sheet
    objectName: "addStationSheet"
    title: qsTr("Add a radio station")
    preferredWidth: 600
    preferredHeight: 290

    function openSheet() {
        url.text = ""
        name.text = ""
        open()
        url.forceActiveFocus()
    }
    function run() {
        if (url.text.trim() === "") return
        radio.addUrl(url.text, name.text, true)
        close()
    }
    onAccepted: run()

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 20
        spacing: 10
        TextField {
            id: url
            objectName: "addStationUrl"
            Layout.fillWidth: true
            placeholderText: qsTr("Stream address (http or https; .pls and .m3u work too)")
            font.pixelSize: 14
            Keys.onReturnPressed: sheet.run()
            Keys.onEnterPressed: sheet.run()
        }
        TextField {
            id: name
            objectName: "addStationName"
            Layout.fillWidth: true
            placeholderText: qsTr("Name (optional)")
            font.pixelSize: 14
            Keys.onReturnPressed: sheet.run()
            Keys.onEnterPressed: sheet.run()
        }
        Item { Layout.fillHeight: true }
    }

    buttons: [
        Button { text: qsTr("Cancel"); flat: true; onClicked: sheet.close() },
        PrimaryButton {
            objectName: "addStationConfirm"
            text: qsTr("Add and play")
            enabled: url.text.trim() !== ""
            onClicked: sheet.run()
        }
    ]
}
