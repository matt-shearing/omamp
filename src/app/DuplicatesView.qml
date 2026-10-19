import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Files to Edit > Duplicates: the same song more than once. Choose how to
// compare (tags, the files' bytes, or how they sound), press Find, and
// each group lists the suggested keeper first with the reason. Tick tracks
// (or "Tick all but the keepers") and take them out of the library, or move
// their files to the trash. Nothing is ever deleted outright.
FocusScope {
    id: view
    objectName: "duplicatesView"
    readonly property var dup: library.duplicates
    signal leave()
    signal confirm(string title, string text, string confirmText, var action)

    Rectangle { anchors.fill: parent; color: theme.c.background }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // How to compare, and Find.
        RowLayout {
            Layout.fillWidth: true
            Layout.margins: 10
            spacing: 6
            Text { text: qsTr("Find the same song by"); font.pixelSize: 13; color: theme.c.light_foreground }
            Repeater {
                model: [
                    { by: "tags", label: qsTr("Artist and title"), tip: qsTr("Same artist and title, lengths within two seconds") },
                    { by: "content", label: qsTr("Identical files"), tip: qsTr("Byte-for-byte copies of the same file") },
                    { by: "fingerprint", label: qsTr("Sound"), tip: qsTr("The same recording in any format, by audio fingerprint (needs fpcalc; slow the first time)") }
                ]
                RadioButton {
                    required property var modelData
                    text: modelData.label
                    font.pixelSize: 13
                    checked: view.dup.by === modelData.by
                    enabled: modelData.by !== "fingerprint" || view.dup.fingerprintAvailable
                    onToggled: if (checked) view.dup.by = modelData.by
                    ToolTip.visible: hovered
                    ToolTip.delay: 600
                    ToolTip.text: modelData.by === "fingerprint" && !view.dup.fingerprintAvailable
                                  ? qsTr("Install the chromaprint package for fpcalc") : modelData.tip
                }
            }
            PrimaryButton {
                objectName: "duplicatesFind"
                text: view.dup.busy ? qsTr("Looking…") : qsTr("Find duplicates")
                highlighted: true
                enabled: !view.dup.busy && !library.busy
                onClicked: view.dup.find()
            }
            Item { Layout.fillWidth: true }
        }

        Text {
            Layout.fillWidth: true
            Layout.leftMargin: 12
            Layout.bottomMargin: 6
            text: !view.dup.searched ? qsTr("Nothing searched yet. Nothing is changed until you choose an action below.")
                                     : view.dup.summary + (view.dup.commonRoot !== "" ? "   ·   " + qsTr("paths below are in %1").arg(view.dup.commonRoot) : "")
            elide: Text.ElideMiddle
            font.pixelSize: 12
            color: theme.c.light_foreground
        }
        Rectangle { Layout.fillWidth: true; height: 1; color: theme.c.lighter_background }

        ListView {
            id: list
            objectName: "duplicatesList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            focus: true
            model: view.dup
            currentIndex: -1
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
            Keys.onPressed: (event) => {
                switch (event.key) {
                case Qt.Key_Down: currentIndex = Math.min(count - 1, currentIndex + 1); break
                case Qt.Key_Up: currentIndex = Math.max(0, currentIndex - 1); break
                case Qt.Key_Left: view.leave(); break
                case Qt.Key_Space:
                    if (!(event.modifiers & Qt.ControlModifier)) return
                    view.dup.toggle(currentIndex)
                    break
                default: return
                }
                positionViewAtIndex(currentIndex, ListView.Contain)
                event.accepted = true
            }

            delegate: Rectangle {
                id: row
                required property int index
                required property string kind
                required property string label
                required property string detail
                required property string reason
                required property string path
                required property string format
                required property string length
                required property int plays
                required property bool keeper
                required property bool rowSelected
                required property bool available
                readonly property bool heading: kind === "group"
                width: ListView.view.width
                height: heading ? 40 : 30
                color: heading ? theme.c.dark_background
                     : rowSelected ? Qt.alpha(theme.c.red, 0.16)
                     : keeper ? Qt.alpha(theme.c.green, 0.10) : "transparent"
                border.width: list.currentIndex === index && list.activeFocus ? 1 : 0
                border.color: theme.accent

                // A group's heading: the song, how many copies, and why the
                // first is the one to keep.
                RowLayout {
                    visible: row.heading
                    anchors { fill: parent; leftMargin: 12; rightMargin: 12; topMargin: 6 }
                    spacing: 10
                    Text {
                        text: row.label
                        font.pixelSize: 13
                        font.bold: true
                        color: theme.c.bright_foreground
                        elide: Text.ElideRight
                        Layout.maximumWidth: row.width * 0.5
                    }
                    Text { text: row.detail; font.pixelSize: 12; color: theme.c.muted }
                    Item { Layout.fillWidth: true }
                    Text {
                        text: row.reason !== "" ? qsTr("Keep the first: %1").arg(row.reason) : ""
                        font.pixelSize: 12
                        color: theme.c.green
                        elide: Text.ElideRight
                        Layout.maximumWidth: row.width * 0.35
                    }
                }

                // A track: its tick, keeper mark, format, length and path.
                RowLayout {
                    visible: !row.heading
                    anchors { fill: parent; leftMargin: 6; rightMargin: 12 }
                    spacing: 8
                    CheckBox {
                        objectName: "duplicateTick"
                        checked: row.rowSelected
                        focusPolicy: Qt.NoFocus
                        implicitHeight: 28
                        topInset: 0; bottomInset: 0
                        onClicked: view.dup.setSelected(row.index, checked)
                    }
                    Text {
                        Layout.preferredWidth: 52
                        text: row.keeper ? qsTr("Keeper") : ""
                        font.pixelSize: 11
                        font.bold: true
                        color: theme.c.green
                    }
                    Text {
                        Layout.preferredWidth: 150
                        text: row.format
                        elide: Text.ElideRight
                        font.pixelSize: 12
                        color: theme.c.foreground
                    }
                    Text {
                        Layout.preferredWidth: 50
                        horizontalAlignment: Text.AlignRight
                        text: row.length
                        font.pixelSize: 12
                        color: theme.c.foreground
                    }
                    Text {
                        Layout.preferredWidth: 70
                        horizontalAlignment: Text.AlignRight
                        text: row.plays > 0 ? qsTr("%n plays", "", row.plays) : ""
                        font.pixelSize: 12
                        color: theme.c.muted
                    }
                    Text {
                        Layout.fillWidth: true
                        leftPadding: 10
                        text: row.available ? row.path : qsTr("%1 (offline)").arg(row.path)
                        elide: Text.ElideLeft
                        font.pixelSize: 12
                        color: row.available ? theme.c.foreground : theme.c.muted
                    }
                }
                MouseArea {
                    anchors.fill: parent
                    anchors.leftMargin: row.heading ? 0 : 40
                    onClicked: { list.currentIndex = row.index; list.forceActiveFocus() }
                    onDoubleClicked: if (!row.heading) view.dup.toggle(row.index)
                }
            }

            Text {
                anchors.centerIn: parent
                width: parent.width - 80
                visible: view.dup.searched && list.count === 0
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                text: qsTr("No duplicates found this way.")
                font.pixelSize: 13
                color: theme.c.muted
            }
        }

        Rectangle { Layout.fillWidth: true; height: 1; color: theme.c.lighter_background }
        // The actions, on the ticked tracks only.
        RowLayout {
            Layout.fillWidth: true
            Layout.margins: 8
            spacing: 6
            Button {
                objectName: "duplicatesTickNonKeepers"
                text: qsTr("Tick all but the keepers")
                flat: true
                enabled: view.dup.groupCount > 0
                onClicked: view.dup.selectNonKeepers()
            }
            Button {
                text: qsTr("Untick all")
                flat: true
                enabled: view.dup.selectedCount > 0
                onClicked: view.dup.clearSelection()
            }
            Item { Layout.fillWidth: true }
            Text {
                text: qsTr("%n ticked", "", view.dup.selectedCount)
                font.pixelSize: 12
                color: theme.c.light_foreground
            }
            Button {
                objectName: "duplicatesRemove"
                text: qsTr("Remove from library…")
                enabled: view.dup.selectedCount > 0 && !library.busy
                onClicked: {
                    const ids = view.dup.selectedIds()
                    view.confirm(qsTr("Remove from the library?"),
                                 qsTr("%n tracks will be taken out of the library. Their files stay on disk, and a rescan of their folders adds them back.", "", ids.length),
                                 qsTr("Remove %n tracks", "", ids.length),
                                 () => library.removeFromLibrary(ids))
                }
            }
            Button {
                objectName: "duplicatesTrash"
                text: qsTr("Move to trash…")
                enabled: view.dup.selectedCount > 0 && !library.busy
                onClicked: {
                    const ids = view.dup.selectedIds()
                    view.confirm(qsTr("Move the files to the trash?"),
                                 qsTr("%n files will go to the desktop's trash and leave the library. You can restore them from the trash; nothing is deleted.", "", ids.length),
                                 qsTr("Move %n files to the trash", "", ids.length),
                                 () => library.moveToTrash(ids))
                }
            }
        }
    }
}
