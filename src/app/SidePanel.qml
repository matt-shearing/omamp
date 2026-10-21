import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// The sidebar on the right of the library, as Spotify's desktop app has
// one: the cover, the track, its lyrics, about the artist and the album,
// credits, what plays next and the track's details. Each section folds
// away, moves up or down, or hides, from the ⋮ menu or Settings >
// Appearance > Layout (appearance.layout keeps the choices).
//
// It follows the track under the cursor (or the album chosen in the grid),
// and the playing one when nothing is chosen; "Follow playing" makes it
// show the playing track only. A Spotify track, a radio station and a file
// outside the library show what the player knows of them.
Rectangle {
    id: panel
    objectName: "infoPanel"
    color: theme.c.dark_background

    readonly property var layoutPrefs: appearance.layout
    // Set by the view: the track to show, or 0 for the one playing.
    property var chosenTrack: 0
    // What the chosen track stands for, when it isn't simply a track.
    property string chosenHeading: ""
    // The playing track: by its file, or (a Navidrome / Subsonic track) by
    // the id Now Playing carries.
    readonly property var playingId: library.playingTrackId > 0 ? library.playingTrackId : nowPlayingArt.trackId
    readonly property bool followPlaying: layoutPrefs.follow === "playing"
    readonly property bool usesChosen: !followPlaying && chosenTrack > 0
    readonly property var trackId: usesChosen ? chosenTrack : playingId
    readonly property bool showingPlaying: !usesChosen && playingId > 0
    // The queue entry playing: a file, spotify://track/…, radio://…
    readonly property var playingEntry: { void backend.playlist.count; return backend.currentIndex >= 0 ? backend.trackAt(backend.currentIndex) : ({}) }
    readonly property string playingPath: playingEntry.path || ""
    readonly property bool radioPlaying: playingPath.startsWith("radio://")
    readonly property bool spotifyPlaying: playingPath.startsWith("spotify:")
    // Something plays that isn't in the library: its cover and tags still show.
    readonly property bool playingOutside: !usesChosen && !(playingId > 0) && backend.currentIndex >= 0
    // Reads again whenever the library changes (library.revision).
    readonly property var info: trackId > 0 && library.revision >= 0 ? library.trackInfo(trackId) : ({})
    readonly property bool hasTrack: info.id !== undefined
    readonly property bool hasSubject: hasTrack || playingOutside
    // A radio song: "Artist – Title" from the station.
    readonly property var radioSong: {
        const line = radio.nowPlaying || ""
        const at = line.indexOf(" – ")
        return at > 0 ? { artist: line.slice(0, at), title: line.slice(at + 3) } : { artist: "", title: line }
    }
    readonly property string shownTitle: hasTrack ? (info.title || "") : radioPlaying ? (radioSong.title || backend.title) : backend.title
    readonly property string shownArtist: hasTrack ? (info.artist || "") : radioPlaying ? radioSong.artist : backend.artist
    readonly property string shownAlbum: hasTrack ? (info.album || "") : radioPlaying ? "" : backend.album

    // Compatibility with the info panel: page 1 shows the lyrics.
    property int page: 0
    onPageChanged: if (page === 1) showLyrics()
    function showLyrics() {
        layoutPrefs.setSectionShown("lyrics", true)
        layoutPrefs.setCollapsed("lyrics", false)
        Qt.callLater(() => {
            const s = sectionItem("lyrics")
            if (s) flick.contentY = Math.max(0, Math.min(s.mapToItem(content, 0, 0).y, flick.contentHeight - flick.height))
        })
        page = 0
    }
    function sectionItem(id) {
        for (let i = 0; i < sectionRepeater.count; ++i) {
            const l = sectionRepeater.itemAt(i)
            if (l && l.sectionId === id) return l
        }
        return null
    }
    signal editLyrics(var trackId)
    signal openBigView()

    function clock(ms) {
        if (!(ms > 0)) return ""
        const total = Math.floor(ms / 1000)
        const h = Math.floor(total / 3600), m = Math.floor(total % 3600 / 60), s = total % 60
        return (h > 0 ? h + ":" + (m < 10 ? "0" : "") : "") + m + ":" + (s < 10 ? "0" : "") + s
    }
    function day(seconds) {
        if (!(seconds > 0)) return ""
        const language = translator.language
        return library.formatDate(seconds)
    }
    function gain(db, peak) {
        if (db === undefined || db === null) return ""
        let text = (db > 0 ? "+" : db < 0 ? "−" : "") + Math.abs(db).toLocaleString(Qt.locale(), 'f', 2) + " dB"
        if (peak !== undefined && peak !== null) text += qsTr(", peak %1").arg(Number(peak).toLocaleString(Qt.locale(), 'f', 3))
        return text
    }
    function formatLine(i) {
        const parts = []
        if (i.format) parts.push(i.format)
        if (i.sampleRate > 0) parts.push((i.sampleRate / 1000).toLocaleString(Qt.locale(), 'f', i.sampleRate % 1000 ? 1 : 0) + " kHz")
        if (i.channels === 1) parts.push(qsTr("mono"))
        else if (i.channels === 2) parts.push(qsTr("stereo"))
        else if (i.channels > 2) parts.push(qsTr("%n channels", "", i.channels))
        if (i.bitrate > 0) parts.push(i.bitrate + " kbps")
        return parts.join("  ·  ")
    }
    function size(bytes) {
        if (!(bytes > 0)) return ""
        return bytes >= 1048576 ? (bytes / 1048576).toLocaleString(Qt.locale(), 'f', 1) + " MB"
                                : Math.round(bytes / 1024) + " KB"
    }
    // ---- About the artist and the album ------------------------------------
    // Asked only while one of those sections (or the credits) shows.
    readonly property bool wantsAbout: visible && hasSubject
        && ["artist", "album", "credits"].some(id => { void layoutPrefs.hiddenSections; void layoutPrefs.collapsedSections
                                                       return layoutPrefs.sectionShown(id) && !layoutPrefs.isCollapsed(id) })
    readonly property var aboutSubject: hasTrack ? { trackId: trackId }
                                       : { artist: shownArtist, album: shownAlbum, title: shownTitle }
    function askAbout() {
        if (!wantsAbout) return
        const s = Object.assign({}, aboutSubject)
        s.language = translator.language
        s.online = layoutPrefs.onlineInfo
        library.about.show(s)
    }
    onWantsAboutChanged: askAbout()
    onAboutSubjectChanged: askAbout()
    Connections {
        target: panel.layoutPrefs
        function onChanged() { panel.askAbout() }
    }

    // ---- The heading and the ⋮ menu -----------------------------------------
    RowLayout {
        id: sideTop
        anchors { left: parent.left; right: parent.right; top: parent.top; leftMargin: 12; rightMargin: 4; topMargin: 4 }
        height: 34
        spacing: 4
        Text {
            objectName: "sideHeading"
            Layout.fillWidth: true
            elide: Text.ElideRight
            text: panel.showingPlaying || panel.playingOutside ? qsTr("Now playing")
                  : panel.hasTrack ? (panel.chosenHeading !== "" ? panel.chosenHeading : qsTr("Selected track")) : qsTr("Now playing")
            font.pixelSize: 13
            font.bold: true
            color: theme.c.bright_foreground
        }
        Button {
            objectName: "sideFollow"
            flat: true
            background: Rectangle {
                radius: Math.min(4, theme.radius)
                color: parent.hovered ? Qt.alpha(theme.c.selection, 0.6) : "transparent"
                border.width: 1
                border.color: Qt.alpha(theme.c.foreground, 0.2)
            }
            contentItem: Text {
                text: parent.text
                font: parent.font
                color: theme.c.light_foreground
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
            text: panel.followPlaying ? qsTr("Following playing") : qsTr("Following selection")
            font.pixelSize: 11
            implicitHeight: 28; topInset: 0; bottomInset: 0
            leftPadding: 8; rightPadding: 8
            ToolTip.visible: hovered
            ToolTip.delay: 600
            ToolTip.text: panel.followPlaying ? qsTr("Show the track under the cursor instead")
                                              : qsTr("Always show the track that is playing")
            onClicked: panel.layoutPrefs.follow = panel.followPlaying ? "selection" : "playing"
        }
        Button {
            id: menuButton
            objectName: "sideMenuButton"
            flat: true
            background: Rectangle {
                radius: Math.min(4, theme.radius)
                color: parent.hovered ? Qt.alpha(theme.c.selection, 0.6) : "transparent"
                border.width: 1
                border.color: Qt.alpha(theme.c.foreground, 0.2)
            }
            contentItem: Text {
                text: parent.text
                font: parent.font
                color: theme.c.light_foreground
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
            text: "⋮"
            font.pixelSize: 16
            implicitWidth: 30
            implicitHeight: 28; topInset: 0; bottomInset: 0
            ToolTip.visible: hovered
            ToolTip.delay: 600
            ToolTip.text: qsTr("Choose and order the sections")
            onClicked: sectionMenu.popup(menuButton, 0, menuButton.height)
        }
    }

    Menu {
        id: sectionMenu
        objectName: "sideSectionMenu"
        Material.roundedScale: theme.radius
        font.pixelSize: 13
        Instantiator {
            model: panel.layoutPrefs.sections
            delegate: MenuItem {
                required property string modelData
                required property int index
                objectName: "sideMenu_" + modelData
                text: panel.layoutPrefs.sectionName(modelData)
                checkable: true
                checked: { void panel.layoutPrefs.hiddenSections; return panel.layoutPrefs.sectionShown(modelData) }
                onTriggered: panel.layoutPrefs.setSectionShown(modelData, checked)
            }
            onObjectAdded: (index, object) => sectionMenu.insertItem(index, object)
            onObjectRemoved: (index, object) => sectionMenu.removeItem(object)
        }
        MenuSeparator { }
        MenuItem { text: qsTr("Order the sections…"); onTriggered: orderPopup.open() }
        MenuItem {
            text: panel.followPlaying ? qsTr("Follow the selection") : qsTr("Follow what is playing")
            onTriggered: panel.layoutPrefs.follow = panel.followPlaying ? "selection" : "playing"
        }
    }

    // Up and down arrows beside each section's name.
    Popup {
        id: orderPopup
        objectName: "sideOrderPopup"
        x: panel.width - width - 8
        y: sideTop.y + sideTop.height
        width: Math.min(panel.width - 16, 280)
        padding: 10
        background: Rectangle {
            color: theme.c.background
            border.width: 1
            border.color: theme.c.lighter_background
            radius: Math.min(6, theme.radius)
        }
        contentItem: SectionOrder { }
    }

    // ---- The sections -------------------------------------------------------
    Flickable {
        id: flick
        anchors { left: parent.left; right: parent.right; top: sideTop.bottom; bottom: parent.bottom; leftMargin: 12; rightMargin: 12 }
        contentHeight: content.implicitHeight + 16
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

        ColumnLayout {
            id: content
            width: flick.width
            spacing: 8

            Repeater {
                id: sectionRepeater
                model: panel.layoutPrefs.sections
                Loader {
                    required property string modelData
                    readonly property string sectionId: modelData
                    Layout.fillWidth: true
                    visible: { void panel.layoutPrefs.hiddenSections; return panel.layoutPrefs.sectionShown(modelData) && item !== null && item.wanted !== false }
                    active: { void panel.layoutPrefs.hiddenSections; return panel.layoutPrefs.sectionShown(modelData) }
                    sourceComponent: modelData === "cover" ? coverSection
                                   : modelData === "title" ? titleSection
                                   : modelData === "lyrics" ? lyricsSection
                                   : modelData === "artist" ? artistSection
                                   : modelData === "album" ? albumSection
                                   : modelData === "credits" ? creditsSection
                                   : modelData === "upnext" ? upNextSection
                                   : detailsSection
                }
            }

            Text {
                Layout.fillWidth: true
                Layout.topMargin: 40
                visible: !panel.hasSubject
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                text: qsTr("Choose a track to see its details here.")
                font.pixelSize: 13
                color: theme.c.muted
            }
        }
    }

    // ---- Each section --------------------------------------------------------

    Component {
        id: coverSection
        Rectangle {
            property bool wanted: panel.hasSubject
            implicitHeight: width
            color: theme.c.darker_background
            border.width: 1
            border.color: theme.c.lighter_background
            radius: Math.min(6, theme.radius)
            Image {
                id: cover
                objectName: "infoCover"
                anchors.fill: parent
                anchors.margins: 1
                asynchronous: true
                cache: true
                fillMode: Image.PreserveAspectCrop
                sourceSize: Qt.size(600, 600)
                source: panel.hasTrack ? (panel.info.artState !== 2 ? "image://thrum-art/track/" + panel.trackId + "/600?r=0&v=" + library.revision : "")
                        : panel.playingOutside ? nowPlayingArt.artUrl : ""
            }
            Text {
                anchors.centerIn: parent
                visible: cover.status !== Image.Ready
                text: String.fromCodePoint(panel.radioPlaying ? 0xF0439 : 0xF0025)
                font.family: "JetBrainsMono Nerd Font"
                font.pixelSize: 56
                color: theme.c.lighter_background
            }
            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: panel.openBigView()
            }
            ToolTip.visible: coverHover.hovered
            ToolTip.delay: 600
            ToolTip.text: qsTr("Big Now Playing view (N)")
            HoverHandler { id: coverHover }
        }
    }

    Component {
        id: titleSection
        ColumnLayout {
            property bool wanted: panel.hasSubject
            spacing: 3
            Text {
                objectName: "infoTitle"
                Layout.fillWidth: true
                text: panel.shownTitle
                wrapMode: Text.Wrap
                maximumLineCount: 3
                elide: Text.ElideRight
                font.pixelSize: 18
                font.bold: true
                color: theme.c.bright_foreground
            }
            Text {
                objectName: "infoArtist"
                Layout.fillWidth: true
                visible: text !== ""
                text: panel.shownArtist
                wrapMode: Text.Wrap
                font.pixelSize: 13
                color: theme.c.foreground
            }
            Text {
                Layout.fillWidth: true
                visible: text !== ""
                text: panel.radioPlaying ? (radio.current.fullName || radio.current.name || "")
                      : panel.shownAlbum + (panel.hasTrack && panel.info.year > 0 ? " (" + panel.info.year + ")" : "")
                wrapMode: Text.Wrap
                font.pixelSize: 13
                color: theme.c.light_foreground
            }
            Text {
                Layout.fillWidth: true
                visible: !panel.hasTrack
                text: panel.radioPlaying ? qsTr("Internet radio") : panel.spotifyPlaying ? qsTr("Playing on Spotify")
                      : qsTr("This file isn't in the library.")
                font.pixelSize: 12
                color: theme.c.muted
            }
            // The rating: click a star to set it (half stars show dimmer).
            Row {
                objectName: "sideRating"
                visible: panel.hasTrack
                Layout.topMargin: 2
                Repeater {
                    model: 5
                    Text {
                        required property int index
                        readonly property real fill: Math.max(0, Math.min(1, (panel.info.rating - index * 20) / 20))
                        text: "★"
                        font.pixelSize: 17
                        rightPadding: 2
                        color: fill >= 1 ? theme.accent : fill >= 0.5 ? Qt.alpha(theme.accent, 0.5) : theme.c.lighter_background
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: library.tagEditor.quickSet(panel.trackId, "rating", String((parent.index + 1) * 20))
                        }
                    }
                }
            }
        }
    }

    Component {
        id: lyricsSection
        SideSection {
            sectionId: "lyrics"
            property bool wanted: panel.hasSubject
            LyricsView {
                Layout.fillWidth: true
                Layout.preferredHeight: panel.hasTrack ? Math.max(220, Math.min(420, panel.height * 0.45)) : 0
                visible: panel.hasTrack
                trackId: panel.hasTrack ? panel.trackId : 0
                onEditLyrics: (id) => panel.editLyrics(id)
            }
            Text {
                Layout.fillWidth: true
                visible: !panel.hasTrack
                wrapMode: Text.Wrap
                text: qsTr("Lyrics show for tracks in the library.")
                font.pixelSize: 12
                color: theme.c.muted
            }
        }
    }

    // What the web had, or why it had nothing.
    component AboutText: ColumnLayout {
        id: about
        property var summary: ({})
        property string state: "idle"
        property string what: ""
        property bool showPicture: false
        spacing: 6
        RowLayout {
            Layout.fillWidth: true
            visible: about.state === "found"
            spacing: 10
            Rectangle {
                visible: about.showPicture && about.summary.imageFile !== undefined && about.summary.imageFile !== ""
                Layout.preferredWidth: 64
                Layout.preferredHeight: 64
                Layout.alignment: Qt.AlignTop
                radius: Math.min(4, theme.radius)
                color: theme.c.lighter_background
                Image {
                    objectName: "aboutPicture"
                    anchors.fill: parent
                    anchors.margins: 0
                    fillMode: Image.PreserveAspectCrop
                    asynchronous: true
                    sourceSize: Qt.size(128, 128)
                    source: about.summary.imageFile || ""
                }
            }
            Text {
                Layout.fillWidth: true
                visible: about.summary.description !== undefined && about.summary.description !== ""
                text: about.summary.description || ""
                wrapMode: Text.Wrap
                font.pixelSize: 12
                font.italic: true
                color: theme.c.light_foreground
            }
        }
        Text {
            id: extract
            objectName: "aboutExtract_" + about.what
            Layout.fillWidth: true
            visible: about.state === "found"
            property bool open: false
            text: about.summary.extract || ""
            wrapMode: Text.Wrap
            maximumLineCount: open ? 1000 : 7
            elide: Text.ElideRight
            lineHeight: 1.15
            font.pixelSize: 13
            color: theme.c.foreground
        }
        RowLayout {
            Layout.fillWidth: true
            visible: about.state === "found"
            spacing: 10
            Text {
                visible: extract.truncated || extract.open
                text: extract.open ? qsTr("Show less") : qsTr("Show more")
                font.pixelSize: 12
                color: theme.accent
                MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: extract.open = !extract.open }
            }
            Text {
                objectName: "aboutReadMore_" + about.what
                text: qsTr("Read more on Wikipedia")
                font.pixelSize: 12
                color: theme.accent
                MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: Qt.openUrlExternally(about.summary.pageUrl) }
            }
        }
        Text {
            Layout.fillWidth: true
            visible: about.state === "found"
            wrapMode: Text.Wrap
            text: qsTr("From Wikipedia, under CC BY-SA 4.0, found through MusicBrainz and Wikidata.")
            font.pixelSize: 10
            color: theme.c.muted
        }
        Text {
            objectName: "aboutStatus_" + about.what
            Layout.fillWidth: true
            visible: about.state !== "found"
            wrapMode: Text.Wrap
            text: about.state === "loading" ? qsTr("Looking it up…")
                  : about.state === "offline" ? qsTr("Offline: no info found.")
                  : about.state === "off" ? qsTr("Online look-ups are off (Settings → Appearance → Layout).")
                  : qsTr("No info found.")
            font.pixelSize: 12
            color: theme.c.muted
        }
        Button {
            objectName: "aboutRetry_" + about.what
            visible: about.state === "none" || about.state === "offline"
            text: qsTr("Retry")
            flat: true
            font.pixelSize: 12
            implicitHeight: 28; topInset: 0; bottomInset: 0
            onClicked: library.about.retry()
        }
    }

    Component {
        id: artistSection
        SideSection {
            sectionId: "artist"
            property bool wanted: panel.hasSubject && panel.shownArtist !== ""
            title: qsTr("About %1").arg(panel.shownArtist)
            AboutText {
                Layout.fillWidth: true
                what: "artist"
                showPicture: true
                summary: library.about.artist
                state: library.about.artistState
            }
        }
    }

    Component {
        id: albumSection
        SideSection {
            sectionId: "album"
            property bool wanted: panel.hasSubject && (panel.shownAlbum !== "" || library.about.notes.length > 0)
            title: panel.shownAlbum !== "" ? qsTr("About %1").arg(panel.shownAlbum) : qsTr("About the album")
            // Notes that came with the album, first.
            Repeater {
                model: library.about.notes
                ColumnLayout {
                    id: noteBox
                    required property var modelData
                    required property int index
                    Layout.fillWidth: true
                    spacing: 3
                    property bool open: false
                    Text {
                        Layout.fillWidth: true
                        text: noteBox.modelData.path ? qsTr("Packaged with the album: %1").arg(noteBox.modelData.name) : noteBox.modelData.name
                        elide: Text.ElideMiddle
                        font.pixelSize: 11
                        font.bold: true
                        color: theme.c.light_foreground
                    }
                    Text {
                        id: noteText
                        objectName: "albumNote_" + noteBox.index
                        Layout.fillWidth: true
                        text: noteBox.modelData.text
                        textFormat: Text.PlainText
                        wrapMode: Text.Wrap
                        maximumLineCount: noteBox.open ? 100000 : 10
                        elide: Text.ElideRight
                        font.pixelSize: 12
                        color: theme.c.foreground
                    }
                    Text {
                        visible: noteText.truncated || noteBox.open
                        text: noteBox.open ? qsTr("Show less") : qsTr("Show all")
                        font.pixelSize: 12
                        color: theme.accent
                        MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: noteBox.open = !noteBox.open }
                    }
                }
            }
            AboutText {
                Layout.fillWidth: true
                visible: panel.shownAlbum !== ""
                what: "album"
                summary: library.about.album
                state: library.about.albumState
            }
        }
    }

    Component {
        id: creditsSection
        SideSection {
            sectionId: "credits"
            property bool wanted: panel.hasSubject && library.about.credits.length > 0
            GridLayout {
                Layout.fillWidth: true
                columns: 2
                columnSpacing: 10
                rowSpacing: 4
                Repeater {
                    objectName: "sideCredits"
                    model: library.about.credits
                    delegate: Text {
                        required property var modelData
                        required property int index
                        Layout.columnSpan: 2
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        textFormat: Text.StyledText
                        text: "<font color=\"" + theme.c.muted + "\">" + modelData.role + "</font>&nbsp;&nbsp;" + modelData.name.replace(/&/g, "&amp;").replace(/</g, "&lt;")
                        font.pixelSize: 12
                        color: theme.c.foreground
                    }
                }
            }
        }
    }

    Component {
        id: upNextSection
        SideSection {
            id: upNextBox
            sectionId: "upnext"
            readonly property var rows: {
                void backend.playlist.count
                const out = []
                for (let r = backend.currentIndex + 1; r < backend.playlist.count && out.length < 5; ++r) out.push(r)
                return out
            }
            property bool wanted: rows.length > 0
            title: backend.shuffle ? qsTr("Up next (in list order; shuffle is on)") : qsTr("Up next")
            Repeater {
                objectName: "sideUpNext"
                model: upNextBox.rows
                Item {
                    id: nextRow
                    required property var modelData
                    readonly property var track: { void backend.playlist.count; return backend.trackAt(modelData) }
                    Layout.fillWidth: true
                    implicitHeight: 36
                    RowLayout {
                        anchors.fill: parent
                        spacing: 8
                        ArtThumb {
                            size: 32
                            path: nextRow.track.path || ""
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 0
                            Text {
                                Layout.fillWidth: true
                                elide: Text.ElideRight
                                text: nextRow.track.title || ""
                                font.pixelSize: 13
                                color: theme.c.foreground
                            }
                            Text {
                                Layout.fillWidth: true
                                elide: Text.ElideRight
                                visible: text !== ""
                                text: nextRow.track.artist || ""
                                font.pixelSize: 11
                                color: theme.c.muted
                            }
                        }
                        Text {
                            text: panel.clock(nextRow.track.durationMs)
                            font.pixelSize: 11
                            color: theme.c.muted
                        }
                    }
                    MouseArea {
                        anchors.fill: parent
                        onDoubleClicked: backend.playIndex(nextRow.modelData)
                    }
                }
            }
        }
    }

    Component {
        id: detailsSection
        SideSection {
            sectionId: "details"
            property bool wanted: panel.hasSubject
            // A radio station: what the station says of itself.
            GridLayout {
                Layout.fillWidth: true
                visible: !panel.hasTrack
                columns: 2
                columnSpacing: 10
                rowSpacing: 5
                Repeater {
                    model: panel.radioPlaying ? [
                        [qsTr("Station"), radio.current.fullName || radio.current.name || ""],
                        [qsTr("About"), radio.current.description || ""],
                        [qsTr("Playing"), radio.nowPlaying],
                        [qsTr("Stream"), (radio.current.codec || "").toUpperCase() + (radio.current.bitrate > 0 ? " " + radio.current.bitrate + " kbps" : "")],
                        [qsTr("Website"), radio.current.homepage || ""]
                    ].filter(r => r[1]) : [
                        [qsTr("Length"), panel.clock(backend.duration)],
                        [qsTr("Format"), backend.bitrate > 0 ? backend.bitrate + " kbps" : ""],
                        [qsTr("Where"), panel.spotifyPlaying ? qsTr("Spotify") : panel.playingPath]
                    ].filter(r => r[1])
                    delegate: Text {
                        required property var modelData
                        Layout.columnSpan: 2
                        Layout.fillWidth: true
                        wrapMode: Text.WrapAnywhere
                        textFormat: Text.StyledText
                        text: "<font color=\"" + theme.c.muted + "\">" + modelData[0] + "</font>&nbsp;&nbsp;" + String(modelData[1]).replace(/&/g, "&amp;").replace(/</g, "&lt;")
                        font.pixelSize: 12
                        color: theme.c.foreground
                    }
                }
            }
            // The details, as label and value pairs; empty ones are left out.
            GridLayout {
                Layout.fillWidth: true
                visible: panel.hasTrack
                columns: 2
                columnSpacing: 10
                rowSpacing: 5

                Repeater {
                    objectName: "infoRows"
                    model: panel.hasTrack ? [
                        [qsTr("Album artist"), panel.info.albumArtist !== panel.info.artist ? panel.info.albumArtist : ""],
                        [qsTr("Genre"), panel.info.genre],
                        [qsTr("Composer"), panel.info.composer],
                        [qsTr("Track"), panel.info.trackNo > 0 ? (panel.info.discNo > 0 ? qsTr("%1, disc %2").arg(panel.info.trackNo).arg(panel.info.discNo) : String(panel.info.trackNo)) : ""],
                        [qsTr("Length"), panel.clock(panel.info.durationMs)],
                        [qsTr("Format"), panel.formatLine(panel.info)],
                        [qsTr("Size"), panel.size(panel.info.size)],
                        [qsTr("ReplayGain"), panel.info.trackGain !== undefined && panel.info.trackGain !== null
                                             ? qsTr("track %1").arg(panel.gain(panel.info.trackGain, panel.info.trackPeak))
                                               + (panel.info.albumGain !== undefined && panel.info.albumGain !== null
                                                  ? "\n" + qsTr("album %1").arg(panel.gain(panel.info.albumGain, panel.info.albumPeak)) : "")
                                             : qsTr("Not analysed")],
                        [qsTr("Plays"), panel.info.plays > 0 ? Number(panel.info.plays).toLocaleString(Qt.locale(), 'f', 0) : qsTr("Never played")],
                        [qsTr("Last played"), panel.day(panel.info.lastPlayed)],
                        [qsTr("Added"), panel.day(panel.info.added)],
                        [qsTr("Lyrics"), panel.info.hasLyrics ? qsTr("Yes, in the file") : qsTr("None")],
                        [qsTr("BPM"), panel.info.bpm > 0 ? String(panel.info.bpm) : ""]
                    ].filter(r => r[1] !== undefined && r[1] !== "") : []

                    delegate: Item {
                        id: pair
                        required property var modelData
                        required property int index
                        Layout.columnSpan: 2
                        Layout.fillWidth: true
                        implicitHeight: Math.max(key.implicitHeight, val.implicitHeight)
                        Text {
                            id: key
                            width: 92
                            text: pair.modelData[0]
                            font.pixelSize: 12
                            color: theme.c.muted
                            elide: Text.ElideRight
                        }
                        Text {
                            id: val
                            anchors { left: key.right; leftMargin: 8; right: parent.right }
                            text: pair.modelData[1]
                            wrapMode: Text.Wrap
                            font.pixelSize: 12
                            color: theme.c.foreground
                        }
                    }
                }
            }
            // Where the file is; selectable, so it can be copied.
            Text {
                Layout.fillWidth: true
                Layout.topMargin: 4
                visible: panel.hasTrack
                text: panel.info.online === false ? qsTr("File (offline)") : qsTr("File")
                font.pixelSize: 12
                color: theme.c.muted
            }
            TextEdit {
                objectName: "infoPath"
                Layout.fillWidth: true
                visible: panel.hasTrack
                readOnly: true
                selectByMouse: true
                text: panel.info.path || ""
                wrapMode: TextEdit.WrapAnywhere
                font.pixelSize: 11
                color: panel.info.online === false ? theme.c.muted : theme.c.foreground
                selectionColor: theme.c.selection
            }
        }
    }
}
