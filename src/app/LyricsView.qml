import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// The info panel's Lyrics tab: the words of one track, from a .lrc file
// beside it, its tags, or LRCLIB. Synced lyrics follow the track while it
// plays, with the line being sung lit and kept in the middle.
Item {
    id: view
    objectName: "lyricsView"
    readonly property var web: library.web
    // The track to show; 0 shows nothing.
    property var trackId: 0
    // Only shown lyrics are looked up.
    property bool active: visible
    // The line follows the playing position while this track plays.
    property bool follow: trackId > 0 && trackId === library.playingTrackId
    property real position: backend.position
    readonly property bool shown: web.lyricsTrack === trackId && trackId > 0
    readonly property int currentLine: shown && follow && web.lyricsSynced ? web.lyricsLineAt(position) : -1
    signal editLyrics(var trackId)

    function load(fetch) { if (trackId > 0) web.loadLyrics(trackId, fetch === true) }
    onTrackIdChanged: if (active) load(false)
    onActiveChanged: if (active && !shown) load(false)
    Component.onCompleted: if (active) load(false)
    // A tag save changes what the file holds.
    Connections {
        target: library
        function onRevisionChanged() { if (view.active && view.shown && view.web.lyricsState !== "loading") view.load(false) }
    }
    onCurrentLineChanged: if (currentLine >= 0) lines.positionViewAtIndex(currentLine, ListView.Center)

    ColumnLayout {
        anchors.fill: parent
        spacing: 8

        // Whose words these are.
        Text {
            objectName: "lyricsTitle"
            Layout.fillWidth: true
            visible: view.trackId > 0
            elide: Text.ElideRight
            text: {
                if (!(view.trackId > 0)) return ""
                const i = library.trackInfo(view.trackId)
                return (i.title || "") + (i.artist ? "  —  " + i.artist : "")
            }
            font.pixelSize: 14
            font.bold: true
            color: theme.c.bright_foreground
        }
        Text {
            Layout.fillWidth: true
            visible: view.shown && view.web.lyricsState === "found"
            wrapMode: Text.Wrap
            text: (view.web.lyricsSource === "sidecar" ? qsTr("From the .lrc file beside the track")
                   : view.web.lyricsSource === "embedded" ? qsTr("From the file's tags")
                   : view.web.lyricsSource === "lrclib" ? qsTr("From LRCLIB, not saved in the file yet") : "")
                  + (view.web.lyricsSynced ? "  ·  " + qsTr("timed") : "")
            font.pixelSize: 11
            color: theme.c.muted
        }

        ListView {
            id: lines
            objectName: "lyricsLines"
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: view.shown && view.web.lyricsState === "found" && !view.web.lyricsInstrumental
            clip: true
            model: view.shown ? view.web.lyricsLines : []
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
            delegate: Text {
                id: line
                required property var modelData
                required property int index
                readonly property bool lit: index === view.currentLine
                width: ListView.view.width
                topPadding: 3
                bottomPadding: 3
                wrapMode: Text.Wrap
                text: modelData.text !== "" ? modelData.text : " "
                font.pixelSize: lit ? 15 : 13
                font.bold: lit
                color: lit ? theme.accent
                     : view.currentLine >= 0 && index < view.currentLine ? theme.c.muted : theme.c.foreground
                // A click on a timed line jumps there while it plays.
                MouseArea {
                    anchors.fill: parent
                    enabled: view.follow && line.modelData.time >= 0
                    cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                    onClicked: backend.seek(line.modelData.time)
                }
            }
        }

        // Nothing to show yet, or nothing at all.
        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: !lines.visible
            spacing: 10
            Item { Layout.preferredHeight: 20 }
            BusyIndicator {
                Layout.alignment: Qt.AlignHCenter
                visible: view.web.lyricsState === "loading"
                running: visible
            }
            Text {
                objectName: "lyricsStatus"
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                text: view.trackId <= 0 ? qsTr("Choose a track to see its lyrics.")
                      : !view.shown || view.web.lyricsState === "loading" ? qsTr("Looking for lyrics…")
                      : view.web.lyricsInstrumental ? qsTr("Instrumental: no words.")
                      : view.web.lyricsState === "error" ? qsTr("Couldn't get lyrics: %1").arg(view.web.lyricsError)
                      : qsTr("No lyrics for this track in its file, beside it, or on LRCLIB yet.")
                font.pixelSize: 13
                color: view.web.lyricsState === "error" ? theme.c.red : theme.c.muted
            }
            Button {
                objectName: "fetchLyrics"
                Layout.alignment: Qt.AlignHCenter
                visible: view.shown && (view.web.lyricsState === "none" || view.web.lyricsState === "error")
                text: view.web.lyricsState === "error" ? qsTr("Try again") : qsTr("Fetch from LRCLIB")
                font.pixelSize: 13
                onClicked: view.load(true)
            }
            Item { Layout.fillHeight: true }
        }

        // What can be done with lyrics that are shown.
        Flow {
            Layout.fillWidth: true
            visible: view.shown && view.web.lyricsState === "found"
            spacing: 4
            Button {
                objectName: "saveLrc"
                text: qsTr("Save as .lrc")
                flat: true
                font.pixelSize: 12
                implicitHeight: 32; topInset: 0; bottomInset: 0
                visible: view.web.lyricsSynced && view.web.lyricsSource !== "sidecar"
                onClicked: view.web.saveLyrics("lrc")
            }
            Button {
                objectName: "saveLyricsTag"
                text: qsTr("Save into the file")
                flat: true
                font.pixelSize: 12
                implicitHeight: 32; topInset: 0; bottomInset: 0
                visible: view.web.lyricsSource === "lrclib"
                onClicked: view.web.saveLyrics("tag")
            }
            Button {
                text: qsTr("Edit…")
                flat: true
                font.pixelSize: 12
                implicitHeight: 32; topInset: 0; bottomInset: 0
                onClicked: view.editLyrics(view.trackId)
            }
            Button {
                text: qsTr("Search LRCLIB again")
                flat: true
                font.pixelSize: 12
                implicitHeight: 32; topInset: 0; bottomInset: 0
                visible: view.web.lyricsSource === "lrclib"
                onClicked: view.load(true)
            }
        }
    }
}
