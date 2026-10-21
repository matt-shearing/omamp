import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Advanced mode: the deck as a strip across the top, the library below it.
//
//   deck strip
//   [Add folder] [Rescan]  [ search .............. ]  [Browser] [Grid] [Info] [Columns]
//   tree | column browser (Genre | Artist | Album)  | info
//        | track list, or the album grid           |
//   jobs and status
FocusScope {
    id: view
    property bool showRemaining: false
    property int treeWidth: 250
    property string columns: ""
    // The optional parts, kept by the window between runs.
    property bool browserVisible: false
    property bool infoVisible: false
    property bool gridVisible: false
    property int browserHeight: 214
    property int infoWidth: 290
    // The now playing bar (appearance.layout): "compact" or "large", "top" or "bottom".
    readonly property string barSize: appearance.layout.barSize
    readonly property string barPosition: appearance.layout.barPosition
    signal clockClicked()
    signal layoutEdited()
    signal toggleBrowser()
    signal toggleInfo()
    signal toggleGrid()
    // A command from a menu (edit-tags, organise, tag-from-filename), run by the window.
    signal action(string id)
    signal confirm(string title, string text, string confirmText, var action)
    // Phase 3: a tree node's menu command, and the deck strip's lamps.
    signal nodeAction(string id, var node)
    signal autoDjClicked()
    signal partyClicked()
    // The strip's Player button: back to the deck.
    signal openPlayer()

    readonly property bool nowPlaying: library.currentKind === "nowplaying"
    // Files to Edit > Duplicates shows its own view instead of the list.
    readonly property bool duplicatesNode: library.currentKind === "duplicates"
    // A playlist, auto-playlist or playlist folder shows the playlist view.
    readonly property bool playlistNode: library.currentKind === "playlist" || library.currentKind === "autoplaylist"
                                         || library.currentKind === "plfolder"
    // Spotify (SpotifyView.qml) when it is switched on.
    readonly property bool spotifyNode: library.currentKind === "spotify"
    readonly property bool radioNode: library.currentKind === "radio" || library.currentKind === "radiogroup"
    readonly property bool special: nowPlaying || duplicatesNode || playlistNode || spotifyNode || radioNode
    // Party mode locks changing playlists.
    readonly property bool playlistsLocked: library.dj.partyOn && !library.dj.allowed("edit-playlists")
    readonly property bool showingBrowser: browserVisible && !special
    readonly property bool showingGrid: gridVisible && !special

    function focusSearch() { search.forceActiveFocus(); search.selectAll() }
    // Spotify's own search box (the node must be chosen first), with text
    // to search for at once when given.
    function focusSpotifySearch(text) { spotifyView.focusSearch(text) }
    // Where the search box looks: "local" (the library), "both" (the
    // library, with Spotify's answers under it) or "spotify" (the Spotify
    // node's own search). Only offered while Spotify is on.
    readonly property bool spotifyScopes: spotify.enabled && spotify.canBrowse
    readonly property string searchScope: spotifyNode ? "spotify" : spotify.blendSearch ? "both" : "local"
    function setSearchScope(scope) {
        if (scope === "spotify") {
            const text = search.text.trim()
            const row = library.tree.reveal(["spotify"])
            if (row >= 0) library.selectNode(row)
            spotifyView.focusSearch(text)
            return
        }
        spotify.blendSearch = scope === "both"
        if (spotifyNode) library.selectNode(0)
        focusSearch()
    }
    function focusList() {
        if (nowPlaying) nowPlayingList.forceActiveFocus()
        else if (duplicatesNode) duplicatesView.forceActiveFocus()
        else if (radioNode) radioView.focusGrid()
        else if (playlistNode) playlistView.forceActiveFocus()
        else if (spotifyNode) spotifyView.focusList()
        else if (showingGrid) grid.forceActiveFocus()
        else table.forceActiveFocus()
    }
    function focusTree() { tree.forceActiveFocus() }
    function focusBrowser() { if (showingBrowser) browserPanes.focusPane(0) }
    // A collection shows its own columns; the window keeps the others.
    property var collectionId: library.currentKind === "collection" ? library.tree.value(library.currentNode) : 0
    property string globalColumns: ""
    property bool applyingColumns: false
    onCollectionIdChanged: {
        applyingColumns = true
        if (collectionId > 0) {
            if (globalColumns === "") globalColumns = table.columnState()
            const c = library.playlists.collection(collectionId)
            if (c.columns && c.columns.length > 0) table.showOnly(c.columns)
        } else if (globalColumns !== "") {
            table.restoreColumns(globalColumns)
            globalColumns = ""
        }
        applyingColumns = false
    }
    function columnState() { return globalColumns !== "" ? globalColumns : table.columnState() }
    function currentTreeWidth() { return Math.round(tree.width) }
    function currentBrowserHeight() { return Math.round(browserPanes.height) }
    function currentInfoWidth() { return Math.round(info.width) }
    function jumpToEnd() { table.jumpToEnd() }
    // The info panel's Lyrics tab, or its details.
    function showInfoPage(page) { info.page = page }
    // F2: the title of the track under the cursor, edited in place.
    function editTitle() { if (!special && !showingGrid) table.editTitle() }
    readonly property bool listShowing: !special && !showingGrid
    // Puts text in the search box and searches at once.
    function setSearchText(text) {
        search.text = text
        typing.stop()
        library.tracks.filter = text
    }

    // The window's settings can arrive after this view is made (when Thrum
    // starts in the library), so the saved columns are applied when they come.
    property bool columnsRestored: false
    function restoreSavedColumns() {
        if (columnsRestored || columns === "") return
        columnsRestored = true
        table.restoreColumns(columns)
    }
    onColumnsChanged: restoreSavedColumns()
    Component.onCompleted: {
        restoreSavedColumns()
        // Built in the background while the deck shows: the keys stay there.
        if (visible) focusList()
    }

    // The chosen radio group fills the picker.
    Connections {
        target: library
        function onCurrentNodeChanged() {
            if (view.radioNode) radio.group = library.tree.value(library.currentNode) || "nightride"
        }
    }
    Connections {
        target: library
        function onSearchSet(text) {
            if (search.text !== text) search.text = text
            typing.stop()
        }
        function onRevealRow(row) {
            table.showRow(row)
            if (!view.showingGrid) table.forceActiveFocus()
        }
    }

    Rectangle { anchors.fill: parent; color: theme.c.background }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // The now playing bar: the strip, or the large bar, at the top or
        // the bottom (Settings > Appearance > Layout, or the View menu).
        Loader {
            id: topBar
            objectName: "topBarSlot"
            Layout.fillWidth: true
            Layout.margins: 8
            Layout.bottomMargin: 6
            active: view.barPosition === "top"
            visible: active
            sourceComponent: view.barSize === "large" ? bigBarComponent : stripComponent
        }

        // Library actions, the search, and the view switches.
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
                Layout.minimumWidth: 160
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
                    objectName: "searchBox"
                    anchors { left: magnifier.right; leftMargin: 8; right: clear.left; rightMargin: 4
                              verticalCenter: parent.verticalCenter }
                    font.pixelSize: 13
                    color: theme.c.foreground
                    selectionColor: theme.c.selection
                    selectedTextColor: theme.c.bright_foreground
                    clip: true
                    onTextChanged: if (!view.spotifyNode) typing.restart()
                    Keys.onPressed: (event) => {
                        if (event.key === Qt.Key_Escape) {
                            if (search.text !== "") search.text = ""
                            else view.focusList()
                            event.accepted = true
                        } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                            typing.stop()
                            // On the Spotify node the box searches Spotify.
                            if (view.spotifyNode) view.focusSpotifySearch(search.text.trim())
                            else {
                                library.tracks.filter = search.text
                                view.focusList()
                            }
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
                        text: view.spotifyNode ? qsTr("Search Spotify  (Enter)")
                            : view.spotifyScopes && spotify.blendSearch ? qsTr("Search the library and Spotify  (/)")
                            : qsTr("Search  (/)   words, artist:name, year:1990..1999, rating:4..")
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
            // Where the search looks, when Spotify is on: said in words, so
            // Spotify's search is found without knowing the tree.
            Row {
                objectName: "searchScope"
                visible: view.spotifyScopes
                spacing: 4
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    rightPadding: 2
                    text: qsTr("Search in")
                    font.pixelSize: 12
                    color: theme.c.light_foreground
                }
                Repeater {
                    model: [
                        { id: "local", label: qsTr("Library"), tip: qsTr("Search your library only") },
                        { id: "spotify", label: qsTr("Spotify"), tip: qsTr("Search Spotify: tracks, albums, artists and playlists") },
                        { id: "both", label: qsTr("Both"), tip: qsTr("Search your library, with Spotify's answers under it") }
                    ]
                    Button {
                        required property var modelData
                        objectName: "searchScope_" + modelData.id
                        text: modelData.label
                        flat: true
                        font.pixelSize: 12
                        implicitHeight: 32; topInset: 0; bottomInset: 0
                        leftPadding: 10; rightPadding: 10
                        readonly property bool on: view.searchScope === modelData.id
                        background: Rectangle {
                            radius: Math.min(4, theme.radius)
                            color: parent.on ? Qt.alpha(theme.accent, 0.16)
                                 : parent.hovered ? Qt.alpha(theme.c.selection, 0.5) : "transparent"
                            border.width: 1
                            border.color: parent.on ? Qt.alpha(theme.accent, 0.7) : Qt.alpha(theme.c.foreground, 0.25)
                        }
                        ToolTip.visible: hovered
                        ToolTip.delay: 600
                        ToolTip.text: modelData.tip
                        onClicked: view.setSearchScope(modelData.id)
                    }
                }
            }
            Item { Layout.fillWidth: library.tracks.filterError === "" }

            // The view switches: each says what it shows and is lit while on.
            Repeater {
                model: [
                    { name: "browserButton", label: qsTr("Browser"), on: view.browserVisible, tip: qsTr("Genre, artist and album columns (Ctrl+B)") },
                    { name: "gridButton", label: qsTr("Albums"), on: view.gridVisible, tip: qsTr("Album covers instead of the track list (G)") },
                    { name: "infoButton", label: qsTr("Sidebar"), on: view.infoVisible, tip: qsTr("Cover, lyrics, about the artist and details (I)") }
                ]
                Button {
                    required property var modelData
                    required property int index
                    objectName: modelData.name
                    text: modelData.label
                    flat: true
                    checkable: false
                    font.pixelSize: 13
                    // Lit while on: a tinted fill and an accent edge.
                    background: Rectangle {
                        radius: Math.min(4, theme.radius)
                        color: modelData.on ? Qt.alpha(theme.accent, 0.16)
                             : parent.hovered ? Qt.alpha(theme.c.selection, 0.5) : "transparent"
                        border.width: modelData.on ? 1 : 0
                        border.color: Qt.alpha(theme.accent, 0.7)
                    }
                    implicitHeight: 34; topInset: 0; bottomInset: 0
                    enabled: index === 2 || !view.special
                    ToolTip.visible: hovered
                    ToolTip.delay: 600
                    ToolTip.text: modelData.tip
                    onClicked: index === 0 ? view.toggleBrowser() : index === 1 ? view.toggleGrid() : view.toggleInfo()
                }
            }
            Button {
                id: columnsButton
                text: qsTr("Columns")
                flat: true
                font.pixelSize: 13
                implicitHeight: 34; topInset: 0; bottomInset: 0
                visible: !view.special && !view.showingGrid
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
                onLeave: view.showingBrowser ? view.focusBrowser() : view.focusList()
                onWidthChanged: view.layoutEdited()
                locked: view.playlistsLocked
                onNodeAction: (id, node) => view.nodeAction(id, node)
                // Clicking Spotify puts the keys in its search box.
                onNodeClicked: if (view.spotifyNode) spotifyView.focusSearch("")
            }

            SplitView {
                id: middle
                SplitView.fillWidth: true
                SplitView.minimumWidth: 300
                orientation: Qt.Vertical

                handle: Rectangle {
                    implicitHeight: 5
                    color: SplitHandle.pressed || SplitHandle.hovered ? theme.c.selection : theme.c.lighter_background
                    Rectangle { anchors.centerIn: parent; height: 1; width: parent.width; color: theme.c.lighter_background }
                }

                BrowserPanes {
                    id: browserPanes
                    visible: view.showingBrowser
                    SplitView.preferredHeight: view.browserHeight
                    SplitView.minimumHeight: 90
                    onLeave: view.focusList()
                    onLeaveLeft: view.focusTree()
                    onHeightChanged: view.layoutEdited()
                }

                Item {
                    SplitView.fillHeight: true
                    SplitView.minimumHeight: 120
                    // "Local" over the list while Spotify's results show below it.
                    Text {
                        id: localHeading
                        visible: blendStrip.visible && table.visible
                        height: visible ? 26 : 0
                        leftPadding: 12
                        verticalAlignment: Text.AlignVCenter
                        text: qsTr("Local: %Ln tracks", "", library.tracks.count)
                        font.pixelSize: 13
                        font.bold: true
                        color: theme.c.bright_foreground
                    }
                    TrackTable {
                        id: table
                        anchors { fill: parent; topMargin: localHeading.height }
                        visible: !view.special && !view.showingGrid
                        onLeave: view.showingBrowser ? view.focusBrowser() : view.focusTree()
                        onLayoutEdited: {
                            if (view.collectionId > 0 && !view.applyingColumns)
                                library.playlists.setCollectionColumns(view.collectionId, table.visibleNames())
                            view.layoutEdited()
                        }
                        onAction: (id) => view.action(id)
                        onTracksDragged: (pos) => tree.dragOver(pos)
                        onTracksDropped: (pos, ids) => tree.dropAt(pos, ids)
                    }
                    AlbumGrid {
                        id: grid
                        anchors.fill: parent
                        visible: view.showingGrid
                        onLeave: view.showingBrowser ? view.focusBrowser() : view.focusTree()
                        onAction: (id) => view.action(id)
                        onOpenAlbum: (albumId) => {
                            if (library.showAlbum(albumId) >= 0) {
                                view.toggleGrid()
                                view.focusList()
                            }
                        }
                    }
                    NowPlayingList {
                        id: nowPlayingList
                        anchors.fill: parent
                        visible: view.nowPlaying
                        onLeave: view.focusTree()
                        onOpenBigView: view.action("big-view")
                    }
                    PlaylistView {
                        id: playlistView
                        anchors.fill: parent
                        visible: view.playlistNode
                        locked: view.playlistsLocked
                        onLeave: view.focusTree()
                        onAction: (id) => view.action(id)
                    }
                    SpotifyView {
                        id: spotifyView
                        anchors.fill: parent
                        visible: view.spotifyNode
                        onLeave: view.focusTree()
                        onAction: (id) => view.action(id)
                        // Chosen Spotify tracks dragged onto a Thrum playlist in the tree.
                        onTracksDragged: (pos) => tree.dragOver(pos)
                        onTracksDropped: (pos) => tree.dropSpotifyAt(pos, spotify.items)
                    }
                    RadioView {
                        id: radioView
                        anchors.fill: parent
                        visible: view.radioNode
                        showTabs: false
                        onLeave: view.focusTree()
                        onFindStations: view.action("radio-find")
                        onAddStation: view.action("radio-add")
                    }
                    DuplicatesView {
                        id: duplicatesView
                        anchors.fill: parent
                        visible: view.duplicatesNode
                        onLeave: view.focusTree()
                        onConfirm: (title, text, confirmText, action) => view.confirm(title, text, confirmText, action)
                    }
                }
                // Spotify's answers to the search, under the track list.
                SpotifyBlendStrip {
                    id: blendStrip
                    onAction: (id) => view.action(id)
                    visible: spotify.enabled && spotify.blendSearch && spotify.canBrowse && !view.special
                             && library.tracks.filter.trim().length >= 2
                    SplitView.preferredHeight: 196
                    SplitView.minimumHeight: 150
                }
            }

            SidePanel {
                id: info
                visible: view.infoVisible
                SplitView.preferredWidth: view.infoWidth
                SplitView.minimumWidth: 240
                onWidthChanged: view.layoutEdited()
                onEditLyrics: (id) => view.action("edit-lyrics:" + id)
                onOpenBigView: view.action("big-view")
                chosenHeading: view.showingGrid ? qsTr("Chosen album, first track") : ""
                // The track under the cursor, or the album chosen in the grid;
                // 0 shows the track that is playing.
                chosenTrack: view.playlistNode ? (library.playlists.items.currentRow >= 0 ? library.playlists.items.trackId(library.playlists.items.currentRow) : 0)
                           : view.special ? 0
                           : view.showingGrid ? (library.albumGrid.currentAlbum > 0 ? library.firstTrackOfAlbum(library.albumGrid.currentAlbum) : 0)
                           : library.tracks.currentRow >= 0 ? library.tracks.trackId(library.tracks.currentRow) : 0
            }
        }

        JobsBar {
            Layout.fillWidth: true
        }

        Loader {
            id: bottomBar
            objectName: "bottomBarSlot"
            Layout.fillWidth: true
            Layout.margins: 8
            Layout.topMargin: 6
            active: view.barPosition === "bottom"
            visible: active
            sourceComponent: view.barSize === "large" ? bigBarComponent : stripComponent
        }
    }

    Component {
        id: stripComponent
        DeckStrip {
            objectName: "deckStrip"
            showRemaining: view.showRemaining
            onClockClicked: view.clockClicked()
            onOpenRadio: view.action("radio")
            onAutoDjClicked: view.autoDjClicked()
            onPartyClicked: view.partyClicked()
            onCoverClicked: view.action("big-view")
            onOpenPlayer: view.openPlayer()
        }
    }
    Component {
        id: bigBarComponent
        BigBar {
            showRemaining: view.showRemaining
            sidebarOn: view.infoVisible
            queueOn: view.nowPlaying
            onClockClicked: view.clockClicked()
            onOpenRadio: view.action("radio")
            onCoverClicked: view.action("big-view")
            onToggleSidebar: view.toggleInfo()
            onShowQueue: view.action("now-playing")
            onShowLyrics: view.action("lyrics")
            onOpenMini: view.action("mini-player")
            onOpenPlayer: view.openPlayer()
        }
    }
}
