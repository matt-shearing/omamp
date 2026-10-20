import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Settings > Spotify: the switch, which program talks to Spotify, where it
// stands, and the sign-in. Omamp never asks for a Spotify password: Sign in
// runs the backend's own sign-in (spotify_player authenticate) in a
// terminal, and the backend keeps what it gets. docs/spotify.md.
ScrollView {
    id: page
    objectName: "spotifyPage"
    clip: true
    contentWidth: availableWidth
    ScrollBar.vertical: ScrollBar { objectName: "spotifyPageScroll"; parent: page; x: page.width - width; height: page.availableHeight }

    component Heading: Text {
        Layout.fillWidth: true
        Layout.topMargin: 10
        font.pixelSize: 14
        font.bold: true
        color: theme.c.bright_foreground
    }
    component Note: Text {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        font.pixelSize: 12
        color: theme.c.muted
    }
    component Fact: RowLayout {
        property string label
        property string value
        property color valueColor: theme.c.foreground
        spacing: 12
        Text { Layout.preferredWidth: 130; text: parent.label; font.pixelSize: 13; color: theme.c.light_foreground }
        Text { Layout.fillWidth: true; text: parent.value; font.pixelSize: 13; color: parent.valueColor; wrapMode: Text.Wrap }
    }

    // Two separate choices in one column: radio buttons with the same
    // parent would otherwise uncheck each other.
    ButtonGroup { id: backendGroup }
    ButtonGroup { id: deviceGroup }

    ColumnLayout {
        width: parent.width - 40
        x: 20
        spacing: 4

        Heading { text: qsTr("Spotify") }
        Switch {
            objectName: "spotifyEnabled"
            text: qsTr("Show Spotify in Omamp")
            font.pixelSize: 13
            checked: spotify.enabled
            onToggled: spotify.enabled = checked
        }
        Note {
            text: qsTr("Adds Spotify to the library tree, and Spotify tracks can sit in Now Playing beside your own. When a Spotify track's turn comes, Spotify plays it and Omamp follows. Playing needs Spotify Premium; a Free account can browse and search.")
        }

        Heading { text: qsTr("How Omamp reaches Spotify"); enabled: spotify.enabled }
        RadioButton {
            objectName: "backend_spotify_player"
            ButtonGroup.group: backendGroup
            enabled: spotify.enabled
            text: qsTr("spotify_player (recommended): your playlists, Liked Songs, albums, artists, search and playback")
            font.pixelSize: 13
            checked: spotify.backendId === "spotify_player"
            onClicked: spotify.backendId = "spotify_player"
        }
        RadioButton {
            objectName: "backend_spotifast"
            ButtonGroup.group: backendGroup
            enabled: spotify.enabled
            text: qsTr("Spotifast, while it is open: plays Spotify links in Now Playing, pauses and seeks; no search or library")
            font.pixelSize: 13
            checked: spotify.backendId === "spotifast"
            onClicked: spotify.backendId = "spotifast"
        }

        Heading { text: qsTr("Status"); enabled: spotify.enabled }
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: statusColumn.implicitHeight + 24
            radius: Math.min(6, theme.radius)
            color: theme.c.dark_background
            border.width: 1
            border.color: theme.c.lighter_background
            opacity: spotify.enabled ? 1 : 0.6
            ColumnLayout {
                id: statusColumn
                anchors { left: parent.left; right: parent.right; top: parent.top; margins: 12 }
                spacing: 6
                Text {
                    objectName: "spotifyStatusText"
                    Layout.fillWidth: true
                    text: spotify.statusText
                    wrapMode: Text.Wrap
                    font.pixelSize: 13
                    color: theme.c.bright_foreground
                }
                Fact {
                    label: qsTr("Program")
                    value: spotify.installed ? qsTr("%1, installed").arg(spotify.backendName)
                                             : qsTr("%1, not installed").arg(spotify.backendName)
                    valueColor: spotify.installed ? theme.c.foreground : theme.c.red
                }
                Fact {
                    visible: spotify.backendId === "spotify_player"
                    label: qsTr("Signed in")
                    value: spotify.signedIn ? qsTr("Yes") : qsTr("No")
                    valueColor: spotify.signedIn ? theme.c.green : theme.c.yellow
                }
                Fact {
                    label: qsTr("Running")
                    value: spotify.running ? qsTr("Yes") : qsTr("No")
                    valueColor: spotify.running ? theme.c.green : theme.c.yellow
                }
                Fact {
                    visible: spotify.account !== ""
                    label: qsTr("Account")
                    value: spotify.account
                }
                Fact {
                    label: qsTr("Premium")
                    value: spotify.premium === "yes" ? qsTr("Yes: playback works")
                         : spotify.premium === "no" ? qsTr("No: browse and search only")
                         : qsTr("Not known yet")
                    valueColor: spotify.premium === "yes" ? theme.c.green : spotify.premium === "no" ? theme.c.yellow : theme.c.muted
                }
                Fact {
                    objectName: "spotifyFollowMode"
                    visible: spotify.enabled
                    label: qsTr("Follows playback")
                    value: spotify.followMode === "mpris" ? qsTr("Through MPRIS, without asking Spotify")
                         : spotify.followMode === "polling" ? qsTr("By asking Spotify every 5 seconds while a Spotify track plays in Omamp (no MPRIS from %1)").arg(spotify.backendName)
                         : qsTr("Not yet")
                }
                Fact {
                    label: qsTr("Plays on")
                    value: spotify.device !== "" ? spotify.device : qsTr("“%1” on this computer").arg(spotify.deviceName)
                }
            }
        }

        RowLayout {
            Layout.topMargin: 6
            spacing: 8
            enabled: spotify.enabled
            PrimaryButton {
                objectName: "spotifySignIn"
                visible: spotify.backendId === "spotify_player" && !spotify.signedIn && spotify.installed
                text: qsTr("Sign in…")
                onClicked: spotify.signIn()
            }
            Button {
                objectName: "spotifyStart"
                visible: spotify.installed && !spotify.running && (spotify.signedIn || spotify.backendId === "spotifast")
                text: spotify.backendId === "spotifast" ? qsTr("Open Spotifast") : qsTr("Start spotify_player")
                font.pixelSize: 13
                onClicked: spotify.startBackend()
            }
            Button {
                objectName: "spotifyCheck"
                text: spotify.checking ? qsTr("Checking…") : qsTr("Check again")
                enabled: !spotify.checking
                font.pixelSize: 13
                onClicked: spotify.refreshStatus()
            }
        }
        Note {
            visible: spotify.backendId === "spotify_player" && !spotify.signedIn
            text: qsTr("Sign in opens a terminal running “spotify_player authenticate”. It opens your browser, where Spotify asks you to allow it, then asks a second time for playback. spotify_player keeps the sign-in in ~/.cache/spotify-player; Omamp only checks that it is there.")
        }
        Note {
            visible: spotify.waitingForSignIn
            text: qsTr("Waiting for the sign-in to finish…")
            color: theme.accent
        }

        // ---- Your own client ID ----------------------------------------------
        Heading {
            text: qsTr("Use your own Spotify app (recommended)")
            visible: spotify.backendId === "spotify_player"
        }
        ColumnLayout {
            objectName: "spotifyOwnApp"
            Layout.fillWidth: true
            visible: spotify.backendId === "spotify_player"
            spacing: 6

            Note {
                text: qsTr("spotify_player reads your library and starts playback through Spotify's Web API, which identifies every app by a client ID. Spotify limits how often one client ID may ask, and the ID spotify_player uses is shared by many people, so at busy times Spotify answers “too many requests” and lists take a long time or fail. With an app of your own the limit is yours alone. It is free and takes a few minutes.")
            }
            Text {
                objectName: "spotifyClientIdState"
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                font.pixelSize: 13
                color: spotify.clientIdKind === "own" ? theme.c.green : theme.c.yellow
                text: spotify.clientIdKind === "own" ? qsTr("spotify_player uses your own client ID (%1).").arg(spotify.clientId)
                    : spotify.clientIdKind === "desktop" ? qsTr("spotify_player's app.toml sets client_id to Spotify's own desktop app ID, which every player built on librespot shares. Expect rate limits.")
                    : spotify.clientIdKind === "command" ? qsTr("spotify_player takes its client ID from client_id_command in app.toml. Omamp leaves that alone.")
                    : qsTr("spotify_player uses its built-in client ID (ncspot's), which many people share. Expect rate limits at busy times.")
            }
            Text {
                Layout.fillWidth: true
                Layout.topMargin: 4
                wrapMode: Text.Wrap
                textFormat: Text.StyledText
                font.pixelSize: 13
                color: theme.c.foreground
                linkColor: theme.accent
                onLinkActivated: (link) => Qt.openUrlExternally(link)
                text: qsTr("1. Open <a href=\"https://developer.spotify.com/dashboard\">developer.spotify.com/dashboard</a> and sign in with your Spotify account.<br>"
                         + "2. Press <b>Create app</b>. Any name and description will do, for example “Omamp”.<br>"
                         + "3. Under <b>Redirect URIs</b>, add exactly: http://127.0.0.1:8989/login<br>"
                         + "4. Where it asks which APIs you will use, tick <b>Web API</b>. Accept the terms and press <b>Save</b>.<br>"
                         + "5. Open the app's <b>Settings</b> and copy its <b>Client ID</b>.<br>"
                         + "6. Paste it below and press <b>Use this client ID</b>.<br>"
                         + "7. Sign in again when Omamp asks: spotify_player needs your permission for the new app.")
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                TextField {
                    id: redirectField
                    Layout.preferredWidth: 260
                    readOnly: true
                    selectByMouse: true
                    text: "http://127.0.0.1:8989/login"
                    font.pixelSize: 12
                }
                Button {
                    text: qsTr("Copy the redirect URI")
                    font.pixelSize: 12
                    onClicked: { redirectField.selectAll(); redirectField.copy(); redirectField.deselect() }
                }
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                enabled: spotify.clientIdKind !== "command" && !spotify.clientIdBusy
                TextField {
                    id: clientIdField
                    objectName: "spotifyClientIdField"
                    Layout.fillWidth: true
                    placeholderText: qsTr("Client ID (32 letters and digits)")
                    font.pixelSize: 13
                    selectByMouse: true
                    onAccepted: useButton.clicked()
                }
                PrimaryButton {
                    id: useButton
                    objectName: "spotifyUseClientId"
                    text: spotify.clientIdBusy ? qsTr("Saving…") : qsTr("Use this client ID")
                    enabled: clientIdField.text.trim().length > 0
                    onClicked: if (spotify.useOwnClientId(clientIdField.text.trim())) clientIdField.text = ""
                }
            }
            Note {
                text: qsTr("Omamp writes the ID into %1, after keeping a copy of the file beside it, and changes nothing else there. It then stops spotify_player, because it needs a new sign-in for the new app; signing in starts it again.").arg(spotify.appConfigPath)
            }
            Text {
                objectName: "spotifyClientIdMessage"
                Layout.fillWidth: true
                visible: spotify.clientIdMessage !== ""
                wrapMode: Text.Wrap
                font.pixelSize: 13
                color: theme.accent
                text: spotify.clientIdMessage
            }
        }

        Heading {
            text: qsTr("Play on")
            visible: spotify.enabled && spotify.devices.length > 0
        }
        Repeater {
            model: spotify.enabled ? spotify.devices : []
            RadioButton {
                required property var modelData
                ButtonGroup.group: deviceGroup
                text: modelData.name + (modelData.type ? "  (" + modelData.type + ")" : "")
                font.pixelSize: 13
                checked: modelData.active
                onClicked: spotify.connectDevice(modelData.id)
            }
        }

        Heading { text: qsTr("Spotify beside your library"); enabled: spotify.enabled }
        CheckBox {
            objectName: "spotifyBlendSearch"
            enabled: spotify.enabled && spotify.canBrowse
            text: qsTr("Show Spotify results under the library search")
            font.pixelSize: 13
            checked: spotify.blendSearch
            onClicked: spotify.blendSearch = checked
        }
        CheckBox {
            objectName: "spotifyBlendPalette"
            enabled: spotify.enabled && spotify.canBrowse
            text: qsTr("Show Spotify results in the command palette (Ctrl+K)")
            font.pixelSize: 13
            checked: spotify.blendPalette
            onClicked: spotify.blendPalette = checked
        }
        CheckBox {
            objectName: "spotifyStartDaemon"
            enabled: spotify.enabled && spotify.backendId === "spotify_player"
            text: qsTr("Start spotify_player in the background when Omamp needs it")
            font.pixelSize: 13
            checked: spotify.startDaemon
            onClicked: spotify.startDaemon = checked
        }

        Heading { text: qsTr("What crosses over, and what doesn't"); enabled: spotify.enabled }
        Note {
            text: qsTr("The deck, the strip, the big view, the mini players, media keys and the Omarchy bar show a Spotify track with its cover and position. Gapless joins, crossfade, ReplayGain, the equaliser and the visualiser work on your own files only: Spotify's sound never passes through Omamp, and its volume is Spotify's.")
        }
        Item { height: 12; width: 1 }
    }
}
