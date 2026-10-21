import QtQuick
import QtQuick.Effects
import Quickshell
import qs.Ui
import qs.Commons
import qs.Commons as Commons
import "Model.js" as Model

// Thrum in the bar: the cover, the state glyph and "Artist – Title" while
// Thrum is running, and a panel led by a large cover, with the track, seek,
// transport, shuffle, repeat and volume.
//
//   left click    open the panel (opens Thrum when it is not running)
//   right click   play or pause
//   middle click  next track
//   scroll        previous / next track (setting: volume, or nothing)
//
// Everything is read from, and every action goes through, Service.qml: this
// file is built once per monitor and must not own side effects.
Panel {
  id: root
  moduleName: "contra.thrum"
  ipcTarget: "contra.thrum"
  // Service.qml owns the "contra.thrum" IPC target. Summon the panel with
  // `omarchy-shell shell toggle contra.thrum`.
  manageIpc: false

  readonly property var service: bar?.shell?.serviceFor("contra.thrum")

  // ---------------------------------------------------------------- settings
  readonly property int maxLabelWidth: Math.max(Style.space(60), Style.space(Number(setting("maxLabelWidth", 220))))
  readonly property bool showClock: setting("showClock", false) === true
  readonly property bool showWhenClosed: setting("showWhenClosed", false) === true
  readonly property string scrollAction: String(setting("scrollAction", "Previous/next track"))
  readonly property bool showThumbnail: setting("showThumbnail", true) !== false
  readonly property string coverMode: Model.coverMode(setting("coverSize", "Large"))
  readonly property bool showLcd: setting("showLcd", false) === true
  // A faint copy of Thrum's visualiser behind the track (MiniVisualiser.qml).
  readonly property bool showVisualiser: setting("showVisualiser", true) !== false
  readonly property string vizChoice: Model.vizChoice(setting("visualiserStyle", "Same as Thrum"))
  readonly property string vizStrength: String(setting("visualiserStrength", "Subtle"))

  // ------------------------------------------------------------------- state
  readonly property bool running: service ? service.running : false
  readonly property bool playing: service ? service.playing : false
  readonly property bool stopped: service ? service.stopped : false
  readonly property string line: service ? service.line : ""
  readonly property real length: service ? service.length : 0
  readonly property real position: service ? service.position : 0
  readonly property color green: service ? service.green : Commons.Color.accent
  readonly property string cover: service && service.cover !== undefined ? service.cover : ""
  readonly property bool hasTrack: service ? service.hasTrack === true : false

  readonly property string fontFamily: bar ? bar.fontFamily : Style.font.family
  readonly property color foreground: bar ? bar.foreground : Commons.Color.foreground
  // Secondary text fades toward the background, so it reads quieter in light
  // themes as well as dark ones.
  readonly property color dim: Qt.rgba(foreground.r, foreground.g, foreground.b, 0.72)
  readonly property color dimmer: Qt.rgba(foreground.r, foreground.g, foreground.b, 0.52)
  readonly property bool vertical: bar ? bar.vertical : false
  readonly property int barSize: bar ? bar.barSize : Style.bar.sizeHorizontal

  // Shared by the panel's clock: elapsed, or time left, like Thrum's own.
  property bool showRemaining: false

  readonly property bool shown: running || showWhenClosed

  // ------------------------------------------------------------ visualiser
  // Asks the service for Thrum's levels while this copy shows a track; the
  // service opens the socket only while some copy asks.
  readonly property bool vizWanted: showVisualiser && running && !vertical && visible
  readonly property string vizStyle: Model.miniStyle(service ? service.vizStyle : "", vizChoice)
  readonly property bool vizCalm: Style.reduceMotion || (service ? service.vizReduceMotion === true : false)
  function vizRegister() { if (service) service.vizSet(root, vizWanted) }
  onVizWantedChanged: vizRegister()
  onServiceChanged: vizRegister()
  Component.onCompleted: vizRegister()
  Component.onDestruction: if (service) service.vizSet(root, false)

  // ---------------------------------------------------------------- bar entry
  visible: shown
  readonly property real labelSlot: labelClip.visible ? labelClip.width + content.spacing : 0
  readonly property real clockSlot: barClock.visible ? barClock.implicitWidth + content.spacing : 0
  readonly property real thumbSlot: thumb.visible ? thumb.width + content.spacing : 0
  implicitWidth: !shown ? 0 : (vertical ? barSize : Math.round(thumbSlot + glyph.implicitWidth + labelSlot + clockSlot + Style.space(12)))
  implicitHeight: !shown ? 0 : (vertical ? Math.round(glyph.implicitHeight + Style.space(10)) : barSize)

  // Behind the words, a little wider than them, the bar's full height.
  MiniVisualiser {
    id: barViz
    readonly property real pad: Style.space(6)
    visible: labelClip.visible && opacity > 0.001
    x: content.x + labelClip.x - pad
    y: Style.space(2)
    width: labelClip.width + pad * 2
    height: root.height - Style.space(4)
    levels: root.vizWanted && root.service ? root.service.vizLevels : []
    active: root.vizWanted && root.service ? root.service.vizActive === true : false
    style: root.vizStyle
    reduceMotion: root.vizCalm
    strength: Model.vizOpacity(root.vizStrength, root.vizStyle)
    low: root.green
    high: Commons.Color.accent
    barWidth: 3
  }

  Row {
    id: content
    anchors.centerIn: parent
    spacing: Style.space(6)

    // The cover as a small square, as tall as the bar allows. It stays on the
    // old picture until the new one has loaded, then fades across.
    CoverArt {
      id: thumb
      readonly property bool wanted: root.showThumbnail && !root.vertical && root.running
      // A cover that will not load gives its place back to the glyph.
      visible: wanted && root.cover !== "" && failedSource !== root.cover
      anchors.verticalCenter: parent.verticalCenter
      width: Model.thumbSize(root.barSize, Style.space(8))
      height: width
      source: wanted ? root.cover : ""
      radius: Math.max(Style.space(2), Math.min(Style.space(4), Style.cornerRadius))
      backdrop: "transparent"
      placeholder: false
      fadeDuration: Style.duration(200)
    }

    Text {
      id: glyph
      anchors.verticalCenter: parent.verticalCenter
      textFormat: Text.PlainText
      text: root.running ? Model.stateGlyph(root.playing, root.stopped) : Model.GLYPH_MUSIC
      color: !root.running ? Qt.darker(root.barForeground, 1.9)
        : root.playing ? root.green
        : Qt.darker(root.barForeground, 1.5)
      font.family: root.fontFamily
      font.pixelSize: Style.font.body

      Behavior on color {
        enabled: !root.bar || root.bar.foregroundAnimationEnabled
        ColorAnimation { duration: Style.duration(160) }
      }
    }

    Item {
      id: labelClip
      visible: !root.vertical && root.running && root.line !== ""
      width: visible ? Math.min(root.maxLabelWidth, labelText.implicitWidth) : 0
      height: glyph.height
      clip: true
      anchors.verticalCenter: parent.verticalCenter

      // The start of the line stays readable. Only a line too long for the
      // slot pans, resting at both ends, and never while the panel is open.
      Text {
        id: labelText
        textFormat: Text.PlainText
        text: root.line
        color: root.barForeground
        font.family: root.fontFamily
        font.pixelSize: Style.font.body
        anchors.verticalCenter: parent.verticalCenter
        x: -panOffset

        property real panOffset: 0
        readonly property real overflow: Math.max(0, implicitWidth - labelClip.width)

        onTextChanged: {
          panOffset = 0
          if (marquee.running) marquee.restart()
        }
      }

      SequentialAnimation {
        id: marquee
        running: labelText.overflow > 0 && labelClip.visible && !root.opened && !Style.reduceMotion
        loops: Animation.Infinite
        onRunningChanged: if (!running) labelText.panOffset = 0

        PauseAnimation { duration: 2600 }
        NumberAnimation {
          target: labelText; property: "panOffset"
          from: 0; to: labelText.overflow
          duration: Math.max(1600, labelText.overflow * 45)
          easing.type: Easing.InOutQuad
        }
        PauseAnimation { duration: 2200 }
        NumberAnimation {
          target: labelText; property: "panOffset"
          from: labelText.overflow; to: 0
          duration: Math.max(900, labelText.overflow * 20)
          easing.type: Easing.InOutQuad
        }
      }
    }

    // Optional elapsed time in the theme green, Thrum's clock in miniature.
    Text {
      id: barClock
      visible: root.showClock && !root.vertical && root.running && root.line !== ""
      anchors.verticalCenter: parent.verticalCenter
      textFormat: Text.PlainText
      text: Model.clock(root.position)
      color: root.playing ? root.green : Qt.darker(root.barForeground, 1.5)
      font.family: root.fontFamily
      font.pixelSize: Style.font.body
      font.features: { "tnum": 1 }
    }
  }

  // The bar forwards a left click only to a slot that exposes triggerPress(),
  // and uses the same check for the pointer cursor and the open-panel mark.
  // Other buttons fall through to the MouseArea below, which routes back here.
  function triggerPress(button) {
    if (bar) bar.hideTooltip(root)
    if (!service) return

    if (!running) {
      service.launch()
      return
    }
    if (button === Qt.MiddleButton) service.next()
    else if (button === Qt.RightButton) service.playPause()
    else toggle()
  }

  MouseArea {
    id: pointer
    anchors.fill: parent
    hoverEnabled: true
    acceptedButtons: Qt.RightButton | Qt.MiddleButton

    // A mouse sends 120 per notch; a touchpad sends a stream of small deltas
    // for one swipe. Track skips are one per notch, or one per swipe, with a
    // short cooldown so a fast spin does not tear through the playlist.
    property real accumulated: 0
    property bool swipeUsed: false
    readonly property real notch: 120

    onClicked: function(mouse) { root.triggerPress(mouse.button) }

    onWheel: function(wheel) {
      if (!root.service || !root.running || root.scrollAction === "Nothing") return
      // Kinetic scrolling after the fingers lift is not a request (Qt::ScrollMomentum).
      if (wheel.phase === 4) return

      var continuous = wheel.phase !== 0 || wheel.pixelDelta.y !== 0 || Math.abs(wheel.angleDelta.y) < notch
      accumulated += wheel.angleDelta.y
      swipeEnd.restart()

      if (root.scrollAction === "Volume") {
        var step = continuous ? notch * 2 : notch
        while (Math.abs(accumulated) >= step) {
          var louder = accumulated > 0
          accumulated += louder ? -step : step
          root.service.nudgeVolume(louder ? 0.05 : -0.05)
        }
        return
      }

      if (continuous) {
        if (swipeUsed || Math.abs(accumulated) < 50) return
        swipeUsed = true
      } else if (Math.abs(accumulated) < notch) {
        return
      }

      var up = accumulated > 0
      accumulated = 0
      if (skipCooldown.running) return
      skipCooldown.restart()
      if (up) root.service.previous()
      else root.service.next()
    }

    onEntered: if (root.bar && root.service)
      root.bar.showTooltip(root, Model.tooltip(root.running, root.line, root.service.state))
    onExited: if (root.bar) root.bar.hideTooltip(root)
  }

  Timer {
    id: swipeEnd
    interval: 220
    onTriggered: { pointer.accumulated = 0; pointer.swipeUsed = false }
  }

  Timer { id: skipCooldown; interval: 350 }

  // A closed Thrum closes its panel too.
  onRunningChanged: if (!running && opened) close()

  // ------------------------------------------------------------------- panel
  PopupCard {
    id: popup
    anchorItem: root
    bar: root.bar
    owner: root
    open: root.opened && root.running
    contentWidth: popup.fittedContentWidth(Style.space(340))
    contentHeight: popup.fittedContentHeight(column.implicitHeight)

    Column {
      id: column
      anchors.left: parent.left
      anchors.right: parent.right
      anchors.top: parent.top
      spacing: Style.space(12)

      // The cover is the centrepiece: a square the width of the panel, from
      // the picture Thrum sends over MPRIS (mpris:artUrl). Local files,
      // Spotify, radio station tiles and Navidrome covers all arrive that way.
      CoverArt {
        id: bigCover
        visible: root.coverMode === "large" && root.hasTrack
        width: parent.width
        height: Model.panelCoverSize(root.coverMode, parent.width, 0)
        source: root.cover
        radius: Math.min(Style.space(8), Style.cornerRadius)
        backdrop: Qt.darker(Commons.Color.popups.background, 1.35)
        glyphColor: root.dimmer
        glyphFamily: root.fontFamily
        glyphSize: Math.round(width * 0.28)
        border.color: Qt.rgba(root.foreground.r, root.foreground.g, root.foreground.b, 0.12)
      }

      // The track. Under a large cover, or beside a medium one.
      Row {
        width: parent.width
        spacing: Style.space(12)
        visible: root.hasTrack

        CoverArt {
          id: smallCover
          visible: root.coverMode === "medium"
          width: visible ? Model.panelCoverSize("medium", parent.width, Style.space(96)) : 0
          height: width
          anchors.verticalCenter: parent.verticalCenter
          source: visible ? root.cover : ""
          radius: Math.min(Style.space(4), Style.cornerRadius)
          backdrop: Qt.darker(Commons.Color.popups.background, 1.35)
          glyphColor: root.dimmer
          glyphFamily: root.fontFamily
          glyphSize: Style.font.displayLarge
          border.color: Qt.rgba(root.foreground.r, root.foreground.g, root.foreground.b, 0.12)
        }

        Column {
          width: parent.width - (smallCover.visible ? smallCover.width + parent.spacing : 0)
          anchors.verticalCenter: parent.verticalCenter
          spacing: Style.space(3)

          Text {
            id: panelTitle
            width: parent.width
            textFormat: Text.PlainText
            text: root.service ? (root.service.title || "Unknown title") : ""

            // A larger copy behind the title, artist and album. A child of
            // the title, drawn under it, so the Column does not place it.
            MiniVisualiser {
              z: -1
              x: -Style.space(4)
              y: -Style.space(2)
              width: parent.width + Style.space(8)
              height: panelTitle.parent.height + Style.space(4)
              levels: popup.visible && root.vizWanted && root.service ? root.service.vizLevels : []
              active: popup.visible && root.vizWanted && root.service ? root.service.vizActive === true : false
              style: root.vizStyle
              reduceMotion: root.vizCalm
              strength: Model.vizOpacity(root.vizStrength, root.vizStyle)
              low: root.green
              high: Commons.Color.accent
              barWidth: 5
              edgeFade: 0.12
            }
            color: root.foreground
            font.family: root.fontFamily
            font.pixelSize: root.coverMode === "large" ? Style.font.title : Style.font.subtitle
            font.bold: true
            elide: Text.ElideRight
          }

          Text {
            width: parent.width
            textFormat: Text.PlainText
            text: root.service ? root.service.artist : ""
            color: root.dim
            font.family: root.fontFamily
            font.pixelSize: Style.font.body
            elide: Text.ElideRight
            visible: text !== ""
          }

          Text {
            width: parent.width
            textFormat: Text.PlainText
            text: root.service ? root.service.album : ""
            color: root.dimmer
            font.family: root.fontFamily
            font.pixelSize: Style.font.caption
            elide: Text.ElideRight
            visible: text !== ""
          }
        }
      }

      // Thrum's LCD: state, the green clock and the title strip. Off by
      // default so it does not compete with the cover (setting: showLcd).
      Rectangle {
        id: lcd
        visible: root.showLcd
        width: parent.width
        height: lcdRow.implicitHeight + Style.space(16)
        radius: Math.min(Style.space(4), Style.cornerRadius)
        color: Qt.darker(Commons.Color.popups.background, 1.35)
        border.width: 1
        border.color: Qt.rgba(root.foreground.r, root.foreground.g, root.foreground.b, 0.12)

        Row {
          id: lcdRow
          anchors.left: parent.left
          anchors.right: parent.right
          anchors.verticalCenter: parent.verticalCenter
          anchors.leftMargin: Style.space(10)
          anchors.rightMargin: Style.space(8)
          spacing: Style.space(10)

          Text {
            id: lcdGlyph
            anchors.verticalCenter: parent.verticalCenter
            textFormat: Text.PlainText
            text: Model.stateGlyph(root.playing, root.stopped)
            color: root.playing ? root.green : root.dimmer
            font.family: root.fontFamily
            font.pixelSize: Style.font.title
          }

          Text {
            id: lcdClock
            anchors.verticalCenter: parent.verticalCenter
            textFormat: Text.PlainText
            text: root.showRemaining && root.length > 0
              ? "-" + Model.clock(root.length - root.position)
              : Model.clock(root.position)
            color: root.green
            opacity: root.playing ? 1 : 0.55
            font.family: root.fontFamily
            font.pixelSize: Style.font.display
            font.bold: true
            font.features: { "tnum": 1 }

            MouseArea {
              anchors.fill: parent
              cursorShape: Qt.PointingHandCursor
              onClicked: root.showRemaining = !root.showRemaining
            }
          }

          // The striped title strip. Faint scanlines, green text, and a
          // marquee only when the line does not fit.
          Rectangle {
            id: strip
            anchors.verticalCenter: parent.verticalCenter
            width: lcdRow.width - lcdGlyph.width - lcdClock.width - lcdRow.spacing * 2
            height: Style.space(24)
            radius: Math.min(Style.space(3), Style.cornerRadius)
            color: "transparent"
            border.width: 1
            border.color: Qt.rgba(root.foreground.r, root.foreground.g, root.foreground.b, 0.10)
            clip: true

            readonly property string text: !root.service || !root.service.hasTrack
              ? "Thrum — drop some music in"
              : root.line + (root.length > 0 ? " (" + Model.clock(root.length) + ")" : "")
            readonly property bool scrolls: stripMetrics.width > width - Style.space(12)

            Repeater {
              model: Math.ceil(strip.height / 2)
              Rectangle {
                required property int index
                x: 1
                y: index * 2
                width: strip.width - 2
                height: 1
                color: Qt.rgba(root.green.r, root.green.g, root.green.b, 0.06)
              }
            }

            TextMetrics { id: stripMetrics; font: stripText.font; text: strip.text }
            TextMetrics { id: stripGap; font: stripText.font; text: "   ***   " }

            Text {
              id: stripText
              anchors.verticalCenter: parent.verticalCenter
              x: Style.space(6)
              textFormat: Text.PlainText
              text: strip.scrolls ? strip.text + "   ***   " + strip.text : strip.text
              color: root.green
              font.family: root.fontFamily
              font.pixelSize: Style.font.bodySmall

              NumberAnimation on x {
                running: strip.scrolls && lcd.visible && popup.visible && !Style.reduceMotion
                loops: Animation.Infinite
                from: Style.space(6)
                to: Style.space(6) - stripMetrics.advanceWidth - stripGap.advanceWidth
                duration: Math.max(4000, (stripMetrics.advanceWidth + stripGap.advanceWidth) * 35)
                onRunningChanged: if (!running) stripText.x = Style.space(6)
              }
            }
          }
        }
      }

      Text {
        width: parent.width
        visible: root.service && !root.hasTrack
        text: "The playlist is empty. Drop files on Thrum, or press O in it to add a folder."
        color: root.dim
        wrapMode: Text.WordWrap
        font.family: root.fontFamily
        font.pixelSize: Style.font.bodySmall
      }

      // Seek.
      Column {
        width: parent.width
        spacing: Style.space(1)
        visible: root.length > 0

        PanelSlider {
          id: seekSlider
          width: parent.width
          bar: root.bar
          minimum: 0
          maximum: Math.max(1, root.length)
          value: root.position
          step: 5
          fillColor: root.green
          knobColor: root.green
          enabled: root.service ? root.service.canSeek : false
          opacity: enabled ? 1.0 : 0.5
          onReleased: function(value) { if (root.service) root.service.seekTo(value) }
        }

        Item {
          width: parent.width
          height: elapsedText.implicitHeight

          // Thrum's green clock in small: time played, or time left after
          // a click, as in Thrum.
          Text {
            id: elapsedText
            anchors.left: parent.left
            readonly property real at: seekSlider.dragging ? seekSlider.liveValue : root.position
            text: root.showRemaining ? "-" + Model.clock(root.length - at) : Model.clock(at)
            color: root.green
            opacity: root.playing ? 1 : 0.7
            font.family: root.fontFamily
            font.pixelSize: Style.font.caption
            font.bold: true
            font.features: { "tnum": 1 }

            MouseArea {
              anchors.fill: parent
              anchors.margins: -Style.space(4)
              cursorShape: Qt.PointingHandCursor
              onClicked: root.showRemaining = !root.showRemaining
            }
          }

          Text {
            anchors.right: parent.right
            text: Model.clock(root.length)
            color: root.dim
            font.features: { "tnum": 1 }
            font.family: root.fontFamily
            font.pixelSize: Style.font.caption
          }
        }
      }

      // Transport on the left, modes on the right. Media controls are not
      // mirrored in right-to-left layouts.
      Item {
        width: parent.width
        height: transport.implicitHeight
        LayoutMirroring.enabled: false
        LayoutMirroring.childrenInherit: true

        Row {
          id: transport
          anchors.left: parent.left
          spacing: Style.space(2)

          Button {
            iconText: Model.GLYPH_PREVIOUS
            tooltipText: "Previous"
            foreground: root.foreground
            accent: root.green
            enabled: root.service ? root.service.canGoPrevious : false
            opacity: enabled ? 1.0 : 0.4
            onClicked: root.service.previous()
          }

          Button {
            iconText: root.playing ? Model.GLYPH_PAUSED : Model.GLYPH_PLAYING
            tooltipText: root.playing ? "Pause" : "Play"
            foreground: root.green
            accent: root.green
            iconSize: Style.font.iconLarge
            horizontalPadding: Style.spacing.panelGap
            enabled: root.service ? root.service.canPlay : false
            opacity: enabled ? 1.0 : 0.4
            onClicked: root.service.playPause()
          }

          Button {
            iconText: Model.GLYPH_STOPPED
            tooltipText: "Stop"
            foreground: root.foreground
            accent: root.green
            enabled: root.service ? (root.service.canPlay && !root.stopped) : false
            opacity: enabled ? 1.0 : 0.4
            onClicked: root.service.stop()
          }

          Button {
            iconText: Model.GLYPH_NEXT
            tooltipText: "Next"
            foreground: root.foreground
            accent: root.green
            enabled: root.service ? root.service.canGoNext : false
            opacity: enabled ? 1.0 : 0.4
            onClicked: root.service.next()
          }
        }

        Row {
          anchors.right: parent.right
          anchors.verticalCenter: transport.verticalCenter
          spacing: Style.space(2)

          Button {
            iconText: Model.GLYPH_SHUFFLE
            readonly property bool on: root.service ? root.service.shuffle : false
            tooltipText: on ? "Shuffle is on" : "Shuffle is off"
            foreground: root.foreground
            accent: root.green
            selected: on
            enabled: root.service ? root.service.shuffleSupported : false
            opacity: enabled ? (on ? 1.0 : 0.55) : 0.3
            onClicked: root.service.toggleShuffle()
          }

          Button {
            readonly property bool on: root.service ? root.service.repeat : false
            iconText: on ? Model.GLYPH_REPEAT : Model.GLYPH_REPEAT_OFF
            tooltipText: on ? "Repeat is on" : "Repeat is off"
            foreground: root.foreground
            accent: root.green
            selected: on
            enabled: root.service ? root.service.loopSupported : false
            opacity: enabled ? (on ? 1.0 : 0.55) : 0.3
            onClicked: root.service.toggleRepeat()
          }
        }
      }

      // Thrum's own volume, not the system's.
      Row {
        width: parent.width
        spacing: Style.space(8)
        visible: root.service ? root.service.volumeSupported : false

        Text {
          anchors.verticalCenter: parent.verticalCenter
          width: Style.space(20)
          horizontalAlignment: Text.AlignHCenter
          text: root.service && root.service.volume < 0.01 ? Model.GLYPH_MUTED : Model.GLYPH_VOLUME
          color: root.dim
          font.family: root.fontFamily
          font.pixelSize: Style.font.body
        }

        PanelSlider {
          id: volumeSlider
          width: parent.width - Style.space(20) - volumeText.width - parent.spacing * 2
          anchors.verticalCenter: parent.verticalCenter
          bar: root.bar
          minimum: 0
          maximum: 1
          step: 0.05
          value: root.service ? root.service.volume : 0
          fillColor: root.green
          knobColor: root.green
          onMoved: function(value) { if (root.service) root.service.setVolume(value) }
        }

        Text {
          id: volumeText
          anchors.verticalCenter: parent.verticalCenter
          width: Style.space(34)
          horizontalAlignment: Text.AlignRight
          text: Math.round((volumeSlider.dragging ? volumeSlider.liveValue : (root.service ? root.service.volume : 0)) * 100) + "%"
          color: root.dim
          font.family: root.fontFamily
          font.pixelSize: Style.font.caption
          font.features: { "tnum": 1 }
        }
      }

      PanelSeparator { foreground: root.foreground }

      Button {
        width: parent.width
        text: "Show Thrum"
        iconText: Model.GLYPH_WINDOW
        tooltipText: "Bring the Thrum window forward"
        foreground: root.foreground
        accent: root.green
        bordered: true
        onClicked: {
          root.close()
          root.service.raise()
        }
      }
    }
  }

  // A cover that fades from one picture to the next. The new picture loads
  // behind the old one and fades in only once it is ready, so a track change
  // never shows a blank square. With no picture, a music note on the theme
  // background stands in (unless `placeholder` is off). Images are decoded
  // at the size drawn (SizedImage) and come from Qt's image cache.
  component CoverArt: Rectangle {
    id: art

    property string source: ""
    property bool placeholder: true
    property color backdrop: "transparent"
    property color glyphColor: "grey"
    property string glyphFamily: ""
    property int glyphSize: 24
    property int fadeDuration: Style.duration(320)

    // The front layer is the one showing; `shown` is its picture.
    property int front: 0
    property string shown: ""
    // The last picture that failed to load, so a caller can hide the cover.
    property string failedSource: ""
    readonly property bool hasPicture: shown !== ""

    color: placeholder ? backdrop : "transparent"
    border.width: placeholder ? 1 : 0
    border.color: "transparent"
    clip: radius <= 0

    function pictureLayer(i) { return i === 0 ? layerA : layerB }

    function load() {
      var want = String(source)
      var cur = pictureLayer(front)
      var back = pictureLayer(1 - front)
      if (want === "") {
        shown = ""
        fadeIn.stop()
        fadeOut.target = cur
        fadeOut.start()
        back.opacity = 0
        back.url = ""
        return
      }
      if (want === String(cur.url) && cur.status === Image.Ready) {
        fadeOut.stop()
        cur.opacity = 1
        shown = want
        return
      }
      back.z = 2
      cur.z = 1
      back.opacity = 0
      if (String(back.url) === want && back.status === Image.Ready) promote(1 - front)
      else back.url = want
    }

    function settled(i) {
      var l = pictureLayer(i)
      if (i === front || String(l.url) !== String(source)) return
      if (l.status === Image.Ready) promote(i)
      else if (l.status === Image.Error) {
        // A broken picture: show the stand-in rather than a stale cover.
        failedSource = String(source)
        l.url = ""
        if (String(pictureLayer(front).url) !== String(source)) {
          shown = ""
          fadeOut.target = pictureLayer(front)
          fadeOut.start()
        }
      }
    }

    function promote(i) {
      var old = pictureLayer(front)
      pictureLayer(i).z = 2
      if (old !== pictureLayer(i)) old.z = 1
      front = i
      shown = String(source)
      fadeOut.stop()
      fadeIn.stop()
      fadeIn.target = pictureLayer(i)
      fadeIn.retire = old
      fadeIn.start()
    }

    onSourceChanged: load()
    Component.onCompleted: load()

    Text {
      anchors.centerIn: parent
      visible: art.placeholder
      opacity: art.hasPicture ? 0 : 1
      textFormat: Text.PlainText
      text: Model.GLYPH_MUSIC
      color: art.glyphColor
      font.family: art.glyphFamily
      font.pixelSize: art.glyphSize
      Behavior on opacity { NumberAnimation { duration: art.fadeDuration } }
    }

    // Rounded corners need a mask; the software renderer cannot run it and
    // draws the square picture inside the rounded backdrop instead.
    Item {
      id: pictures
      anchors.fill: parent
      anchors.margins: art.border.width
      readonly property bool masked: art.radius > 0 && GraphicsInfo.api !== GraphicsInfo.Software
      layer.enabled: masked
      layer.smooth: true
      layer.effect: MultiEffect {
        maskEnabled: true
        maskSource: pictureMask
        maskThresholdMin: 0.5
        maskSpreadAtMin: 1.0
      }

      SizedImage {
        id: layerA
        anchors.fill: parent
        crop: true
        opacity: 0
        onStatusChanged: art.settled(0)
      }

      SizedImage {
        id: layerB
        anchors.fill: parent
        crop: true
        opacity: 0
        onStatusChanged: art.settled(1)
      }
    }

    Rectangle {
      id: pictureMask
      anchors.fill: pictures
      radius: Math.max(0, art.radius - art.border.width)
      color: "black"
      visible: false
      layer.enabled: pictures.masked
      layer.smooth: true
    }

    NumberAnimation {
      id: fadeIn
      property var retire: null
      property: "opacity"
      from: 0
      to: 1
      duration: art.fadeDuration
      easing.type: Easing.InOutQuad
      onStopped: {
        if (retire && retire !== art.pictureLayer(art.front)) {
          retire.opacity = 0
          retire.url = ""
        }
        retire = null
      }
    }

    NumberAnimation {
      id: fadeOut
      property: "opacity"
      to: 0
      duration: art.fadeDuration
      easing.type: Easing.InOutQuad
      onStopped: {
        var l = art.pictureLayer(art.front)
        if (art.shown === "" && l.opacity === 0) l.url = ""
      }
    }
  }
}
