import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// One section of the sidebar: a heading that folds it away (remembered in
// appearance.layout), and its content below.
ColumnLayout {
    id: section
    required property string sectionId
    property string title: appearance.layout.sectionName(sectionId)
    // Something small at the heading's right (a Retry button, a count).
    property alias extra: extraSlot.data
    default property alias content: body.data
    readonly property bool collapsed: { void appearance.layout.collapsedSections; return appearance.layout.isCollapsed(sectionId) }
    objectName: "sideSection_" + sectionId
    spacing: 6

    Rectangle {
        Layout.fillWidth: true
        Layout.topMargin: 4
        height: 1
        color: theme.c.lighter_background
    }
    RowLayout {
        Layout.fillWidth: true
        spacing: 6
        Text {
            objectName: "sideSectionHeading_" + section.sectionId
            Layout.fillWidth: true
            text: (section.collapsed ? "▸  " : "▾  ") + section.title
            elide: Text.ElideRight
            font.pixelSize: 11
            font.bold: true
            font.capitalization: Font.AllUppercase
            font.letterSpacing: 0.6
            color: headingHover.hovered ? theme.c.foreground : theme.c.muted
            HoverHandler { id: headingHover; cursorShape: Qt.PointingHandCursor }
            TapHandler { onTapped: appearance.layout.setCollapsed(section.sectionId, !section.collapsed) }
            ToolTip.visible: headingHover.hovered
            ToolTip.delay: 800
            ToolTip.text: section.collapsed ? qsTr("Show this section") : qsTr("Fold this section away")
        }
        Item { id: extraSlot; implicitWidth: childrenRect.width; implicitHeight: childrenRect.height; visible: !section.collapsed }
    }
    ColumnLayout {
        id: body
        Layout.fillWidth: true
        visible: !section.collapsed
        spacing: 6
    }
}
