import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Volumes: every disk, share and server the library's music lives on,
// whether it is connected now, and what can be done with it. Shares are
// looked at on a worker thread that gives up after 1.5 s, so a NAS that is
// asleep or gone never holds up the window.
ToolSheet {
    id: sheet
    objectName: "volumesSheet"
    readonly property var nas: library.nas
    title: qsTr("Volumes")
    subtitle: qsTr("Where the library's music lives")
    preferredWidth: 1120
    preferredHeight: 760
    signal confirm(string title, string text, string confirmText, var action)
    signal connectNas()
    signal addServer()

    function openSheet() {
        library.activate()
        nas.reload()
        open()
        list.forceActiveFocus()
    }
    function count(n) { return Number(n).toLocaleString(Qt.locale(), 'f', 0) }

    readonly property var qualities: [
        { label: qsTr("Original file"), bitrate: 0, format: "" },
        { label: qsTr("Opus, 192 kbit/s"), bitrate: 192, format: "opus" },
        { label: qsTr("Opus, 128 kbit/s"), bitrate: 128, format: "opus" },
        { label: qsTr("MP3, 320 kbit/s"), bitrate: 320, format: "mp3" },
        { label: qsTr("MP3, 192 kbit/s"), bitrate: 192, format: "mp3" }
    ]
    function qualityIndex(bitrate, format) {
        for (let i = 0; i < qualities.length; ++i)
            if (qualities[i].bitrate === bitrate && qualities[i].format === (format || "")) return i
        return 0
    }

    component Glyph: Text {
        Layout.preferredWidth: 28
        horizontalAlignment: Text.AlignHCenter
        font.family: "JetBrainsMono Nerd Font"
        font.pixelSize: 22
        color: theme.c.light_foreground
    }
    component SmallButton: Button {
        flat: true
        font.pixelSize: 12
        implicitHeight: 32
        topInset: 0; bottomInset: 0
        ToolTip.visible: hovered && ToolTip.text !== ""
        ToolTip.delay: 600
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 10

        RowLayout {
            Layout.fillWidth: true
            spacing: 10
            Text {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: qsTr("Shares are checked every 30 seconds, and whenever something is mounted or unmounted. One that doesn't answer shows as not connected: its tracks stay in the library, greyed, until it's back.")
                font.pixelSize: 12
                color: theme.c.muted
            }
            BusyIndicator {
                Layout.preferredWidth: 28
                Layout.preferredHeight: 28
                running: sheet.nas.checking
                visible: running
            }
            Button {
                objectName: "checkVolumes"
                text: sheet.nas.checking ? qsTr("Checking…") : qsTr("Check now")
                font.pixelSize: 13
                enabled: !sheet.nas.checking
                onClicked: sheet.nas.checkNow()
            }
        }

        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            contentWidth: availableWidth

            ColumnLayout {
                width: parent.width
                spacing: 8

                ListView {
                    id: list
                    objectName: "volumeList"
                    Layout.fillWidth: true
                    Layout.preferredHeight: contentHeight
                    interactive: false
                    spacing: 6
                    // Servers have their own rows below.
                    model: sheet.nas.volumes.filter(v => v.kind !== "subsonic")
                    delegate: Rectangle {
                        id: vol
                        required property var modelData
                        required property int index
                        readonly property bool remote: modelData.kind === "subsonic"
                        width: ListView.view.width
                        implicitHeight: volRow.implicitHeight + 20
                        radius: Math.min(4, theme.radius)
                        color: theme.c.darker_background
                        border.width: 1
                        border.color: theme.c.lighter_background
                        RowLayout {
                            id: volRow
                            anchors { fill: parent; leftMargin: 14; rightMargin: 10 }
                            spacing: 14
                            Glyph {
                                Layout.alignment: Qt.AlignTop
                                Layout.topMargin: 2
                                text: String.fromCodePoint(vol.remote ? 0xF015F : vol.modelData.kind === "network" ? 0xF048B : 0xF02CA)
                                color: vol.modelData.online ? theme.c.light_foreground : theme.c.muted
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 2
                                RowLayout {
                                    spacing: 8
                                    Text {
                                        text: vol.modelData.name
                                        font.pixelSize: 14
                                        font.bold: true
                                        color: theme.c.bright_foreground
                                    }
                                    Text {
                                        text: vol.modelData.kindText
                                        font.pixelSize: 12
                                        color: theme.c.muted
                                    }
                                }
                                Text {
                                    Layout.fillWidth: true
                                    visible: text !== ""
                                    elide: Text.ElideMiddle
                                    text: vol.remote || vol.modelData.root === vol.modelData.name ? ""
                                          : vol.modelData.root
                                            + (vol.modelData.kind === "network" && vol.modelData.source !== "" && vol.modelData.source !== vol.modelData.name
                                               ? "  ·  " + vol.modelData.source : "")
                                    font.pixelSize: 12
                                    color: theme.c.light_foreground
                                }
                                Text {
                                    Layout.fillWidth: true
                                    visible: !vol.remote && vol.modelData.roots.length > 0
                                             && !(vol.modelData.roots.length === 1 && vol.modelData.roots[0] === vol.modelData.root)
                                    elide: Text.ElideMiddle
                                    text: qsTr("Library folders: %1").arg(vol.modelData.roots.join(", "))
                                    font.pixelSize: 12
                                    color: theme.c.light_foreground
                                }
                                RowLayout {
                                    spacing: 6
                                    Rectangle {
                                        width: 8; height: 8; radius: 4
                                        color: vol.modelData.online ? theme.c.green : theme.c.red
                                    }
                                    Text {
                                        objectName: "volumeState"
                                        text: vol.modelData.stateText
                                        font.pixelSize: 12
                                        color: vol.modelData.online ? theme.c.green : theme.c.red
                                    }
                                    Text {
                                        text: "·  " + qsTr("%n tracks", "", vol.modelData.tracks)
                                              + (vol.modelData.offline > 0 ? "  ·  " + qsTr("%1 offline").arg(sheet.count(vol.modelData.offline)) : "")
                                              + (vol.modelData.missing > 0 ? "  ·  " + qsTr("%1 missing").arg(sheet.count(vol.modelData.missing)) : "")
                                              + (!vol.modelData.online && vol.modelData.lastSeen !== "" ? "  ·  " + qsTr("last seen %1").arg(vol.modelData.lastSeen) : "")
                                        font.pixelSize: 12
                                        color: theme.c.foreground
                                    }
                                }
                            }
                            // What can be done with it.
                            ColumnLayout {
                                Layout.alignment: Qt.AlignTop
                                spacing: 0
                                Row {
                                    Layout.alignment: Qt.AlignRight
                                    spacing: 2
                                    SmallButton {
                                        text: vol.remote ? qsTr("Sync now") : qsTr("Quick rescan")
                                        enabled: vol.modelData.online || vol.remote
                                        ToolTip.text: vol.remote ? qsTr("Fetch albums that are new or changed on the server")
                                                                 : qsTr("Look only in folders that changed")
                                        onClicked: sheet.nas.rescanVolume(vol.modelData.id, true)
                                    }
                                    SmallButton {
                                        text: vol.remote ? qsTr("Full sync") : qsTr("Rescan")
                                        enabled: vol.modelData.online || vol.remote
                                        ToolTip.text: vol.remote ? qsTr("Fetch every album again") : qsTr("Read every file's size and date again")
                                        onClicked: sheet.nas.rescanVolume(vol.modelData.id, false)
                                    }
                                    SmallButton {
                                        text: qsTr("Moved…")
                                        visible: !vol.remote
                                        ToolTip.text: qsTr("The music is somewhere else now: choose the folder, and Omamp points this volume there")
                                        onClicked: sheet.nas.reassignVolume(vol.modelData.id)
                                    }
                                    SmallButton {
                                        objectName: "forgetVolume"
                                        text: qsTr("Forget…")
                                        visible: !vol.remote
                                        enabled: !vol.modelData.online
                                        ToolTip.text: vol.modelData.online ? qsTr("Only a volume that isn't connected can be forgotten; rescan a connected one instead")
                                                                           : qsTr("Take this volume and its tracks out of the library")
                                        onClicked: {
                                            const v = vol.modelData
                                            sheet.confirm(qsTr("Forget %1?").arg(v.name),
                                                          qsTr("Its %n tracks leave the library, with their ratings and play counts. Playlists keep their entries. Nothing on the disk or the NAS is touched.", "", v.tracks),
                                                          qsTr("Forget"), () => sheet.nas.forgetVolume(v.id))
                                        }
                                    }
                                }
                                RowLayout {
                                    Layout.alignment: Qt.AlignRight
                                    visible: !vol.remote
                                    spacing: 6
                                    Text {
                                        text: qsTr("Folders read at once")
                                        font.pixelSize: 12
                                        color: theme.c.muted
                                    }
                                    SpinBox {
                                        from: 1; to: 32
                                        editable: true
                                        font.pixelSize: 12
                                        implicitHeight: 32
                                        value: vol.modelData.effectiveWorkers
                                        onValueModified: sheet.nas.setWorkers(vol.modelData.id, value)
                                        ToolTip.visible: hovered
                                        ToolTip.delay: 600
                                        ToolTip.text: vol.modelData.kind === "network"
                                                      ? qsTr("More helps over a network, where each folder costs a round trip; 8 is the default")
                                                      : qsTr("A local disk is limited by the disk; 4 is the default")
                                    }
                                }
                            }
                        }
                    }
                }
                Text {
                    Layout.fillWidth: true
                    visible: list.count === 0
                    text: qsTr("No volumes yet: add a folder of music to the library.")
                    font.pixelSize: 13
                    color: theme.c.muted
                }

                // Navidrome and other Subsonic servers.
                Text {
                    Layout.topMargin: 12
                    text: qsTr("Navidrome and Subsonic servers")
                    font.pixelSize: 14
                    font.bold: true
                    color: theme.c.bright_foreground
                }
                Text {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    text: qsTr("A server's library is copied into Omamp's as a list (no files are downloaded), so it can be searched, rated and put in playlists. Its tracks carry a cloud mark and stream from the server.")
                    font.pixelSize: 12
                    color: theme.c.muted
                }
                Repeater {
                    model: sheet.nas.servers
                    Rectangle {
                        id: srv
                        required property var modelData
                        Layout.fillWidth: true
                        implicitHeight: srvRow.implicitHeight + 20
                        radius: Math.min(4, theme.radius)
                        color: theme.c.darker_background
                        border.width: 1
                        border.color: theme.c.lighter_background
                        RowLayout {
                            id: srvRow
                            anchors { fill: parent; leftMargin: 14; rightMargin: 10 }
                            spacing: 14
                            Glyph { text: String.fromCodePoint(0xF015F) }
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 2
                                Text {
                                    text: srv.modelData.name + (srv.modelData.server !== "" ? "  ·  " + srv.modelData.server : "")
                                    font.pixelSize: 14
                                    font.bold: true
                                    color: theme.c.bright_foreground
                                }
                                Text {
                                    text: srv.modelData.url + "  ·  " + (srv.modelData.auth === "apikey" ? qsTr("API key") : qsTr("as %1").arg(srv.modelData.user))
                                    font.pixelSize: 12
                                    color: theme.c.light_foreground
                                }
                                Text {
                                    text: qsTr("%n tracks", "", srv.modelData.tracks) + "  ·  "
                                          + (srv.modelData.lastSync !== "" ? qsTr("synced %1").arg(srv.modelData.lastSync) : qsTr("never synced"))
                                    font.pixelSize: 12
                                    color: theme.c.foreground
                                }
                                Text {
                                    Layout.fillWidth: true
                                    visible: srv.modelData.lastError !== ""
                                    wrapMode: Text.Wrap
                                    text: qsTr("Last sync failed: %1").arg(srv.modelData.lastError)
                                    font.pixelSize: 12
                                    color: theme.c.red
                                }
                            }
                            ColumnLayout {
                                Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
                                spacing: 0
                                Row {
                                    Layout.alignment: Qt.AlignRight
                                    spacing: 2
                                    SmallButton { objectName: "syncServer"; text: qsTr("Sync now"); onClicked: sheet.nas.syncServer(srv.modelData.id, false) }
                                    SmallButton { text: qsTr("Full sync"); onClicked: sheet.nas.syncServer(srv.modelData.id, true) }
                                    SmallButton {
                                        text: qsTr("Remove…")
                                        onClicked: {
                                            const s = srv.modelData
                                            sheet.confirm(qsTr("Remove %1?").arg(s.name),
                                                          qsTr("Its %n tracks leave the library, and the saved password or key is deleted. The music on the server is not touched; adding the server again brings the tracks back.", "", s.tracks),
                                                          qsTr("Remove"), () => sheet.nas.removeServer(s.id, false))
                                        }
                                    }
                                }
                                RowLayout {
                                    Layout.alignment: Qt.AlignRight
                                    spacing: 6
                                    Text { text: qsTr("Stream as"); font.pixelSize: 12; color: theme.c.muted }
                                    ComboBox {
                                        Layout.preferredWidth: 190
                                        implicitHeight: 32; topInset: 0; bottomInset: 0
                                        font.pixelSize: 12
                                        model: sheet.qualities.map(q => q.label)
                                        currentIndex: sheet.qualityIndex(srv.modelData.maxBitrate, srv.modelData.format)
                                        onActivated: (i) => sheet.nas.setStreaming(srv.modelData.id, sheet.qualities[i].bitrate, sheet.qualities[i].format)
                                    }
                                }
                            }
                        }
                    }
                }
                Text {
                    visible: sheet.nas.servers.length === 0
                    text: qsTr("No servers yet.")
                    font.pixelSize: 13
                    color: theme.c.muted
                }
                Item { Layout.preferredHeight: 8 }
            }
        }
    }

    footerLeft: [
        Row {
            anchors.verticalCenter: parent.verticalCenter
            spacing: 8
            Button { objectName: "connectNas"; text: qsTr("Connect a NAS…"); font.pixelSize: 13; onClicked: sheet.connectNas() }
            Button { objectName: "addServer"; text: qsTr("Add Navidrome / Subsonic server…"); font.pixelSize: 13; onClicked: sheet.addServer() }
        }
    ]
    buttons: [
        PrimaryButton { text: qsTr("Done"); onClicked: sheet.close() }
    ]
}
