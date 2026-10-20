import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// The Spotify node of the library tree: your playlists, Liked Songs, saved
// albums and followed artists on the left, with search and a box for a
// pasted Spotify link; the chosen list on the right, with a button for
// each thing a row can do. What Spotify is playing shows along the top.
//
// Enter or a double-click plays a track (the list from it, as the library
// does) or opens a playlist, album or artist; Backspace goes back; Left
// returns to the tree.
FocusScope {
    id: view
    objectName: "spotifyView"
    signal leave()
    signal openSettings()

    readonly property var current: list.currentIndex >= 0 && list.currentIndex < spotify.items.count
                                   ? spotify.items.get(list.currentIndex) : null
    readonly property bool currentIsTrack: current !== null && current.kind === "track"

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
                            text: (spotify.nowPlaying.playing ? qsTr("Playing on Spotify: ") : qsTr("Paused on Spotify: ")) + (spotify.nowPlaying.title || "")
                            elide: Text.ElideRight
                            font.pixelSize: 13
                            font.bold: true
                            color: theme.c.bright_foreground
                        }
                        Text {
                            Layout.fillWidth: true
                            text: (spotify.nowPlaying.artists || "") + (spotify.nowPlaying.device ? "  ·  " + qsTr("on %1").arg(spotify.nowPlaying.device) : "")
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
                    ToolTip.text: qsTr("Read this list from Spotify again. Omamp keeps lists for ten minutes, so it asks Spotify less often.")
                    onClicked: spotify.reload()
                }
                Button {
                    objectName: "spotifyPlay"
                    text: view.currentIsTrack ? qsTr("Play") : qsTr("Play all")
                    enabled: view.current !== null
                    font.pixelSize: 12
                    onClicked: spotify.playRow(list.currentIndex)
                }
                Button {
                    objectName: "spotifyPlayNext"
                    text: qsTr("Play next")
                    enabled: view.current !== null
                    font.pixelSize: 12
                    onClicked: spotify.playNextRow(list.currentIndex)
                }
                Button {
                    objectName: "spotifyEnqueue"
                    text: qsTr("Add to Now Playing")
                    enabled: view.current !== null
                    font.pixelSize: 12
                    onClicked: spotify.enqueueRow(list.currentIndex)
                }
                Button {
                    id: addButton
                    objectName: "spotifyAddToPlaylist"
                    text: qsTr("Add to playlist ▾")
                    enabled: view.current !== null && (view.current.kind === "track" || view.current.kind === "album")
                             && spotify.ownPlaylists.length > 0
                    font.pixelSize: 12
                    onClicked: playlistMenu.popup(addButton, 0, addButton.height)
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
                visible: spotify.viewNote !== ""
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
                            text: qsTr("Play")
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
                            text: qsTr("Play next")
                            font.pixelSize: 12
                            onClicked: spotify.enqueueView(true)
                        }
                        Button {
                            text: qsTr("Add to Now Playing")
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
                currentIndex: count > 0 ? 0 : -1
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
                Keys.onReturnPressed: view.activate(currentIndex)
                Keys.onEnterPressed: view.activate(currentIndex)
                Keys.onUpPressed: decrementCurrentIndex()
                Keys.onDownPressed: incrementCurrentIndex()
                Keys.onLeftPressed: view.leave()
                Keys.onPressed: (event) => {
                    if (event.key === Qt.Key_Backspace) { spotify.back(); event.accepted = true }
                }

                delegate: Rectangle {
                    id: row
                    required property int index
                    required property string kind
                    required property string name
                    required property string detail
                    required property string duration
                    required property string art
                    width: ListView.view.width
                    height: 52
                    color: ListView.isCurrentItem ? theme.listSelection
                         : hover.hovered ? Qt.alpha(theme.listSelection, 0.4) : "transparent"
                    Component.onCompleted: spotify.items.wantArt(index)
                    HoverHandler { id: hover }

                    RowLayout {
                        anchors { fill: parent; leftMargin: 12; rightMargin: 14 }
                        spacing: 12
                        Rectangle {
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
                                text: row.kind === "track" ? row.detail
                                    : row.detail !== "" ? row.detail : ""
                                elide: Text.ElideRight
                                font.pixelSize: 12
                                color: theme.c.muted
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
                        onClicked: (mouse) => {
                            list.currentIndex = row.index
                            list.forceActiveFocus()
                            if (mouse.button === Qt.RightButton) rowMenu.popup()
                        }
                        onDoubleClicked: view.activate(row.index)
                    }
                }
            }
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
        MenuItem { text: view.currentIsTrack ? qsTr("Play from here") : qsTr("Play all"); onTriggered: spotify.playRow(list.currentIndex) }
        MenuItem { text: qsTr("Play next"); onTriggered: spotify.playNextRow(list.currentIndex) }
        MenuItem { text: qsTr("Add to Now Playing"); onTriggered: spotify.enqueueRow(list.currentIndex) }
        MenuItem { text: qsTr("Open"); visible: !view.currentIsTrack; height: visible ? implicitHeight : 0
                   onTriggered: spotify.openRow(list.currentIndex) }
        MenuItem { text: qsTr("Add to playlist…"); enabled: addButton.enabled
                   onTriggered: playlistMenu.popup() }
    }
    Menu {
        id: playlistMenu
        Repeater {
            model: spotify.ownPlaylists
            MenuItem {
                required property var modelData
                text: modelData.name
                onTriggered: spotify.addRowToPlaylist(list.currentIndex, modelData.id)
            }
        }
    }
}
