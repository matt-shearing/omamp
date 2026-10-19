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
                  : library.currentKind === "duplicates"
                  ? (library.duplicates.searched ? library.duplicates.summary : qsTr("Duplicates"))
                  : library.currentKind === "playlist" || library.currentKind === "autoplaylist" || library.currentKind === "plfolder"
                  ? qsTr("%Ln entries", "", library.playlists.items.count)
                    + (library.playlists.items.selectedCount > 1 ? "  ·  " + qsTr("%Ln selected", "", library.playlists.items.selectedCount) : "")
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
            // What was asked for meanwhile; it starts when this one ends.
            Text {
                objectName: "jobsWaiting"
                visible: library.waitingText !== ""
                Layout.maximumWidth: 300
                elide: Text.ElideRight
                text: "·  " + library.waitingText
                font.pixelSize: 12
                color: theme.c.light_foreground
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

        // The folder watcher, and the covers being made, when no scan runs.
        RowLayout {
            objectName: "watchStatus"
            visible: !library.busy && (watchText.text !== "" || library.artworkBusy)
            spacing: 8
            Rectangle { width: 1; height: 16; color: theme.c.lighter_background }
            Text {
                id: watchText
                readonly property var w: library.watcher
                Layout.maximumWidth: 420
                elide: Text.ElideRight
                text: !w ? ""
                      : w.busy ? qsTr("Updating changed folders…")
                      : w.degraded ? qsTr("Watching %Ln folders; the rest are checked every 15 minutes", "", w.watchedFolders)
                      : w.running && w.watchedFolders > 0
                        ? qsTr("Watching %Ln folders", "", w.watchedFolders)
                          + (w.networkRoots.length > 0 ? "  ·  " + qsTr("%Ln shares checked every 15 minutes", "", w.networkRoots.length) : "")
                      : w.networkRoots.length > 0 ? qsTr("%Ln shares checked every 15 minutes", "", w.networkRoots.length)
                      : ""
                font.pixelSize: 12
                color: w && w.degraded ? theme.c.yellow : theme.c.light_foreground
            }
            Text {
                visible: library.artworkBusy
                text: "·  " + qsTr("Finding album covers…")
                font.pixelSize: 12
                color: theme.c.light_foreground
            }
        }

        // A share that came back, and its rescan.
        RowLayout {
            objectName: "shareNote"
            visible: library.nas.shareNote !== ""
            spacing: 8
            Rectangle { width: 1; height: 16; color: theme.c.lighter_background }
            Text {
                Layout.maximumWidth: 420
                elide: Text.ElideMiddle
                text: library.nas.shareNote
                font.pixelSize: 12
                color: theme.c.green
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
