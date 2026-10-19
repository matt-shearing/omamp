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

    style: appearance.visualiser
    levels: backend.levels
    peaks: backend.peaks
    waveform: backend.waveform
    low: skin.visLow
    mid: skin.visMid
    high: skin.visHigh
    peak: skin.visPeak
    spacing: 2

    MouseArea {
        anchors.fill: parent
        enabled: vis.cycles
        cursorShape: Qt.PointingHandCursor
        onClicked: appearance.nextVisualiser()
    }
}
