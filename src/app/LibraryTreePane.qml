import QtQuick
import QtQuick.Controls

// The library tree. Arrows move, Right opens a node (or steps into it),
// Left closes it (or steps out to its parent), Enter goes to the track list.
// The list follows the highlighted node after a short pause, so holding an
// arrow key doesn't query every node on the way.
FocusScope {
    id: pane
    signal leave() // to the track list
    // A command on a node, run by the window: new-playlist, new-autoplaylist,
    // new-folder, rename, delete, move, play, queue, append, export,
    // edit-autoplaylist, import, save-now-playing, new-collection,
    // edit-collection, restore-collections. node is {row, kind, id, label}.
    signal nodeAction(string id, var node)
    // A node chosen with the mouse (not the keys).
    signal nodeClicked(int row)
    // Party mode: playlists can't be changed.
    property bool locked: false

    function nodeAt(row) {
        return { row: row, kind: library.tree.kind(row), key: library.tree.key(row),
                 id: library.tree.value(row), label: library.tree.data(library.tree.index(row, 0), 257) }
    }
    function openMenu(row, item, x, y) {
        menu.node = nodeAt(row)
        menu.popup(item, x, y)
    }

    function choose(row) {
        list.currentIndex = row
        follow.restart()
    }

    Timer {
        id: follow
        interval: 120
        onTriggered: if (list.currentIndex !== library.currentNode) library.selectNode(list.currentIndex)
    }

    Connections {
        target: library
        function onCurrentNodeChanged() {
            if (list.currentIndex !== library.currentNode) list.currentIndex = library.currentNode
        }
    }

    // The tree reloads after the library changes (a scan or the folder
    // watcher); it keeps its scroll position through that.
    property real keepY: 0
    Connections {
        target: library.tree
        function onModelAboutToBeReset() { pane.keepY = list.contentY }
        function onModelReset() {
            list.currentIndex = library.currentNode
            const y = pane.keepY
            list.contentY = y
            Qt.callLater(() => { list.contentY = Math.max(0, Math.min(y, list.contentHeight - list.height)) })
        }
    }

    Rectangle {
        anchors.fill: parent
        color: theme.c.dark_background
    }

    // Tracks dragged from the list land on a static playlist. The list
    // reports where the pointer is in the window; the tree works out the
    // row under it and lights it.
    property int dropRow: -1
    function playlistRowAt(scenePos) {
        const p = list.mapFromItem(null, scenePos.x, scenePos.y)
        if (p.x < 0 || p.y < 0 || p.x > list.width || p.y > list.height) return -1
        const row = list.indexAt(10, p.y + list.contentY)
        return row >= 0 && library.tree.kind(row) === "playlist" && !pane.locked ? row : -1
    }
    function dragOver(scenePos) { dropRow = playlistRowAt(scenePos) }
    // The chosen tracks of a Spotify list (a SpotifyItemsModel) dropped on
    // a playlist: they go in as Spotify entries of a mixed playlist.
    function dropSpotifyAt(scenePos, rows) {
        const row = playlistRowAt(scenePos)
        dropRow = -1
        if (row < 0) return false
        return spotify.addChosenToOmampPlaylist(rows, library.tree.value(row))
    }
    // Returns true when the tracks went into a playlist.
    function dropAt(scenePos, ids) {
        const row = playlistRowAt(scenePos)
        dropRow = -1
        if (row < 0 || ids.length === 0) return false
        library.playlists.addTracks(library.tree.value(row), ids)
        return true
    }

    ListView {
        id: list
        objectName: "treeList"
        anchors.fill: parent
        anchors.margins: 4
        clip: true
        focus: true
        model: library.tree
        currentIndex: library.currentNode
        boundsBehavior: Flickable.StopAtBounds
        keyNavigationEnabled: false
        highlightMoveDuration: 0
        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

        Keys.onPressed: (event) => {
            const row = list.currentIndex
            const tree = library.tree
            switch (event.key) {
            case Qt.Key_Up: pane.choose(Math.max(0, row - 1)); break
            case Qt.Key_Down: pane.choose(Math.min(list.count - 1, row + 1)); break
            case Qt.Key_PageUp: pane.choose(Math.max(0, row - Math.floor(list.height / 24))); break
            case Qt.Key_PageDown: pane.choose(Math.min(list.count - 1, row + Math.floor(list.height / 24))); break
            case Qt.Key_Home: pane.choose(0); break
            case Qt.Key_End: pane.choose(list.count - 1); break
            case Qt.Key_Right:
                if (tree.isExpandable(row) && !tree.isExpanded(row)) tree.expand(row)
                else if (tree.isExpanded(row)) pane.choose(row + 1)
                else pane.leave()
                break
            case Qt.Key_Left:
                if (tree.isExpanded(row)) tree.collapse(row)
                else if (tree.parentRow(row) >= 0) pane.choose(tree.parentRow(row))
                break
            case Qt.Key_Return:
            case Qt.Key_Enter:
                library.selectNode(row)
                pane.leave()
                break
            case Qt.Key_Delete: {
                const k = tree.kind(row)
                if (k === "playlist" || k === "autoplaylist" || k === "plfolder" || k === "collection")
                    pane.nodeAction("delete", pane.nodeAt(row))
                break
            }
            case Qt.Key_Menu:
                pane.openMenu(row, list, list.width / 2, Math.min(list.height - 40, 24 * (row + 1) - list.contentY))
                break
            default: return
            }
            list.positionViewAtIndex(list.currentIndex, ListView.Contain)
            event.accepted = true
        }

        delegate: Rectangle {
            id: node
            required property int index
            required property string label
            required property int depth
            required property bool expandable
            required property bool expanded
            required property var trackCount
            required property bool dim
            required property string description
            readonly property bool chosen: index === library.currentNode
            readonly property bool cursor: index === list.currentIndex

            width: ListView.view.width
            height: 24
            radius: Math.min(3, theme.radius)
            color: chosen ? (pane.activeFocus ? theme.listSelection : Qt.alpha(theme.listSelection, 0.6))
                 : cursor && pane.activeFocus ? Qt.alpha(theme.listSelection, 0.35)
                 : hover.hovered ? Qt.alpha(theme.listSelection, 0.25) : "transparent"
            border.width: cursor && pane.activeFocus && !chosen ? 1 : 0
            border.color: theme.accent

            HoverHandler { id: hover }
            required property string kind
            // Tracks dragged from the list over this playlist.
            Rectangle {
                anchors.fill: parent
                visible: pane.dropRow === node.index
                color: Qt.alpha(theme.accent, 0.25)
                border.width: 1
                border.color: theme.accent
                radius: parent.radius
            }
            ToolTip.visible: hover.hovered && node.description !== ""
            ToolTip.delay: 700
            ToolTip.text: node.description

            Text {
                id: arrow
                x: 4 + node.depth * 14
                width: 14
                anchors.verticalCenter: parent.verticalCenter
                text: node.expandable ? (node.expanded ? "▾" : "▸") : ""
                font.pixelSize: 12
                color: theme.c.muted
            }
            // Folders, playlists and auto-playlists each have a small mark.
            Text {
                id: mark
                anchors { left: arrow.right; leftMargin: visible ? 2 : 0; verticalCenter: parent.verticalCenter }
                visible: node.kind === "plfolder" || node.kind === "playlist" || node.kind === "autoplaylist"
                width: visible ? 18 : 0
                text: node.kind === "plfolder" ? String.fromCodePoint(0xF024B)
                    : node.kind === "autoplaylist" ? String.fromCodePoint(0xF0DF2) : String.fromCodePoint(0xF0CB8)
                font.family: "JetBrainsMono Nerd Font"
                font.pixelSize: 13
                color: node.kind === "autoplaylist" ? theme.accent : theme.c.muted
            }
            Text {
                anchors { left: mark.right; leftMargin: 2; right: count.left; rightMargin: 6
                          verticalCenter: parent.verticalCenter }
                elide: Text.ElideRight
                text: node.label
                font.pixelSize: 13
                font.bold: node.depth === 0
                color: node.dim ? theme.c.muted : node.chosen ? theme.c.bright_foreground : theme.c.foreground
                opacity: node.dim ? 0.7 : 1
            }
            Text {
                id: count
                anchors { right: parent.right; rightMargin: 8; verticalCenter: parent.verticalCenter }
                text: node.trackCount >= 0 ? Number(node.trackCount).toLocaleString(Qt.locale(), 'f', 0) : ""
                font.pixelSize: 11
                color: theme.c.muted
            }

            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.LeftButton | Qt.RightButton
                onPressed: (mouse) => {
                    pane.forceActiveFocus()
                    if (mouse.button === Qt.RightButton) {
                        list.currentIndex = node.index
                        const p = mapToItem(list, mouse.x, mouse.y)
                        pane.openMenu(node.index, list, p.x, p.y)
                        return
                    }
                    if (node.expandable && mouse.x < arrow.x + arrow.width + 2) {
                        library.tree.toggle(node.index)
                        return
                    }
                    list.currentIndex = node.index
                    library.selectNode(node.index)
                    pane.nodeClicked(node.index)
                }
                onDoubleClicked: (mouse) => {
                    if (node.expandable && mouse.x >= arrow.x + arrow.width + 2) library.tree.toggle(node.index)
                }
            }
        }
    }

    // The node's menu: what can be done with a playlist, a folder, a
    // collection or the groups that hold them.
    Menu {
        id: menu
        property var node: ({})
        readonly property string kind: node.kind || ""
        readonly property bool isList: kind === "playlist" || kind === "autoplaylist"
        readonly property bool isPlaylistNode: isList || kind === "plfolder"
        readonly property bool inPlaylists: isPlaylistNode || node.key === "playlists"
        readonly property bool edit: !pane.locked

        MenuItem { text: qsTr("Play"); visible: menu.isList; height: visible ? implicitHeight : 0
                   onTriggered: pane.nodeAction("play", menu.node) }
        MenuItem { text: qsTr("Play next"); visible: menu.isList; height: visible ? implicitHeight : 0
                   onTriggered: pane.nodeAction("queue", menu.node) }
        MenuItem { text: qsTr("Add to Now Playing"); visible: menu.isList; height: visible ? implicitHeight : 0
                   onTriggered: pane.nodeAction("append", menu.node) }
        MenuItem { text: qsTr("Edit auto-playlist…"); visible: menu.kind === "autoplaylist"; height: visible ? implicitHeight : 0
                   enabled: menu.edit; onTriggered: pane.nodeAction("edit-autoplaylist", menu.node) }
        MenuItem { text: qsTr("Save as a playlist…"); visible: menu.kind === "nowplaying"; height: visible ? implicitHeight : 0
                   enabled: menu.edit; onTriggered: pane.nodeAction("save-now-playing", menu.node) }
        MenuItem { text: menu.kind === "plfolder" ? qsTr("New playlist in this folder…") : qsTr("New playlist…")
                   visible: menu.node.key === "playlists" || menu.kind === "plfolder"; height: visible ? implicitHeight : 0
                   enabled: menu.edit; onTriggered: pane.nodeAction("new-playlist", menu.node) }
        MenuItem { text: menu.kind === "plfolder" ? qsTr("New auto-playlist in this folder…") : qsTr("New auto-playlist…")
                   visible: menu.node.key === "playlists" || menu.kind === "plfolder"; height: visible ? implicitHeight : 0
                   enabled: menu.edit; onTriggered: pane.nodeAction("new-autoplaylist", menu.node) }
        MenuItem { text: qsTr("New folder…")
                   visible: menu.node.key === "playlists" || menu.kind === "plfolder"; height: visible ? implicitHeight : 0
                   enabled: menu.edit; onTriggered: pane.nodeAction("new-folder", menu.node) }
        MenuItem { text: qsTr("Import playlists…"); visible: menu.node.key === "playlists"; height: visible ? implicitHeight : 0
                   enabled: menu.edit; onTriggered: pane.nodeAction("import", menu.node) }
        MenuItem { text: qsTr("Save Now Playing as a playlist…"); visible: menu.node.key === "playlists"; height: visible ? implicitHeight : 0
                   enabled: menu.edit; onTriggered: pane.nodeAction("save-now-playing", menu.node) }
        MenuItem { text: qsTr("Export…"); visible: menu.isList; height: visible ? implicitHeight : 0
                   onTriggered: pane.nodeAction("export", menu.node) }
        MenuItem { text: qsTr("Rename…"); visible: menu.isPlaylistNode; height: visible ? implicitHeight : 0
                   enabled: menu.edit; onTriggered: pane.nodeAction("rename", menu.node) }
        Menu {
            id: moveMenu
            title: qsTr("Move to folder")
            enabled: menu.isPlaylistNode && menu.edit
            Instantiator {
                model: menu.isPlaylistNode ? library.playlists.folderChoices(menu.node.id || 0) : []
                delegate: MenuItem {
                    required property var modelData
                    text: modelData.name
                    onTriggered: library.playlists.moveTo(menu.node.id, modelData.id)
                }
                onObjectAdded: (index, object) => moveMenu.insertItem(index, object)
                onObjectRemoved: (index, object) => moveMenu.removeItem(object)
            }
        }
        MenuItem { text: qsTr("Delete…"); visible: menu.isPlaylistNode; height: visible ? implicitHeight : 0
                   enabled: menu.edit; onTriggered: pane.nodeAction("delete", menu.node) }
        MenuItem { text: qsTr("New collection…"); visible: menu.node.key === "collections"; height: visible ? implicitHeight : 0
                   enabled: menu.edit; onTriggered: pane.nodeAction("new-collection", menu.node) }
        MenuItem { text: qsTr("Put back Music, Classical, Audiobooks and Podcasts"); visible: menu.node.key === "collections"
                   height: visible ? implicitHeight : 0
                   enabled: menu.edit; onTriggered: pane.nodeAction("restore-collections", menu.node) }
        MenuItem { text: qsTr("Edit collection…"); visible: menu.kind === "collection"; height: visible ? implicitHeight : 0
                   enabled: menu.edit; onTriggered: pane.nodeAction("edit-collection", menu.node) }
        MenuItem { text: qsTr("Delete collection…"); visible: menu.kind === "collection"; height: visible ? implicitHeight : 0
                   enabled: menu.edit; onTriggered: pane.nodeAction("delete", menu.node) }
        // Location: the volumes, shares and servers.
        MenuItem { text: qsTr("Rescan this volume"); visible: menu.kind === "volume"; height: visible ? implicitHeight : 0
                   onTriggered: pane.nodeAction("rescan-volume", menu.node) }
        MenuItem { text: qsTr("Volumes, shares and servers…"); visible: menu.kind === "volume" || menu.node.key === "locations"
                   height: visible ? implicitHeight : 0; onTriggered: pane.nodeAction("volumes", menu.node) }
        MenuItem { text: qsTr("Connect a NAS…"); visible: menu.node.key === "locations"; height: visible ? implicitHeight : 0
                   onTriggered: pane.nodeAction("connect-nas", menu.node) }
        MenuItem { text: qsTr("Add Navidrome / Subsonic server…"); visible: menu.node.key === "locations"; height: visible ? implicitHeight : 0
                   onTriggered: pane.nodeAction("add-server", menu.node) }
        // Devices: sync profiles.
        MenuItem { objectName: "menuSyncDevice"; text: qsTr("Sync now"); visible: menu.kind === "device"; height: visible ? implicitHeight : 0
                   onTriggered: pane.nodeAction("device-sync", menu.node) }
        MenuItem { text: qsTr("Edit sync profile…"); visible: menu.kind === "device"; height: visible ? implicitHeight : 0
                   onTriggered: pane.nodeAction("device-edit", menu.node) }
        MenuItem { text: qsTr("New sync profile…"); visible: menu.node.key === "devices"; height: visible ? implicitHeight : 0
                   onTriggered: pane.nodeAction("devices", menu.node) }
        // Any list: save it as a report.
        MenuItem { text: qsTr("Save the list as CSV…"); visible: menu.kind !== "group" && menu.kind !== "" && menu.kind !== "plfolder"
                   height: visible ? implicitHeight : 0; onTriggered: pane.nodeAction("export-csv", menu.node) }
        MenuItem { text: qsTr("Save the list as a web page…"); visible: menu.kind !== "group" && menu.kind !== "" && menu.kind !== "plfolder"
                   height: visible ? implicitHeight : 0; onTriggered: pane.nodeAction("export-html", menu.node) }
        // Files to Edit > Missing artwork.
        MenuItem { objectName: "menuFindCovers"; text: qsTr("Find the missing covers on the Cover Art Archive")
                   visible: menu.node.key === "audit:missing-artwork"; height: visible ? implicitHeight : 0
                   onTriggered: pane.nodeAction("find-missing-covers", menu.node) }
    }
}
