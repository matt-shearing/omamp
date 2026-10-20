import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material

// "Add to playlist" for the chosen Spotify rows, in two groups: Omamp's own
// playlists (a mixed playlist can hold Spotify tracks beside local files)
// and your Spotify playlists. Each group starts with a way to make a new
// one; the window asks for its name (SpotifyHooks).
Menu {
    id: menu
    Material.roundedScale: theme.radius
    title: qsTr("Add to playlist")
    // The list whose chosen rows are added: spotify.items or spotify.blendResults.
    property var rows: spotify.items
    // Which list, for the window's "new playlist" actions: view or blend.
    property string source: "view"
    signal action(string id)
    readonly property bool omampAllowed: { void library.dj.partyOn; return library.dj.allowed("edit-playlists") }

    topPadding: 4; bottomPadding: 4
    // As wide as its widest line (Material's menus stay 200 pixels wide and
    // cut longer playlist names off), as the menu bar's menus are.
    function fitWidth() {
        let widest = 160
        for (let i = 0; i < count; ++i) {
            const item = itemAt(i)
            if (item && item.visible !== false && !(item instanceof MenuSeparator))
                widest = Math.max(widest, item.implicitWidth)
        }
        width = Math.min(widest + leftPadding + rightPadding, 420)
    }
    onAboutToShow: fitWidth()
    // A plain panel, as the menu bar's: Material's shadow is drawn by a GPU
    // effect, which leaves a menu without any background in software rendering.
    background: Rectangle {
        implicitWidth: 200
        implicitHeight: 30
        radius: Math.min(6, theme.radius)
        color: theme.c.background
        border.width: 1
        border.color: theme.c.lighter_background === theme.c.background ? theme.c.selection : theme.c.lighter_background
    }

    // Compact lines, like the track lists' menus' text.
    component AddEntry: MenuItem {
        implicitHeight: 32
        font.pixelSize: 13
    }

    // Where a group's playlists go: after its "New … playlist" entry.
    function indexAfter(item) {
        for (let i = 0; i < menu.count; ++i)
            if (menu.itemAt(i) === item) return i + 1
        return menu.count
    }

    AddEntry {
        objectName: "spotifyAddOmampHeading"
        text: qsTr("Omamp playlists")
        enabled: false
        implicitHeight: 28
        font.bold: true
        font.pixelSize: 12
    }
    AddEntry {
        id: newOmamp
        objectName: "spotifyAddNewOmamp"
        text: qsTr("New Omamp playlist…")
        enabled: menu.omampAllowed
        onTriggered: menu.action("spotify-new-omamp-playlist:" + menu.source)
    }
    MenuSeparator { }
    AddEntry {
        objectName: "spotifyAddSpotifyHeading"
        text: qsTr("Spotify playlists")
        enabled: false
        implicitHeight: 28
        font.bold: true
        font.pixelSize: 12
    }
    AddEntry {
        id: newSpotify
        objectName: "spotifyAddNewSpotify"
        text: qsTr("New Spotify playlist…")
        visible: spotify.canCreatePlaylists
        height: visible ? implicitHeight : 0
        onTriggered: menu.action("spotify-new-spotify-playlist:" + menu.source)
    }

    Instantiator {
        model: { void library.playlists.revision; return library.playlists.staticPlaylists() }
        delegate: AddEntry {
            required property var modelData
            objectName: "spotifyAddOmamp:" + modelData.name
            text: modelData.name
            enabled: menu.omampAllowed
            onTriggered: spotify.addChosenToOmampPlaylist(menu.rows, modelData.id)
        }
        onObjectAdded: (index, object) => menu.insertItem(menu.indexAfter(newOmamp) + index, object)
        onObjectRemoved: (index, object) => menu.removeItem(object)
    }
    Instantiator {
        model: spotify.ownPlaylists
        delegate: AddEntry {
            required property var modelData
            objectName: "spotifyAddSpotify:" + modelData.name
            text: modelData.name
            onTriggered: spotify.addChosenToSpotifyPlaylist(menu.rows, modelData.id)
        }
        onObjectAdded: (index, object) => menu.insertItem(menu.indexAfter(newSpotify) + index, object)
        onObjectRemoved: (index, object) => menu.removeItem(object)
    }
}
