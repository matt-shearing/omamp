import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// "Find cover…" found more than one: the covers side by side, with the
// release each came from. Clicking one (or Enter on the chosen one) saves it
// for the album, and into the files when Settings → Web says so.
ToolSheet {
    id: sheet
    objectName: "coverChooser"
    readonly property var web: library.web
    property int chosen: 0
    title: qsTr("Choose a cover")
    subtitle: web.coverAlbum
    preferredWidth: 900
    preferredHeight: 560
    onOpened: { chosen = 0; grid.forceActiveFocus() }
    onAccepted: use(chosen)
    onClosed: web.clearCovers()

    function use(i) {
        if (i < 0 || i >= web.coverCandidates.length) return
        close()
        web.chooseCover(i)
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 10
        Text {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: qsTr("These come from the Cover Art Archive, for the MusicBrainz releases that match the album. Pick the one that looks like your copy.")
            font.pixelSize: 13
            color: theme.c.foreground
        }
        GridView {
            id: grid
            objectName: "coverGrid"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            cellWidth: 200
            cellHeight: 262
            model: sheet.web.coverCandidates
            currentIndex: sheet.chosen
            keyNavigationEnabled: true
            onCurrentIndexChanged: sheet.chosen = currentIndex
            Keys.onReturnPressed: sheet.use(currentIndex)
            Keys.onEnterPressed: sheet.use(currentIndex)
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
            delegate: Rectangle {
                id: tile
                required property var modelData
                required property int index
                width: 188
                height: 250
                radius: Math.min(4, theme.radius)
                color: index === sheet.chosen ? Qt.alpha(theme.accent, 0.18) : hover.hovered ? Qt.alpha(theme.c.selection, 0.5) : "transparent"
                border.width: index === sheet.chosen ? 2 : 0
                border.color: theme.accent
                HoverHandler { id: hover }
                MouseArea {
                    anchors.fill: parent
                    onClicked: sheet.chosen = tile.index
                    onDoubleClicked: sheet.use(tile.index)
                }
                Column {
                    anchors { fill: parent; margins: 8 }
                    spacing: 4
                    Image {
                        width: parent.width
                        height: width
                        fillMode: Image.PreserveAspectFit
                        asynchronous: true
                        source: tile.modelData.preview
                    }
                    Text {
                        width: parent.width
                        elide: Text.ElideRight
                        text: tile.modelData.title
                        font.pixelSize: 12
                        font.bold: true
                        color: theme.c.bright_foreground
                    }
                    Text {
                        width: parent.width
                        elide: Text.ElideRight
                        text: [tile.modelData.date, tile.modelData.country, tile.modelData.format].filter(x => x).join("  ·  ")
                              || (tile.modelData.how === "tags" ? qsTr("The release in the tags") : "")
                        font.pixelSize: 11
                        color: theme.c.muted
                    }
                }
            }
        }
    }

    buttons: [
        Button { text: qsTr("Cancel"); flat: true; onClicked: sheet.close() },
        PrimaryButton {
            objectName: "useCover"
            text: qsTr("Use this cover")
            enabled: sheet.web.coverCandidates.length > 0
            onClicked: sheet.use(sheet.chosen)
        }
    ]
}
