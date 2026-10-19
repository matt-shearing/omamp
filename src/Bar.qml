import QtQuick

// A thin slider for position and volume. It reports where it was dragged
// to, and leaves the value to the player, so playback never fights the hand.
Item {
    id: bar
    property real value: 0            // 0..1
    property bool active: true
    property color fill: theme.accent
    property real dragValue: 0
    readonly property bool dragging: area.pressed
    readonly property real shown: dragging ? dragValue : value
    signal moved(real value)          // while dragging, for live volume
    signal released(real value)       // on release, for seeking

    implicitHeight: 14

    Rectangle {
        id: track
        anchors.verticalCenter: parent.verticalCenter
        width: parent.width
        height: 6
        radius: Math.min(3, theme.radius)
        color: theme.c.darker_background
        border.width: 1
        border.color: theme.c.lighter_background

        Rectangle {
            width: Math.max(0, Math.min(1, bar.shown)) * parent.width
            height: parent.height
            radius: parent.radius
            color: bar.fill
            opacity: bar.active ? 0.85 : 0.3
        }
    }

    Rectangle {
        visible: bar.active
        width: 10
        height: 14
        radius: Math.min(2, theme.radius)
        anchors.verticalCenter: parent.verticalCenter
        x: Math.max(0, Math.min(1, bar.shown)) * (bar.width - width)
        color: area.pressed || area.containsMouse ? theme.c.bright_foreground : theme.c.light_foreground
        border.width: 1
        border.color: theme.c.darker_background
    }

    MouseArea {
        id: area
        anchors.fill: parent
        anchors.margins: -4
        enabled: bar.active
        hoverEnabled: true
        preventStealing: true
        cursorShape: Qt.PointingHandCursor
        function at(x) { return Math.max(0, Math.min(1, (x - 4) / bar.width)) }
        onPressed: (mouse) => { bar.dragValue = at(mouse.x); bar.moved(bar.dragValue) }
        onPositionChanged: (mouse) => { if (pressed) { bar.dragValue = at(mouse.x); bar.moved(bar.dragValue) } }
        onReleased: (mouse) => bar.released(at(mouse.x))
        onWheel: (wheel) => {
            const step = wheel.angleDelta.y > 0 ? 0.05 : -0.05
            const v = Math.max(0, Math.min(1, bar.value + step))
            bar.moved(v); bar.released(v)
        }
    }
}
