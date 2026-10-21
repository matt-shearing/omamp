import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material

// The menu bar across the top of the deck and the library: every command
// Thrum has, with its key beside it, so nothing needs to be remembered.
// Each entry runs the same action id as its key and the command palette
// (Main.qml's runAction), so the three never disagree. An entry is greyed
// when it can't run now: nothing chosen, nothing to undo, or locked by
// party mode. It hides in the mini and micro players and the big view.
//
// The keys are the window's own Shortcuts; the entries only show them, so
// a key never fires twice.
MenuBar {
    id: bar
    objectName: "mainMenuBar"
    // The ApplicationWindow (Main.qml), its now-playing modes and its prefs.
    required property var window
    required property var modes
    required property var prefs

    // A menu is open: the window's single-letter keys wait until it closes.
    readonly property bool anyOpen: fileMenu.visible || editMenu.visible || viewMenu.visible || playMenu.visible
                                    || libraryMenu.visible || radioMenu.visible || spotifyMenu.visible || helpMenu.visible
    // The radio's stations, read when the Radio menu opens.
    property var stations: []
    // Opens the first menu (F10), as on other desktops.
    function openFirst() {
        const first = itemAt(0)
        if (first) first.triggered()
    }

    Material.roundedScale: theme.radius
    font.pixelSize: 13

    background: Rectangle {
        implicitHeight: 30
        color: theme.c.dark_background
        Rectangle { anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
                    height: 1; color: theme.c.lighter_background }
    }
    delegate: MenuBarItem {
        id: barItem
        objectName: "menuTitle_" + (menu ? menu.objectName : "")
        leftPadding: 11; rightPadding: 11
        topPadding: 5; bottomPadding: 5
        font.pixelSize: 13
        background: Rectangle {
            implicitHeight: 29
            color: barItem.highlighted || barItem.down ? theme.c.selection : "transparent"
        }
    }

    // One line of a menu: a mark (✓ or ●) on the left, the words, and the
    // key on the right. It runs `command` through the window, unless it is a
    // line of its own (a station, a style) that sets onTriggered itself.
    component Entry: MenuItem {
        id: entry
        property string command: ""
        property string keys: ""
        // "check", "radio" or "": what the left column shows while marked.
        property string markStyle: ""
        property bool marked: false
        // The window, which runs the action: its menu's.
        readonly property var host: menu ? menu.host : null
        // Whether the entry belongs in the menu now (Spotify's, while it is
        // on); visible alone also reads false while the menu is closed.
        property bool shown: true
        visible: shown
        enabled: subMenu ? subMenu.enabled : command === "" || (host !== null && host.actionEnabled(command))
        height: shown ? implicitHeight : 0
        leftPadding: 10; rightPadding: subMenu ? 34 : 14
        topPadding: 0; bottomPadding: 0
        font.pixelSize: 13
        // After the menu has closed, so an action that moves the keys
        // (Find, Search Spotify) keeps them.
        onTriggered: if (command !== "" && host) Qt.callLater(host.runAction, command)
        contentItem: Item {
            implicitWidth: 24 + label.implicitWidth + (keyLabel.text !== "" ? 36 + keyLabel.implicitWidth : 0)
            implicitHeight: 30
            Text {
                anchors { left: parent.left; verticalCenter: parent.verticalCenter }
                width: 24
                horizontalAlignment: Text.AlignHCenter
                visible: entry.marked && entry.markStyle !== ""
                text: entry.markStyle === "radio" ? "●" : "✓"
                font.pixelSize: entry.markStyle === "radio" ? 10 : 13
                color: theme.accent
            }
            Text {
                id: label
                anchors { left: parent.left; leftMargin: 24; verticalCenter: parent.verticalCenter }
                text: entry.text
                font: entry.font
                color: entry.enabled ? theme.c.foreground : theme.c.muted
            }
            Text {
                id: keyLabel
                anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                text: entry.keys
                font.family: "JetBrainsMono Nerd Font"
                font.pixelSize: 11
                color: entry.enabled ? theme.c.light_foreground : theme.c.muted
            }
        }
        background: Rectangle {
            implicitWidth: 220
            implicitHeight: 30
            color: entry.highlighted ? theme.c.selection : "transparent"
        }
    }
    component Line: MenuSeparator {
        property bool shown: true
        visible: shown
        topPadding: 3; bottomPadding: 3
        height: shown ? implicitHeight : 0
        contentItem: Rectangle { implicitWidth: 200; implicitHeight: 1; color: theme.c.lighter_background }
    }
    component BarMenu: Menu {
        id: barMenu
        // The window (Main.qml): runs and greys the entries.
        property var host: null
        delegate: Entry {}
        topPadding: 4; bottomPadding: 4
        Material.roundedScale: theme.radius
        // Closes at once, so the next thing to take the keys keeps them.
        exit: null
        // As wide as its widest line: Material's menus stay 200 pixels wide
        // and cut longer lines off.
        function fitWidth() {
            let widest = 160
            for (let i = 0; i < count; ++i) {
                const item = itemAt(i)
                if (item && item.shown !== false && !(item instanceof MenuSeparator)) widest = Math.max(widest, item.implicitWidth)
            }
            width = widest + leftPadding + rightPadding
        }
        // Every greyed entry is worked out again as a menu opens.
        onAboutToShow: {
            if (host) host.updateActions()
            fitWidth()
        }
        // A plain panel: Material's shadow is drawn by a GPU effect, which
        // leaves the menu without any background in software rendering.
        background: Rectangle {
            implicitWidth: 200
            implicitHeight: 30
            radius: Math.min(6, theme.radius)
            color: theme.c.background
            border.width: 1
            border.color: theme.c.lighter_background === theme.c.background ? theme.c.selection : theme.c.lighter_background
        }
    }

    // ---- File ---------------------------------------------------------------
    BarMenu {
        id: fileMenu
        host: bar.window
        objectName: "fileMenu"
        title: qsTr("&File")
        Entry { text: qsTr("Add folder to library…"); command: "library-add-folder" }
        Entry { text: qsTr("Open folder in Now Playing…"); command: "open-folder"; keys: bar.window.advanced ? "" : "O" }
        Line {}
        Entry { text: qsTr("Import playlists (M3U, PLS)…"); command: "import-playlists" }
        Entry { text: qsTr("Export the chosen playlist…"); command: "export-playlist" }
        Entry { text: qsTr("Save Now Playing as a playlist…"); command: "save-now-playing" }
        Line {}
        Entry { text: qsTr("Import radio stations…"); command: "radio-import" }
        Entry { text: qsTr("Save radio stations as a list…"); command: "radio-export" }
        Line {}
        Entry { text: qsTr("Settings…"); command: "settings"; keys: "Ctrl+," }
        Line {}
        Entry { text: qsTr("Quit"); command: "quit"; keys: "Q" }
    }

    // ---- Edit ---------------------------------------------------------------
    BarMenu {
        id: editMenu
        host: bar.window
        objectName: "editMenu"
        title: qsTr("&Edit")
        Entry {
            objectName: "menuUndo"
            text: library.undoText !== "" ? qsTr("Undo: %1").arg(library.undoText) : qsTr("Undo")
            command: "undo"; keys: "Ctrl+Z"
        }
        Entry {
            objectName: "menuRedo"
            text: library.redoText !== "" ? qsTr("Redo: %1").arg(library.redoText) : qsTr("Redo")
            command: "redo"; keys: "Ctrl+Shift+Z"
        }
        Entry { text: qsTr("Undo history…"); command: "undo-history" }
        Line {}
        Entry { text: qsTr("Edit tags…"); command: "edit-tags"; keys: "Ctrl+E" }
        Entry { text: qsTr("Rename title"); command: "edit-title"; keys: "F2" }
        Entry { text: qsTr("Select all"); command: "select-all"; keys: "Ctrl+A" }
        Line {}
        Entry { objectName: "menuFind"; text: qsTr("Find in the library"); command: "search"; keys: "Ctrl+F" }
        Entry { text: qsTr("Command palette…"); command: "palette"; keys: "Ctrl+K" }
    }

    // ---- View ---------------------------------------------------------------
    BarMenu {
        id: viewMenu
        host: bar.window
        objectName: "viewMenu"
        title: qsTr("&View")
        Entry {
            objectName: "menuViewPlayer"
            text: qsTr("Player"); command: "view-player"; keys: "Tab"
            markStyle: "radio"; marked: !bar.window.advanced && !bar.modes.bigView
        }
        Entry {
            objectName: "menuViewLibrary"
            text: qsTr("Library"); command: "view-library"; keys: "Tab"
            markStyle: "radio"; marked: bar.window.advanced && !bar.modes.bigView
        }
        Entry {
            objectName: "menuViewNowPlaying"
            text: qsTr("Now Playing (big view)"); command: "big-view"; keys: "N"
            markStyle: "radio"; marked: bar.modes.bigView
        }
        Line {}
        Entry { text: qsTr("Mini player"); command: "mini-player"; keys: "Ctrl+M" }
        Entry { text: qsTr("Micro player"); command: "micro-player"; keys: "Ctrl+Shift+M" }
        Entry { text: qsTr("Fullscreen"); command: "fullscreen"; keys: "F11" }
        Line {}
        Entry { objectName: "menuBrowser"; text: qsTr("Column browser"); command: "browser"; keys: "Ctrl+B"
                markStyle: "check"; marked: bar.window.advanced && bar.prefs.browserVisible }
        Entry { objectName: "menuGrid"; text: qsTr("Album grid"); command: "grid"; keys: "G"
                markStyle: "check"; marked: bar.window.advanced && bar.prefs.gridVisible }
        Entry { objectName: "menuInfo"; text: qsTr("Sidebar"); command: "info"; keys: "I"
                markStyle: "check"; marked: bar.window.advanced && bar.prefs.infoVisible }
        // The library's layout (appearance.layout), switched at once.
        Entry { objectName: "menuArtColumn"; text: qsTr("Art column")
                markStyle: "check"; marked: appearance.layout.artColumn
                onTriggered: appearance.layout.artColumn = !appearance.layout.artColumn }
        BarMenu {
            id: layoutBarMenu
            host: bar.window
            objectName: "nowPlayingBarMenu"
            title: qsTr("Now playing bar")
            Entry { objectName: "menuBarCompact"; text: qsTr("Compact strip"); markStyle: "radio"
                    marked: appearance.layout.barSize === "compact"; onTriggered: appearance.layout.barSize = "compact" }
            Entry { objectName: "menuBarLarge"; text: qsTr("Large"); markStyle: "radio"
                    marked: appearance.layout.barSize === "large"; onTriggered: appearance.layout.barSize = "large" }
            Line {}
            Entry { objectName: "menuBarTop"; text: qsTr("At the top"); markStyle: "radio"
                    marked: appearance.layout.barPosition === "top"; onTriggered: appearance.layout.barPosition = "top" }
            Entry { objectName: "menuBarBottom"; text: qsTr("At the bottom"); markStyle: "radio"
                    marked: appearance.layout.barPosition === "bottom"; onTriggered: appearance.layout.barPosition = "bottom" }
        }
        BarMenu {
            id: rowHeightMenu
            host: bar.window
            objectName: "rowHeightMenu"
            title: qsTr("Row height")
            Entry { text: qsTr("Compact"); markStyle: "radio"; marked: appearance.layout.rowHeight === "compact"
                    onTriggered: appearance.layout.rowHeight = "compact" }
            Entry { text: qsTr("Comfortable"); markStyle: "radio"; marked: appearance.layout.rowHeight === "comfortable"
                    onTriggered: appearance.layout.rowHeight = "comfortable" }
            Entry { text: qsTr("Large"); markStyle: "radio"; marked: appearance.layout.rowHeight === "large"
                    onTriggered: appearance.layout.rowHeight = "large" }
        }
        Entry { text: qsTr("Lyrics"); command: "lyrics" }
        Entry { text: qsTr("Time left instead of time played"); command: "time"; keys: "T"
                markStyle: "check"; marked: bar.window.showRemaining }
        Line {}
        BarMenu {
            id: deckStyleMenu
            host: bar.window
            objectName: "deckStyleMenu"
            title: qsTr("Deck style")
            Instantiator {
                model: appearance.deckStyles
                delegate: Entry {
                    required property string modelData
                    objectName: "menuDeckStyle_" + modelData
                    text: appearance.deckStyleName(modelData)
                    command: "deck-style:" + modelData
                    markStyle: "radio"; marked: appearance.deckStyle === modelData
                }
                onObjectAdded: (index, object) => deckStyleMenu.insertItem(index, object)
                onObjectRemoved: (index, object) => deckStyleMenu.removeItem(object)
            }
        }
        Entry { objectName: "menuChooseVisualiser"; text: qsTr("Choose visualiser…"); command: "choose-visualiser" }
        BarMenu {
            id: visualiserMenu
            host: bar.window
            objectName: "visualiserMenu"
            title: qsTr("Visualiser")
            Instantiator {
                model: appearance.visualisers
                delegate: Entry {
                    required property string modelData
                    objectName: "menuVisualiser_" + modelData
                    text: appearance.visualiserName(modelData)
                    markStyle: "radio"; marked: appearance.visualiser === modelData
                    onTriggered: appearance.visualiser = modelData
                }
                onObjectAdded: (index, object) => visualiserMenu.insertItem(index, object)
                onObjectRemoved: (index, object) => visualiserMenu.removeItem(object)
            }
            Line {}
            Entry { text: qsTr("Next visualiser"); command: "next-visualiser"; keys: "Shift+V" }
        }
        Entry { text: qsTr("Appearance settings…"); command: "settings-appearance" }
        Line {}
        Entry { objectName: "menuShowMenuBar"; text: qsTr("Show the menu bar"); command: "toggle-menu-bar"; keys: "F10"
                markStyle: "check"; marked: bar.prefs.menuBar }
    }

    // ---- Play ---------------------------------------------------------------
    BarMenu {
        id: playMenu
        host: bar.window
        objectName: "playMenu"
        title: qsTr("&Play")
        Entry { objectName: "menuPlayPause"; text: backend.state === "playing" ? qsTr("Pause") : qsTr("Play")
                command: "play-pause"; keys: "Space" }
        Entry { text: qsTr("Stop"); command: "stop"; keys: "V" }
        Entry { text: qsTr("Previous track"); command: "previous"; keys: "Z" }
        Entry { text: qsTr("Next track"); command: "next"; keys: "B" }
        Line {}
        Entry { text: qsTr("Back 5 seconds"); command: "seek-back"; keys: bar.window.advanced ? "" : "←" }
        Entry { text: qsTr("Forward 5 seconds"); command: "seek-forward"; keys: bar.window.advanced ? "" : "→" }
        Line {}
        Entry { objectName: "menuShuffle"; text: qsTr("Shuffle"); command: "shuffle"; keys: "S"
                markStyle: "check"; marked: backend.shuffle }
        Entry { objectName: "menuRepeat"; text: qsTr("Repeat"); command: "repeat"; keys: "R"
                markStyle: "check"; marked: backend.repeat }
        Line {}
        Entry { text: qsTr("Volume up"); command: "volume-up"; keys: "+" }
        Entry { text: qsTr("Volume down"); command: "volume-down"; keys: "−" }
        Line {}
        Entry { objectName: "menuAutoDj"; text: qsTr("Auto-DJ"); command: "autodj"; keys: "Ctrl+J"
                markStyle: "check"; marked: library.dj.enabled }
        Entry { text: qsTr("Auto-DJ settings…"); command: "autodj-settings" }
        Entry { objectName: "menuParty"; text: qsTr("Party mode…"); command: "party"
                markStyle: "check"; marked: library.dj.partyOn }
        Line {}
        Entry { text: qsTr("Equaliser, crossfade and ReplayGain…"); command: "settings-playback" }
        Entry { text: qsTr("Scrobbling…"); command: "settings-scrobbling" }
    }

    // ---- Library ------------------------------------------------------------
    BarMenu {
        id: libraryMenu
        host: bar.window
        objectName: "libraryMenu"
        title: qsTr("&Library")
        Entry { text: qsTr("Rescan the library"); command: "rescan"; keys: "F5" }
        Entry { text: qsTr("Cancel the scan"); command: "cancel-job" }
        Line {}
        Entry { text: qsTr("Organise files…"); command: "organise"; keys: "Ctrl+Shift+O" }
        Entry { text: qsTr("Tags from file names…"); command: "tag-from-filename" }
        Entry { text: qsTr("Identify with MusicBrainz…"); command: "identify"; keys: "Ctrl+I" }
        Entry { text: qsTr("Find a cover for the chosen album…"); command: "find-cover" }
        Entry { text: qsTr("Find missing covers"); command: "find-missing-covers" }
        Entry { text: qsTr("Find duplicates"); command: "find-duplicates" }
        BarMenu {
            id: auditMenu
            host: bar.window
            objectName: "auditMenu"
            title: qsTr("Files to Edit")
            Instantiator {
                model: library.auditList()
                delegate: Entry {
                    required property var modelData
                    text: modelData.title
                    command: "audit:" + modelData.id
                }
                onObjectAdded: (index, object) => auditMenu.insertItem(index, object)
                onObjectRemoved: (index, object) => auditMenu.removeItem(object)
            }
        }
        Line {}
        Entry { text: qsTr("Volumes, NAS shares and servers…"); command: "volumes" }
        Entry { text: qsTr("Connect a NAS…"); command: "connect-nas" }
        Entry { text: qsTr("Add Navidrome / Subsonic server…"); command: "add-server" }
        Entry { text: qsTr("Sync Navidrome / Subsonic servers"); command: "sync-servers" }
        Entry { text: qsTr("Devices and sync…"); command: "devices" }
        Line {}
        BarMenu {
            host: bar.window
            objectName: "reportsMenu"
            title: qsTr("Reports")
            Entry { text: qsTr("Save this list as CSV…"); command: "export-csv" }
            Entry { text: qsTr("Save this list as a web page (HTML)…"); command: "export-html" }
        }
        Line {}
        Entry { text: qsTr("New playlist…"); command: "new-playlist" }
        Entry { text: qsTr("New auto-playlist…"); command: "new-autoplaylist" }
        Entry { text: qsTr("New playlist folder…"); command: "new-playlist-folder" }
        Entry { text: qsTr("New collection…"); command: "new-collection" }
        Line {}
        Entry { text: qsTr("Go to Entire Library"); command: "entire" }
        Entry { text: qsTr("Go to Now Playing"); command: "now-playing" }
        Line {}
        Entry { text: qsTr("Web services: AcoustID, MusicBrainz, covers…"); command: "settings-web" }
    }

    // ---- Radio --------------------------------------------------------------
    BarMenu {
        id: radioMenu
        host: bar.window
        objectName: "radioMenu"
        title: qsTr("&Radio")
        onAboutToShow: bar.stations = radio.allStations().map(s => Object.assign({ favourite: radio.isFavourite(s.key) }, s))
        Entry { text: qsTr("Choose a channel…"); command: "radio"; keys: "Shift+R" }
        Line {}
        BarMenu {
            id: nightrideMenu
            host: bar.window
            objectName: "radioMenu_nightride"
            title: qsTr("Nightride FM")
            Instantiator {
                model: bar.stations.filter(s => s.network === "nightride")
                delegate: Entry {
                    required property var modelData
                    objectName: "menuStation_" + modelData.key
                    text: modelData.fullName.replace(/^Nightride FM: /, "")
                    command: "radio:" + modelData.key
                    markStyle: "radio"; marked: radio.active && radio.currentKey === modelData.key
                }
                onObjectAdded: (index, object) => nightrideMenu.insertItem(index, object)
                onObjectRemoved: (index, object) => nightrideMenu.removeItem(object)
            }
        }
        BarMenu {
            id: rektMenu
            host: bar.window
            objectName: "radioMenu_rekt"
            title: qsTr("Rekt Network")
            Instantiator {
                model: bar.stations.filter(s => s.network === "rekt")
                delegate: Entry {
                    required property var modelData
                    objectName: "menuStation_" + modelData.key
                    text: modelData.fullName.replace(/^Rekt Network: /, "")
                    command: "radio:" + modelData.key
                    markStyle: "radio"; marked: radio.active && radio.currentKey === modelData.key
                }
                onObjectAdded: (index, object) => rektMenu.insertItem(index, object)
                onObjectRemoved: (index, object) => rektMenu.removeItem(object)
            }
        }
        BarMenu {
            id: mineMenu
            host: bar.window
            objectName: "radioMenu_mine"
            title: qsTr("My stations")
            enabled: bar.stations.some(s => s.network !== "nightride" && s.network !== "rekt")
            Instantiator {
                model: bar.stations.filter(s => s.network !== "nightride" && s.network !== "rekt")
                delegate: Entry {
                    required property var modelData
                    text: modelData.fullName
                    command: "radio:" + modelData.key
                    markStyle: "radio"; marked: radio.active && radio.currentKey === modelData.key
                }
                onObjectAdded: (index, object) => mineMenu.insertItem(index, object)
                onObjectRemoved: (index, object) => mineMenu.removeItem(object)
            }
        }
        BarMenu {
            id: favouritesMenu
            host: bar.window
            objectName: "radioMenu_favourites"
            title: qsTr("Favourites")
            enabled: bar.stations.some(s => s.favourite)
            Instantiator {
                model: bar.stations.filter(s => s.favourite)
                delegate: Entry {
                    required property var modelData
                    text: modelData.fullName
                    command: "radio:" + modelData.key
                    markStyle: "radio"; marked: radio.active && radio.currentKey === modelData.key
                }
                onObjectAdded: (index, object) => favouritesMenu.insertItem(index, object)
                onObjectRemoved: (index, object) => favouritesMenu.removeItem(object)
            }
        }
        Line {}
        Entry { text: qsTr("Previous channel"); command: "radio-previous"; keys: "[" }
        Entry { text: qsTr("Next channel"); command: "radio-next"; keys: "]" }
        Entry { objectName: "menuRadioFavourite"; text: qsTr("Favourite this station"); command: "radio-favourite"
                markStyle: "check"; marked: radio.active && radio.isFavourite(radio.currentKey) }
        Line {}
        Entry { text: qsTr("Find stations…"); command: "radio-find" }
        Entry { text: qsTr("Add a station by address…"); command: "radio-add" }
    }

    // ---- Spotify ------------------------------------------------------------
    BarMenu {
        id: spotifyMenu
        host: bar.window
        objectName: "spotifyMenu"
        title: qsTr("&Spotify")
        Entry { objectName: "menuSpotifyOn"; text: qsTr("Turn on Spotify…"); command: "settings-spotify"; shown: !spotify.enabled }
        Entry { objectName: "menuSpotifySearch"; text: qsTr("Search Spotify…"); command: "spotify-search"; shown: spotify.enabled }
        Line { shown: spotify.enabled }
        Entry { objectName: "menuSpotifyPlaylists"; text: qsTr("Your playlists"); command: "spotify-section:playlists"; shown: spotify.enabled }
        Entry { text: qsTr("Liked Songs"); command: "spotify-section:liked"; shown: spotify.enabled }
        Entry { text: qsTr("Saved albums"); command: "spotify-section:albums"; shown: spotify.enabled }
        Entry { text: qsTr("Followed artists"); command: "spotify-section:artists"; shown: spotify.enabled }
        Line { shown: spotify.enabled }
        Entry { objectName: "menuSpotifyLike"; text: qsTr("Like the playing Spotify track"); command: "spotify-like"; shown: spotify.enabled
                markStyle: "check"; marked: spotify.nowPlaying.liked === true }
        Line { shown: spotify.enabled }
        Entry { objectName: "menuSpotifySettings"; text: qsTr("Spotify settings…"); command: "settings-spotify"; shown: spotify.enabled }
    }

    // ---- Help ---------------------------------------------------------------
    BarMenu {
        id: helpMenu
        host: bar.window
        objectName: "helpMenu"
        title: qsTr("&Help")
        Entry { text: qsTr("Keyboard shortcuts"); command: "shortcuts"; keys: "?" }
        Entry { text: qsTr("Command palette…"); command: "palette"; keys: "Ctrl+K" }
        Entry { objectName: "menuRunSetup"; text: qsTr("Run setup again…"); command: "setup" }
        Line {}
        Entry { text: qsTr("Thrum on GitHub"); command: "open-website" }
        Entry { text: qsTr("Importing from MediaMonkey (guide)"); command: "open-mediamonkey-guide" }
        Line {}
        Entry { objectName: "menuAbout"; text: qsTr("About Thrum"); command: "about" }
    }
}
