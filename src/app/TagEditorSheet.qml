import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// The tag editor (Ctrl+E): Details, Artwork and Lyrics for the chosen
// tracks. With several tracks, a field they disagree on shows ‹keep› and
// stays as it is in each file unless you type in it. Save writes only what
// you changed, as one job; Ctrl+Z undoes it.
ToolSheet {
    id: sheet
    objectName: "tagEditor"
    readonly property var ed: library.tagEditor
    title: qsTr("Edit tags")
    subtitle: ed.trackCount > 1 ? ed.heading : ed.heading !== "" ? "—  " + ed.heading : ""
    preferredWidth: 980
    preferredHeight: 700
    dirty: ed.dirty
    onAccepted: save()
    onClosed: ed.close()

    function openFor(ids) {
        if (!ed.load(ids)) return false
        tabs.currentIndex = 0
        open()
        return true
    }
    function save() {
        if (ed.dirty && !ed.applying && !ed.loading) ed.apply()
    }

    Connections {
        target: sheet.ed
        function onApplied(ok) { if (ok) sheet.close() }
    }

    // A field bound to one tag.
    component TagField: FormField {
        required property string tag
        Layout.fillWidth: true
        objectName: "tagField_" + tag
        text: library.tagEditor.revision >= 0 ? library.tagEditor.value(tag) : ""
        mixed: library.tagEditor.revision >= 0 && library.tagEditor.mixed(tag)
        edited: library.tagEditor.revision >= 0 && library.tagEditor.edited(tag)
        placeholder: edited && library.tagEditor.trackCount > 1 ? qsTr("Empty in every track") : ""
        tip: mixed ? qsTr("The tracks have: %1").arg(library.tagEditor.originalValues(tag).join(" · ")) : ""
        readOnly: library.tagEditor.loading
        onEdit: (t) => library.tagEditor.setValue(tag, t)
        onCleared: library.tagEditor.clearField(tag)
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        TabBar {
            id: tabs
            Layout.fillWidth: true
            Layout.leftMargin: 12
            background: null
            TabButton { text: qsTr("Details"); width: implicitWidth + 24 }
            TabButton { text: qsTr("Artwork"); width: implicitWidth + 24 }
            TabButton { text: qsTr("Lyrics"); width: implicitWidth + 24 }
        }

        // What is true of the tracks as a whole.
        Text {
            Layout.fillWidth: true
            Layout.leftMargin: 20
            Layout.rightMargin: 20
            Layout.topMargin: 6
            visible: text !== ""
            wrapMode: Text.Wrap
            text: sheet.ed.loading ? qsTr("Reading the tags…") : sheet.ed.notice
            font.pixelSize: 12
            color: sheet.ed.loading ? theme.c.muted : theme.c.yellow
        }

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.margins: 16
            currentIndex: tabs.currentIndex

            // Details.
            ScrollView {
                id: detailsScroll
                clip: true
                contentWidth: availableWidth
                ColumnLayout {
                    width: detailsScroll.availableWidth
                    spacing: 14

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 28

                        ColumnLayout {
                            Layout.fillWidth: true
                            Layout.preferredWidth: 3
                            Layout.alignment: Qt.AlignTop
                            spacing: 8
                            TagField { tag: "title"; label: qsTr("Title") }
                            TagField { tag: "artist"; label: qsTr("Artists") }
                            TagField { tag: "albumartist"; label: qsTr("Album artist") }
                            TagField { tag: "album"; label: qsTr("Album") }
                            TagField { tag: "genre"; label: qsTr("Genres") }
                            TagField { tag: "composer"; label: qsTr("Composers") }
                            TagField { tag: "comment"; label: qsTr("Comment"); multiline: true; Layout.preferredHeight: 84 }
                            Text {
                                Layout.fillWidth: true
                                Layout.leftMargin: 122
                                wrapMode: Text.Wrap
                                text: qsTr("Separate several artists, composers or genres with a semicolon: Neon Coast; Lia")
                                font.pixelSize: 11
                                color: theme.c.muted
                            }
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            Layout.preferredWidth: 2
                            Layout.alignment: Qt.AlignTop
                            spacing: 8
                            TagField { tag: "year"; label: qsTr("Year"); numeric: true; Layout.maximumWidth: 230 }
                            RowLayout {
                                spacing: 8
                                TagField { tag: "track"; label: qsTr("Track"); numeric: true; Layout.preferredWidth: 186; Layout.fillWidth: false; clearable: false }
                                Text { text: qsTr("of"); font.pixelSize: 13; color: theme.c.light_foreground }
                                TagField { tag: "tracktotal"; label: ""; numeric: true; Layout.preferredWidth: 64; Layout.fillWidth: false; clearable: false }
                            }
                            RowLayout {
                                spacing: 8
                                TagField { tag: "disc"; label: qsTr("Disc"); numeric: true; Layout.preferredWidth: 186; Layout.fillWidth: false; clearable: false }
                                Text { text: qsTr("of"); font.pixelSize: 13; color: theme.c.light_foreground }
                                TagField { tag: "disctotal"; label: ""; numeric: true; Layout.preferredWidth: 64; Layout.fillWidth: false; clearable: false }
                            }
                            TagField { tag: "bpm"; label: qsTr("BPM"); numeric: true; Layout.maximumWidth: 230 }

                            // Rating: stars, or ‹keep› when the tracks differ.
                            RowLayout {
                                spacing: 10
                                Text {
                                    Layout.preferredWidth: 112
                                    horizontalAlignment: Text.AlignRight
                                    text: qsTr("Rating")
                                    font.pixelSize: 13
                                    color: sheet.ed.revision >= 0 && sheet.ed.edited("rating") ? theme.c.bright_foreground : theme.c.light_foreground
                                }
                                ComboBox {
                                    id: rating
                                    objectName: "tagRating"
                                    Layout.preferredWidth: 180
                                    implicitHeight: 34
                                    topInset: 0; bottomInset: 0
                                    font.pixelSize: 13
                                    enabled: !sheet.ed.loading
                                    textRole: "text"
                                    valueRole: "value"
                                    model: [
                                        { text: qsTr("Unrated"), value: "" },
                                        { text: qsTr("No stars"), value: "0" },
                                        { text: "½", value: "10" }, { text: "★", value: "20" },
                                        { text: "★½", value: "30" }, { text: "★★", value: "40" },
                                        { text: "★★½", value: "50" }, { text: "★★★", value: "60" },
                                        { text: "★★★½", value: "70" }, { text: "★★★★", value: "80" },
                                        { text: "★★★★½", value: "90" }, { text: "★★★★★", value: "100" }
                                    ]
                                    readonly property bool keep: sheet.ed.revision >= 0 && sheet.ed.mixed("rating")
                                    currentIndex: {
                                        if (sheet.ed.revision < 0 || keep) return -1
                                        const v = sheet.ed.value("rating")
                                        if (v === "") return 0
                                        // The nearest half star.
                                        return Math.max(1, Math.min(11, Math.round(Number(v) / 10) + 1))
                                    }
                                    displayText: keep ? qsTr("‹keep›") : currentText
                                    onActivated: (i) => sheet.ed.setValue("rating", model[i].value)
                                }
                            }
                            Text {
                                Layout.fillWidth: true
                                Layout.leftMargin: 122
                                wrapMode: Text.Wrap
                                text: qsTr("Ratings stay in the library, not in the files, unless you turn that on.")
                                font.pixelSize: 11
                                color: theme.c.muted
                            }

                            Item { Layout.preferredHeight: 6 }
                            TagField { tag: "custom1"; label: qsTr("Custom 1") }
                            TagField { tag: "custom2"; label: qsTr("Custom 2") }
                            TagField { tag: "custom3"; label: qsTr("Custom 3") }
                            TagField { tag: "custom4"; label: qsTr("Custom 4") }
                            TagField { tag: "custom5"; label: qsTr("Custom 5") }
                        }
                    }

                    // Files that failed in the last save, each with why.
                    ColumnLayout {
                        Layout.fillWidth: true
                        visible: sheet.ed.failures.length > 0
                        spacing: 4
                        Text {
                            text: qsTr("These files were not changed")
                            font.pixelSize: 13
                            font.bold: true
                            color: theme.c.red
                        }
                        Repeater {
                            model: sheet.ed.failures
                            Text {
                                required property var modelData
                                Layout.fillWidth: true
                                wrapMode: Text.Wrap
                                text: modelData.path + "\n    " + modelData.error
                                font.pixelSize: 12
                                color: theme.c.foreground
                            }
                        }
                    }
                }
            }

            // Artwork.
            RowLayout {
                spacing: 24
                Rectangle {
                    Layout.preferredWidth: 320
                    Layout.preferredHeight: 320
                    Layout.alignment: Qt.AlignTop
                    color: theme.c.darker_background
                    border.width: 1
                    border.color: theme.c.lighter_background
                    radius: Math.min(4, theme.radius)
                    Image {
                        id: art
                        objectName: "tagArtwork"
                        anchors.fill: parent
                        anchors.margins: 6
                        fillMode: Image.PreserveAspectFit
                        asynchronous: true
                        source: sheet.ed.artworkSource
                        opacity: sheet.ed.artworkAction === "remove" ? 0.25 : 1
                    }
                    Text {
                        anchors.centerIn: parent
                        visible: art.source == "" || art.status !== Image.Ready
                        text: sheet.ed.loading ? qsTr("Reading…") : qsTr("No picture")
                        font.pixelSize: 13
                        color: theme.c.muted
                    }
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignTop
                    spacing: 10
                    Text {
                        objectName: "tagArtworkNote"
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        text: sheet.ed.artworkNote
                        font.pixelSize: 13
                        color: theme.c.foreground
                    }
                    Text {
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        text: qsTr("A new picture replaces every picture in the files as the front cover. Folder pictures such as cover.jpg are left as they are.")
                        font.pixelSize: 12
                        color: theme.c.muted
                    }
                    Flow {
                        Layout.fillWidth: true
                        spacing: 8
                        Button { text: qsTr("Choose a picture…"); enabled: !sheet.ed.loading; onClicked: sheet.ed.pickArtwork() }
                        Button { text: qsTr("Paste"); enabled: !sheet.ed.loading; onClicked: sheet.ed.pasteArtwork()
                                 ToolTip.visible: hovered; ToolTip.delay: 600; ToolTip.text: qsTr("A picture copied from a browser or an image file copied in the file manager") }
                        Button { text: qsTr("Remove"); flat: true; enabled: !sheet.ed.loading && sheet.ed.artworkAction !== "remove"; onClicked: sheet.ed.removeArtwork() }
                        Button { text: qsTr("Keep as it is"); flat: true; visible: sheet.ed.artworkAction !== "keep"; onClicked: sheet.ed.keepArtwork() }
                    }
                }
            }

            // Lyrics.
            ColumnLayout {
                spacing: 8
                Text {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    text: qsTr("Plain lyrics, kept in the file (USLT in MP3s, LYRICS in FLAC and Ogg).")
                    font.pixelSize: 12
                    color: theme.c.muted
                }
                TagField {
                    tag: "lyrics"
                    label: ""
                    multiline: true
                    Layout.fillHeight: true
                }
            }
        }
    }

    footerLeft: Text {
        anchors.verticalCenter: parent.verticalCenter
        width: parent.width
        elide: Text.ElideRight
        text: sheet.ed.applying ? qsTr("Saving…  (the jobs bar shows progress)")
              : sheet.ed.resultText !== "" ? sheet.ed.resultText
              : sheet.ed.dirty ? qsTr("Ctrl+Enter saves") : ""
        font.pixelSize: 12
        color: sheet.ed.failures.length > 0 ? theme.c.red : theme.c.light_foreground
    }
    buttons: [
        Button {
            text: qsTr("Revert")
            flat: true
            enabled: sheet.ed.dirty && !sheet.ed.applying
            onClicked: sheet.ed.revertAll()
        },
        Button {
            text: qsTr("Cancel")
            flat: true
            onClicked: sheet.askToClose()
        },
        PrimaryButton {
            objectName: "tagSave"
            text: sheet.ed.trackCount > 1 ? qsTr("Save %n tracks", "", sheet.ed.trackCount) : qsTr("Save")
            highlighted: true
            enabled: sheet.ed.dirty && !sheet.ed.applying && !sheet.ed.loading
            onClicked: sheet.save()
        }
    ]
}
