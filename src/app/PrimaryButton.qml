import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material

// The one main action of a sheet (Save, Organise): filled with the theme's
// accent, so it is plain which button does the work.
Button {
    id: button
    font.pixelSize: 13
    // The same height and insets as Material's own buttons, so a row of them lines up.
    implicitHeight: 48
    leftPadding: 18
    rightPadding: 18
    topInset: 6
    bottomInset: 6
    contentItem: Text {
        text: button.text
        font: button.font
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        color: button.enabled ? theme.accentForeground : theme.c.muted
    }
    background: Rectangle {
        radius: Math.min(6, theme.radius)
        color: !button.enabled ? Qt.alpha(theme.c.lighter_background, 0.7)
             : button.down ? Qt.darker(theme.accent, 1.25)
             : button.hovered || button.visualFocus ? Qt.lighter(theme.accent, 1.12) : theme.accent
        border.width: button.visualFocus ? 2 : 0
        border.color: theme.c.bright_foreground
    }
}
