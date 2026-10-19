import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// The auto-playlist editor: a name and a query, checked and counted as you
// type, with a preview of the tracks it picks. The rules below the query
// build the same text from rows of field, operator and value. The text is
// what is saved; when the rows can't say what the text says (brackets, AND
// mixed with OR), they step aside and say so.
ToolSheet {
    id: sheet
    objectName: "autoPlaylistSheet"
    title: playlistId > 0 ? qsTr("Edit auto-playlist") : qsTr("New auto-playlist")
    subtitle: check.ok ? qsTr("%n tracks", "", check.count || 0) : ""
    preferredWidth: 1120
    preferredHeight: 720
    dirty: edited

    property var playlistId: 0
    property var parentId: 0
    property bool edited: false
    // The last check of the text: {ok, error, position, count, preview}.
    property var check: ({ ok: true, count: 0, preview: [] })
    // The rule rows, from the text or from the rows' own edits.
    property var rules: ({ any: false, rules: [], sort: "", limitKind: "none", limitValue: "", representable: true })
    readonly property var fields: library.playlists.ruleFields()

    function openFor(id, parent) {
        playlistId = id || 0
        parentId = parent || 0
        const p = playlistId > 0 ? library.playlists.get(playlistId) : null
        nameField.text = p ? p.name : qsTr("New auto-playlist")
        queryField.text = p ? p.query : "rating:4.."
        readText()
        edited = false
        open()
        nameField.forceActiveFocus()
        nameField.selectAll()
    }
    // The text changed: check it and read the rows from it.
    function readText() {
        check = library.playlists.checkQuery(queryField.text)
        rules = library.playlists.rulesFromQuery(queryField.text)
    }
    // A row changed: write the text from the rows (the rows stay as they
    // are, so a half-filled row isn't lost).
    function writeText() {
        const text = library.playlists.queryFromRules(rules)
        if (text !== queryField.text) {
            queryField.programmatic = true
            queryField.text = text
            queryField.programmatic = false
        }
        check = library.playlists.checkQuery(text)
        edited = true
    }
    function setRules(change) {
        const r = JSON.parse(JSON.stringify(rules))
        change(r)
        rules = r
        writeText()
    }
    function fieldType(name) {
        for (const f of fields) if (f.name === name) return f.type
        return "text"
    }
    function save() {
        if (!check.ok || nameField.text.trim() === "") return
        let ok
        if (playlistId > 0) ok = library.playlists.saveAutoPlaylist(playlistId, nameField.text, queryField.text)
        else {
            const id = library.playlists.createAutoPlaylist(nameField.text, queryField.text, parentId)
            ok = id > 0
            if (ok) library.playlists.show(id)
        }
        if (ok) { edited = false; close() }
    }
    onAccepted: save()

    Timer { id: typing; interval: 150; onTriggered: sheet.readText() }

    RowLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 16

        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.preferredWidth: 640
            spacing: 8

            TextField {
                id: nameField
                objectName: "autoName"
                Layout.fillWidth: true
                placeholderText: qsTr("Name")
                font.pixelSize: 14
                onTextEdited: sheet.edited = true
            }
            TextField {
                id: queryField
                objectName: "autoQuery"
                property bool programmatic: false
                Layout.fillWidth: true
                placeholderText: qsTr("Query, as in the search box: genre:jazz rating:4.. sort:-rating limit:50")
                font.family: "JetBrainsMono Nerd Font"
                font.pixelSize: 13
                onTextChanged: if (!programmatic) { sheet.edited = true; typing.restart() }
            }
            Text {
                objectName: "autoCheck"
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: !sheet.check.ok
                      ? (sheet.check.position >= 0 ? qsTr("%1, at character %2").arg(sheet.check.error).arg(sheet.check.position + 1)
                                                    : sheet.check.error)
                      : qsTr("%n tracks match", "", sheet.check.count || 0)
                font.pixelSize: 12
                color: sheet.check.ok ? theme.c.light_foreground : theme.c.red
            }

            Rectangle { Layout.fillWidth: true; height: 1; color: theme.c.lighter_background; Layout.topMargin: 4 }

            // The rule rows.
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Text {
                    text: qsTr("Rules")
                    font.pixelSize: 14
                    font.bold: true
                    color: theme.c.bright_foreground
                }
                Item { Layout.fillWidth: true }
                Text {
                    visible: sheet.rules.representable
                    text: qsTr("Match")
                    font.pixelSize: 13
                    color: theme.c.light_foreground
                }
                ComboBox {
                    objectName: "matchAll"
                    visible: sheet.rules.representable
                    Layout.preferredWidth: 230
                    implicitHeight: 34; topInset: 0; bottomInset: 0
                    font.pixelSize: 13
                    model: [qsTr("all of these rules"), qsTr("any of these rules")]
                    currentIndex: sheet.rules.any ? 1 : 0
                    onActivated: (i) => sheet.setRules((r) => r.any = i === 1)
                }
            }
            Text {
                objectName: "advancedNote"
                Layout.fillWidth: true
                visible: !sheet.rules.representable
                wrapMode: Text.Wrap
                text: sheet.check.ok ? qsTr("Advanced query: the rules can't show it (%1). Edit the text above, or clear it to start again with rules.").arg(sheet.rules.reason)
                                     : qsTr("Fix the query above to see it as rules.")
                font.pixelSize: 13
                color: theme.c.yellow
            }
            ScrollView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                visible: sheet.rules.representable
                clip: true
                ColumnLayout {
                    width: parent.width
                    spacing: 4
                    Repeater {
                        model: sheet.rules.representable ? sheet.rules.rules : []
                        RowLayout {
                            id: ruleRow
                            required property var modelData
                            required property int index
                            readonly property string type: sheet.fieldType(modelData.field)
                            readonly property var ops: library.playlists.ruleOperators(type)
                            Layout.fillWidth: true
                            spacing: 6
                            ComboBox {
                                Layout.preferredWidth: 170
                                font.pixelSize: 13
                                model: sheet.fields
                                textRole: "label"
                                currentIndex: {
                                    for (let i = 0; i < sheet.fields.length; ++i)
                                        if (sheet.fields[i].name === ruleRow.modelData.field) return i
                                    return 0
                                }
                                onActivated: (i) => sheet.setRules((r) => { r.rules[ruleRow.index].field = sheet.fields[i].name })
                            }
                            ComboBox {
                                Layout.preferredWidth: 180
                                font.pixelSize: 13
                                model: ruleRow.ops
                                textRole: "label"
                                currentIndex: {
                                    for (let i = 0; i < ruleRow.ops.length; ++i)
                                        if (ruleRow.ops[i].id === ruleRow.modelData.op) return i
                                    return 0
                                }
                                onActivated: (i) => sheet.setRules((r) => { r.rules[ruleRow.index].op = ruleRow.ops[i].id })
                            }
                            TextField {
                                Layout.fillWidth: true
                                visible: ruleRow.modelData.op !== "empty" && ruleRow.modelData.op !== "set"
                                font.pixelSize: 13
                                text: ruleRow.modelData.value
                                placeholderText: ruleRow.type === "date" ? qsTr("30d, 2024, 2024-01..2024-06")
                                               : ruleRow.type === "length" ? qsTr("3:00, 10m")
                                               : ruleRow.type === "rating" ? qsTr("stars, 0 to 5")
                                               : qsTr("value")
                                onTextEdited: { sheet.rules.rules[ruleRow.index].value = text; sheet.writeText() }
                            }
                            TextField {
                                Layout.preferredWidth: 110
                                visible: ruleRow.modelData.op === "between"
                                font.pixelSize: 13
                                text: ruleRow.modelData.value2
                                placeholderText: qsTr("and")
                                onTextEdited: { sheet.rules.rules[ruleRow.index].value2 = text; sheet.writeText() }
                            }
                            ToolButton {
                                text: "✕"
                                implicitWidth: 32; implicitHeight: 32
                                ToolTip.visible: hovered
                                ToolTip.delay: 600
                                ToolTip.text: qsTr("Remove this rule")
                                onClicked: sheet.setRules((r) => r.rules.splice(ruleRow.index, 1))
                            }
                        }
                    }
                    Button {
                        objectName: "addRule"
                        text: qsTr("Add a rule")
                        flat: true
                        font.pixelSize: 13
                        onClicked: sheet.setRules((r) => r.rules.push({ field: "genre", op: "contains", value: "", value2: "" }))
                    }
                }
            }

            // Order and limit.
            GridLayout {
                Layout.fillWidth: true
                visible: sheet.rules.representable
                columns: 4
                columnSpacing: 8
                rowSpacing: 4
                Text { text: qsTr("Order"); font.pixelSize: 13; color: theme.c.light_foreground }
                ComboBox {
                    id: sortBox
                    Layout.preferredWidth: 200
                    implicitHeight: 34; topInset: 0; bottomInset: 0
                    font.pixelSize: 13
                    readonly property string key: sheet.rules.sort.replace(/^-/, "").split(",")[0].split(":")[0]
                    model: [{ name: "", label: qsTr("Library order") }, { name: "random", label: qsTr("Random") }]
                           .concat(sheet.fields.slice(1))
                    textRole: "label"
                    enabled: sheet.rules.sort.indexOf(",") < 0
                    currentIndex: {
                        for (let i = 0; i < model.length; ++i) if (model[i].name === key) return i
                        return 0
                    }
                    onActivated: (i) => sheet.setRules((r) => {
                        const name = model[i].name
                        r.sort = name === "" ? "" : (descending.checked && name !== "random" ? "-" : "") + name
                    })
                }
                CheckBox {
                    id: descending
                    text: qsTr("Descending")
                    font.pixelSize: 13
                    enabled: sortBox.key !== "" && sortBox.key !== "random" && sortBox.enabled
                    checked: sheet.rules.sort.startsWith("-")
                    onClicked: sheet.setRules((r) => r.sort = (checked ? "-" : "") + r.sort.replace(/^-/, ""))
                }
                Button {
                    text: qsTr("New shuffle")
                    flat: true
                    font.pixelSize: 13
                    visible: sortBox.key === "random" && sheet.playlistId > 0
                    onClicked: { library.playlists.reroll(sheet.playlistId); sheet.readText() }
                }
                Item { visible: !(sortBox.key === "random" && sheet.playlistId > 0); width: 1 }

                Text { text: qsTr("Limit"); font.pixelSize: 13; color: theme.c.light_foreground }
                ComboBox {
                    Layout.preferredWidth: 200
                    implicitHeight: 34; topInset: 0; bottomInset: 0
                    font.pixelSize: 13
                    readonly property var kinds: ["none", "count", "time", "size"]
                    model: [qsTr("No limit"), qsTr("Number of tracks"), qsTr("Playing time"), qsTr("Size")]
                    currentIndex: Math.max(0, kinds.indexOf(sheet.rules.limitKind))
                    onActivated: (i) => sheet.setRules((r) => {
                        r.limitKind = kinds[i]
                        r.limitValue = i === 0 ? "" : i === 1 ? "50" : i === 2 ? "2h" : "700mb"
                    })
                }
                TextField {
                    Layout.preferredWidth: 120
                    visible: sheet.rules.limitKind !== "none"
                    font.pixelSize: 13
                    text: sheet.rules.limitValue
                    placeholderText: sheet.rules.limitKind === "count" ? "50" : sheet.rules.limitKind === "time" ? "2h" : "700mb"
                    onTextEdited: { sheet.rules.limitValue = text; sheet.writeText() }
                }
                Item { width: 1 }
            }
        }

        // What the query picks now.
        Rectangle {
            Layout.fillHeight: true
            Layout.preferredWidth: 400
            color: theme.c.darker_background
            border.width: 1
            border.color: theme.c.lighter_background
            radius: Math.min(4, theme.radius)
            clip: true
            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 1
                spacing: 0
                Text {
                    Layout.fillWidth: true
                    Layout.margins: 10
                    text: sheet.check.ok ? qsTr("Preview: the first %n", "", (sheet.check.preview || []).length) : qsTr("Preview")
                    font.pixelSize: 12
                    font.bold: true
                    color: theme.c.light_foreground
                }
                ListView {
                    objectName: "autoPreview"
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: sheet.check.ok ? sheet.check.preview : []
                    boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                    delegate: Item {
                        required property var modelData
                        width: ListView.view.width
                        height: 40
                        Column {
                            anchors { left: parent.left; right: parent.right; leftMargin: 10; rightMargin: 10
                                      verticalCenter: parent.verticalCenter }
                            Text {
                                width: parent.width
                                elide: Text.ElideRight
                                text: modelData.title
                                font.pixelSize: 13
                                color: theme.c.foreground
                            }
                            Text {
                                width: parent.width
                                elide: Text.ElideRight
                                text: [modelData.artist, modelData.album].filter((s) => s).join(" — ")
                                font.pixelSize: 11
                                color: theme.c.muted
                            }
                        }
                    }
                }
            }
        }
    }

    footerLeft: [
        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: qsTr("The list updates by itself as the library changes.")
            font.pixelSize: 12
            color: theme.c.muted
        }
    ]
    buttons: [
        Button { text: qsTr("Cancel"); flat: true; onClicked: sheet.askToClose() },
        PrimaryButton {
            objectName: "autoSave"
            text: sheet.playlistId > 0 ? qsTr("Save") : qsTr("Create")
            enabled: sheet.check.ok && nameField.text.trim() !== ""
            onClicked: sheet.save()
        }
    ]
}
