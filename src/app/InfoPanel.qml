import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// The info panel on the right of the library: the cover and everything the
// library knows about one track. It shows the track under the cursor in the
// list (or the first track of the album chosen in the grid), and the track
// that is playing when nothing is chosen.
Rectangle {
    id: panel
    objectName: "infoPanel"
    color: theme.c.dark_background

    // Set by the view: the track to show, or 0 for the one playing.
    property var chosenTrack: 0
    readonly property var trackId: chosenTrack > 0 ? chosenTrack : library.playingTrackId
    readonly property bool showingPlaying: !(chosenTrack > 0) && library.playingTrackId > 0
    // Reads again whenever the library changes (library.revision).
    readonly property var info: trackId > 0 && library.revision >= 0 ? library.trackInfo(trackId) : ({})
    readonly property bool hasTrack: info.id !== undefined

    function clock(ms) {
        if (!(ms > 0)) return ""
        const total = Math.floor(ms / 1000)
        const h = Math.floor(total / 3600), m = Math.floor(total % 3600 / 60), s = total % 60
        return (h > 0 ? h + ":" + (m < 10 ? "0" : "") : "") + m + ":" + (s < 10 ? "0" : "") + s
    }
    function day(seconds) {
        if (!(seconds > 0)) return ""
        return new Date(seconds * 1000).toLocaleDateString(Qt.locale(), Locale.ShortFormat)
    }
    function gain(db, peak) {
        if (db === undefined || db === null) return ""
        let text = (db > 0 ? "+" : db < 0 ? "−" : "") + Math.abs(db).toLocaleString(Qt.locale(), 'f', 2) + " dB"
        if (peak !== undefined && peak !== null) text += qsTr(", peak %1").arg(Number(peak).toLocaleString(Qt.locale(), 'f', 3))
        return text
    }
    function formatLine(i) {
        const parts = []
        if (i.format) parts.push(i.format)
        if (i.sampleRate > 0) parts.push((i.sampleRate / 1000).toLocaleString(Qt.locale(), 'f', i.sampleRate % 1000 ? 1 : 0) + " kHz")
        if (i.channels === 1) parts.push(qsTr("mono"))
        else if (i.channels === 2) parts.push(qsTr("stereo"))
        else if (i.channels > 2) parts.push(qsTr("%n channels", "", i.channels))
        if (i.bitrate > 0) parts.push(i.bitrate + " kbps")
        return parts.join("  ·  ")
    }
    function size(bytes) {
        if (!(bytes > 0)) return ""
        return bytes >= 1048576 ? (bytes / 1048576).toLocaleString(Qt.locale(), 'f', 1) + " MB"
                                : Math.round(bytes / 1024) + " KB"
    }

    Flickable {
        id: flick
        anchors.fill: parent
        anchors.margins: 12
        contentHeight: column.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

        ColumnLayout {
            id: column
            width: flick.width
            spacing: 8

            Text {
                Layout.fillWidth: true
                text: panel.showingPlaying ? qsTr("Now playing") : panel.hasTrack ? qsTr("Selected track") : ""
                visible: text !== ""
                font.pixelSize: 11
                font.bold: true
                font.capitalization: Font.AllUppercase
                color: theme.c.muted
            }

            // The cover, or a plain square when there is none.
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: width
                visible: panel.hasTrack
                color: theme.c.darker_background
                border.width: 1
                border.color: theme.c.lighter_background
                radius: Math.min(4, theme.radius)
                Image {
                    id: cover
                    objectName: "infoCover"
                    anchors.fill: parent
                    anchors.margins: 1
                    asynchronous: true
                    cache: true
                    fillMode: Image.PreserveAspectFit
                    sourceSize: Qt.size(256, 256)
                    source: panel.hasTrack && panel.info.artState !== 2 ? "image://omamp-art/track/" + panel.trackId + "/256?v=" + library.revision : ""
                }
                Text {
                    anchors.centerIn: parent
                    visible: cover.status !== Image.Ready
                    text: String.fromCodePoint(0xF0025) // album
                    font.family: "JetBrainsMono Nerd Font"
                    font.pixelSize: 48
                    color: theme.c.lighter_background
                }
            }

            Text {
                objectName: "infoTitle"
                Layout.fillWidth: true
                visible: panel.hasTrack
                text: panel.info.title || ""
                wrapMode: Text.Wrap
                maximumLineCount: 3
                elide: Text.ElideRight
                font.pixelSize: 16
                font.bold: true
                color: theme.c.bright_foreground
            }
            Text {
                Layout.fillWidth: true
                visible: panel.hasTrack && text !== ""
                text: panel.info.artist || ""
                wrapMode: Text.Wrap
                font.pixelSize: 13
                color: theme.c.foreground
            }
            Text {
                Layout.fillWidth: true
                visible: panel.hasTrack && text !== ""
                text: (panel.info.album || "") + (panel.info.year > 0 ? " (" + panel.info.year + ")" : "")
                wrapMode: Text.Wrap
                font.pixelSize: 13
                color: theme.c.light_foreground
            }

            // The details, as label and value pairs; empty ones are left out.
            GridLayout {
                Layout.fillWidth: true
                Layout.topMargin: 4
                visible: panel.hasTrack
                columns: 2
                columnSpacing: 10
                rowSpacing: 5

                Repeater {
                    objectName: "infoRows"
                    model: panel.hasTrack ? [
                        [qsTr("Album artist"), panel.info.albumArtist !== panel.info.artist ? panel.info.albumArtist : ""],
                        [qsTr("Genre"), panel.info.genre],
                        [qsTr("Composer"), panel.info.composer],
                        [qsTr("Track"), panel.info.trackNo > 0 ? (panel.info.discNo > 0 ? qsTr("%1, disc %2").arg(panel.info.trackNo).arg(panel.info.discNo) : String(panel.info.trackNo)) : ""],
                        [qsTr("Length"), panel.clock(panel.info.durationMs)],
                        [qsTr("Format"), panel.formatLine(panel.info)],
                        [qsTr("Size"), panel.size(panel.info.size)],
                        [qsTr("ReplayGain"), panel.info.trackGain !== undefined && panel.info.trackGain !== null
                                             ? qsTr("track %1").arg(panel.gain(panel.info.trackGain, panel.info.trackPeak))
                                               + (panel.info.albumGain !== undefined && panel.info.albumGain !== null
                                                  ? "\n" + qsTr("album %1").arg(panel.gain(panel.info.albumGain, panel.info.albumPeak)) : "")
                                             : qsTr("Not analysed")],
                        [qsTr("Rating"), "★"],
                        [qsTr("Plays"), panel.info.plays > 0 ? Number(panel.info.plays).toLocaleString(Qt.locale(), 'f', 0) : qsTr("Never played")],
                        [qsTr("Last played"), panel.day(panel.info.lastPlayed)],
                        [qsTr("Added"), panel.day(panel.info.added)],
                        [qsTr("Lyrics"), panel.info.hasLyrics ? qsTr("In the file (the lyrics view comes later)") : qsTr("None")],
                        [qsTr("BPM"), panel.info.bpm > 0 ? String(panel.info.bpm) : ""]
                    ].filter(r => r[1] !== undefined && r[1] !== "") : []

                    delegate: Item {
                        id: pair
                        required property var modelData
                        required property int index
                        Layout.columnSpan: 2
                        Layout.fillWidth: true
                        implicitHeight: Math.max(key.implicitHeight, val.implicitHeight, stars.implicitHeight)
                        Text {
                            id: key
                            width: 92
                            text: pair.modelData[0]
                            font.pixelSize: 12
                            color: theme.c.muted
                            elide: Text.ElideRight
                        }
                        Text {
                            id: val
                            anchors { left: key.right; leftMargin: 8; right: parent.right }
                            visible: pair.modelData[0] !== qsTr("Rating")
                            text: pair.modelData[1]
                            wrapMode: Text.Wrap
                            font.pixelSize: 12
                            color: theme.c.foreground
                        }
                        Row {
                            id: stars
                            anchors { left: key.right; leftMargin: 8 }
                            visible: pair.modelData[0] === qsTr("Rating")
                            Repeater {
                                model: 5
                                Text {
                                    required property int index
                                    readonly property real fill: Math.max(0, Math.min(1, (panel.info.rating - index * 20) / 20))
                                    text: "★"
                                    font.pixelSize: 13
                                    color: fill >= 1 ? theme.accent : fill >= 0.5 ? Qt.alpha(theme.accent, 0.5) : theme.c.lighter_background
                                }
                            }
                            Text {
                                leftPadding: 6
                                visible: !(panel.info.rating >= 0)
                                text: qsTr("Unrated")
                                font.pixelSize: 12
                                color: theme.c.muted
                            }
                        }
                    }
                }
            }

            // Where the file is; selectable, so it can be copied.
            Text {
                Layout.fillWidth: true
                Layout.topMargin: 6
                visible: panel.hasTrack
                text: panel.info.online === false ? qsTr("File (offline)") : qsTr("File")
                font.pixelSize: 12
                color: theme.c.muted
            }
            TextEdit {
                objectName: "infoPath"
                Layout.fillWidth: true
                visible: panel.hasTrack
                readOnly: true
                selectByMouse: true
                text: panel.info.path || ""
                wrapMode: TextEdit.WrapAnywhere
                font.pixelSize: 11
                color: panel.info.online === false ? theme.c.muted : theme.c.foreground
                selectionColor: theme.c.selection
            }

            Text {
                Layout.fillWidth: true
                Layout.topMargin: 40
                visible: !panel.hasTrack
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                text: qsTr("Choose a track to see its details here.")
                font.pixelSize: 13
                color: theme.c.muted
            }
        }
    }
}
