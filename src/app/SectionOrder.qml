import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// The sidebar's sections, one line each: shown or not, and up or down.
// Used by the sidebar's ⋮ menu and by Settings > Appearance > Layout.
ColumnLayout {
    id: list
    objectName: "sectionOrder"
    spacing: 0
    readonly property var layoutPrefs: appearance.layout
    Repeater {
        model: list.layoutPrefs.sections
        RowLayout {
            id: line
            required property string modelData
            required property int index
            Layout.fillWidth: true
            spacing: 2
            CheckBox {
                objectName: "sectionShown_" + line.modelData
                Layout.fillWidth: true
                text: list.layoutPrefs.sectionName(line.modelData)
                font.pixelSize: 13
                checked: { void list.layoutPrefs.hiddenSections; return list.layoutPrefs.sectionShown(line.modelData) }
                onClicked: list.layoutPrefs.setSectionShown(line.modelData, checked)
                implicitHeight: 34; topInset: 0; bottomInset: 0
            }
            Button {
                objectName: "sectionUp_" + line.modelData
                flat: true
                text: "↑"
                enabled: line.index > 0
                implicitWidth: 32; implicitHeight: 30; topInset: 0; bottomInset: 0
                ToolTip.visible: hovered
                ToolTip.delay: 600
                ToolTip.text: qsTr("Move up")
                onClicked: list.layoutPrefs.moveSection(line.modelData, -1)
            }
            Button {
                objectName: "sectionDown_" + line.modelData
                flat: true
                text: "↓"
                enabled: line.index < list.layoutPrefs.sections.length - 1
                implicitWidth: 32; implicitHeight: 30; topInset: 0; bottomInset: 0
                ToolTip.visible: hovered
                ToolTip.delay: 600
                ToolTip.text: qsTr("Move down")
                onClicked: list.layoutPrefs.moveSection(line.modelData, 1)
            }
        }
    }
}
