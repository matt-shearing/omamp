import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// The album-art grid: a second view of the list, one tile per album.
// Click chooses an album (the info panel shows it); double-click or Enter
// plays it; Ctrl+Enter or "Show tracks" opens it in the tree and the list.
// Covers load as tiles come into view, and fill in as the background job
// makes them.
FocusScope {
    id: pane
    objectName: "albumGrid"
    property var albums: library.albumGrid
    // Tile width in pixels; the covers come at 96 px up to 130, else 256.
    property int tileSize: 168
    signal leave()          // to the tree
    signal openAlbum(var albumId)

    readonly property int coverSize: tileSize <= 130 ? 96 : 256

    function show(row) { if (row >= 0) grid.positionViewAtIndex(row, GridView.Contain) }

    Rectangle { anchors.fill: parent; color: theme.c.background }

    GridView {
        id: grid
        objectName: "albumGridView"
        anchors.fill: parent
        anchors.leftMargin: 8
        anchors.topMargin: 8
        clip: true
        focus: true
        model: pane.albums
        reuseItems: true
        cacheBuffer: 400
        keyNavigationEnabled: false
        highlightMoveDuration: 0
        boundsBehavior: Flickable.StopAtBounds
        // As many tiles as fit; the spare width is shared out.
        readonly property int columns: Math.max(1, Math.floor((width - 4) / pane.tileSize))
        cellWidth: Math.floor((width - 4) / columns)
        cellHeight: cellWidth - 16 + 52
        currentIndex: pane.albums.currentRow
        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

        function choose(row) {
            if (count === 0) return
            pane.albums.currentRow = Math.max(0, Math.min(count - 1, row))
            positionViewAtIndex(pane.albums.currentRow, GridView.Contain)
        }

        Keys.onPressed: (event) => {
            const row = pane.albums.currentRow
            const rowsPerPage = Math.max(1, Math.floor(height / cellHeight))
            switch (event.key) {
            case Qt.Key_Left:
                if (row <= 0 || row % columns === 0) pane.leave(); else choose(row - 1)
                break
            case Qt.Key_Right: choose(row < 0 ? 0 : row + 1); break
            case Qt.Key_Up: choose(row < 0 ? 0 : row - columns); break
            case Qt.Key_Down: choose(row < 0 ? 0 : row + columns); break
            case Qt.Key_PageUp: choose(row - columns * rowsPerPage); break
            case Qt.Key_PageDown: choose(row + columns * rowsPerPage); break
            case Qt.Key_Home: choose(0); break
            case Qt.Key_End: choose(count - 1); break
            case Qt.Key_Return:
            case Qt.Key_Enter:
                if (row < 0) break
                if (event.modifiers & Qt.ControlModifier) pane.openAlbum(pane.albums.albumId(row))
                else library.playAlbum(pane.albums.albumId(row))
                break
            case Qt.Key_Menu:
                if (row >= 0) menu.popup(pane, pane.width / 3, pane.height / 3)
                break
            default: return
            }
            event.accepted = true
        }

        delegate: Item {
            id: tile
            required property int index
            required property var albumId
            required property string title
            required property string artist
            required property int year
            required property int artState
            readonly property bool chosen: index === pane.albums.currentRow
            width: grid.cellWidth
            height: grid.cellHeight

            Rectangle {
                anchors.fill: parent
                anchors.rightMargin: 8
                anchors.bottomMargin: 8
                radius: Math.min(4, theme.radius)
                color: tile.chosen ? (grid.activeFocus ? theme.listSelection : Qt.alpha(theme.listSelection, 0.6))
                     : hover.hovered ? Qt.alpha(theme.listSelection, 0.25) : "transparent"
            }
            HoverHandler { id: hover }

            Rectangle {
                id: frame
                x: 6; y: 6
                width: grid.cellWidth - 20
                height: width
                color: theme.c.darker_background
                border.width: 1
                border.color: theme.c.lighter_background
                radius: Math.min(4, theme.radius)
                Image {
                    id: cover
                    anchors.fill: parent
                    anchors.margins: 1
                    asynchronous: true
                    fillMode: Image.PreserveAspectCrop
                    sourceSize: Qt.size(pane.coverSize, pane.coverSize)
                    source: tile.artState === 2 ? "" : "image://omamp-art/album/" + tile.albumId + "/" + pane.coverSize + "?v=" + library.revision
                }
                // No picture (yet): the album's first letter on a plain square.
                Text {
                    anchors.centerIn: parent
                    visible: cover.status !== Image.Ready
                    text: tile.title !== "" ? tile.title.charAt(0).toUpperCase() : "?"
                    font.pixelSize: Math.round(frame.width / 3)
                    font.bold: true
                    color: theme.c.lighter_background
                }
            }
            Text {
                anchors { left: frame.left; right: frame.right; top: frame.bottom; topMargin: 6 }
                text: tile.title
                elide: Text.ElideRight
                font.pixelSize: 12
                font.bold: true
                color: tile.chosen ? theme.c.bright_foreground : theme.c.foreground
            }
            Text {
                anchors { left: frame.left; right: frame.right; top: frame.bottom; topMargin: 23 }
                text: tile.artist + (tile.year > 0 ? "  ·  " + tile.year : "")
                elide: Text.ElideRight
                font.pixelSize: 11
                color: theme.c.muted
            }

            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.LeftButton | Qt.RightButton
                onPressed: (mouse) => {
                    grid.forceActiveFocus()
                    pane.albums.currentRow = tile.index
                    if (mouse.button === Qt.RightButton) {
                        const p = mapToItem(pane, mouse.x, mouse.y)
                        menu.popup(pane, p.x, p.y)
                    }
                }
                onDoubleClicked: (mouse) => { if (mouse.button === Qt.LeftButton) library.playAlbum(tile.albumId) }
            }
        }
    }

    Text {
        anchors.centerIn: parent
        width: parent.width - 60
        visible: grid.count === 0
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.WordWrap
        text: qsTr("No albums here.")
        font.pixelSize: 13
        color: theme.c.muted
    }

    Menu {
        id: menu
        Material.roundedScale: theme.radius
        MenuItem { text: qsTr("Play"); onTriggered: library.playAlbum(pane.albums.currentAlbum) }
        MenuItem { text: qsTr("Show tracks"); onTriggered: pane.openAlbum(pane.albums.currentAlbum) }
    }
}
