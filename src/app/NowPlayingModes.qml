import QtCore
import QtQuick

// The window's now-playing modes, kept out of Main.qml: the mini and micro
// players (Ctrl+M, Ctrl+Shift+M), the big Now Playing view (N), fullscreen
// (F11), and their palette actions. Main.qml asks this object for the
// window's size and title while a compact player shows.
//
// Each compact player remembers its own size in window.conf under
// [compact]. Which mode was showing is not remembered: Thrum opens as the
// full window unless started with `thrum mini` or `thrum micro`, because a
// compact player that Hyprland tiles instead of floating is no use.
//
// On Hyprland a window rule applies only when a window opens, so switching
// asks Hyprland (WindowPlacement) to float, pin, size and place the window
// top right under the bar, and to put it back as it was afterwards.
QtObject {
    id: modes
    // The ApplicationWindow and its Settings (prefs).
    required property var window
    required property var prefs

    // "", "mini" or "micro". `thrum mini` / `thrum micro` start in one, so
    // the window has its title and size before its first frame and a
    // Hyprland window rule for the title applies when it opens.
    property string compact: typeof startMode === "string" && (startMode === "mini" || startMode === "micro")
                             ? startMode : ""
    // Hyprland is placing the window (WindowPlacement): the content stays
    // hidden until the new size arrives, so no frame shows a layout at the
    // wrong size.
    property bool placing: false
    readonly property var placement: typeof windowPlacement !== "undefined" ? windowPlacement : null
    readonly property bool hyprland: placement !== null && placement.available
    property bool bigView: false
    readonly property bool fullScreen: window.visibility === Window.FullScreen

    // A fixed title, so a Hyprland window rule can find a compact player.
    readonly property string title: compact === "micro" ? "Thrum micro" : "Thrum mini"
    readonly property int compactWidth: compact === "micro" ? sizes.microWidth : sizes.miniWidth
    readonly property int compactHeight: compact === "micro" ? sizes.microHeight : sizes.miniHeight
    readonly property int minimumWidth: compact === "micro" ? 220 : 320
    readonly property int minimumHeight: compact === "micro" ? 24 : 56

    property Settings sizes: Settings {
        location: settingsUrl
        category: "compact"
        property int miniWidth: 420
        property int miniHeight: 64
        property int microWidth: 275
        property int microHeight: 28
    }

    // Keeps the size the compact player has now, for next time.
    function rememberSize() {
        if (window.sizeManaged) return
        if (compact === "mini") {
            sizes.miniWidth = window.width
            sizes.miniHeight = window.height
        } else if (compact === "micro") {
            sizes.microWidth = window.width
            sizes.microHeight = window.height
        }
    }

    // Switches to a compact player, or back to the full window with "" (or
    // with the mode that is already showing, so the same key toggles).
    function setCompact(mode) {
        if (mode === compact) mode = ""
        if (compact !== "") rememberSize()
        else window.rememberLayout()
        if (fullScreen) window.showNormal()
        bigView = false
        // The title first, then the size.
        compact = mode
        if (hyprland) {
            // Hyprland floats, pins and sizes the window, or tiles it again
            // (compactPlaced / compactLeft below).
            placing = true
            settle.restart()
            if (mode !== "") placement.enterCompact(title, compactWidth, compactHeight, false)
            else placement.leaveCompact()
            return
        }
        resizeForMode()
    }

    // Without Hyprland, or when it said no: the window asks for the size
    // itself; nothing happens where the compositor decides (sizeManaged),
    // and the compact player then lays its strip along the top.
    function resizeForMode() {
        if (compact !== "") window.resizeTo(compactWidth, compactHeight)
        else window.resizeTo(prefs.advanced ? prefs.advancedWidth : prefs.deckWidth,
                             prefs.advanced ? prefs.advancedHeight : prefs.deckHeight)
    }

    // The window's size has caught up with the mode: compact-sized in a
    // compact player, larger out of one.
    function sizeArrived() {
        const small = window.width <= compactWidth + 40 && window.height <= compactHeight + 40
        return compact !== "" ? small : !small
    }

    // Shows the content once Hyprland has answered and the size has
    // arrived, or after a second whatever happened.
    property Timer settle: Timer {
        interval: 1000
        onTriggered: modes.placing = false
    }
    property bool answered: false
    property Timer poll: Timer {
        interval: 16
        repeat: true
        running: modes.placing && modes.answered
        onTriggered: if (modes.sizeArrived()) { modes.placing = false; modes.answered = false }
    }
    onPlacingChanged: if (!placing) { answered = false; settle.stop() }

    property Connections placed: Connections {
        target: modes.placement
        ignoreUnknownSignals: true
        function onCompactPlaced(ok) {
            if (modes.compact === "") return // left again meanwhile
            modes.answered = true
            if (!ok) { modes.resizeForMode(); modes.placing = false }
        }
        function onCompactLeft(ok) {
            if (modes.compact !== "") return
            modes.answered = true
            if (!ok) { modes.resizeForMode(); modes.placing = false }
        }
    }

    function setBigView(on) {
        if (on && compact !== "") setCompact("")
        if (!on && fullScreen) window.showNormal()
        bigView = on
    }

    // F11: fullscreen, in the big view.
    function toggleFullScreen() {
        if (fullScreen) {
            window.showNormal()
            return
        }
        if (!bigView) setBigView(true)
        window.showFullScreen()
    }

    // Esc: out of fullscreen first, then out of the big view.
    function back() {
        if (fullScreen) window.showNormal()
        else if (bigView) bigView = false
    }

    // `thrum mini` and `thrum micro` start here.
    // The window already opened as that player (compact starts from
    // startMode); without a window rule Hyprland tiled it, so ask it to
    // float the window too. Going back then tiles it.
    function start(mode) {
        if (mode !== "mini" && mode !== "micro") return
        if (compact !== mode) compact = mode
        if (!hyprland) return
        placing = true
        settle.restart()
        placement.enterCompact(title, compactWidth, compactHeight, true)
    }

    function registerActions(p) {
        p.addAction("big-view", qsTr("Big Now Playing view"), ["now playing", "cover", "art", "fullscreen", "large", "display"], "N")
        p.addAction("fullscreen", qsTr("Fullscreen Now Playing"), ["full screen", "big", "display", "tv"], "F11")
        p.addAction("mini-player", qsTr("Mini player on or off"), ["compact", "small", "miniplayer", "windowshade"], "Ctrl+M")
        p.addAction("micro-player", qsTr("Micro player on or off (Winamp windowshade)"), ["compact", "tiny", "microplayer", "windowshade", "shade"], "Ctrl+Shift+M")
        p.addAction("choose-visualiser", qsTr("Choose visualiser…"), ["visualizer", "analyser", "analyzer", "spectrum", "style", "picker", "gallery"], "")
        p.addAction("next-visualiser", qsTr("Next visualiser style"), ["visualizer", "analyser", "analyzer", "spectrum", "oscilloscope", "scope", "fire", "milkdrop"], "Shift+V")
        p.addAction("big-lyrics", qsTr("Lyrics in the big view on or off"), ["words", "karaoke", "lrc"], "Shift+L")
        p.addAction("settings-appearance", qsTr("Appearance: deck style, visualiser, big view…"), ["skin", "theme", "classic", "silver", "winamp", "mediamonkey", "look"], "")
        for (const place of appearance.barPlacements)
            p.addAction("bar-visualiser:" + place, qsTr("Large bar visualiser: %1").arg(appearance.barPlacementName(place)), ["visualizer", "analyser", "spectrum", "now playing bar"], "")
        p.addAction("reduce-motion", qsTr("Reduce motion on or off"), ["animation", "calm", "visualiser", "accessibility"], "")
        for (const style of appearance.deckStyles)
            p.addAction("deck-style:" + style, qsTr("Deck style: %1").arg(appearance.deckStyleName(style)), ["skin", "theme", "look", "winamp", "mediamonkey"], "")
    }

    // Whether the visualiser in view is the large bar's: the library shows
    // with the large bar, and neither the big view nor a compact player.
    function barVisualiserShown() {
        return window.advanced === true && compact === "" && !bigView && appearance.layout.barSize === "large"
    }

    // Runs one of the actions above; false when the id isn't one of them.
    function runAction(id) {
        switch (id) {
        case "big-view": setBigView(!bigView); return true
        case "fullscreen": toggleFullScreen(); return true
        case "mini-player": setCompact("mini"); return true
        case "micro-player": setCompact("micro"); return true
        // The library's large bar has its own style when one is chosen.
        case "next-visualiser": appearance.cycleVisualiser(barVisualiserShown()); return true
        case "big-lyrics": appearance.bigLyrics = !appearance.bigLyrics; return true
        // The picker, in the middle of the window, for the visualiser in view.
        case "choose-visualiser":
            appearance.choosePicker(null, barVisualiserShown() ? "bar" : bigView ? "big" : "player")
            return true
        }
        if (id === "reduce-motion") {
            appearance.reduceMotion = !appearance.reduceMotion
            return true
        }
        if (id.startsWith("bar-visualiser:")) {
            appearance.barVisualiser = id.slice(15)
            return true
        }
        if (id.startsWith("deck-style:")) {
            appearance.deckStyle = id.slice(11)
            return true
        }
        return false
    }
}
