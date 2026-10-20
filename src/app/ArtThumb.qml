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
        : path !== "" ? "image://omamp-art/entry/" + base64url(path) + "/96" : ""
    // The path as URL-safe base64 of its UTF-8, which the provider decodes.
    function base64url(text) {
        const utf8 = unescape(encodeURIComponent(text))
        const abc = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"
        let out = ""
        for (let i = 0; i < utf8.length; i += 3) {
            const a = utf8.charCodeAt(i), b = utf8.charCodeAt(i + 1), c = utf8.charCodeAt(i + 2)
            const n = (a << 16) | ((b || 0) << 8) | (c || 0)
            out += abc[(n >> 18) & 63] + abc[(n >> 12) & 63]
            out += i + 1 < utf8.length ? abc[(n >> 6) & 63] : ""
            out += i + 2 < utf8.length ? abc[n & 63] : ""
        }
        return out
    }
    implicitWidth: size
    implicitHeight: size
    radius: Math.min(4, theme.radius)
    // Under the picture: what shows while it loads, or when there is none
    // (the provider then sends a clear square).
    color: theme.c.lighter_background
    Text {
        anchors.centerIn: parent
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
