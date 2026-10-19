import QtQuick
import QtQuick.Controls

// Now Playing in advanced mode: the deck's own playlist, in the library's
// plain style. Up and Down move, Enter or a double-click plays, Delete
// removes, Left goes back to the tree.
FocusScope {
    id: pane
    signal leave()
    signal openBigView()

    function clock(ms) {
        const total = Math.floor(ms / 1000)
        const h = Math.floor(total / 3600), m = Math.floor(total % 3600 / 60), s = total % 60
        return (h > 0 ? h + ":" + (m < 10 ? "0" : "") : "") + m + ":" + (s < 10 ? "0" : "") + s
    }

    Rectangle { anchors.fill: parent; color: theme.c.background }

    // What is playing, with its cover; the button opens the big view.
    Rectangle {
        id: banner
        objectName: "nowPlayingBanner"
        anchors { left: parent.left; right: parent.right; top: parent.top }
        height: 76
        color: theme.c.dark_background
        Rectangle { anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
                    height: 1; color: theme.c.lighter_background }

        CoverArt {
            id: bannerCover
            objectName: "nowPlayingCover"
            anchors { left: parent.left; leftMargin: 10; verticalCenter: parent.verticalCenter }
            width: 60
            height: 60
            source: nowPlayingArt.thumbUrl
            onClicked: pane.openBigView()
        }
        Column {
            anchors { left: bannerCover.right; leftMargin: 12; right: bigButton.left; rightMargin: 12
                      verticalCenter: parent.verticalCenter }
            spacing: 2
            Text {
                width: parent.width
                elide: Text.ElideRight
                text: backend.currentIndex < 0 ? qsTr("Nothing playing") : backend.title
                font.pixelSize: 15
                font.bold: true
                color: theme.c.bright_foreground
            }
            Text {
                width: parent.width
                elide: Text.ElideRight
                visible: text !== ""
                text: backend.artist + (backend.album ? (backend.artist ? "  —  " : "") + backend.album : "")
                font.pixelSize: 12
                color: theme.c.light_foreground
            }
            Text {
                width: parent.width
                elide: Text.ElideRight
                text: backend.playlist.count === 0 ? ""
                      : qsTr("%n tracks", "", backend.playlist.count)
                        + (backend.playlist.totalKnownMs > 0 ? "  ·  " + pane.clock(backend.playlist.totalKnownMs) : "")
                font.pixelSize: 11
                color: theme.c.muted
            }
        }
        Button {
            id: bigButton
            objectName: "nowPlayingBigView"
            anchors { right: parent.right; rightMargin: 10; verticalCenter: parent.verticalCenter }
            text: qsTr("Big view")
            font.pixelSize: 12
            flat: true
            onClicked: pane.openBigView()
            ToolTip.visible: hovered
            ToolTip.delay: 600
            ToolTip.text: qsTr("Big Now Playing view (N)")
        }
    }

    Rectangle {
        id: header
        anchors { left: parent.left; right: parent.right; top: banner.bottom }
        height: 28
        color: theme.c.dark_background
        Rectangle { anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
                    height: 1; color: theme.c.lighter_background }
        Row {
            anchors.fill: parent
            Repeater {
                model: [[qsTr("#"), 48], [qsTr("Title"), 0.5], [qsTr("Artist"), 0.5], [qsTr("Length"), 70]]
                Text {
                    required property var modelData
                    width: modelData[1] > 1 ? modelData[1] : (pane.width - 118) * modelData[1]
                    height: header.height
                    leftPadding: 8; rightPadding: 8
                    verticalAlignment: Text.AlignVCenter
                    horizontalAlignment: modelData[1] > 1 ? Text.AlignRight : Text.AlignLeft
                    text: modelData[0]
                    font.pixelSize: 12
                    color: theme.c.light_foreground
                }
            }
        }
    }

    ListView {
        id: list
        anchors { left: parent.left; right: parent.right; top: header.bottom; bottom: parent.bottom }
        clip: true
        focus: true
        model: backend.playlist
        currentIndex: backend.currentIndex
        keyNavigationEnabled: false
        highlightMoveDuration: 0
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

        Keys.onPressed: (event) => {
            switch (event.key) {
            case Qt.Key_Up: list.currentIndex = Math.max(0, list.currentIndex - 1); break
            case Qt.Key_Down: list.currentIndex = Math.min(list.count - 1, list.currentIndex + 1); break
            case Qt.Key_Return:
            case Qt.Key_Enter: backend.playIndex(list.currentIndex); break
            case Qt.Key_Delete: backend.remove(list.currentIndex); break
            case Qt.Key_Left: pane.leave(); break
            default: return
            }
            list.positionViewAtIndex(list.currentIndex, ListView.Contain)
            event.accepted = true
        }

        delegate: Rectangle {
            id: item
            required property int index
            required property int number
            required property string title
            required property string artist
            required property var duration
            readonly property bool playing: index === backend.currentIndex
            width: ListView.view.width
            height: 24
            color: index === list.currentIndex ? (list.activeFocus ? theme.listSelection : Qt.alpha(theme.listSelection, 0.6))
                 : index % 2 ? Qt.alpha(theme.c.lighter_background, 0.35) : "transparent"
            Row {
                anchors.fill: parent
                Repeater {
                    model: [[item.number, 48, true], [item.title, 0.5, false], [item.artist, 0.5, false],
                            [item.duration > 0 ? pane.clock(item.duration) : "", 70, true]]
                    Text {
                        required property var modelData
                        width: modelData[1] > 1 ? modelData[1] : (pane.width - 118) * modelData[1]
                        height: item.height
                        leftPadding: 8; rightPadding: 8
                        verticalAlignment: Text.AlignVCenter
                        horizontalAlignment: modelData[2] ? Text.AlignRight : Text.AlignLeft
                        elide: Text.ElideRight
                        text: modelData[0]
                        font.pixelSize: 13
                        font.bold: item.playing
                        color: item.playing ? theme.accent : theme.c.foreground
                    }
                }
            }
            MouseArea {
                anchors.fill: parent
                onPressed: { list.forceActiveFocus(); list.currentIndex = item.index }
                onDoubleClicked: backend.playIndex(item.index)
            }
        }
    }

    Text {
        anchors.centerIn: list
        visible: list.count === 0
        text: qsTr("Nothing in Now Playing. Pick tracks in the library and press Enter.")
        font.pixelSize: 13
        color: theme.c.muted
    }
}
