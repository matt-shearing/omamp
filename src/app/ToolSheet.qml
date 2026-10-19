import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// A large panel over the library for one piece of work: the tag editor,
// organise, tag from file names, the undo history. It has a title, the
// work in the middle and its buttons along the bottom, and closes with Esc
// (asking first when there are unsaved changes).
//
// While any sheet is open the window's single-key shortcuts are off (see
// Main.qml's sheetOpen), so typing a Q in a field never quits.
Popup {
    id: sheet

    property string title: ""
    property string subtitle: ""
    property int preferredWidth: 960
    property int preferredHeight: 680
    // True while closing would lose work; Esc then asks first.
    property bool dirty: false
    // Ctrl+Enter and Ctrl+S run this.
    signal accepted()
    // The sheet's body.
    default property alias body: bodyItem.data
    // The buttons on the right of the bottom row, and anything on its left.
    property alias buttons: buttonRow.data
    property alias footerLeft: footerLeftItem.data

    function askToClose() {
        if (dirty) discardBar.visible = true
        else close()
    }

    parent: Overlay.overlay
    modal: true
    focus: true
    padding: 0
    width: Math.min(preferredWidth, parent ? parent.width - 48 : preferredWidth)
    height: Math.min(preferredHeight, parent ? parent.height - 48 : preferredHeight)
    x: parent ? Math.round((parent.width - width) / 2) : 0
    y: parent ? Math.round((parent.height - height) / 2) : 0
    closePolicy: Popup.NoAutoClose
    Material.roundedScale: theme.radius
    enter: null
    exit: null

    Overlay.modal: Rectangle { color: Qt.alpha(theme.c.darker_background, 0.55) }

    background: Rectangle {
        color: theme.c.background
        border.width: 1
        border.color: theme.c.lighter_background
        radius: theme.radius
    }

    onOpened: { discardBar.visible = false; contentScope.forceActiveFocus() }

    contentItem: FocusScope {
        id: contentScope
        focus: true

        Keys.onPressed: (event) => {
            if (event.key === Qt.Key_Escape) {
                if (discardBar.visible) discardBar.visible = false
                else sheet.askToClose()
                event.accepted = true
            } else if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter || event.key === Qt.Key_S)
                       && (event.modifiers & Qt.ControlModifier)) {
                sheet.accepted()
                event.accepted = true
            }
        }

        ColumnLayout {
            anchors.fill: parent
            spacing: 0

            // The title line.
            RowLayout {
                Layout.fillWidth: true
                Layout.leftMargin: 20
                Layout.rightMargin: 12
                Layout.topMargin: 14
                Layout.bottomMargin: 10
                spacing: 12
                Text {
                    text: sheet.title
                    font.pixelSize: 18
                    font.bold: true
                    color: theme.c.bright_foreground
                }
                Text {
                    Layout.fillWidth: true
                    text: sheet.subtitle
                    elide: Text.ElideRight
                    font.pixelSize: 14
                    color: theme.c.light_foreground
                }
                ToolButton {
                    text: "✕"
                    font.pixelSize: 13
                    implicitWidth: 32; implicitHeight: 32
                    topInset: 0; bottomInset: 0
                    ToolTip.visible: hovered
                    ToolTip.delay: 600
                    ToolTip.text: qsTr("Close (Esc)")
                    onClicked: sheet.askToClose()
                }
            }
            Rectangle { Layout.fillWidth: true; height: 1; color: theme.c.lighter_background }

            Item {
                id: bodyItem
                Layout.fillWidth: true
                Layout.fillHeight: true
            }

            // Closing with unsaved changes asks first.
            Rectangle {
                id: discardBar
                objectName: "discardBar"
                Layout.fillWidth: true
                Layout.preferredHeight: 44
                visible: false
                color: Qt.alpha(theme.c.yellow, 0.14)
                RowLayout {
                    anchors { fill: parent; leftMargin: 20; rightMargin: 12 }
                    spacing: 8
                    Text {
                        Layout.fillWidth: true
                        text: qsTr("Close without saving your changes?")
                        font.pixelSize: 13
                        color: theme.c.foreground
                    }
                    Button { text: qsTr("Keep editing"); flat: true; onClicked: discardBar.visible = false }
                    Button { text: qsTr("Discard changes"); onClicked: { discardBar.visible = false; sheet.close() } }
                }
            }

            Rectangle { Layout.fillWidth: true; height: 1; color: theme.c.lighter_background }
            RowLayout {
                Layout.fillWidth: true
                Layout.leftMargin: 20
                Layout.rightMargin: 12
                Layout.topMargin: 8
                Layout.bottomMargin: 8
                spacing: 8
                Item {
                    id: footerLeftItem
                    Layout.fillWidth: true
                    Layout.preferredHeight: Math.max(36, childrenRect.height)
                }
                Row {
                    id: buttonRow
                    spacing: 8
                }
            }
        }
    }
}
