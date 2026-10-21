import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import Thrum

// The visualiser picker: a small window of live previews, one tile per
// style, that opens beside the visualiser button in the large bar, the big
// view, the deck and the strip, or from the palette and the View menu
// ("Choose visualiser…"). A click or Enter picks a style; it applies at
// once and is kept in window.conf like everything in Settings > Appearance.
//
// For the large bar it also has the bar's places and whether the bar shares
// the player's style or has its own. Reduce motion and a way to the rest of
// Settings > Appearance are along the bottom.
//
// The previews exist only while the picker shows: the tiles are made when
// it opens and destroyed when it closes, so nothing animates behind it.
// While no sound reaches Thrum they play a made-up signal, so every style
// shows what it does rather than a flat line.
Popup {
    id: picker
    objectName: "visualiserPicker"
    // "bar" (the large bar: its places and its own style), "big", "player"
    // or "strip" (the shared style).
    property string where: "player"
    property Item anchorItem: null
    signal openSettings()

    readonly property var styles: appearance.visualisers
    readonly property bool forBar: where === "bar"
    // The large bar has its own style rather than the player's.
    readonly property bool own: appearance.barVisualiserStyle !== ""
    readonly property string current: forBar ? appearance.barStyle : appearance.visualiser
    // The tile the keys are on; it follows a style chosen any other way.
    property int cursor: 0
    onCurrentChanged: cursor = Math.max(0, styles.indexOf(current))
    // Four across, or fewer in a narrow window (the deck is 500 wide).
    readonly property int columns: parent ? Math.max(2, Math.min(4, Math.floor((parent.width - 16 - 2 * padding + 6) / 138))) : 4
    // The strip draws with the plain theme, everything else with the deck's.
    readonly property var skin: where === "strip" ? appearance.plain : appearance.deck
    // Real sound reaches the analyser.
    readonly property bool live: backend.state === "playing" && !backend.playsElsewhere
    // The previews are alive (the tests check they stop).
    readonly property bool previewing: tiles.status === Loader.Ready
    // Lines that show in light themes too, where lighter_background is white.
    readonly property color line: Qt.alpha(theme.c.foreground, 0.16)

    function pick(style) {
        if (forBar && own) appearance.barVisualiserStyle = style
        else appearance.visualiser = style
        cursor = Math.max(0, styles.indexOf(style))
    }
    function moveCursor(delta) {
        cursor = Math.max(0, Math.min(styles.length - 1, cursor + delta))
    }

    // Opens beside `anchor` (or in the middle of the window), or closes when
    // it is already open for that anchor.
    property double closedAt: 0
    property bool closingByKey: false
    function show(anchor, kind) {
        if (anchor && anchor === anchorItem && (visible || Date.now() - closedAt < 300)) {
            // The press that reached the button also closed the picker.
            close()
            return
        }
        where = kind || "player"
        anchorItem = anchor
        cursor = Math.max(0, styles.indexOf(current))
        open()
        place()
    }
    function place() {
        const area = parent
        if (!area) return
        const w = implicitWidth, h = implicitHeight
        if (!anchorItem) {
            x = Math.round((area.width - w) / 2)
            y = Math.round((area.height - h) / 2)
            return
        }
        const at = anchorItem.mapToItem(area, 0, 0)
        // Above or below the button's `edgeItem` (the whole large bar, so
        // the picker never covers it), else the button itself: below when
        // there is room, else above.
        const edge = anchorItem.edgeItem ? anchorItem.edgeItem : anchorItem
        const top = edge.mapToItem(area, 0, 0).y
        const below = top + edge.height + 6
        y = below + h <= area.height - 8 || top - h - 6 < 8 ? below : top - h - 6
        // Right edges together in the right half, left edges in the left.
        x = at.x + anchorItem.width / 2 > area.width / 2 ? at.x + anchorItem.width - w : at.x
        x = Math.max(8, Math.min(area.width - w - 8, x))
        y = Math.max(8, Math.min(area.height - h - 8, y))
    }
    onImplicitHeightChanged: if (visible) place()

    Connections {
        target: appearance
        function onPickerRequested(anchor, where) { picker.show(anchor, where) }
    }

    parent: Overlay.overlay
    modal: false
    dim: false
    focus: true
    padding: 14
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    Material.roundedScale: theme.radius
    onOpened: {
        appearance.pickerAnchor = anchorItem
        grid.forceActiveFocus()
    }
    onClosed: {
        // A press outside closes it before the click lands; when that press
        // was on the button it opened from, the click must not open it again.
        closedAt = closingByKey ? 0 : Date.now()
        closingByKey = false
        appearance.pickerAnchor = null
    }

    background: Rectangle {
        color: theme.c.background
        radius: Math.min(8, theme.radius)
        border.width: 1
        border.color: Qt.alpha(theme.c.foreground, 0.22)
        // A soft edge, drawn without a GPU effect.
        Rectangle {
            z: -1
            anchors { fill: parent; margins: -3 }
            radius: parent.radius + 3
            color: "transparent"
            border.width: 3
            border.color: Qt.alpha("black", theme.light ? 0.06 : 0.25)
        }
    }

    // ---- The made-up signal, while nothing plays -------------------------
    property real phase: 0
    property var fakeLevels: []
    property var fakePeaks: []
    property var fakeWave: []
    property var fakeStereo: []
    Timer {
        interval: 33
        repeat: true
        running: picker.visible && !picker.live
        triggeredOnStart: true
        onTriggered: {
            picker.phase += 0.12
            const p = picker.phase
            const levels = [], peaks = [], wave = [], stereo = []
            for (let i = 0; i < 20; ++i) {
                const beat = Math.max(0, Math.sin(p * 2.2)) * (i < 4 ? 0.35 : 0.1)
                const v = 0.4 + beat + 0.3 * Math.sin(p * (1 + i * 0.07) + i * 0.6) * Math.cos(p * 0.4 + i * 0.2)
                levels.push(Math.max(0.04, Math.min(1, v * (1 - i * 0.03))))
                peaks.push(Math.min(1, levels[i] + 0.08))
            }
            for (let j = 0; j < 96; ++j)
                wave.push(0.55 * Math.sin(j * 0.33 + p * 3) * Math.cos(j * 0.05 + p))
            for (let k = 0; k < 96; ++k)
                stereo.push(wave[k])
            for (let k = 0; k < 96; ++k)
                stereo.push(0.5 * Math.sin(k * 0.29 + p * 2.6) * Math.cos(k * 0.06 - p))
            picker.fakeLevels = levels
            picker.fakePeaks = peaks
            picker.fakeWave = wave
            picker.fakeStereo = stereo
        }
    }

    // ---- Pieces ----------------------------------------------------------
    component Chip: AbstractButton {
        id: chip
        property bool chosen: false
        hoverEnabled: true
        // Tab reaches it; a click leaves the keys on the tiles.
        focusPolicy: Qt.TabFocus
        leftPadding: 10; rightPadding: 10
        implicitHeight: 26
        font.pixelSize: 12
        contentItem: Text {
            text: chip.text
            font: chip.font
            color: chip.chosen ? theme.c.bright_foreground : theme.c.foreground
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
        background: Rectangle {
            radius: Math.min(13, theme.radius > 0 ? theme.radius : 0)
            color: chip.chosen ? Qt.alpha(theme.accent, 0.22) : chip.hovered ? theme.c.selection : "transparent"
            border.width: chip.chosen || chip.visualFocus ? 2 : 1
            border.color: chip.chosen || chip.visualFocus ? theme.accent : picker.line
        }
        Keys.onReturnPressed: clicked()
        Keys.onEnterPressed: clicked()
    }
    component Caption: Text {
        font.pixelSize: 12
        color: theme.c.muted
    }

    contentItem: ColumnLayout {
        id: grid
        objectName: "pickerKeys"
        spacing: 10
        // Takes the keys: arrows move, Enter picks and closes, Space picks.
        focus: true
        Keys.onPressed: (event) => {
            switch (event.key) {
            case Qt.Key_Left: picker.moveCursor(-1); break
            case Qt.Key_Right: picker.moveCursor(1); break
            case Qt.Key_Up: picker.moveCursor(-picker.columns); break
            case Qt.Key_Down: picker.moveCursor(picker.columns); break
            case Qt.Key_Home: picker.cursor = 0; break
            case Qt.Key_End: picker.cursor = picker.styles.length - 1; break
            case Qt.Key_Space: picker.pick(picker.styles[picker.cursor]); break
            case Qt.Key_Return: case Qt.Key_Enter:
                picker.pick(picker.styles[picker.cursor])
                picker.closingByKey = true
                picker.close()
                break
            case Qt.Key_Escape:
                picker.closingByKey = true
                picker.close()
                break
            default: return
            }
            event.accepted = true
        }

        RowLayout {
            Layout.fillWidth: true
            Text {
                Layout.fillWidth: true
                text: picker.forBar ? qsTr("Visualiser in the now playing bar") : qsTr("Visualiser")
                font.pixelSize: 14
                font.bold: true
                color: theme.c.bright_foreground
            }
            Caption {
                text: picker.live ? qsTr("Previews follow what's playing") : qsTr("Previews play a sample")
            }
        }

        // The places, for the large bar.
        ColumnLayout {
            visible: picker.forBar
            Layout.fillWidth: true
            spacing: 6
            RowLayout {
                spacing: 6
                Caption { text: qsTr("Where:"); Layout.preferredWidth: 52 }
                Repeater {
                    // Short names; Settings has the long ones.
                    model: [
                        ["full", qsTr("Whole bar")], ["behind", qsTr("Seek bar")], ["edge", qsTr("Edge")],
                        ["beside", qsTr("Beside cover")], ["off", qsTr("Off")]
                    ]
                    Chip {
                        required property var modelData
                        objectName: "pickerPlace_" + modelData[0]
                        text: modelData[1]
                        chosen: appearance.barVisualiser === modelData[0]
                        ToolTip.visible: hovered
                        ToolTip.delay: 600
                        ToolTip.text: appearance.barPlacementName(modelData[0])
                        onClicked: appearance.barVisualiser = modelData[0]
                    }
                }
            }
            RowLayout {
                spacing: 6
                Caption { text: qsTr("Style:"); Layout.preferredWidth: 52 }
                Chip {
                    objectName: "pickerShared"
                    text: qsTr("Same as the player")
                    chosen: !picker.own
                    onClicked: appearance.barVisualiserStyle = ""
                }
                Chip {
                    objectName: "pickerOwn"
                    text: qsTr("Own style")
                    chosen: picker.own
                    // Starts from the style it shows now, so nothing jumps.
                    onClicked: if (!picker.own) appearance.barVisualiserStyle = appearance.visualiser
                }
            }
        }

        // The tiles, made only while the picker shows.
        Loader {
            id: tiles
            Layout.fillWidth: true
            active: picker.visible
            sourceComponent: Grid {
                objectName: "pickerGrid"
                columns: picker.columns
                spacing: 6
                Repeater {
                    model: picker.styles
                    AbstractButton {
                        id: tile
                        required property string modelData
                        required property int index
                        objectName: "pickerTile_" + modelData
                        readonly property bool chosen: picker.current === modelData
                        readonly property bool atCursor: picker.cursor === index
                        width: 132
                        height: 76
                        hoverEnabled: true
                        focusPolicy: Qt.NoFocus
                        padding: 6
                        onClicked: picker.pick(modelData)
                        background: Rectangle {
                            radius: Math.min(6, theme.radius)
                            color: tile.chosen ? Qt.alpha(theme.accent, 0.16) : tile.hovered ? theme.c.selection : "transparent"
                            border.width: tile.atCursor || tile.chosen ? 2 : 1
                            border.color: tile.atCursor ? theme.accent
                                        : tile.chosen ? Qt.alpha(theme.accent, 0.6) : picker.line
                        }
                        contentItem: ColumnLayout {
                            spacing: 4
                            Rectangle {
                                Layout.preferredWidth: 120
                                Layout.preferredHeight: 48
                                Layout.alignment: Qt.AlignHCenter
                                radius: Math.min(3, theme.radius)
                                color: picker.skin.lcd
                                clip: true
                                Visualiser {
                                    objectName: "pickerPreview"
                                    anchors.fill: parent
                                    anchors.margins: 2
                                    style: tile.modelData
                                    levels: picker.live ? backend.levels : picker.fakeLevels
                                    peaks: picker.live ? backend.peaks : picker.fakePeaks
                                    waveform: picker.live ? backend.waveform : picker.fakeWave
                                    stereoWaveform: tile.modelData !== "ribbon" ? [] : picker.live ? backend.stereoWaveform : picker.fakeStereo
                                    reduceMotion: appearance.reduceMotion
                                    low: picker.skin.visLow
                                    mid: picker.skin.visMid
                                    high: picker.skin.visHigh
                                    peak: picker.skin.visPeak
                                    spacing: 1
                                }
                            }
                            Text {
                                Layout.fillWidth: true
                                horizontalAlignment: Text.AlignHCenter
                                elide: Text.ElideRight
                                text: (tile.chosen ? "✓ " : "") + appearance.visualiserName(tile.modelData)
                                font.pixelSize: 12
                                font.bold: tile.chosen
                                color: tile.chosen ? theme.c.bright_foreground : theme.c.foreground
                            }
                        }
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            CheckBox {
                objectName: "pickerReduceMotion"
                text: qsTr("Reduce motion")
                font.pixelSize: 12
                padding: 0
                implicitHeight: 28
                topInset: 0; bottomInset: 0
                focusPolicy: Qt.TabFocus
                checked: appearance.reduceMotion
                onClicked: appearance.reduceMotion = checked
            }
            // How readily the bars fill (the analyser's auto-range).
            Caption { text: qsTr("Sensitivity") }
            Repeater {
                model: appearance.sensitivities
                Chip {
                    required property string modelData
                    objectName: "pickerSensitivity_" + modelData
                    text: appearance.sensitivityName(modelData)
                    chosen: appearance.visualiserSensitivity === modelData
                    onClicked: appearance.visualiserSensitivity = modelData
                }
            }
            Caption {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignRight
                text: qsTr("Arrows, Enter to pick, Esc to close")
                elide: Text.ElideRight
            }
            Chip {
                objectName: "pickerMoreSettings"
                text: qsTr("More settings…")
                onClicked: {
                    picker.close()
                    picker.openSettings()
                }
            }
        }
    }
}
