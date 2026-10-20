import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Find stations: searches radio-browser.info, the open directory of
// internet radio, by name. Each result can be played (which keeps it under
// My stations) or added without playing.
ToolSheet {
    id: sheet
    objectName: "findStationsSheet"
    title: qsTr("Find radio stations")
    subtitle: qsTr("from radio-browser.info")
    preferredWidth: 860
    preferredHeight: 620

    function openSheet(text) {
        open()
        if (text) { field.text = text; radio.search(text) }
        field.forceActiveFocus()
        field.selectAll()
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 18
        spacing: 12

        RowLayout {
            Layout.fillWidth: true
            spacing: 10
            TextField {
                id: field
                objectName: "findStationsField"
                Layout.fillWidth: true
                placeholderText: qsTr("Station name, for example “jazz” or “BBC Radio 6”")
                font.pixelSize: 14
                Keys.onReturnPressed: radio.search(text)
                Keys.onEnterPressed: radio.search(text)
                Keys.onDownPressed: { if (list.count > 0) { list.currentIndex = 0; list.forceActiveFocus() } }
            }
            PrimaryButton {
                objectName: "findStationsGo"
                text: qsTr("Search")
                enabled: field.text.trim() !== ""
                onClicked: radio.search(field.text)
            }
        }

        Text {
            Layout.fillWidth: true
            visible: text !== ""
            text: radio.searching ? qsTr("Searching for “%1”…").arg(radio.searchedFor)
                : radio.searchError !== "" ? radio.searchError
                : radio.searchedFor !== "" ? (radio.results.length === 1 ? qsTr("1 station for “%1”").arg(radio.searchedFor)
                                              : qsTr("%1 stations for “%2”").arg(radio.results.length).arg(radio.searchedFor))
                : ""
            font.pixelSize: 12
            color: radio.searchError !== "" ? theme.c.red : theme.c.light_foreground
        }

        ListView {
            id: list
            objectName: "findStationsList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: radio.results
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
            keyNavigationEnabled: true
            Keys.onReturnPressed: if (currentIndex >= 0) { radio.previewResult(currentIndex); sheet.close() }
            Keys.onEnterPressed: if (currentIndex >= 0) { radio.previewResult(currentIndex); sheet.close() }
            Keys.onUpPressed: { if (currentIndex <= 0) field.forceActiveFocus(); else decrementCurrentIndex() }
            Keys.onPressed: (event) => {
                if (event.key === Qt.Key_A && !event.modifiers && currentIndex >= 0) {
                    radio.addResult(currentIndex, false)
                    event.accepted = true
                }
            }

            delegate: Rectangle {
                id: row
                required property int index
                required property var modelData
                width: ListView.view.width
                height: 58
                radius: Math.min(4, theme.radius)
                color: ListView.isCurrentItem && list.activeFocus ? theme.listSelection
                     : hover.hovered ? theme.c.lighter_background : "transparent"
                HoverHandler { id: hover }
                MouseArea { anchors.fill: parent; onClicked: { list.currentIndex = row.index; list.forceActiveFocus() } }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 10
                    anchors.rightMargin: 8
                    spacing: 12
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2
                        Text {
                            Layout.fillWidth: true
                            text: row.modelData.name
                            elide: Text.ElideRight
                            font.pixelSize: 14; font.bold: true
                            color: theme.c.bright_foreground
                        }
                        Text {
                            Layout.fillWidth: true
                            text: [row.modelData.codec ? row.modelData.codec + (row.modelData.bitrate > 0 ? " " + row.modelData.bitrate + " kbps" : "") : "",
                                   row.modelData.country, row.modelData.tags].filter(s => s).join("  ·  ")
                            elide: Text.ElideRight
                            font.pixelSize: 12
                            color: theme.c.light_foreground
                        }
                    }
                    Text {
                        visible: row.modelData.known
                        text: qsTr("In My stations")
                        font.pixelSize: 12
                        color: theme.c.green
                    }
                    Button {
                        text: qsTr("Play")
                        focusPolicy: Qt.NoFocus
                        onClicked: { radio.previewResult(row.index); sheet.close() }
                    }
                    Button {
                        text: qsTr("Add")
                        flat: true
                        enabled: !row.modelData.known
                        focusPolicy: Qt.NoFocus
                        onClicked: radio.addResult(row.index, false)
                    }
                }
            }
        }

        Text {
            Layout.fillWidth: true
            elide: Text.ElideRight
            text: qsTr("Enter: play (and keep)   ·   A: add to My stations   ·   Esc: close")
            font.pixelSize: 11
            color: theme.c.muted
        }
    }

    buttons: [
        Button { text: qsTr("Close"); flat: true; onClicked: sheet.close() }
    ]
}
