import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Settings > Appearance > Layout: how the library window is laid out. The
// Art column and the row height, the sidebar and its sections, the now
// playing bar's size and place, and Reset layout. Everything applies as it
// is chosen and is kept in window.conf.
ColumnLayout {
    id: page
    objectName: "layoutSettings"
    spacing: 4
    readonly property var layoutPrefs: appearance.layout

    component Heading: Text {
        Layout.fillWidth: true
        Layout.topMargin: 10
        font.pixelSize: 14
        font.bold: true
        color: theme.c.bright_foreground
    }
    component Sub: Text {
        Layout.fillWidth: true
        Layout.topMargin: 6
        font.pixelSize: 13
        font.bold: true
        color: theme.c.foreground
    }
    component Note: Text {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        font.pixelSize: 12
        color: theme.c.muted
    }

    Heading { text: qsTr("Layout") }
    Note { text: qsTr("How the library window is laid out. The View menu switches most of these too.") }

    Sub { text: qsTr("Track lists") }
    CheckBox {
        objectName: "layoutArtColumn"
        text: qsTr("Show a small cover before each track (the Art column)")
        font.pixelSize: 13
        checked: page.layoutPrefs.artColumn
        onClicked: page.layoutPrefs.artColumn = checked
    }
    RowLayout {
        spacing: 4
        Text { text: qsTr("Row height:"); font.pixelSize: 13; color: theme.c.foreground; rightPadding: 6 }
        Repeater {
            model: [["compact", qsTr("Compact")], ["comfortable", qsTr("Comfortable")], ["large", qsTr("Large")]]
            RadioButton {
                required property var modelData
                objectName: "rowHeight_" + modelData[0]
                text: modelData[1]
                font.pixelSize: 13
                checked: page.layoutPrefs.rowHeight === modelData[0]
                onClicked: page.layoutPrefs.rowHeight = modelData[0]
            }
        }
    }
    Note { text: qsTr("Taller rows show bigger covers. Drag a column's header to move it; right-click a header to choose columns.") }

    Sub { text: qsTr("Sidebar") }
    CheckBox {
        objectName: "layoutSidebar"
        text: qsTr("Show the sidebar (I)")
        font.pixelSize: 13
        checked: page.layoutPrefs.sidebarShown
        onClicked: page.layoutPrefs.toggleSidebar()
    }
    RowLayout {
        spacing: 4
        Text { text: qsTr("It shows:"); font.pixelSize: 13; color: theme.c.foreground; rightPadding: 6 }
        RadioButton {
            objectName: "followSelection"
            text: qsTr("the chosen track")
            font.pixelSize: 13
            checked: page.layoutPrefs.follow === "selection"
            onClicked: page.layoutPrefs.follow = "selection"
        }
        RadioButton {
            objectName: "followPlaying"
            text: qsTr("the playing track")
            font.pixelSize: 13
            checked: page.layoutPrefs.follow === "playing"
            onClicked: page.layoutPrefs.follow = "playing"
        }
    }
    CheckBox {
        objectName: "layoutOnlineInfo"
        text: qsTr("Look up the artist and album on MusicBrainz, Wikidata and Wikipedia")
        font.pixelSize: 13
        checked: page.layoutPrefs.onlineInfo
        onClicked: page.layoutPrefs.onlineInfo = checked
    }
    Note { text: qsTr("Notes that come with an album (album.txt, notes.md, info.txt, README, .nfo, liner notes) always show first. Answers are kept for 30 days.") }
    Text { text: qsTr("Sections, top to bottom:"); font.pixelSize: 13; color: theme.c.foreground; Layout.topMargin: 4 }
    SectionOrder { Layout.fillWidth: true; Layout.maximumWidth: 420 }

    Sub { text: qsTr("Now playing bar") }
    RowLayout {
        spacing: 4
        Text { text: qsTr("Size:"); font.pixelSize: 13; color: theme.c.foreground; rightPadding: 6 }
        RadioButton {
            objectName: "barSize_compact"
            text: qsTr("Compact strip")
            font.pixelSize: 13
            checked: page.layoutPrefs.barSize === "compact"
            onClicked: page.layoutPrefs.barSize = "compact"
        }
        RadioButton {
            objectName: "barSize_large"
            text: qsTr("Large")
            font.pixelSize: 13
            checked: page.layoutPrefs.barSize === "large"
            onClicked: page.layoutPrefs.barSize = "large"
        }
    }
    RowLayout {
        spacing: 4
        Text { text: qsTr("Position:"); font.pixelSize: 13; color: theme.c.foreground; rightPadding: 6 }
        RadioButton {
            objectName: "barPosition_top"
            text: qsTr("Top")
            font.pixelSize: 13
            checked: page.layoutPrefs.barPosition === "top"
            onClicked: page.layoutPrefs.barPosition = "top"
        }
        RadioButton {
            objectName: "barPosition_bottom"
            text: qsTr("Bottom")
            font.pixelSize: 13
            checked: page.layoutPrefs.barPosition === "bottom"
            onClicked: page.layoutPrefs.barPosition = "bottom"
        }
    }
    Note { text: qsTr("The large bar has a big play button, the position across the middle and the volume on the right, as in Spotify's desktop app.") }

    Button {
        objectName: "resetLayout"
        Layout.topMargin: 8
        text: qsTr("Reset layout")
        font.pixelSize: 13
        onClicked: page.layoutPrefs.reset()
    }
    Note { text: qsTr("Puts the columns, the sidebar and the bar back as they start.") }
}
