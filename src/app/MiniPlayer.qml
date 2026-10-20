import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// The compact players, MediaMonkey's MiniPlayer and Winamp's windowshade:
//
//   mini   a 420 x 64 strip: cover, scrolling title, position, transport
//          and volume
//   micro  a 275 x 28 bar: state, clock, scrolling title, transport, and a
//          thin position line along the bottom
//
// Ctrl+M (or the button at the right) goes back to the full window.
// Wayland lets no window place itself or stay on top: on Hyprland the
// window asks Hyprland to (WindowPlacement), and a rule for the title
// "Omamp mini" / "Omamp micro" does it for `omamp mini` (docs/now-playing.md).
Rectangle {
    id: mini
    objectName: "miniPlayer"
    property string mode: "mini" // or "micro"
    property var skin: appearance.deck
    property bool showRemaining: false
    // The strip's own height. A window much taller than that is one the
    // window manager keeps large (a tiled window): the strip then sits along
    // the top, full width, with a line saying why, never in the middle of
    // empty space.
    property int stripHeight: micro ? 28 : 64
    readonly property bool large: height > stripHeight + 40
    readonly property int stripAreaHeight: large ? stripHeight : height
    signal restore()
    signal toggleRemaining()
    signal openBigView()

    readonly property string mono: "JetBrainsMono Nerd Font"
    readonly property bool micro: mode === "micro"
    readonly property string line: backend.currentIndex < 0 ? qsTr("Omamp — drop some music in")
                                  : (backend.artist ? backend.artist + " - " : "") + backend.title
                                    + (backend.live && backend.album ? "  ·  " + backend.album : "")
    function clock(ms) {
        if (ms === undefined || ms < 0 || isNaN(ms)) ms = 0
        const total = Math.floor(ms / 1000)
        const h = Math.floor(total / 3600), m = Math.floor(total % 3600 / 60), s = total % 60
        return (h > 0 ? h + ":" + (m < 10 ? "0" : "") : "") + (m < 10 && h === 0 ? "0" : "") + m + ":" + (s < 10 ? "0" : "") + s
    }

    color: skin.panel
    gradient: skin.gradient ? fill : null
    border.width: 1
    border.color: skin.panelBorder
    Gradient {
        id: fill
        GradientStop { position: 0.0; color: mini.skin.panelTop }
        GradientStop { position: 1.0; color: mini.skin.panelBottom }
    }
    Bevel { anchors.fill: parent; anchors.margins: 1; skin: mini.skin; visible: mini.skin.bevel }

    // A double-click on an empty part goes back to the full window, as
    // Winamp's windowshade does.
    MouseArea { anchors.fill: parent; onDoubleClicked: mini.restore() }

    // A title that scrolls when it doesn't fit, on the LCD colours.
    component Marquee: Rectangle {
        id: box
        property int pixelSize: 12
        color: mini.skin.lcd
        border.width: 1
        border.color: mini.skin.lcdBorder
        radius: mini.skin.smallRadius
        clip: true
        readonly property bool scrolls: metrics.width > width - 12
        TextMetrics { id: metrics; font: label.font; text: mini.line }
        TextMetrics { id: gap; font: label.font; text: "   ***   " }
        Text {
            id: label
            anchors.verticalCenter: parent.verticalCenter
            x: 6
            text: box.scrolls ? mini.line + "   ***   " + mini.line : mini.line
            font.family: mini.mono
            font.pixelSize: box.pixelSize
            color: mini.skin.lcdText
            NumberAnimation on x {
                id: scroll
                running: box.scrolls && box.visible
                loops: Animation.Infinite
                from: 6
                to: 6 - gap.width - metrics.width
                duration: Math.max(4000, (metrics.width + gap.width) * 35)
            }
            onTextChanged: { x = 6; if (scroll.running) scroll.restart() }
        }
    }

    // ---- Mini: 420 x 64 ----------------------------------------------------
    RowLayout {
        objectName: "miniLayout"
        anchors { left: parent.left; right: parent.right; top: parent.top }
        height: mini.stripAreaHeight - 2 * anchors.margins
        anchors.margins: 6
        spacing: 6
        visible: !mini.micro

        CoverArt {
            objectName: "miniCover"
            skin: mini.skin
            source: nowPlayingArt.thumbUrl
            Layout.preferredWidth: parent.height
            Layout.preferredHeight: parent.height
            onClicked: mini.openBigView()
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 3
            RowLayout {
                Layout.fillWidth: true
                spacing: 4
                Marquee {
                    objectName: "miniTitle"
                    Layout.fillWidth: true
                    Layout.preferredHeight: 22
                }
                Rectangle {
                    Layout.preferredWidth: miniClock.implicitWidth + 12
                    Layout.preferredHeight: 22
                    color: mini.skin.lcd
                    border.width: 1
                    border.color: mini.skin.lcdBorder
                    radius: mini.skin.smallRadius
                    Text {
                        id: miniClock
                        anchors.centerIn: parent
                        // A live station: LIVE and the time listened.
                        text: backend.live ? qsTr("LIVE") + " " + mini.clock(backend.position)
                              : (mini.showRemaining ? "-" : "") + mini.clock(mini.showRemaining ? backend.duration - backend.position
                                                                                           : backend.position)
                        font.family: mini.mono
                        font.pixelSize: 13
                        font.bold: true
                        color: mini.skin.lcdText
                    }
                    MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: mini.toggleRemaining() }
                }
            }
            Bar {
                objectName: "miniSeek"
                skin: mini.skin
                Layout.fillWidth: true
                implicitHeight: 12
                active: backend.duration > 0
                value: backend.duration > 0 ? backend.position / backend.duration : 0
                onReleased: (v) => backend.seek(v * backend.duration)
            }
        }

        ColumnLayout {
            Layout.fillHeight: true
            spacing: 3
            Row {
                spacing: 2
                DeckButton { skin: mini.skin; glyph: 0xF04AE; implicitWidth: 26; implicitHeight: 22; onClicked: backend.previous() }
                DeckButton { skin: mini.skin; glyph: backend.state === "playing" ? 0xF03E4 : 0xF040A
                             lit: backend.state === "playing"; implicitWidth: 30; implicitHeight: 22; onClicked: backend.toggle() }
                DeckButton { skin: mini.skin; glyph: 0xF04DB; implicitWidth: 26; implicitHeight: 22; onClicked: backend.stop() }
                DeckButton { skin: mini.skin; glyph: 0xF04AD; implicitWidth: 26; implicitHeight: 22; onClicked: backend.next() }
            }
            Bar {
                objectName: "miniVolume"
                skin: mini.skin
                implicitWidth: 114
                implicitHeight: 12
                value: backend.volume / 100
                fill: mini.skin.volumeFill
                onMoved: (v) => backend.volume = Math.round(v * 100)
            }
        }

        TitleButton {
            objectName: "miniRestore"
            Layout.alignment: Qt.AlignTop
            skin: mini.skin
            glyph: 0xF05AF // window-maximize
            tip: qsTr("Back to the full window (Ctrl+M)")
            onClicked: mini.restore()
        }
    }

    // ---- Micro: 275 x 28, Winamp's windowshade ---------------------------
    Item {
        objectName: "microLayout"
        anchors { left: parent.left; right: parent.right; top: parent.top }
        height: mini.stripAreaHeight - 2 * anchors.margins
        anchors.margins: 3
        visible: mini.micro

        Row {
            id: microRow
            anchors { left: parent.left; right: parent.right; top: parent.top }
            height: parent.height - 4
            spacing: 3

            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: String.fromCodePoint(backend.state === "playing" ? 0xF040A : backend.state === "paused" ? 0xF03E4 : 0xF04DB)
                font.family: mini.mono
                font.pixelSize: 10
                color: backend.state === "playing" ? mini.skin.lcdText : mini.skin.labelDim
            }
            Text {
                id: microClock
                anchors.verticalCenter: parent.verticalCenter
                text: backend.live ? qsTr("LIVE") + " " + mini.clock(backend.position)
                      : (mini.showRemaining ? "-" : "") + mini.clock(mini.showRemaining ? backend.duration - backend.position
                                                                                   : backend.position)
                font.family: mini.mono
                font.pixelSize: 11
                font.bold: true
                color: mini.skin.lcdText
                MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: mini.toggleRemaining() }
            }
            Marquee {
                objectName: "microTitle"
                anchors.verticalCenter: parent.verticalCenter
                width: microRow.width - x - microButtons.width - microRow.spacing
                height: 16
                pixelSize: 10
            }
            Row {
                id: microButtons
                anchors.verticalCenter: parent.verticalCenter
                spacing: 1
                Repeater {
                    model: [[0xF04AE, () => backend.previous()], [0xF040A, () => backend.play()],
                            [0xF03E4, () => backend.pause()], [0xF04DB, () => backend.stop()],
                            [0xF04AD, () => backend.next()]]
                    TitleButton {
                        required property var modelData
                        skin: mini.skin
                        width: 15; height: 16
                        glyph: modelData[0]
                        onClicked: modelData[1]()
                    }
                }
                Item { width: 2; height: 1 }
                TitleButton {
                    objectName: "microRestore"
                    skin: mini.skin
                    width: 15; height: 16
                    glyph: 0xF05AF
                    tip: qsTr("Back to the full window (Ctrl+M)")
                    onClicked: mini.restore()
                }
            }
        }

        // The position as a thin line along the bottom; a click seeks.
        Rectangle {
            objectName: "microSeek"
            anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
            height: 3
            color: mini.skin.sliderTrack
            Rectangle {
                width: backend.duration > 0 ? parent.width * Math.min(1, backend.position / backend.duration) : 0
                height: parent.height
                color: mini.skin.sliderFill
            }
            MouseArea {
                anchors.fill: parent
                anchors.topMargin: -3
                enabled: backend.duration > 0
                cursorShape: Qt.PointingHandCursor
                onClicked: (mouse) => backend.seek(Math.max(0, Math.min(1, mouse.x / width)) * backend.duration)
            }
        }
    }

    // The window manager kept the window large.
    Text {
        objectName: "compactHint"
        visible: mini.large
        anchors { left: parent.left; right: parent.right; top: parent.top; bottom: parent.bottom
                  topMargin: mini.stripHeight + 12; margins: 16 }
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.Wrap
        text: qsTr("Your window manager keeps this window large. Make it floating to shrink it.")
        font.pixelSize: 13
        color: mini.skin.labelDim
    }
}
