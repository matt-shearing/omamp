import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Spotify's answers to the library search, under the track list: a row of
// tiles for tracks, albums, artists and playlists. Double-click a track to
// play it next, or anything else to open it in the Spotify view; the
// buttons do the same for the chosen tile. Tiles are chosen as rows are in
// the track lists: Shift+click for a range, Ctrl+click for one more.
Rectangle {
    id: strip
    objectName: "spotifyBlendStrip"
    color: theme.c.dark_background
    // A command run by the window (spotify-new-thrum-playlist:blend…).
    signal action(string id)
    readonly property string query: library.tracks.filter
    readonly property var rows: spotify.blendResults
    readonly property bool several: rows.selectedCount > 1
    readonly property var current: { void rows.count; return rows.currentRow >= 0 && rows.currentRow < rows.count
                                   ? rows.get(rows.currentRow) : null }
    onQueryChanged: spotify.blendQuery(query)
    Component.onCompleted: spotify.blendQuery(query)

    ColumnLayout {
        anchors { fill: parent; margins: 10 }
        spacing: 6

        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            Text {
                text: String.fromCodePoint(0xF04C7)
                font.family: "JetBrainsMono Nerd Font"
                font.pixelSize: 16
                color: theme.accent
            }
            Text {
                Layout.fillWidth: true
                text: spotify.blendLoading ? qsTr("Spotify: searching for “%1”…").arg(spotify.blendText)
                    : spotify.blendError !== "" ? qsTr("Spotify: %1").arg(spotify.blendError)
                    : spotify.blendResults.count === 0 ? qsTr("Spotify: nothing for “%1”").arg(spotify.blendText)
                    : qsTr("Spotify: %1 results for “%2”").arg(spotify.blendResults.count).arg(spotify.blendText)
                elide: Text.ElideRight
                font.pixelSize: 13
                font.bold: true
                color: spotify.blendError !== "" ? theme.c.yellow : theme.c.bright_foreground
            }
            Text {
                objectName: "blendSelectedCount"
                visible: strip.several
                text: qsTr("%Ln selected", "", strip.rows.selectedCount)
                font.pixelSize: 12
                color: theme.c.light_foreground
            }
            Button {
                text: strip.several || (strip.current && strip.current.kind === "track") ? qsTr("Play next") : qsTr("Open")
                enabled: strip.current !== null
                flat: true
                font.pixelSize: 12
                implicitHeight: 32; topInset: 0; bottomInset: 0
                onClicked: strip.several ? spotify.playChosen(strip.rows, "next") : spotify.openBlendRow(strip.rows.currentRow)
            }
            Button {
                text: qsTr("Add to Now Playing")
                enabled: strip.current !== null
                flat: true
                font.pixelSize: 12
                implicitHeight: 32; topInset: 0; bottomInset: 0
                onClicked: spotify.playChosen(strip.rows, "append")
            }
            Button {
                id: blendAdd
                objectName: "blendAddToPlaylist"
                text: qsTr("Add to playlist ▾")
                enabled: strip.current !== null && (strip.several ? strip.rows.chosenTrackCount > 0
                         : strip.current.kind === "track" || strip.current.kind === "album")
                flat: true
                font.pixelSize: 12
                implicitHeight: 32; topInset: 0; bottomInset: 0
                onClicked: blendMenu.popup(blendAdd, 0, blendAdd.height)
            }
        }

        ListView {
            id: tiles
            objectName: "spotifyBlendTiles"
            Layout.fillWidth: true
            Layout.fillHeight: true
            orientation: ListView.Horizontal
            spacing: 10
            clip: true
            model: spotify.blendResults
            currentIndex: strip.rows.currentRow
            keyNavigationEnabled: false
            highlightMoveDuration: 0
            Keys.onPressed: (event) => {
                const t = strip.rows
                const mods = event.modifiers & (Qt.ShiftModifier | Qt.ControlModifier)
                switch (event.key) {
                case Qt.Key_Left: t.moveCurrent(-1, mods); break
                case Qt.Key_Right: t.moveCurrent(1, mods); break
                case Qt.Key_A:
                    if (!(event.modifiers & Qt.ControlModifier)) return
                    t.selectAll()
                    break
                case Qt.Key_Escape:
                    if (t.selectedCount === 0) return
                    t.clearSelection()
                    break
                default: return
                }
                tiles.positionViewAtIndex(t.currentRow, ListView.Contain)
                event.accepted = true
            }
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.horizontal: ScrollBar {}
            delegate: Rectangle {
                id: tile
                required property int index
                required property string kind
                required property string name
                required property string detail
                required property string art
                required property bool rowSelected
                width: 128
                height: ListView.view.height - 8
                radius: Math.min(6, theme.radius)
                color: rowSelected ? theme.listSelection : tileHover.hovered ? Qt.alpha(theme.listSelection, 0.3) : "transparent"
                border.width: ListView.isCurrentItem && tiles.activeFocus ? 1 : 0
                border.color: theme.accent
                Component.onCompleted: spotify.blendResults.wantArt(index)
                HoverHandler { id: tileHover }
                ColumnLayout {
                    anchors { fill: parent; margins: 6 }
                    spacing: 3
                    Rectangle {
                        Layout.alignment: Qt.AlignHCenter
                        Layout.preferredWidth: Math.min(96, tile.height - 52)
                        Layout.preferredHeight: Layout.preferredWidth
                        radius: tile.kind === "artist" ? width / 2 : Math.min(4, theme.radius)
                        color: theme.c.lighter_background
                        clip: true
                        Image {
                            anchors.fill: parent
                            source: tile.art
                            visible: tile.art !== ""
                            fillMode: Image.PreserveAspectCrop
                            asynchronous: true
                            sourceSize: Qt.size(192, 192)
                        }
                        Text {
                            anchors.centerIn: parent
                            visible: tile.art === ""
                            text: String.fromCodePoint(tile.kind === "album" ? 0xF0025 : tile.kind === "artist" ? 0xF0803
                                                       : tile.kind === "playlist" ? 0xF0CB8 : 0xF075A)
                            font.family: "JetBrainsMono Nerd Font"
                            font.pixelSize: 26
                            color: theme.c.muted
                        }
                    }
                    Text {
                        Layout.fillWidth: true
                        text: tile.name
                        elide: Text.ElideRight
                        font.pixelSize: 12
                        color: theme.c.bright_foreground
                    }
                    Text {
                        Layout.fillWidth: true
                        text: tile.kind === "track" ? tile.detail.split(" — ")[0]
                            : (tile.kind === "album" ? qsTr("Album") : tile.kind === "artist" ? qsTr("Artist") : qsTr("Playlist"))
                        elide: Text.ElideRight
                        font.pixelSize: 11
                        color: theme.c.muted
                    }
                }
                MouseArea {
                    anchors.fill: parent
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    onPressed: (mouse) => {
                        tiles.forceActiveFocus()
                        if (mouse.button === Qt.RightButton) {
                            if (!strip.rows.isSelected(tile.index)) strip.rows.select(tile.index, 0)
                            const p = mapToItem(strip, mouse.x, mouse.y)
                            tileMenu.popup(strip, p.x, p.y)
                            return
                        }
                        strip.rows.select(tile.index, mouse.modifiers & (Qt.ShiftModifier | Qt.ControlModifier))
                    }
                    onDoubleClicked: (mouse) => { if (mouse.button === Qt.LeftButton) spotify.openBlendRow(tile.index) }
                }
            }
        }
    }

    Menu {
        id: tileMenu
        Material.roundedScale: theme.radius
        MenuItem { text: qsTr("Play next"); onTriggered: spotify.playChosen(strip.rows, "next") }
        MenuItem { text: qsTr("Add to Now Playing"); onTriggered: spotify.playChosen(strip.rows, "append") }
        SpotifyAddMenu {
            rows: strip.rows
            source: "blend"
            enabled: blendAdd.enabled
            onAction: (id) => strip.action(id)
        }
        MenuSeparator { }
        MenuItem { text: strip.several ? qsTr("Copy Spotify links") : qsTr("Copy Spotify link")
                   onTriggered: spotify.copyChosenLinks(strip.rows) }
    }
    SpotifyAddMenu {
        id: blendMenu
        objectName: "blendAddMenu"
        rows: strip.rows
        source: "blend"
        onAction: (id) => strip.action(id)
    }
}
