import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Connect a NAS: the share's name and where to mount it, and the exact
// steps to make Linux mount it on demand. Thrum never runs sudo or mounts
// anything itself; it writes out the commands, each with a Copy button, for
// you to run in a terminal. The full guide (docs/nas.md) is on the second tab.
ToolSheet {
    id: sheet
    objectName: "nasGuideSheet"
    readonly property var nas: library.nas
    title: qsTr("Connect a NAS")
    preferredWidth: 1040
    preferredHeight: 800
    readonly property string protocol: nfsMode.checked ? "nfs" : "smb"
    readonly property var guide: share.text.trim() === "" ? ({}) : nas.mountGuide(protocol, share.text, mountPoint.text)

    function openSheet() {
        library.activate()
        tabs.currentIndex = 0
        open()
        share.forceActiveFocus()
    }
    function copy(text) {
        nas.copyText(text)
        copied.text = qsTr("Copied")
        copiedTimer.restart()
    }

    component CodeBox: Rectangle {
        property alias text: code.text
        Layout.fillWidth: true
        implicitHeight: code.implicitHeight + 16
        radius: Math.min(4, theme.radius)
        color: theme.c.darker_background
        border.width: 1
        border.color: theme.c.lighter_background
        TextEdit {
            id: code
            anchors { left: parent.left; right: copyButton.left; top: parent.top; margins: 8 }
            readOnly: true
            selectByMouse: true
            wrapMode: TextEdit.WrapAnywhere
            font.family: "JetBrainsMono Nerd Font"
            font.pixelSize: 12
            color: theme.c.foreground
            selectionColor: theme.c.selection
        }
        Button {
            id: copyButton
            anchors { right: parent.right; top: parent.top; rightMargin: 4 }
            text: qsTr("Copy")
            flat: true
            font.pixelSize: 12
            implicitHeight: 30; topInset: 0; bottomInset: 0
            onClicked: sheet.copy(code.text)
        }
    }
    component Step: Text {
        Layout.fillWidth: true
        Layout.topMargin: 10
        wrapMode: Text.Wrap
        font.pixelSize: 14
        font.bold: true
        color: theme.c.bright_foreground
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        TabBar {
            id: tabs
            Layout.fillWidth: true
            Layout.leftMargin: 12
            TabButton { text: qsTr("Steps"); font.pixelSize: 13; width: implicitWidth + 24 }
            TabButton { text: qsTr("The full guide"); font.pixelSize: 13; width: implicitWidth + 24 }
        }
        Rectangle { Layout.fillWidth: true; height: 1; color: theme.c.lighter_background }

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: tabs.currentIndex

            ScrollView {
                clip: true
                contentWidth: availableWidth
                ColumnLayout {
                    width: parent.width - 40
                    x: 20
                    spacing: 6

                    Text {
                        Layout.fillWidth: true
                        Layout.topMargin: 12
                        wrapMode: Text.Wrap
                        text: qsTr("Thrum reads music from a NAS through a folder that Linux mounts. These steps make it mount by itself the first time Thrum looks, and never hold up start-up when the NAS is off. Thrum never runs these commands or asks for your password: you run them in a terminal.")
                        font.pixelSize: 13
                        color: theme.c.foreground
                    }
                    RowLayout {
                        Layout.topMargin: 6
                        spacing: 16
                        Text { text: qsTr("The NAS shares it by"); font.pixelSize: 13; color: theme.c.light_foreground }
                        RadioButton { id: smbMode; text: qsTr("SMB (Windows sharing; most NAS boxes)"); font.pixelSize: 13; checked: true }
                        RadioButton { id: nfsMode; objectName: "nfsMode"; text: qsTr("NFS"); font.pixelSize: 13 }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 12
                        TextField {
                            id: share
                            objectName: "nasShare"
                            Layout.fillWidth: true
                            placeholderText: nfsMode.checked ? qsTr("Export, for example nas:/volume1/music") : qsTr("Share, for example //nas/music")
                            font.pixelSize: 13
                            selectByMouse: true
                        }
                        TextField {
                            id: mountPoint
                            objectName: "nasMountPoint"
                            Layout.preferredWidth: 280
                            placeholderText: qsTr("Mount it on")
                            text: "/mnt/music"
                            font.pixelSize: 13
                            selectByMouse: true
                        }
                    }
                    Text {
                        objectName: "nasGuideError"
                        Layout.fillWidth: true
                        visible: sheet.guide.ok === false
                        wrapMode: Text.Wrap
                        text: sheet.guide.error || ""
                        font.pixelSize: 13
                        color: theme.c.red
                    }
                    Text {
                        Layout.fillWidth: true
                        visible: sheet.guide.ok === undefined
                        text: qsTr("Enter the share to see the steps.")
                        font.pixelSize: 13
                        color: theme.c.muted
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        visible: sheet.guide.ok === true
                        spacing: 6

                        Step { text: qsTr("1. Save these two files in ~/thrum-mount-units") }
                        Text {
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            text: qsTr("They tell systemd what to mount and to do it on first use. The names matter: they spell the folder it's mounted on.")
                            font.pixelSize: 12
                            color: theme.c.muted
                        }
                        Repeater {
                            model: sheet.guide.units || []
                            ColumnLayout {
                                required property var modelData
                                Layout.fillWidth: true
                                spacing: 2
                                Text {
                                    text: modelData.name
                                    font.family: "JetBrainsMono Nerd Font"
                                    font.pixelSize: 12
                                    font.bold: true
                                    color: theme.c.light_foreground
                                }
                                CodeBox { text: modelData.text.trim() }
                            }
                        }

                        Step { text: qsTr("2. Run these commands in a terminal, one at a time") }
                        Text {
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            text: sheet.protocol === "smb"
                                  ? qsTr("They install the SMB tools, keep the NAS password in a file only root can read (sudoedit opens it: write username=… and password=… on two lines), and switch the mount on.")
                                  : qsTr("They install the NFS tools and switch the mount on. The NAS must allow this computer in the export's settings.")
                            font.pixelSize: 12
                            color: theme.c.muted
                        }
                        Repeater {
                            objectName: "nasCommands"
                            model: sheet.guide.commands || []
                            CodeBox { required property string modelData; text: modelData }
                        }
                        Button {
                            text: qsTr("Copy every command")
                            flat: true
                            font.pixelSize: 13
                            onClicked: sheet.copy((sheet.guide.commands || []).join("\n"))
                        }

                        Step { text: qsTr("Or, instead of the two files, one line in /etc/fstab") }
                        CodeBox { text: sheet.guide.fstab || "" }

                        Step { text: qsTr("3. Add the folder to the library") }
                        RowLayout {
                            spacing: 10
                            Text {
                                Layout.fillWidth: true
                                wrapMode: Text.Wrap
                                text: qsTr("Once %1 shows your music, add it as a library folder. Thrum marks the share so it knows it again, even if it's later mounted somewhere else.").arg(mountPoint.text)
                                font.pixelSize: 12
                                color: theme.c.muted
                            }
                            Button { text: qsTr("Add a folder…"); font.pixelSize: 13; onClicked: library.addFolder() }
                        }
                        Item { Layout.preferredHeight: 12 }
                    }
                }
            }

            ScrollView {
                clip: true
                contentWidth: availableWidth
                TextEdit {
                    objectName: "nasGuideText"
                    width: parent.width - 40
                    x: 20
                    topPadding: 12
                    bottomPadding: 12
                    readOnly: true
                    selectByMouse: true
                    wrapMode: TextEdit.Wrap
                    textFormat: TextEdit.MarkdownText
                    text: tabs.currentIndex === 1 ? sheet.nas.nasGuideText() : ""
                    font.pixelSize: 13
                    color: theme.c.foreground
                    selectionColor: theme.c.selection
                    onLinkActivated: (link) => Qt.openUrlExternally(link)
                }
            }
        }
    }

    footerLeft: [
        Text {
            id: copied
            anchors.verticalCenter: parent.verticalCenter
            font.pixelSize: 12
            color: theme.c.green
            Timer { id: copiedTimer; interval: 1500; onTriggered: copied.text = "" }
        }
    ]
    buttons: [
        PrimaryButton { text: qsTr("Done"); onClicked: sheet.close() }
    ]
}
