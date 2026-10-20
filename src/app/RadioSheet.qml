import QtQuick
import QtQuick.Controls

// The channel picker over the deck (Shift+R, or the station's name on the
// live bar). Tuning in closes it; Esc closes it too.
ToolSheet {
    id: sheet
    objectName: "radioSheet"
    title: qsTr("Radio")
    subtitle: radio.active && radio.nowPlaying !== "" ? radio.current.fullName + "  ·  " + radio.nowPlaying
            : radio.active ? radio.current.fullName : ""
    preferredWidth: 900
    preferredHeight: 640
    signal findStations()
    signal addStation()

    function openSheet(group) {
        if (group) radio.group = group
        else if (radio.active && radio.current.network !== "nightride" && radio.current.network !== "rekt")
            radio.group = radio.current.favourite ? "favourites" : "mine"
        else if (radio.active) radio.group = radio.current.network
        open()
        picker.focusGrid()
    }

    onOpened: picker.focusGrid()

    RadioView {
        id: picker
        anchors.fill: parent
        visible: sheet.opened
        onTuned: sheet.close()
        onFindStations: { sheet.close(); sheet.findStations() }
        onAddStation: { sheet.close(); sheet.addStation() }
    }

    buttons: [
        Button { text: qsTr("Close"); flat: true; onClicked: sheet.close() }
    ]
}
