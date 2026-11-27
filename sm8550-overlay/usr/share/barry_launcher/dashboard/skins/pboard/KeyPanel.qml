pragma ComponentBehavior: Bound
// The Pboard skin's keys: the built-in keyboard (shell/KeyPanel.qml), whose
// behavior this keeps, drawn as a modern phone keyboard in its dark theme:
// flat rounded keys on a near-black panel, the letters a soft gray, Shift,
// Backspace, comma, period and Enter a blue-gray, ?123 and Enter as pills,
// space a plain letter key, and a key lighter while pressed. The built-in
// file's notes follow.
//
// The keys of Barry Launcher's on-screen keyboards: the bottom screen's own
// (Keyboard.qml) and the one that types on the top screen (TopInput.qml).
//
// Only how they look: what the keys are and do is the controller's
// (Barry.Input's KeyboardController: the pages, Shift and the sticky
// modifiers, glide typing, the strip, autocorrect, the keyboard packs).
// This lays the controller's rows out at any width, draws its state, and
// tells it about touches: a key going down and up, a tap, a hold, a glide.
// Colors are the skin's (Barry.Input's Skin); a skin can also replace this
// file (SkinView.qml) and keep the keyboard's behavior.
import QtQuick
import Barry.Input
import "file:///usr/share/barry_launcher/shell" as Shell

Rectangle {
    id: panel
    required property KeyboardController controller
    property real s: 1
    property real keyHeight: 92  // at 1240 x 1080

    readonly property real stripHeight: controller.glide ? 72 * s : 0
    // Five rows of keys (and the strip) plus the gaps and margins.
    implicitHeight: (5 * keyHeight + 4 * 10 + 24) * s + (controller.glide ? stripHeight + 10 * s : 0)
    color: Skin.colors.background  // the near-black behind the keys

    // Where the keys are, for glide typing.
    Binding { target: panel.controller; property: "panel"; value: panel }
    Binding { target: panel.controller; property: "keyUnit"; value: panel.unit }

    readonly property var labels: ({
        "Escape": "Esc", "Tab": "Tab", "Home": "Home", "End": "End", "Delete": "Del",
        "Left": "←", "Up": "↑", "Down": "↓", "Right": "→",
        "Prior": "PgUp", "Next": "PgDn", "keys": "Fn", "abc": "ABC",
        "ctrl": "Ctrl", "alt": "Alt", "super": "Super",
        "copy": "Copy", "paste": "Paste", "cut": "Cut", "selectall": "All", "undo": "Undo", "redo": "Redo",
    })
    // Key widths on the Fn page, in key units.
    readonly property var keyWidths: ({
        "Escape": 1.2, "Tab": 1.2, "Home": 1.2, "End": 1.2, "Prior": 1.2, "Next": 1.2, "Delete": 1.2,
        "copy": 1.5, "paste": 1.5, "cut": 1.5, "selectall": 1.5, "undo": 1.5, "redo": 1.5,
        "Left": 2, "Up": 2, "Down": 2, "Right": 2, "ctrl": 1.5, "alt": 1.5, "super": 1.5, "abc": 1.5,
    })
    // Key widths, in units: the unit is a tenth of the width, smaller when a
    // pack's row needs it.
    function keyUnits(k) {
        if (k === "space")
            return controller.manyLanguages && controller.page === "letters" ? 2 : 3
        if (controller.page === "keys" && keyWidths[k] !== undefined)
            return keyWidths[k]
        if (k === "shift" || k === "BackSpace")
            return 1.45
        if (k === "symbols" || k === "Return")
            return 1.5
        return 1
    }
    readonly property real gap: 10 * s
    readonly property real unit: {
        const avail = width - 24 * s
        let u = (avail - 9 * gap) / 10
        for (const row of controller.rows) {
            let units = 0
            for (const k of row)
                units += keyUnits(k)
            // space spans its gaps too
            const spaceGaps = row.indexOf("space") >= 0 ? (keyUnits("space") - 1) * gap : 0
            u = Math.min(u, (avail - (row.length - 1) * gap - spaceGaps) / units)
        }
        return u
    }

    // Take taps on the gaps, so they do not reach what is behind.
    TapHandler { gesturePolicy: TapHandler.ReleaseWithinBounds }

    Column {
        anchors.fill: parent
        anchors.margins: 12 * panel.s
        spacing: 10 * panel.s

        // The strip: the controller's words; tap one.
        Item {
            visible: panel.controller.glide
            width: parent.width
            height: panel.stripHeight

            Row {
                anchors.centerIn: parent
                height: parent.height
                spacing: 16 * panel.s

                Repeater {
                    model: panel.controller.alternatives
                    delegate: Rectangle {
                        id: alt
                        required property string modelData
                        required property int index
                        height: parent.height
                        width: Math.max(160 * panel.s, altLabel.implicitWidth + 48 * panel.s)
                        radius: 16 * panel.s
                        color: altTap.pressed ? Skin.colors.pressed : Skin.colors.raised

                        Text {
                            id: altLabel
                            anchors.centerIn: parent
                            text: panel.controller.stripMode === "undo" ? "“" + alt.modelData + "”" : alt.modelData
                            color: Skin.colors.text
                            font { family: Skin.font; pixelSize: 36 * panel.s }
                        }
                        TapHandler {
                            id: altTap
                            gesturePolicy: TapHandler.ReleaseWithinBounds
                            onPressedChanged: if (pressed) panel.controller.keyDown("strip")
                            onTapped: panel.controller.pickStrip(alt.index)
                        }
                    }
                }
            }
        }

        Repeater {
            model: panel.controller.rows
            delegate: Row {
                id: row
                required property var modelData
                required property int index
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: panel.gap

                Repeater {
                    model: row.modelData
                    delegate: Key {
                        required property string modelData
                        k: modelData
                        bottomRow: row.index === panel.controller.rows.length - 1
                    }
                }
            }
        }
    }

    // On top: the glide's trail, and a PointHandler that follows every touch
    // passively, leaving the keys their taps.
    Canvas {
        id: trailCanvas
        anchors.fill: parent
        Connections {
            target: panel.controller
            function onTrailChanged() { trailCanvas.requestPaint() }
        }

        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            const t = panel.controller.trail
            if (!panel.controller.gliding || t.length < 2)
                return
            ctx.globalAlpha = 0.75
            ctx.strokeStyle = Skin.colors.accent
            ctx.lineWidth = 12 * panel.s
            ctx.lineCap = "round"
            ctx.lineJoin = "round"
            ctx.beginPath()
            ctx.moveTo(t[0].x, t[0].y)
            for (let i = 1; i < t.length; i++)
                ctx.lineTo(t[i].x, t[i].y)
            ctx.stroke()
        }

        PointHandler {
            onActiveChanged: active ? panel.controller.glideStart(point.position.x, point.position.y)
                                    : panel.controller.glideEnd()
            onPointChanged: if (active) panel.controller.glideMove(point.position.x, point.position.y)
        }
    }

    // A held key's alternatives, above it; a tap beside them puts them away.
    property point moreAt  // above the key held
    MouseArea {
        anchors.fill: parent
        visible: panel.controller.moreFor !== ""
        z: 2
        onClicked: panel.controller.dismissMore()
    }
    Rectangle {
        id: morePopup
        visible: panel.controller.moreFor !== ""
        z: 3
        width: moreRow.width + 16 * panel.s
        height: panel.keyHeight * panel.s + 16 * panel.s
        radius: 18 * panel.s
        color: Skin.colors.raised
        border.color: Skin.colors.accent
        border.width: 2 * panel.s
        x: Math.max(0, Math.min(panel.width - width, panel.moreAt.x - width / 2))
        y: Math.max(0, panel.moreAt.y - height - 6 * panel.s)
        Row {
            id: moreRow
            anchors.centerIn: parent
            spacing: 8 * panel.s
            Repeater {
                model: panel.controller.moreAlts
                delegate: Rectangle {
                    id: alt
                    required property string modelData
                    width: Math.max(panel.unit, altText.implicitWidth + 24 * panel.s)
                    height: panel.keyHeight * panel.s
                    radius: 14 * panel.s
                    color: altPick.pressed ? Skin.colors.lit : Skin.colors.key
                    Text {
                        id: altText
                        anchors.centerIn: parent
                        text: alt.modelData
                        color: altPick.pressed ? Skin.colors.litText : Skin.colors.keyText
                        font { family: Skin.font; pixelSize: 44 * panel.s }
                    }
                    TapHandler {
                        id: altPick
                        gesturePolicy: TapHandler.ReleaseWithinBounds
                        onPressedChanged: if (pressed) panel.controller.keyDown(alt.modelData)
                        onTapped: panel.controller.pickMore(alt.modelData)
                    }
                }
            }
        }
    }

    component Key: Rectangle {

        // The space bar held as a cursor: the other keys fade.

        opacity: panel.controller.cursorMode && k !== "space" ? 0.35 : 1
        id: key
        property string k
        property bool bottomRow: false
        readonly property bool special: panel.controller.isNamed(k)
        // Enter only: space stays a letter key here.
        readonly property bool action: k === "Return"
        // Blue-gray: the named keys but space, and comma and period.
        readonly property bool tinted: (special && k !== "space") || k === "," || k === "."
        // What it types now (Shift's character while shifted).
        readonly property string out: { panel.controller.shifted; return panel.controller.output(k) }
        readonly property bool lit: (k === "shift" && panel.controller.shifted)
                                    || (k === "symbols" && panel.controller.page === "symbols")
                                    || (k === "keys" && panel.controller.page === "keys")
                                    || (k === "ctrl" && panel.controller.ctrlOn)
                                    || (k === "alt" && panel.controller.altOn)
                                    || (k === "super" && panel.controller.superOn)
        readonly property color face: tap.pressed ? Skin.colors.pressed : lit ? Skin.colors.lit
                                    : action ? Skin.colors.keyAction : tinted ? Skin.colors.keySpecial : Skin.colors.key
        readonly property color legend: tap.pressed ? Skin.colors.text : lit ? Skin.colors.litText
                                    : action && Skin.colors.keyActionText !== "" ? Skin.colors.keyActionText
                                    : tinted ? Skin.colors.keySpecialText : Skin.colors.keyText
        // A keycap's lower edge, when the skin has one: pressed, the cap
        // sinks onto it.
        readonly property real edge: 0  // flat keys

        width: panel.unit * panel.keyUnits(k) + (k === "space" ? (panel.keyUnits(k) - 1) * panel.gap : 0)
        height: panel.keyHeight * panel.s
        // ?123 and Enter are pills; the rest rounded.
        radius: k === "symbols" || k === "Return" ? height / 2 : 14 * panel.s
        color: face

        Rectangle {
            id: cap
            anchors.fill: parent
            radius: key.radius
            color: key.face
        }

        Text {
            anchors.centerIn: cap
            visible: !icon.visible
            text: key.k === "symbols" ? (panel.controller.page === "symbols" ? "ABC" : "?123")
                : key.k === "space" ? (panel.controller.manyLanguages ? panel.controller.language.name : "")
                : key.k === "lang" ? panel.controller.language.label
                : panel.labels[key.k] !== undefined ? panel.labels[key.k]
                : key.out
            color: key.legend
            font {
                family: Skin.font
                pixelSize: (key.k === "symbols" || key.k === "space" || key.k === "lang"
                            || (panel.labels[key.k] || "").length > 1 || key.out.length > 2 ? 32 : 44) * panel.s
            }
        }
        Shell.Icon {
            id: icon
            anchors.centerIn: cap
            width: 52 * panel.s
            height: 52 * panel.s
            visible: ["shift", "BackSpace", "Return", "hide"].indexOf(key.k) >= 0
            kind: key.k === "BackSpace" ? "backspace" : key.k === "Return" ? "enter" : key.k
            color: key.legend
            lineWidth: 5 * panel.s
        }

        // The letters glide typing follows.
        Component.onCompleted: panel.controller.registerKey(k, key)
        Component.onDestruction: panel.controller.unregisterKey(k, key)

        // The bottom row is touched down to the screen's edge: thumbs land
        // below it, in the panel's margin.
        Item {
            anchors { fill: parent; bottomMargin: key.bottomRow ? -12 * panel.s : 0 }

            TapHandler {
                id: tap
                gesturePolicy: TapHandler.ReleaseWithinBounds  // exclusive: not a tap outside too
                // Text keys: held, their alternatives. Named keys keep the
                // default: Backspace and the arrows repeat instead.
                longPressThreshold: key.special ? 0.8 : 0.4
                onPressedChanged: pressed ? panel.controller.keyDown(key.k) : panel.controller.keyUp(key.k)
                onTapped: panel.controller.tap(key.k)
                onLongPressed: {
                    const p = key.mapToItem(panel, key.width / 2, 0)
                    panel.moreAt = Qt.point(p.x, p.y)
                    panel.controller.longPress(key.k)
                }
            }
        }
    }
}
