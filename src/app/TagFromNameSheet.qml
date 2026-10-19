import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Tag from file names: a pattern read against the end of each file's path,
// with a live preview of what each track would get. Tag writes them as one
// job; Ctrl+Z undoes it.
ToolSheet {
    id: sheet
    objectName: "tagFromNameSheet"
    readonly property var tfn: library.tagFromName
    title: qsTr("Tag from file names")
    subtitle: qsTr("%n tracks", "", tfn.count)
    preferredWidth: 1080
    preferredHeight: 680
    onAccepted: if (applyButton.enabled) tfn.apply()
    onClosed: tfn.close()

    function openFor(ids) {
        if (!tfn.load(ids)) return false
        open()
        patternField.focusInput()
        return true
    }

    Connections {
        target: sheet.tfn
        function onApplied(ok) { if (ok) sheet.close() }
    }

    readonly property int fileColumn: 340

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 10

        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            FormField {
                id: patternField
                objectName: "tagFromNamePattern"
                Layout.fillWidth: true
                label: qsTr("Pattern")
                monospace: true
                clearable: false
                text: sheet.tfn.pattern
                onEdit: (t) => sheet.tfn.pattern = t
            }
            ComboBox {
                Layout.preferredWidth: 300
                implicitHeight: 34
                topInset: 0; bottomInset: 0
                font.pixelSize: 13
                model: sheet.tfn.presets
                displayText: qsTr("Presets")
                onActivated: (i) => sheet.tfn.pattern = sheet.tfn.presets[i]
            }
        }
        Text {
            Layout.fillWidth: true
            Layout.leftMargin: 122
            wrapMode: Text.Wrap
            text: sheet.tfn.patternError !== ""
                  ? (sheet.tfn.patternErrorPosition >= 0 ? qsTr("%1, at character %2").arg(sheet.tfn.patternError).arg(sheet.tfn.patternErrorPosition + 1) : sheet.tfn.patternError)
                  : qsTr("Use <Track#>, <Title>, <Artist>, <Album>, <Album Artist>, <Year>, <Disc#>, <Genre>, and <Skip> for a part to ignore. A / reads the folder above.")
            font.pixelSize: 12
            color: sheet.tfn.patternError !== "" ? theme.c.red : theme.c.muted
        }

        Text {
            Layout.fillWidth: true
            text: qsTr("%1 of %2 file names match").arg(sheet.tfn.matched).arg(sheet.tfn.count)
            font.pixelSize: 13
            color: theme.c.light_foreground
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: theme.c.darker_background
            border.width: 1
            border.color: theme.c.lighter_background
            radius: Math.min(4, theme.radius)
            clip: true
            readonly property real fieldWidth: sheet.tfn.fields.length > 0
                                               ? Math.max(90, (width - sheet.fileColumn - 24) / sheet.tfn.fields.length) : 0

            Rectangle {
                id: head
                anchors { left: parent.left; right: parent.right; top: parent.top; margins: 1 }
                height: 28
                color: theme.c.dark_background
                Row {
                    anchors.fill: parent
                    anchors.leftMargin: 10
                    Text {
                        width: sheet.fileColumn; height: 28
                        verticalAlignment: Text.AlignVCenter
                        text: qsTr("File")
                        font.pixelSize: 12; font.bold: true
                        color: theme.c.light_foreground
                    }
                    Repeater {
                        model: sheet.tfn.fieldLabels
                        Text {
                            required property string modelData
                            width: head.parent.fieldWidth; height: 28
                            verticalAlignment: Text.AlignVCenter
                            text: modelData
                            elide: Text.ElideRight
                            font.pixelSize: 12; font.bold: true
                            color: theme.c.light_foreground
                        }
                    }
                }
            }

            ListView {
                id: rows
                objectName: "tagFromNamePreview"
                anchors { left: parent.left; right: parent.right; top: head.bottom; bottom: parent.bottom; margins: 1 }
                clip: true
                model: sheet.tfn
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                delegate: Rectangle {
                    id: line
                    required property int index
                    required property string path
                    required property bool matched
                    required property string error
                    required property var values
                    width: ListView.view.width
                    height: 26
                    color: index % 2 ? Qt.alpha(theme.c.lighter_background, 0.3) : "transparent"
                    Row {
                        anchors.fill: parent
                        anchors.leftMargin: 10
                        Text {
                            width: sheet.fileColumn; height: parent.height
                            verticalAlignment: Text.AlignVCenter
                            rightPadding: 12
                            text: line.path
                            elide: Text.ElideLeft
                            font.pixelSize: 12
                            color: theme.c.foreground
                        }
                        Text {
                            visible: !line.matched
                            height: parent.height
                            verticalAlignment: Text.AlignVCenter
                            text: line.error
                            font.pixelSize: 12
                            color: theme.c.yellow
                        }
                        Repeater {
                            model: line.matched ? line.values : []
                            Text {
                                required property string modelData
                                width: rows.parent.fieldWidth; height: 26
                                verticalAlignment: Text.AlignVCenter
                                rightPadding: 10
                                text: modelData
                                elide: Text.ElideRight
                                font.pixelSize: 12
                                color: theme.c.bright_foreground
                            }
                        }
                    }
                }
            }
        }
    }

    footerLeft: Text {
        anchors.verticalCenter: parent.verticalCenter
        width: parent.width
        elide: Text.ElideRight
        text: sheet.tfn.applying ? qsTr("Writing tags…") : sheet.tfn.resultText !== "" ? sheet.tfn.resultText
              : qsTr("Only matching files are tagged; the others are left alone.")
        font.pixelSize: 12
        color: theme.c.light_foreground
    }
    buttons: [
        Button { text: qsTr("Cancel"); flat: true; onClicked: sheet.close() },
        PrimaryButton {
            id: applyButton
            objectName: "tagFromNameApply"
            text: qsTr("Tag %n files", "", sheet.tfn.matched)
            highlighted: true
            enabled: sheet.tfn.matched > 0 && sheet.tfn.patternError === "" && !sheet.tfn.applying
            onClicked: sheet.tfn.apply()
        }
    ]
}
