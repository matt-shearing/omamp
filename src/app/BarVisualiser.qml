import QtQuick

// The large now playing bar's visualiser (BigBar.qml). It wears the bar's
// own style when one is chosen in Settings > Appearance, else the player's;
// a click, or Shift+V while the library shows, moves that style on. While
// no sound reaches Omamp it shows a calm idle animation, or hides when
// Settings says so.
Analyser {
    objectName: "barVisualiser"
    style: appearance.barStyle
    cycle: function() { appearance.nextBarVisualiser() }
    visible: !(idle && appearance.idleVisualiser === "hide")
}
