import QtQuick
import QtQuick.Controls

// The lyrics in the big view: timed lines scroll with the song, the one
// being sung large and lit in the middle; untimed words just scroll. They
// come from the same place as the info panel's Lyrics tab (a .lrc file
// beside the track, its tags, or LRCLIB once fetched there).
Item {
    id: lyrics
    property var skin: appearance.deck
    property var trackId: 0
    property bool active: false
    property bool centred: false
    readonly property var web: library.web
    readonly property bool shown: web.lyricsTrack === trackId && trackId > 0
    readonly property bool hasLines: shown && web.lyricsState === "found" && !web.lyricsInstrumental
                                     && web.lyricsLines.length > 0
    readonly property int currentLine: hasLines && web.lyricsSynced ? web.lyricsLineAt(backend.position) : -1

    // Asked for a moment later, never while a binding is being worked out
    // (the answer can arrive at once and change what the binding reads).
    function load() { Qt.callLater(loadNow) }
    function loadNow() { if (active && trackId > 0 && library.ready && !shown) web.loadLyrics(trackId, false) }
    onActiveChanged: load()
    onTrackIdChanged: load()
    Connections {
        target: library
        function onReadyChanged() { lyrics.load() }
    }
    Component.onCompleted: load()
    onCurrentLineChanged: if (currentLine >= 0) lines.positionViewAtIndex(currentLine, ListView.Center)

    ListView {
        id: lines
        objectName: "bigLyricsLines"
        anchors.fill: parent
        visible: lyrics.hasLines
        clip: true
        interactive: !lyrics.web.lyricsSynced
        model: lyrics.hasLines ? lyrics.web.lyricsLines : []
        boundsBehavior: Flickable.StopAtBounds
        preferredHighlightBegin: height / 2 - 20
        preferredHighlightEnd: height / 2 + 20
        highlightRangeMode: lyrics.web.lyricsSynced ? ListView.ApplyRange : ListView.NoHighlightRange
        currentIndex: Math.max(0, lyrics.currentLine)
        highlightMoveDuration: 400
        delegate: Text {
            required property var modelData
            required property int index
            readonly property bool lit: index === lyrics.currentLine
            width: ListView.view.width
            topPadding: 5
            bottomPadding: 5
            wrapMode: Text.Wrap
            horizontalAlignment: lyrics.centred ? Text.AlignHCenter : Text.AlignLeft
            text: modelData.text !== "" ? modelData.text : " "
            font.pixelSize: lit ? 24 : 18
            font.bold: lit
            color: lit ? lyrics.skin.accent
                 : lyrics.currentLine >= 0 && index < lyrics.currentLine ? Qt.alpha(lyrics.skin.bigSubtext, 0.55)
                 : lyrics.skin.bigText
            opacity: lyrics.currentLine < 0 || Math.abs(index - lyrics.currentLine) < 6 ? 1 : 0.5
        }
    }

    Text {
        anchors.centerIn: parent
        width: parent.width
        visible: !lyrics.hasLines
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.Wrap
        text: !lyrics.shown || lyrics.web.lyricsState === "loading" ? qsTr("Looking for lyrics…")
              : lyrics.web.lyricsInstrumental ? qsTr("Instrumental: no words.")
              : qsTr("No lyrics for this track. The info panel's Lyrics tab can fetch them from LRCLIB.")
        font.pixelSize: 15
        color: lyrics.skin.bigSubtext
    }
}
