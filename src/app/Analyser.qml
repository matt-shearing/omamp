import QtQuick
import Omamp

// The analyser on the deck, the strip and the mini player, in the style
// chosen in Settings > Appearance (appearance.visualiser). A click moves on
// to the next style, and the mouse wheel goes forwards and back through
// them. Colours come from the deck style it sits in.
Visualiser {
    id: vis
    objectName: "analyser"
    property var skin: appearance.plain
    property bool cycles: true
    // What a click does; the large bar's visualiser cycles its own style.
    property var cycle: function() { appearance.nextVisualiser() }
    // What the wheel does: `delta` styles on, or back when negative.
    property var step: function(delta) { appearance.stepVisualiser(delta) }

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

    // Without `cycles` (behind the whole bar, where clicks belong to what
    // is drawn over it) only the wheel is taken.
    MouseArea {
        anchors.fill: parent
        acceptedButtons: vis.cycles ? Qt.LeftButton : Qt.NoButton
        cursorShape: vis.cycles ? Qt.PointingHandCursor : Qt.ArrowCursor
        onClicked: vis.cycle()
        // One style per notch (120 eighths of a degree); a touchpad's small
        // steps add up to one. Down or right goes on, up or left goes back.
        property int wheelSum: 0
        onWheel: (wheel) => {
            const d = wheel.angleDelta.y !== 0 ? wheel.angleDelta.y : -wheel.angleDelta.x
            wheelSum += d
            const notches = wheelSum > 0 ? Math.floor(wheelSum / 120) : Math.ceil(wheelSum / 120)
            if (notches !== 0) {
                wheelSum -= notches * 120
                vis.step(-notches)
            }
            wheel.accepted = true
        }
    }
}
