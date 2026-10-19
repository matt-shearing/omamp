import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Organise files (Ctrl+Shift+O): rename and move the chosen tracks' files
// by a mask, with a preview of every old and new path before anything
// moves. A new path can be changed by hand (double-click it, or F2). One
// job moves them all; Undo, or Ctrl+Z later, puts them back.
ToolSheet {
    id: sheet
    objectName: "organiseSheet"
    readonly property var org: library.organiser
    title: qsTr("Organise files")
    subtitle: qsTr("%n tracks", "", org.count)
    preferredWidth: 1180
    preferredHeight: 760
    onAccepted: if (applyButton.enabled) org.apply()
    onClosed: org.close()

    function openFor(ids) {
        if (!org.load(ids)) return false
        open()
        maskField.focusInput()
        return true
    }

    // Completing a field or function name in the mask.
    property var completions: []
    property int completionIndex: 0
    function updateCompletions() {
        completions = maskField.inputFocused ? org.completions(maskField.input.text, maskField.input.cursorPosition) : []
        completionIndex = 0
    }
    function complete(i) {
        const c = completions[i]
        if (!c) return
        const t = maskField.input.text, cursor = maskField.input.cursorPosition
        const text = t.slice(0, c.start) + c.insert + t.slice(cursor)
        org.mask = text
        maskField.input.text = text
        maskField.input.cursorPosition = c.start + c.insert.length
        completions = []
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 10

        // The mask, with presets.
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            FormField {
                id: maskField
                objectName: "organiseMask"
                Layout.fillWidth: true
                label: qsTr("Mask")
                monospace: true
                clearable: false
                text: sheet.org.mask
                tip: qsTr("Fields in angle brackets, functions after $. Type < or $ for a list.")
                onEdit: (t) => { sheet.org.mask = t; sheet.updateCompletions() }
                keyHandler: (event) => {
                    event.accepted = false
                    if (sheet.completions.length === 0) return
                    if (event.key === Qt.Key_Down) { sheet.completionIndex = Math.min(sheet.completions.length - 1, sheet.completionIndex + 1); event.accepted = true }
                    else if (event.key === Qt.Key_Up) { sheet.completionIndex = Math.max(0, sheet.completionIndex - 1); event.accepted = true }
                    else if (event.key === Qt.Key_Tab || event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                        sheet.complete(sheet.completionIndex); event.accepted = true
                    } else if (event.key === Qt.Key_Escape) { sheet.completions = []; event.accepted = true }
                }
                Connections {
                    target: maskField.input
                    function onCursorPositionChanged() { sheet.updateCompletions() }
                    function onActiveFocusChanged() { if (!maskField.input.activeFocus) sheet.completions = [] }
                }
            }
            ComboBox {
                id: presets
                Layout.preferredWidth: 300
                implicitHeight: 34
                topInset: 0; bottomInset: 0
                font.pixelSize: 13
                model: sheet.org.presets
                textRole: "name"
                displayText: qsTr("Presets")
                onActivated: (i) => { sheet.org.mask = sheet.org.presets[i].mask; sheet.org.replan() }
            }
        }

        // Where the mask is wrong, said plainly.
        Text {
            objectName: "organiseMaskError"
            Layout.fillWidth: true
            Layout.leftMargin: 122
            visible: sheet.org.maskError !== ""
            wrapMode: Text.Wrap
            text: sheet.org.maskErrorPosition >= 0
                  ? qsTr("%1, at character %2").arg(sheet.org.maskError).arg(sheet.org.maskErrorPosition + 1)
                  : sheet.org.maskError
            font.pixelSize: 12
            color: theme.c.red
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            FormField {
                Layout.fillWidth: true
                label: qsTr("Destination")
                readOnly: true
                clearable: false
                text: sheet.org.destination
                placeholder: qsTr("Each track's own library folder")
            }
            Button { text: qsTr("Choose…"); onClicked: sheet.org.pickDestination() }
            Button { text: qsTr("Library folders"); flat: true; enabled: sheet.org.destination !== ""; onClicked: sheet.org.destination = "" }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 116
            spacing: 18
            CheckBox {
                text: qsTr("Move pictures, cue sheets and logs with their folder")
                checked: sheet.org.moveCompanions
                onToggled: sheet.org.moveCompanions = checked
            }
            CheckBox {
                text: qsTr("Remove folders left empty")
                checked: sheet.org.removeEmptyFolders
                onToggled: sheet.org.removeEmptyFolders = checked
            }
        }

        // What will happen.
        Text {
            objectName: "organiseSummary"
            Layout.fillWidth: true
            text: {
                const o = sheet.org
                const parts = [qsTr("%n to move", "", o.moves), qsTr("%n already in place", "", o.unchanged)]
                if (o.conflicts > 0) parts.push(qsTr("%n conflicts", "", o.conflicts))
                if (o.problems > 0) parts.push(qsTr("%n can't be moved", "", o.problems))
                if (o.companions > 0) parts.push(qsTr("%n other files go along", "", o.companions))
                return parts.join("   ·   ")
            }
            font.pixelSize: 13
            color: theme.c.light_foreground
        }
        Text {
            Layout.fillWidth: true
            visible: sheet.org.commonRoot !== ""
            elide: Text.ElideMiddle
            text: qsTr("Paths below are in %1").arg(sheet.org.commonRoot)
            font.pixelSize: 12
            color: theme.c.muted
        }

        // The preview: status, current place, new place.
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: theme.c.darker_background
            border.width: 1
            border.color: theme.c.lighter_background
            radius: Math.min(4, theme.radius)
            clip: true

            readonly property int statusWidth: 130
            readonly property int pathWidth: (width - statusWidth - 24) / 2

            Rectangle {
                id: previewHeader
                anchors { left: parent.left; right: parent.right; top: parent.top; margins: 1 }
                height: 28
                color: theme.c.dark_background
                Row {
                    anchors.fill: parent
                    anchors.leftMargin: 10
                    Repeater {
                        model: [[qsTr("What happens"), 130], [qsTr("Now"), (previewHeader.width - 154) / 2], [qsTr("New place  (double-click to change)"), (previewHeader.width - 154) / 2]]
                        Text {
                            required property var modelData
                            width: modelData[1]
                            height: 28
                            verticalAlignment: Text.AlignVCenter
                            text: modelData[0]
                            elide: Text.ElideRight
                            font.pixelSize: 12
                            font.bold: true
                            color: theme.c.light_foreground
                        }
                    }
                }
            }

            ListView {
                id: preview
                objectName: "organisePreview"
                anchors { left: parent.left; right: parent.right; top: previewHeader.bottom; bottom: parent.bottom; margins: 1 }
                clip: true
                model: sheet.org
                boundsBehavior: Flickable.StopAtBounds
                currentIndex: -1
                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                Keys.onPressed: (event) => {
                    if (event.key === Qt.Key_Down) { currentIndex = Math.min(count - 1, currentIndex + 1); event.accepted = true }
                    else if (event.key === Qt.Key_Up) { currentIndex = Math.max(0, currentIndex - 1); event.accepted = true }
                    else if (event.key === Qt.Key_F2 && currentItem) { currentItem.startEdit(); event.accepted = true }
                }

                delegate: Rectangle {
                    id: row
                    required property int index
                    required property string oldPath
                    required property string newPath
                    required property string oldShown
                    required property string newShown
                    required property string status
                    required property string statusText
                    required property string note
                    required property bool edited
                    width: ListView.view.width
                    height: 26
                    color: ListView.isCurrentItem && preview.activeFocus ? Qt.alpha(theme.listSelection, 0.8)
                         : index % 2 ? Qt.alpha(theme.c.lighter_background, 0.3) : "transparent"
                    readonly property color statusColour: status === "conflict" || status === "error" || status === "failed" ? theme.c.red
                                                        : status === "renamed" || status === "offline" || status === "missing" ? theme.c.yellow
                                                        : status === "move" || status === "moved" ? theme.c.green
                                                        : theme.c.muted
                    function startEdit() {
                        if (sheet.org.applied || !(status === "move" || status === "renamed" || status === "unchanged" || status === "conflict")) return
                        editor.text = newPath
                        editor.visible = true
                        editor.forceActiveFocus()
                        editor.selectAll()
                    }

                    Row {
                        anchors.fill: parent
                        anchors.leftMargin: 10
                        Text {
                            width: 130; height: parent.height
                            verticalAlignment: Text.AlignVCenter
                            text: row.statusText
                            elide: Text.ElideRight
                            font.pixelSize: 12
                            font.bold: row.status === "conflict"
                            color: row.statusColour
                        }
                        Text {
                            width: (preview.width - 154) / 2; height: parent.height
                            verticalAlignment: Text.AlignVCenter
                            rightPadding: 12
                            text: row.oldShown
                            elide: Text.ElideLeft
                            font.pixelSize: 12
                            color: theme.c.foreground
                        }
                        Item {
                            width: (preview.width - 154) / 2; height: parent.height
                            Text {
                                anchors.fill: parent
                                visible: !editor.visible
                                verticalAlignment: Text.AlignVCenter
                                text: row.status === "unchanged" ? qsTr("(stays)") : row.newShown
                                elide: Text.ElideLeft
                                font.pixelSize: 12
                                font.italic: row.edited
                                color: row.status === "unchanged" ? theme.c.muted : theme.c.bright_foreground
                            }
                            TextInput {
                                id: editor
                                anchors.fill: parent
                                visible: false
                                verticalAlignment: Text.AlignVCenter
                                font.pixelSize: 12
                                color: theme.c.bright_foreground
                                selectionColor: theme.c.selection
                                clip: true
                                onEditingFinished: if (visible) { visible = false; sheet.org.setNewPath(row.index, text); preview.forceActiveFocus() }
                                Keys.onEscapePressed: (event) => { visible = false; preview.forceActiveFocus(); event.accepted = true }
                                Rectangle { anchors.fill: parent; z: -1; color: theme.c.background; border.width: 1; border.color: theme.accent }
                            }
                        }
                    }
                    HoverHandler { id: rowHover }
                    ToolTip.visible: rowHover.hovered && row.note !== ""
                    ToolTip.delay: 500
                    ToolTip.text: row.note
                    TapHandler {
                        onTapped: { preview.currentIndex = row.index; preview.forceActiveFocus() }
                        onDoubleTapped: { preview.currentIndex = row.index; row.startEdit() }
                    }
                }
            }
        }
    }

    // Field and function names for the mask, under the field.
    Rectangle {
        parent: sheet.contentItem
        visible: sheet.completions.length > 0
        x: maskField.mapToItem(sheet.contentItem, 122, 0).x
        y: maskField.mapToItem(sheet.contentItem, 0, maskField.height + 2).y
        width: 300
        height: Math.min(8, sheet.completions.length) * 26 + 6
        z: 10
        color: theme.c.dark_background
        border.width: 1
        border.color: theme.c.lighter_background
        radius: Math.min(4, theme.radius)
        ListView {
            anchors.fill: parent
            anchors.margins: 3
            clip: true
            model: sheet.completions
            currentIndex: sheet.completionIndex
            delegate: Rectangle {
                required property var modelData
                required property int index
                width: ListView.view.width
                height: 26
                color: index === sheet.completionIndex ? theme.listSelection : "transparent"
                Text {
                    anchors { fill: parent; leftMargin: 8 }
                    verticalAlignment: Text.AlignVCenter
                    text: modelData.label
                    font.family: "JetBrainsMono Nerd Font"
                    font.pixelSize: 12
                    color: theme.c.foreground
                }
                MouseArea { anchors.fill: parent; onClicked: sheet.complete(index) }
            }
        }
    }

    footerLeft: Text {
        objectName: "organiseResult"
        anchors.verticalCenter: parent.verticalCenter
        width: parent.width
        elide: Text.ElideRight
        text: sheet.org.applying ? qsTr("Moving files…  (the jobs bar shows progress)")
              : sheet.org.resultText !== "" ? sheet.org.resultText
              : qsTr("Nothing moves until you press Organise. Existing files are never replaced.")
        font.pixelSize: 12
        color: theme.c.light_foreground
    }
    buttons: [
        Button {
            objectName: "organiseUndo"
            text: qsTr("Undo")
            flat: true
            visible: sheet.org.applied && sheet.org.lastBatch !== ""
            onClicked: sheet.org.undo()
        },
        Button {
            text: sheet.org.applied ? qsTr("Close") : qsTr("Cancel")
            flat: true
            onClicked: sheet.close()
        },
        PrimaryButton {
            id: applyButton
            objectName: "organiseApply"
            text: qsTr("Organise %n files", "", sheet.org.moves)
            highlighted: true
            visible: !sheet.org.applied
            enabled: sheet.org.moves > 0 && sheet.org.maskError === "" && !sheet.org.applying
            onClicked: sheet.org.apply()
        }
    ]
}
