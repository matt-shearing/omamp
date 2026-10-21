import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Settings > Appearance: whether Thrum shares its visualiser with the
// Omarchy bar plugin (levelsfeed.h). Its own file, so the Appearance page
// needs one line for it.
ColumnLayout {
    id: feedSettings
    spacing: 4
    readonly property bool available: typeof levelsFeed !== "undefined" && levelsFeed !== null
    visible: available

    CheckBox {
        objectName: "shareWithBar"
        text: qsTr("Share the visualiser with the Omarchy bar")
        font.pixelSize: 13
        checked: feedSettings.available && levelsFeed.enabled
        onClicked: levelsFeed.enabled = checked
    }
    Text {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        font.pixelSize: 12
        color: theme.c.muted
        text: qsTr("The Thrum widget in the Omarchy bar can draw a small, faint copy of this visualiser behind the track's name. Nothing is sent unless the bar asks and music is playing.")
    }
}
