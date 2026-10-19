import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// The column browser: Genre | Album Artist (or Artist) | Album, above the
// track list. Click a value to narrow the panes to its right and the list;
// Ctrl+click adds or removes a value, Shift+click picks a range, and
// "All" goes back. From the keyboard: Up and Down choose, Shift extends,
// Left and Right move between panes, Enter goes to the list.
FocusScope {
    id: browser
    objectName: "columnBrowser"
    property var model: library.browser
    signal leave()      // to the track list
    signal leaveLeft()  // to the tree

    function focusPane(i) {
        const p = repeater.itemAt(Math.max(0, Math.min(2, i)))
        if (p) p.focusList()
    }

    Rectangle { anchors.fill: parent; color: theme.c.background }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        Repeater {
            id: repeater
            model: 3

            FocusScope {
                id: pane
                required property int index
                readonly property var facet: browser.model.pane(index)
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.preferredWidth: 1
                objectName: "browserPane" + index

                function focusList() { list.forceActiveFocus() }

                Rectangle {
                    anchors { right: parent.right; top: parent.top; bottom: parent.bottom }
                    width: 1
                    visible: pane.index < 2
                    color: theme.c.lighter_background
                }

                // The pane's title; the middle one switches between album
                // artist and artist.
                Rectangle {
                    id: head
                    anchors { left: parent.left; right: parent.right; top: parent.top; rightMargin: pane.index < 2 ? 1 : 0 }
                    height: 26
                    color: theme.c.dark_background
                    Rectangle { anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
                                height: 1; color: theme.c.lighter_background }
                    Text {
                        anchors { left: parent.left; leftMargin: 8; verticalCenter: parent.verticalCenter
                                  right: modeSwitch.left; rightMargin: 4 }
                        text: pane.facet ? pane.facet.title : ""
                        elide: Text.ElideRight
                        font.pixelSize: 12
                        font.bold: true
                        color: theme.c.light_foreground
                    }
                    Text {
                        id: modeSwitch
                        anchors { right: parent.right; rightMargin: 8; verticalCenter: parent.verticalCenter }
                        visible: pane.index === 1
                        text: browser.model.artistMode === "artist" ? qsTr("Show album artists") : qsTr("Show artists")
                        font.pixelSize: 11
                        color: switchHover.hovered ? theme.accent : theme.c.muted
                        HoverHandler { id: switchHover; cursorShape: Qt.PointingHandCursor }
                        TapHandler {
                            onTapped: browser.model.artistMode = browser.model.artistMode === "artist" ? "albumartist" : "artist"
                        }
                    }
                }

                ListView {
                    id: list
                    anchors { left: parent.left; right: parent.right; top: head.bottom; bottom: parent.bottom
                              rightMargin: pane.index < 2 ? 1 : 0 }
                    clip: true
                    focus: true
                    model: pane.facet
                    currentIndex: 0
                    keyNavigationEnabled: false
                    highlightMoveDuration: 0
                    boundsBehavior: Flickable.StopAtBounds
                    reuseItems: true
                    ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                    function choose(row, mods) {
                        row = Math.max(0, Math.min(count - 1, row))
                        currentIndex = row
                        // Ctrl moves the cursor only, as in the track list, so
                        // Ctrl+Space can pick values that are far apart.
                        if (!((mods & Qt.ControlModifier) && !(mods & Qt.ShiftModifier)))
                            browser.model.select(pane.index, row, mods)
                        positionViewAtIndex(row, ListView.Contain)
                    }

                    Keys.onPressed: (event) => {
                        const mods = event.modifiers & (Qt.ShiftModifier | Qt.ControlModifier)
                        const page = Math.max(1, Math.floor(list.height / 22) - 1)
                        switch (event.key) {
                        case Qt.Key_Up: list.choose(list.currentIndex - 1, mods); break
                        case Qt.Key_Down: list.choose(list.currentIndex + 1, mods); break
                        case Qt.Key_PageUp: list.choose(list.currentIndex - page, mods); break
                        case Qt.Key_PageDown: list.choose(list.currentIndex + page, mods); break
                        case Qt.Key_Home: list.choose(0, mods); break
                        case Qt.Key_End: list.choose(list.count - 1, mods); break
                        case Qt.Key_Left:
                            if (pane.index === 0) browser.leaveLeft(); else browser.focusPane(pane.index - 1)
                            break
                        case Qt.Key_Right:
                            if (pane.index === 2) browser.leave(); else browser.focusPane(pane.index + 1)
                            break
                        case Qt.Key_Return:
                        case Qt.Key_Enter: browser.leave(); break
                        case Qt.Key_Space:
                            if (!(event.modifiers & Qt.ControlModifier)) return
                            browser.model.select(pane.index, list.currentIndex, Qt.ControlModifier)
                            break
                        default: return
                        }
                        event.accepted = true
                    }

                    // A new list of values starts at the first chosen one.
                    Connections {
                        target: pane.facet
                        function onModelReset() {
                            list.currentIndex = pane.facet.firstSelectedRow()
                            list.positionViewAtIndex(list.currentIndex, ListView.Contain)
                        }
                        // Chosen from elsewhere (the palette, a refresh): show it.
                        function onSelectionChanged() {
                            if (list.activeFocus) return
                            list.currentIndex = pane.facet.firstSelectedRow()
                            list.positionViewAtIndex(list.currentIndex, ListView.Contain)
                        }
                    }

                    delegate: Rectangle {
                        id: value
                        required property int index
                        required property string label
                        required property string detail
                        required property var valueCount
                        required property bool valueSelected
                        required property bool isAll
                        readonly property bool cursor: index === list.currentIndex && list.activeFocus

                        width: ListView.view.width
                        height: 22
                        color: valueSelected ? (list.activeFocus ? theme.listSelection : Qt.alpha(theme.listSelection, 0.6))
                             : hover.hovered ? Qt.alpha(theme.listSelection, 0.25) : "transparent"
                        border.width: cursor && !valueSelected ? 1 : 0
                        border.color: theme.accent

                        HoverHandler { id: hover }

                        Text {
                            anchors { left: parent.left; leftMargin: 8; right: countText.left; rightMargin: 6
                                      verticalCenter: parent.verticalCenter }
                            elide: Text.ElideRight
                            text: value.label + (value.detail !== "" && !value.isAll && pane.index === 2
                                                 ? "  ·  " + value.detail : "")
                            textFormat: Text.PlainText
                            font.pixelSize: 13
                            font.bold: value.isAll
                            color: value.valueSelected ? theme.c.bright_foreground : theme.c.foreground
                        }
                        Text {
                            id: countText
                            anchors { right: parent.right; rightMargin: 8; verticalCenter: parent.verticalCenter }
                            visible: !value.isAll
                            text: Number(value.valueCount).toLocaleString(Qt.locale(), 'f', 0)
                            font.pixelSize: 11
                            color: theme.c.muted
                        }

                        MouseArea {
                            anchors.fill: parent
                            onPressed: (mouse) => {
                                list.forceActiveFocus()
                                list.currentIndex = value.index
                                browser.model.select(pane.index, value.index,
                                                     mouse.modifiers & (Qt.ShiftModifier | Qt.ControlModifier))
                            }
                        }
                    }
                }
            }
        }
    }
}
