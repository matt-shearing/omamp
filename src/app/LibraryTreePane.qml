import QtQuick
import QtQuick.Controls

// The library tree. Arrows move, Right opens a node (or steps into it),
// Left closes it (or steps out to its parent), Enter goes to the track list.
// The list follows the highlighted node after a short pause, so holding an
// arrow key doesn't query every node on the way.
FocusScope {
    id: pane
    signal leave() // to the track list

    function choose(row) {
        list.currentIndex = row
        follow.restart()
    }

    Timer {
        id: follow
        interval: 120
        onTriggered: if (list.currentIndex !== library.currentNode) library.selectNode(list.currentIndex)
    }

    Connections {
        target: library
        function onCurrentNodeChanged() {
            if (list.currentIndex !== library.currentNode) list.currentIndex = library.currentNode
        }
    }

    Rectangle {
        anchors.fill: parent
        color: theme.c.dark_background
    }

    ListView {
        id: list
        anchors.fill: parent
        anchors.margins: 4
        clip: true
        focus: true
        model: library.tree
        currentIndex: library.currentNode
        boundsBehavior: Flickable.StopAtBounds
        keyNavigationEnabled: false
        highlightMoveDuration: 0
        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

        Keys.onPressed: (event) => {
            const row = list.currentIndex
            const tree = library.tree
            switch (event.key) {
            case Qt.Key_Up: pane.choose(Math.max(0, row - 1)); break
            case Qt.Key_Down: pane.choose(Math.min(list.count - 1, row + 1)); break
            case Qt.Key_PageUp: pane.choose(Math.max(0, row - Math.floor(list.height / 24))); break
            case Qt.Key_PageDown: pane.choose(Math.min(list.count - 1, row + Math.floor(list.height / 24))); break
            case Qt.Key_Home: pane.choose(0); break
            case Qt.Key_End: pane.choose(list.count - 1); break
            case Qt.Key_Right:
                if (tree.isExpandable(row) && !tree.isExpanded(row)) tree.expand(row)
                else if (tree.isExpanded(row)) pane.choose(row + 1)
                else pane.leave()
                break
            case Qt.Key_Left:
                if (tree.isExpanded(row)) tree.collapse(row)
                else if (tree.parentRow(row) >= 0) pane.choose(tree.parentRow(row))
                break
            case Qt.Key_Return:
            case Qt.Key_Enter:
                library.selectNode(row)
                pane.leave()
                break
            default: return
            }
            list.positionViewAtIndex(list.currentIndex, ListView.Contain)
            event.accepted = true
        }

        delegate: Rectangle {
            id: node
            required property int index
            required property string label
            required property int depth
            required property bool expandable
            required property bool expanded
            required property var trackCount
            required property bool dim
            readonly property bool chosen: index === library.currentNode
            readonly property bool cursor: index === list.currentIndex

            width: ListView.view.width
            height: 24
            radius: Math.min(3, theme.radius)
            color: chosen ? (pane.activeFocus ? theme.c.selection : Qt.alpha(theme.c.selection, 0.6))
                 : cursor && pane.activeFocus ? Qt.alpha(theme.c.selection, 0.35)
                 : hover.hovered ? Qt.alpha(theme.c.selection, 0.25) : "transparent"
            border.width: cursor && pane.activeFocus && !chosen ? 1 : 0
            border.color: theme.accent

            HoverHandler { id: hover }

            Text {
                id: arrow
                x: 4 + node.depth * 14
                width: 14
                anchors.verticalCenter: parent.verticalCenter
                text: node.expandable ? (node.expanded ? "▾" : "▸") : ""
                font.pixelSize: 12
                color: theme.c.muted
            }
            Text {
                anchors { left: arrow.right; leftMargin: 2; right: count.left; rightMargin: 6
                          verticalCenter: parent.verticalCenter }
                elide: Text.ElideRight
                text: node.label
                font.pixelSize: 13
                font.bold: node.depth === 0
                color: node.dim ? theme.c.muted : node.chosen ? theme.c.bright_foreground : theme.c.foreground
                opacity: node.dim ? 0.7 : 1
            }
            Text {
                id: count
                anchors { right: parent.right; rightMargin: 8; verticalCenter: parent.verticalCenter }
                text: node.trackCount >= 0 ? Number(node.trackCount).toLocaleString(Qt.locale(), 'f', 0) : ""
                font.pixelSize: 11
                color: theme.c.muted
            }

            MouseArea {
                anchors.fill: parent
                onPressed: (mouse) => {
                    pane.forceActiveFocus()
                    if (node.expandable && mouse.x < arrow.x + arrow.width + 2) {
                        library.tree.toggle(node.index)
                        return
                    }
                    list.currentIndex = node.index
                    library.selectNode(node.index)
                }
                onDoubleClicked: (mouse) => {
                    if (node.expandable && mouse.x >= arrow.x + arrow.width + 2) library.tree.toggle(node.index)
                }
            }
        }
    }
}
