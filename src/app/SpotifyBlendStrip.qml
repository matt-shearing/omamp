import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Spotify's answers to the library search, under the track list: a row of
// tiles for tracks, albums, artists and playlists. Double-click a track to
// play it next, or anything else to open it in the Spotify view; the
// buttons do the same for the chosen tile.
Rectangle {
    id: strip
    objectName: "spotifyBlendStrip"
    color: theme.c.dark_background
    readonly property string query: library.tracks.filter
    readonly property var current: tiles.currentIndex >= 0 && tiles.currentIndex < spotify.blendResults.count
                                   ? spotify.blendResults.get(tiles.currentIndex) : null
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
            Button {
                text: strip.current && strip.current.kind === "track" ? qsTr("Play next") : qsTr("Open")
                enabled: strip.current !== null
                flat: true
                font.pixelSize: 12
                implicitHeight: 32; topInset: 0; bottomInset: 0
                onClicked: spotify.openBlendRow(tiles.currentIndex)
            }
            Button {
                text: qsTr("Add to Now Playing")
                enabled: strip.current !== null
                flat: true
                font.pixelSize: 12
                implicitHeight: 32; topInset: 0; bottomInset: 0
                onClicked: spotify.enqueueBlendRow(tiles.currentIndex)
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
            currentIndex: count > 0 ? 0 : -1
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.horizontal: ScrollBar {}
            delegate: Rectangle {
                id: tile
                required property int index
                required property string kind
                required property string name
                required property string detail
                required property string art
                width: 128
                height: ListView.view.height - 8
                radius: Math.min(6, theme.radius)
                color: ListView.isCurrentItem ? theme.listSelection : tileHover.hovered ? Qt.alpha(theme.listSelection, 0.4) : "transparent"
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
                    onClicked: tiles.currentIndex = tile.index
                    onDoubleClicked: spotify.openBlendRow(tile.index)
                }
            }
        }
    }
}
