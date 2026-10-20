import QtQuick

// The radio's keys, palette actions and sheets, kept out of Main.qml as
// NowPlayingModes keeps the now-playing ones.
//
//   Shift+R      the channel picker (Radio sheet over the deck; the Radio
//                node in advanced mode)
//   [  ]         previous or next channel, while a station plays
//   Ctrl+K       "Nightride FM: Darksynth" and every other station by name
Item {
    id: ui
    property var window: null
    readonly property bool sheetOpen: radioSheet.opened || findSheet.opened || addSheet.opened
    // The palette ids of the stations, to replace when the list changes.
    property var stationIds: []

    function may(action) { return window ? window.may(action) : true }

    function registerActions(p) {
        p.addAction("radio", qsTr("Radio: choose a channel…"), ["nightride", "station", "stream", "internet radio", "net radio", "channels"], "Shift+R")
        p.addAction("radio-find", qsTr("Find radio stations…"), ["radio-browser", "search", "station", "internet radio"], "")
        p.addAction("radio-add", qsTr("Add a radio station by address…"), ["stream", "url", "pls", "m3u", "icecast", "shoutcast"], "")
        p.addAction("radio-next", qsTr("Next radio channel"), ["station", "channel", "nightride"], "]")
        p.addAction("radio-previous", qsTr("Previous radio channel"), ["station", "channel", "nightride"], "[")
        p.addAction("radio-favourite", qsTr("Favourite the radio station"), ["star", "keep", "station"], "")
        p.addAction("radio-import", qsTr("Import radio stations (PLS, M3U)…"), ["pls", "m3u", "stations", "list"], "")
        p.addAction("radio-export", qsTr("Save the radio stations as a list (M3U)…"), ["m3u", "export", "stations", "list"], "")
        registerStations(p)
    }
    function registerStations(p) {
        for (const id of stationIds) p.removeAction(id)
        const ids = []
        for (const s of radio.allStations()) {
            const id = "radio:" + s.key
            p.addAction(id, s.fullName, ["radio", "station", s.networkName].concat(s.description.split(/[\/,]/).map(w => w.trim()).filter(w => w)), "")
            ids.push(id)
        }
        stationIds = ids
    }
    function updateActions(p) {
        p.setActionEnabled("radio-next", radio.active)
        p.setActionEnabled("radio-previous", radio.active)
        p.setActionEnabled("radio-favourite", radio.active)
    }

    function openPicker(group) {
        if (window && window.advanced && window.goToNode) {
            window.goToNode(["radio", "radio:" + (group || (radio.active ? (radio.current.network === "rekt" ? "rekt"
                                 : radio.current.network === "nightride" ? "nightride" : "mine") : "nightride"))])
            return
        }
        radioSheet.openSheet(group)
    }

    // True when it was a radio action.
    function runAction(id) {
        if (!id.startsWith("radio")) return false
        if (id.startsWith("radio:")) {
            if (window && !window.guard("play")) return true
            radio.tune(id.slice(6))
            return true
        }
        switch (id) {
        case "radio": openPicker(""); return true
        case "radio-find": findSheet.openSheet(""); return true
        case "radio-add": addSheet.openSheet(); return true
        case "radio-next": if (radio.active && (!window || window.guard("skip"))) radio.step(1); return true
        case "radio-previous": if (radio.active && (!window || window.guard("skip"))) radio.step(-1); return true
        case "radio-favourite": if (radio.active) radio.toggleFavourite(radio.currentKey); return true
        case "radio-import": radio.importLists(); return true
        case "radio-export": radio.exportList(); return true
        }
        return false
    }

    Connections {
        target: radio
        function onStationsChanged() {
            ui.registerStations(commandPalette)
            // The tree's counts (favourites, My stations).
            if (library.ready) library.reloadTree()
        }
    }

    // [ and ] change channel only while a station plays, so they mean
    // nothing else the rest of the time.
    Shortcut { sequence: "Shift+R"; context: Qt.ApplicationShortcut; enabled: !ui.window.sheetOpen; onActivated: ui.runAction("radio") }
    Shortcut { sequence: "]"; context: Qt.ApplicationShortcut; enabled: !ui.window.sheetOpen && backend.live; onActivated: ui.runAction("radio-next") }
    Shortcut { sequence: "["; context: Qt.ApplicationShortcut; enabled: !ui.window.sheetOpen && backend.live; onActivated: ui.runAction("radio-previous") }

    RadioSheet {
        id: radioSheet
        onFindStations: findSheet.openSheet("")
        onAddStation: addSheet.openSheet()
        onClosed: if (ui.window) ui.window.backToList()
    }
    FindStationsSheet { id: findSheet; onClosed: if (ui.window) ui.window.backToList() }
    AddStationSheet { id: addSheet; onClosed: if (ui.window) ui.window.backToList() }
}
