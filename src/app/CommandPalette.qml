import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// The command palette (Ctrl+K): type, move with Up and Down, Enter to
// choose, Esc to close. The results come from a Palette model (C++,
// src/core/palette.h), which also emits what was chosen.
//
//   CommandPalette { id: palette; model: commandPalette }
//   Shortcut { sequence: "Ctrl+K"; context: Qt.ApplicationShortcut; onActivated: palette.toggle() }
Popup {
    id: root

    // A Palette: text, count, activate(row), get(row).
    property var model: null
    property string fontFamily: "JetBrainsMono Nerd Font"
    readonly property int rowHeight: 44
    readonly property int visibleRows: 8

    // After a row is chosen, with the row's roles (kind, itemId, title...).
    signal chosen(var item)

    function toggle() { opened ? close() : open() }
    // Opens with text already typed (for screenshots and tests).
    function openWith(text) {
        if (!opened) open()
        input.text = text
    }

    function move(by) {
        if (list.count === 0) return
        list.currentIndex = Math.max(0, Math.min(list.count - 1, list.currentIndex + by))
        list.positionViewAtIndex(list.currentIndex, ListView.Contain)
    }

    function choose(row) {
        if (!model || row < 0 || row >= list.count) return
        const item = model.get(row)
        if (!model.activate(row)) return
        close()
        chosen(item)
    }

    parent: Overlay.overlay
    modal: true
    focus: true
    padding: 0
    width: Math.min(640, parent ? parent.width - 32 : 640)
    x: parent ? Math.round((parent.width - width) / 2) : 0
    y: parent ? Math.round(Math.min(parent.height * 0.12, 96)) : 0
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    Material.roundedScale: theme.radius

    // No fade or scale: it should be there the moment the key is pressed.
    enter: null
    exit: null

    Overlay.modal: Rectangle { color: Qt.alpha(theme.c.darker_background, 0.45) }

    background: Rectangle {
        color: theme.c.dark_background
        border.width: 1
        border.color: theme.c.lighter_background
        radius: theme.radius
    }

    onOpened: {
        input.text = ""
        if (model) model.text = ""
        list.currentIndex = list.count > 0 ? 0 : -1
        input.forceActiveFocus()
    }

    contentItem: ColumnLayout {
        spacing: 0

        // The search line.
        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: 46
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            spacing: 10

            Text {
                text: String.fromCodePoint(0xF0349) // magnify
                font.family: root.fontFamily
                font.pixelSize: 16
                color: theme.c.muted
            }

            TextInput {
                id: input
                Layout.fillWidth: true
                font.family: root.fontFamily
                font.pixelSize: 15
                color: theme.c.foreground
                selectionColor: theme.c.selection
                selectedTextColor: theme.c.bright_foreground
                clip: true
                focus: true

                onTextChanged: {
                    if (!root.model) return
                    root.model.text = text
                    list.currentIndex = list.count > 0 ? 0 : -1
                    list.positionViewAtBeginning()
                }

                // While typing, every key belongs here: the window's
                // single-letter shortcuts (Space, Q, Up, Return...) must
                // not fire. Ctrl+K still reaches the window, to close.
                Keys.onShortcutOverride: (event) => {
                    event.accepted = !(event.key === Qt.Key_K && (event.modifiers & Qt.ControlModifier))
                }
                Keys.onPressed: (event) => {
                    switch (event.key) {
                    case Qt.Key_Down: root.move(1); break
                    case Qt.Key_Up: root.move(-1); break
                    case Qt.Key_PageDown: root.move(root.visibleRows); break
                    case Qt.Key_PageUp: root.move(-root.visibleRows); break
                    case Qt.Key_Return:
                    case Qt.Key_Enter: root.choose(list.currentIndex); break
                    case Qt.Key_Escape: root.close(); break
                    default: return
                    }
                    event.accepted = true
                }

                Text {
                    anchors.fill: parent
                    verticalAlignment: Text.AlignVCenter
                    visible: input.text.length === 0
                    text: qsTr("Search actions, artists, albums and tracks")
                    font: input.font
                    color: theme.c.muted
                    elide: Text.ElideRight
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color: theme.c.lighter_background
        }

        ListView {
            id: list
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(count, root.visibleRows) * root.rowHeight
            Layout.topMargin: count > 0 ? 6 : 0
            Layout.bottomMargin: count > 0 ? 6 : 0
            Layout.leftMargin: 6
            Layout.rightMargin: 6
            clip: true
            model: root.model
            currentIndex: 0
            keyNavigationEnabled: false
            highlightMoveDuration: 0
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

            delegate: Rectangle {
                id: row
                required property int index
                required property string kind
                required property string kindLabel
                required property string title
                required property string subtitle
                required property string detail
                required property string shortcut
                required property bool actionEnabled
                readonly property bool current: ListView.isCurrentItem

                width: ListView.view.width
                height: root.rowHeight
                radius: Math.min(4, theme.radius)
                color: current ? theme.listSelection : hover.hovered ? Qt.alpha(theme.listSelection, 0.4) : "transparent"
                opacity: actionEnabled ? 1 : 0.5

                HoverHandler { id: hover }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 10
                    anchors.rightMargin: 10
                    spacing: 12

                    Text {
                        Layout.preferredWidth: 18
                        horizontalAlignment: Text.AlignHCenter
                        text: String.fromCodePoint(row.kind === "action" ? 0xF0241   // flash
                                                 : row.kind === "artist" ? 0xF0803   // account-music
                                                 : row.kind === "album" ? 0xF0025    // album
                                                 : row.kind === "track" ? 0xF075A    // music-note
                                                 : 0xF0349)                          // magnify
                        font.family: root.fontFamily
                        font.pixelSize: 16
                        color: row.current ? theme.accent : theme.c.muted
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 1
                        Text {
                            Layout.fillWidth: true
                            text: row.title
                            elide: Text.ElideRight
                            font.family: root.fontFamily
                            font.pixelSize: 13
                            color: row.current ? theme.c.bright_foreground : theme.c.foreground
                        }
                        Text {
                            Layout.fillWidth: true
                            visible: row.subtitle.length > 0
                            text: row.subtitle
                            elide: Text.ElideRight
                            font.family: root.fontFamily
                            font.pixelSize: 11
                            color: theme.c.muted
                        }
                    }

                    // A shortcut as a key cap; a length or year as plain text.
                    Rectangle {
                        visible: row.shortcut.length > 0
                        implicitWidth: keyText.implicitWidth + 12
                        implicitHeight: 20
                        radius: Math.min(3, theme.radius)
                        color: "transparent"
                        border.width: 1
                        border.color: theme.c.lighter_background
                        Text {
                            id: keyText
                            anchors.centerIn: parent
                            text: row.shortcut
                            font.family: root.fontFamily
                            font.pixelSize: 11
                            color: theme.c.light_foreground
                        }
                    }
                    Text {
                        visible: row.shortcut.length === 0 && row.detail.length > 0
                        text: row.detail
                        font.family: root.fontFamily
                        font.pixelSize: 11
                        color: theme.c.muted
                    }
                    Text {
                        Layout.preferredWidth: 52
                        horizontalAlignment: Text.AlignRight
                        text: row.kindLabel
                        elide: Text.ElideRight
                        font.family: root.fontFamily
                        font.pixelSize: 10
                        color: theme.c.muted
                    }
                }

                MouseArea {
                    anchors.fill: parent
                    onClicked: root.choose(row.index)
                }
            }
        }

        Text {
            Layout.fillWidth: true
            Layout.margins: 14
            visible: list.count === 0 && input.text.length > 0
            text: qsTr("Nothing matches “%1”").arg(input.text)
            elide: Text.ElideRight
            font.family: root.fontFamily
            font.pixelSize: 12
            color: theme.c.muted
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color: theme.c.lighter_background
        }

        Text {
            Layout.fillWidth: true
            Layout.leftMargin: 14
            Layout.rightMargin: 14
            Layout.topMargin: 6
            Layout.bottomMargin: 7
            text: qsTr("↑ ↓ to move    Enter to choose    Esc to close    > for actions only")
            elide: Text.ElideRight
            font.family: root.fontFamily
            font.pixelSize: 10
            color: theme.c.muted
        }
    }
}
