import QtQuick

// The analyser bars on their own, for the strip in advanced mode: green
// through cyan to the accent, with a falling peak cap on each bar.
Row {
    id: bars
    spacing: 2
    readonly property int count: backend.levels.length
    Repeater {
        model: bars.count
        Item {
            required property int index
            width: (bars.width - bars.spacing * (bars.count - 1)) / bars.count
            height: bars.height
            Item {
                anchors.bottom: parent.bottom
                width: parent.width
                height: Math.max(1, backend.levels[index] * parent.height)
                clip: true
                Rectangle {
                    anchors.bottom: parent.bottom
                    width: parent.width
                    height: bars.height
                    gradient: Gradient {
                        GradientStop { position: 0.0; color: theme.accent }
                        GradientStop { position: 0.5; color: theme.c.cyan }
                        GradientStop { position: 1.0; color: theme.c.green }
                    }
                }
            }
            Rectangle {
                width: parent.width
                height: 2
                y: parent.height - backend.peaks[index] * parent.height - 2
                visible: backend.peaks[index] > 0.02
                color: theme.c.bright_foreground
                opacity: 0.8
            }
        }
    }
}
