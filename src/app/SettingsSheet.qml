import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Settings, on five pages: Playback (engine, crossfade, ReplayGain, the
// equaliser), Library (folders, watching, ratings in files), Scrobbling,
// Web, and Appearance (AppearancePage.qml: deck style, visualiser, big view). Everything applies as it is changed; playback settings go to
// the player's omamp.conf, the rest to the library.
ToolSheet {
    id: sheet
    objectName: "settingsSheet"
    title: qsTr("Settings")
    preferredWidth: 980
    preferredHeight: 772
    readonly property var s: library.appSettings
    // Re-read the library settings whenever they change.
    readonly property int rev: s.revision
    property string validateText: ""
    property bool validateOk: false
    property string flushText: ""
    property string lastFmText: ""
    property bool lastFmOk: false
    signal confirm(string title, string text, string confirmText, var action)
    signal openVolumes()

    function openPage(page) {
        library.activate()
        tabs.currentIndex = page || 0
        validateText = ""
        flushText = ""
        lastFmText = ""
        open()
        tabs.forceActiveFocus()
    }
    function text(key, fallback) { void rev; return String(s.value(key, fallback === undefined ? "" : fallback) || "") }
    function on(key, fallback) { void rev; return s.flag(key, fallback) }

    Connections {
        target: sheet.s
        function onValidated(ok, user, error) {
            sheet.validateOk = ok
            sheet.validateText = ok ? qsTr("Logged in as %1").arg(user) : error
            if (ok) tokenField.text = ""
        }
        function onFlushed(result) {
            if (result.error) sheet.flushText = result.error
            else if (result.offline) sheet.flushText = qsTr("Offline: nothing could be sent. The listens wait in the queue.")
            else sheet.flushText = qsTr("Sent %1; %2 refused; %3 still waiting.").arg(result.sent || 0).arg(result.failed || 0).arg(result.remaining || 0)
        }
        function onLastFmResult(step, ok, text) {
            sheet.lastFmOk = ok
            sheet.lastFmText = text
        }
    }

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
    component FieldLabel: Text {
        font.pixelSize: 13
        color: theme.c.light_foreground
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        TabBar {
            id: tabs
            objectName: "settingsTabs"
            Layout.fillWidth: true
            Layout.leftMargin: 12
            Layout.rightMargin: 12
            TabButton { text: qsTr("Playback"); font.pixelSize: 13 }
            TabButton { text: qsTr("Library"); font.pixelSize: 13 }
            TabButton { text: qsTr("Scrobbling"); font.pixelSize: 13 }
            TabButton { text: qsTr("Web"); font.pixelSize: 13 }
            TabButton { objectName: "appearanceTab"; text: qsTr("Appearance"); font.pixelSize: 13 }
        }
        Rectangle { Layout.fillWidth: true; height: 1; color: theme.c.lighter_background }

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: tabs.currentIndex

            // ---- Playback ------------------------------------------------
            ScrollView {
                clip: true
                contentWidth: availableWidth
                ColumnLayout {
                    width: parent.width - 40
                    x: 20
                    spacing: 4

                    Heading { text: qsTr("Engine") }
                    RowLayout {
                        spacing: 16
                        RadioButton {
                            objectName: "engineQt"
                            text: qsTr("Qt Multimedia")
                            font.pixelSize: 13
                            checked: backend.preferredEngine === "qt"
                            onClicked: backend.preferredEngine = "qt"
                        }
                        RadioButton {
                            objectName: "engineNative"
                            text: qsTr("Native: gapless, crossfade, ReplayGain, equaliser (experimental)")
                            font.pixelSize: 13
                            checked: backend.preferredEngine === "native"
                            onClicked: backend.preferredEngine = "native"
                        }
                    }
                    Note {
                        text: backend.preferredEngine !== backend.engineName
                              ? qsTr("Takes effect the next time Omamp starts. Playing now: %1.").arg(backend.engineName === "native" ? qsTr("native") : qsTr("Qt Multimedia"))
                              : backend.engineName === "native" ? qsTr("Playing through the native engine.")
                              : qsTr("The settings below are kept, and apply once the native engine plays.")
                    }

                    Heading { text: qsTr("Between tracks") }
                    RowLayout {
                        spacing: 12
                        FieldLabel { text: qsTr("Crossfade") }
                        Slider {
                            objectName: "crossfade"
                            Layout.preferredWidth: 320
                            from: 0; to: 12; stepSize: 0.5
                            value: backend.crossfadeSeconds
                            onMoved: backend.crossfadeSeconds = value
                        }
                        FieldLabel {
                            text: backend.crossfadeSeconds > 0
                                  ? qsTr("%1 s").arg(backend.crossfadeSeconds.toLocaleString(Qt.locale(), 'f', 1))
                                  : qsTr("Off: tracks join without a gap")
                        }
                    }
                    Note { text: qsTr("Tracks from the same album always join without a gap and are never crossfaded, so a live album or a DJ mix plays as one piece.") }

                    Heading { text: qsTr("Volume levelling (ReplayGain)") }
                    RowLayout {
                        spacing: 12
                        FieldLabel { text: qsTr("Use") }
                        ComboBox {
                            objectName: "replayGainMode"
                            Layout.preferredWidth: 220
                            implicitHeight: 34; topInset: 0; bottomInset: 0
                            font.pixelSize: 13
                            readonly property var modes: ["off", "track", "album"]
                            model: [qsTr("Off"), qsTr("Track gain"), qsTr("Album gain")]
                            currentIndex: Math.max(0, modes.indexOf(backend.replayGainMode))
                            onActivated: (i) => backend.replayGainMode = modes[i]
                        }
                        FieldLabel { text: qsTr("Preamp") }
                        Slider {
                            Layout.preferredWidth: 200
                            from: -15; to: 15; stepSize: 0.5
                            value: backend.replayGainPreamp
                            enabled: backend.replayGainMode !== "off"
                            onMoved: backend.replayGainPreamp = value
                        }
                        FieldLabel { text: qsTr("%1 dB").arg(backend.replayGainPreamp.toLocaleString(Qt.locale(), 'f', 1)) }
                        CheckBox {
                            text: qsTr("Prevent clipping")
                            font.pixelSize: 13
                            enabled: backend.replayGainMode !== "off"
                            checked: backend.clipPrevention
                            onClicked: backend.clipPrevention = checked
                            ToolTip.visible: hovered
                            ToolTip.delay: 600
                            ToolTip.text: qsTr("Lower the gain when a track's tagged peak would go over full scale")
                        }
                    }

                    RowLayout {
                        Layout.topMargin: 6
                        spacing: 12
                        Heading { Layout.fillWidth: false; Layout.topMargin: 0; text: qsTr("Equaliser") }
                        CheckBox {
                            objectName: "eqEnabled"
                            text: qsTr("On")
                            font.pixelSize: 13
                            checked: backend.eqEnabled
                            onClicked: backend.eqEnabled = checked
                        }
                        ComboBox {
                            Layout.preferredWidth: 220
                            implicitHeight: 34; topInset: 0; bottomInset: 0
                            font.pixelSize: 13
                            model: backend.eqPresets
                            displayText: qsTr("Presets")
                            onActivated: (i) => backend.applyEqPreset(backend.eqPresets[i])
                        }
                    }
                    // Preamp and ten bands, as vertical sliders.
                    Row {
                        Layout.topMargin: 4
                        spacing: 6
                        opacity: backend.eqEnabled ? 1 : 0.55
                        Repeater {
                            model: 11
                            Column {
                                required property int index
                                readonly property bool preamp: index === 0
                                readonly property real gain: preamp ? backend.eqPreamp : backend.eqBands[index - 1]
                                spacing: 2
                                width: preamp ? 64 : 50
                                Text {
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    text: (parent.gain > 0 ? "+" : "") + parent.gain.toLocaleString(Qt.locale(), 'f', 1)
                                    font.pixelSize: 11
                                    color: theme.c.foreground
                                }
                                Slider {
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    orientation: Qt.Vertical
                                    height: 150
                                    from: -12; to: 12; stepSize: 0.5
                                    value: parent.gain
                                    onMoved: parent.preamp ? backend.eqPreamp = value : backend.setEqBand(parent.index - 1, value)
                                }
                                Text {
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    text: {
                                        if (parent.preamp) return qsTr("Preamp")
                                        const f = backend.eqFrequencies[parent.index - 1]
                                        return f >= 1000 ? (f / 1000) + "k" : String(Math.round(f))
                                    }
                                    font.pixelSize: 11
                                    font.bold: parent.preamp
                                    color: theme.c.light_foreground
                                }
                            }
                        }
                    }
                    Note { text: qsTr("Changes are heard within half a second.") }
                    Item { height: 12; width: 1 }
                }
            }

            // ---- Library -------------------------------------------------
            ScrollView {
                clip: true
                contentWidth: availableWidth
                ColumnLayout {
                    width: parent.width - 40
                    x: 20
                    spacing: 4

                    Heading { text: qsTr("Folders") }
                    Repeater {
                        model: { void sheet.rev; return sheet.s.folders() }
                        RowLayout {
                            required property string modelData
                            Layout.fillWidth: true
                            Text {
                                Layout.fillWidth: true
                                elide: Text.ElideMiddle
                                text: modelData
                                font.pixelSize: 13
                                color: theme.c.foreground
                            }
                            Button {
                                text: qsTr("Remove…")
                                flat: true
                                font.pixelSize: 13
                                implicitHeight: 34; topInset: 0; bottomInset: 0
                                onClicked: {
                                    const folder = modelData
                                    sheet.confirm(qsTr("Remove the folder from the library?"),
                                                  qsTr("Tracks in %1 leave the library, with their ratings and play counts. The files stay on disk, and adding the folder again brings the tracks back.").arg(folder),
                                                  qsTr("Remove"), () => sheet.s.removeFolder(folder))
                                }
                            }
                        }
                    }
                    Note { visible: sheet.s.folders().length === 0; text: qsTr("No folders yet.") }
                    RowLayout {
                        Button { text: qsTr("Add a folder…"); font.pixelSize: 13; onClicked: library.addFolder() }
                        Button { text: qsTr("Rescan now"); flat: true; font.pixelSize: 13; enabled: !library.busy; onClicked: library.rescan() }
                        Button {
                            objectName: "openVolumes"
                            text: qsTr("Volumes, NAS shares and servers…")
                            flat: true
                            font.pixelSize: 13
                            onClicked: { sheet.close(); sheet.openVolumes() }
                        }
                    }

                    Heading { text: qsTr("Watching for changes") }
                    CheckBox {
                        objectName: "watchEnabled"
                        text: qsTr("Watch the folders and pick up changes by themselves")
                        font.pixelSize: 13
                        checked: sheet.on("watch.enabled", true)
                        onClicked: sheet.s.setFlag("watch.enabled", checked)
                    }
                    RowLayout {
                        spacing: 12
                        FieldLabel { text: qsTr("Check network shares every") }
                        SpinBox {
                            from: 0; to: 1440; editable: true
                            font.pixelSize: 13
                            value: Number(sheet.text("watch.network_minutes", 15))
                            onValueModified: sheet.s.setValue("watch.network_minutes", value)
                        }
                        FieldLabel { text: qsTr("minutes (0: never)") }
                    }
                    Note { text: qsTr("Local disks are watched as they change. Shares on a NAS can't tell Omamp about changes made by other machines, so they are checked on this schedule, by file size and date only.") }

                    Heading { text: qsTr("Ratings and play counts") }
                    CheckBox {
                        text: qsTr("Write ratings into the music files too")
                        font.pixelSize: 13
                        checked: sheet.on("tags.write_ratings", false)
                        onClicked: sheet.s.setFlag("tags.write_ratings", checked)
                    }
                    Note { text: qsTr("Off: ratings and play counts stay in Omamp's library, so playing music never rewrites files on a NAS.") }
                    Item { height: 12; width: 1 }
                }
            }

            // ---- Scrobbling ----------------------------------------------
            ScrollView {
                clip: true
                contentWidth: availableWidth
                ColumnLayout {
                    width: parent.width - 40
                    x: 20
                    spacing: 4
                    readonly property var status: { void sheet.rev; return sheet.s.scrobbleStatus() }
                    readonly property var lb: status.find((x) => x.service === "listenbrainz") || ({})
                    readonly property var fm: status.find((x) => x.service === "lastfm") || ({})

                    CheckBox {
                        text: qsTr("Send what I listen to")
                        font.pixelSize: 13
                        checked: sheet.on("scrobble.enabled", true)
                        onClicked: sheet.s.setFlag("scrobble.enabled", checked)
                    }
                    Note { text: qsTr("A listen counts after half the track or four minutes. Listens wait in a queue while you're offline.") }

                    Heading { text: qsTr("ListenBrainz") }
                    Note {
                        text: parent.lb.configured
                              ? qsTr("Logged in as %1. %2 waiting, %3 sent, %4 refused.").arg(parent.lb.user || qsTr("(unknown)"))
                                    .arg(parent.lb.pending || 0).arg(parent.lb.sent || 0).arg(parent.lb.failed || 0)
                              : qsTr("Paste your user token from listenbrainz.org/settings.")
                        color: theme.c.foreground
                    }
                    RowLayout {
                        spacing: 8
                        TextField {
                            id: tokenField
                            objectName: "listenBrainzToken"
                            Layout.preferredWidth: 420
                            placeholderText: qsTr("User token")
                            echoMode: TextInput.Password
                            font.pixelSize: 13
                        }
                        Button {
                            text: sheet.s.validating ? qsTr("Checking…") : qsTr("Check and save")
                            font.pixelSize: 13
                            enabled: tokenField.text.trim() !== "" && !sheet.s.validating
                            onClicked: sheet.s.validateListenBrainz(tokenField.text)
                        }
                        Button {
                            text: qsTr("Log out")
                            flat: true
                            font.pixelSize: 13
                            visible: parent.parent.lb.configured === true
                            onClicked: sheet.s.forgetListenBrainz()
                        }
                    }
                    Text {
                        visible: sheet.validateText !== ""
                        text: sheet.validateText
                        font.pixelSize: 13
                        color: sheet.validateOk ? theme.c.green : theme.c.red
                    }
                    CheckBox {
                        text: qsTr("Send to ListenBrainz")
                        font.pixelSize: 13
                        checked: sheet.on("scrobble.listenbrainz", true)
                        onClicked: sheet.s.setFlag("scrobble.listenbrainz", checked)
                    }

                    Heading { text: qsTr("Last.fm") }
                    Note {
                        objectName: "lastFmStatus"
                        text: parent.fm.configured
                              ? qsTr("Logged in as %1. %2 waiting, %3 sent, %4 refused.").arg(parent.fm.user || qsTr("(unknown)"))
                                    .arg(parent.fm.pending || 0).arg(parent.fm.sent || 0).arg(parent.fm.failed || 0)
                              : qsTr("Last.fm asks each app for its own API account. Make one at last.fm/api/account/create (it's free), paste its key and shared secret here, then log in.")
                        color: theme.c.foreground
                    }
                    GridLayout {
                        visible: !parent.fm.configured
                        columns: 2
                        columnSpacing: 12
                        rowSpacing: 4
                        FieldLabel { text: qsTr("API key") }
                        TextField {
                            objectName: "lastFmKey"
                            Layout.preferredWidth: 420
                            placeholderText: qsTr("API key")
                            font.pixelSize: 13
                            text: sheet.text("lastfm.api_key")
                            onEditingFinished: sheet.s.setValue("lastfm.api_key", text.trim())
                        }
                        FieldLabel { text: qsTr("Shared secret") }
                        TextField {
                            objectName: "lastFmSecret"
                            Layout.preferredWidth: 420
                            placeholderText: qsTr("Shared secret")
                            echoMode: TextInput.Password
                            font.pixelSize: 13
                            text: sheet.text("lastfm.api_secret")
                            onEditingFinished: sheet.s.setValue("lastfm.api_secret", text.trim())
                        }
                    }
                    RowLayout {
                        spacing: 8
                        Button {
                            objectName: "lastFmLogin"
                            visible: !parent.parent.fm.configured
                            text: sheet.s.lastFmBusy ? qsTr("Asking Last.fm…") : sheet.s.lastFmWaiting ? qsTr("Start again") : qsTr("Log in to Last.fm")
                            font.pixelSize: 13
                            enabled: !sheet.s.lastFmBusy
                            onClicked: sheet.s.beginLastFm()
                        }
                        Button {
                            objectName: "lastFmFinish"
                            visible: !parent.parent.fm.configured && sheet.s.lastFmWaiting
                            text: qsTr("Finish logging in")
                            font.pixelSize: 13
                            highlighted: true
                            enabled: !sheet.s.lastFmBusy
                            onClicked: sheet.s.finishLastFm()
                        }
                        Button {
                            visible: !parent.parent.fm.configured && sheet.s.lastFmWaiting && sheet.s.lastFmAuthUrl() !== ""
                            text: qsTr("Open the page again")
                            flat: true
                            font.pixelSize: 13
                            onClicked: Qt.openUrlExternally(sheet.s.lastFmAuthUrl())
                        }
                        Button {
                            text: qsTr("Log out")
                            flat: true
                            font.pixelSize: 13
                            visible: parent.parent.fm.configured === true
                            onClicked: sheet.s.forgetLastFm()
                        }
                    }
                    Text {
                        objectName: "lastFmMessage"
                        Layout.fillWidth: true
                        visible: sheet.lastFmText !== ""
                        wrapMode: Text.Wrap
                        text: sheet.lastFmText
                        font.pixelSize: 13
                        color: sheet.lastFmOk ? theme.c.green : theme.c.red
                    }
                    CheckBox {
                        text: qsTr("Send to Last.fm")
                        font.pixelSize: 13
                        checked: sheet.on("scrobble.lastfm", true)
                        onClicked: sheet.s.setFlag("scrobble.lastfm", checked)
                    }

                    Heading { text: qsTr("The queue") }
                    Note {
                        objectName: "queueStatus"
                        text: {
                            const lines = []
                            for (const x of parent.status) {
                                const name = x.service === "lastfm" ? "Last.fm" : "ListenBrainz"
                                let line = qsTr("%1: %2 waiting, %3 sent, %4 refused").arg(name).arg(x.pending || 0).arg(x.sent || 0).arg(x.failed || 0)
                                if (x.lastError) line += "  ·  " + qsTr("last error: %1").arg(x.lastError)
                                lines.push(line)
                            }
                            return lines.join("\n")
                        }
                        color: theme.c.foreground
                    }
                    RowLayout {
                        spacing: 10
                        Button {
                            objectName: "flushNow"
                            text: sheet.s.flushing ? qsTr("Sending…") : qsTr("Send now")
                            font.pixelSize: 13
                            enabled: !sheet.s.flushing
                            onClicked: { sheet.flushText = ""; sheet.s.flushScrobbles() }
                        }
                        Text {
                            objectName: "flushResult"
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            text: sheet.flushText
                            font.pixelSize: 13
                            color: theme.c.foreground
                        }
                    }
                    Note { text: qsTr("Omamp sends the queue after each counted play and when it starts; this sends it now.") }
                    Item { height: 12; width: 1 }
                }
            }

            // ---- Web -----------------------------------------------------
            ScrollView {
                clip: true
                contentWidth: availableWidth
                ColumnLayout {
                    width: parent.width - 40
                    x: 20
                    spacing: 4

                    Heading { text: qsTr("AcoustID") }
                    Note { text: qsTr("Identifying tracks by their sound needs an application key from acoustid.org/new-application (free for non-commercial use). Without one, Identify matches by tags and MusicBrainz IDs only.") }
                    TextField {
                        objectName: "acoustidKey"
                        Layout.preferredWidth: 420
                        placeholderText: qsTr("Application key")
                        font.pixelSize: 13
                        text: sheet.text("acoustid.key")
                        onEditingFinished: sheet.s.setValue("acoustid.key", text.trim())
                    }

                    Heading { text: qsTr("MusicBrainz") }
                    GridLayout {
                        columns: 2
                        columnSpacing: 12
                        rowSpacing: 4
                        FieldLabel { text: qsTr("Preferred release countries") }
                        TextField {
                            Layout.preferredWidth: 260
                            placeholderText: qsTr("AU, GB, US")
                            font.pixelSize: 13
                            text: sheet.text("identify.countries")
                            onEditingFinished: sheet.s.setValue("identify.countries", text.trim())
                        }
                        FieldLabel { text: qsTr("Preferred formats") }
                        TextField {
                            Layout.preferredWidth: 260
                            placeholderText: qsTr("CD, Digital Media")
                            font.pixelSize: 13
                            text: sheet.text("identify.formats")
                            onEditingFinished: sheet.s.setValue("identify.formats", text.trim())
                        }
                    }
                    CheckBox {
                        text: qsTr("Write genres from MusicBrainz")
                        font.pixelSize: 13
                        checked: sheet.on("identify.write_genres", false)
                        onClicked: sheet.s.setFlag("identify.write_genres", checked)
                    }
                    CheckBox {
                        text: qsTr("Write the record label and catalogue number")
                        font.pixelSize: 13
                        checked: sheet.on("identify.write_label", true)
                        onClicked: sheet.s.setFlag("identify.write_label", checked)
                    }

                    Heading { text: qsTr("Covers") }
                    RowLayout {
                        spacing: 12
                        FieldLabel { text: qsTr("Cover size to fetch") }
                        ComboBox {
                            objectName: "coverSize"
                            Layout.preferredWidth: 200
                            implicitHeight: 34; topInset: 0; bottomInset: 0
                            font.pixelSize: 13
                            readonly property var sizes: [250, 500, 1200]
                            model: [qsTr("250 pixels"), qsTr("500 pixels"), qsTr("1200 pixels")]
                            currentIndex: { const i = sizes.indexOf(Number(sheet.text("coverart.size", 500))); return i < 0 ? 1 : i }
                            onActivated: (i) => { sheet.s.setValue("coverart.size", sizes[i]); sheet.s.setValue("identify.cover_size", sizes[i]) }
                        }
                    }
                    CheckBox {
                        text: qsTr("Embed fetched covers in the files")
                        font.pixelSize: 13
                        checked: sheet.on("identify.cover_embed", true)
                        onClicked: sheet.s.setFlag("identify.cover_embed", checked)
                    }
                    CheckBox {
                        text: qsTr("Also save cover.jpg in the album's folder")
                        font.pixelSize: 13
                        checked: sheet.on("identify.cover_file", false)
                        onClicked: sheet.s.setFlag("identify.cover_file", checked)
                    }
                    Item { height: 12; width: 1 }
                }
            }

            // ---- Appearance (page 4) ---------------------------------------
            AppearancePage {}
        }
    }

    footerLeft: [
        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: qsTr("Changes apply at once.")
            font.pixelSize: 12
            color: theme.c.muted
        }
    ]
    buttons: [
        PrimaryButton { text: qsTr("Done"); onClicked: sheet.close() }
    ]
}
