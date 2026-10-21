import QtQuick
import ".." as Plugin
import "../Model.js" as Model

// Renders the bar pill with the mini visualiser behind the track, once per
// style, with made-up levels and a made-up track, for the README pictures.
// The shell's own modules are not here, so the pill is drawn the way
// Widget.qml lays it out (cover, glyph, "Artist – Title") at the bar's size.
//
//   QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software \
//     /usr/lib/qt6/bin/qml omarchy-plugin/tests/render-visualiser.qml -- dark out.png
//
// The last argument is the picture to write; "light" anywhere picks the
// light theme.
Window {
  id: win
  readonly property var args: Qt.application.arguments
  readonly property bool dark: args.indexOf("light") < 0
  readonly property string out: args[args.length - 1]

  // Tokyo Night and its light twin: the bar's background, its words, the
  // theme green and the accent.
  readonly property color bg: dark ? "#1a1b26" : "#e1e2e7"
  readonly property color fg: dark ? "#c0caf5" : "#3760bf"
  readonly property color green: dark ? "#9ece6a" : "#587539"
  readonly property color accent: dark ? "#7aa2f7" : "#2e7de9"
  readonly property string font: "JetBrainsMono Nerd Font"

  readonly property var styles: [
    ["bars", "Bars, Bars with peaks, Fire"], ["skyline", "Skyline, Mirrored spectrum"],
    ["led", "LED matrix"], ["synthwave", "Synthwave"], ["line", "Spectrum line"],
    ["ribbon", "Waveform ribbon, Oscilloscope"], ["pulse", "Pulse"], ["stars", "Star field"]
  ]
  // A made-up moment of music: strong bass, a lift in the mids, quiet top.
  readonly property var levels: [0.92, 0.85, 0.7, 0.62, 0.55, 0.6, 0.66, 0.58, 0.47, 0.42, 0.36, 0.3, 0.26, 0.2, 0.14, 0.09]
  readonly property var levels2: [0.88, 0.9, 0.74, 0.6, 0.58, 0.64, 0.7, 0.55, 0.5, 0.4, 0.38, 0.28, 0.22, 0.18, 0.12, 0.08]

  width: 560
  height: column.implicitHeight + 24
  visible: true
  color: bg
  title: "render"

  Rectangle {
    anchors.fill: parent
    color: win.bg
  }

  Column {
    id: column
    x: 12
    y: 12
    spacing: 6

    Repeater {
      model: win.styles
      Row {
        required property var modelData
        spacing: 14

        // The bar: 26 px, as Omarchy draws it.
        Rectangle {
          id: bar
          width: 330
          height: 26
          color: win.bg
          border.width: 1
          border.color: Qt.rgba(win.fg.r, win.fg.g, win.fg.b, 0.08)

          Plugin.MiniVisualiser {
            id: viz
            x: content.x + label.x - 6
            y: 2
            width: label.width + 12
            height: bar.height - 4
            style: modelData[0]
            active: true
            strength: Model.vizOpacity("Subtle", modelData[0])
            low: win.green
            high: win.accent
            Component.onCompleted: { levels = win.levels2; levels = win.levels }
          }

          Row {
            id: content
            anchors.centerIn: parent
            spacing: 6

            // A made-up cover: two theme colours.
            Rectangle {
              width: 18
              height: 18
              radius: 3
              anchors.verticalCenter: parent.verticalCenter
              gradient: Gradient {
                GradientStop { position: 0; color: win.accent }
                GradientStop { position: 1; color: win.green }
              }
            }
            Text {
              anchors.verticalCenter: parent.verticalCenter
              text: Model.GLYPH_PLAYING
              color: win.green
              font.family: win.font
              font.pixelSize: 13
            }
            Text {
              id: label
              anchors.verticalCenter: parent.verticalCenter
              text: Model.trackLine("Neon Coast", "Midnight Highway")
              color: win.fg
              font.family: win.font
              font.pixelSize: 13
            }
          }
        }

        Text {
          anchors.verticalCenter: parent.verticalCenter
          text: modelData[1]
          color: Qt.rgba(win.fg.r, win.fg.g, win.fg.b, 0.6)
          font.family: win.font
          font.pixelSize: 11
        }
      }
    }
  }

  Timer {
    interval: 900
    running: true
    onTriggered: win.contentItem.grabToImage(function(result) {
      result.saveToFile(win.out)
      Qt.quit()
    })
  }
}
