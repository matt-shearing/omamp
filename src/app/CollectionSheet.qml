import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// A collection: a name, a query that picks its tracks, the nodes it shows
// under itself in the tree, and the columns its track list shows. Columns
// hidden or shown while the collection is open are remembered too.
ToolSheet {
    id: sheet
    objectName: "collectionSheet"
    title: collectionId > 0 ? qsTr("Edit collection") : qsTr("New collection")
    subtitle: check.ok ? qsTr("%n tracks", "", check.count || 0) : ""
    preferredWidth: 760
    preferredHeight: 600
    dirty: edited

    property var collectionId: 0
    property bool edited: false
    property var check: ({ ok: true, count: 0 })
    property var tree: []
    property var columns: []
    property string visibleWhen: "always"
    signal confirm(string title, string text, string confirmText, var action)

    function openFor(id) {
        collectionId = id || 0
        const c = collectionId > 0 ? library.playlists.collection(collectionId) : null
        nameField.text = c ? c.name : qsTr("New collection")
        queryField.text = c ? c.query : ""
        tree = c ? c.tree : ["artist", "album", "genre"]
        columns = c && c.columns.length > 0 ? c.columns : ["number", "title", "artist", "album", "length", "rating"]
        visibleWhen = c ? c.visible : "always"
        check = library.playlists.checkQuery(queryField.text)
        edited = false
        open()
        nameField.forceActiveFocus()
        nameField.selectAll()
    }
    function toggled(list, id, on) {
        const out = list.filter((x) => x !== id)
        if (on) out.push(id)
        edited = true
        return out
    }
    function save() {
        if (!check.ok || nameField.text.trim() === "") return
        // Columns keep the list's own order.
        const order = library.playlists.columnNames()
        const id = library.playlists.saveCollection({
            id: collectionId, name: nameField.text, query: queryField.text, visible: visibleWhen,
            tree: library.playlists.treeNodeChoices().map((n) => n.id).filter((n) => tree.indexOf(n) >= 0),
            columns: order.filter((c) => columns.indexOf(c) >= 0) })
        if (id > 0) { edited = false; close() }
    }
    onAccepted: save()

    Timer { id: typing; interval: 150; onTriggered: sheet.check = library.playlists.checkQuery(queryField.text) }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 8

        TextField {
            id: nameField
            objectName: "collectionName"
            Layout.fillWidth: true
            placeholderText: qsTr("Name")
            font.pixelSize: 14
            onTextEdited: sheet.edited = true
        }
        TextField {
            id: queryField
            objectName: "collectionQuery"
            Layout.fillWidth: true
            placeholderText: qsTr("Query: type:music, genre:classical, -genre:christmas … (empty: every track)")
            font.family: "JetBrainsMono Nerd Font"
            font.pixelSize: 13
            onTextEdited: { sheet.edited = true; typing.restart() }
        }
        Text {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: sheet.check.ok ? qsTr("%n tracks in this collection", "", sheet.check.count || 0) : sheet.check.error
            font.pixelSize: 12
            color: sheet.check.ok ? theme.c.light_foreground : theme.c.red
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            Text { text: qsTr("Show in the tree"); font.pixelSize: 13; color: theme.c.light_foreground }
            ComboBox {
                Layout.preferredWidth: 220
                implicitHeight: 34; topInset: 0; bottomInset: 0
                font.pixelSize: 13
                readonly property var values: ["always", "nonempty", "never"]
                model: [qsTr("Always"), qsTr("When it has tracks"), qsTr("Never")]
                currentIndex: Math.max(0, values.indexOf(sheet.visibleWhen))
                onActivated: (i) => { sheet.visibleWhen = values[i]; sheet.edited = true }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 24
            ColumnLayout {
                Layout.alignment: Qt.AlignTop
                spacing: 0
                Text { text: qsTr("Nodes under it"); font.pixelSize: 13; font.bold: true; color: theme.c.bright_foreground }
                Repeater {
                    model: library.playlists.treeNodeChoices()
                    CheckBox {
                        required property var modelData
                        text: modelData.label
                        font.pixelSize: 13
                        checked: sheet.tree.indexOf(modelData.id) >= 0
                        onClicked: sheet.tree = sheet.toggled(sheet.tree, modelData.id, checked)
                    }
                }
            }
            ColumnLayout {
                Layout.alignment: Qt.AlignTop
                spacing: 0
                Text { text: qsTr("Columns"); font.pixelSize: 13; font.bold: true; color: theme.c.bright_foreground }
                GridLayout {
                    columns: 2
                    columnSpacing: 12
                    rowSpacing: 0
                    Repeater {
                        model: library.playlists.columnNames()
                        CheckBox {
                            required property string modelData
                            readonly property var labels: ({ number: qsTr("#"), title: qsTr("Title"), artist: qsTr("Artist"),
                                                             album: qsTr("Album"), year: qsTr("Year"), genre: qsTr("Genre"),
                                                             length: qsTr("Length"), rating: qsTr("Rating"), plays: qsTr("Plays"),
                                                             added: qsTr("Added"), format: qsTr("Format") })
                            text: labels[modelData]
                            font.pixelSize: 13
                            enabled: modelData !== "title"
                            checked: modelData === "title" || sheet.columns.indexOf(modelData) >= 0
                            onClicked: sheet.columns = sheet.toggled(sheet.columns, modelData, checked)
                        }
                    }
                }
            }
            Item { Layout.fillWidth: true }
        }
    }

    footerLeft: [
        Button {
            anchors.verticalCenter: parent.verticalCenter
            visible: sheet.collectionId > 0
            text: qsTr("Delete collection…")
            flat: true
            font.pixelSize: 13
            onClicked: {
                const id = sheet.collectionId
                sheet.edited = false
                sheet.close()
                sheet.confirm(qsTr("Delete the collection?"),
                              qsTr("“%1” leaves the tree. Its tracks stay in the library.").arg(nameField.text),
                              qsTr("Delete"), () => library.playlists.removeCollection(id))
            }
        }
    ]
    buttons: [
        Button { text: qsTr("Cancel"); flat: true; onClicked: sheet.askToClose() },
        PrimaryButton {
            objectName: "collectionSave"
            text: sheet.collectionId > 0 ? qsTr("Save") : qsTr("Create")
            enabled: sheet.check.ok && nameField.text.trim() !== ""
            onClicked: sheet.save()
        }
    ]
}
