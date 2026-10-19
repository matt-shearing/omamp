import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// The status line under the library: how many tracks are showing, and the
// running job (a scan) with its progress and a Cancel button. The result of
// the last job, and anything that went wrong, shows on the right until
// dismissed.
Rectangle {
    id: bar
    implicitHeight: 30
    color: theme.c.dark_background
    Rectangle { anchors { left: parent.left; right: parent.right; top: parent.top }
                height: 1; color: theme.c.lighter_background }

    readonly property var tracks: library.tracks
    readonly property string note: backend.error !== "" ? backend.error : library.error !== "" ? library.error : library.message
    readonly property bool noteIsError: backend.error !== "" || library.error !== ""

    RowLayout {
        anchors { fill: parent; leftMargin: 10; rightMargin: 6 }
        spacing: 10

        Text {
            text: library.currentKind === "nowplaying"
                  ? qsTr("%Ln tracks in Now Playing", "", backend.playlist.count)
                  : qsTr("%Ln tracks", "", bar.tracks.count)
                    + (bar.tracks.selectedCount > 1 ? "  ·  " + qsTr("%Ln selected", "", bar.tracks.selectedCount) : "")
            font.pixelSize: 12
            color: theme.c.light_foreground
        }

        // The running job.
        RowLayout {
            visible: library.busy
            spacing: 8
            Rectangle { width: 1; height: 16; color: theme.c.lighter_background }
            ProgressBar {
                Layout.preferredWidth: 160
                indeterminate: library.jobProgress < 0
                value: Math.max(0, library.jobProgress)
            }
            Text {
                Layout.maximumWidth: 360
                elide: Text.ElideMiddle
                text: library.jobText
                font.pixelSize: 12
                color: theme.c.foreground
            }
            Button {
                text: qsTr("Cancel")
                flat: true
                implicitHeight: 28
                topInset: 0; bottomInset: 0
                font.pixelSize: 12
                onClicked: library.cancelJob()
            }
        }

        Item { Layout.fillWidth: true }

        Text {
            Layout.maximumWidth: bar.width / 2
            visible: bar.note !== ""
            elide: Text.ElideRight
            text: bar.note
            font.pixelSize: 12
            color: bar.noteIsError ? theme.c.red : theme.c.foreground
        }
        ToolButton {
            visible: bar.note !== ""
            text: "✕"
            implicitWidth: 28; implicitHeight: 28
            topInset: 0; bottomInset: 0
            font.pixelSize: 11
            onClicked: { backend.dismissError(); library.dismissMessage() }
        }
    }

    Timer {
        running: library.message !== "" && !library.busy
        interval: 10000
        onTriggered: library.dismissMessage()
    }
}
