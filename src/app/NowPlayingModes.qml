import QtCore
import QtQuick

// The window's now-playing modes, kept out of Main.qml: the mini and micro
// players (Ctrl+M, Ctrl+Shift+M), the big Now Playing view (N), fullscreen
// (F11), and their palette actions. Main.qml asks this object for the
// window's size and title while a compact player shows.
//
// Each compact player remembers its own size in window.conf under
// [compact]. Which mode was showing is not remembered: Omamp opens as the
// full window unless started with `omamp mini` or `omamp micro`, because a
// compact player that Hyprland tiles instead of floating is no use.
QtObject {
    id: modes
    // The ApplicationWindow and its Settings (prefs).
    required property var window
    required property var prefs

    // "", "mini" or "micro".
    property string compact: ""
    property bool bigView: false
    readonly property bool fullScreen: window.visibility === Window.FullScreen

    // A fixed title, so a Hyprland window rule can find a compact player.
    readonly property string title: compact === "micro" ? "Omamp micro" : "Omamp mini"
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
        compact = mode
        if (mode !== "") {
            window.width = compactWidth
            window.height = compactHeight
        } else {
            window.width = prefs.advanced ? prefs.advancedWidth : prefs.deckWidth
            window.height = prefs.advanced ? prefs.advancedHeight : prefs.deckHeight
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

    // `omamp mini` and `omamp micro` start here.
    function start(mode) {
        if (mode === "mini" || mode === "micro") setCompact(mode)
    }

    function registerActions(p) {
        p.addAction("big-view", qsTr("Big Now Playing view"), ["now playing", "cover", "art", "fullscreen", "large", "display"], "N")
        p.addAction("fullscreen", qsTr("Fullscreen Now Playing"), ["full screen", "big", "display", "tv"], "F11")
        p.addAction("mini-player", qsTr("Mini player on or off"), ["compact", "small", "miniplayer", "windowshade"], "Ctrl+M")
        p.addAction("micro-player", qsTr("Micro player on or off (Winamp windowshade)"), ["compact", "tiny", "microplayer", "windowshade", "shade"], "Ctrl+Shift+M")
        p.addAction("next-visualiser", qsTr("Next visualiser style"), ["visualizer", "analyser", "analyzer", "spectrum", "oscilloscope", "scope", "fire", "milkdrop"], "Shift+V")
        p.addAction("big-lyrics", qsTr("Lyrics in the big view on or off"), ["words", "karaoke", "lrc"], "Shift+L")
        p.addAction("settings-appearance", qsTr("Appearance: deck style, visualiser, big view…"), ["skin", "theme", "classic", "silver", "winamp", "mediamonkey", "look"], "")
        for (const style of appearance.deckStyles)
            p.addAction("deck-style:" + style, qsTr("Deck style: %1").arg(appearance.deckStyleName(style)), ["skin", "theme", "look", "winamp", "mediamonkey"], "")
    }

    // Runs one of the actions above; false when the id isn't one of them.
    function runAction(id) {
        switch (id) {
        case "big-view": setBigView(!bigView); return true
        case "fullscreen": toggleFullScreen(); return true
        case "mini-player": setCompact("mini"); return true
        case "micro-player": setCompact("micro"); return true
        case "next-visualiser": appearance.nextVisualiser(); return true
        case "big-lyrics": appearance.bigLyrics = !appearance.bigLyrics; return true
        }
        if (id.startsWith("deck-style:")) {
            appearance.deckStyle = id.slice(11)
            return true
        }
        return false
    }
}
