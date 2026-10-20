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
