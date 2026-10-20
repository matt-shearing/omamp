import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// The channel picker: one card per station in the chosen group (Nightride
// FM, Rekt Network, Favourites, My stations), each with its colour, its
// genres and what it is playing right now. Click or Enter tunes in; F
// marks a favourite; Delete removes a station of your own.
//
// It shows in advanced mode when Radio is chosen in the tree, and in the
// Radio sheet (Shift+R) over the deck.
FocusScope {
    id: view
    objectName: "radioView"
    // The tabs along the top; the tree chooses the group in advanced mode,
    // so it can hide them.
    property bool showTabs: true
    signal findStations()
    signal addStation()
    signal leave()
    signal tuned()

    readonly property string mono: "JetBrainsMono Nerd Font"

    function focusGrid() { grid.forceActiveFocus() }
    function tuneCurrent() {
        if (grid.currentIndex >= 0 && radio.tuneRow(grid.currentIndex)) view.tuned()
    }

    // Every channel's now-playing line stays live while the picker shows.
    Binding { target: radio; property: "pickerVisible"; value: view.visible }
    // The cursor starts on the station playing, else on the first card.
    function placeCursor() {
        const row = radio.stations.rowOf(radio.currentKey)
        if (row >= 0) grid.currentIndex = row
        else if (grid.currentIndex < 0 || grid.currentIndex >= grid.count) grid.currentIndex = grid.count > 0 ? 0 : -1
    }
    onVisibleChanged: if (visible) Qt.callLater(placeCursor)
    Connections {
        target: radio.stations
        function onModelReset() { Qt.callLater(view.placeCursor) }
    }

    Rectangle { anchors.fill: parent; color: theme.c.background }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 14
        spacing: 12

        RowLayout {
            Layout.fillWidth: true
            spacing: 6

            Repeater {
                model: view.showTabs ? radio.groups : []
                Button {
                    required property var modelData
                    objectName: "radioTab_" + modelData.id
                    text: modelData.label
                    flat: radio.group !== modelData.id
                    highlighted: radio.group === modelData.id
                    focusPolicy: Qt.NoFocus
                    onClicked: { radio.group = modelData.id; grid.currentIndex = 0; grid.forceActiveFocus() }
                }
            }
            Text {
                visible: !view.showTabs
                text: radio.groups.find(g => g.id === radio.group)?.label ?? ""
                font.pixelSize: 18; font.bold: true
                color: theme.c.bright_foreground
            }
            Item { Layout.fillWidth: true }
            Button {
                objectName: "findStationsButton"
                text: qsTr("Find stations…")
                focusPolicy: Qt.NoFocus
                onClicked: view.findStations()
            }
            Button {
                objectName: "addStationButton"
                text: qsTr("Add address…")
                flat: true
                focusPolicy: Qt.NoFocus
                onClicked: view.addStation()
            }
        }

        GridView {
            id: grid
            objectName: "radioGrid"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            focus: true
            model: radio.stations
            keyNavigationEnabled: true
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
            // As many columns of at least 260 px as fit.
            readonly property int columns: Math.max(1, Math.floor(width / 260))
            cellWidth: Math.floor(width / columns)
            cellHeight: 108
            highlightFollowsCurrentItem: false

            Keys.onReturnPressed: view.tuneCurrent()
            Keys.onEnterPressed: view.tuneCurrent()
            Keys.onPressed: (event) => {
                if (event.modifiers & (Qt.ControlModifier | Qt.AltModifier)) return
                if (event.key === Qt.Key_F && currentIndex >= 0) {
                    radio.toggleFavourite(radio.stations.keyAt(currentIndex))
                    event.accepted = true
                } else if (event.key === Qt.Key_Delete && currentIndex >= 0 && currentItem && !currentItem.isBuiltin) {
                    radio.removeStation(radio.stations.keyAt(currentIndex))
                    event.accepted = true
                } else if (event.key === Qt.Key_Left) {
                    // From the first column back to the tree.
                    if (currentIndex % columns === 0) view.leave()
                    else moveCurrentIndexLeft()
                    event.accepted = true
                } else if (event.key === Qt.Key_Right) {
                    moveCurrentIndexRight()
                    event.accepted = true
                }
            }

            delegate: Item {
                id: cell
                required property int index
                required property string key
                required property string name
                required property string description
                required property string stationColor
                required property string nowPlaying
                required property bool favourite
                required property bool playing
                required property string artUrl
                required property bool builtin
                required property string detail
                readonly property bool isBuiltin: builtin
                readonly property bool chosen: GridView.isCurrentItem && grid.activeFocus
                width: grid.cellWidth
                height: grid.cellHeight

                Rectangle {
                    id: card
                    objectName: "radioCard_" + cell.key
                    anchors.fill: parent
                    anchors.margins: 5
                    radius: theme.radius
                    color: cell.playing ? Qt.alpha(cell.stationColor, theme.light ? 0.16 : 0.22)
                         : hover.hovered ? theme.c.lighter_background : theme.c.darker_background
                    border.width: cell.chosen || cell.playing ? 2 : 1
                    border.color: cell.chosen ? theme.accent : cell.playing ? cell.stationColor : theme.c.lighter_background

                    HoverHandler { id: hover }

                    // The station's colour down the left edge.
                    Rectangle {
                        anchors { left: parent.left; top: parent.top; bottom: parent.bottom; margins: 1 }
                        width: 5
                        radius: Math.min(2, theme.radius)
                        color: cell.stationColor
                    }

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 14
                        anchors.rightMargin: 10
                        anchors.topMargin: 8
                        anchors.bottomMargin: 8
                        spacing: 12

                        Image {
                            Layout.preferredWidth: 64
                            Layout.preferredHeight: 64
                            Layout.alignment: Qt.AlignVCenter
                            source: cell.artUrl
                            sourceSize: Qt.size(128, 128)
                            fillMode: Image.PreserveAspectCrop
                            asynchronous: true
                            smooth: true
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 6
                                Text {
                                    Layout.fillWidth: true
                                    text: cell.name
                                    elide: Text.ElideRight
                                    font.pixelSize: 15; font.bold: true
                                    color: theme.c.bright_foreground
                                }
                                Rectangle {
                                    visible: cell.playing && backend.state !== "stopped"
                                    Layout.preferredWidth: onAir.implicitWidth + 10
                                    Layout.preferredHeight: 16
                                    radius: Math.min(3, theme.radius)
                                    color: theme.c.red
                                    Text {
                                        id: onAir
                                        anchors.centerIn: parent
                                        text: backend.state === "playing" ? qsTr("ON AIR") : qsTr("PAUSED")
                                        font.pixelSize: 9; font.bold: true
                                        color: "white"
                                    }
                                }
                                Text {
                                    objectName: "favouriteStar_" + cell.key
                                    text: String.fromCodePoint(cell.favourite ? 0xF04CE : 0xF04D2)
                                    font.family: view.mono; font.pixelSize: 15
                                    color: cell.favourite ? theme.accent : theme.c.muted
                                    opacity: cell.favourite || hover.hovered || cell.chosen ? 1 : 0.4
                                    MouseArea {
                                        anchors.fill: parent; anchors.margins: -4
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: radio.toggleFavourite(cell.key)
                                    }
                                }
                            }
                            Text {
                                Layout.fillWidth: true
                                text: cell.description !== "" ? cell.description : cell.detail
                                elide: Text.ElideRight
                                font.pixelSize: 12
                                color: theme.c.light_foreground
                            }
                            // What it plays now: the feed for Nightride, the
                            // stream's own titles for the station playing.
                            Text {
                                objectName: "nowPlaying_" + cell.key
                                Layout.fillWidth: true
                                text: cell.nowPlaying !== "" ? String.fromCodePoint(0xF075A) + "  " + cell.nowPlaying
                                     : cell.playing ? qsTr("Tuning in…") : ""
                                elide: Text.ElideRight
                                font.pixelSize: 12
                                color: cell.playing ? theme.accent : theme.c.foreground
                            }
                        }
                    }

                    MouseArea {
                        anchors.fill: parent
                        z: -1
                        acceptedButtons: Qt.LeftButton | Qt.RightButton
                        onClicked: (mouse) => {
                            grid.currentIndex = cell.index
                            grid.forceActiveFocus()
                            if (mouse.button === Qt.RightButton) menu.popupFor(cell)
                            else view.tuneCurrent()
                        }
                    }
                }
            }

            Column {
                anchors.centerIn: parent
                width: Math.min(parent.width - 40, 420)
                visible: grid.count === 0
                spacing: 12
                Text {
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                    text: radio.group === "favourites"
                          ? qsTr("No favourites yet. Press F on a channel, or click its star.")
                          : qsTr("No stations of your own yet. Find one on radio-browser.info, or add a stream address.")
                    font.pixelSize: 14
                    color: theme.c.light_foreground
                }
            }
        }

        // The keys, in words.
        Text {
            Layout.fillWidth: true
            elide: Text.ElideRight
            text: qsTr("Enter or click: tune in   ·   F: favourite   ·   [ and ]: previous or next channel while one plays")
            font.pixelSize: 11
            color: theme.c.muted
        }
    }

    Menu {
        id: menu
        property string key: ""
        property bool builtin: true
        function popupFor(cell) {
            key = cell.key
            builtin = cell.isBuiltin
            popup()
        }
        MenuItem { text: qsTr("Tune in"); onTriggered: radio.tune(menu.key) }
        MenuItem { text: radio.isFavourite(menu.key) ? qsTr("Remove from favourites") : qsTr("Add to favourites")
                   onTriggered: radio.toggleFavourite(menu.key) }
        MenuItem { text: qsTr("Remove from My stations"); visible: !menu.builtin; height: visible ? implicitHeight : 0
                   onTriggered: radio.removeStation(menu.key) }
        MenuItem { text: qsTr("Save the stations as a list (M3U)…"); onTriggered: radio.exportList() }
        MenuItem { text: qsTr("Import stations from a list (PLS, M3U)…"); onTriggered: radio.importLists() }
    }
}
