import QtQuick
import QtQuick.Layouts

// In place of the position bar while a radio station plays: a station
// has no length and can't be seeked, so the bar becomes the dial. The
// arrows (and [ and ]) step through the channels of the station's group,
// the star keeps it as a favourite, the name opens the channel picker.
Item {
    id: live
    property var skin: appearance.plain
    property int fontSize: 11
    signal openPicker()

    readonly property string mono: "JetBrainsMono Nerd Font"
    implicitHeight: Math.max(16, fontSize + 6)

    function clock(ms) {
        if (ms === undefined || ms < 0 || isNaN(ms)) ms = 0
        const total = Math.floor(ms / 1000)
        const h = Math.floor(total / 3600), m = Math.floor(total % 3600 / 60), s = total % 60
        const mm = (h > 0 && m < 10 ? "0" : "") + m
        return (h > 0 ? h + ":" : "") + (mm.length < 2 ? "0" + mm : mm) + ":" + (s < 10 ? "0" : "") + s
    }

    RowLayout {
        anchors.fill: parent
        spacing: 6

        Text {
            objectName: "previousChannel"
            text: String.fromCodePoint(0xF0141) // chevron-left
            font.family: live.mono; font.pixelSize: live.fontSize + 3
            color: prevArea.containsMouse ? live.skin.accent : live.skin.label
            MouseArea { id: prevArea; anchors.fill: parent; anchors.margins: -4; hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor; onClicked: radio.step(-1) }
        }

        // LIVE, as a broadcast shows it.
        Rectangle {
            objectName: "liveBadge"
            Layout.preferredWidth: badge.implicitWidth + 10
            Layout.preferredHeight: live.fontSize + 5
            radius: Math.min(3, live.skin.radius)
            color: live.skin.error
            Text {
                id: badge
                anchors.centerIn: parent
                text: qsTr("LIVE")
                font.family: live.mono; font.pixelSize: live.fontSize - 1; font.bold: true
                color: "white"
            }
        }

        Text {
            Layout.fillWidth: true
            elide: Text.ElideRight
            text: radio.active ? radio.current.fullName : backend.title
            font.family: live.mono; font.pixelSize: live.fontSize
            color: nameArea.containsMouse ? live.skin.accent : live.skin.label
            MouseArea { id: nameArea; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                        onClicked: live.openPicker() }
        }

        Text {
            text: qsTr("listening %1").arg(live.clock(backend.position))
            font.family: live.mono; font.pixelSize: live.fontSize - 1
            color: live.skin.labelDim
        }

        Text {
            objectName: "favouriteChannel"
            visible: radio.active
            text: String.fromCodePoint(radio.active && radio.current.favourite ? 0xF04CE : 0xF04D2) // star, star-outline
            font.family: live.mono; font.pixelSize: live.fontSize + 2
            color: radio.active && radio.current.favourite ? live.skin.accent : live.skin.labelDim
            MouseArea { anchors.fill: parent; anchors.margins: -3; cursorShape: Qt.PointingHandCursor
                        onClicked: radio.toggleFavourite(radio.currentKey) }
        }

        Text {
            objectName: "nextChannel"
            text: String.fromCodePoint(0xF0142) // chevron-right
            font.family: live.mono; font.pixelSize: live.fontSize + 3
            color: nextArea.containsMouse ? live.skin.accent : live.skin.label
            MouseArea { id: nextArea; anchors.fill: parent; anchors.margins: -4; hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor; onClicked: radio.step(1) }
        }
    }
}
