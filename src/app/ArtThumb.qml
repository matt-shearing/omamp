import QtQuick
import QtQuick.Window

// A small square cover for a list row: a library track by its id, or any
// queue or playlist entry by its path (a loose file, a Spotify track, a
// radio station's tile). The picture provider crops and rounds it on a
// worker thread, so the row needs no clipping effect; a music note shows
// until it arrives, or when there is none.
Rectangle {
    id: thumb
    property var trackId: 0
    property string path: ""
    // A ready address (image://omamp-art/…), instead of trackId and path.
    property string art: ""
    property int size: 20
    readonly property int px: Math.round(size * Screen.devicePixelRatio)
    readonly property string base: art !== "" ? art
        : trackId > 0 ? "image://omamp-art/track/" + trackId + "/96"
        : path !== "" ? "image://omamp-art/entry/" + encodeURIComponent(Qt.btoa(path)) + "/96" : ""
    implicitWidth: size
    implicitHeight: size
    radius: Math.min(4, theme.radius)
    color: picture.status === Image.Ready ? "transparent" : theme.c.lighter_background
    Text {
        anchors.centerIn: parent
        visible: picture.status !== Image.Ready
        text: String.fromCodePoint(thumb.path.startsWith("radio://") ? 0xF0439 : 0xF075A)
        font.family: "JetBrainsMono Nerd Font"
        font.pixelSize: Math.max(8, Math.round(thumb.size * 0.5))
        color: theme.c.muted
    }
    Image {
        id: picture
        objectName: "artThumb"
        anchors.fill: parent
        asynchronous: true
        cache: true
        sourceSize: Qt.size(thumb.px, thumb.px)
        source: thumb.base === "" ? ""
            : thumb.base + (thumb.base.indexOf("?") >= 0 ? "&" : "?") + "r=" + Math.round(Math.min(4, theme.radius) * Screen.devicePixelRatio)
    }
}
