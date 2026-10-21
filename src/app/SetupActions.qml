import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// The first-run setup (SetupWizard.qml), its palette action and Help entry
// ("setup"), and the one-time banner that offers it to people who used Omamp
// before it had one (firstRun.bannerPending). Kept out of Main.qml as
// RadioActions keeps the radio's.
Item {
    id: ui
    property var window: null
    property var prefs: null
    property var modes: null
    // Room to leave below the banner (the deck's library hint).
    property int bottomOffset: 0
    readonly property bool sheetOpen: wizard.opened
    readonly property alias wizard: wizard

    function registerActions(p) {
        p.addAction("setup", qsTr("Run setup again…"), ["first run", "welcome", "wizard", "layout", "look", "theme", "start"], "")
    }
    // True when it was the setup's action.
    function runAction(id) {
        if (id !== "setup") return false
        openSetup()
        return true
    }
    function openSetup() {
        if (wizard.opened) return
        // The compact players and the big view have no room for it.
        if (modes) {
            if (modes.compact !== "") modes.setCompact("")
            if (modes.bigView) modes.setBigView(false)
        }
        wizard.openWizard()
    }
    // At start: a new install opens the setup once the window is up.
    function start(startMode) {
        if (firstRun.wizardPending && !startMode) Qt.callLater(openSetup)
    }

    SetupWizard {
        id: wizard
        window: ui.window
        prefs: ui.prefs
        onClosed: if (ui.window) ui.window.backToList()
    }

    // "New: choose your look and layout", once, for older installs.
    Rectangle {
        id: banner
        objectName: "setupBanner"
        parent: ui.parent
        visible: firstRun.bannerPending && !wizard.opened && ui.window !== null && !ui.window.sheetOpen
                 && ui.modes !== null && ui.modes.compact === "" && !ui.modes.bigView
                 && !library.dj.partyDisplay
        anchors.horizontalCenter: parent ? parent.horizontalCenter : undefined
        anchors.bottom: parent ? parent.bottom : undefined
        anchors.bottomMargin: 40 + ui.bottomOffset
        width: Math.min(520, (parent ? parent.width : 520) - 28)
        height: bannerRow.implicitHeight + 12
        z: 50
        radius: Math.min(6, theme.radius)
        color: theme.c.dark_background
        border.width: 1
        border.color: theme.accent
        RowLayout {
            id: bannerRow
            anchors { fill: parent; leftMargin: 12; rightMargin: 6; topMargin: 6; bottomMargin: 6 }
            spacing: 8
            Text {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: qsTr("New: a short setup to choose your look and layout.")
                font.pixelSize: 13
                color: theme.c.bright_foreground
            }
            Button {
                objectName: "setupBannerRun"
                text: qsTr("Run setup")
                flat: true
                font.pixelSize: 13
                implicitHeight: 32; topInset: 0; bottomInset: 0
                onClicked: ui.openSetup()
            }
            Button {
                objectName: "setupBannerClose"
                text: "✕"
                flat: true
                font.pixelSize: 13
                implicitWidth: 32; implicitHeight: 32; topInset: 0; bottomInset: 0
                ToolTip.visible: hovered
                ToolTip.delay: 600
                ToolTip.text: qsTr("Don't show this again")
                onClicked: firstRun.dismissBanner()
            }
        }
    }
}
