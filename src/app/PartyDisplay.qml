import QtQuick
import QtQuick.Layouts

// Party mode's big now-playing display: the cover, the title and artist
// large enough to read across a room, what comes next, and how to leave.
// It covers the window; the keys that party mode allows still work.
Rectangle {
    id: display
    objectName: "partyDisplay"
    signal unlock()
    color: theme.c.darker_background

    function clock(ms) {
        const total = Math.floor(Math.max(0, ms) / 1000)
        const m = Math.floor(total / 60), s = total % 60
        return m + ":" + (s < 10 ? "0" : "") + s
    }
    readonly property int nextRow: backend.currentIndex + 1
    readonly property bool hasNext: nextRow > 0 && nextRow < backend.playlist.count

    // Swallows clicks, so nothing underneath is touched.
    MouseArea { anchors.fill: parent }

    RowLayout {
        anchors.centerIn: parent
        width: Math.min(parent.width - 80, 1400)
        spacing: 48

        Rectangle {
            Layout.preferredWidth: Math.min(display.height * 0.55, display.width * 0.35)
            Layout.preferredHeight: Layout.preferredWidth
            color: theme.c.dark_background
            radius: theme.radius
            clip: true
            Image {
                anchors.fill: parent
                fillMode: Image.PreserveAspectCrop
                asynchronous: true
                source: library.ready && library.playingTrackId > 0
                        ? "image://omamp-art/track/" + library.playingTrackId + "/512?v=" + library.revision : ""
            }
            Text {
                anchors.centerIn: parent
                visible: parent.children[0].status !== Image.Ready
                text: String.fromCodePoint(0xF075A)
                font.family: "JetBrainsMono Nerd Font"
                font.pixelSize: 96
                color: theme.c.lighter_background
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 14
            Text {
                Layout.fillWidth: true
                text: backend.currentIndex < 0 ? qsTr("Nothing playing") : backend.title
                wrapMode: Text.Wrap
                maximumLineCount: 3
                elide: Text.ElideRight
                font.pixelSize: Math.max(28, Math.min(64, display.width / 22))
                font.bold: true
                color: theme.c.bright_foreground
            }
            Text {
                Layout.fillWidth: true
                text: backend.artist + (backend.album ? "  —  " + backend.album : "")
                wrapMode: Text.Wrap
                maximumLineCount: 2
                elide: Text.ElideRight
                font.pixelSize: Math.max(20, Math.min(36, display.width / 40))
                color: theme.accent
            }
            Bar {
                Layout.fillWidth: true
                Layout.topMargin: 12
                active: false
                value: backend.duration > 0 ? backend.position / backend.duration : 0
            }
            Text {
                text: display.clock(backend.position) + " / " + display.clock(backend.duration)
                font.family: "JetBrainsMono Nerd Font"
                font.pixelSize: 20
                color: theme.c.light_foreground
            }
            Text {
                Layout.topMargin: 24
                Layout.fillWidth: true
                visible: display.hasNext
                elide: Text.ElideRight
                readonly property var next: display.hasNext ? backend.trackAt(display.nextRow) : ({})
                text: display.hasNext ? qsTr("Next: %1").arg((next.artist ? next.artist + " - " : "") + next.title) : ""
                font.pixelSize: 20
                color: theme.c.muted
            }
        }
    }

    Text {
        anchors { right: parent.right; bottom: parent.bottom; margins: 20 }
        text: qsTr("Party mode · Ctrl+K for the palette · click here to leave")
        font.pixelSize: 13
        color: theme.c.muted
        MouseArea { anchors.fill: parent; anchors.margins: -8; onClicked: display.unlock() }
    }
}
