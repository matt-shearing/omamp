import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// The deck: the cover, the LCD with the clock and the analyser, the
// scrolling title, the sliders and the transport, and the playlist under
// them. It wears the deck style from Settings > Appearance (appearance.deck):
// the Omarchy theme, Classic Winamp grey, or 2000s Silver.
ColumnLayout {
    id: deck
    objectName: "deckView"
    property var skin: appearance.deck
    property bool showRemaining: false
    property int selected: -1
    property bool dropHover: false
    signal toggleRemaining()
    signal selectRow(int row)
    signal openBigView()
    signal openMini()
    signal openRadio()
    signal openLibrary()

    readonly property string mono: "JetBrainsMono Nerd Font"
    readonly property bool coverShown: appearance.coverInDeck

    function positionAt(row) { list.positionViewAtIndex(row, ListView.Contain) }
    function clock(ms) {
        if (ms === undefined || ms < 0 || isNaN(ms)) ms = 0
        const total = Math.floor(ms / 1000)
        const h = Math.floor(total / 3600), m = Math.floor(total % 3600 / 60), s = total % 60
        const mm = (h > 0 && m < 10 ? "0" : "") + m
        return (h > 0 ? h + ":" : "") + (mm.length < 2 ? "0" + mm : mm) + ":" + (s < 10 ? "0" : "") + s
    }

    spacing: 8

    // The deck: cover, display, analyser, sliders and transport.
    Panel {
        skin: deck.skin
        Layout.fillWidth: true
        Layout.preferredHeight: 206
        label: "O M A M P"

        // In the title bar, where Winamp keeps its windowshade button: the
        // library and the big Now Playing view, said in words, and the mini
        // player.
        tools: [
            TitleButton {
                objectName: "libraryButton"
                skin: deck.skin
                glyph: 0xF0CB8 // music box multiple
                label: qsTr("Library")
                tip: qsTr("Open the library (Tab)")
                onClicked: deck.openLibrary()
            },
            TitleButton {
                objectName: "bigViewButton"
                skin: deck.skin
                glyph: 0xF0293 // fullscreen
                label: qsTr("Now Playing")
                tip: qsTr("Big Now Playing view (N)")
                onClicked: deck.openBigView()
            },
            TitleButton {
                objectName: "miniButton"
                skin: deck.skin
                glyph: 0xF05B0 // window-minimize
                tip: qsTr("Mini player (Ctrl+M)")
                onClicked: deck.openMini()
            }
        ]

        ColumnLayout {
            anchors.fill: parent
            spacing: 8

            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                // The cover of what is playing; a click opens the big view.
                CoverArt {
                    objectName: "deckCover"
                    visible: deck.coverShown
                    skin: deck.skin
                    Layout.preferredWidth: 100
                    Layout.preferredHeight: 100
                    onClicked: deck.openBigView()
                }

                // The LCD: state, clock and analyser.
                Rectangle {
                    Layout.preferredWidth: deck.coverShown ? 144 : 176
                    Layout.preferredHeight: 100
                    color: deck.skin.lcd
                    border.width: 1
                    border.color: deck.skin.lcdBorder
                    radius: deck.skin.smallRadius

                    Bevel { anchors.fill: parent; skin: deck.skin; sunken: true; visible: deck.skin.bevel }

                    Text {
                        id: stateGlyph
                        x: 8; y: 8
                        text: String.fromCodePoint(backend.state === "playing" ? 0xF040A
                                                  : backend.state === "paused" ? 0xF03E4 : 0xF04DB)
                        font.family: deck.mono
                        font.pixelSize: 14
                        color: backend.state === "playing" ? deck.skin.lcdText : deck.skin.lcdDim
                    }
                    // On the air.
                    Rectangle {
                        objectName: "deckLiveBadge"
                        visible: backend.live
                        anchors { left: stateGlyph.left; top: stateGlyph.bottom; topMargin: 3 }
                        width: liveWord.implicitWidth + 8; height: 14
                        radius: Math.min(2, deck.skin.radius)
                        color: deck.skin.error
                        Text { id: liveWord; anchors.centerIn: parent; text: qsTr("LIVE"); color: "white"
                               font.family: deck.mono; font.pixelSize: 9; font.bold: true }
                    }

                    Text {
                        id: time
                        anchors { right: parent.right; rightMargin: 10; top: parent.top; topMargin: 2 }
                        // A live station has no time left: always the time listened.
                        text: (deck.showRemaining && !backend.live ? "-" : "") + deck.clock(deck.showRemaining && !backend.live
                              ? backend.duration - backend.position : backend.position)
                        font.family: deck.mono
                        font.pixelSize: deck.coverShown ? 31 : 34
                        font.bold: true
                        color: deck.skin.lcdText
                        // Blinks while paused, like the original.
                        opacity: backend.state === "paused" ? (blink.on ? 1 : 0.15) : 1
                        Timer { id: blink; property bool on: true; interval: 600; repeat: true
                                running: backend.state === "paused"; onTriggered: on = !on }
                        MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                                    onClicked: deck.toggleRemaining() }
                    }

                    // The analyser: a click moves on to the next style.
                    Analyser {
                        anchors { left: parent.left; right: parent.right; bottom: parent.bottom; margins: 8 }
                        height: 46
                        skin: deck.skin
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 100
                    spacing: 6

                    // The scrolling title, as Winamp shows it.
                    Rectangle {
                        id: marqueeBox
                        Layout.fillWidth: true
                        Layout.preferredHeight: 26
                        color: deck.skin.lcd
                        border.width: 1
                        border.color: deck.skin.lcdBorder
                        radius: deck.skin.smallRadius
                        clip: true

                        readonly property string line: backend.currentIndex < 0
                            ? qsTr("Omamp — drop some music in")
                            : (backend.currentIndex + 1) + ". " + (backend.artist ? backend.artist + " - " : "")
                              + backend.title + (backend.duration > 0 ? " (" + deck.clock(backend.duration) + ")" : "")
                              + (backend.live && backend.album ? "  ·  " + backend.album : "")
                        readonly property bool scrolls: one.width > width - 16

                        TextMetrics { id: one; font: marquee.font; text: marqueeBox.line }
                        Text {
                            id: marquee
                            anchors.verticalCenter: parent.verticalCenter
                            x: 8
                            text: marqueeBox.scrolls ? marqueeBox.line + "   ***   " + marqueeBox.line : marqueeBox.line
                            font.family: deck.mono
                            font.pixelSize: 13
                            color: deck.skin.lcdText
                            NumberAnimation on x {
                                id: scroll
                                running: marqueeBox.scrolls
                                loops: Animation.Infinite
                                from: 8
                                to: 8 - marqueeGap.width - one.width
                                duration: Math.max(4000, (one.width + marqueeGap.width) * 35)
                            }
                            onTextChanged: { x = 8; if (scroll.running) scroll.restart() }
                        }
                        TextMetrics { id: marqueeGap; font: marquee.font; text: "   ***   " }
                    }

                    // Bit rate, sample rate and channel lamps.
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 6
                        Repeater {
                            model: [
                                { value: backend.bitrate > 0 ? backend.bitrate : "", unit: "kbps" },
                                { value: backend.sampleRate > 0 ? Math.round(backend.sampleRate / 1000) : "", unit: "kHz" }
                            ]
                            Row {
                                required property var modelData
                                spacing: 4
                                Rectangle {
                                    width: 34; height: 18
                                    color: deck.skin.lcd
                                    border.width: 1; border.color: deck.skin.lcdBorder
                                    radius: Math.min(3, deck.skin.radius)
                                    Text {
                                        anchors.centerIn: parent
                                        text: modelData.value
                                        font.family: deck.mono; font.pixelSize: 11
                                        color: deck.skin.lcdText
                                    }
                                }
                                Text {
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: modelData.unit
                                    font.family: deck.mono; font.pixelSize: 9
                                    color: deck.skin.labelDim
                                }
                            }
                        }
                        Item { Layout.fillWidth: true }
                        Repeater {
                            model: [ { word: qsTr("mono"), lit: backend.channels === 1 },
                                     { word: qsTr("stereo"), lit: backend.channels >= 2 } ]
                            Text {
                                required property var modelData
                                text: modelData.word
                                font.family: deck.mono; font.pixelSize: 10; font.bold: modelData.lit
                                color: modelData.lit ? deck.skin.accent : deck.skin.lampBorder
                            }
                        }
                    }

                    // Volume.
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        Text {
                            text: String.fromCodePoint(backend.volume === 0 ? 0xF075F
                                                       : backend.volume < 50 ? 0xF0580 : 0xF057E)
                            font.family: deck.mono; font.pixelSize: 15
                            color: deck.skin.label
                        }
                        Bar {
                            skin: deck.skin
                            Layout.fillWidth: true
                            value: backend.volume / 100
                            fill: deck.skin.volumeFill
                            onMoved: (v) => backend.volume = Math.round(v * 100)
                        }
                        Text {
                            Layout.preferredWidth: 30
                            horizontalAlignment: Text.AlignRight
                            text: backend.volume + "%"
                            font.family: deck.mono; font.pixelSize: 10
                            color: deck.skin.labelDim
                        }
                    }
                    Item { Layout.fillHeight: true }
                }
            }

            // Position.
            Bar {
                id: seekBar
                skin: deck.skin
                Layout.fillWidth: true
                visible: !backend.live
                active: backend.duration > 0
                value: backend.duration > 0 ? backend.position / backend.duration : 0
                onReleased: (v) => backend.seek(v * backend.duration)
            }
            // A radio station: the dial instead of the position.
            LiveBar {
                objectName: "deckLiveBar"
                skin: deck.skin
                Layout.fillWidth: true
                Layout.preferredHeight: seekBar.implicitHeight
                visible: backend.live
                onOpenPicker: deck.openRadio()
            }

            // Transport.
            RowLayout {
                Layout.fillWidth: true
                spacing: 4
                DeckButton { skin: deck.skin; glyph: 0xF04AE; onClicked: backend.previous() }
                DeckButton { skin: deck.skin; glyph: 0xF040A; lit: backend.state === "playing"; onClicked: backend.play() }
                DeckButton { skin: deck.skin; glyph: 0xF03E4; lit: backend.state === "paused"; onClicked: backend.pause() }
                DeckButton { skin: deck.skin; glyph: 0xF04DB; onClicked: backend.stop() }
                DeckButton { skin: deck.skin; glyph: 0xF04AD; onClicked: backend.next() }
                Item { width: 4 }
                DeckButton { skin: deck.skin; glyph: 0xF01EA; implicitWidth: 32; onClicked: backend.openFolder() }
                Item { Layout.fillWidth: true }
                ToggleLamp { skin: deck.skin; label: qsTr("SHUFFLE"); on: backend.shuffle; onToggled: backend.shuffle = !backend.shuffle }
                ToggleLamp { skin: deck.skin; label: qsTr("REPEAT"); on: backend.repeat; onToggled: backend.repeat = !backend.repeat }
            }
        }
    }

    // The playlist.
    Panel {
        skin: deck.skin
        Layout.fillWidth: true
        Layout.fillHeight: true
        label: qsTr("P L A Y L I S T")

        Rectangle {
            id: listBox
            anchors { left: parent.left; right: parent.right; top: parent.top; bottom: footer.top; bottomMargin: 6 }
            color: deck.skin.field
            border.width: 1
            border.color: deck.dropHover ? deck.skin.accent : deck.skin.fieldBorder
            radius: deck.skin.smallRadius

            ListView {
                id: list
                anchors.fill: parent
                anchors.margins: 4
                clip: true
                model: backend.playlist
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                delegate: Rectangle {
                    id: row
                    required property int index
                    required property int number
                    required property string title
                    required property string artist
                    required property var duration
                    readonly property bool current: index === backend.currentIndex
                    width: ListView.view.width
                    height: 22
                    radius: Math.min(3, deck.skin.radius)
                    color: index === deck.selected ? deck.skin.selection
                         : hover.hovered ? deck.skin.hover : "transparent"

                    HoverHandler { id: hover }

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 6
                        anchors.rightMargin: 10
                        spacing: 6
                        Text {
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                            text: row.number + ". " + (row.artist ? row.artist + " - " : "") + row.title
                            font.family: deck.mono
                            font.pixelSize: 12
                            font.bold: row.current
                            color: row.current ? deck.skin.textCurrent : deck.skin.text
                        }
                        Text {
                            text: row.duration > 0 ? deck.clock(row.duration) : ""
                            font.family: deck.mono
                            font.pixelSize: 12
                            color: row.current ? deck.skin.textCurrent : deck.skin.muted
                        }
                    }

                    MouseArea {
                        anchors.fill: parent
                        onClicked: deck.selectRow(row.index)
                        onDoubleClicked: backend.playIndex(row.index)
                    }
                }
            }

            Column {
                anchors.centerIn: parent
                visible: backend.playlist.count === 0
                spacing: 10
                width: parent.width - 40
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: String.fromCodePoint(0xF0387)
                    font.family: deck.mono; font.pixelSize: 40
                    color: deck.skin.placeholder
                }
                Text {
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                    text: qsTr("Drop music here, or press O to add a folder.")
                    font.family: deck.mono; font.pixelSize: 12
                    color: deck.skin.muted
                }
            }
        }

        RowLayout {
            id: footer
            anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
            height: 24
            spacing: 6

            Text {
                Layout.fillWidth: true
                elide: Text.ElideRight
                text: backend.playlist.count === 0 ? ""
                      : qsTr("%n tracks", "", backend.playlist.count)
                        + (backend.playlist.totalKnownMs > 0 ? "  ·  " + deck.clock(backend.playlist.totalKnownMs) : "")
                font.family: deck.mono; font.pixelSize: 10
                color: deck.skin.labelDim
            }
            ToggleLamp { skin: deck.skin; label: qsTr("+ ADD"); onToggled: backend.openFolder() }
            ToggleLamp { skin: deck.skin; label: qsTr("− REM"); onToggled: { backend.remove(deck.selected); deck.selectRow(deck.selected) } }
            ToggleLamp { skin: deck.skin; label: qsTr("CLEAR"); onToggled: backend.clear() }
        }
    }

    // Something went wrong: said plainly, dismissed with a click.
    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: 28
        visible: backend.error !== ""
        color: Qt.alpha(deck.skin.error, 0.15)
        border.width: 1
        border.color: deck.skin.error
        radius: deck.skin.smallRadius
        Text {
            anchors.fill: parent
            anchors.leftMargin: 10
            anchors.rightMargin: 10
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
            text: backend.error
            font.family: deck.mono; font.pixelSize: 11
            color: deck.skin.error
        }
        MouseArea { anchors.fill: parent; onClicked: backend.dismissError() }
        Timer { running: backend.error !== ""; interval: 7000; onTriggered: backend.dismissError() }
    }
}
