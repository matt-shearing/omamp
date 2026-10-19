import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Devices: sync profiles. Each copies playlists, collections or a query to
// a folder, a USB drive or a Syncthing folder, converting the formats its
// rules name with ffmpeg. Preview shows what a sync would do, file by file,
// before anything is written; Sync now runs it as a job in the jobs bar.
ToolSheet {
    id: sheet
    objectName: "devicesSheet"
    readonly property var devices: library.devices
    title: qsTr("Devices")
    subtitle: qsTr("Copy music to a phone, a USB drive or a Syncthing folder")
    preferredWidth: 1180
    preferredHeight: 800
    dirty: editDirty
    signal confirm(string title, string text, string confirmText, var action)

    // The profile being edited (0: a new one).
    property int editId: 0
    property string editName: ""
    property string editKind: "folder"
    property string editTarget: ""
    property var editSources: []
    property string editRules: ""
    property string editMask: ""
    property bool editDeleteOthers: false
    property bool editPlaylists: true
    property bool editDirty: false
    property var sourceChoices: []

    readonly property var kinds: [
        { kind: "folder", label: qsTr("A folder") },
        { kind: "usb", label: qsTr("A USB drive or phone") },
        { kind: "syncthing", label: qsTr("A Syncthing folder") }
    ]
    readonly property var current: {
        for (const p of devices.profiles) if (p.id === editId) return p
        return null
    }

    function openSheet(id) {
        library.activate()
        devices.reload()
        devices.refreshDevices()
        sourceChoices = devices.sourceChoices()
        open()
        if (id && id > 0) edit(id)
        else if (devices.profiles.length > 0) edit(devices.profiles[0].id)
        else newProfile()
    }
    function edit(id) {
        for (const p of devices.profiles) {
            if (p.id !== id) continue
            editId = p.id
            editName = p.name
            editKind = p.kind
            editTarget = p.target
            editSources = p.sources.slice()
            editRules = p.rules.join("; ")
            editMask = p.mask === "" ? devices.defaultMask : p.mask
            editDeleteOthers = p.deleteOthers
            editPlaylists = p.writePlaylists
            editDirty = false
            if (devices.preview.profileId !== id) devices.clearPreview()
            return
        }
    }
    function newProfile() {
        editId = 0
        editName = devices.profiles.length === 0 ? qsTr("Phone") : ""
        editKind = "folder"
        editTarget = ""
        editSources = []
        editRules = "lossless->opus:128"
        editMask = devices.defaultMask
        editDeleteOthers = false
        editPlaylists = true
        editDirty = true
        devices.clearPreview()
        nameField.focusInput()
    }
    function changed() { editDirty = true; devices.clearPreview() }
    function save() {
        const rules = editRules.split(/[;\n]/).map(s => s.trim()).filter(s => s !== "")
        const id = devices.saveProfile({ id: editId, name: editName, kind: editKind, target: editTarget,
                                         sources: editSources, rules: rules, mask: editMask,
                                         deleteOthers: editDeleteOthers, writePlaylists: editPlaylists })
        if (id > 0) { editDirty = false; edit(id) }
        return id
    }
    function addSource(source) {
        if (source === "" || editSources.indexOf(source) >= 0) return
        editSources = editSources.concat([source])
        changed()
    }
    function sourceLabel(s) {
        if (s === "library") return qsTr("Entire Library")
        const kind = s.slice(0, s.indexOf(":")), ref = s.slice(s.indexOf(":") + 1)
        if (kind === "collection") return qsTr("Collection: %1").arg(ref)
        if (kind === "query") return qsTr("Query: %1").arg(ref)
        return qsTr("Playlist: %1").arg(ref)
    }
    function presetIndex() {
        const presets = devices.rulePresets
        const now = editRules.split(/[;\n]/).map(s => s.trim()).filter(s => s !== "").join(";")
        for (let i = 0; i < presets.length; ++i)
            if (presets[i].rules.join(";") === now) return i
        return presets.length
    }

    onAccepted: if (editDirty) save()
    onClosed: devices.clearPreview()

    Connections {
        target: sheet.devices
        function onTargetChosen(path) { sheet.editTarget = path; sheet.changed() }
    }

    component SmallButton: Button {
        flat: true
        font.pixelSize: 12
        implicitHeight: 32
        topInset: 0; bottomInset: 0
    }
    component Label: Text {
        Layout.preferredWidth: 112
        horizontalAlignment: Text.AlignRight
        font.pixelSize: 13
        color: theme.c.light_foreground
    }

    RowLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 16

        // The profiles.
        ColumnLayout {
            Layout.preferredWidth: 260
            Layout.fillHeight: true
            spacing: 8
            ListView {
                id: profileList
                objectName: "profileList"
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                spacing: 4
                model: sheet.devices.profiles
                delegate: Rectangle {
                    id: row
                    required property var modelData
                    width: ListView.view.width
                    implicitHeight: rowText.implicitHeight + 16
                    radius: Math.min(4, theme.radius)
                    color: modelData.id === sheet.editId ? theme.c.selection
                                                         : mouse.containsMouse ? theme.c.darker_background : "transparent"
                    ColumnLayout {
                        id: rowText
                        anchors { left: parent.left; right: parent.right; verticalCenter: parent.verticalCenter; margins: 10 }
                        spacing: 2
                        Text {
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                            text: row.modelData.name
                            font.pixelSize: 14
                            font.bold: true
                            color: theme.c.bright_foreground
                        }
                        RowLayout {
                            spacing: 6
                            Rectangle { width: 8; height: 8; radius: 4; color: row.modelData.ready ? theme.c.green : theme.c.red }
                            Text {
                                Layout.fillWidth: true
                                elide: Text.ElideRight
                                text: row.modelData.ready ? row.modelData.lastText : row.modelData.stateText
                                font.pixelSize: 11
                                color: theme.c.muted
                            }
                        }
                    }
                    MouseArea {
                        id: mouse
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: sheet.edit(row.modelData.id)
                    }
                }
            }
            Text {
                visible: sheet.devices.profiles.length === 0
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: qsTr("No sync profiles yet. Make one on the right.")
                font.pixelSize: 12
                color: theme.c.muted
            }
            Button {
                objectName: "newProfile"
                Layout.fillWidth: true
                text: qsTr("New profile")
                font.pixelSize: 13
                onClicked: sheet.newProfile()
            }
        }

        Rectangle { Layout.fillHeight: true; width: 1; color: theme.c.lighter_background }

        // The chosen profile, and below it the preview.
        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 8

        ScrollView {
            id: scroll
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(form.implicitHeight, sheet.height * 0.52)
            clip: true
            contentWidth: availableWidth

            ColumnLayout {
                id: form
                width: scroll.availableWidth
                spacing: 8

                FormField {
                    id: nameField
                    objectName: "profileName"
                    Layout.fillWidth: true
                    label: qsTr("Name")
                    text: sheet.editName
                    clearable: false
                    onEdit: (t) => { sheet.editName = t; sheet.changed() }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    Label { text: qsTr("Sync to") }
                    ComboBox {
                        objectName: "profileKind"
                        Layout.preferredWidth: 230
                        implicitHeight: 32; topInset: 0; bottomInset: 0
                        font.pixelSize: 13
                        model: sheet.kinds.map(k => k.label)
                        currentIndex: Math.max(0, sheet.kinds.findIndex(k => k.kind === sheet.editKind))
                        onActivated: (i) => { sheet.editKind = sheet.kinds[i].kind; sheet.changed() }
                    }
                    FormField {
                        objectName: "profileTarget"
                        Layout.fillWidth: true
                        labelWidth: 0
                        placeholder: sheet.editKind === "usb" ? "/run/media/you/PHONE/Music"
                                     : sheet.editKind === "syncthing" ? qsTr("~/Sync/Phone music") : qsTr("~/Music for the car")
                        text: sheet.editTarget
                        monospace: true
                        onEdit: (t) => { sheet.editTarget = t; sheet.changed() }
                    }
                    Button { text: qsTr("Choose…"); font.pixelSize: 13; onClicked: sheet.devices.chooseTarget() }
                }
                // Drives and Syncthing folders found now.
                Flow {
                    Layout.fillWidth: true
                    Layout.leftMargin: 120
                    spacing: 6
                    visible: sheet.devices.devices.length > 0
                    Repeater {
                        model: sheet.devices.devices
                        delegate: SmallButton {
                            required property var modelData
                            text: (modelData.kind === "usb" ? "\u{F02CA}  " : "\u{F04E6}  ") + modelData.label
                                  + (modelData.freeText !== "" ? "  ·  " + modelData.freeText : "")
                            font.family: "JetBrainsMono Nerd Font"
                            ToolTip.visible: hovered
                            ToolTip.text: modelData.path
                            onClicked: {
                                sheet.editKind = modelData.kind
                                sheet.editTarget = modelData.path + (modelData.kind === "usb" ? "/Music" : "")
                                sheet.changed()
                            }
                        }
                    }
                }
                Text {
                    Layout.fillWidth: true
                    Layout.leftMargin: 120
                    visible: sheet.current !== null && !sheet.editDirty
                    wrapMode: Text.Wrap
                    text: sheet.current ? sheet.current.stateText + (sheet.current.freeText !== "" ? "  ·  " + sheet.current.freeText : "") : ""
                    font.pixelSize: 12
                    color: sheet.current && sheet.current.ready ? theme.c.green : theme.c.red
                }

                // What goes there.
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    Label { Layout.alignment: Qt.AlignTop; Layout.topMargin: 6; text: qsTr("What to sync") }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 4
                        Repeater {
                            model: sheet.editSources
                            delegate: RowLayout {
                                required property string modelData
                                required property int index
                                Layout.fillWidth: true
                                Text {
                                    Layout.fillWidth: true
                                    elide: Text.ElideRight
                                    text: sheet.sourceLabel(modelData)
                                    font.pixelSize: 13
                                    color: theme.c.foreground
                                }
                                SmallButton {
                                    text: qsTr("Remove")
                                    onClicked: {
                                        const s = sheet.editSources.slice()
                                        s.splice(index, 1)
                                        sheet.editSources = s
                                        sheet.changed()
                                    }
                                }
                            }
                        }
                        Text {
                            visible: sheet.editSources.length === 0
                            text: qsTr("Nothing yet: add a playlist, a collection or a query.")
                            font.pixelSize: 12
                            color: theme.c.muted
                        }
                        RowLayout {
                            spacing: 6
                            ComboBox {
                                id: sourceBox
                                objectName: "sourceBox"
                                Layout.preferredWidth: 320
                                implicitHeight: 32; topInset: 0; bottomInset: 0
                                font.pixelSize: 13
                                model: sheet.sourceChoices.map(c => c.group === qsTr("Library") ? c.label : c.group + ": " + c.label)
                            }
                            Button {
                                objectName: "addSource"
                                text: qsTr("Add")
                                font.pixelSize: 13
                                enabled: sourceBox.currentIndex >= 0
                                onClicked: sheet.addSource(sheet.sourceChoices[sourceBox.currentIndex].source)
                            }
                            FormField {
                                id: queryField
                                Layout.fillWidth: true
                                labelWidth: 0
                                placeholder: qsTr("or a query, e.g. rating:4.. genre:jazz")
                                monospace: true
                            }
                            Button {
                                text: qsTr("Add query")
                                font.pixelSize: 13
                                enabled: queryField.text.trim() !== "" || queryField.input.text.trim() !== ""
                                onClicked: { sheet.addSource("query:" + queryField.input.text.trim()); queryField.text = ""; queryField.input.text = "" }
                            }
                        }
                    }
                }

                // How.
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    Label { text: qsTr("Formats") }
                    ComboBox {
                        objectName: "rulePreset"
                        Layout.fillWidth: true
                        implicitHeight: 32; topInset: 0; bottomInset: 0
                        font.pixelSize: 13
                        model: sheet.devices.rulePresets.map(p => p.label).concat([qsTr("Your own rules (below)")])
                        currentIndex: sheet.presetIndex()
                        onActivated: (i) => {
                            if (i < sheet.devices.rulePresets.length) {
                                sheet.editRules = sheet.devices.rulePresets[i].rules.join("; ")
                                sheet.changed()
                            }
                        }
                    }
                }
                FormField {
                    objectName: "profileRules"
                    Layout.fillWidth: true
                    label: qsTr("Rules")
                    text: sheet.editRules
                    monospace: true
                    placeholder: qsTr("none: every file is copied as it is")
                    tip: qsTr("The first rule that fits a file wins; a file no rule fits is copied. For example: flac,wav->opus:128; wma->skip; *->copy. Formats: opus, mp3, aac, vorbis, flac.")
                    onEdit: (t) => { sheet.editRules = t; sheet.changed() }
                }
                FormField {
                    objectName: "profileMask"
                    Layout.fillWidth: true
                    label: qsTr("File names")
                    text: sheet.editMask
                    monospace: true
                    tip: qsTr("A mask, as in Organise: where each file goes on the device")
                    onEdit: (t) => { sheet.editMask = t; sheet.changed() }
                }
                Flow {
                    Layout.fillWidth: true
                    Layout.leftMargin: 112
                    spacing: 12
                    CheckBox {
                        objectName: "deleteOthers"
                        text: qsTr("Delete other files in the folder")
                        font.pixelSize: 13
                        checked: sheet.editDeleteOthers
                        onToggled: { sheet.editDeleteOthers = checked; sheet.changed() }
                        ToolTip.visible: hovered
                        ToolTip.delay: 600
                        ToolTip.text: qsTr("Anything in the folder this sync didn't put there. Without it, only files an earlier sync put there and that are no longer in the list are removed. Hidden files (.stfolder and the like) are never touched.")
                    }
                    CheckBox {
                        text: qsTr("Write playlists as .m3u8 files")
                        font.pixelSize: 13
                        checked: sheet.editPlaylists
                        onToggled: { sheet.editPlaylists = checked; sheet.changed() }
                    }
                }

            }
        }

                // The preview.
                Rectangle { Layout.fillWidth: true; height: 1; color: theme.c.lighter_background }
                Text {
                    objectName: "previewSummary"
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    readonly property var p: sheet.devices.preview
                    text: sheet.devices.previewing ? qsTr("Looking at the library and the device…")
                          : p.profileId === undefined ? qsTr("Preview shows what a sync would copy, convert and delete before anything is written.")
                          : !p.ok ? p.error
                          : qsTr("%1 to copy, %2 to convert, %3 already there, %4 to delete, %5 left off. %6 to write; %7 free.")
                              .arg(p.copy).arg(p.convert).arg(p.keep).arg(p["delete"]).arg(p.skip).arg(p.writeText).arg(p.freeText)
                    font.pixelSize: 13
                    font.bold: p.profileId !== undefined
                    color: p.profileId !== undefined && !p.ok ? theme.c.red : theme.c.foreground
                }
                Repeater {
                    model: sheet.devices.preview.warnings || []
                    delegate: Text {
                        required property string modelData
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        text: modelData
                        font.pixelSize: 12
                        color: theme.c.yellow
                    }
                }
                ListView {
                    objectName: "previewList"
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    ScrollBar.vertical: ScrollBar {}
                    model: sheet.devices.preview.rows || []
                    delegate: RowLayout {
                        required property var modelData
                        width: ListView.view.width
                        spacing: 10
                        Text {
                            Layout.preferredWidth: 80
                            text: modelData.opText
                            font.pixelSize: 12
                            font.bold: true
                            color: modelData.op === "delete" ? theme.c.red : modelData.op === "skip" ? theme.c.muted
                                   : modelData.op === "convert" ? theme.c.blue : theme.c.green
                        }
                        Text {
                            Layout.fillWidth: true
                            elide: Text.ElideMiddle
                            text: modelData.target
                            font.pixelSize: 12
                            font.family: "JetBrainsMono Nerd Font"
                            color: theme.c.foreground
                        }
                        Text {
                            text: modelData.detail
                            font.pixelSize: 12
                            color: theme.c.muted
                        }
                    }
                }
                Text {
                    visible: (sheet.devices.preview.more || 0) > 0
                    text: qsTr("…and %n more", "", sheet.devices.preview.more || 0)
                    font.pixelSize: 12
                    color: theme.c.muted
                }
        }
    }

    footerLeft: [
        Row {
            anchors.verticalCenter: parent.verticalCenter
            spacing: 8
            Button {
                objectName: "deleteProfile"
                text: qsTr("Delete profile…")
                font.pixelSize: 13
                enabled: sheet.editId > 0
                onClicked: {
                    const id = sheet.editId, name = sheet.editName
                    sheet.confirm(qsTr("Delete the profile %1?").arg(name),
                                  qsTr("The profile goes; the music already on the device stays where it is."),
                                  qsTr("Delete"), () => { sheet.devices.removeProfile(id); sheet.openSheet(0) })
                }
            }
            Button {
                objectName: "saveProfile"
                text: qsTr("Save")
                font.pixelSize: 13
                enabled: sheet.editDirty
                onClicked: sheet.save()
            }
        }
    ]
    buttons: [
        Button {
            objectName: "previewProfile"
            text: qsTr("Preview")
            font.pixelSize: 13
            enabled: !sheet.devices.previewing && (sheet.editId > 0 || sheet.editDirty)
            onClicked: {
                const id = sheet.editDirty ? sheet.save() : sheet.editId
                if (id > 0) sheet.devices.previewProfile(id)
            }
        },
        PrimaryButton {
            objectName: "syncProfile"
            text: qsTr("Sync now")
            enabled: sheet.editId > 0 || sheet.editDirty
            onClicked: {
                const id = sheet.editDirty ? sheet.save() : sheet.editId
                if (id > 0) sheet.devices.syncProfile(id)
            }
        }
    ]
}
