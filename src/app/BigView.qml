import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// The big Now Playing view (N): the cover large, the title, artist, album
// and year, the rating, the position with its times, the transport, what
// plays next, the lyrics when the track has timed ones, and a visualiser
// along the bottom. It fills the window; F11 makes the window fullscreen
// and Esc comes back. Party mode's display is this view with the controls
// guests may not use taken away.
//
// The background is the cover blurred on the CPU (nowPlayingArt.blurUrl, a
// 64-pixel picture stretched smoothly) under a shade of the style's own
// colour, or just that colour, as Settings > Appearance says. No GPU effect
// is used, so it looks the same with the software renderer.
Rectangle {
    id: view
    objectName: "bigView"
    property var skin: appearance.deck
    // Party mode: no closing, and only what the party allows.
    property bool party: false
    property bool fullScreen: false
    signal close()
    signal toggleFullScreen()
    signal unlock()

    // The library's details of the playing track, when it is in the
    // library (the year and the rating); the library opens for them.
    readonly property var trackId: nowPlayingArt.trackId
    readonly property var info: visible && library.ready && trackId > 0 && library.revision >= 0
                                ? library.trackInfo(trackId) : ({})
    readonly property bool inLibrary: info.id !== undefined
    onVisibleChanged: if (visible && trackId > 0) library.activate()
    onTrackIdChanged: if (visible && trackId > 0) library.activate()

    function allowed(action) { void library.dj.partyOn; return !party || library.dj.allowed(action) }
    function clock(ms) {
        if (ms === undefined || ms < 0 || isNaN(ms)) ms = 0
        const total = Math.floor(ms / 1000)
        const h = Math.floor(total / 3600), m = Math.floor(total % 3600 / 60), s = total % 60
        return (h > 0 ? h + ":" + (m < 10 ? "0" : "") : "") + m + ":" + (s < 10 ? "0" : "") + s
    }

    readonly property real margin: Math.max(16, Math.round(width * 0.03))
    readonly property bool wide: content.width > content.height * 1.25
    readonly property real titleSize: Math.max(22, Math.min(54, Math.round(Math.min(width / 26, height / 14))))

    color: skin.bigShade

    // Swallows clicks, so nothing underneath is touched.
    MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons }

    // ---- The background --------------------------------------------------
    Image {
        id: backdrop
        objectName: "bigBackdrop"
        anchors.fill: parent
        visible: appearance.bigBackground === "blur" && status === Image.Ready
        source: appearance.bigBackground === "blur" ? nowPlayingArt.blurUrl : ""
        fillMode: Image.PreserveAspectCrop
        smooth: true
        asynchronous: true
    }
    Rectangle {
        anchors.fill: parent
        visible: backdrop.visible
        color: view.skin.bigShade
        opacity: view.skin.light ? 0.62 : 0.58
    }

    // ---- The top line: what this is, and the view's own buttons -----------
    RowLayout {
        id: topBar
        anchors { left: parent.left; right: parent.right; top: parent.top; margins: view.margin }
        height: 30
        spacing: 8

        Text {
            Layout.fillWidth: true
            text: view.party ? qsTr("Party mode") : qsTr("Now playing")
            elide: Text.ElideRight
            font.pixelSize: 13
            font.bold: true
            font.letterSpacing: 2
            font.capitalization: Font.AllUppercase
            color: view.skin.bigSubtext
        }
        BigButton {
            objectName: "bigLyricsButton"
            text: appearance.bigLyrics ? qsTr("Hide lyrics") : qsTr("Show lyrics")
            tip: qsTr("Lyrics on or off (Shift+L)")
            visible: view.inLibrary
            onClicked: appearance.bigLyrics = !appearance.bigLyrics
        }
        BigButton {
            objectName: "bigVisualiserButton"
            text: appearance.visualiserName(appearance.visualiser)
            tip: qsTr("Next visualiser (Shift+V, or click it)")
            onClicked: appearance.nextVisualiser()
        }
        BigButton {
            objectName: "bigFullScreenButton"
            visible: !view.party
            text: view.fullScreen ? qsTr("Leave fullscreen") : qsTr("Fullscreen")
            tip: "F11"
            onClicked: view.toggleFullScreen()
        }
        BigButton {
            objectName: "bigCloseButton"
            visible: !view.party
            text: qsTr("Close")
            tip: "Esc"
            onClicked: view.close()
        }
    }

    // ---- Cover, words and controls ---------------------------------------
    Item {
        id: content
        anchors { left: parent.left; right: parent.right; top: topBar.bottom; bottom: visualiser.top
                  leftMargin: view.margin; rightMargin: view.margin; topMargin: view.margin * 0.6; bottomMargin: view.margin * 0.6 }

        // The lyrics column shows only when the track has words to show.
        readonly property bool lyricsRoom: appearance.bigLyrics && view.inLibrary && view.wide && width > 980
        readonly property bool lyricsShown: lyricsRoom && wideLyrics.hasLines
        readonly property real coverSize: view.wide ? Math.min(height, width * (lyricsShown ? 0.32 : 0.4))
                                                    : Math.min(width, height * 0.42)

        CoverArt {
            id: cover
            objectName: "bigCover"
            skin: view.skin
            source: nowPlayingArt.artUrl
            clickable: false
            width: content.coverSize
            height: width
            x: view.wide ? 0 : (content.width - width) / 2
            y: view.wide ? (content.height - height) / 2 : 0
        }

        ColumnLayout {
            id: words
            spacing: Math.round(view.titleSize * 0.22)
            x: view.wide ? cover.width + view.margin * 1.4 : 0
            y: view.wide ? Math.max(0, (content.height - implicitHeight) / 2) : cover.height + view.margin * 0.8
            width: view.wide ? (content.lyricsShown ? content.width * 0.36 : Math.min(760, content.width - x)) : content.width
            height: view.wide ? Math.min(implicitHeight, content.height) : content.height - y
            clip: true

            Text {
                objectName: "bigTitle"
                Layout.fillWidth: true
                text: backend.currentIndex < 0 ? qsTr("Nothing playing") : backend.title
                wrapMode: Text.Wrap
                maximumLineCount: 2
                elide: Text.ElideRight
                horizontalAlignment: view.wide ? Text.AlignLeft : Text.AlignHCenter
                font.pixelSize: view.titleSize
                font.bold: true
                color: view.skin.bigText
            }
            Text {
                objectName: "bigArtist"
                Layout.fillWidth: true
                visible: text !== ""
                text: backend.artist
                elide: Text.ElideRight
                horizontalAlignment: view.wide ? Text.AlignLeft : Text.AlignHCenter
                font.pixelSize: Math.round(view.titleSize * 0.56)
                color: view.skin.accent
            }
            Text {
                objectName: "bigAlbum"
                Layout.fillWidth: true
                visible: text !== ""
                readonly property int year: view.info.year || 0
                text: backend.album + (year > 0 ? (backend.album ? "  ·  " : "") + year : "")
                elide: Text.ElideRight
                horizontalAlignment: view.wide ? Text.AlignLeft : Text.AlignHCenter
                font.pixelSize: Math.round(view.titleSize * 0.42)
                color: view.skin.bigSubtext
            }

            // The rating: click a star to set it, written to the library and,
            // when the settings say so, to the file.
            Row {
                objectName: "bigStars"
                Layout.alignment: view.wide ? Qt.AlignLeft : Qt.AlignHCenter
                visible: view.inLibrary
                spacing: 2
                readonly property int rating: view.info.rating >= 0 ? view.info.rating : -1
                Repeater {
                    model: 5
                    Text {
                        required property int index
                        readonly property real fill: Math.max(0, Math.min(1, (parent.rating - index * 20) / 20))
                        text: String.fromCodePoint(fill >= 1 ? 0xF04CE : fill >= 0.5 ? 0xF04D0 : 0xF04D2)
                        font.family: "JetBrainsMono Nerd Font"
                        font.pixelSize: Math.max(18, Math.round(view.titleSize * 0.5))
                        color: fill > 0 ? view.skin.accent : Qt.alpha(view.skin.bigSubtext, 0.6)
                        MouseArea {
                            anchors.fill: parent
                            enabled: view.allowed("edit-tags")
                            cursorShape: Qt.PointingHandCursor
                            onClicked: library.tagEditor.quickSet(view.trackId, "rating", String((index + 1) * 20))
                        }
                    }
                }
            }

            // Where the track is, and how long it is.
            ColumnLayout {
                Layout.fillWidth: true
                Layout.topMargin: 6
                spacing: 2
                Bar {
                    objectName: "bigSeek"
                    skin: view.skin
                    Layout.fillWidth: true
                    active: backend.duration > 0 && view.allowed("seek")
                    value: backend.duration > 0 ? backend.position / backend.duration : 0
                    onReleased: (v) => backend.seek(v * backend.duration)
                }
                RowLayout {
                    Layout.fillWidth: true
                    Text {
                        text: view.clock(backend.position)
                        font.family: "JetBrainsMono Nerd Font"
                        font.pixelSize: 14
                        color: view.skin.bigSubtext
                    }
                    Item { Layout.fillWidth: true }
                    Text {
                        text: (backend.duration > 0 ? "-" + view.clock(backend.duration - backend.position) + "  /  " : "")
                              + view.clock(backend.duration)
                        font.family: "JetBrainsMono Nerd Font"
                        font.pixelSize: 14
                        color: view.skin.bigSubtext
                    }
                }
            }

            // The transport, the modes and the volume.
            RowLayout {
                Layout.alignment: view.wide ? Qt.AlignLeft : Qt.AlignHCenter
                spacing: 4
                DeckButton { skin: view.skin; glyph: 0xF04AE; enabled: view.allowed("skip"); onClicked: backend.previous() }
                DeckButton {
                    skin: view.skin
                    implicitWidth: 48
                    glyph: backend.state === "playing" ? 0xF03E4 : 0xF040A
                    lit: backend.state === "playing"
                    enabled: view.allowed("play")
                    onClicked: backend.toggle()
                }
                DeckButton { skin: view.skin; glyph: 0xF04DB; enabled: view.allowed("play"); onClicked: backend.stop() }
                DeckButton { skin: view.skin; glyph: 0xF04AD; enabled: view.allowed("skip"); onClicked: backend.next() }
                Item { width: 8 }
                ToggleLamp { skin: view.skin; label: qsTr("SHUFFLE"); on: backend.shuffle; enabled: view.allowed("reorder")
                             onToggled: backend.shuffle = !backend.shuffle }
                ToggleLamp { skin: view.skin; label: qsTr("REPEAT"); on: backend.repeat; enabled: view.allowed("reorder")
                             onToggled: backend.repeat = !backend.repeat }
                Item { width: 8 }
                Text {
                    text: String.fromCodePoint(backend.volume === 0 ? 0xF075F : backend.volume < 50 ? 0xF0580 : 0xF057E)
                    font.family: "JetBrainsMono Nerd Font"; font.pixelSize: 16
                    color: view.skin.bigSubtext
                }
                Bar {
                    skin: view.skin
                    Layout.preferredWidth: 90
                    active: view.allowed("volume")
                    value: backend.volume / 100
                    fill: view.skin.volumeFill
                    onMoved: (v) => backend.volume = Math.round(v * 100)
                }
            }

            // What plays next, in list order. Lyrics take this place in a
            // narrow window, when they are on.
            Text {
                Layout.topMargin: 10
                Layout.fillWidth: true
                visible: upNext.count > 0 && !narrowLyrics.visible
                text: backend.shuffle ? qsTr("Up next in the list (shuffle is on)") : qsTr("Up next")
                horizontalAlignment: view.wide ? Text.AlignLeft : Text.AlignHCenter
                font.pixelSize: 12
                font.bold: true
                font.letterSpacing: 1.5
                font.capitalization: Font.AllUppercase
                color: view.skin.bigSubtext
            }
            Repeater {
                id: upNext
                objectName: "bigUpNext"
                model: {
                    void backend.playlist.count
                    const rows = []
                    for (let r = backend.currentIndex + 1; r < backend.playlist.count && rows.length < 5; ++r)
                        rows.push(r)
                    return narrowLyrics.visible ? [] : rows
                }
                Item {
                    id: nextRow
                    required property var modelData
                    readonly property var track: { void backend.playlist.count; return backend.trackAt(modelData) }
                    Layout.fillWidth: true
                    implicitHeight: 24
                    RowLayout {
                        anchors.fill: parent
                        spacing: 10
                        Text {
                            text: (nextRow.modelData + 1) + "."
                            font.family: "JetBrainsMono Nerd Font"
                            font.pixelSize: 13
                            color: Qt.alpha(view.skin.bigSubtext, 0.8)
                        }
                        Text {
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                            text: (nextRow.track.artist ? nextRow.track.artist + " – " : "") + (nextRow.track.title || "")
                            font.pixelSize: 14
                            color: view.skin.bigText
                        }
                        Text {
                            text: nextRow.track.durationMs > 0 ? view.clock(nextRow.track.durationMs) : ""
                            font.family: "JetBrainsMono Nerd Font"
                            font.pixelSize: 13
                            color: view.skin.bigSubtext
                        }
                    }
                    MouseArea {
                        anchors.fill: parent
                        enabled: view.allowed("play")
                        cursorShape: Qt.PointingHandCursor
                        onDoubleClicked: backend.playIndex(nextRow.modelData)
                    }
                }
            }

            BigLyrics {
                id: narrowLyrics
                Layout.topMargin: 10
                Layout.fillWidth: true
                Layout.preferredHeight: Math.max(120, content.height - cover.height - 330)
                visible: !view.wide && appearance.bigLyrics && view.inLibrary && hasLines
                skin: view.skin
                trackId: view.trackId
                active: view.visible && !view.wide && appearance.bigLyrics && view.inLibrary
                centred: true
            }
            Item { Layout.fillHeight: true }
        }

        BigLyrics {
            id: wideLyrics
            objectName: "bigLyrics"
            visible: content.lyricsShown
            active: view.visible && content.lyricsRoom
            skin: view.skin
            trackId: view.trackId
            anchors { right: parent.right; top: parent.top; bottom: parent.bottom }
            width: content.width - words.x - words.width - view.margin * 1.4
        }
    }

    // ---- The visualiser --------------------------------------------------
    Analyser {
        id: visualiser
        objectName: "bigVisualiser"
        skin: view.skin
        anchors { left: parent.left; right: parent.right; bottom: footer.top; leftMargin: view.margin; rightMargin: view.margin }
        height: view.height > 420 ? Math.max(48, Math.min(170, Math.round(view.height * 0.16))) : 0
        visible: height > 0
        spacing: view.width > 900 ? 4 : 2
    }

    Item {
        id: footer
        anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
        height: view.party ? 44 : view.margin
        Text {
            anchors { right: parent.right; verticalCenter: parent.verticalCenter; rightMargin: view.margin }
            visible: view.party
            text: qsTr("Party mode · Ctrl+K for the palette · click here to leave")
            font.pixelSize: 13
            color: view.skin.bigSubtext
            MouseArea { anchors.fill: parent; anchors.margins: -8; onClicked: view.unlock() }
        }
    }

    // A plain text button for the top line.
    component BigButton: Rectangle {
        id: button
        property string text: ""
        property string tip: ""
        signal clicked()
        implicitWidth: label.implicitWidth + 20
        implicitHeight: 28
        radius: Math.min(4, view.skin.radius)
        color: area.containsMouse ? Qt.alpha(view.skin.bigText, 0.14) : Qt.alpha(view.skin.bigText, 0.06)
        border.width: 1
        border.color: Qt.alpha(view.skin.bigText, area.containsMouse ? 0.45 : 0.22)
        Text {
            id: label
            anchors.centerIn: parent
            text: button.text
            font.pixelSize: 12
            color: view.skin.bigText
        }
        MouseArea {
            id: area
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: button.clicked()
        }
        ToolTip.visible: area.containsMouse && tip !== ""
        ToolTip.delay: 600
        ToolTip.text: tip
    }
}
