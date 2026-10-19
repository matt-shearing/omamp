import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Undo history: every tag edit, tag from file names and organise, newest
// first, with what each changed. Any of them can be undone from here;
// Ctrl+Z always takes the newest.
ToolSheet {
    id: sheet
    objectName: "undoHistory"
    title: qsTr("Undo history")
    preferredWidth: 1100
    preferredHeight: 700

    property var batches: []
    property var entries: []
    property int chosen: -1

    function reload() {
        const keep = chosen >= 0 && batches[chosen] ? batches[chosen].batch : ""
        batches = library.journalBatches(200)
        chosen = -1
        for (let i = 0; i < batches.length; ++i) if (batches[i].batch === keep) chosen = i
        if (chosen < 0 && batches.length > 0) chosen = 0
        entries = chosen >= 0 ? library.journalEntries(batches[chosen].batch) : []
        list.currentIndex = chosen
    }
    function choose(i) {
        chosen = i
        entries = i >= 0 ? library.journalEntries(batches[i].batch) : []
    }
    onOpened: { reload(); list.forceActiveFocus() }

    // An undo or redo finished: show the new state.
    Connections {
        target: library
        function onUndoChanged() { if (sheet.opened) sheet.reload() }
        function onJobFinished(kind, result) { if (sheet.opened) sheet.reload() }
    }

    RowLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 16

        // The batches.
        Rectangle {
            Layout.preferredWidth: 430
            Layout.fillHeight: true
            color: theme.c.darker_background
            border.width: 1
            border.color: theme.c.lighter_background
            radius: Math.min(4, theme.radius)
            ListView {
                id: list
                objectName: "undoBatches"
                anchors.fill: parent
                anchors.margins: 3
                clip: true
                model: sheet.batches
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                Keys.onPressed: (event) => {
                    if (event.key === Qt.Key_Down) { sheet.choose(Math.min(count - 1, currentIndex + 1)); currentIndex = sheet.chosen; event.accepted = true }
                    else if (event.key === Qt.Key_Up) { sheet.choose(Math.max(0, currentIndex - 1)); currentIndex = sheet.chosen; event.accepted = true }
                }
                delegate: Rectangle {
                    required property var modelData
                    required property int index
                    width: ListView.view.width
                    height: 46
                    radius: Math.min(3, theme.radius)
                    color: index === sheet.chosen ? theme.listSelection : "transparent"
                    Column {
                        anchors { left: parent.left; right: parent.right; verticalCenter: parent.verticalCenter; leftMargin: 10; rightMargin: 10 }
                        spacing: 2
                        Text {
                            width: parent.width
                            elide: Text.ElideRight
                            text: modelData.label
                            font.pixelSize: 13
                            font.strikeout: modelData.undone
                            color: modelData.undone ? theme.c.muted : theme.c.bright_foreground
                        }
                        Text {
                            width: parent.width
                            elide: Text.ElideRight
                            text: modelData.when + (modelData.undone ? "   ·   " + qsTr("undone") : "")
                            font.pixelSize: 11
                            color: theme.c.muted
                        }
                    }
                    MouseArea { anchors.fill: parent; onClicked: { sheet.choose(index); list.currentIndex = index; list.forceActiveFocus() } }
                }
                Text {
                    anchors.centerIn: parent
                    visible: sheet.batches.length === 0
                    text: qsTr("Nothing has been changed yet.")
                    font.pixelSize: 13
                    color: theme.c.muted
                }
            }
        }

        // The chosen batch's changes.
        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 8
            Text {
                Layout.fillWidth: true
                elide: Text.ElideRight
                text: sheet.chosen >= 0 ? sheet.batches[sheet.chosen].label : ""
                font.pixelSize: 15
                font.bold: true
                color: theme.c.bright_foreground
            }
            ListView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                model: sheet.entries
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                delegate: Item {
                    required property var modelData
                    width: ListView.view.width
                    height: change.implicitHeight + 10
                    Column {
                        id: change
                        width: parent.width - 12
                        spacing: 1
                        Text {
                            width: parent.width
                            elide: Text.ElideRight
                            text: modelData.title + "  ·  " + modelData.field
                            font.pixelSize: 12
                            color: modelData.undone ? theme.c.muted : theme.c.light_foreground
                        }
                        Text {
                            width: parent.width
                            elide: Text.ElideMiddle
                            text: (modelData.oldValue === "" ? qsTr("(empty)") : modelData.oldValue) + "   →   "
                                  + (modelData.newValue === "" ? qsTr("(empty)") : modelData.newValue)
                            font.pixelSize: 13
                            font.strikeout: modelData.undone
                            color: modelData.undone ? theme.c.muted : theme.c.foreground
                        }
                    }
                }
            }
        }
    }

    footerLeft: Text {
        anchors.verticalCenter: parent.verticalCenter
        width: parent.width
        elide: Text.ElideRight
        text: library.undoText !== "" ? qsTr("Ctrl+Z undoes: %1").arg(library.undoText) : ""
        font.pixelSize: 12
        color: theme.c.light_foreground
    }
    buttons: [
        Button { text: qsTr("Close"); flat: true; onClicked: sheet.close() },
        PrimaryButton {
            objectName: "undoChosen"
            text: qsTr("Undo this change")
            highlighted: true
            enabled: sheet.chosen >= 0 && sheet.batches[sheet.chosen].canUndo && !library.busy
            onClicked: library.undoBatch(sheet.batches[sheet.chosen].batch)
        }
    ]
}
