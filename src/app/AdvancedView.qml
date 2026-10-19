import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Advanced mode: the deck as a strip across the top, the library below it.
//
//   deck strip
//   [Add folder] [Rescan]  [ search ......................... ]  [Columns]
//   tree        | track list (or Now Playing)
//   jobs and status
FocusScope {
    id: view
    property bool showRemaining: false
    property int treeWidth: 250
    property string columns: ""
    signal clockClicked()
    signal layoutEdited()

    readonly property bool nowPlaying: library.currentKind === "nowplaying"

    function focusSearch() { search.forceActiveFocus(); search.selectAll() }
    function focusList() { if (nowPlaying) nowPlayingList.forceActiveFocus(); else table.forceActiveFocus() }
    function focusTree() { tree.forceActiveFocus() }
    function columnState() { return table.columnState() }
    function currentTreeWidth() { return Math.round(tree.width) }
    function jumpToEnd() { table.jumpToEnd() }

    Component.onCompleted: {
        if (columns !== "") table.restoreColumns(columns)
        focusList()
    }

    Rectangle { anchors.fill: parent; color: theme.c.background }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        DeckStrip {
            Layout.fillWidth: true
            Layout.margins: 8
            Layout.bottomMargin: 6
            showRemaining: view.showRemaining
            onClockClicked: view.clockClicked()
        }

        // Library actions and the search.
        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 8
            Layout.rightMargin: 8
            Layout.bottomMargin: 6
            spacing: 6

            Button {
                text: qsTr("Add folder…")
                flat: true
                font.pixelSize: 13
                implicitHeight: 34; topInset: 0; bottomInset: 0
                onClicked: library.addFolder()
            }
            Button {
                text: qsTr("Rescan")
                flat: true
                font.pixelSize: 13
                implicitHeight: 34; topInset: 0; bottomInset: 0
                enabled: !library.busy
                onClicked: library.rescan()
            }
            Item { Layout.preferredWidth: 6 }

            Rectangle {
                Layout.fillWidth: true
                Layout.maximumWidth: 640
                implicitHeight: 32
                radius: Math.min(4, theme.radius)
                color: theme.c.darker_background
                border.width: 1
                border.color: library.tracks.filterError !== "" ? theme.c.red
                            : search.activeFocus ? theme.accent : theme.c.lighter_background

                Text {
                    id: magnifier
                    anchors { left: parent.left; leftMargin: 9; verticalCenter: parent.verticalCenter }
                    text: String.fromCodePoint(0xF0349)
                    font.family: "JetBrainsMono Nerd Font"
                    font.pixelSize: 14
                    color: theme.c.muted
                }
                TextInput {
                    id: search
                    anchors { left: magnifier.right; leftMargin: 8; right: clear.left; rightMargin: 4
                              verticalCenter: parent.verticalCenter }
                    font.pixelSize: 13
                    color: theme.c.foreground
                    selectionColor: theme.c.selection
                    selectedTextColor: theme.c.bright_foreground
                    clip: true
                    onTextChanged: typing.restart()
                    Keys.onPressed: (event) => {
                        if (event.key === Qt.Key_Escape) {
                            if (search.text !== "") search.text = ""
                            else view.focusList()
                            event.accepted = true
                        } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                            typing.stop()
                            library.tracks.filter = search.text
                            view.focusList()
                            event.accepted = true
                        } else if (event.key === Qt.Key_Down) {
                            view.focusList()
                            event.accepted = true
                        }
                    }
                    Text {
                        anchors.fill: parent
                        visible: search.text === ""
                        verticalAlignment: Text.AlignVCenter
                        elide: Text.ElideRight
                        text: qsTr("Search  (/)   words, artist:name, year:1990..1999, rating:4..")
                        font.pixelSize: 13
                        color: theme.c.muted
                    }
                }
                Text {
                    id: clear
                    anchors { right: parent.right; rightMargin: 9; verticalCenter: parent.verticalCenter }
                    visible: search.text !== ""
                    text: "✕"
                    font.pixelSize: 12
                    color: theme.c.muted
                    MouseArea { anchors.fill: parent; anchors.margins: -6; onClicked: search.text = "" }
                }
                // Applies the search shortly after typing stops.
                Timer {
                    id: typing
                    interval: 60
                    onTriggered: library.tracks.filter = search.text
                }
            }

            Text {
                Layout.fillWidth: true
                visible: library.tracks.filterError !== ""
                elide: Text.ElideRight
                text: library.tracks.filterError
                font.pixelSize: 12
                color: theme.c.red
            }
            Item { Layout.fillWidth: library.tracks.filterError === "" }

            Button {
                id: columnsButton
                text: qsTr("Columns")
                flat: true
                font.pixelSize: 13
                implicitHeight: 34; topInset: 0; bottomInset: 0
                visible: !view.nowPlaying
                onClicked: table.openColumns(columnsButton)
            }
        }

        Rectangle { Layout.fillWidth: true; height: 1; color: theme.c.lighter_background }

        SplitView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            orientation: Qt.Horizontal

            handle: Rectangle {
                implicitWidth: 5
                color: SplitHandle.pressed || SplitHandle.hovered ? theme.c.selection : theme.c.lighter_background
                Rectangle { anchors.centerIn: parent; width: 1; height: parent.height; color: theme.c.lighter_background }
            }

            LibraryTreePane {
                id: tree
                SplitView.preferredWidth: view.treeWidth
                SplitView.minimumWidth: 150
                onLeave: view.focusList()
                onWidthChanged: view.layoutEdited()
            }

            Item {
                SplitView.fillWidth: true
                SplitView.minimumWidth: 300
                TrackTable {
                    id: table
                    anchors.fill: parent
                    visible: !view.nowPlaying
                    onLeave: view.focusTree()
                    onLayoutEdited: view.layoutEdited()
                }
                NowPlayingList {
                    id: nowPlayingList
                    anchors.fill: parent
                    visible: view.nowPlaying
                    onLeave: view.focusTree()
                }
            }
        }

        JobsBar {
            Layout.fillWidth: true
        }
    }
}
