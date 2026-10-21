import QtQuick
import Omamp

// The analyser on the deck, the strip and the mini player, in the style
// chosen in Settings > Appearance (appearance.visualiser). A click moves on
// to the next style. Colours come from the deck style it sits in.
Visualiser {
    id: vis
    objectName: "analyser"
    property var skin: appearance.plain
    property bool cycles: true
    // What a click does; the large bar's visualiser cycles its own style.
    property var cycle: function() { appearance.nextVisualiser() }

    style: appearance.visualiser
    levels: backend.levels
    peaks: backend.peaks
    waveform: backend.waveform
    // Only the ribbon reads both channels; the others skip the copy.
    stereoWaveform: style === "ribbon" ? backend.stereoWaveform : []
    // No sound reaches Omamp: paused, stopped, or Spotify playing elsewhere.
    idle: backend.state !== "playing" || backend.playsElsewhere
    reduceMotion: appearance.reduceMotion
    low: skin.visLow
    mid: skin.visMid
    high: skin.visHigh
    peak: skin.visPeak
    spacing: 2

    MouseArea {
        anchors.fill: parent
        enabled: vis.cycles
        cursorShape: Qt.PointingHandCursor
        onClicked: vis.cycle()
    }
}
