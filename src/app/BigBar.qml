import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// The large now playing bar, as Spotify's desktop app has one across the
// bottom: the cover, the title and artist with the rating on the left, the
// transport with a big play button and the position across the middle, and
// the volume and the panel switches on the right. It takes the deck style's
// colours (Settings > Appearance), flat, with no bevels.
Rectangle {
    id: bar
    objectName: "bigBar"
    property var skin: appearance.deck
    property bool showRemaining: false
    // The view's switches, lit while on.
    property bool sidebarOn: false
    property bool queueOn: false
    signal coverClicked()
    signal clockClicked()
    signal openRadio()
    signal toggleSidebar()
    signal showQueue()
    signal showLyrics()
    signal openMini()
    signal openPlayer()

    readonly property bool playing: backend.state === "playing"
    // Where the visualiser sits (Settings > Appearance): "off", "behind" the
    // transport and seek bar, along the "edge" that faces the library, or
    // "beside" the cover. The edge strip makes the bar a little taller.
    readonly property string visPlace: appearance.barVisualiser
    readonly property bool atTop: appearance.layout.barPosition === "top"
    readonly property int edgeHeight: visPlace === "edge" ? 24 : 0
    readonly property var info: library.playingTrackId > 0 && library.revision >= 0 ? library.trackInfo(library.playingTrackId) : ({})

    function clock(ms) {
        if (ms === undefined || ms < 0 || isNaN(ms)) ms = 0
        const total = Math.floor(ms / 1000)
        const h = Math.floor(total / 3600), m = Math.floor(total % 3600 / 60), s = total % 60
        return (h > 0 ? h + ":" + (m < 10 ? "0" : "") : "") + m + ":" + (s < 10 ? "0" : "") + s
    }

    implicitHeight: 96 + edgeHeight
    color: skin.panel
    border.width: 1
    border.color: skin.panelBorder
    radius: Math.min(8, theme.radius)

    // A round button with a glyph; `big` is the play button.
    component RoundButton: Rectangle {
        id: rb
        property int glyph: 0
        property bool big: false
        property bool lit: false
        property string tip: ""
        signal clicked()
        implicitWidth: big ? 44 : 34
        implicitHeight: implicitWidth
        radius: width / 2
        color: big ? bar.skin.accent : hover.hovered ? Qt.alpha(bar.skin.text, 0.12) : "transparent"
        Text {
            anchors.centerIn: parent
            text: String.fromCodePoint(rb.glyph)
            font.family: "JetBrainsMono Nerd Font"
            font.pixelSize: rb.big ? 22 : 17
            color: rb.big ? bar.skin.panel : rb.lit ? bar.skin.accent : hover.hovered ? bar.skin.text : bar.skin.label
        }
        HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor }
        TapHandler { onTapped: rb.clicked() }
        ToolTip.visible: hover.hovered && tip !== ""
        ToolTip.delay: 600
        ToolTip.text: tip
    }

    // Behind the seek bar: a wide band from the middle of the transport
    // buttons down, faint enough that the buttons, the position and the
    // times stay easy to read.
    BarVisualiser {
        objectName: "barVisualiserBehind"
        skin: bar.skin
        x: row.x + middle.x
        y: Math.max(3, row.y + middle.y + transport.y + transport.height / 2)
        width: middle.width
        height: bar.height - y - 3
        opacity: 0.6
        visible: bar.visPlace === "behind" && !(idle && appearance.idleVisualiser === "hide")
    }

    // Along the edge that faces the library: the bottom when the bar is at
    // the top, the top when it is at the bottom.
    BarVisualiser {
        objectName: "barVisualiserEdge"
        skin: bar.skin
        x: 10
        width: bar.width - 20
        y: bar.atTop ? bar.height - height - 3 : 3
        height: bar.edgeHeight - 2
        visible: bar.visPlace === "edge" && !(idle && appearance.idleVisualiser === "hide")
    }

    RowLayout {
        id: row
        anchors { fill: parent; leftMargin: 12; rightMargin: 14
                  topMargin: 8 + (bar.atTop ? 0 : bar.edgeHeight); bottomMargin: 8 + (bar.atTop ? bar.edgeHeight : 0) }
        spacing: 14

        // Left: the cover and the words.
        RowLayout {
            Layout.preferredWidth: Math.max(220, bar.width * 0.28)
            Layout.maximumWidth: Math.max(220, bar.width * 0.28)
            Layout.fillHeight: true
            spacing: 12
            CoverArt {
                objectName: "bigBarCover"
                skin: bar.skin
                Layout.preferredWidth: 72
                Layout.preferredHeight: 72
                source: nowPlayingArt.coverUrl
                onClicked: bar.coverClicked()
                ToolTip.visible: coverHover.hovered
                ToolTip.delay: 600
                ToolTip.text: qsTr("Big Now Playing view (N)")
                HoverHandler { id: coverHover }
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2
                Text {
                    objectName: "bigBarTitle"
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                    text: backend.currentIndex < 0 ? qsTr("Nothing playing") : backend.title
                    font.pixelSize: 15
                    font.bold: true
                    color: bar.skin.text
                }
                Text {
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                    visible: text !== ""
                    text: backend.live ? (radio.nowPlaying || backend.artist) : backend.artist
                    font.pixelSize: 13
                    color: bar.skin.label
                }
                // The rating of a library track; a click sets it.
                Row {
                    visible: library.playingTrackId > 0
                    Repeater {
                        model: 5
                        Text {
                            required property int index
                            readonly property real fill: Math.max(0, Math.min(1, ((bar.info.rating !== undefined ? bar.info.rating : -1) - index * 20) / 20))
                            text: "★"
                            font.pixelSize: 14
                            rightPadding: 1
                            color: fill >= 1 ? bar.skin.accent : fill >= 0.5 ? Qt.alpha(bar.skin.accent, 0.5) : Qt.alpha(bar.skin.label, 0.35)
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: library.tagEditor.quickSet(library.playingTrackId, "rating", String((parent.index + 1) * 20))
                            }
                        }
                    }
                }
                // A Spotify track: Like.
                Text {
                    visible: spotify.nowPlaying.active === true && library.playingTrackId <= 0 && !backend.live
                    text: String.fromCodePoint(spotify.nowPlaying.liked ? 0xF02D1 : 0xF02D5) + "  " + (spotify.nowPlaying.liked ? qsTr("Liked") : qsTr("Like"))
                    font.family: "JetBrainsMono Nerd Font"
                    font.pixelSize: 12
                    color: spotify.nowPlaying.liked ? bar.skin.accent : bar.skin.label
                    MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                                onClicked: spotify.setPlayingLiked(!spotify.nowPlaying.liked) }
                }
            }
        }

        // Beside the cover and the words: a small panel of its own, when
        // the window is wide enough to spare it.
        Rectangle {
            objectName: "barVisualiserPanel"
            visible: bar.visPlace === "beside" && bar.width >= 1000
                     && !(appearance.idleVisualiser === "hide" && (backend.state !== "playing" || backend.playsElsewhere))
            Layout.preferredWidth: Math.min(240, bar.width * 0.14)
            Layout.preferredHeight: 64
            Layout.alignment: Qt.AlignVCenter
            radius: Math.min(6, theme.radius)
            color: Qt.alpha(bar.skin.text, 0.04)
            border.width: 1
            border.color: Qt.alpha(bar.skin.text, 0.08)
            BarVisualiser {
                objectName: "barVisualiserBeside"
                skin: bar.skin
                anchors { fill: parent; margins: 5 }
            }
        }

        // Middle: the transport, and the position under it.
        ColumnLayout {
            id: middle
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignVCenter
            spacing: 4
            RowLayout {
                id: transport
                Layout.alignment: Qt.AlignHCenter
                spacing: 10
                RoundButton { glyph: 0xF049D; lit: backend.shuffle; tip: qsTr("Shuffle (S)"); onClicked: backend.shuffle = !backend.shuffle }
                RoundButton { glyph: 0xF04AE; tip: qsTr("Previous (Z)"); onClicked: backend.previous() }
                RoundButton {
                    objectName: "bigBarPlay"
                    big: true
                    glyph: bar.playing ? 0xF03E4 : 0xF040A
                    tip: bar.playing ? qsTr("Pause (Space)") : qsTr("Play (Space)")
                    onClicked: bar.playing ? backend.pause() : backend.play()
                }
                RoundButton { glyph: 0xF04AD; tip: qsTr("Next (B)"); onClicked: backend.next() }
                RoundButton { glyph: 0xF0456; lit: backend.repeat; tip: qsTr("Repeat (R)"); onClicked: backend.repeat = !backend.repeat }
            }
            RowLayout {
                id: seekRow
                Layout.fillWidth: true
                Layout.maximumWidth: 720
                Layout.alignment: Qt.AlignHCenter
                spacing: 8
                Text {
                    Layout.preferredWidth: 48
                    horizontalAlignment: Text.AlignRight
                    visible: !backend.live
                    text: bar.clock(backend.position)
                    font.family: "JetBrainsMono Nerd Font"
                    font.pixelSize: 11
                    color: bar.skin.label
                }
                Bar {
                    Layout.fillWidth: true
                    visible: !backend.live
                    skin: bar.skin
                    fill: bar.skin.accent
                    active: backend.duration > 0
                    value: backend.duration > 0 ? backend.position / backend.duration : 0
                    onReleased: (v) => backend.seek(v * backend.duration)
                }
                Text {
                    Layout.preferredWidth: 48
                    visible: !backend.live
                    text: (bar.showRemaining ? "-" + bar.clock(backend.duration - backend.position) : bar.clock(backend.duration))
                    font.family: "JetBrainsMono Nerd Font"
                    font.pixelSize: 11
                    color: bar.skin.label
                    MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: bar.clockClicked() }
                }
                LiveBar {
                    Layout.fillWidth: true
                    visible: backend.live
                    onOpenPicker: bar.openRadio()
                }
            }
        }

        // Right: the panel switches and the volume.
        RowLayout {
            Layout.alignment: Qt.AlignVCenter
            spacing: 2
            RoundButton { objectName: "bigBarLyrics"; glyph: 0xF036C; tip: qsTr("Lyrics"); onClicked: bar.showLyrics() }
            RoundButton { objectName: "bigBarQueue"; glyph: 0xF0CB8; lit: bar.queueOn; tip: qsTr("Now Playing list"); onClicked: bar.showQueue() }
            RoundButton { objectName: "bigBarSidebar"; glyph: 0xF10AB; lit: bar.sidebarOn; tip: qsTr("Sidebar (I)"); onClicked: bar.toggleSidebar() }
            RoundButton { objectName: "bigBarMini"; glyph: 0xF0E57; tip: qsTr("Mini player (Ctrl+M)"); onClicked: bar.openMini() }
            Item { implicitWidth: 6 }
            Text {
                text: String.fromCodePoint(backend.volume === 0 ? 0xF075F : backend.volume < 50 ? 0xF0580 : 0xF057E)
                font.family: "JetBrainsMono Nerd Font"
                font.pixelSize: 16
                color: bar.skin.label
            }
            Bar {
                Layout.preferredWidth: 110
                skin: bar.skin
                value: backend.volume / 100
                fill: bar.skin.accent
                onMoved: (v) => backend.volume = Math.round(v * 100)
            }
            Item { implicitWidth: 6 }
            Button {
                objectName: "playerButton"
                text: qsTr("Player")
                flat: true
                font.pixelSize: 13
                implicitHeight: 34; topInset: 0; bottomInset: 0
                background: Rectangle {
                    radius: Math.min(4, theme.radius)
                    color: parent.hovered ? Qt.alpha(bar.skin.text, 0.12) : "transparent"
                    border.width: 1
                    border.color: Qt.alpha(bar.skin.text, 0.3)
                }
                contentItem: Text { text: parent.text; font: parent.font; color: bar.skin.text; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                ToolTip.visible: hovered
                ToolTip.delay: 600
                ToolTip.text: qsTr("Back to the player (Tab)")
                onClicked: bar.openPlayer()
            }
        }
    }
}
