import QtQuick

// Spotify's part of the window: its palette actions, opening the Spotify
// node or Settings → Spotify, and its messages as toasts. Kept here so
// Main.qml only gains one line.
Item {
    id: hooks
    visible: false
    // The window (goToNode), the settings sheet and the toast.
    required property var window
    required property var settings
    required property var toast
    // Settings → Spotify's tab.
    property int settingsPage: 5

    function showSpotify() {
        if (spotify.enabled) window.goToNode(["spotify"])
        else settings.openPage(settingsPage)
    }

    Component.onCompleted: {
        commandPalette.addAction("spotify", qsTr("Spotify: playlists, Liked Songs and search"), ["spotify", "streaming", "liked"], "")
        commandPalette.addAction("settings-spotify", qsTr("Spotify settings: switch on, sign in…"), ["spotify", "sign in", "premium", "spotify_player", "spotifast"], "")
    }

    Connections {
        target: commandPalette
        function onActionTriggered(id) {
            if (id === "spotify") hooks.showSpotify()
            else if (id === "settings-spotify") hooks.settings.openPage(hooks.settingsPage)
        }
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
