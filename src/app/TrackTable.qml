import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material

// The track list: every track in the chosen tree node that matches the
// search. Click a header to sort (again to reverse, a third time for the
// library order), drag a header's edge to resize, right-click a header to
// choose columns. Click, Shift+click and Ctrl+click select; Enter or a
// double-click plays.
FocusScope {
    id: pane
    property var tracks: library.tracks
    // Column layout, saved by the window: which are hidden, and widths set by hand.
    // Added starts hidden, so the rest fit a 1360-pixel window.
    property var hidden: [false, false, false, false, false, false, false, false, false, true, false]
    property var widths: ({})
    readonly property var defaultWidths: [46, 180, 170, 180, 56, 110, 64, 80, 56, 88, 124]
    signal leave() // to the tree
    signal layoutEdited()
    // A command from the menu, run by the window: edit-tags, tag-from-filename, organise.
    signal action(string id)

    // F2: edit the title of the track under the cursor in place. Enter
    // saves it to the file (one undoable batch), Esc leaves it.
    function editTitle() {
        const row = tracks.currentRow
        if (row < 0) return
        showRow(row)
        titleEditor.trackId = tracks.trackId(row)
        titleEditor.row = row
        titleInput.text = tracks.data(tracks.index(row, 1), Qt.DisplayRole)
        titleEditor.visible = true
        titleInput.forceActiveFocus()
        titleInput.selectAll()
    }

    function columnState() {
        const w = {}
        for (let c = 0; c < defaultWidths.length; ++c) {
            const width = table.explicitColumnWidth(c)
            if (width > 0) w[c] = Math.round(width)
        }
        return JSON.stringify({ hidden: hidden, widths: w })
    }
    function restoreColumns(json) {
        try {
            const s = JSON.parse(json)
            if (s.hidden && s.hidden.length === defaultWidths.length) hidden = s.hidden
            for (const c in s.widths) table.setColumnWidth(Number(c), s.widths[c])
        } catch (e) { }
        table.forceLayout()
    }
    function setHidden(column, hide) {
        const h = hidden.slice()
        h[column] = hide
        // Title stays: a list with no titles reads as nothing.
        if (column === 1) h[1] = false
        hidden = h
        table.forceLayout()
        layoutEdited()
    }
    function showRow(row) {
        if (row >= 0) table.positionViewAtRow(row, TableView.Contain)
    }
    function jumpToEnd() {
        tracks.select(tracks.count - 1, 0)
        table.positionViewAtRow(tracks.count - 1, TableView.AlignBottom)
    }
    function openMenu(x, y) {
        rowMenu.popup(pane, x, y)
    }

    Rectangle {
        anchors.fill: parent
        color: theme.c.background
    }

    // A new list (another node, a search, a sort) starts from the top, or
    // from the track the cursor was on. TableView left far down a list it
    // no longer has rebuilds row by row, which takes seconds at 200,000.
    //
    // A refresh (the library changed underneath, for example the folder
    // watcher added files) keeps the place instead: the track at the top
    // stays at the top, at the same offset.
    property var keepTop: null
    Connections {
        target: pane.tracks
        function onAboutToRefresh() {
            const top = Math.max(0, table.topRow)
            pane.keepTop = pane.tracks.count > 0
                ? { id: pane.tracks.trackId(top), row: top, offset: table.contentY - top * 24 } : null
        }
        function onModelAboutToBeReset() { table.contentY = 0 }
        function onReloaded() {
            const keep = pane.keepTop
            pane.keepTop = null
            if (keep) {
                const row = pane.tracks.rowOfTrack(keep.id)
                if (row >= 0) {
                    // Rows are all 24 pixels, so the place is a sum. (A
                    // positionViewAtRow() here is lost in the rebuild that
                    // follows the reset.)
                    table.contentY = row * 24 + Math.max(0, keep.offset)
                    return
                }
            }
            if (pane.tracks.currentRow >= 0) pane.showRow(pane.tracks.currentRow)
        }
    }

    HorizontalHeaderView {
        id: header
        objectName: "trackHeader"
        anchors { left: parent.left; right: parent.right; top: parent.top }
        syncView: table
        clip: true
        resizableColumns: true
        boundsBehavior: Flickable.StopAtBounds
        delegate: Rectangle {
            id: head
            required property int column
            required property var model
            implicitHeight: 28
            implicitWidth: 60
            color: hoverHead.hovered ? theme.c.lighter_background : theme.c.dark_background
            Rectangle { anchors { right: parent.right; top: parent.top; bottom: parent.bottom; margins: 5 }
                        width: 1; color: theme.c.lighter_background }
            Rectangle { anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
                        height: 1; color: theme.c.lighter_background }
            Text {
                anchors { fill: parent; leftMargin: 8; rightMargin: 8 }
                verticalAlignment: Text.AlignVCenter
                horizontalAlignment: head.column === 0 || head.column === 4 || head.column === 6 || head.column === 8
                                     ? Text.AlignRight : Text.AlignLeft
                elide: Text.ElideRight
                text: head.model.display !== undefined ? head.model.display : ""
                font.pixelSize: 12
                font.bold: pane.tracks.sortColumn === head.column
                color: pane.tracks.sortColumn === head.column ? theme.c.bright_foreground : theme.c.light_foreground
            }
            HoverHandler { id: hoverHead }
            TapHandler {
                acceptedButtons: Qt.LeftButton
                onTapped: pane.tracks.sortBy(head.column)
            }
            TapHandler {
                acceptedButtons: Qt.RightButton
                onTapped: (point) => columnMenu.popup()
            }
        }
    }

    TableView {
        id: table
        objectName: "trackTable"
        anchors { left: parent.left; right: parent.right; top: header.bottom; bottom: parent.bottom }
        clip: true
        focus: true
        model: pane.tracks
        boundsBehavior: Flickable.StopAtBounds
        resizableColumns: true
        keyNavigationEnabled: false
        pointerNavigationEnabled: false
        reuseItems: true
        columnWidthProvider: function (column) {
            if (pane.hidden[column]) return 0
            const w = explicitColumnWidth(column)
            if (w > 0) return w
            if (column !== 1) return pane.defaultWidths[column]
            // Title takes the room the other columns leave, until resized by hand.
            let others = 0
            for (let c = 0; c < pane.defaultWidths.length; ++c) {
                if (c === 1 || pane.hidden[c]) continue
                const e = explicitColumnWidth(c)
                others += e > 0 ? e : pane.defaultWidths[c]
            }
            return Math.max(pane.defaultWidths[1], table.width - others - 12)
        }
        onWidthChanged: forceLayout()
        onTopRowChanged: pane.tracks.setVisibleRows(topRow, bottomRow)
        onBottomRowChanged: pane.tracks.setVisibleRows(topRow, bottomRow)
        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
        ScrollBar.horizontal: ScrollBar { policy: ScrollBar.AsNeeded }

        Keys.onPressed: (event) => {
            const t = pane.tracks
            const page = Math.max(1, Math.floor(table.height / 24) - 1)
            const mods = event.modifiers & (Qt.ShiftModifier | Qt.ControlModifier)
            switch (event.key) {
            case Qt.Key_Up: t.moveCurrent(-1, mods); break
            case Qt.Key_Down: t.moveCurrent(1, mods); break
            case Qt.Key_PageUp: t.moveCurrent(-page, mods); break
            case Qt.Key_PageDown: t.moveCurrent(page, mods); break
            case Qt.Key_Home: t.moveCurrent(-t.count, mods); break
            case Qt.Key_End: t.moveCurrent(t.count, mods); break
            case Qt.Key_Left: pane.leave(); break
            case Qt.Key_Return:
            case Qt.Key_Enter:
                if (t.selectedCount > 1) library.playSelection()
                else if (t.currentRow >= 0) library.playRow(t.currentRow)
                break
            case Qt.Key_A:
                if (!(event.modifiers & Qt.ControlModifier)) return
                t.selectAll()
                break
            case Qt.Key_Space:
                // Ctrl+Space picks the row under the cursor; plain Space is play/pause.
                if (!(event.modifiers & Qt.ControlModifier)) return
                t.select(t.currentRow, Qt.ControlModifier)
                break
            case Qt.Key_Menu:
                pane.openMenu(pane.width / 3, Math.min(pane.height - 40, header.height + 24 * (t.currentRow + 1) - table.contentY))
                break
            default: return
            }
            if (event.key !== Qt.Key_Menu) pane.showRow(t.currentRow)
            event.accepted = true
        }

        delegate: Rectangle {
            id: cell
            required property int row
            required property int column
            required property var display
            required property bool rowSelected
            required property bool rowCurrent
            required property bool numeric
            required property var rating
            required property var available
            required property var playing
            readonly property bool isRating: column === 7
            readonly property int ratingValue: rating !== undefined ? rating : -1

            implicitHeight: 24
            color: rowSelected ? (table.activeFocus ? theme.listSelection : Qt.alpha(theme.listSelection, 0.6))
                 : row % 2 ? Qt.alpha(theme.c.lighter_background, 0.35) : "transparent"

            // The keyboard cursor: a line above and below the row.
            Rectangle { visible: cell.rowCurrent && table.activeFocus; color: theme.accent; height: 1
                        anchors { left: parent.left; right: parent.right; top: parent.top } }
            Rectangle { visible: cell.rowCurrent && table.activeFocus; color: theme.accent; height: 1
                        anchors { left: parent.left; right: parent.right; bottom: parent.bottom } }

            Text {
                visible: !cell.isRating
                anchors { fill: parent; leftMargin: 8; rightMargin: 8 }
                verticalAlignment: Text.AlignVCenter
                horizontalAlignment: cell.numeric ? Text.AlignRight : Text.AlignLeft
                elide: Text.ElideRight
                text: cell.display !== undefined ? cell.display : ""
                font.pixelSize: 13
                font.bold: cell.playing === true
                color: cell.playing === true ? theme.accent
                     : cell.rowSelected ? theme.c.bright_foreground
                     : cell.available !== false ? theme.c.foreground : theme.c.muted
            }
            Row {
                visible: cell.isRating
                anchors { left: parent.left; leftMargin: 8; verticalCenter: parent.verticalCenter }
                Repeater {
                    model: 5
                    Text {
                        required property int index
                        // A half star shows as a dimmer full one.
                        readonly property real fill: Math.max(0, Math.min(1, (cell.ratingValue - index * 20) / 20))
                        text: "★"
                        font.pixelSize: 12
                        color: fill >= 1 ? theme.accent : fill >= 0.5 ? Qt.alpha(theme.accent, 0.5) : theme.c.lighter_background
                    }
                }
            }

            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.LeftButton | Qt.RightButton
                preventStealing: true
                onPressed: (mouse) => {
                    table.forceActiveFocus()
                    if (mouse.button === Qt.RightButton) {
                        if (!pane.tracks.isSelected(cell.row)) pane.tracks.select(cell.row, 0)
                        const p = mapToItem(pane, mouse.x, mouse.y)
                        pane.openMenu(p.x, p.y)
                        return
                    }
                    pane.tracks.select(cell.row, mouse.modifiers & (Qt.ShiftModifier | Qt.ControlModifier))
                }
                onDoubleClicked: (mouse) => {
                    if (mouse.button === Qt.LeftButton) library.playRow(cell.row)
                }
            }
        }
    }

    Text {
        anchors.centerIn: table
        width: table.width - 60
        visible: pane.tracks.count === 0
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.WordWrap
        text: pane.tracks.filter !== "" ? qsTr("No tracks match the search.")
              : library.tree.count > 0 && library.currentNode === 0
                ? qsTr("The library is empty. Add a folder of music to start.") : qsTr("No tracks here.")
        font.pixelSize: 13
        color: theme.c.muted
    }

    Menu {
        id: rowMenu
        Material.roundedScale: theme.radius
        MenuItem { text: qsTr("Play"); onTriggered: library.playSelection() }
        MenuItem { text: qsTr("Play next"); onTriggered: library.queueSelection() }
        MenuItem { text: qsTr("Add to Now Playing"); onTriggered: library.appendSelection() }
        MenuSeparator { }
        MenuItem { objectName: "menuEditTags"; text: qsTr("Edit tags…"); onTriggered: pane.action("edit-tags") }
        MenuItem { text: qsTr("Tag from file names…"); onTriggered: pane.action("tag-from-filename") }
        MenuItem { text: qsTr("Organise files…"); onTriggered: pane.action("organise") }
        MenuSeparator { }
        MenuItem { text: qsTr("Show in folder"); onTriggered: library.showInFolder(pane.tracks.currentRow) }
    }

    // The in-place title editor (F2), over the Title cell.
    Rectangle {
        id: titleEditor
        objectName: "titleEditor"
        property int row: -1
        property var trackId: 0
        visible: false
        z: 5
        x: (pane.hidden[0] ? 0 : table.columnWidth(0)) - table.contentX
        y: header.height + row * 24 - table.contentY
        width: Math.max(160, table.columnWidth(1))
        height: 24
        color: theme.c.darker_background
        border.width: 1
        border.color: theme.accent
        function finish(save) {
            if (!visible) return
            visible = false
            if (save) library.tagEditor.quickSet(trackId, "title", titleInput.text)
            table.forceActiveFocus()
        }
        TextInput {
            id: titleInput
            objectName: "titleInput"
            anchors { fill: parent; leftMargin: 8; rightMargin: 8 }
            verticalAlignment: Text.AlignVCenter
            font.pixelSize: 13
            color: theme.c.bright_foreground
            selectionColor: theme.c.selection
            selectByMouse: true
            clip: true
            Keys.onPressed: (event) => {
                if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) { titleEditor.finish(true); event.accepted = true }
                else if (event.key === Qt.Key_Escape) { titleEditor.finish(false); event.accepted = true }
            }
            // Every key belongs to the editor while it is open.
            Keys.onShortcutOverride: (event) => event.accepted = true
            onActiveFocusChanged: if (!activeFocus) titleEditor.finish(false)
        }
    }

    Menu {
        id: columnMenu
        Material.roundedScale: theme.radius
        Instantiator {
            model: pane.defaultWidths.length
            delegate: MenuItem {
                required property int index
                text: pane.tracks.columnName(index)
                checkable: true
                checked: !pane.hidden[index]
                enabled: index !== 1
                onTriggered: pane.setHidden(index, !checked)
            }
            onObjectAdded: (index, object) => columnMenu.insertItem(index, object)
            onObjectRemoved: (index, object) => columnMenu.removeItem(object)
        }
    }

    function openColumns(item) {
        columnMenu.popup(item, 0, item.height)
    }
}
