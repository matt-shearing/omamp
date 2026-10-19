import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// The deck folded into one strip across the top of advanced mode: the
// clock, a small analyser, the title with the position under it, the
// transport and the volume. The Winamp look stays here; the library below
// is plain.
Rectangle {
    id: strip
    property bool showRemaining: false
    signal clockClicked()
    // The Auto-DJ and party lamps: a click switches Auto-DJ, or asks for
    // the PIN to leave party mode.
    signal autoDjClicked()
    signal partyClicked()
    // The cover: opens the big Now Playing view.
    signal coverClicked()

    readonly property string mono: "JetBrainsMono Nerd Font"

    function clock(ms) {
        if (ms === undefined || ms < 0 || isNaN(ms)) ms = 0
        const total = Math.floor(ms / 1000)
        const h = Math.floor(total / 3600), m = Math.floor(total % 3600 / 60), s = total % 60
        const mm = (h > 0 && m < 10 ? "0" : "") + m
        return (h > 0 ? h + ":" : "") + (mm.length < 2 ? "0" + mm : mm) + ":" + (s < 10 ? "0" : "") + s
    }

    implicitHeight: 64
    color: theme.c.dark_background
    border.width: 1
    border.color: theme.c.lighter_background
    radius: theme.radius

    RowLayout {
        anchors.fill: parent
        anchors.margins: 8
        spacing: 10

        // The cover of what is playing; a click opens the big view.
        CoverArt {
            objectName: "stripCover"
            visible: appearance.coverInDeck
            Layout.preferredWidth: 48
            Layout.preferredHeight: 48
            source: nowPlayingArt.thumbUrl
            onClicked: strip.coverClicked()
            ToolTip.visible: coverHover.hovered
            ToolTip.delay: 600
            ToolTip.text: qsTr("Big Now Playing view (N)")
            HoverHandler { id: coverHover }
        }

        // Clock and analyser share one LCD, as on the deck.
        Rectangle {
            Layout.preferredWidth: 222
            Layout.fillHeight: true
            color: theme.c.darker_background
            border.width: 1
            border.color: theme.c.lighter_background
            radius: Math.min(4, theme.radius)

            Text {
                id: glyph
                anchors { left: parent.left; leftMargin: 8; verticalCenter: parent.verticalCenter }
                text: String.fromCodePoint(backend.state === "playing" ? 0xF040A
                                          : backend.state === "paused" ? 0xF03E4 : 0xF04DB)
                font.family: strip.mono
                font.pixelSize: 13
                color: backend.state === "playing" ? theme.c.green : theme.c.muted
            }
            Text {
                id: time
                anchors { left: glyph.right; leftMargin: 6; verticalCenter: parent.verticalCenter }
                width: 92
                text: (strip.showRemaining ? "-" : "") + strip.clock(strip.showRemaining
                      ? backend.duration - backend.position : backend.position)
                font.family: strip.mono
                font.pixelSize: 24
                font.bold: true
                color: theme.c.green
                opacity: backend.state === "paused" ? (blink.on ? 1 : 0.15) : 1
                Timer { id: blink; property bool on: true; interval: 600; repeat: true
                        running: backend.state === "paused"; onTriggered: on = !on }
                MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: strip.clockClicked() }
            }
            Analyser {
                anchors { left: time.right; leftMargin: 4; right: parent.right; rightMargin: 8
                          top: parent.top; bottom: parent.bottom; margins: 7 }
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 4
            Text {
                Layout.fillWidth: true
                elide: Text.ElideRight
                text: backend.currentIndex < 0 ? qsTr("Nothing playing")
                      : (backend.artist ? backend.artist + " - " : "") + backend.title
                        + (backend.duration > 0 ? "  (" + strip.clock(backend.duration) + ")" : "")
                font.family: strip.mono
                font.pixelSize: 13
                color: backend.currentIndex < 0 ? theme.c.muted : theme.c.green
            }
            Bar {
                Layout.fillWidth: true
                active: backend.duration > 0
                value: backend.duration > 0 ? backend.position / backend.duration : 0
                onReleased: (v) => backend.seek(v * backend.duration)
            }
        }

        Row {
            spacing: 3
            DeckButton { glyph: 0xF04AE; implicitWidth: 32; onClicked: backend.previous() }
            DeckButton { glyph: 0xF040A; implicitWidth: 32; lit: backend.state === "playing"; onClicked: backend.play() }
            DeckButton { glyph: 0xF03E4; implicitWidth: 32; lit: backend.state === "paused"; onClicked: backend.pause() }
            DeckButton { glyph: 0xF04DB; implicitWidth: 32; onClicked: backend.stop() }
            DeckButton { glyph: 0xF04AD; implicitWidth: 32; onClicked: backend.next() }
        }

        Column {
            spacing: 4
            ToggleLamp {
                objectName: "autoDjLamp"
                label: qsTr("AUTO-DJ")
                on: library.dj.enabled
                width: Math.max(implicitWidth, partyLamp.implicitWidth)
                ToolTip.visible: hoverDj.hovered
                ToolTip.delay: 600
                ToolTip.text: library.dj.enabled ? qsTr("Auto-DJ is keeping the queue full from %1 (Ctrl+J)").arg(library.dj.sourceLabel)
                                                 : qsTr("Auto-DJ: keep the queue full by itself (Ctrl+J)")
                HoverHandler { id: hoverDj }
                onToggled: strip.autoDjClicked()
            }
            ToggleLamp {
                id: partyLamp
                objectName: "partyLamp"
                label: qsTr("PARTY")
                on: library.dj.partyOn
                width: Math.max(implicitWidth, parent.children[0].implicitWidth)
                onToggled: strip.partyClicked()
            }
        }

        RowLayout {
            spacing: 6
            Text {
                text: String.fromCodePoint(backend.volume === 0 ? 0xF075F
                                           : backend.volume < 50 ? 0xF0580 : 0xF057E)
                font.family: strip.mono; font.pixelSize: 15
                color: theme.c.light_foreground
            }
            Bar {
                Layout.preferredWidth: 90
                value: backend.volume / 100
                fill: theme.c.cyan
                onMoved: (v) => backend.volume = Math.round(v * 100)
            }
        }
    }
}
