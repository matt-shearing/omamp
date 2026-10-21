import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import Omamp

// The first-run setup: five short steps over the real window, each applied
// as it is chosen so the window behind shows the result.
//
//   1 Start       the player (the deck) or the library
//   2 Look        the deck style and the visualiser
//   3 Layout      a preset for the library, and its main switches
//   4 Your music  a music folder, Spotify, radio and Navidrome
//   5 Done        the keys worth knowing
//
// The panel sits at the bottom of the window (at the top while the library's
// bar is at the bottom, so the bar shows) and dims nothing. Enter is Next,
// Esc asks whether to skip. Skipping puts back everything as it was when
// the setup opened; Finish keeps it. Either way it doesn't open by itself
// again (firstRun.markCompleted()).
Popup {
    id: wiz
    objectName: "setupWizard"

    // Main.qml and its window settings.
    property var window: null
    property var prefs: null
    readonly property var layoutPrefs: appearance.layout

    readonly property var stepIds: ["start", "look", "layout", "music", "done"]
    property int step: 0
    readonly property string stepId: stepIds[step]
    // Where Omamp starts: "player" or "library".
    property string startView: "player"
    // Switched on at Finish, which then opens the Spotify page to sign in.
    property bool wantSpotify: false
    property bool confirmingSkip: false
    // Everything the setup can change, as it was when it opened.
    property var saved: ({})
    signal finished(bool skipped)

    parent: Overlay.overlay
    modal: true
    focus: true
    closePolicy: Popup.NoAutoClose
    padding: 0
    // Nothing dimmed: the window behind is the preview.
    Overlay.modal: Rectangle { color: "transparent" }
    width: Math.max(0, Math.min(660, parent ? parent.width - 24 : 660))
    // As tall as the step needs, never taller than the room the window has
    // (the step scrolls inside instead), so Next and Finish always show.
    readonly property real pageHeight: pages.children[step] ? pages.children[step].implicitHeight : 200
    readonly property real wantedHeight: titleRow.implicitHeight + pageHeight + footerRow.implicitHeight
                                         + (confirmingSkip ? skipBar.implicitHeight : 0) + 28
    readonly property real dockTopY: menuHeight + 8
    readonly property real room: !parent ? 392 : Math.max(0, parent.height - (dockTop ? dockTopY : 12) - 12)
    height: Math.min(wantedHeight, room)
    x: parent ? Math.round((parent.width - width) / 2) : 0
    readonly property bool dockTop: stepId === "layout" && layoutPrefs.barPosition === "bottom"
    // Below the menu bar when at the top: the overlay covers the whole window.
    readonly property real menuHeight: window && window.menuBar && window.menuBar.visible ? window.menuBar.height : 0
    y: !parent ? 0 : dockTop ? dockTopY : parent.height - height - 12
    // The room the setup wants: the panel's tallest step with the view
    // behind it showing, and wide enough for the library. A floating window
    // grows to it when it opens; a tiled one keeps the size it is given.
    readonly property int comfortableWidth: 900
    readonly property int comfortableHeight: 760
    Material.roundedScale: theme.radius
    enter: null
    exit: null

    background: Rectangle {
        color: theme.c.background
        border.width: 1
        border.color: theme.accent
        radius: theme.radius
    }

    // ---- Opening, moving and closing ------------------------------------------

    // Switching views hands the keys to the library's list; while the setup
    // is open they come back here, so Enter and Esc keep working.
    function holdsFocus(item) {
        for (let i = item; i; i = i.parent)
            if (i === contentScope) return true
        return false
    }
    Connections {
        target: wiz.window
        enabled: wiz.opened
        function onActiveFocusItemChanged() {
            Qt.callLater(() => {
                if (wiz.opened && !wiz.holdsFocus(wiz.window.activeFocusItem)) contentScope.forceActiveFocus()
            })
        }
    }

    function snapshot() {
        return {
            advanced: prefs.advanced, deckStyle: appearance.deckStyle, visualiser: appearance.visualiser,
            artColumn: layoutPrefs.artColumn, sidebar: prefs.infoVisible, barSize: layoutPrefs.barSize,
            barPosition: layoutPrefs.barPosition, browser: prefs.browserVisible,
            barVisualiser: appearance.barVisualiser
        }
    }
    function restore(s) {
        appearance.deckStyle = s.deckStyle
        appearance.visualiser = s.visualiser
        layoutPrefs.artColumn = s.artColumn
        prefs.infoVisible = s.sidebar
        layoutPrefs.barSize = s.barSize
        layoutPrefs.barPosition = s.barPosition
        prefs.browserVisible = s.browser
        appearance.barVisualiser = s.barVisualiser
    }
    function openWizard() {
        saved = snapshot()
        // The present view's size is its own; then room for the setup.
        if (window) {
            window.rememberLayout()
            window.growTo(comfortableWidth, comfortableHeight)
        }
        startView = prefs.advanced ? "library" : "player"
        wantSpotify = spotify.enabled
        confirmingSkip = false
        step = 0
        open()
        showStep()
    }
    function showView(view) {
        // Straight to the window, not through runAction(), so the action
        // that opened the setup stays the last one run.
        // At the window's present size: the previews of both views show in
        // it, and nothing is remembered until the setup ends (settle()).
        if (window) window.setAdvanced(view === "library", true)
    }
    // Finish and Skip: the start view at its own size, so the window and
    // its content agree again (a floating window is resized by Hyprland and
    // the content then takes the size it got).
    function settle(view) {
        if (!window) return
        window.setAdvanced(view === "library", true)
        window.settleSize()
    }
    // The window behind shows what the step is about: the deck for Look,
    // the library for Layout, and the chosen start view otherwise.
    function showStep() {
        confirmingSkip = false
        showView(stepId === "look" ? "player" : stepId === "layout" ? "library" : startView)
        Qt.callLater(() => contentScope.forceActiveFocus())
    }
    function goTo(n) {
        step = Math.max(0, Math.min(stepIds.length - 1, n))
        showStep()
    }
    function next() {
        if (step === stepIds.length - 1) finish()
        else goTo(step + 1)
    }
    function back() { if (step > 0) goTo(step - 1) }
    function finish() {
        firstRun.markCompleted()
        prefs.libraryHintDone = true
        settle(startView)
        close()
        finished(false)
        if (wantSpotify && !spotify.enabled) {
            spotify.enabled = true
            if (window) window.runAction("settings-spotify")
        }
    }
    function skip() {
        restore(saved)
        firstRun.markCompleted()
        settle(saved.advanced ? "library" : "player")
        close()
        finished(true)
    }

    // ---- Layout presets ---------------------------------------------------------

    readonly property var presets: [
        { id: "classic", name: qsTr("Classic"), text: qsTr("Tree, column browser and list, with the slim strip on top."),
          artColumn: false, sidebar: false, browser: true, barSize: "compact", barPosition: "top" },
        { id: "spotify", name: qsTr("Spotify-like"), text: qsTr("Covers in the list, the sidebar, and a large bar at the bottom."),
          artColumn: true, sidebar: true, browser: false, barSize: "large", barPosition: "bottom" },
        { id: "minimal", name: qsTr("Minimal"), text: qsTr("Just the tree and the list."),
          artColumn: false, sidebar: false, browser: false, barSize: "compact", barPosition: "top" }
    ]
    function applyPreset(p) {
        layoutPrefs.artColumn = p.artColumn
        prefs.infoVisible = p.sidebar
        prefs.browserVisible = p.browser
        layoutPrefs.barSize = p.barSize
        layoutPrefs.barPosition = p.barPosition
    }
    function presetMatches(p) {
        return layoutPrefs.artColumn === p.artColumn && prefs.infoVisible === p.sidebar
            && prefs.browserVisible === p.browser && layoutPrefs.barSize === p.barSize
            && layoutPrefs.barPosition === p.barPosition
    }

    // ---- The large bar's visualiser ---------------------------------------------

    // The two looks offered, in this order, of the placements the build has
    // (appearance.barPlacements): bold or subtle. Off is a link beside them.
    readonly property var barLooks: {
        const notes = { full: qsTr("Bold: the visualiser fills the bar behind everything."),
                        behind: qsTr("Subtle: only behind the seek bar. The default.") }
        return ["full", "behind"].filter(p => appearance.barPlacements.indexOf(p) >= 0)
                                 .map(p => ({ id: p, name: appearance.barPlacementName(p), text: notes[p] }))
    }

    // ---- The visualiser previews' made-up music --------------------------------

    property real phase: 0
    property var fakeLevels: []
    property var fakePeaks: []
    property var fakeWave: []
    Timer {
        interval: 50
        repeat: true
        running: wiz.opened && (wiz.stepId === "look" || (wiz.stepId === "layout" && wiz.layoutPrefs.barSize === "large"))
        triggeredOnStart: true
        onTriggered: {
            wiz.phase += 0.18
            const levels = [], peaks = [], wave = []
            for (let i = 0; i < 20; ++i) {
                const v = 0.45 + 0.35 * Math.sin(wiz.phase * (1 + i * 0.07) + i * 0.6) * Math.cos(wiz.phase * 0.4 + i * 0.2)
                levels.push(Math.max(0.04, Math.min(1, v * (1 - i * 0.025))))
                peaks.push(Math.min(1, levels[i] + 0.08))
            }
            for (let j = 0; j < 96; ++j)
                wave.push(0.6 * Math.sin(j * 0.33 + wiz.phase * 3) * Math.cos(j * 0.05 + wiz.phase))
            wiz.fakeLevels = levels
            wiz.fakePeaks = peaks
            wiz.fakeWave = wave
        }
    }

    // ---- Pieces -------------------------------------------------------------------

    component Note: Text {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        font.pixelSize: 13
        color: theme.c.foreground
    }
    component Sub: Text {
        Layout.fillWidth: true
        font.pixelSize: 13
        font.bold: true
        color: theme.c.bright_foreground
    }
    // A choice drawn as a card; checked shows in the accent.
    component Card: AbstractButton {
        id: card
        property bool chosen: false
        hoverEnabled: true
        focusPolicy: Qt.StrongFocus
        padding: 8
        background: Rectangle {
            radius: Math.min(6, theme.radius)
            color: card.chosen ? Qt.alpha(theme.accent, 0.14) : card.hovered ? theme.c.selection : "transparent"
            border.width: card.chosen || card.visualFocus ? 2 : 1
            border.color: card.chosen || card.visualFocus ? theme.accent : theme.c.lighter_background
        }
        Keys.onSpacePressed: clicked()
    }
    // A small drawing of the window: the deck, or the library with its parts.
    component Sketch: Rectangle {
        id: sk
        property bool deck: false
        property bool browser: false
        property bool art: false
        property bool sidebar: false
        property string barSize: "compact"
        property string barPosition: "top"
        readonly property color ink: theme.c.light_foreground
        readonly property color faint: Qt.alpha(theme.c.light_foreground, 0.35)
        implicitWidth: 132
        implicitHeight: 76
        radius: Math.min(3, theme.radius)
        color: theme.c.darker_background
        border.width: 1
        border.color: theme.c.lighter_background
        // The deck: the readout and analyser, the buttons, the playlist.
        Item {
            visible: sk.deck
            anchors.fill: parent
            anchors.margins: 6
            Rectangle { x: 0; y: 0; width: parent.width; height: 22; radius: 2; color: Qt.alpha(theme.accent, 0.25) }
            Row {
                x: 6; y: 6; spacing: 2
                Repeater { model: [8, 12, 6, 10, 14, 7, 11]; Rectangle { required property int modelData; width: 3; height: modelData; y: 14 - modelData; color: theme.accent } }
            }
            Row {
                y: 27; spacing: 3
                Repeater { model: 5; Rectangle { width: 10; height: 6; radius: 1; color: sk.ink } }
            }
            Column {
                y: 38; spacing: 4; width: parent.width
                Repeater { model: 4; Rectangle { width: parent.width * (0.9 - index * 0.12); height: 3; color: sk.faint } }
            }
        }
        // The library.
        Item {
            visible: !sk.deck
            anchors.fill: parent
            anchors.margins: 4
            readonly property int barH: sk.barSize === "large" ? 12 : 6
            readonly property int topY: sk.barPosition === "top" ? barH + 3 : 0
            readonly property int bodyH: height - barH - 3
            Rectangle {
                width: parent.width; height: parent.barH; radius: 1
                y: sk.barPosition === "top" ? 0 : parent.height - height
                color: Qt.alpha(theme.accent, sk.barSize === "large" ? 0.45 : 0.3)
            }
            // The tree.
            Rectangle { x: 0; y: parent.topY; width: 22; height: parent.bodyH; color: sk.faint; radius: 1 }
            Item {
                id: listArea
                x: 25; y: parent.topY
                width: parent.width - 25 - (sk.sidebar ? 29 : 0)
                height: parent.bodyH
                Row {
                    visible: sk.browser
                    spacing: 2
                    Repeater { model: 3; Rectangle { width: (listArea.width - 4) / 3; height: 16; color: sk.faint; radius: 1 } }
                }
                Column {
                    y: sk.browser ? 19 : 0
                    spacing: 3
                    Repeater {
                        model: sk.browser ? 5 : 8
                        Row {
                            spacing: 2
                            Rectangle { visible: sk.art; width: 4; height: 4; y: -0.5; color: theme.accent }
                            Rectangle { width: listArea.width - (sk.art ? 6 : 0); height: 3; color: sk.ink; opacity: 0.6 }
                        }
                    }
                }
            }
            Rectangle {
                visible: sk.sidebar
                x: parent.width - 26; y: parent.topY; width: 26; height: parent.bodyH; radius: 1
                color: sk.faint
                Rectangle { x: 3; y: 3; width: 20; height: 20; color: Qt.alpha(theme.accent, 0.5) }
            }
        }
    }

    // ---- The panel ------------------------------------------------------------

    // Enter and Esc as shortcuts, so they work whichever control has the
    // keys (a card or a check box would take Enter for itself).
    Shortcut {
        sequences: ["Return", "Enter"]
        enabled: wiz.opened
        onActivated: wiz.confirmingSkip ? wiz.skip() : wiz.next()
    }
    Shortcut {
        sequence: "Escape"
        enabled: wiz.opened
        onActivated: wiz.confirmingSkip = !wiz.confirmingSkip
    }

    contentItem: FocusScope {
        id: contentScope
        focus: true

        ColumnLayout {
            anchors.fill: parent
            spacing: 0

            // Title and where we are.
            RowLayout {
                id: titleRow
                Layout.fillWidth: true
                Layout.leftMargin: 18; Layout.rightMargin: 18; Layout.topMargin: 14
                spacing: 10
                Text {
                    objectName: "setupTitle"
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                    font.pixelSize: 17
                    font.bold: true
                    color: theme.c.bright_foreground
                    text: [qsTr("Welcome to Omamp"), qsTr("Choose a look"), qsTr("Lay out the library"),
                           qsTr("Your music"), qsTr("You're all set")][wiz.step]
                }
                Text {
                    text: qsTr("Step %1 of %2").arg(wiz.step + 1).arg(wiz.stepIds.length)
                    font.pixelSize: 12
                    color: theme.c.light_foreground
                }
            }

            ScrollView {
                id: scroller
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.topMargin: 6
                clip: true
                contentWidth: availableWidth
                ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

                StackLayout {
                    id: pages
                    width: scroller.availableWidth
                    currentIndex: wiz.step

                    // ---- 1 Start ------------------------------------------------
                    ColumnLayout {
                        objectName: "setupStepStart"
                        spacing: 10
                        Item { Layout.preferredHeight: 0 }
                        Note {
                            Layout.leftMargin: 18; Layout.rightMargin: 18
                            text: qsTr("A few quick choices about how Omamp looks. Each one shows in the window as you pick it. Where should Omamp start?")
                        }
                        Flow {
                            Layout.fillWidth: true
                            Layout.leftMargin: 18; Layout.rightMargin: 18
                            spacing: 10
                            Repeater {
                                model: [
                                    { id: "player", name: qsTr("Player"), text: qsTr("The Winamp-style deck: big buttons, the analyser and a playlist.") },
                                    { id: "library", name: qsTr("Library"), text: qsTr("Your whole collection: browse, search and tidy tags.") }
                                ]
                                Card {
                                    required property var modelData
                                    objectName: "setupStart_" + modelData.id
                                    width: Math.min(286, (scroller.availableWidth - 46) / 2)
                                    chosen: wiz.startView === modelData.id
                                    onClicked: { wiz.startView = modelData.id; wiz.showView(modelData.id) }
                                    contentItem: RowLayout {
                                        spacing: 10
                                        Sketch { deck: modelData.id === "player"; sidebar: true; art: true; Layout.alignment: Qt.AlignTop; implicitWidth: 96; implicitHeight: 64 }
                                        ColumnLayout {
                                            spacing: 2
                                            Sub { text: modelData.name }
                                            Text { Layout.fillWidth: true; wrapMode: Text.Wrap; text: modelData.text; font.pixelSize: 12; color: theme.c.light_foreground }
                                        }
                                    }
                                }
                            }
                        }
                        Note {
                            Layout.leftMargin: 18; Layout.rightMargin: 18
                            text: qsTr("Tab switches between the two at any time.")
                            color: theme.c.light_foreground
                        }
                    }

                    // ---- 2 Look -------------------------------------------------
                    ColumnLayout {
                        objectName: "setupStepLook"
                        spacing: 6
                        Sub { Layout.leftMargin: 18; text: qsTr("Player style") }
                        ColumnLayout {
                            Layout.fillWidth: true
                            Layout.leftMargin: 10; Layout.rightMargin: 18
                            spacing: 0
                            Repeater {
                                model: appearance.deckStyles
                                RowLayout {
                                    required property string modelData
                                    spacing: 2
                                    RadioButton {
                                        objectName: "setupDeckStyle_" + modelData
                                        implicitHeight: 34
                                        topPadding: 0; bottomPadding: 0
                                        font.pixelSize: 13
                                        text: appearance.deckStyleName(modelData)
                                        checked: appearance.deckStyle === modelData
                                        onClicked: appearance.deckStyle = modelData
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                        elide: Text.ElideRight
                                        font.pixelSize: 12
                                        color: theme.c.light_foreground
                                        text: ({ omarchy: qsTr("follows your Omarchy theme"), classic: qsTr("Winamp's grey and green"),
                                                 silver: qsTr("brushed silver from the 2000s") })[modelData] || ""
                                    }
                                }
                            }
                        }
                        Sub { Layout.leftMargin: 18; Layout.topMargin: 4; text: qsTr("Visualiser") }
                        Flow {
                            Layout.fillWidth: true
                            Layout.leftMargin: 18; Layout.rightMargin: 18
                            spacing: 8
                            Repeater {
                                // Every style the build has, so new ones show up here too.
                                model: appearance.visualisers
                                Card {
                                    required property string modelData
                                    objectName: "setupVisualiser_" + modelData
                                    width: 112
                                    padding: 5
                                    chosen: appearance.visualiser === modelData
                                    onClicked: appearance.visualiser = modelData
                                    contentItem: ColumnLayout {
                                        spacing: 3
                                        Rectangle {
                                            Layout.fillWidth: true
                                            implicitHeight: 44
                                            radius: Math.min(3, theme.radius)
                                            color: appearance.deck.lcd
                                            Visualiser {
                                                objectName: "setupVisualiserPreview"
                                                anchors.fill: parent
                                                anchors.margins: 3
                                                style: modelData
                                                levels: wiz.fakeLevels
                                                peaks: wiz.fakePeaks
                                                waveform: wiz.fakeWave
                                                low: appearance.deck.visLow
                                                mid: appearance.deck.visMid
                                                high: appearance.deck.visHigh
                                                peak: appearance.deck.visPeak
                                                spacing: 1
                                            }
                                        }
                                        Text {
                                            Layout.fillWidth: true
                                            horizontalAlignment: Text.AlignHCenter
                                            elide: Text.ElideRight
                                            text: appearance.visualiserName(modelData)
                                            font.pixelSize: 12
                                            color: theme.c.foreground
                                        }
                                    }
                                }
                            }
                        }
                    }

                    // ---- 3 Layout -----------------------------------------------
                    ColumnLayout {
                        objectName: "setupStepLayout"
                        spacing: 8
                        Flow {
                            Layout.fillWidth: true
                            Layout.leftMargin: 18; Layout.rightMargin: 18
                            spacing: 8
                            Repeater {
                                model: wiz.presets
                                Card {
                                    required property var modelData
                                    objectName: "setupPreset_" + modelData.id
                                    width: Math.max(150, Math.min(196, (scroller.availableWidth - 52) / 3))
                                    chosen: { void wiz.prefs.infoVisible; void wiz.prefs.browserVisible; void wiz.layoutPrefs.artColumn
                                              void wiz.layoutPrefs.barSize; void wiz.layoutPrefs.barPosition
                                              return wiz.presetMatches(modelData) }
                                    onClicked: wiz.applyPreset(modelData)
                                    contentItem: ColumnLayout {
                                        spacing: 4
                                        Sketch {
                                            Layout.fillWidth: true
                                            implicitHeight: 64
                                            browser: modelData.browser; art: modelData.artColumn; sidebar: modelData.sidebar
                                            barSize: modelData.barSize; barPosition: modelData.barPosition
                                        }
                                        Sub { text: modelData.name }
                                        Text { Layout.fillWidth: true; wrapMode: Text.Wrap; text: modelData.text; font.pixelSize: 12; color: theme.c.light_foreground }
                                    }
                                }
                            }
                        }
                        Flow {
                            Layout.fillWidth: true
                            Layout.leftMargin: 12; Layout.rightMargin: 18
                            spacing: 0
                            CheckBox {
                                objectName: "setupArtColumn"
                                text: qsTr("Covers in the list")
                                font.pixelSize: 13
                                checked: wiz.layoutPrefs.artColumn
                                onClicked: wiz.layoutPrefs.artColumn = checked
                            }
                            CheckBox {
                                objectName: "setupSidebar"
                                text: qsTr("Sidebar")
                                font.pixelSize: 13
                                checked: wiz.prefs ? wiz.prefs.infoVisible : false
                                onClicked: wiz.prefs.infoVisible = checked
                            }
                            CheckBox {
                                objectName: "setupLargeBar"
                                text: qsTr("Large now playing bar")
                                font.pixelSize: 13
                                checked: wiz.layoutPrefs.barSize === "large"
                                onClicked: wiz.layoutPrefs.barSize = checked ? "large" : "compact"
                            }
                            CheckBox {
                                objectName: "setupBarBottom"
                                text: qsTr("Bar at the bottom")
                                font.pixelSize: 13
                                checked: wiz.layoutPrefs.barPosition === "bottom"
                                onClicked: wiz.layoutPrefs.barPosition = checked ? "bottom" : "top"
                            }
                        }
                        // The large bar's visualiser: bold or subtle, shown
                        // live in the bar behind the panel.
                        RowLayout {
                            visible: wiz.layoutPrefs.barSize === "large" && wiz.barLooks.length > 0
                            Layout.fillWidth: true
                            Layout.leftMargin: 18; Layout.rightMargin: 18; Layout.topMargin: 2
                            Sub { Layout.fillWidth: true; text: qsTr("Visualiser in the large bar") }
                            Button {
                                objectName: "setupBarVisualiser_off"
                                visible: appearance.barPlacements.indexOf("off") >= 0
                                text: appearance.barVisualiser === "off" ? qsTr("Off ✓") : appearance.barPlacementName("off")
                                flat: true
                                font.pixelSize: 12
                                implicitHeight: 28; topInset: 0; bottomInset: 0
                                onClicked: appearance.barVisualiser = "off"
                            }
                        }
                        Flow {
                            visible: wiz.layoutPrefs.barSize === "large" && wiz.barLooks.length > 0
                            Layout.fillWidth: true
                            Layout.leftMargin: 18; Layout.rightMargin: 18
                            spacing: 8
                            Repeater {
                                model: wiz.barLooks
                                Card {
                                    id: lookCard
                                    required property var modelData
                                    objectName: "setupBarVisualiser_" + modelData.id
                                    width: Math.max(180, Math.min(300, (scroller.availableWidth - 46) / 2))
                                    chosen: appearance.barVisualiser === modelData.id
                                    onClicked: appearance.barVisualiser = modelData.id
                                    contentItem: ColumnLayout {
                                        spacing: 4
                                        // A small bar: the cover, the title and the seek
                                        // bar, with the visualiser where this look puts it.
                                        Rectangle {
                                            Layout.fillWidth: true
                                            implicitHeight: 40
                                            radius: Math.min(3, theme.radius)
                                            color: theme.c.darker_background
                                            border.width: 1
                                            border.color: theme.c.lighter_background
                                            clip: true
                                            Visualiser {
                                                readonly property bool full: lookCard.modelData.id === "full"
                                                x: full ? 0 : 40
                                                y: full ? 0 : 22
                                                width: full ? parent.width : parent.width - 52
                                                height: full ? parent.height : 14
                                                opacity: full ? 0.55 : 0.8
                                                style: appearance.barStyle
                                                levels: wiz.fakeLevels
                                                peaks: wiz.fakePeaks
                                                waveform: wiz.fakeWave
                                                low: appearance.deck.visLow
                                                mid: appearance.deck.visMid
                                                high: appearance.deck.visHigh
                                                peak: appearance.deck.visPeak
                                                spacing: 1
                                            }
                                            Rectangle { x: 6; y: 6; width: 28; height: 28; radius: 2; color: Qt.alpha(theme.accent, 0.6) }
                                            Rectangle { x: 40; y: 8; width: parent.width * 0.4; height: 4; color: theme.c.light_foreground }
                                            Rectangle { x: 40; y: 30; width: parent.width - 52; height: 2; color: theme.c.light_foreground; opacity: 0.6 }
                                        }
                                        Sub { text: lookCard.modelData.name }
                                        Text { Layout.fillWidth: true; wrapMode: Text.Wrap; text: lookCard.modelData.text; font.pixelSize: 12; color: theme.c.light_foreground }
                                    }
                                }
                            }
                        }
                    }

                    // ---- 4 Your music -------------------------------------------
                    ColumnLayout {
                        objectName: "setupStepMusic"
                        spacing: 6
                        RowLayout {
                            Layout.leftMargin: 18; Layout.rightMargin: 18
                            spacing: 12
                            PrimaryButton {
                                objectName: "setupAddFolder"
                                text: qsTr("Add a music folder…")
                                onClicked: { library.activate(); library.addFolder() }
                            }
                            Note {
                                text: qsTr("Optional. Omamp reads it in the background.")
                                color: theme.c.light_foreground
                            }
                        }
                        CheckBox {
                            objectName: "setupSpotify"
                            Layout.leftMargin: 12
                            text: qsTr("Turn on Spotify")
                            font.pixelSize: 13
                            checked: wiz.wantSpotify
                            onClicked: wiz.wantSpotify = checked
                        }
                        Note {
                            Layout.leftMargin: 52; Layout.rightMargin: 18; Layout.topMargin: -10
                            text: qsTr("Play Spotify beside your own files. You sign in on the Spotify page after Finish.")
                            color: theme.c.light_foreground
                            font.pixelSize: 12
                        }
                        Note {
                            Layout.leftMargin: 18; Layout.rightMargin: 18; Layout.topMargin: 4
                            textFormat: Text.StyledText
                            text: qsTr("<b>Internet radio</b> is ready to play: Nightride FM's synthwave channels and more. Press Shift+R.")
                        }
                        Note {
                            Layout.leftMargin: 18; Layout.rightMargin: 18
                            textFormat: Text.StyledText
                            text: qsTr("<b>A Navidrome or Subsonic server?</b> Add it later from the Library menu.")
                        }
                    }

                    // ---- 5 Done -------------------------------------------------
                    ColumnLayout {
                        objectName: "setupStepDone"
                        spacing: 6
                        Note {
                            Layout.leftMargin: 18; Layout.rightMargin: 18
                            text: qsTr("Everything you chose is in the View menu, and you can run this again from Help.")
                        }
                        GridLayout {
                            Layout.leftMargin: 18; Layout.rightMargin: 18; Layout.topMargin: 4
                            columns: 2
                            columnSpacing: 14
                            rowSpacing: 6
                            Repeater {
                                model: [
                                    ["Tab", qsTr("Switch between the player and the library")],
                                    ["Ctrl+K", qsTr("Find any command, artist, album or track")],
                                    ["?", qsTr("Show every key")],
                                    ["F10", qsTr("Open the menu bar")]
                                ]
                                delegate: RowLayout {
                                    required property var modelData
                                    Layout.columnSpan: 2
                                    spacing: 12
                                    Rectangle {
                                        implicitWidth: Math.max(58, keyText.implicitWidth + 14)
                                        implicitHeight: 24
                                        radius: Math.min(4, theme.radius)
                                        color: theme.c.darker_background
                                        border.width: 1
                                        border.color: theme.c.lighter_background
                                        Text {
                                            id: keyText
                                            anchors.centerIn: parent
                                            text: modelData[0]
                                            font.family: "JetBrainsMono Nerd Font"
                                            font.pixelSize: 12
                                            color: theme.c.bright_foreground
                                        }
                                    }
                                    Text { text: modelData[1]; font.pixelSize: 13; color: theme.c.foreground }
                                }
                            }
                        }
                    }
                }
            }

            // Skipping asks first.
            Rectangle {
                id: skipBar
                objectName: "setupSkipConfirm"
                visible: wiz.confirmingSkip
                Layout.fillWidth: true
                Layout.leftMargin: 12; Layout.rightMargin: 12
                implicitHeight: skipRow.implicitHeight + 8
                radius: Math.min(6, theme.radius)
                color: theme.c.darker_background
                RowLayout {
                    id: skipRow
                    anchors { fill: parent; leftMargin: 10; rightMargin: 4 }
                    Text {
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        text: qsTr("Skip setup? Everything goes back to how it was.")
                        font.pixelSize: 13
                        color: theme.c.bright_foreground
                    }
                    Button { text: qsTr("Keep going"); flat: true; font.pixelSize: 13; onClicked: wiz.confirmingSkip = false }
                    Button { objectName: "setupSkipYes"; text: qsTr("Skip"); flat: true; font.pixelSize: 13; onClicked: wiz.skip() }
                }
            }

            // Skip, the step dots, Back and Next.
            RowLayout {
                id: footerRow
                Layout.fillWidth: true
                Layout.leftMargin: 10; Layout.rightMargin: 12; Layout.bottomMargin: 4
                spacing: 6
                Button {
                    objectName: "setupSkip"
                    text: qsTr("Skip setup")
                    flat: true
                    font.pixelSize: 13
                    visible: !wiz.confirmingSkip && wiz.stepId !== "done"
                    onClicked: wiz.confirmingSkip = true
                }
                Item { Layout.fillWidth: true }
                Row {
                    spacing: 6
                    Repeater {
                        model: wiz.stepIds.length
                        Rectangle {
                            required property int index
                            width: 7; height: 7; radius: 3.5
                            color: index === wiz.step ? theme.accent : Qt.alpha(theme.c.foreground, 0.25)
                        }
                    }
                }
                Item { Layout.fillWidth: true }
                Button {
                    objectName: "setupBack"
                    text: qsTr("Back")
                    flat: true
                    font.pixelSize: 13
                    enabled: wiz.step > 0
                    onClicked: wiz.back()
                }
                PrimaryButton {
                    objectName: "setupNext"
                    text: wiz.stepId === "done" ? qsTr("Finish") : qsTr("Next")
                    onClicked: wiz.next()
                }
            }
        }
    }
}
