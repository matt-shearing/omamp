import QtQuick

// The playing track's cover in a square, or a music note in the style's
// colours when there is none. Pictures come from nowPlayingArt as file URLs,
// already at the right size, so they load at once and cost little.
Rectangle {
    id: cover
    property var skin: appearance.plain
    // A file:// URL; nowPlayingArt.coverUrl (256 px) suits most squares.
    property string source: nowPlayingArt.coverUrl
    readonly property bool showing: picture.status === Image.Ready && source !== ""
    signal clicked()

    color: skin.field
    border.width: 1
    border.color: skin.fieldBorder
    radius: skin.smallRadius
    clip: true

    Image {
        id: picture
        objectName: "coverImage"
        anchors.fill: parent
        anchors.margins: 1
        fillMode: Image.PreserveAspectCrop
        asynchronous: true
        smooth: true
        mipmap: false
        sourceSize: Qt.size(Math.max(32, Math.round(cover.width * 1.0)), Math.max(32, Math.round(cover.height * 1.0)))
        source: cover.source
    }

    Text {
        anchors.centerIn: parent
        visible: !cover.showing
        text: String.fromCodePoint(0xF075A) // a music note
        font.family: "JetBrainsMono Nerd Font"
        font.pixelSize: Math.max(10, Math.round(Math.min(cover.width, cover.height) * 0.42))
        color: cover.skin.placeholder
    }

    property bool clickable: true
    MouseArea {
        anchors.fill: parent
        enabled: cover.clickable
        cursorShape: Qt.PointingHandCursor
        onClicked: cover.clicked()
    }
}
