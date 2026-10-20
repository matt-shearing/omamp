import QtCore
import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

ApplicationWindow {
    id: win
    // Now playing: the mini and micro players and the big view (NowPlayingModes.qml).
    width: modes.compact !== "" ? modes.compactWidth : prefs.advanced ? prefs.advancedWidth : prefs.deckWidth
    height: modes.compact !== "" ? modes.compactHeight : prefs.advanced ? prefs.advancedHeight : prefs.deckHeight
    minimumWidth: modes.compact !== "" ? modes.minimumWidth : advanced ? 820 : 440
    minimumHeight: modes.compact !== "" ? modes.minimumHeight : advanced ? 520 : 460
    visible: true
    title: modes.compact !== "" ? modes.title
           : backend.currentIndex >= 0 && backend.title
           ? (backend.artist ? backend.artist + " - " : "") + backend.title + " — Omamp" : "Omamp"
    color: advanced && modes.compact === "" ? theme.c.background : appearance.deck.window

    Material.theme: theme.light ? Material.Light : Material.Dark
    Material.accent: theme.accent
    Material.roundedScale: theme.radius

    readonly property string mono: "JetBrainsMono Nerd Font"
    property bool helpVisible: false
    property bool showRemaining: false
    property int selected: backend.currentIndex
    // Tab switches between the deck and advanced mode (the library).
    readonly property bool advanced: prefs.advanced
    // A sheet (tag editor, organise...) is open: the window's own keys are
    // off, so letters typed or pressed there never play, skip or quit.
    readonly property bool sheetOpen: tagSheet.opened || organiseSheet.opened || tagFromNameSheet.opened
                                      || historySheet.opened || confirmSheet.opened || nameSheet.opened
                                      || autoSheet.opened || collectionSheet.opened || djSheet.opened
                                      || partySheet.opened || settingsSheet.opened
                                      || identifySheet.opened || coverChooser.opened || volumesSheet.opened
                                      || nasGuideSheet.opened || remoteSheet.opened || devicesSheet.opened
                                      || radioUi.sheetOpen
    // Party mode: what guests may do (PartyMode's action names).
    function may(action) { void library.dj.partyOn; return library.dj.allowed(action) }
    // Asks first; says so when party mode locks it.
    function guard(action) {
        if (library.dj.allowed(action)) return true
        toast.show(qsTr("Locked in party mode"))
        return false
    }

    // The mode, each mode's window size, and the library's layout, kept
    // between runs in ~/.config/omamp/window.conf.
    Settings {
        id: prefs
        location: settingsUrl
        category: "window"
        property bool advanced: false
        property int deckWidth: 500
        property int deckHeight: 680
        property int advancedWidth: 1360
        property int advancedHeight: 820
        property int treeWidth: 250
        property string columns: ""
        // Advanced mode's optional parts.
        property bool browserVisible: false
        property string browserArtistMode: "albumartist"
        property int browserHeight: 214
        property bool infoVisible: false
        property int infoWidth: 290
        property bool gridVisible: false
    }

    function rememberLayout() {
        if (modes.compact !== "") { modes.rememberSize(); return }
        if (advanced) {
            prefs.advancedWidth = width
            prefs.advancedHeight = height
            if (advancedLoader.item) {
                prefs.columns = advancedLoader.item.columnState()
                prefs.treeWidth = advancedLoader.item.currentTreeWidth()
                if (prefs.browserVisible) prefs.browserHeight = advancedLoader.item.currentBrowserHeight()
                if (prefs.infoVisible) prefs.infoWidth = advancedLoader.item.currentInfoWidth()
            }
        } else {
            prefs.deckWidth = width
            prefs.deckHeight = height
        }
    }

    function setAdvanced(on) {
        if (modes.compact !== "") modes.setCompact("")
        if (on === advanced) return
        rememberLayout()
        prefs.advanced = on
        width = on ? prefs.advancedWidth : prefs.deckWidth
        height = on ? prefs.advancedHeight : prefs.deckHeight
        if (on && advancedLoader.item) advancedLoader.item.focusList()
    }

    // The column browser, the info panel and the album grid: each switched
    // by a key and a button, and remembered.
    function toggleBrowser() {
        if (!advanced) setAdvanced(true)
        if (prefs.browserVisible && advancedLoader.item) prefs.browserHeight = advancedLoader.item.currentBrowserHeight()
        prefs.browserVisible = !prefs.browserVisible
        if (advancedLoader.item) {
            if (prefs.browserVisible) advancedLoader.item.focusBrowser()
            else advancedLoader.item.focusList()
        }
    }
    function toggleInfo() {
        if (!advanced) setAdvanced(true)
        if (prefs.infoVisible && advancedLoader.item) prefs.infoWidth = advancedLoader.item.currentInfoWidth()
        prefs.infoVisible = !prefs.infoVisible
    }
    function toggleGrid() {
        if (!advanced) setAdvanced(true)
        prefs.gridVisible = !prefs.gridVisible
        if (advancedLoader.item) advancedLoader.item.focusList()
    }

    // The browser counts and filters only while it shows; the grid reads
    // its albums only while it shows.
    Binding { target: library.browser; property: "enabled"; value: win.advanced && prefs.browserVisible }
    Binding { target: library.browser; property: "artistMode"; value: prefs.browserArtistMode }
    Binding { target: library.albumGrid; property: "active"; value: win.advanced && prefs.gridVisible && library.currentKind !== "nowplaying" }
    Connections {
        target: library.browser
        function onArtistModeChanged() { prefs.browserArtistMode = library.browser.artistMode }
    }

    onClosing: (close) => {
        if (!library.dj.allowed("quit")) {
            close.accepted = false
            partySheet.openSheet("quit")
            return
        }
        rememberLayout()
    }
    Component.onCompleted: {
        if (advanced) library.activate()
        registerActions()
        modes.registerActions(commandPalette)
        radioUi.registerActions(commandPalette)
        modes.start(typeof startMode === "string" ? startMode : "")
    }

    // Now playing: the big view, fullscreen, and the mini and micro players.
    NowPlayingModes { id: modes; window: win; prefs: prefs }
    onAdvancedChanged: if (advanced) library.activate()

    // ---- The command palette (Ctrl+K) ------------------------------------

    function registerActions() {
        const p = commandPalette
        p.addAction("play-pause", qsTr("Play/Pause"), ["pause", "resume"], "Space")
        p.addAction("play", qsTr("Play"), ["start"], "X")
        p.addAction("stop", qsTr("Stop"), [], "V")
        p.addAction("next", qsTr("Next track"), ["skip", "forward"], "B")
        p.addAction("previous", qsTr("Previous track"), ["back"], "Z")
        p.addAction("shuffle", qsTr("Shuffle on or off"), ["random"], "S")
        p.addAction("repeat", qsTr("Repeat on or off"), ["loop"], "R")
        p.addAction("volume-up", qsTr("Volume up"), ["louder"], "+")
        p.addAction("volume-down", qsTr("Volume down"), ["quieter"], "−")
        p.addAction("time", qsTr("Show time left or time played"), ["remaining", "elapsed", "clock"], "T")
        p.addAction("advanced", qsTr("Switch between the deck and the library"), ["advanced", "library", "deck", "mode"], "Tab")
        p.addAction("search", qsTr("Search the library"), ["find", "filter"], "/")
        p.addAction("browser", qsTr("Show or hide the column browser"), ["genre", "facets", "filters", "columns"], "Ctrl+B")
        p.addAction("grid", qsTr("Show albums as a grid of covers"), ["album", "art", "covers", "grid"], "G")
        p.addAction("info", qsTr("Show or hide the info panel"), ["details", "properties", "tags"], "I")
        p.addAction("add-folder", qsTr("Add a folder to the library…"), ["open", "import", "scan"], "O")
        p.addAction("rescan", qsTr("Rescan the library"), ["scan", "refresh", "update"], "F5")
        p.addAction("cancel-job", qsTr("Cancel the running scan"), ["stop", "abort"], "")
        p.addAction("entire", qsTr("Go to Entire Library"), ["all", "tracks"], "")
        p.addAction("now-playing", qsTr("Go to Now Playing"), ["queue", "playlist"], "")
        p.addAction("play-selection", qsTr("Play the selected tracks"), ["enter"], "Enter")
        p.addAction("queue-selection", qsTr("Play the selected tracks next"), ["queue", "enqueue"], "")
        p.addAction("append-selection", qsTr("Add the selected tracks to Now Playing"), ["enqueue", "append"], "")
        p.addAction("select-all", qsTr("Select every track in the list"), [], "Ctrl+A")
        p.addAction("show-folder", qsTr("Show the track in its folder"), ["file manager", "reveal"], "")
        p.addAction("clear-browser", qsTr("Clear the column browser"), ["reset", "all"], "")
        p.addAction("prefetch-art", qsTr("Find covers for every album"), ["artwork", "pictures"], "")
        p.addAction("edit-tags", qsTr("Edit tags of the selected tracks…"), ["properties", "tag editor", "metadata", "artwork", "lyrics"], "Ctrl+E")
        p.addAction("edit-title", qsTr("Rename the track (edit its title)"), ["title", "rename"], "F2")
        p.addAction("organise", qsTr("Organise files: rename and move by a mask…"), ["organize", "rename", "move", "auto-organize", "mask"], "Ctrl+Shift+O")
        p.addAction("tag-from-filename", qsTr("Tag from file names…"), ["filename", "pattern", "auto-tag"], "")
        p.addAction("undo", qsTr("Undo the last tag edit or organise"), ["revert", "back"], "Ctrl+Z")
        p.addAction("redo", qsTr("Redo what was undone"), ["again"], "Ctrl+Shift+Z")
        p.addAction("undo-history", qsTr("Undo history"), ["journal", "changes", "log"], "")
        p.addAction("find-duplicates", qsTr("Find duplicates"), ["duplicate", "same", "copies", "dupes", "files to edit"], "")
        for (const a of library.auditList())
            p.addAction("audit:" + a.id, qsTr("Files to Edit: %1").arg(a.title), ["files to edit", "audit", "fix", "check"], "")
        // Phase 3: playlists, collections, Auto-DJ, party mode, settings.
        p.addAction("new-playlist", qsTr("New playlist…"), ["create", "list", "static"], "")
        p.addAction("new-autoplaylist", qsTr("New auto-playlist…"), ["smart", "query", "rules", "create"], "")
        p.addAction("new-playlist-folder", qsTr("New playlist folder…"), ["folder", "group"], "")
        p.addAction("import-playlists", qsTr("Import playlists (M3U, PLS)…"), ["m3u", "m3u8", "pls", "open"], "")
        p.addAction("export-playlist", qsTr("Export the chosen playlist to M3U…"), ["m3u", "m3u8", "save"], "")
        p.addAction("save-now-playing", qsTr("Save Now Playing as a playlist…"), ["queue", "save", "playlist"], "")
        p.addAction("play-playlist", qsTr("Play the chosen playlist"), ["start"], "")
        p.addAction("rename-playlist", qsTr("Rename the chosen playlist…"), ["name"], "")
        p.addAction("delete-playlist", qsTr("Delete the chosen playlist…"), ["remove"], "")
        p.addAction("edit-autoplaylist", qsTr("Edit the chosen auto-playlist…"), ["query", "rules", "smart"], "")
        p.addAction("new-collection", qsTr("New collection…"), ["collections", "view"], "")
        p.addAction("edit-collection", qsTr("Edit the chosen collection…"), ["collections", "columns"], "")
        p.addAction("autodj", qsTr("Auto-DJ on or off"), ["dj", "auto", "radio", "endless"], "Ctrl+J")
        p.addAction("autodj-settings", qsTr("Auto-DJ settings…"), ["dj", "source", "separation"], "")
        p.addAction("party", qsTr("Party mode…"), ["guests", "lock", "kiosk", "pin"], "")
        p.addAction("settings", qsTr("Settings…"), ["preferences", "options", "config"], "Ctrl+,")
        p.addAction("settings-playback", qsTr("Equaliser, crossfade and ReplayGain…"), ["eq", "equalizer", "crossfade", "replaygain", "gapless", "engine"], "")
        p.addAction("settings-scrobbling", qsTr("Scrobbling: ListenBrainz and Last.fm…"), ["scrobble", "listenbrainz", "lastfm"], "")
        // Phases 4 and 5: the web, and the NAS.
        p.addAction("identify", qsTr("Identify with MusicBrainz…"), ["picard", "tag", "lookup", "musicbrainz", "acoustid", "fingerprint", "match"], "Ctrl+I")
        p.addAction("find-cover", qsTr("Find a cover for the chosen album…"), ["artwork", "picture", "cover art archive"], "")
        p.addAction("find-missing-covers", qsTr("Find missing covers"), ["artwork", "pictures", "cover art archive", "fetch"], "")
        p.addAction("lyrics", qsTr("Show lyrics"), ["words", "lrc", "lrclib", "karaoke"], "")
        p.addAction("volumes", qsTr("Volumes, NAS shares and servers…"), ["drives", "disks", "nas", "smb", "nfs", "offline", "reassign", "location"], "")
        p.addAction("connect-nas", qsTr("Connect a NAS…"), ["mount", "smb", "nfs", "share", "samba", "network"], "")
        p.addAction("add-server", qsTr("Add Navidrome / Subsonic server…"), ["navidrome", "subsonic", "opensubsonic", "remote", "stream"], "")
        p.addAction("sync-servers", qsTr("Sync Navidrome / Subsonic servers"), ["navidrome", "subsonic", "refresh", "update"], "")
        p.addAction("settings-web", qsTr("Web services: AcoustID, MusicBrainz, covers…"), ["acoustid", "musicbrainz", "key", "cover size"], "")
        // Phase 6: device sync and reports.
        p.addAction("devices", qsTr("Devices: sync to a phone, USB drive or Syncthing…"), ["sync", "phone", "usb", "syncthing", "transcode", "convert", "copy", "device"], "")
        p.addAction("sync-device", qsTr("Sync the chosen device now"), ["sync", "phone", "usb", "device"], "")
        p.addAction("export-csv", qsTr("Save this list as CSV…"), ["report", "export", "spreadsheet", "csv", "excel"], "")
        p.addAction("export-html", qsTr("Save this list as a web page (HTML)…"), ["report", "export", "html", "print", "web page"], "")
        p.addAction("shortcuts", qsTr("Show keyboard shortcuts"), ["help", "keys"], "?")
        p.addAction("quit", qsTr("Quit"), ["exit", "close"], "Q")
    }

    // The tracks a command works on: the selection in the list, or the
    // chosen album's tracks in the grid.
    function chosenTrackIds() {
        if (!advanced || !library.ready) return []
        const view = advancedLoader.item
        if (view && view.playlistNode) return library.playlists.items.selectedTrackIds()
        if (view && view.special) return []
        if (prefs.gridVisible && library.albumGrid.currentAlbum > 0) return library.albumTrackIds(library.albumGrid.currentAlbum)
        return library.selectedTrackIds()
    }
    // The album a cover command works on: the grid's, or the cursor track's.
    function chosenAlbum() {
        if (!advanced || !library.ready) return 0
        if (prefs.gridVisible && library.albumGrid.currentAlbum > 0) return library.albumGrid.currentAlbum
        const row = library.tracks.currentRow
        if (row < 0) return 0
        const info = library.trackInfo(library.tracks.trackId(row))
        return info.albumId || 0
    }
    function withTracks(run) {
        const ids = chosenTrackIds()
        if (ids.length === 0) { toast.show(qsTr("Choose some tracks in the list first")); return }
        run(ids)
    }
    function backToList() {
        if (advanced && advancedLoader.item && !sheetOpen) advancedLoader.item.focusList()
    }
    function goToNode(keys) {
        setAdvanced(true)
        const row = library.tree.reveal(keys)
        if (row >= 0) library.selectNode(row)
        if (advancedLoader.item) advancedLoader.item.focusList()
    }

    function updateActions() {
        const p = commandPalette
        p.setActionEnabled("rescan", !library.busy)
        p.setActionEnabled("cancel-job", library.busy)
        const listed = advanced && library.ready && library.currentKind !== "nowplaying"
        for (const id of ["play-selection", "queue-selection", "append-selection", "select-all", "show-folder"])
            p.setActionEnabled(id, listed && !prefs.gridVisible && library.tracks.count > 0)
        p.setActionEnabled("clear-browser", prefs.browserVisible && library.browser.filtering)
        const view = advancedLoader.item
        const inPlaylist = advanced && library.ready && view && view.playlistNode && library.playlists.items.count > 0
        const chosen = (advanced && library.ready && !(view && view.special)
                       && (prefs.gridVisible ? library.albumGrid.currentAlbum > 0 : library.tracks.count > 0)) || inPlaylist
        const editing = may("edit-tags")
        for (const id of ["edit-tags", "organise", "tag-from-filename", "identify", "find-cover"])
            p.setActionEnabled(id, chosen && editing)
        p.setActionEnabled("find-missing-covers", editing)
        for (const id of ["volumes", "connect-nas", "add-server", "settings-web"])
            p.setActionEnabled(id, may("settings"))
        p.setActionEnabled("sync-servers", may("settings") && library.nas.servers.length > 0)
        p.setActionEnabled("devices", may("settings"))
        p.setActionEnabled("sync-device", may("settings") && library.currentKind === "device")
        const listing = advanced && library.ready && !(view && view.special)
        p.setActionEnabled("export-csv", listing)
        p.setActionEnabled("export-html", listing)
        // Playlists: what the chosen node allows.
        const kind = library.currentKind
        const isList = kind === "playlist" || kind === "autoplaylist"
        const lists = may("edit-playlists")
        for (const id of ["new-playlist", "new-autoplaylist", "new-playlist-folder", "import-playlists", "save-now-playing", "new-collection"])
            p.setActionEnabled(id, lists)
        p.setActionEnabled("export-playlist", isList)
        p.setActionEnabled("play-playlist", isList)
        p.setActionEnabled("rename-playlist", lists && (isList || kind === "plfolder"))
        p.setActionEnabled("delete-playlist", lists && (isList || kind === "plfolder"))
        p.setActionEnabled("edit-autoplaylist", lists && kind === "autoplaylist")
        p.setActionEnabled("edit-collection", lists && kind === "collection")
        for (const id of ["autodj", "autodj-settings", "settings", "settings-playback", "settings-scrobbling"])
            p.setActionEnabled(id, may("settings"))
        p.setActionEnabled("edit-title", chosen && !prefs.gridVisible && library.tracks.currentRow >= 0)
        // A job running meanwhile only makes these wait their turn.
        p.setActionEnabled("undo", library.undoText !== "" || library.busy)
        p.setActionEnabled("redo", library.redoText !== "" || library.busy)
        radioUi.updateActions(p)
    }

    // The tree node that is chosen, as the tree's menu hands it over.
    function chosenNode() {
        const row = library.currentNode
        return { row: row, kind: library.tree.kind(row), key: library.tree.key(row), id: library.tree.value(row),
                 label: library.currentLabel }
    }

    // A tree node's menu command (LibraryTreePane.nodeAction), or the same
    // from the palette on the chosen node.
    function nodeAction(id, node) {
        switch (id) {
        case "volumes": case "connect-nas": case "add-server": case "find-missing-covers":
            runAction(id)
            return
        case "export-csv": case "export-html":
            // The menu's node, which may not be the chosen one yet.
            if (node && node.row !== undefined && node.row !== library.currentNode) library.selectNode(node.row)
            runAction(id)
            return
        case "devices":
            if (guard("settings")) { setAdvanced(true); devicesSheet.openSheet(0) }
            return
        case "device-edit":
            if (guard("settings")) { setAdvanced(true); devicesSheet.openSheet(node.id) }
            return
        case "device-sync":
            if (guard("settings")) library.devices.syncProfile(node.id)
            return
        case "rescan-volume":
            if (guard("settings")) library.nas.rescanVolume(node.id, true)
            return
        }
        const pl = library.playlists
        const parent = node && node.kind === "plfolder" ? node.id : 0
        const playing = ["play", "queue", "append", "export"]
        if (playing.indexOf(id) < 0 && !guard("edit-playlists")) return
        switch (id) {
        case "new-playlist":
            nameSheet.ask(qsTr("New playlist"), qsTr("Name"), qsTr("New playlist"), qsTr("Create"), (name) => {
                const made = pl.createPlaylist(name, parent)
                if (made > 0) pl.show(made)
                return made > 0
            })
            break
        case "new-folder":
            nameSheet.ask(qsTr("New playlist folder"), qsTr("Name"), qsTr("New folder"), qsTr("Create"), (name) => {
                const made = pl.createFolder(name, parent)
                if (made > 0) pl.show(made)
                return made > 0
            })
            break
        case "new-autoplaylist": autoSheet.openFor(0, parent); break
        case "edit-autoplaylist": autoSheet.openFor(node.id, 0); break
        case "rename":
            nameSheet.ask(node.kind === "plfolder" ? qsTr("Rename the folder") : qsTr("Rename the playlist"), qsTr("Name"),
                          node.label, qsTr("Rename"), (name) => pl.rename(node.id, name))
            break
        case "delete":
            if (node.kind === "collection") {
                confirmSheet.ask(qsTr("Delete the collection?"),
                                 qsTr("“%1” leaves the tree. Its tracks stay in the library.").arg(node.label),
                                 qsTr("Delete"), () => pl.removeCollection(node.id))
            } else {
                confirmSheet.ask(node.kind === "plfolder" ? qsTr("Delete the folder?") : qsTr("Delete the playlist?"),
                                 node.kind === "plfolder"
                                 ? qsTr("“%1” and every playlist in it are deleted. The tracks stay in the library.").arg(node.label)
                                 : qsTr("“%1” is deleted. Its tracks stay in the library.").arg(node.label),
                                 qsTr("Delete"), () => pl.remove(node.id))
            }
            break
        case "play": pl.play(node.id, 0); break
        case "queue": pl.queueNext(node.id); break
        case "append": pl.append(node.id); break
        case "export": pl.exportPlaylist(node.id); break
        case "import": pl.importPlaylists(); break
        case "save-now-playing":
            nameSheet.ask(qsTr("Save Now Playing as a playlist"), qsTr("Name"),
                          qsTr("Now Playing %1").arg(new Date().toLocaleDateString(Qt.locale(), Locale.ShortFormat)),
                          qsTr("Save"), (name) => {
                const made = pl.saveNowPlaying(name)
                if (made > 0) pl.show(made)
                return made > 0
            })
            break
        case "new-collection": collectionSheet.openFor(0); break
        case "edit-collection": collectionSheet.openFor(node.id); break
        case "restore-collections": pl.restoreCollections(); break
        }
    }

    function runAction(id) {
        const view = advancedLoader.item
        // Party mode locks what guests may not do.
        const locks = { "edit-tags": "edit-tags", "edit-title": "edit-tags", "organise": "edit-tags",
                        "tag-from-filename": "edit-tags", "undo": "edit-tags", "redo": "edit-tags",
                        "undo-history": "edit-tags", "find-duplicates": "edit-tags",
                        "identify": "edit-tags", "find-cover": "edit-tags", "find-missing-covers": "edit-tags",
                        "volumes": "settings", "connect-nas": "settings", "add-server": "settings",
                        "sync-servers": "settings", "settings-web": "settings",
                        "devices": "settings", "sync-device": "settings",
                        "add-folder": "settings", "rescan": "settings", "prefetch-art": "settings",
                        "autodj": "settings", "autodj-settings": "settings", "settings": "settings",
                        "settings-playback": "settings", "settings-scrobbling": "settings",
                        "play-pause": "play", "play": "play", "stop": "play", "next": "skip", "previous": "skip",
                        "volume-up": "volume", "volume-down": "volume", "shuffle": "reorder", "repeat": "reorder",
                        "queue-selection": "add", "append-selection": "add", "play-selection": "play",
                        "advanced": "library", "search": "library", "settings-appearance": "settings" }
        if (locks[id] !== undefined && !guard(locks[id])) return
        if (modes.runAction(id)) return
        if (radioUi.runAction(id)) return
        switch (id) {
        case "play-pause": backend.toggle(); break
        case "play": backend.play(); break
        case "stop": backend.stop(); break
        case "next": backend.next(); break
        case "previous": backend.previous(); break
        case "shuffle": backend.shuffle = !backend.shuffle; break
        case "repeat": backend.repeat = !backend.repeat; break
        case "volume-up": backend.volume += 5; break
        case "volume-down": backend.volume -= 5; break
        case "time": showRemaining = !showRemaining; break
        case "advanced": setAdvanced(!advanced); break
        case "search": setAdvanced(true); if (advancedLoader.item) advancedLoader.item.focusSearch(); break
        case "browser": toggleBrowser(); break
        case "grid": toggleGrid(); break
        case "info": toggleInfo(); break
        case "add-folder": advanced ? library.addFolder() : backend.openFolder(); break
        case "rescan": setAdvanced(true); library.rescan(); break
        case "cancel-job": library.cancelJob(); break
        case "entire": setAdvanced(true); library.selectNode(0); break
        case "now-playing": {
            setAdvanced(true)
            const row = library.tree.reveal(["playlists", "nowplaying"])
            if (row >= 0) library.selectNode(row)
            break
        }
        case "play-selection": library.playSelection(); break
        case "queue-selection": library.queueSelection(); break
        case "append-selection": library.appendSelection(); break
        case "select-all": library.tracks.selectAll(); break
        case "show-folder": library.showInFolder(library.tracks.currentRow); break
        case "clear-browser": library.browser.clear(); break
        case "prefetch-art": setAdvanced(true); library.prefetchArtwork(); break
        case "edit-tags": withTracks((ids) => tagSheet.openFor(ids)); break
        case "edit-title":
            if (view && view.listShowing && library.tracks.currentRow >= 0) view.editTitle()
            else toast.show(qsTr("Put the cursor on a track in the list first"))
            break
        case "organise": withTracks((ids) => organiseSheet.openFor(ids)); break
        case "tag-from-filename": withTracks((ids) => tagFromNameSheet.openFor(ids)); break
        case "undo": library.undo(); break
        case "redo": library.redo(); break
        case "undo-history": setAdvanced(true); library.activate(); historySheet.open(); break
        case "find-duplicates":
            goToNode(["filestoedit", "duplicates"])
            // By artist and title is quick; the other ways wait for Find.
            if (!library.duplicates.searched && library.duplicates.by === "tags") library.duplicates.find()
            break
        case "new-playlist": setAdvanced(true); nodeAction("new-playlist", library.currentKind === "plfolder" ? chosenNode() : {}); break
        case "new-autoplaylist": setAdvanced(true); nodeAction("new-autoplaylist", library.currentKind === "plfolder" ? chosenNode() : {}); break
        case "new-playlist-folder": setAdvanced(true); nodeAction("new-folder", {}); break
        case "import-playlists": setAdvanced(true); nodeAction("import", {}); break
        case "save-now-playing": setAdvanced(true); nodeAction("save-now-playing", {}); break
        case "new-collection": setAdvanced(true); nodeAction("new-collection", {}); break
        case "export-playlist": nodeAction("export", chosenNode()); break
        case "play-playlist": nodeAction("play", chosenNode()); break
        case "rename-playlist": nodeAction("rename", chosenNode()); break
        case "delete-playlist": nodeAction("delete", chosenNode()); break
        case "edit-autoplaylist": nodeAction("edit-autoplaylist", chosenNode()); break
        case "edit-collection": nodeAction("edit-collection", chosenNode()); break
        case "new-playlist-from-selection": {
            const ids = chosenTrackIds()
            if (!guard("edit-playlists")) break
            nameSheet.ask(qsTr("New playlist"), qsTr("Name"), qsTr("New playlist"), qsTr("Create"), (name) => {
                const made = library.playlists.createPlaylist(name, 0)
                if (made > 0) library.playlists.addTracks(made, ids)
                return made > 0
            })
            break
        }
        case "autodj": library.dj.toggle(); break
        case "autodj-settings": djSheet.openSheet(); break
        case "party": partySheet.openSheet("leave"); break
        case "settings": settingsSheet.openPage(0); break
        case "settings-playback": settingsSheet.openPage(0); break
        case "settings-scrobbling": settingsSheet.openPage(2); break
        case "settings-appearance": settingsSheet.openPage(4); break
        case "identify": withTracks((ids) => identifySheet.openFor(ids)); break
        case "find-cover": {
            const album = chosenAlbum()
            if (album > 0) library.web.findCover(album)
            else toast.show(qsTr("Choose an album in the grid, or a track of it in the list, first"))
            break
        }
        case "find-missing-covers": setAdvanced(true); library.web.findMissingCovers(); break
        case "lyrics":
            setAdvanced(true)
            if (!prefs.infoVisible) toggleInfo()
            if (advancedLoader.item) advancedLoader.item.showInfoPage(1)
            break
        case "volumes": setAdvanced(true); volumesSheet.openSheet(); break
        case "devices": setAdvanced(true); devicesSheet.openSheet(library.currentKind === "device" ? library.tree.value(library.currentNode) : 0); break
        case "sync-device":
            if (library.currentKind === "device") library.devices.syncProfile(library.tree.value(library.currentNode))
            break
        case "export-csv": library.exportList("csv"); break
        case "export-html": library.exportList("html"); break
        case "connect-nas": setAdvanced(true); nasGuideSheet.openSheet(); break
        case "add-server": setAdvanced(true); remoteSheet.openSheet(); break
        case "sync-servers":
            for (const s of library.nas.servers) library.nas.syncServer(s.id, false)
            break
        case "settings-web": settingsSheet.openPage(3); break
        case "shortcuts": helpVisible = true; break
        case "quit":
            // Party mode: quitting takes the PIN.
            if (!library.dj.allowed("quit")) partySheet.openSheet("quit")
            else Qt.quit()
            break
        default:
            if (id.startsWith("audit:")) goToNode(["filestoedit", id])
            else if (id.startsWith("edit-lyrics:") && guard("edit-tags")) tagSheet.openFor([Number(id.slice(12))], 2)
        }
    }

    function showInList() {
        setAdvanced(true)
        if (prefs.gridVisible) prefs.gridVisible = false
        if (advancedLoader.item) advancedLoader.item.focusList()
    }

    Connections {
        target: commandPalette
        function onActionTriggered(id) { win.runAction(id) }
        function onArtistActivated(artistId, name) { win.showInList(); library.showArtist(artistId) }
        function onAlbumActivated(albumId, title, albumArtist) { win.showInList(); library.showAlbum(albumId) }
        function onTrackActivated(trackId) { win.showInList(); library.showTrack(trackId) }
        function onSearchRequested(text) {
            win.setAdvanced(true)
            if (advancedLoader.item) {
                advancedLoader.item.setSearchText(text)
                advancedLoader.item.focusList()
            }
        }
    }

    function clock(ms) {
        if (ms === undefined || ms < 0 || isNaN(ms)) ms = 0
        const total = Math.floor(ms / 1000)
        const h = Math.floor(total / 3600), m = Math.floor(total % 3600 / 60), s = total % 60
        const mm = (h > 0 && m < 10 ? "0" : "") + m
        return (h > 0 ? h + ":" : "") + (mm.length < 2 ? "0" + mm : mm) + ":" + (s < 10 ? "0" : "") + s
    }

    function select(row) {
        if (backend.playlist.count === 0) return
        selected = Math.max(0, Math.min(backend.playlist.count - 1, row))
        deckView.positionAt(selected)
    }

    // Winamp's own keys (Z X C V B) alongside the Omarchy ones.
    Shortcut { sequences: ["?", "Shift+/"]; context: Qt.ApplicationShortcut; enabled: !win.sheetOpen; onActivated: win.helpVisible = !win.helpVisible }
    Shortcut { sequence: "Escape"; context: Qt.ApplicationShortcut; enabled: !win.sheetOpen && (win.helpVisible); onActivated: win.helpVisible = false }
    Shortcut { sequence: "Q"; context: Qt.ApplicationShortcut; enabled: !win.sheetOpen; onActivated: win.runAction("quit") }
    Shortcut { sequence: "Space"; context: Qt.ApplicationShortcut; enabled: !win.sheetOpen && win.may("play"); onActivated: backend.toggle() }
    Shortcut { sequence: "Z"; context: Qt.ApplicationShortcut; enabled: !win.sheetOpen && win.may("skip"); onActivated: backend.previous() }
    Shortcut { sequence: "X"; context: Qt.ApplicationShortcut; enabled: !win.sheetOpen && win.may("play"); onActivated: backend.play() }
    Shortcut { sequence: "C"; context: Qt.ApplicationShortcut; enabled: !win.sheetOpen && win.may("play"); onActivated: backend.pause() }
    Shortcut { sequence: "V"; context: Qt.ApplicationShortcut; enabled: !win.sheetOpen && win.may("play"); onActivated: backend.stop() }
    Shortcut { sequence: "B"; context: Qt.ApplicationShortcut; enabled: !win.sheetOpen && win.may("skip"); onActivated: backend.next() }
    Shortcut { sequence: "Left"; context: Qt.ApplicationShortcut; enabled: !win.sheetOpen && (!win.advanced) && win.may("seek"); onActivated: backend.seekBy(-5000) }
    Shortcut { sequence: "Right"; context: Qt.ApplicationShortcut; enabled: !win.sheetOpen && (!win.advanced) && win.may("seek"); onActivated: backend.seekBy(5000) }
    Shortcut { sequence: "Up"; context: Qt.ApplicationShortcut; enabled: !win.sheetOpen && (!win.advanced); onActivated: win.select(win.selected - 1) }
    Shortcut { sequence: "Down"; context: Qt.ApplicationShortcut; enabled: !win.sheetOpen && (!win.advanced); onActivated: win.select(win.selected + 1) }
    Shortcut { sequence: "Return"; context: Qt.ApplicationShortcut; enabled: !win.sheetOpen && (!win.advanced); onActivated: backend.playIndex(win.selected) }
    Shortcut { sequence: "Delete"; context: Qt.ApplicationShortcut; enabled: !win.sheetOpen && (!win.advanced) && win.may("remove"); onActivated: { backend.remove(win.selected); win.select(win.selected) } }
    Shortcut { sequences: ["-", "Ctrl+Down"]; context: Qt.ApplicationShortcut; enabled: !win.sheetOpen && win.may("volume"); onActivated: backend.volume -= 5 }
    Shortcut { sequences: ["=", "+", "Ctrl+Up"]; context: Qt.ApplicationShortcut; enabled: !win.sheetOpen && win.may("volume"); onActivated: backend.volume += 5 }
    Shortcut { sequence: "S"; context: Qt.ApplicationShortcut; enabled: !win.sheetOpen && win.may("reorder"); onActivated: backend.shuffle = !backend.shuffle }
    Shortcut { sequence: "R"; context: Qt.ApplicationShortcut; enabled: !win.sheetOpen && win.may("reorder"); onActivated: backend.repeat = !backend.repeat }
    Shortcut { sequences: ["O", "L"]; context: Qt.ApplicationShortcut; enabled: !win.sheetOpen && win.may("settings")
               onActivated: win.advanced ? library.addFolder() : backend.openFolder() }
    Shortcut { sequence: "T"; context: Qt.ApplicationShortcut; enabled: !win.sheetOpen; onActivated: win.showRemaining = !win.showRemaining }
    // Advanced mode.
    Shortcut { sequences: ["Tab", "Backtab"]; context: Qt.ApplicationShortcut; enabled: !win.sheetOpen && win.may("library"); onActivated: win.setAdvanced(!win.advanced) }
    Shortcut { sequences: ["/", "Ctrl+F"]; context: Qt.ApplicationShortcut; enabled: !win.sheetOpen && (win.advanced)
               onActivated: advancedLoader.item.focusSearch() }
    Shortcut { sequence: "F5"; context: Qt.ApplicationShortcut; enabled: !win.sheetOpen && (win.advanced); onActivated: win.runAction("rescan") }
    // B stays Winamp's "next", so the column browser takes Ctrl+B.
    Shortcut { sequence: "Ctrl+B"; context: Qt.ApplicationShortcut; enabled: !win.sheetOpen && (win.advanced); onActivated: win.toggleBrowser() }
    Shortcut { sequence: "I"; context: Qt.ApplicationShortcut; enabled: !win.sheetOpen && (win.advanced); onActivated: win.toggleInfo() }
    Shortcut { sequence: "G"; context: Qt.ApplicationShortcut; enabled: !win.sheetOpen && (win.advanced); onActivated: win.toggleGrid() }
    Shortcut { sequence: "Ctrl+K"; context: Qt.ApplicationShortcut; enabled: !win.sheetOpen; onActivated: palette.toggle() }
    // Phase 2: tags, organise, undo.
    Shortcut { sequence: "Ctrl+E"; context: Qt.ApplicationShortcut; enabled: !win.sheetOpen && win.advanced; onActivated: win.runAction("edit-tags") }
    Shortcut { sequence: "F2"; context: Qt.ApplicationShortcut; enabled: !win.sheetOpen && win.advanced; onActivated: win.runAction("edit-title") }
    Shortcut { sequence: "Ctrl+Shift+O"; context: Qt.ApplicationShortcut; enabled: !win.sheetOpen && win.advanced; onActivated: win.runAction("organise") }
    Shortcut { sequences: [StandardKey.Undo]; context: Qt.ApplicationShortcut; enabled: !win.sheetOpen; onActivated: win.runAction("undo") }
    Shortcut { sequences: ["Ctrl+Shift+Z", "Ctrl+Y"]; context: Qt.ApplicationShortcut; enabled: !win.sheetOpen; onActivated: win.runAction("redo") }
    // Phase 3: Auto-DJ and Settings.
    Shortcut { sequence: "Ctrl+J"; context: Qt.ApplicationShortcut; enabled: !win.sheetOpen; onActivated: win.runAction("autodj") }
    Shortcut { sequences: ["Ctrl+,", StandardKey.Preferences]; context: Qt.ApplicationShortcut; enabled: !win.sheetOpen; onActivated: win.runAction("settings") }
    // Phase 4: I is the info panel, so identifying takes Ctrl+I.
    Shortcut { sequence: "Ctrl+I"; context: Qt.ApplicationShortcut; enabled: !win.sheetOpen && win.advanced; onActivated: win.runAction("identify") }
    // Now playing: N is free in both modes and V stays Winamp's stop, so
    // the visualiser takes Shift+V and the big view's lyrics Shift+L.
    Shortcut { sequence: "N"; context: Qt.ApplicationShortcut; enabled: !win.sheetOpen; onActivated: win.runAction("big-view") }
    Shortcut { sequence: "F11"; context: Qt.ApplicationShortcut; enabled: !win.sheetOpen; onActivated: win.runAction("fullscreen") }
    Shortcut { sequence: "Escape"; context: Qt.ApplicationShortcut; enabled: !win.sheetOpen && !win.helpVisible && (modes.bigView || modes.fullScreen); onActivated: modes.back() }
    Shortcut { sequence: "Shift+V"; context: Qt.ApplicationShortcut; enabled: !win.sheetOpen; onActivated: win.runAction("next-visualiser") }
    Shortcut { sequence: "Shift+L"; context: Qt.ApplicationShortcut; enabled: !win.sheetOpen && modes.bigView; onActivated: win.runAction("big-lyrics") }
    Shortcut { sequence: "Ctrl+M"; context: Qt.ApplicationShortcut; enabled: !win.sheetOpen; onActivated: win.runAction("mini-player") }
    Shortcut { sequence: "Ctrl+Shift+M"; context: Qt.ApplicationShortcut; enabled: !win.sheetOpen; onActivated: win.runAction("micro-player") }

    Item {
        anchors.fill: parent
        LayoutMirroring.enabled: Qt.application.layoutDirection === Qt.RightToLeft
        LayoutMirroring.childrenInherit: true

        // The deck (DeckView.qml), in the deck style from Settings > Appearance.
        DeckView {
            id: deckView
            anchors.fill: parent
            anchors.margins: 10
            visible: !win.advanced && modes.compact === ""
            showRemaining: win.showRemaining
            selected: win.selected
            dropHover: drop.containsDrag
            onToggleRemaining: win.showRemaining = !win.showRemaining
            onSelectRow: (row) => win.select(row)
            onOpenBigView: win.runAction("big-view")
            onOpenMini: win.runAction("mini-player")
            onOpenRadio: win.runAction("radio")
        }

        // Advanced mode, built the first time it is shown.
        Loader {
            id: advancedLoader
            anchors.fill: parent
            active: win.advanced || item !== null
            visible: win.advanced && modes.compact === ""
            sourceComponent: AdvancedView {
                objectName: "advancedView"
                showRemaining: win.showRemaining
                treeWidth: prefs.treeWidth
                columns: prefs.columns
                browserVisible: prefs.browserVisible
                browserHeight: prefs.browserHeight
                infoVisible: prefs.infoVisible
                infoWidth: prefs.infoWidth
                gridVisible: prefs.gridVisible
                onClockClicked: win.showRemaining = !win.showRemaining
                onToggleBrowser: win.toggleBrowser()
                onToggleInfo: win.toggleInfo()
                onToggleGrid: win.toggleGrid()
                onAction: (id) => win.runAction(id)
                onConfirm: (title, text, confirmText, action) => confirmSheet.ask(title, text, confirmText, action)
                onNodeAction: (id, node) => win.nodeAction(id, node)
                onAutoDjClicked: win.runAction("autodj")
                onPartyClicked: win.runAction("party")
            }
        }

        // The big Now Playing view (N), and the mini and micro players
        // (Ctrl+M, Ctrl+Shift+M), each built when first shown.
        Loader {
            anchors.fill: parent
            active: modes.bigView && !library.dj.partyDisplay
            sourceComponent: BigView {
                fullScreen: modes.fullScreen
                onClose: modes.setBigView(false)
                onOpenRadio: { modes.setBigView(false); win.runAction("radio") }
                onToggleFullScreen: modes.toggleFullScreen()
            }
        }
        Loader {
            anchors.fill: parent
            active: modes.compact !== ""
            sourceComponent: MiniPlayer {
                mode: modes.compact
                showRemaining: win.showRemaining
                onRestore: modes.setCompact("")
                onToggleRemaining: win.showRemaining = !win.showRemaining
                onOpenBigView: modes.setBigView(true)
            }
        }

        // Party mode's big now-playing display, over everything but the sheets.
        Loader {
            anchors.fill: parent
            active: library.dj.partyDisplay
            sourceComponent: PartyDisplay {
                fullScreen: modes.fullScreen
                onUnlock: partySheet.openSheet("leave")
            }
        }

        // Phase 2's sheets, over the library.
        // Closing one puts the keys back on the list.
        TagEditorSheet { id: tagSheet; onClosed: win.backToList() }
        OrganiseSheet { id: organiseSheet; onClosed: win.backToList() }
        TagFromNameSheet { id: tagFromNameSheet; onClosed: win.backToList() }
        UndoHistorySheet { id: historySheet; onClosed: win.backToList() }
        ConfirmSheet { id: confirmSheet; onClosed: win.backToList() }
        // Phase 3's sheets.
        NameSheet { id: nameSheet; onClosed: win.backToList() }
        AutoPlaylistSheet { id: autoSheet; onClosed: win.backToList() }
        CollectionSheet {
            id: collectionSheet
            onClosed: win.backToList()
            onConfirm: (title, text, confirmText, action) => confirmSheet.ask(title, text, confirmText, action)
        }
        AutoDjSheet { id: djSheet; onClosed: win.backToList() }
        PartySheet { id: partySheet; onClosed: win.backToList() }
        SettingsSheet {
            id: settingsSheet
            onClosed: win.backToList()
            onConfirm: (title, text, confirmText, action) => confirmSheet.ask(title, text, confirmText, action)
            onOpenVolumes: volumesSheet.openSheet()
        }
        // Phases 4 and 5.
        IdentifySheet {
            id: identifySheet
            onClosed: win.backToList()
            onOpenSettings: (page) => settingsSheet.openPage(page)
        }
        CoverChooserSheet { id: coverChooser; onClosed: win.backToList() }
        VolumesSheet {
            id: volumesSheet
            onClosed: win.backToList()
            onConfirm: (title, text, confirmText, action) => confirmSheet.ask(title, text, confirmText, action)
            onConnectNas: nasGuideSheet.openSheet()
            onAddServer: remoteSheet.openSheet()
        }
        // Phase 6.
        DevicesSheet {
            id: devicesSheet
            onClosed: win.backToList()
            onConfirm: (title, text, confirmText, action) => confirmSheet.ask(title, text, confirmText, action)
        }
        NasGuideSheet { id: nasGuideSheet; onClosed: if (!volumesSheet.opened) win.backToList() }
        RemoteSourceSheet { id: remoteSheet; onClosed: if (!volumesSheet.opened) win.backToList() }
        Connections {
            target: library.web
            function onCoverChanged() { if (library.web.coverState === "choose" && !coverChooser.opened) coverChooser.open() }
        }

        Toast {
            id: toast
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 48
        }
        SpotifyHooks { window: win; settings: settingsSheet; toast: toast }
        Connections {
            target: library
            function onToast(text) { toast.show(text) }
        }
        Connections {
            target: library.tagEditor
            function onMessage(text) { toast.show(text) }
        }
        Connections {
            target: library.organiser
            function onMessage(text) { toast.show(text) }
        }
        Connections {
            target: library.duplicates
            function onMessage(text) { toast.show(text) }
        }

        // Internet radio: its keys, palette actions and sheets (RadioActions.qml).
        RadioActions { id: radioUi; window: win }
        Connections {
            target: radio
            function onToast(text) { toast.show(text) }
        }

        // Ctrl+K: actions, artists, albums and tracks in one list.
        CommandPalette {
            id: palette
            objectName: "commandPalette"
            model: commandPalette
            onAboutToShow: win.updateActions()
            // Back to where the keys were.
            onClosed: if (win.advanced && !win.sheetOpen && advancedLoader.item && !advancedLoader.item.activeFocus) advancedLoader.item.focusList()
        }

        DropArea {
            id: drop
            anchors.fill: parent
            onDropped: (event) => {
                if (event.hasUrls) {
                    backend.addUrls(event.urls)
                    event.acceptProposedAction()
                }
            }
        }

        // Every shortcut, on ?.
        Rectangle {
            anchors.fill: parent
            visible: win.helpVisible
            color: Qt.alpha(theme.c.darker_background, 0.94)
            MouseArea { anchors.fill: parent; onClicked: win.helpVisible = false }

            // Two groups a row in advanced mode, so all four fit.
            Grid {
                anchors.centerIn: parent
                columns: win.advanced ? 2 : 1
                columnSpacing: 48
                rowSpacing: 22

                Repeater {
                    // The player's keys, and in advanced mode the library's beside them.
                    model: win.advanced ? [
                        [qsTr("Player"), [
                            ["Space", qsTr("Play or pause")],
                            ["Z  X  C  V  B", qsTr("Previous, play, pause, stop, next")],
                            ["−  +", qsTr("Volume down or up")],
                            ["S", qsTr("Shuffle on or off")],
                            ["R", qsTr("Repeat on or off")],
                            ["T", qsTr("Show time left or time played")],
                            ["N  F11", qsTr("Big Now Playing view, fullscreen")],
                            ["Ctrl+M", qsTr("Mini player (Ctrl+Shift+M: micro)")],
                            ["Shift+R  [  ]", qsTr("Radio channels; previous or next channel")],
                            ["Tab", qsTr("Back to the deck")],
                            ["?", qsTr("Show or hide these keys")],
                            ["Q", qsTr("Quit")]
                        ]],
                        [qsTr("Library"), [
                            ["/", qsTr("Search (Esc clears, Enter goes to the list)")],
                            ["↑ ↓", qsTr("Move through the tree or the list")],
                            ["→ ←", qsTr("Open or close a tree node")],
                            ["Enter  ←", qsTr("From the tree to the list, and back")],
                            ["Shift+↑ ↓", qsTr("Select a range")],
                            ["Ctrl+Space", qsTr("Select or unselect a track")],
                            ["Ctrl+A", qsTr("Select every track in the list")],
                            ["Enter", qsTr("Play the list from this track")],
                            ["Menu", qsTr("Play next, edit tags, organise, show in folder")],
                            ["O", qsTr("Add a folder to the library")],
                            ["F5", qsTr("Rescan the library")],
                            ["Ctrl+K", qsTr("Command palette")],
                            ["Ctrl+B", qsTr("Show or hide the column browser")],
                            ["G", qsTr("Album covers or the track list")],
                            ["I", qsTr("Show or hide the info panel")]
                        ]],
                        [qsTr("Tags and files"), [
                            ["Ctrl+E", qsTr("Edit tags of the selected tracks")],
                            ["F2", qsTr("Edit the title in place")],
                            ["Ctrl+Shift+O", qsTr("Organise files: rename and move")],
                            ["Ctrl+I", qsTr("Identify with MusicBrainz")],
                            ["Menu", qsTr("Tag from file names")],
                            ["Ctrl+Z", qsTr("Undo the last edit or organise")],
                            ["Ctrl+Shift+Z", qsTr("Redo")],
                            ["Ctrl+Enter", qsTr("Save, in an editor")],
                            ["Esc", qsTr("Close an editor")],
                            ["Ctrl+K", qsTr("Undo history, covers, lyrics, volumes")]
                        ]],
                        [qsTr("Playlists"), [
                            ["Menu", qsTr("On a tree node: new, rename, move, delete")],
                            ["Drag", qsTr("Tracks onto a playlist in the tree")],
                            ["Alt+↑ ↓", qsTr("Move entries in a playlist")],
                            ["Delete", qsTr("Remove entries from a playlist")],
                            ["Ctrl+J", qsTr("Auto-DJ on or off")],
                            ["Ctrl+,", qsTr("Settings: playback, EQ, library")],
                            ["Ctrl+K", qsTr("New playlist, import, party mode")]
                        ]]
                    ] : [
                        [qsTr("Keys"), [
                            ["Space", qsTr("Play or pause")],
                            ["Z  X  C  V  B", qsTr("Previous, play, pause, stop, next")],
                            ["← →", qsTr("Back or forward 5 seconds")],
                            ["↑ ↓", qsTr("Move through the playlist")],
                            ["Enter", qsTr("Play the selected track")],
                            ["Delete", qsTr("Remove the selected track")],
                            ["−  +", qsTr("Volume down or up")],
                            ["S", qsTr("Shuffle on or off")],
                            ["R", qsTr("Repeat on or off")],
                            ["O", qsTr("Add a music folder")],
                            ["T", qsTr("Show time left or time played")],
                            ["N  F11", qsTr("Big Now Playing view, fullscreen")],
                            ["Shift+V", qsTr("Next visualiser style")],
                            ["Ctrl+M", qsTr("Mini player (Ctrl+Shift+M: micro)")],
                            ["Shift+R", qsTr("Radio: choose a channel")],
                            ["[  ]", qsTr("Previous or next radio channel")],
                            ["Tab", qsTr("Open the library (advanced mode)")],
                            ["Ctrl+K", qsTr("Command palette")],
                            ["Ctrl+J", qsTr("Auto-DJ on or off")],
                            ["Ctrl+,", qsTr("Settings")],
                            ["?", qsTr("Show or hide these keys")],
                            ["Q", qsTr("Quit")]
                        ]]
                    ]

                    Column {
                        required property var modelData
                        spacing: 6
                        Text {
                            text: modelData[0]
                            font.family: win.mono; font.pixelSize: 16; font.bold: true
                            color: theme.accent
                            bottomPadding: 6
                        }
                        Repeater {
                            model: modelData[1]
                            Row {
                                required property var modelData
                                spacing: 16
                                Text {
                                    width: 110
                                    text: modelData[0]
                                    font.family: win.mono; font.pixelSize: 12; font.bold: true
                                    color: theme.c.green
                                }
                                Text {
                                    text: modelData[1]
                                    font.family: win.mono; font.pixelSize: 12
                                    color: theme.c.foreground
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
