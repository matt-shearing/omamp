import QtQuick
import Quickshell
import Quickshell.Io
import Quickshell.Services.Mpris
import qs.Commons
import qs.Commons as Commons
import "Model.js" as Model

// The one place that acts on Omamp. The bar widget exists once per monitor,
// so every copy reads from here and calls these functions; the position
// ticker, the theme-green reader, the launcher and the IPC target live here
// once. The player itself is Quickshell's shared MPRIS object, so there is no
// state to keep in sync: Omamp's own D-Bus properties are the truth.
Item {
  id: root

  // Injected by omarchy-shell's service loader.
  property var shell: null
  property var manifest: null
  property var pluginRegistry: null

  readonly property var players: Mpris.players ? Mpris.players.values : []
  readonly property var player: Model.findOmamp(players)
  readonly property bool running: player !== null && player !== undefined

  readonly property bool playing: running && player.isPlaying === true
  readonly property bool stopped: running && player.playbackState === MprisPlaybackState.Stopped
  readonly property string state: Model.stateName(running, playing, stopped)

  readonly property string title: running ? String(player.trackTitle || "") : ""
  readonly property string artist: running ? String(player.trackArtist || "") : ""
  readonly property string album: running ? String(player.trackAlbum || "") : ""
  // The cover, as Omamp sends it over MPRIS (mpris:artUrl, a file:// URL).
  readonly property string artUrl: running ? String(player.trackArtUrl || "") : ""
  // The cover the bar and panel draw: artUrl when it is a picture, held for a
  // moment when it goes away so a track change does not blink the thumbnail.
  // Kept here once, so every monitor's widget agrees.
  property string cover: ""
  onArtUrlChanged: root.updateCover()
  onRunningChanged: root.updateCover()
  Component.onCompleted: root.updateCover()

  function updateCover() {
    var step = Model.coverStep(root.cover, root.artUrl, root.running)
    root.cover = step.cover
    if (step.hold) coverHold.restart()
    else coverHold.stop()
  }

  Timer {
    id: coverHold
    interval: 1500
    onTriggered: if (Model.coverSource(root.artUrl) === "") root.cover = ""
  }
  readonly property string line: Model.trackLine(artist, title)
  readonly property bool hasTrack: title !== "" || artist !== ""

  readonly property real length: running && player.lengthSupported ? Math.max(0, player.length) : 0
  // Quickshell extrapolates position between reads, so it can run past the
  // end for a moment before the next track arrives.
  readonly property real position: {
    if (!running || !player.positionSupported) return 0
    var p = Math.max(0, player.position)
    return length > 0 ? Math.min(p, length) : p
  }
  readonly property real progress: length > 0 ? Model.clamp01(position / length) : 0

  readonly property bool canControl: running && player.canControl === true
  readonly property bool canPlay: running && (player.canPlay === true || player.canTogglePlaying === true)
  readonly property bool canSeek: running && player.canSeek === true && length > 0
  readonly property bool canGoNext: running && player.canGoNext === true
  readonly property bool canGoPrevious: running && player.canGoPrevious === true
  readonly property bool volumeSupported: running && player.volumeSupported === true
  readonly property real volume: volumeSupported ? Model.clamp01(player.volume) : 0
  readonly property bool shuffleSupported: running && player.shuffleSupported === true
  readonly property bool shuffle: shuffleSupported && player.shuffle === true
  readonly property bool loopSupported: running && player.loopSupported === true
  // Omamp repeats the whole playlist or nothing; MPRIS "Track" reads as on.
  readonly property bool repeat: loopSupported && player.loopState !== MprisLoopState.None

  // ---------------------------------------------------------------- the green
  // Omamp draws its clock and title in the theme's `green`. The shell's Color
  // singleton has no green, so read colors.toml the same way Omamp does, and
  // re-read whenever the shell reports a theme change.
  readonly property string themeDir: (Quickshell.env("HOME") || "") + "/.local/state/omarchy/current/theme"
  property string themeGreen: ""
  readonly property color green: themeGreen !== "" ? themeGreen : Commons.Color.accent

  FileView {
    id: colorsFile
    path: root.themeDir + "/colors.toml"
    watchChanges: true
    printErrors: false
    onFileChanged: reload()
    onLoaded: root.themeGreen = Model.pickGreen(Model.themeColors(text()))
    onLoadFailed: root.themeGreen = ""
  }

  // A theme switch swaps the `current/theme` symlink, which a file watch can
  // miss; the shell's own colours changing is the dependable signal.
  Connections {
    target: Commons.Color
    function onAccentChanged() { themeReload.restart() }
    function onForegroundChanged() { themeReload.restart() }
    function onBackgroundChanged() { themeReload.restart() }
  }

  Timer {
    id: themeReload
    interval: 150
    onTriggered: colorsFile.reload()
  }

  // ------------------------------------------------------------------ actions
  function playPause() {
    if (!running) return false
    if (player.canTogglePlaying) { player.togglePlaying(); return true }
    if (playing && player.canPause) { player.pause(); return true }
    if (!playing && player.canPlay) { player.play(); return true }
    return false
  }

  function stop() {
    if (!running || !player.canControl) return false
    player.stop()
    return true
  }

  function next() {
    if (!canGoNext) return false
    player.next()
    return true
  }

  function previous() {
    if (!canGoPrevious) return false
    player.previous()
    return true
  }

  function seekTo(seconds) {
    if (!canSeek) return false
    player.position = Math.max(0, Math.min(length, Number(seconds) || 0))
    return true
  }

  function setVolume(value) {
    if (!volumeSupported) return false
    player.volume = Model.clamp01(value)
    return true
  }

  function nudgeVolume(delta) {
    if (!volumeSupported) return false
    return setVolume(player.volume + delta)
  }

  function toggleShuffle() {
    if (!shuffleSupported) return false
    player.shuffle = !player.shuffle
    return true
  }

  function toggleRepeat() {
    if (!loopSupported) return false
    player.loopState = repeat ? MprisLoopState.None : MprisLoopState.Playlist
    return true
  }

  // MPRIS Raise shows and raises the window, but Hyprland ignores an
  // activation request that comes without a token, so follow it with
  // Omarchy's own focus helper, which switches to the window's workspace.
  function raise() {
    if (!running) return launch()
    if (player.canRaise) player.raise()
    focusDelay.restart()
    return true
  }

  // The shell's PATH starts with $OMARCHY_PATH/bin, so name the user install
  // by absolute path first and fall back to PATH for the package.
  function launch() {
    Quickshell.execDetached(["bash", "-c",
      'if [ -x "$HOME/.local/bin/omamp" ]; then app="$HOME/.local/bin/omamp"; else app=omamp; fi; '
      + 'exec omarchy-launch-or-focus "^omamp$" "uwsm-app -- $app"'])
    return true
  }

  Timer {
    id: focusDelay
    interval: 120
    onTriggered: Quickshell.execDetached(["omarchy-hyprland-focus-app", "^omamp$"])
  }

  // MPRIS position is only re-read when something asks, so ask while it is
  // moving. Cheap: Quickshell extrapolates rather than calling D-Bus.
  Timer {
    running: root.playing
    interval: 500
    repeat: true
    onTriggered: if (root.player) root.player.positionChanged()
  }

  function statusJson() {
    return JSON.stringify({
      running: root.running,
      copies: Model.countOmamp(root.players),
      state: root.state,
      bus: root.running ? String(root.player.dbusName || "") : "",
      title: root.title,
      artist: root.artist,
      album: root.album,
      artUrl: root.artUrl,
      cover: root.cover,
      position: Math.round(root.position),
      length: Math.round(root.length),
      volume: Math.round(root.volume * 100),
      shuffle: root.shuffle,
      repeat: root.repeat,
      green: String(root.green)
    })
  }

  IpcHandler {
    target: "contra.omamp"

    function status(): string { return root.statusJson() }
    function playPause(): string { return root.playPause() ? "ok" : "unhandled" }
    function stop(): string { return root.stop() ? "ok" : "unhandled" }
    function next(): string { return root.next() ? "ok" : "unhandled" }
    function previous(): string { return root.previous() ? "ok" : "unhandled" }
    function shuffle(): string { return root.toggleShuffle() ? "ok" : "unhandled" }
    function repeat(): string { return root.toggleRepeat() ? "ok" : "unhandled" }
    function volume(percent: string): string {
      return root.setVolume(Number(percent) / 100) ? "ok" : "unhandled"
    }
    function raise(): string { return root.raise() ? "ok" : "unhandled" }
    function launch(): string { return root.launch() ? "ok" : "unhandled" }
  }
}
