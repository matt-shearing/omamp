import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Identify with MusicBrainz (Ctrl+I), the way Picard does it: the chosen
// tracks are grouped into albums, each album gets a ranked list of
// MusicBrainz releases with a score and the reasons for it, and the files
// are paired with the release's tracks. Every tag change shows, old and
// new, before anything is written. Save writes each ticked album as one
// change Ctrl+Z can undo.
ToolSheet {
    id: sheet
    objectName: "identifySheet"
    readonly property var web: library.web
    readonly property var clusters: web.clusters
    property int current: 0
    readonly property var cluster: clusters.length > current ? clusters[current] : null
    readonly property bool working: web.identifyState === "working" || web.identifyState === "applying"
    readonly property int includedCount: clusters.filter(c => c.included).length
    title: qsTr("Identify with MusicBrainz")
    subtitle: web.identifyFiles > 0 ? qsTr("%n files", "", web.identifyFiles) : ""
    preferredWidth: 1220
    preferredHeight: 800
    signal openSettings(int page)

    function openFor(ids) {
        current = 0
        tabs.currentIndex = 0
        releaseField.text = ""
        const o = web.identifyOptions()
        embedBox.checked = o.coverEmbed !== false
        coverFileBox.checked = o.coverFile === true
        genresBox.checked = o.writeGenres === true
        labelBox.checked = o.writeLabel !== false
        if (!web.identify(ids)) return false
        open()
        return true
    }
    function save() {
        if (!saveButton.enabled) return
        web.applyIdentify({ coverEmbed: embedBox.checked, coverFile: coverFileBox.checked,
                            writeGenres: genresBox.checked, writeLabel: labelBox.checked })
    }
    function confidenceColour(c) {
        return c === "high" ? theme.c.green : c === "medium" ? theme.c.yellow : theme.c.red
    }
    function confidenceWord(c) {
        return c === "high" ? qsTr("High") : c === "medium" ? qsTr("Medium") : qsTr("Low")
    }
    onAccepted: save()
    onClosed: { if (working) library.cancelJob(); web.clearIdentify() }

    Connections {
        target: sheet.web
        function onIdentifyApplied(changed, failed) { if (failed === 0) sheet.close() }
    }

    component Note: Rectangle {
        property alias text: noteText.text
        property color tint: theme.c.yellow
        default property alias extra: noteRow.data
        Layout.fillWidth: true
        implicitHeight: noteRow.implicitHeight + 16
        radius: Math.min(4, theme.radius)
        color: Qt.alpha(tint, 0.12)
        border.width: 1
        border.color: Qt.alpha(tint, 0.5)
        RowLayout {
            id: noteRow
            anchors { fill: parent; leftMargin: 12; rightMargin: 8 }
            spacing: 10
            Text {
                id: noteText
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                font.pixelSize: 13
                color: theme.c.foreground
            }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 10

        // What the run couldn't use, said plainly.
        Note {
            objectName: "acoustidNote"
            visible: !sheet.web.acoustIdKeySet
            text: qsTr("No AcoustID key is set, so tracks are matched by their tags and any MusicBrainz IDs they already carry, not by their sound.")
            Button {
                text: qsTr("Add a key in Settings → Web")
                flat: true
                font.pixelSize: 13
                implicitHeight: 34; topInset: 0; bottomInset: 0
                onClicked: { sheet.close(); sheet.openSettings(3) }
            }
        }
        Note {
            objectName: "offlineNote"
            visible: sheet.web.identifyOffline
            tint: theme.c.red
            text: qsTr("Offline: MusicBrainz couldn't be reached. Anything shown came from earlier look-ups. Check the network and try again.")
        }
        Repeater {
            model: sheet.web.identifyNotes
            Note { required property string modelData; text: modelData; tint: theme.c.blue }
        }

        // Looking things up: one request a second, as MusicBrainz asks.
        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: sheet.clusters.length === 0
            Item { Layout.fillHeight: true }
            BusyIndicator {
                Layout.alignment: Qt.AlignHCenter
                running: sheet.working
                visible: sheet.working
            }
            Text {
                Layout.alignment: Qt.AlignHCenter
                Layout.maximumWidth: 640
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                text: sheet.working ? (library.jobText !== "" ? library.jobText : qsTr("Looking up on MusicBrainz…"))
                      : qsTr("Nothing to show.")
                font.pixelSize: 14
                color: theme.c.foreground
            }
            Text {
                Layout.alignment: Qt.AlignHCenter
                visible: sheet.working
                text: qsTr("MusicBrainz answers one request a second, so a big selection takes a while.")
                font.pixelSize: 12
                color: theme.c.muted
            }
            Item { Layout.fillHeight: true }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: sheet.clusters.length > 0
            spacing: 14

            // The albums found among the chosen tracks.
            ColumnLayout {
                Layout.preferredWidth: 270
                Layout.fillHeight: true
                spacing: 6
                Text {
                    text: qsTr("Albums")
                    font.pixelSize: 13
                    font.bold: true
                    color: theme.c.bright_foreground
                }
                Rectangle {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    color: theme.c.darker_background
                    border.width: 1
                    border.color: theme.c.lighter_background
                    radius: Math.min(4, theme.radius)
                    ListView {
                        id: clusterList
                        objectName: "clusterList"
                        anchors.fill: parent
                        anchors.margins: 4
                        clip: true
                        model: sheet.clusters
                        currentIndex: sheet.current
                        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                        delegate: Rectangle {
                            id: crow
                            required property var modelData
                            required property int index
                            readonly property var best: modelData.candidates.length > 0 && modelData.chosen >= 0
                                                        ? modelData.candidates[modelData.chosen] : null
                            width: ListView.view.width
                            implicitHeight: ccol.implicitHeight + 12
                            radius: Math.min(3, theme.radius)
                            color: index === sheet.current ? theme.c.selection : "transparent"
                            MouseArea { anchors.fill: parent; onClicked: { sheet.current = crow.index; tabs.currentIndex = 0 } }
                            RowLayout {
                                anchors { fill: parent; leftMargin: 2; rightMargin: 6 }
                                spacing: 2
                                CheckBox {
                                    checked: crow.modelData.included
                                    enabled: crow.best !== null && !sheet.working
                                    onToggled: sheet.web.setIncluded(crow.index, checked)
                                    ToolTip.visible: hovered
                                    ToolTip.delay: 600
                                    ToolTip.text: qsTr("Save this album")
                                }
                                ColumnLayout {
                                    id: ccol
                                    Layout.fillWidth: true
                                    spacing: 1
                                    Text {
                                        Layout.fillWidth: true
                                        elide: Text.ElideRight
                                        text: crow.modelData.album
                                        font.pixelSize: 13
                                        font.bold: true
                                        color: theme.c.bright_foreground
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                        elide: Text.ElideRight
                                        text: crow.modelData.artist
                                        font.pixelSize: 12
                                        color: theme.c.foreground
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                        elide: Text.ElideRight
                                        text: crow.best
                                              ? qsTr("%n files", "", crow.modelData.fileCount) + "  ·  "
                                                + qsTr("%1% match, %2").arg(crow.best.score).arg(sheet.confidenceWord(crow.best.confidence).toLowerCase())
                                              : qsTr("%n files", "", crow.modelData.fileCount) + "  ·  " + qsTr("no match")
                                        font.pixelSize: 11
                                        color: crow.best ? sheet.confidenceColour(crow.best.confidence) : theme.c.red
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // One album: its releases, the pairing of files and tracks, the changes.
            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 8

                Text {
                    text: qsTr("MusicBrainz releases, best first")
                    font.pixelSize: 13
                    font.bold: true
                    color: theme.c.bright_foreground
                }
                Text {
                    Layout.fillWidth: true
                    visible: sheet.cluster !== null && sheet.cluster.candidates.length === 0
                    wrapMode: Text.Wrap
                    text: sheet.cluster ? (sheet.cluster.error !== "" ? sheet.cluster.error : qsTr("No release matches these files.")) + " "
                                          + qsTr("Paste a release's ID or link below if you know it.") : ""
                    font.pixelSize: 13
                    color: theme.c.red
                }
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: Math.min(236, Math.max(62, candidateList.contentHeight + 8))
                    visible: sheet.cluster !== null && sheet.cluster.candidates.length > 0
                    color: theme.c.darker_background
                    border.width: 1
                    border.color: theme.c.lighter_background
                    radius: Math.min(4, theme.radius)
                    ListView {
                        id: candidateList
                        objectName: "candidateList"
                        anchors.fill: parent
                        anchors.margins: 4
                        clip: true
                        model: sheet.cluster ? sheet.cluster.candidates : []
                        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                        delegate: Rectangle {
                            id: cand
                            required property var modelData
                            required property int index
                            readonly property bool chosen: sheet.cluster && sheet.cluster.chosen === index
                            width: ListView.view.width
                            implicitHeight: candCol.implicitHeight + 12
                            radius: Math.min(3, theme.radius)
                            color: chosen ? Qt.alpha(theme.accent, 0.16) : hover.hovered ? Qt.alpha(theme.c.selection, 0.5) : "transparent"
                            HoverHandler { id: hover }
                            MouseArea {
                                anchors.fill: parent
                                enabled: !sheet.working
                                onClicked: sheet.web.chooseCandidate(sheet.current, cand.index)
                            }
                            RowLayout {
                                anchors { fill: parent; leftMargin: 6; rightMargin: 8 }
                                spacing: 10
                                RadioButton {
                                    checked: cand.chosen
                                    enabled: !sheet.working
                                    onClicked: sheet.web.chooseCandidate(sheet.current, cand.index)
                                }
                                // The score, coloured by how sure it is.
                                Rectangle {
                                    Layout.preferredWidth: 74
                                    Layout.preferredHeight: 40
                                    radius: Math.min(4, theme.radius)
                                    color: Qt.alpha(sheet.confidenceColour(cand.modelData.confidence), 0.16)
                                    border.width: 1
                                    border.color: sheet.confidenceColour(cand.modelData.confidence)
                                    Column {
                                        anchors.centerIn: parent
                                        Text {
                                            anchors.horizontalCenter: parent.horizontalCenter
                                            text: cand.modelData.score + "%"
                                            font.pixelSize: 15
                                            font.bold: true
                                            color: theme.c.bright_foreground
                                        }
                                        Text {
                                            anchors.horizontalCenter: parent.horizontalCenter
                                            text: sheet.confidenceWord(cand.modelData.confidence)
                                            font.pixelSize: 10
                                            color: theme.c.foreground
                                        }
                                    }
                                }
                                ColumnLayout {
                                    id: candCol
                                    Layout.fillWidth: true
                                    spacing: 2
                                    Text {
                                        Layout.fillWidth: true
                                        elide: Text.ElideRight
                                        text: cand.modelData.title + (cand.modelData.disambiguation ? " (" + cand.modelData.disambiguation + ")" : "")
                                              + "  —  " + cand.modelData.artist
                                        font.pixelSize: 13
                                        font.bold: cand.chosen
                                        color: theme.c.bright_foreground
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                        elide: Text.ElideRight
                                        text: [cand.modelData.date, cand.modelData.country, cand.modelData.format,
                                               qsTr("%n tracks", "", cand.modelData.tracks), cand.modelData.labels]
                                              .filter(x => x !== undefined && x !== "").join("  ·  ")
                                        font.pixelSize: 12
                                        color: theme.c.foreground
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                        wrapMode: Text.Wrap
                                        text: cand.modelData.why
                                        font.pixelSize: 11
                                        color: theme.c.muted
                                    }
                                }
                                Button {
                                    text: qsTr("Open on MusicBrainz")
                                    flat: true
                                    font.pixelSize: 12
                                    implicitHeight: 32; topInset: 0; bottomInset: 0
                                    onClicked: Qt.openUrlExternally(cand.modelData.url)
                                }
                            }
                        }
                    }
                }

                // Another release, by its ID or link.
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    visible: sheet.cluster !== null
                    TextField {
                        id: releaseField
                        objectName: "releaseField"
                        Layout.fillWidth: true
                        placeholderText: qsTr("Use another release: paste its MusicBrainz ID or link")
                        font.pixelSize: 13
                        selectByMouse: true
                        onAccepted: lookUp.clicked()
                    }
                    Button {
                        id: lookUp
                        objectName: "releaseLookUp"
                        text: qsTr("Look up")
                        font.pixelSize: 13
                        enabled: releaseField.text.trim() !== "" && !sheet.working
                        onClicked: if (sheet.web.useRelease(sheet.current, releaseField.text)) releaseField.text = ""
                    }
                }

                TabBar {
                    id: tabs
                    Layout.fillWidth: true
                    visible: sheet.cluster !== null && sheet.cluster.chosen >= 0
                    TabButton { text: qsTr("Files and tracks"); font.pixelSize: 13; width: implicitWidth + 24 }
                    TabButton {
                        objectName: "changesTab"
                        text: sheet.cluster && sheet.cluster.changeCount > 0
                              ? qsTr("Tag changes (%1)").arg(sheet.cluster.changeCount) : qsTr("Tag changes")
                        font.pixelSize: 13
                        width: implicitWidth + 24
                    }
                }

                StackLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    visible: tabs.visible
                    currentIndex: tabs.currentIndex

                    // Each file and the release track it was paired with.
                    Rectangle {
                        color: theme.c.darker_background
                        border.width: 1
                        border.color: theme.c.lighter_background
                        radius: Math.min(4, theme.radius)
                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: 4
                            spacing: 0
                            RowLayout {
                                Layout.fillWidth: true
                                Layout.leftMargin: 8
                                Layout.rightMargin: 8
                                Layout.preferredHeight: 26
                                spacing: 10
                                Text { Layout.preferredWidth: 300; text: qsTr("File"); font.pixelSize: 12; font.bold: true; color: theme.c.muted }
                                Text { Layout.fillWidth: true; text: qsTr("Release track"); font.pixelSize: 12; font.bold: true; color: theme.c.muted }
                                Text { Layout.preferredWidth: 110; text: qsTr("Length"); font.pixelSize: 12; font.bold: true; color: theme.c.muted }
                                Text { Layout.preferredWidth: 60; text: qsTr("Match"); horizontalAlignment: Text.AlignRight; font.pixelSize: 12; font.bold: true; color: theme.c.muted }
                            }
                            ListView {
                                objectName: "mappingList"
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                clip: true
                                model: sheet.cluster ? sheet.cluster.mapping : []
                                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                                delegate: Rectangle {
                                    id: mrow
                                    required property var modelData
                                    required property int index
                                    width: ListView.view.width
                                    height: 26
                                    color: index % 2 ? Qt.alpha(theme.c.lighter_background, 0.35) : "transparent"
                                    readonly property color scoreColour: !modelData.matched ? theme.c.red
                                        : modelData.score >= 85 ? theme.c.green : modelData.score >= 65 ? theme.c.yellow : theme.c.red
                                    RowLayout {
                                        anchors { fill: parent; leftMargin: 8; rightMargin: 8 }
                                        spacing: 10
                                        Text {
                                            Layout.preferredWidth: 300
                                            elide: Text.ElideMiddle
                                            text: mrow.modelData.file
                                            font.pixelSize: 12
                                            color: theme.c.foreground
                                        }
                                        Text {
                                            Layout.fillWidth: true
                                            elide: Text.ElideRight
                                            text: mrow.modelData.matched ? mrow.modelData.position + ". " + mrow.modelData.title
                                                                         : qsTr("No track on this release fits; left as it is")
                                            font.pixelSize: 12
                                            font.italic: !mrow.modelData.matched
                                            color: mrow.modelData.matched ? theme.c.bright_foreground : theme.c.muted
                                        }
                                        Text {
                                            Layout.preferredWidth: 110
                                            text: (mrow.modelData.fromLength || "?") + (mrow.modelData.matched ? " → " + (mrow.modelData.length || "?") : "")
                                            font.pixelSize: 12
                                            color: theme.c.light_foreground
                                        }
                                        Rectangle {
                                            Layout.preferredWidth: 60
                                            Layout.preferredHeight: 18
                                            radius: Math.min(3, theme.radius)
                                            color: Qt.alpha(mrow.scoreColour, 0.18)
                                            Text {
                                                anchors.centerIn: parent
                                                text: mrow.modelData.matched ? mrow.modelData.score + "%" : "—"
                                                font.pixelSize: 11
                                                font.bold: true
                                                color: theme.c.bright_foreground
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }

                    // Every tag that would change, old and new.
                    Rectangle {
                        color: theme.c.darker_background
                        border.width: 1
                        border.color: theme.c.lighter_background
                        radius: Math.min(4, theme.radius)
                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: 4
                            spacing: 0
                            Text {
                                Layout.fillWidth: true
                                Layout.margins: 8
                                visible: sheet.cluster && sheet.cluster.previewError !== ""
                                wrapMode: Text.Wrap
                                text: sheet.cluster ? sheet.cluster.previewError : ""
                                font.pixelSize: 12
                                color: theme.c.red
                            }
                            Text {
                                Layout.fillWidth: true
                                Layout.margins: 8
                                visible: sheet.cluster && sheet.cluster.changeCount === 0 && !sheet.working
                                text: qsTr("Nothing to change: these files already carry this release's tags.")
                                font.pixelSize: 12
                                color: theme.c.muted
                            }
                            ListView {
                                id: changeList
                                objectName: "changeList"
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                clip: true
                                model: sheet.cluster ? sheet.cluster.changes : []
                                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                                delegate: Column {
                                    id: change
                                    required property var modelData
                                    required property int index
                                    // The file's name above its first change.
                                    readonly property bool first: index === 0 || changeList.model[index - 1].file !== modelData.file
                                    width: ListView.view.width
                                    Text {
                                        width: parent.width
                                        visible: change.first
                                        topPadding: 8
                                        bottomPadding: 2
                                        leftPadding: 8
                                        elide: Text.ElideMiddle
                                        text: change.modelData.file
                                        font.pixelSize: 12
                                        font.bold: true
                                        color: theme.c.bright_foreground
                                    }
                                RowLayout {
                                    width: parent.width
                                    height: 22
                                    spacing: 10
                                    Text {
                                        Layout.leftMargin: 24
                                        Layout.preferredWidth: 190
                                        elide: Text.ElideRight
                                        text: change.modelData.label
                                        font.pixelSize: 12
                                        color: theme.c.muted
                                    }
                                    Text {
                                        Layout.preferredWidth: 280
                                        elide: Text.ElideRight
                                        text: change.modelData.oldValue !== "" ? change.modelData.oldValue : qsTr("(empty)")
                                        font.pixelSize: 12
                                        font.italic: change.modelData.oldValue === ""
                                        color: theme.c.light_foreground
                                    }
                                    Text { text: "→"; font.pixelSize: 12; color: theme.c.muted }
                                    Text {
                                        Layout.fillWidth: true
                                        elide: Text.ElideRight
                                        text: change.modelData.newValue !== "" ? change.modelData.newValue : qsTr("(cleared)")
                                        font.pixelSize: 12
                                        color: change.modelData.field === "error" ? theme.c.red : theme.c.bright_foreground
                                    }
                                }
                                }
                            }
                            Text {
                                Layout.fillWidth: true
                                Layout.margins: 8
                                visible: embedBox.checked && sheet.cluster && sheet.cluster.chosen >= 0
                                text: qsTr("The release's front cover is also fetched from the Cover Art Archive and saved into the files.")
                                font.pixelSize: 12
                                color: theme.c.muted
                            }
                        }
                    }
                }
            }
        }
    }

    footerLeft: [
        Row {
            anchors.verticalCenter: parent.verticalCenter
            spacing: 2
            CheckBox { id: embedBox; text: qsTr("Save the cover into the files"); font.pixelSize: 12 }
            CheckBox { id: coverFileBox; text: qsTr("Also save cover.jpg"); font.pixelSize: 12 }
            CheckBox { id: genresBox; text: qsTr("Genres from MusicBrainz"); font.pixelSize: 12 }
            CheckBox { id: labelBox; text: qsTr("Label and catalogue number"); font.pixelSize: 12 }
        }
    ]
    buttons: [
        Button {
            text: sheet.working ? qsTr("Stop") : qsTr("Cancel")
            flat: true
            onClicked: sheet.working ? library.cancelJob() : sheet.close()
        },
        PrimaryButton {
            id: saveButton
            objectName: "identifySave"
            text: sheet.web.identifyState === "applying" ? qsTr("Saving…")
                  : sheet.includedCount > 1 ? qsTr("Save %n albums", "", sheet.includedCount) : qsTr("Save tags")
            enabled: sheet.web.identifyState === "ready" && sheet.includedCount > 0
            onClicked: sheet.save()
        }
    ]
}
