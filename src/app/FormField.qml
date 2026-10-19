import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// One labelled text field in a form, plain and boxed like the search box.
//
// For the tag editor it knows three states: a value; "mixed", when the
// chosen tracks differ (shown as ‹keep›: each track keeps its own); and
// edited, marked with an accent bar. The ✕ empties the field in every track.
Item {
    id: field
    property string label: ""
    property string text: ""
    property string placeholder: ""
    property bool mixed: false
    property bool edited: false
    property bool multiline: false
    property bool monospace: false
    property bool clearable: true
    property bool readOnly: false
    // Digits only, for years, track numbers and BPM.
    property bool numeric: false
    property string tip: ""
    property int labelWidth: 112
    property alias input: input
    property alias cursorPosition: input.cursorPosition
    // Called with each key press in the field, before the field sees it.
    property var keyHandler: null
    readonly property bool inputFocused: input.activeFocus || area.activeFocus
    // The user typed: the new text.
    signal edit(string text)
    signal cleared()
    signal reverted()

    implicitHeight: multiline ? Math.max(96, box.implicitHeight) : 32
    implicitWidth: 300

    function focusInput() { if (multiline) area.forceActiveFocus(); else input.forceActiveFocus() }

    // Shows the model's value unless the user is typing in this field.
    onTextChanged: {
        if (multiline) { if (area.text !== text) area.text = text }
        else if (input.text !== text) input.text = text
    }

    // The ordinary font, for fields that aren't monospaced.
    Text { id: fontProbe; visible: false }

    RowLayout {
        anchors.fill: parent
        spacing: 10

        Text {
            Layout.preferredWidth: field.labelWidth
            Layout.alignment: field.multiline ? Qt.AlignTop : Qt.AlignVCenter
            Layout.topMargin: field.multiline ? 8 : 0
            visible: field.label !== ""
            text: field.label
            horizontalAlignment: Text.AlignRight
            elide: Text.ElideRight
            font.pixelSize: 13
            color: field.edited ? theme.c.bright_foreground : theme.c.light_foreground
        }

        Rectangle {
            id: box
            Layout.fillWidth: true
            Layout.fillHeight: true
            radius: Math.min(4, theme.radius)
            color: field.readOnly ? theme.c.background : theme.c.darker_background
            border.width: 1
            border.color: field.inputFocused ? theme.accent : theme.c.lighter_background
            implicitHeight: field.multiline ? 96 : 32

            // Edited fields carry an accent bar on the left.
            Rectangle {
                visible: field.edited
                width: 3
                anchors { left: parent.left; top: parent.top; bottom: parent.bottom; margins: 3 }
                radius: 1
                color: theme.accent
            }

            TextInput {
                id: input
                visible: !field.multiline
                anchors { left: parent.left; leftMargin: 10; right: clearButton.visible ? clearButton.left : parent.right
                          rightMargin: 8; verticalCenter: parent.verticalCenter }
                font.pixelSize: 13
                font.family: field.monospace ? "JetBrainsMono Nerd Font" : fontProbe.font.family
                color: theme.c.foreground
                selectionColor: theme.c.selection
                selectedTextColor: theme.c.bright_foreground
                selectByMouse: true
                clip: true
                readOnly: field.readOnly
                validator: field.numeric ? digits : null
                onTextEdited: field.edit(text)
                Keys.onPressed: (event) => { if (field.keyHandler) field.keyHandler(event); else event.accepted = false }
                Text {
                    anchors.fill: parent
                    visible: input.text === "" && !input.activeFocus || input.text === "" && field.mixed
                    verticalAlignment: Text.AlignVCenter
                    elide: Text.ElideRight
                    text: field.mixed ? (input.width > 200 ? qsTr("‹keep›  each track keeps its own") : qsTr("‹keep›"))
                                      : field.placeholder
                    font.pixelSize: 13
                    font.italic: field.mixed
                    color: theme.c.muted
                }
            }
            RegularExpressionValidator { id: digits; regularExpression: /[0-9]{0,6}/ }

            ScrollView {
                visible: field.multiline
                anchors { fill: parent; margins: 2; rightMargin: clearButton.visible ? 30 : 2 }
                TextArea {
                    id: area
                    wrapMode: TextEdit.Wrap
                    font.pixelSize: 13
                    color: theme.c.foreground
                    selectionColor: theme.c.selection
                    selectedTextColor: theme.c.bright_foreground
                    selectByMouse: true
                    readOnly: field.readOnly
                    background: null
                    placeholderText: field.mixed ? qsTr("‹keep›  each track keeps its own") : field.placeholder
                    placeholderTextColor: theme.c.muted
                    onTextChanged: if (activeFocus) field.edit(text)
                }
            }

            ToolButton {
                id: clearButton
                anchors { right: parent.right; rightMargin: 2; top: field.multiline ? parent.top : undefined
                          verticalCenter: field.multiline ? undefined : parent.verticalCenter }
                visible: field.clearable && !field.readOnly && (field.text !== "" || field.mixed)
                text: "✕"
                font.pixelSize: 11
                implicitWidth: 26; implicitHeight: 26
                topInset: 0; bottomInset: 0
                focusPolicy: Qt.NoFocus
                ToolTip.visible: hovered
                ToolTip.delay: 500
                ToolTip.text: qsTr("Empty this field")
                onClicked: field.cleared()
            }

            HoverHandler { id: hover }
            ToolTip.visible: hover.hovered && field.tip !== ""
            ToolTip.delay: 700
            ToolTip.text: field.tip
        }
    }
}
