import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// One playlist in its own order: a static playlist's entries, duplicates and
// entries that match no library track included, or an auto-playlist's
// tracks as its query gives them now. A static playlist can mix Spotify
// tracks with local files: they carry Spotify's mark, and are greyed (and
// left out when the playlist plays) while Spotify can't play them.
//
// Up and Down move, Shift selects a range, Enter plays from the cursor,
// Alt+Up and Alt+Down move the selected entries, Delete takes them out,
// Left goes back to the tree. Entries can also be dragged into a new place.
FocusScope {
    id: pane
    objectName: "playlistView"
    readonly property var items: library.playlists.items
    // Party mode can lock editing.
    property bool locked: false
    readonly property bool canEdit: items.editable && !locked
    // Why Spotify entries can't play now, or empty.
    readonly property string spotifyOff: spotify.unavailableReason
    signal leave()
    // A command run by the window (edit-tags, edit-autoplaylist, export-playlist...).
    signal action(string id)

    function clock(ms) {
        const total = Math.floor(Math.max(0, ms) / 1000)
        const h = Math.floor(total / 3600), m = Math.floor(total % 3600 / 60), s = total % 60
        return (h > 0 ? h + ":" + (m < 10 ? "0" : "") : "") + m + ":" + (s < 10 ? "0" : "") + s
    }
    function showRow(row) { if (row >= 0) list.positionViewAtIndex(row, ListView.Contain) }
    function removeSelected() {
        if (!canEdit) return
        items.removeSelection()
        showRow(items.currentRow)
    }

    readonly property int numberWidth: 52
    readonly property int lengthWidth: 70
    readonly property real textWidth: Math.max(120, (list.width - numberWidth - lengthWidth - 12) / 3)

    Rectangle { anchors.fill: parent; color: theme.c.background }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // What the playlist is, and what can be done with it.
        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 10
            Layout.rightMargin: 6
            Layout.topMargin: 4
            Layout.bottomMargin: 4
            spacing: 6
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 0
                Text {
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                    text: pane.items.name
                    font.pixelSize: 15
                    font.bold: true
                    color: theme.c.bright_foreground
                }
                Text {
                    objectName: "playlistSummary"
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                    text: {
                        if (pane.items.kind === "folder") return qsTr("A folder of playlists")
                        let parts = [qsTr("%n entries", "", pane.items.count)]
                        if (pane.items.totalMs > 0) parts.push(pane.clock(pane.items.totalMs))
                        if (pane.items.duplicates > 0) parts.push(qsTr("%n repeated", "", pane.items.duplicates))
                        if (pane.items.spotifyCount > 0) parts.push(pane.spotifyOff !== ""
                            ? qsTr("%n from Spotify (off)", "", pane.items.spotifyCount)
                            : qsTr("%n from Spotify", "", pane.items.spotifyCount))
                        if (pane.items.unmatched > 0) parts.push(qsTr("%n not in the library", "", pane.items.unmatched))
                        if (pane.items.kind === "auto") parts.push(qsTr("auto-playlist: %1").arg(pane.items.query || qsTr("every track")))
                        return parts.join("  ·  ")
                    }
                    font.pixelSize: 12
                    color: theme.c.muted
                }
            }
            Button {
                text: qsTr("Play")
                flat: true
                font.pixelSize: 13
                implicitHeight: 34; topInset: 0; bottomInset: 0
                enabled: pane.items.count > 0
                onClicked: library.playlists.play(pane.items.playlistId, 0)
            }
            Button {
                text: qsTr("Play next")
                flat: true
                font.pixelSize: 13
                implicitHeight: 34; topInset: 0; bottomInset: 0
                enabled: pane.items.count > 0
                onClicked: library.playlists.queueNext(pane.items.playlistId)
            }
            Button {
                text: qsTr("Edit…")
                flat: true
                font.pixelSize: 13
                implicitHeight: 34; topInset: 0; bottomInset: 0
                visible: pane.items.kind === "auto"
                enabled: !pane.locked
                onClicked: pane.action("edit-autoplaylist")
            }
            Button {
                text: qsTr("New shuffle")
                flat: true
                font.pixelSize: 13
                implicitHeight: 34; topInset: 0; bottomInset: 0
                visible: pane.items.kind === "auto" && pane.items.query.indexOf("sort:random") >= 0
                ToolTip.visible: hovered
                ToolTip.delay: 600
                ToolTip.text: qsTr("A random auto-playlist keeps its order until you ask for a new one")
                onClicked: library.playlists.reroll(pane.items.playlistId)
            }
            Button {
                text: qsTr("Export…")
                flat: true
                font.pixelSize: 13
                implicitHeight: 34; topInset: 0; bottomInset: 0
                visible: pane.items.kind !== "folder"
                onClicked: library.playlists.exportPlaylist(pane.items.playlistId)
            }
        }

        Rectangle {
            id: header
            Layout.fillWidth: true
            height: 28
            color: theme.c.dark_background
            Rectangle { anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
                        height: 1; color: theme.c.lighter_background }
            Row {
                anchors.fill: parent
                Repeater {
                    model: [[qsTr("#"), pane.numberWidth, true], [qsTr("Title"), pane.textWidth, false],
                            [qsTr("Artist"), pane.textWidth, false], [qsTr("Album"), pane.textWidth, false],
                            [qsTr("Length"), pane.lengthWidth, true]]
                    Text {
                        required property var modelData
                        width: modelData[1]
                        height: header.height
                        leftPadding: 8; rightPadding: 8
                        verticalAlignment: Text.AlignVCenter
                        horizontalAlignment: modelData[2] ? Text.AlignRight : Text.AlignLeft
                        text: modelData[0]
                        font.pixelSize: 12
                        color: theme.c.light_foreground
                    }
                }
            }
        }

        ListView {
            id: list
            objectName: "playlistList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            focus: true
            model: pane.items
            currentIndex: pane.items.currentRow
            keyNavigationEnabled: false
            highlightMoveDuration: 0
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

            // Where a drag would put the entries: a line above that row.
            property int dropRow: -1

            Keys.onPressed: (event) => {
                const t = pane.items
                const page = Math.max(1, Math.floor(list.height / 24) - 1)
                const mods = event.modifiers & (Qt.ShiftModifier | Qt.ControlModifier)
                const alt = event.modifiers & Qt.AltModifier
                switch (event.key) {
                case Qt.Key_Up:
                    if (alt) { if (pane.canEdit) t.moveSelection(-1) }
                    else t.moveCurrent(-1, mods)
                    break
                case Qt.Key_Down:
                    if (alt) { if (pane.canEdit) t.moveSelection(1) }
                    else t.moveCurrent(1, mods)
                    break
                case Qt.Key_PageUp: t.moveCurrent(-page, mods); break
                case Qt.Key_PageDown: t.moveCurrent(page, mods); break
                case Qt.Key_Home: t.moveCurrent(-t.count, mods); break
                case Qt.Key_End: t.moveCurrent(t.count, mods); break
                case Qt.Key_Left: pane.leave(); break
                case Qt.Key_Delete: pane.removeSelected(); break
                case Qt.Key_Return:
                case Qt.Key_Enter:
                    if (t.selectedCount > 1) library.playlists.playSelection()
                    else if (t.currentRow >= 0) library.playlists.play(t.playlistId, t.currentRow)
                    break
                case Qt.Key_A:
                    if (!(event.modifiers & Qt.ControlModifier)) return
                    t.selectAll()
                    break
                case Qt.Key_Menu:
                    rowMenu.popup(list, list.width / 3, Math.min(list.height - 40, 24 * (t.currentRow + 1) - list.contentY))
                    break
                default: return
                }
                pane.showRow(t.currentRow)
                event.accepted = true
            }

            delegate: Rectangle {
                id: entry
                required property int index
                required property int number
                required property string title
                required property string artist
                required property string album
                required property var duration
                required property string availability
                required property bool duplicate
                required property bool rowSelected
                required property bool rowCurrent
                required property bool fromSpotify
                readonly property bool spotifyOff: fromSpotify && pane.spotifyOff !== ""
                readonly property bool playable: availability === "online" || (fromSpotify && !spotifyOff)

                width: ListView.view.width
                height: 24
                color: rowSelected ? (list.activeFocus ? theme.listSelection : Qt.alpha(theme.listSelection, 0.6))
                     : index % 2 ? Qt.alpha(theme.c.lighter_background, 0.35) : "transparent"

                Rectangle { visible: entry.rowCurrent && list.activeFocus; color: theme.accent; height: 1
                            anchors { left: parent.left; right: parent.right; top: parent.top } }
                Rectangle { visible: entry.rowCurrent && list.activeFocus; color: theme.accent; height: 1
                            anchors { left: parent.left; right: parent.right; bottom: parent.bottom } }
                // The drop line while dragging.
                Rectangle { visible: list.dropRow === entry.index; color: theme.accent; height: 2; z: 2
                            anchors { left: parent.left; right: parent.right; top: parent.top } }
                Rectangle { visible: list.dropRow === list.count && entry.index === list.count - 1
                            color: theme.accent; height: 2; z: 2
                            anchors { left: parent.left; right: parent.right; bottom: parent.bottom } }

                Row {
                    anchors.fill: parent
                    Text {
                        width: pane.numberWidth; height: entry.height
                        leftPadding: 8; rightPadding: 8
                        verticalAlignment: Text.AlignVCenter
                        horizontalAlignment: Text.AlignRight
                        text: entry.number
                        font.pixelSize: 12
                        color: theme.c.muted
                    }
                    Item {
                        width: pane.textWidth; height: entry.height
                        // Spotify's mark before a Spotify track's title.
                        Text {
                            id: spotifyMark
                            objectName: "spotifyMark"
                            visible: entry.fromSpotify
                            anchors { left: parent.left; leftMargin: 8; verticalCenter: parent.verticalCenter }
                            text: String.fromCodePoint(0xF04C7)
                            font.family: "JetBrainsMono Nerd Font"
                            font.pixelSize: 13
                            color: entry.spotifyOff ? theme.c.muted : theme.c.green
                        }
                        Text {
                            id: titleText
                            anchors { left: parent.left; leftMargin: spotifyMark.visible ? 26 : 8; verticalCenter: parent.verticalCenter }
                            width: Math.min(implicitWidth, parent.width - anchors.leftMargin - 8 - (badge.visible ? badge.width + 6 : 0))
                            elide: Text.ElideRight
                            text: entry.title
                            font.pixelSize: 13
                            color: entry.rowSelected ? theme.c.bright_foreground
                                 : entry.playable ? theme.c.foreground : theme.c.muted
                            font.italic: entry.availability === "unmatched"
                        }
                        // Marks an entry that is in the list more than once,
                        // or that no longer matches a track.
                        Rectangle {
                            id: badge
                            visible: entry.duplicate || (entry.availability !== "online" && !entry.fromSpotify)
                            anchors { left: titleText.right; leftMargin: 6; verticalCenter: parent.verticalCenter }
                            width: badgeText.implicitWidth + 10
                            height: 16
                            radius: Math.min(3, theme.radius)
                            color: "transparent"
                            border.width: 1
                            border.color: entry.availability !== "online" ? theme.c.yellow : theme.c.muted
                            Text {
                                id: badgeText
                                anchors.centerIn: parent
                                text: entry.availability === "unmatched" ? qsTr("not in library")
                                    : entry.availability === "offline" ? qsTr("offline")
                                    : entry.availability === "missing" ? qsTr("missing")
                                    : qsTr("repeat")
                                font.pixelSize: 10
                                color: entry.availability !== "online" ? theme.c.yellow : theme.c.muted
                            }
                        }
                    }
                    Text {
                        width: pane.textWidth; height: entry.height
                        leftPadding: 8; rightPadding: 8
                        verticalAlignment: Text.AlignVCenter
                        elide: Text.ElideRight
                        text: entry.artist
                        font.pixelSize: 13
                        color: entry.playable ? theme.c.foreground : theme.c.muted
                    }
                    Text {
                        width: pane.textWidth; height: entry.height
                        leftPadding: 8; rightPadding: 8
                        verticalAlignment: Text.AlignVCenter
                        elide: Text.ElideRight
                        text: entry.album
                        font.pixelSize: 13
                        color: entry.playable ? theme.c.foreground : theme.c.muted
                    }
                    Text {
                        width: pane.lengthWidth; height: entry.height
                        leftPadding: 8; rightPadding: 8
                        verticalAlignment: Text.AlignVCenter
                        horizontalAlignment: Text.AlignRight
                        text: entry.duration > 0 ? pane.clock(entry.duration) : ""
                        font.pixelSize: 13
                        color: theme.c.muted
                    }
                }

                // Why a greyed Spotify entry won't play, on hover.
                HoverHandler { id: entryHover; enabled: entry.spotifyOff }
                ToolTip.visible: entryHover.hovered
                ToolTip.delay: 500
                ToolTip.text: pane.spotifyOff

                MouseArea {
                    anchors.fill: parent
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    preventStealing: true
                    property point pressedAt
                    property bool dragging: false
                    onPressed: (mouse) => {
                        list.forceActiveFocus()
                        pressedAt = Qt.point(mouse.x, mouse.y)
                        dragging = false
                        if (mouse.button === Qt.RightButton) {
                            if (!entry.rowSelected) pane.items.select(entry.index, 0)
                            const p = mapToItem(list, mouse.x, mouse.y)
                            rowMenu.popup(list, p.x, p.y)
                            return
                        }
                        // A press on a chosen entry keeps the selection, so it can be dragged.
                        if (!(entry.rowSelected && !(mouse.modifiers & (Qt.ShiftModifier | Qt.ControlModifier))))
                            pane.items.select(entry.index, mouse.modifiers & (Qt.ShiftModifier | Qt.ControlModifier))
                    }
                    onPositionChanged: (mouse) => {
                        if (!pressed || !pane.canEdit) return
                        if (!dragging && Math.abs(mouse.y - pressedAt.y) < 8) return
                        dragging = true
                        const p = mapToItem(list.contentItem, mouse.x, mouse.y)
                        list.dropRow = Math.max(0, Math.min(list.count, Math.round(p.y / 24)))
                    }
                    onReleased: (mouse) => {
                        if (dragging && list.dropRow >= 0) pane.items.moveSelectionTo(list.dropRow)
                        else if (mouse.button === Qt.LeftButton && entry.rowSelected && !(mouse.modifiers & (Qt.ShiftModifier | Qt.ControlModifier)))
                            pane.items.select(entry.index, 0)
                        dragging = false
                        list.dropRow = -1
                    }
                    onDoubleClicked: (mouse) => {
                        if (mouse.button === Qt.LeftButton) library.playlists.play(pane.items.playlistId, entry.index)
                    }
                }
            }
        }
    }

    Text {
        anchors.centerIn: parent
        width: parent.width - 60
        visible: pane.items.count === 0
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.WordWrap
        text: pane.items.kind === "folder" ? qsTr("Choose a playlist inside this folder.")
            : pane.items.kind === "auto" ? (pane.items.error !== "" ? pane.items.error : qsTr("No tracks match this auto-playlist's query."))
            : qsTr("This playlist is empty. Drag tracks from the library or from Spotify onto it in the tree, or use Add to playlist in a track's menu.")
        font.pixelSize: 13
        color: theme.c.muted
    }

    Menu {
        id: rowMenu
        MenuItem { text: qsTr("Play"); onTriggered: library.playlists.playSelection() }
        MenuItem { text: qsTr("Play next"); onTriggered: library.playlists.queueSelection() }
        MenuSeparator { }
        MenuItem { text: qsTr("Edit tags…"); enabled: pane.items.selectedTrackIds().length > 0; onTriggered: pane.action("edit-tags") }
        MenuItem { text: qsTr("Move up (Alt+Up)"); enabled: pane.canEdit; onTriggered: pane.items.moveSelection(-1) }
        MenuItem { text: qsTr("Move down (Alt+Down)"); enabled: pane.canEdit; onTriggered: pane.items.moveSelection(1) }
        MenuItem { text: qsTr("Remove from playlist (Delete)"); enabled: pane.canEdit; onTriggered: pane.removeSelected() }
    }
}
