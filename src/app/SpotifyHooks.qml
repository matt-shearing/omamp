import QtQuick

// Spotify's part of the window: its actions (palette and menu bar), opening
// the Spotify node or Settings → Spotify, and its messages as toasts. Kept
// here so Main.qml only gains a line or two.
Item {
    id: hooks
    visible: false
    // The window (goToNode, focusSpotifySearch), the settings sheet and the toast.
    required property var window
    required property var settings
    required property var toast
    // Settings → Spotify's tab.
    property int settingsPage: 5

    function showSpotify() {
        if (spotify.enabled) window.goToNode(["spotify"])
        else settings.openPage(settingsPage)
    }

    // Spotify's node with its search box taking the keys, from anywhere:
    // the deck, the big view or another node. Switched off, Settings →
    // Spotify, where it is switched on.
    function searchSpotify(text) {
        if (!spotify.enabled) { settings.openPage(settingsPage); return }
        window.goToNode(["spotify"])
        window.focusSpotifySearch(text || "")
    }

    // True when it was one of Spotify's actions.
    function runAction(id) {
        switch (id) {
        case "spotify": showSpotify(); return true
        case "settings-spotify": settings.openPage(settingsPage); return true
        case "spotify-search": searchSpotify(""); return true
        case "spotify-like":
            if (spotify.nowPlaying.active) spotify.setPlayingLiked(spotify.nowPlaying.liked !== true)
            return true
        }
        if (id.startsWith("spotify-section:")) {
            if (!spotify.enabled) { settings.openPage(settingsPage); return true }
            window.goToNode(["spotify"])
            spotify.openSection(id.slice(16))
            return true
        }
        return false
    }

    function updateActions(p) {
        const browse = spotify.enabled && spotify.canBrowse
        p.setActionEnabled("spotify-search", spotify.enabled)
        for (const s of ["playlists", "liked", "albums", "artists"])
            p.setActionEnabled("spotify-section:" + s, browse)
        p.setActionEnabled("spotify-like", spotify.enabled && spotify.nowPlaying.active === true
                                           && (spotify.nowPlaying.uri || "") !== "")
    }

    Component.onCompleted: {
        commandPalette.addAction("spotify", qsTr("Spotify: playlists, Liked Songs and search"), ["spotify", "streaming", "liked"], "")
        commandPalette.addAction("spotify-search", qsTr("Search Spotify…"), ["spotify", "find", "streaming", "search"], "")
        commandPalette.addAction("spotify-like", qsTr("Like the playing Spotify track"), ["spotify", "heart", "liked songs", "save"], "")
        commandPalette.addAction("settings-spotify", qsTr("Spotify settings: switch on, sign in…"), ["spotify", "sign in", "premium", "spotify_player", "spotifast"], "")
    }

    Connections {
        target: spotify
        function onShowRequested() { hooks.showSpotify() }
        function onSettingsRequested() { hooks.settings.openPage(hooks.settingsPage) }
        function onMessageChanged() {
            if (spotify.message === "") return
            hooks.toast.show(spotify.message)
            spotify.dismissMessage()
        }
        function onSettingsChanged() {
            if (!spotify.enabled && library.currentKind === "spotify") library.selectNode(0)
        }
    }
}
