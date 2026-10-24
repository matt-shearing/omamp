import QtQuick
import QtQuick.Window
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// The Spotify node of the library tree: your playlists, Liked Songs, saved
// albums and followed artists on the left, with search and a box for a
// pasted Spotify link; the chosen list on the right, with a button for
// each thing a row can do. What Spotify is playing shows along the top.
//
// Rows are chosen as in the library's track list: click, Shift+click for a
// range, Ctrl+click for one more; Shift+Up/Down extends, Ctrl+A chooses
// all, Esc clears. The buttons and the row menu act on the chosen rows.
// Enter or a double-click plays a track (the list from it, as the library
// does) or opens a playlist, album or artist; Backspace goes back; Left
// returns to the tree. Chosen tracks can be dragged onto a Thrum playlist
// in the tree.
FocusScope {
    id: view
    objectName: "spotifyView"
    signal leave()
    signal openSettings()
    // A command run by the window (spotify-new-thrum-playlist:view…).
    signal action(string id)
    // Chosen tracks dragged out of the list: where the pointer is in the
    // window, and where they were let go.
    signal tracksDragged(point scenePos)
    signal tracksDropped(point scenePos)

    readonly property var rows: spotify.items
    readonly property int currentRow: rows.currentRow
    readonly property var current: { void rows.count; return currentRow >= 0 && currentRow < rows.count ? rows.get(currentRow) : null }
    readonly property bool currentIsTrack: current !== null && current.kind === "track"
    // More than one row chosen: the actions take the chosen tracks.
    readonly property bool several: rows.selectedCount > 1

    // The list, or the search box while there is no list yet.
    function focusList() {
        if (spotify.items.count === 0 && searchField.enabled) searchField.forceActiveFocus()
        else list.forceActiveFocus()
    }
    // The search box, with text to search for at once when given.
    function focusSearch(text) {
        if (text) {
            searchField.text = text
            spotify.search(text)
        }
        searchField.forceActiveFocus()
        searchField.selectAll()
    }
    function activate(row) {
        const item = spotify.items.get(row)
        if (!item.kind) return
        if (item.kind === "track") spotify.playRow(row)
        else spotify.openRow(row)
    }
    function showRow(row) { if (row >= 0) list.positionViewAtIndex(row, ListView.Contain) }
    function openRowMenu(x, y) { rowMenu.popup(list, x, y) }

    onVisibleChanged: spotify.setViewVisible(visible)
    Component.onCompleted: spotify.setViewVisible(visible)

    Rectangle { anchors.fill: parent; color: theme.c.background }

    component SideButton: ItemDelegate {
        property string sectionId
        property string glyph
        Layout.fillWidth: true
        implicitHeight: 34
        highlighted: spotify.section === sectionId
        onClicked: spotify.openSection(sectionId)
        contentItem: RowLayout {
            spacing: 10
            Text {
                Layout.preferredWidth: 18
                text: parent.parent.glyph
                font.family: "JetBrainsMono Nerd Font"
                font.pixelSize: 15
                color: parent.parent.highlighted ? theme.accent : theme.c.muted
            }
            Text {
                Layout.fillWidth: true
                text: parent.parent.text
                elide: Text.ElideRight
                font.pixelSize: 13
                color: parent.parent.highlighted ? theme.c.bright_foreground : theme.c.foreground
            }
        }
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0
        visible: spotify.enabled

        // ---- Sections, search and links ------------------------------------
        Rectangle {
            Layout.preferredWidth: 220
            Layout.fillHeight: true
            color: theme.c.dark_background
            Rectangle { anchors { right: parent.right; top: parent.top; bottom: parent.bottom }
                        width: 1; color: theme.c.lighter_background }
            ColumnLayout {
                anchors { fill: parent; margins: 10 }
                spacing: 2
                TextField {
                    id: searchField
                    objectName: "spotifySearch"
                    Layout.fillWidth: true
                    Layout.bottomMargin: 6
                    enabled: spotify.canBrowse
                    placeholderText: qsTr("Search Spotify")
                    font.pixelSize: 13
                    text: spotify.searchText
                    onAccepted: { spotify.search(text); list.forceActiveFocus() }
                }
                SideButton { objectName: "spotifyPlaylists"; sectionId: "playlists"; glyph: String.fromCodePoint(0xF0CB8); text: qsTr("Your playlists"); enabled: spotify.canBrowse }
                SideButton { objectName: "spotifyLiked"; sectionId: "liked"; glyph: String.fromCodePoint(0xF02D1); text: qsTr("Liked Songs"); enabled: spotify.canBrowse }
                SideButton { objectName: "spotifyAlbums"; sectionId: "albums"; glyph: String.fromCodePoint(0xF0025); text: qsTr("Saved albums"); enabled: spotify.canBrowse }
                SideButton { objectName: "spotifyArtists"; sectionId: "artists"; glyph: String.fromCodePoint(0xF0803); text: qsTr("Followed artists"); enabled: spotify.canBrowse }
                Item { Layout.fillHeight: true }
                Text {
                    Layout.fillWidth: true
                    text: qsTr("Play a Spotify link")
                    font.pixelSize: 12
                    color: theme.c.light_foreground
                }
                TextField {
                    id: linkField
                    objectName: "spotifyLink"
                    Layout.fillWidth: true
                    placeholderText: qsTr("open.spotify.com/…")
                    font.pixelSize: 12
                    onAccepted: {
                        if (spotify.activateUri(text) !== "") text = ""
                        else linkError.visible = true
                    }
                    onTextChanged: linkError.visible = false
                }
                Text {
                    id: linkError
                    Layout.fillWidth: true
                    visible: false
                    wrapMode: Text.Wrap
                    text: qsTr("That isn't a Spotify track, album, artist or playlist link.")
                    font.pixelSize: 11
                    color: theme.c.red
                }
                Button {
                    Layout.fillWidth: true
                    flat: true
                    text: qsTr("Spotify settings…")
                    font.pixelSize: 12
                    onClicked: spotify.openSettings()
                }
            }
        }

        // ---- The list --------------------------------------------------------
        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            // What Spotify is playing, whoever started it.
            Rectangle {
                objectName: "spotifyNowPlaying"
                Layout.fillWidth: true
                Layout.preferredHeight: spotify.nowPlaying.active ? 64 : 0
                visible: spotify.nowPlaying.active === true
                color: theme.c.dark_background
                Rectangle { anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
                            height: 1; color: theme.c.lighter_background }
                RowLayout {
                    anchors { fill: parent; leftMargin: 12; rightMargin: 12 }
                    spacing: 12
                    CoverArt {
                        Layout.preferredWidth: 46
                        Layout.preferredHeight: 46
                        source: spotify.nowPlaying.art || ""
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 1
                        Text {
                            Layout.fillWidth: true
                            text: (spotify.nowPlaying.thrum ? (spotify.nowPlaying.playing ? qsTr("Playing in Thrum: ") : qsTr("Paused in Thrum: "))
                                   : (spotify.nowPlaying.playing ? qsTr("Playing on Spotify: ") : qsTr("Paused on Spotify: "))) + (spotify.nowPlaying.title || "")
                            elide: Text.ElideRight
                            font.pixelSize: 13
                            font.bold: true
                            color: theme.c.bright_foreground
                        }
                        Text {
                            Layout.fillWidth: true
                            text: (spotify.nowPlaying.artists || "") + (spotify.nowPlaying.device && spotify.nowPlaying.device !== "Thrum" ? "  ·  " + qsTr("on %1").arg(spotify.nowPlaying.device) : "")
                            elide: Text.ElideRight
                            font.pixelSize: 12
                            color: theme.c.muted
                        }
                    }
                    Button {
                        objectName: "spotifyLike"
                        flat: true
                        text: spotify.nowPlaying.liked ? qsTr("♥ Liked") : qsTr("♡ Like")
                        font.pixelSize: 12
                        onClicked: spotify.setPlayingLiked(!spotify.nowPlaying.liked)
                    }
                }
            }

            // Title, back, and the actions on the chosen row.
            RowLayout {
                Layout.fillWidth: true
                Layout.leftMargin: 12
                Layout.rightMargin: 12
                Layout.topMargin: 6
                spacing: 6
                Button {
                    objectName: "spotifyBack"
                    visible: spotify.canGoBack
                    flat: true
                    text: qsTr("‹ Back")
                    font.pixelSize: 13
                    onClicked: spotify.back()
                }
                Text {
                    Layout.fillWidth: true
                    text: spotify.viewTitle + (spotify.items.count > 0 ? "  (" + spotify.items.count + ")" : "")
                    elide: Text.ElideRight
                    font.pixelSize: 15
                    font.bold: true
                    color: theme.c.bright_foreground
                }
                Text {
                    objectName: "spotifySelectedCount"
                    visible: view.several
                    text: qsTr("%Ln selected", "", view.rows.selectedCount)
                    font.pixelSize: 12
                    color: theme.c.light_foreground
                }
                BusyIndicator { visible: spotify.loading; running: visible; implicitWidth: 28; implicitHeight: 28 }
                Button {
                    objectName: "spotifyRefresh"
                    flat: true
                    enabled: !spotify.loading && spotify.canBrowse
                    implicitWidth: 36
                    text: String.fromCodePoint(0xF0450)
                    font.family: "JetBrainsMono Nerd Font"
                    font.pixelSize: 16
                    Accessible.name: qsTr("Refresh")
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Read this list from Spotify again. Thrum keeps lists for ten minutes, so it asks Spotify less often.")
                    onClicked: spotify.reload()
                }
                Button {
                    objectName: "spotifyPlay"
                    text: view.several || view.currentIsTrack ? qsTr("Play") : qsTr("Play all")
                    enabled: view.current !== null
                    font.pixelSize: 12
                    onClicked: spotify.playChosen(view.rows, "play")
                }
                Button {
                    objectName: "spotifyPlayNext"
                    text: qsTr("Play next")
                    enabled: view.current !== null
                    font.pixelSize: 12
                    onClicked: spotify.playChosen(view.rows, "next")
                }
                Button {
                    objectName: "spotifyEnqueue"
                    text: qsTr("Add to Now Playing")
                    enabled: view.current !== null
                    font.pixelSize: 12
                    onClicked: spotify.playChosen(view.rows, "append")
                }
                Button {
                    id: addButton
                    objectName: "spotifyAddToPlaylist"
                    text: qsTr("Add to playlist ▾")
                    enabled: view.current !== null && (view.several ? view.rows.chosenTrackCount > 0
                             : view.current.kind === "track" || view.current.kind === "album")
                    font.pixelSize: 12
                    onClicked: addMenu.popup(addButton, 0, addButton.height)
                }
            }

            // A playlist Spotify keeps from your app, or an artist: plays as a
            // whole on Spotify, with a sentence on why.
            Rectangle {
                objectName: "spotifyContextBar"
                Layout.fillWidth: true
                Layout.leftMargin: 12
                Layout.rightMargin: 12
                Layout.topMargin: 6
                visible: spotify.viewNote !== "" || spotify.viewKind === "artist"
                implicitHeight: contextColumn.implicitHeight + 20
                radius: Math.min(4, theme.radius)
                color: theme.c.dark_background
                border.width: 1
                border.color: theme.c.lighter_background
                ColumnLayout {
                    id: contextColumn
                    anchors { left: parent.left; right: parent.right; verticalCenter: parent.verticalCenter
                              leftMargin: 12; rightMargin: 12 }
                    spacing: 8
                    Text {
                        objectName: "spotifyContextNote"
                        visible: text !== ""
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        text: spotify.viewNote
                        font.pixelSize: 13
                        color: theme.c.foreground
                    }
                    Flow {
                        Layout.fillWidth: true
                        spacing: 6
                        visible: spotify.viewRestricted || spotify.viewKind === "artist"
                        Button {
                            objectName: "spotifyContextPlay"
                            text: spotify.viewKind === "artist" ? qsTr("Play artist") : qsTr("Play playlist")
                            font.pixelSize: 12
                            onClicked: spotify.playView(false)
                        }
                        Button {
                            objectName: "spotifyContextShuffle"
                            text: qsTr("Shuffle")
                            font.pixelSize: 12
                            onClicked: spotify.playView(true)
                        }
                        Button {
                            text: qsTr("Play it next")
                            font.pixelSize: 12
                            onClicked: spotify.enqueueView(true)
                        }
                        Button {
                            text: qsTr("Add it to Now Playing")
                            font.pixelSize: 12
                            onClicked: spotify.enqueueView(false)
                        }
                        Button {
                            objectName: "spotifyShowTracks"
                            visible: spotify.viewRestricted
                            flat: true
                            text: qsTr("Show tracks (slow)")
                            font.pixelSize: 12
                            onClicked: spotify.showRestrictedTracks()
                        }
                    }
                }
            }

            // Where the list stands: loading, slow, rate-limited, or failed.
            Rectangle {
                objectName: "spotifyLoadState"
                Layout.fillWidth: true
                Layout.leftMargin: 12
                Layout.rightMargin: 12
                Layout.topMargin: 6
                visible: stateText.text !== ""
                implicitHeight: stateRow.implicitHeight + 16
                radius: Math.min(4, theme.radius)
                color: theme.c.dark_background
                border.width: 1
                border.color: spotify.viewError !== "" ? theme.c.yellow : theme.c.lighter_background
                RowLayout {
                    id: stateRow
                    anchors { left: parent.left; right: parent.right; verticalCenter: parent.verticalCenter
                              leftMargin: 12; rightMargin: 8 }
                    spacing: 10
                    Text {
                        id: stateText
                        objectName: "spotifyLoadText"
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        font.pixelSize: 13
                        color: spotify.viewError !== "" ? theme.c.yellow : theme.c.foreground
                        text: {
                            if (!spotify.loading)
                                return spotify.viewError
                            if (spotify.rateLimited)
                                return spotify.retryIn > 0
                                    ? qsTr("Spotify is rate-limiting requests. Retrying in %1 s…").arg(spotify.retryIn)
                                    : qsTr("Spotify is rate-limiting requests. Retrying…")
                            if (spotify.loadingSlow)
                                return qsTr("Still loading… Spotify is slow right now (%1 s).").arg(spotify.loadingSeconds)
                            if (spotify.refreshing)
                                return qsTr("Showing the copy from earlier while it reloads.")
                            return spotify.items.count === 0 ? qsTr("Loading…") : ""
                        }
                    }
                    Button {
                        objectName: "spotifyCancelLoad"
                        visible: spotify.loading && (spotify.loadingSlow || spotify.rateLimited)
                        flat: true
                        text: qsTr("Cancel")
                        font.pixelSize: 12
                        onClicked: spotify.cancelLoad()
                    }
                    Button {
                        objectName: "spotifyRetry"
                        visible: !spotify.loading && spotify.viewError !== "" && spotify.canBrowse
                        text: qsTr("Retry")
                        font.pixelSize: 12
                        onClicked: spotify.reload()
                    }
                }
            }

            ListView {
                id: list
                objectName: "spotifyList"
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.topMargin: 4
                clip: true
                focus: true
                model: spotify.items
                currentIndex: view.currentRow
                keyNavigationEnabled: false
                highlightMoveDuration: 0
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar {}
                section.property: "section"
                section.delegate: Text {
                    required property string section
                    width: ListView.view.width
                    height: section.length > 0 ? 30 : 0
                    leftPadding: 14
                    verticalAlignment: Text.AlignBottom
                    bottomPadding: 4
                    text: section
                    font.pixelSize: 12
                    font.bold: true
                    color: theme.c.muted
                }
                // The library's keys (TrackTable.qml), plus Backspace for back.
                Keys.onPressed: (event) => {
                    const t = view.rows
                    const page = Math.max(1, Math.floor(list.height / 52) - 1)
                    const mods = event.modifiers & (Qt.ShiftModifier | Qt.ControlModifier)
                    switch (event.key) {
                    case Qt.Key_Up: t.moveCurrent(-1, mods); break
                    case Qt.Key_Down: t.moveCurrent(1, mods); break
                    case Qt.Key_PageUp: t.moveCurrent(-page, mods); break
                    case Qt.Key_PageDown: t.moveCurrent(page, mods); break
                    case Qt.Key_Home: t.moveCurrent(-t.count, mods); break
                    case Qt.Key_End: t.moveCurrent(t.count, mods); break
                    case Qt.Key_Left: view.leave(); break
                    case Qt.Key_Backspace: spotify.back(); break
                    case Qt.Key_Return:
                    case Qt.Key_Enter:
                        if (view.several) spotify.playChosen(t, "play")
                        else if (t.currentRow >= 0) view.activate(t.currentRow)
                        break
                    case Qt.Key_A:
                        if (!(event.modifiers & Qt.ControlModifier)) return
                        t.selectAll()
                        break
                    case Qt.Key_Escape:
                        if (t.selectedCount === 0) return
                        t.clearSelection()
                        break
                    case Qt.Key_Space:
                        // Ctrl+Space picks the row under the cursor; plain Space is play/pause.
                        if (!(event.modifiers & Qt.ControlModifier)) return
                        t.select(t.currentRow, Qt.ControlModifier)
                        break
                    case Qt.Key_Menu:
                        view.openRowMenu(list.width / 3, Math.min(list.height - 40, 52 * (t.currentRow + 1) - list.contentY))
                        break
                    default: return
                    }
                    if (event.key !== Qt.Key_Menu) view.showRow(t.currentRow)
                    event.accepted = true
                }

                delegate: Rectangle {
                    id: row
                    required property int index
                    required property string kind
                    required property string name
                    required property string detail
                    required property string duration
                    required property string art
                    required property bool rowSelected
                    required property bool rowCurrent
                    width: ListView.view.width
                    height: 52
                    color: rowSelected ? (list.activeFocus ? theme.listSelection : Qt.alpha(theme.listSelection, 0.6))
                         : hover.hovered ? Qt.alpha(theme.listSelection, 0.3) : "transparent"
                    Component.onCompleted: spotify.items.wantArt(index)
                    HoverHandler { id: hover }

                    // The keyboard cursor: a line above and below the row, as in the library.
                    Rectangle { visible: row.rowCurrent && list.activeFocus; color: theme.accent; height: 1
                                anchors { left: parent.left; right: parent.right; top: parent.top } }
                    Rectangle { visible: row.rowCurrent && list.activeFocus; color: theme.accent; height: 1
                                anchors { left: parent.left; right: parent.right; bottom: parent.bottom } }

                    RowLayout {
                        anchors { fill: parent; leftMargin: 12; rightMargin: 14 }
                        spacing: 12
                        Rectangle {
                            // The Art column's switch covers Spotify's lists too.
                            visible: appearance.layout.artColumn
                            Layout.preferredWidth: 40
                            Layout.preferredHeight: 40
                            radius: row.kind === "artist" ? 20 : Math.min(4, theme.radius)
                            color: theme.c.lighter_background
                            clip: true
                            Image {
                                anchors.fill: parent
                                source: row.art
                                visible: row.art !== ""
                                fillMode: Image.PreserveAspectCrop
                                asynchronous: true
                                sourceSize: Qt.size(80, 80)
                            }
                            Text {
                                anchors.centerIn: parent
                                visible: row.art === ""
                                text: String.fromCodePoint(row.kind === "album" ? 0xF0025 : row.kind === "artist" ? 0xF0803
                                                           : row.kind === "playlist" ? 0xF0CB8 : 0xF075A)
                                font.family: "JetBrainsMono Nerd Font"
                                font.pixelSize: 18
                                color: theme.c.muted
                            }
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 1
                            Text {
                                Layout.fillWidth: true
                                text: row.name
                                elide: Text.ElideRight
                                font.pixelSize: 13
                                color: theme.c.bright_foreground
                            }
                            Text {
                                Layout.fillWidth: true
                                visible: text !== ""
                                text: row.detail
                                elide: Text.ElideRight
                                font.pixelSize: 12
                                color: row.rowSelected ? theme.c.light_foreground : theme.c.muted
                            }
                        }
                        Text {
                            visible: row.kind !== "track"
                            text: row.kind === "album" ? qsTr("Album") : row.kind === "artist" ? qsTr("Artist") : qsTr("Playlist")
                            font.pixelSize: 11
                            color: theme.c.muted
                        }
                        Text {
                            Layout.preferredWidth: 44
                            horizontalAlignment: Text.AlignRight
                            text: row.duration
                            font.pixelSize: 12
                            color: theme.c.light_foreground
                        }
                    }
                    MouseArea {
                        anchors.fill: parent
                        acceptedButtons: Qt.LeftButton | Qt.RightButton
                        preventStealing: true
                        property point pressedAt
                        property bool deferred: false
                        onPressed: (mouse) => {
                            list.forceActiveFocus()
                            pressedAt = Qt.point(mouse.x, mouse.y)
                            deferred = false
                            if (mouse.button === Qt.RightButton) {
                                if (!view.rows.isSelected(row.index)) view.rows.select(row.index, 0)
                                const p = mapToItem(list, mouse.x, mouse.y)
                                view.openRowMenu(p.x, p.y)
                                return
                            }
                            const mods = mouse.modifiers & (Qt.ShiftModifier | Qt.ControlModifier)
                            // A press on a chosen row keeps the selection until the
                            // release, so several tracks can be dragged together.
                            if (mods === 0 && row.rowSelected && view.rows.selectedCount > 1) deferred = true
                            else view.rows.select(row.index, mods)
                        }
                        // Dragging tracks onto a Thrum playlist in the tree.
                        onPositionChanged: (mouse) => {
                            if (!pressed || !(pressedButtons & Qt.LeftButton)) return
                            if (!dragProxy.active) {
                                if (Math.abs(mouse.x - pressedAt.x) + Math.abs(mouse.y - pressedAt.y) < 10) return
                                dragProxy.start(view.rows.chosenTrackCount)
                                if (!dragProxy.active) return
                            }
                            const p = mapToItem(dragProxy.parent, mouse.x, mouse.y)
                            dragProxy.x = p.x + 12
                            dragProxy.y = p.y + 4
                            view.tracksDragged(mapToItem(null, mouse.x, mouse.y))
                        }
                        onReleased: (mouse) => {
                            if (dragProxy.active) {
                                dragProxy.active = false
                                view.tracksDropped(mapToItem(null, mouse.x, mouse.y))
                            }
                            else if (deferred) view.rows.select(row.index, 0)
                            deferred = false
                        }
                        onDoubleClicked: (mouse) => { if (mouse.button === Qt.LeftButton) view.activate(row.index) }
                    }
                }
            }
        }
    }

    // What is being dragged: the chosen tracks, under the pointer.
    Rectangle {
        id: dragProxy
        objectName: "spotifyDragProxy"
        property int tracks: 0
        property bool active: false
        function start(n) {
            tracks = n
            parent = view.Window.contentItem
            active = n > 0
        }
        visible: active
        z: 1000
        width: dragText.implicitWidth + 20
        height: 26
        radius: Math.min(4, theme.radius)
        color: theme.c.dark_background
        border.width: 1
        border.color: theme.accent
        Text {
            id: dragText
            anchors.centerIn: parent
            text: qsTr("%n Spotify tracks", "", dragProxy.tracks)
            font.pixelSize: 12
            color: theme.c.bright_foreground
        }
    }

    // Spotify off: say how to turn it on.
    ColumnLayout {
        anchors.centerIn: parent
        width: Math.min(parent.width - 48, 460)
        visible: !spotify.enabled
        spacing: 12
        Text {
            Layout.fillWidth: true
            text: qsTr("Spotify is off")
            font.pixelSize: 16
            font.bold: true
            color: theme.c.bright_foreground
        }
        Text {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: qsTr("Switch it on in Settings → Spotify to browse, search and play Spotify here.")
            font.pixelSize: 13
            color: theme.c.muted
        }
        PrimaryButton { text: qsTr("Spotify settings…"); onClicked: spotify.openSettings() }
    }

    Menu {
        id: rowMenu
        objectName: "spotifyRowMenu"
        Material.roundedScale: theme.radius
        MenuItem { text: view.several ? qsTr("Play") : view.currentIsTrack ? qsTr("Play from here") : qsTr("Play all")
                   onTriggered: spotify.playChosen(view.rows, "play") }
        MenuItem { text: qsTr("Play next"); onTriggered: spotify.playChosen(view.rows, "next") }
        MenuItem { text: qsTr("Add to Now Playing"); onTriggered: spotify.playChosen(view.rows, "append") }
        MenuItem { text: qsTr("Open"); visible: !view.several && !view.currentIsTrack; height: visible ? implicitHeight : 0
                   onTriggered: spotify.openRow(view.currentRow) }
        SpotifyAddMenu {
            id: rowAddMenu
            objectName: "spotifyRowAddMenu"
            rows: view.rows
            source: "view"
            enabled: addButton.enabled
            onAction: (id) => view.action(id)
        }
        MenuSeparator { }
        MenuItem { objectName: "spotifyCopyLinks"
                   text: view.several ? qsTr("Copy Spotify links") : qsTr("Copy Spotify link")
                   onTriggered: spotify.copyChosenLinks(view.rows) }
    }
    SpotifyAddMenu {
        id: addMenu
        objectName: "spotifyAddMenu"
        rows: view.rows
        source: "view"
        onAction: (id) => view.action(id)
    }
}
