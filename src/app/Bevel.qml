import QtQuick

// The 3D edge of Winamp-era chrome: a light line along the top and left,
// a dark one along the bottom and right. sunken swaps them.
Item {
    id: bevel
    property var skin: appearance.plain
    property bool sunken: false
    readonly property color lit: sunken ? skin.bevelDark : skin.bevelLight
    readonly property color shade: sunken ? skin.bevelLight : skin.bevelDark
    Rectangle { anchors { left: parent.left; right: parent.right; top: parent.top } height: 1; color: bevel.lit }
    Rectangle { anchors { left: parent.left; top: parent.top; bottom: parent.bottom } width: 1; color: bevel.lit }
    Rectangle { anchors { left: parent.left; right: parent.right; bottom: parent.bottom } height: 1; color: bevel.shade }
    Rectangle { anchors { right: parent.right; top: parent.top; bottom: parent.bottom } width: 1; color: bevel.shade }
}
