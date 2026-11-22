pragma ComponentBehavior: Bound
// Barry Launcher's quick settings: the page to the right of the apps, a
// grid of widgets the user picks and arranges (a long press on a toggle or
// a widget's name: Customize). Wi-Fi and Bluetooth, the screens' brightness
// (both at once, or each), the volume (the top screen's, which is the
// system's, and the bottom screen's apps' on top of it), the stick lights, a
// screenshot or recording of the top screen, the fan profile, how soon Barry
// goes back to the game art, charging, and the game profile of the game
// running (else the one Steam played last). Brightness, charging and game
// profiles are the PB-OS tab's own settings, read and changed through
// pbos-qamd (Barry.Input's PbosQam); fan and lights are the dashboard's
// (barry_launcher_statsd's /controls); Wi-Fi, Bluetooth, volume and capture
// are statsd's /quick; how soon Barry goes back to the game art is Barry's
// own setting (statsd's /settings standbySeconds). On a device without a
// Barry button (the Retroid Pocket Duo) Steam's slider is the top screen's,
// so Brightness has the bottom screen's alone, and Screen off puts the
// bottom screen to sleep (statsd's /quick; a touch wakes it). A setting this
// device lacks is left out. While the page shows, it reads them again every few
// seconds (the charger may come or go). Colors are the skin's; a skin can
// also replace this file (SkinView.qml).
import QtQuick
import QtQuick.Layouts
import Barry.Input

Rectangle {
    id: qs
    property real s: 1
    property bool shown: false
    // Untouched this long, Barry goes back to the game art (0: never);
    // Shell.qml sets it and hears a new choice.
    property int standbySeconds: 15
    signal standbyChosen(int seconds)
    readonly property var standbyChoices: [10, 15, 30, 60, 0]
    // Over the game art, frosted (Shell.qml sets it): how solid this page's
    // own background is, 1 fully.
    property real glass: 1
    function tint(color) {
        const c = Qt.color(color)
        return Qt.rgba(c.r, c.g, c.b, c.a * glass)
    }

    readonly property string statsd: "http://127.0.0.1:47823"
    property var screens: null      // { top, bottom } 0-100
    property bool topSlider: true   // the top screen's brightness is set here (pbos-qamd's get_state)
    // Where the "Both screens" slider was last put (-1: never, it starts at
    // the two screens' average): moving one screen's slider leaves it be.
    property int both: -1
    property var controls: ({})     // statsd /controls
    property var quick: ({})        // statsd /quick
    readonly property var light: controls.lighting || null
    readonly property var capture: quick.capture || ({})
    property string captureNote: ""  // the last screenshot or recording
    property double recordingSince: 0
    property var charge: null       // tools get_charge_control
    property var profiles: null     // tools get_game_profiles
    property string error: ""
    property double movedAt: 0      // a slider moved: leave it alone a moment

    readonly property var fanNames: ({ eco: "Quiet", balanced: "Balanced", performance: "Performance", max: "Max" })
    readonly property var chargeModes: [
        { id: "normal", label: "Normal", desc: "Charges to 100 %." },
        { id: "limit80", label: "Stop at 80 %", desc: "The battery firmware stops charging at 80 %." },
        { id: "hold", label: "Hold", desc: "Charges to the level below, then leaves the battery alone." },
        { id: "bypass", label: "Run from USB", desc: "Runs from USB power and doesn't charge.", feature: "bypass" },
    ]
    readonly property var coreChoices: [
        { id: "all", label: "All" },
        { id: "performance", label: "Big + Prime" },
        { id: "big", label: "Big" },
        { id: "little", label: "Little" },
    ]

    color: tint(Skin.colors.background)

    function settled() { return Date.now() - movedAt > 2000 }

    function refresh() {
        PbosQam.call("barry", "get_state", [], function (ok, v) {
            if (ok && v && v.brightness && qs.settled())
                qs.screens = v.brightness
            if (ok && v)
                qs.topSlider = v.topSlider !== false
        })
        PbosQam.call("tools", "get_charge_control", [], function (ok, v) {
            if (ok && qs.settled())
                qs.charge = v && v.supported ? v : null
        })
        PbosQam.call("tools", "get_game_profiles", [], function (ok, v) {
            if (ok)
                qs.profiles = v && v.available ? v : null
            qs.error = ok || PbosQam.connected ? "" : "The PB-OS tab's backend (pbos-qamd) isn't answering."
        })
        statsdRequest("GET", "/controls", null, function (c) { if (c) qs.controls = c })
        statsdRequest("GET", "/quick", null, function (q) { if (q && qs.settled()) qs.takeQuick(q) })
        statsdRequest("GET", "/widgets", null, function (w) {
            if (!w || !w.widgets)
                return
            const map = {}
            for (const p of w.widgets)
                if (p.kind === "pack")
                    map[p.id] = p
            if (JSON.stringify(map) !== JSON.stringify(qs.packs))
                qs.packs = map
        })
    }

    function takeQuick(q) {
        if (q.capture && q.capture.recording && !(quick.capture && quick.capture.recording))
            recordingSince = Date.now() - q.capture.seconds * 1000
        if (q.capture && !q.capture.recording && q.capture.last)
            captureNote = q.capture.last
        if (q.error)
            captureNote = q.error
        quick = q
    }

    function setQuick(body) {
        movedAt = Date.now()
        statsdRequest("POST", "/quick", body, function (q) {
            if (!q) {
                qs.captureNote = "Barry's stats service (barry_launcher_statsd) isn't answering."
                return
            }
            if (q.saved)
                qs.captureNote = "Saved " + q.saved.split("/").pop() + " in Pictures/Screenshots"
            qs.movedAt = 0
            qs.takeQuick(q)
        })
    }

    function setVolume(which, value) {
        const v = {}
        v[which] = value
        quick = Object.assign({}, quick, { volume: Object.assign({}, quick.volume, v) })
        setQuick({ volume: v })
    }

    function setBoth(value) {
        movedAt = Date.now()
        both = value
        screens = { top: value, bottom: value }
        sendBrightness("top", value)
        sendBrightness("bottom", value)
    }

    // set_brightness, one call at a time: pbos-qamd runs each call on a
    // thread of its own and each rewrites both screens' file, so two at
    // once (Both screens, or a quick drag) lost one screen's value, and a
    // drag's calls could land out of order. A move while a call is out
    // waits; only each screen's latest value goes.
    property var brightnessPending: ({})
    property bool brightnessBusy: false
    function sendBrightness(screen, value) {
        brightnessPending[screen] = value
        flushBrightness()
    }
    function flushBrightness() {
        if (brightnessBusy)
            return
        const screen = ["top", "bottom"].find(function (s) { return brightnessPending[s] !== undefined })
        if (!screen)
            return
        const value = brightnessPending[screen]
        delete brightnessPending[screen]
        brightnessBusy = true
        PbosQam.call("barry", "set_brightness", [screen, value], function () {
            qs.brightnessBusy = false
            qs.flushBrightness()
        })
    }

    function setLight(patch) {
        movedAt = Date.now()
        statsdRequest("POST", "/controls", { lighting: patch }, function (c) { if (c) qs.controls = c })
    }

    // The light colour slider: white at its left end, then the hues round
    // to red again.
    readonly property real whiteEnd: 0.07
    function colorAt(p) {
        if (p < whiteEnd)
            return "ffffff"
        return Qt.hsva(Math.min(1, (p - whiteEnd) / (1 - whiteEnd)) * 0.999, 1, 1, 1).toString().substring(1)
    }
    function positionOf(hex) {
        const c = Qt.color("#" + hex)
        if (c.hsvSaturation < 0.25)
            return whiteEnd / 2
        return whiteEnd + Math.max(0, c.hsvHue) * (1 - whiteEnd)
    }
    function clock(seconds) {
        const m = Math.floor(seconds / 60), s = seconds % 60
        return m + ":" + (s < 10 ? "0" : "") + s
    }

    function statsdRequest(method, path, body, callback) {
        const x = new XMLHttpRequest()
        x.onreadystatechange = function () {
            if (x.readyState !== XMLHttpRequest.DONE)
                return
            let obj = null
            if (x.status === 200) {
                try { obj = JSON.parse(x.responseText) } catch (e) { obj = null }
            }
            callback(obj)
        }
        x.open(method, statsd + path)
        if (body) {
            x.setRequestHeader("Content-Type", "application/json")
            x.send(JSON.stringify(body))
        } else {
            x.send()
        }
    }

    function setBrightness(screen, value) {
        movedAt = Date.now()
        // "Both screens" keeps where it showed (else, at the average, it
        // moved along with this one).
        if (both < 0 && screens)
            both = Math.round((screens.top + screens.bottom) / 2)
        const next = Object.assign({}, screens)
        next[screen] = value
        screens = next
        sendBrightness(screen, value)
    }

    function setCharge(patch) {
        movedAt = Date.now()
        const next = Object.assign({}, charge.settings, patch)
        charge = Object.assign({}, charge, { settings: next })
        PbosQam.call("tools", "set_charge_control", [next], function (ok, v) {
            if (ok && v && v.supported && qs.settled())
                qs.charge = v
        })
    }

    readonly property string game: SteamGame.profileAppid
    readonly property var profile: profiles && profiles.games && profiles.games[game] ? profiles.games[game] : ({})

    function setProfile(patch) {
        const next = Object.assign({}, profile, patch, { name: SteamGame.profileName })
        for (const k in patch)
            if (patch[k] === undefined)
                delete next[k]
        const games = Object.assign({}, profiles.games)
        games[game] = next
        profiles = Object.assign({}, profiles, { games: games })
        PbosQam.call("tools", "set_game_profile", [game, next], function (ok, v) {
            if (!ok || !v || !v.ok) {
                qs.error = "Not saved: " + (ok && v ? v.error || "unknown error" : v)
                return
            }
            qs.error = ""
            const all = Object.assign({}, qs.profiles.games)
            if (v.profile)
                all[qs.game] = v.profile
            else
                delete all[qs.game]
            qs.profiles = Object.assign({}, qs.profiles, { games: all })
        })
    }

    onShownChanged: {
        if (shown)
            refresh()
        else
            editing = false
    }
    Timer {
        interval: 5000
        repeat: true
        running: qs.shown
        onTriggered: qs.refresh()
    }


    // The recording's clock, while one runs.
    property int recordingSeconds: 0
    Timer {
        interval: 1000
        repeat: true
        triggeredOnStart: true
        running: qs.shown && !!qs.capture.recording
        onTriggered: qs.recordingSeconds = Math.max(0, Math.round((Date.now() - qs.recordingSince) / 1000))
    }

    // ---- The widgets and their layout ----
    //
    // The page is a grid four units wide. Each widget takes 1-4 units of a
    // row (some need 2 at least); rows fill in the order chosen, and a row
    // with room left over widens its widgets, so every row is full and the
    // widgets always make one even rectangle. The rows share the height by
    // what their widgets need; if they need more than the screen, all of it
    // is drawn smaller (ws) to fit. The order and widths are the user's
    // (statsd's settings quickLayout); hidden widgets are left out of it.
    // Widget packs (statsd's /widgets: the user's and the shipped ones), by
    // id ("w-<name>"): { title, url, minSpan, span, height }.
    property var packs: ({})
    readonly property var catalog: {
        const all = Object.assign({}, builtins)
        for (const id in packs)
            all[id] = { title: packs[id].title, min: packs[id].minSpan }
        return all
    }
    readonly property var builtins: ({
        wifi: { title: "Wi-Fi", min: 1 },
        bluetooth: { title: "Bluetooth", min: 1 },
        screenshot: { title: "Screenshot", min: 1 },
        record: { title: "Record", min: 1 },
        brightness: { title: "Brightness", min: 2 },
        volume: { title: "Volume", min: 2 },
        lights: { title: "Lights", min: 2 },
        fan: { title: "Fan", min: 2 },
        standby: { title: "Back to the game art", min: 2 },
        battery: { title: "Charging", min: 2 },
        game: { title: "Game profile", min: 2 },
        screenoff: { title: "Screen off", min: 1 },
    })
    readonly property var defaultLayout: [
        { id: "wifi", span: 1 }, { id: "bluetooth", span: 1 }, { id: "screenshot", span: 1 }, { id: "record", span: 1 },
        { id: "brightness", span: 2 }, { id: "volume", span: 2 },
        { id: "lights", span: 2 }, { id: "fan", span: 2 },
    ].concat(Device.barryButton === "" ? [{ id: "screenoff", span: 2 }] : []).concat([
        { id: "battery", span: 2 }, { id: "game", span: 2 },
        { id: "standby", span: 4 },
    ])
    // The saved layout, else the default. Ids not known (yet: the packs
    // come a moment later; or a pack removed) stay in it, undrawn.
    property var layout: {
        // (A list from C++ is no JS Array: copied into one.)
        const saved = Skin.settings.quickLayout
        const list = saved && saved.length ? Array.from(saved, w => ({ id: w.id, span: w.span })) : defaultLayout
        return list.filter(w => w && typeof w.id === "string")
    }
    function saveLayout(list) {
        layout = list
        statsdRequest("POST", "/settings", { quickLayout: list }, function () {})
    }

    // This device has it (else it is left out, wherever it is placed).
    function available(id) {
        switch (id) {
        case "wifi": return !!quick.wifi
        case "bluetooth": return !!quick.bluetooth
        case "screenshot": case "record": return !!quick.capture
        case "brightness": return !!screens
        case "volume": return !!quick.volume
        case "lights": return !!light
        case "fan": return !!controls.fan
        // The game art's timer: Game Mode's only (no art in Desktop Mode).
        case "standby": return Skin.settings.heroArt !== false && Qt.application.arguments.indexOf("desktop") < 0
        case "battery": return !!charge
        case "game": return !!profiles
        // No Barry button: the bottom screen to sleep (Game Mode's).
        case "screenoff": return Device.barryButton === "" && !!quick.screenOff
                                 && Qt.application.arguments.indexOf("desktop") < 0
        }
        return packs[id] !== undefined
    }

    // What a widget needs, at scale 1 (qs.s).
    readonly property real pad: 18 * s
    readonly property real rowHeight: 56 * s
    readonly property real titleHeight: 30 * s
    readonly property real spacing: 10 * s
    function natural(id, span) {
        const box = 2 * pad, t = titleHeight + spacing, r = rowHeight
        switch (id) {
        case "wifi": case "bluetooth": case "screenshot": case "record": case "screenoff":
            return 120 * s
        case "brightness": return topSlider ? box + 3 * r + 2 * spacing : box + r
        case "volume": return box + 2 * r + spacing
        case "lights": return box + t + 46 * s + spacing + r
        case "fan": case "standby": return box + t + r
        case "battery": {
            const mode = charge && charge.settings ? charge.settings.mode : ""
            const rows = 1 + (mode === "hold" ? 1 : 0) + (charge && charge.features && charge.features.slow ? 1 : 0)
            return box + t + rows * r + (rows - 1) * spacing + spacing + (span >= 4 ? 1 : 2) * 26 * s
        }
        case "game":
            return box + t + (span >= 4 ? 1 : 2) * 26 * s + (game !== "" ? 2 * (r + spacing) : 0)
        }
        return packs[id] ? packs[id].height * s : 120 * s
    }

    // [{ id, span }] -> rows of [{ id, span }], every row four units.
    function pack(list) {
        const rows = []
        let row = [], used = 0
        for (const w of list) {
            if (!available(w.id))
                continue
            const span = Math.max(catalog[w.id].min, Math.min(4, w.span || 1))
            if (used + span > 4) {
                rows.push(row)
                row = []
                used = 0
            }
            row.push({ id: w.id, span: span })
            used += span
        }
        if (row.length)
            rows.push(row)
        // Room left in a row: its widgets widen, from the last one back.
        for (const r of rows) {
            let left = 4 - r.reduce((a, w) => a + w.span, 0)
            for (let i = r.length - 1; left > 0; i = (i - 1 + r.length) % r.length) {
                r[i].span++
                left--
            }
        }
        return rows
    }

    readonly property real margin: 24 * s
    readonly property real gap: 14 * s
    // The edit bar takes the bottom while customizing.
    readonly property real barHeight: editing ? 96 * s : 0
    readonly property var rows: pack(layout)
    // Placed widgets: { id, x, y, w, h }, and ws, the scale they draw at.
    readonly property var placed: {
        const W = width - 2 * margin
        const H = height - 2 * margin - barHeight - (editing ? gap : 0)
        const unit = (W - 3 * gap) / 4
        const needs = rows.map(r => Math.max(...r.map(w => natural(w.id, w.span))))
        const gaps = Math.max(0, rows.length - 1) * gap
        const total = needs.reduce((a, b) => a + b, 0)
        const k = total > 0 ? Math.min(1, (H - gaps) / total) : 1
        const extra = Math.max(0, H - gaps - total * k)
        const out = []
        let y = margin
        rows.forEach((r, i) => {
            const h = needs[i] * k + (total > 0 ? extra * needs[i] / total : 0)
            let col = 0
            for (const w of r) {
                out.push({ id: w.id, span: w.span, x: margin + col * (unit + gap), y: y,
                           w: w.span * unit + (w.span - 1) * gap, h: h })
                col += w.span
            }
            y += h + gap
        })
        return { list: out, k: k }
    }
    readonly property real ws: s * placed.k
    // The widgets shown, by id and in no order: the delegates are made again
    // only when one comes or goes, not when one moves (a dragged widget, or
    // a slider, keeps its touch).
    readonly property string shownIds: placed.list.map(p => p.id).sort().join(" ")
    function place(id) {
        return placed.list.find(p => p.id === id) || { id: id, span: 1, x: 0, y: 0, w: 0, h: 0 }
    }

    // ---- Customizing ----
    property bool editing: false
    function indexOf(id) { return layout.findIndex(w => w.id === id) }
    // The width it is drawn at now (a row may have widened it).
    function shownSpan(id) {
        const p = placed.list.find(w => w.id === id)
        return p ? p.span : 1
    }
    function resize(id, by) {
        const list = layout.map(w => Object.assign({}, w))
        const w = list[indexOf(id)]
        w.span = Math.max(catalog[id].min, Math.min(4, shownSpan(id) + by))
        saveLayout(list)
    }
    function hide(id) { saveLayout(layout.filter(w => w.id !== id)) }
    function add(id) {
        saveLayout(layout.concat([{ id: id, span: packs[id] ? packs[id].span : catalog[id].min }]))
    }
    readonly property var hidden: Object.keys(catalog).filter(id => indexOf(id) < 0 && available(id))

    // ---- The look: the home screen's (Home.qml) tiles, the theme's colors ----
    readonly property string font: Skin.font || "Noto Sans"
    // Over the game art: the widgets see-through as Home's tiles are.
    readonly property real widgetGlass: glass < 1 ? 0.6 : 1
    function surface(color) {
        const c = Qt.color(color)
        return Qt.rgba(c.r, c.g, c.b, c.a * widgetGlass)
    }

    component Title: Text {
        color: Skin.colors.text
        opacity: 0.75
        Layout.preferredHeight: qs.titleHeight * qs.placed.k
        verticalAlignment: Text.AlignVCenter
        font { family: qs.font; pixelSize: 24 * qs.ws; weight: Font.DemiBold }
    }
    component Note: Text {
        color: Skin.colors.text
        opacity: 0.65
        wrapMode: Text.Wrap
        Layout.fillWidth: true
        font { family: qs.font; pixelSize: 20 * qs.ws }
    }

    // A quick toggle or action, as a home screen tile: the icon, the name,
    // its state. Lit while on.
    component Toggle: Rectangle {
        id: tg
        property string label
        property string state_
        property string icon
        property bool on: false
        signal tapped()
        radius: 40 * qs.ws
        color: tgTap.pressed ? Skin.colors.pressed : on ? Skin.colors.lit : qs.surface(Skin.colors.panel)
        border.color: on ? Skin.colors.accent : Skin.colors.border
        border.width: 3 * qs.ws
        readonly property color ink: on && !tgTap.pressed ? Skin.colors.litText : Skin.colors.text
        Row {
            anchors.centerIn: parent
            spacing: 14 * qs.ws
            Icon {
                anchors.verticalCenter: parent.verticalCenter
                width: 46 * qs.ws
                height: width
                kind: tg.icon
                color: tg.on ? tg.ink : Skin.colors.icon
                lineWidth: 5 * qs.ws
            }
            Column {
                anchors.verticalCenter: parent.verticalCenter
                Text {
                    text: tg.label
                    color: tg.ink
                    font { family: qs.font; pixelSize: 27 * qs.ws; weight: Font.DemiBold }
                }
                Text {
                    text: tg.state_
                    color: tg.ink
                    opacity: 0.7
                    font { family: qs.font; pixelSize: 21 * qs.ws }
                }
            }
        }
        TapHandler {
            id: tgTap
            gesturePolicy: TapHandler.ReleaseWithinBounds
            longPressThreshold: 0.6
            onTapped: tg.tapped()
            // A held button keeps its touch from the widget: it starts
            // customizing itself (a long press is no tap).
            onLongPressed: qs.editing = true
        }
    }

    // A row of choices in one pill; the chosen one lit. Each takes room by
    // its label's length, all in one size (smaller only if they must be).
    component Segmented: Rectangle {
        id: seg
        property var options: []   // [{ id, label }]
        property var current
        signal chosen(var id)
        Layout.fillWidth: true
        Layout.preferredHeight: qs.rowHeight * qs.placed.k
        radius: height / 2
        color: Skin.colors.raised
        FontMetrics {
            id: fm
            font { family: qs.font; pixelSize: 22 * qs.ws; weight: Font.DemiBold }
        }
        readonly property real inset: 5 * qs.ws
        readonly property real inner: width - 2 * inset - Math.max(0, options.length - 1) * 4 * qs.ws
        readonly property var wants: options.map(o => fm.advanceWidth(String(o.label)) + 30 * qs.ws)
        readonly property real total: Math.max(1, wants.reduce((a, b) => a + b, 0))
        readonly property real fontSize: 22 * qs.ws * Math.max(0.65, Math.min(1, inner / total))
        Row {
            x: seg.inset
            y: seg.inset
            spacing: 4 * qs.ws
            Repeater {
                model: seg.options
                delegate: Rectangle {
                    id: opt
                    required property var modelData
                    required property int index
                    readonly property bool active: seg.current === modelData.id
                    width: seg.inner * seg.wants[index] / seg.total
                    height: seg.height - 2 * seg.inset
                    radius: height / 2
                    color: optTap.pressed ? Skin.colors.pressed : active ? Skin.colors.lit : "transparent"
                    Text {
                        anchors.centerIn: parent
                        width: parent.width - 8 * qs.ws
                        text: opt.modelData.label
                        color: opt.active && !optTap.pressed ? Skin.colors.litText : Skin.colors.text
                        font { family: qs.font; pixelSize: seg.fontSize; weight: Font.DemiBold }
                        horizontalAlignment: Text.AlignHCenter
                        elide: Text.ElideRight
                    }
                    TapHandler {
                        id: optTap
                        gesturePolicy: TapHandler.ReleaseWithinBounds
                        longPressThreshold: 0.6
                        onTapped: seg.chosen(opt.modelData.id)
                        onLongPressed: qs.editing = true
                    }
                }
            }
        }
    }

    // A slider's words, once in the text color and once, over the fill, in
    // the lit text color.
    component SliderWords: Item {
        property var slider
        property color ink
        Icon {
            id: ic
            visible: parent.slider.icon !== ""
            x: 22 * qs.ws
            anchors.verticalCenter: parent.verticalCenter
            width: 30 * qs.ws
            height: width
            kind: parent.slider.icon
            color: parent.ink
            lineWidth: 4 * qs.ws
        }
        Text {
            anchors.left: parent.slider.icon !== "" ? ic.right : parent.left
            anchors.leftMargin: parent.slider.icon !== "" ? 14 * qs.ws : 24 * qs.ws
            anchors.verticalCenter: parent.verticalCenter
            text: parent.slider.label
            color: parent.ink
            font { family: qs.font; pixelSize: 23 * qs.ws; weight: Font.DemiBold }
        }
        Text {
            anchors.right: parent.right
            anchors.rightMargin: 24 * qs.ws
            anchors.verticalCenter: parent.verticalCenter
            text: parent.slider.valueText
            color: parent.ink
            opacity: 0.85
            font { family: qs.font; pixelSize: 22 * qs.ws }
        }
    }

    // A thick pill slider with its icon, name and value inside; the page
    // swipe never takes its touch (preventStealing). It sends a few times a
    // second while moving, and once on release.
    component Slider: Item {
        id: slider
        property string label
        property string icon
        property real value: 0
        property real from: 0
        property real to: 100
        property real step: 1
        property string valueText: Math.round(shownValue) + " %"
        signal moved(real value)
        property bool dragging: false
        property real dragValue: 0
        readonly property real shownValue: dragging ? dragValue : value
        readonly property real fraction: Math.max(0, Math.min(1, (shownValue - from) / Math.max(1, to - from)))
        property real sent: NaN
        Layout.fillWidth: true
        Layout.preferredHeight: qs.rowHeight * qs.placed.k

        function send() {
            if (dragValue !== sent) {
                sent = dragValue
                slider.moved(dragValue)
            }
        }
        Rectangle {
            id: track
            anchors.fill: parent
            radius: height / 2
            color: Skin.colors.raised
            clip: true
            SliderWords { anchors.fill: parent; slider: slider; ink: Skin.colors.text }
            Rectangle {
                width: Math.max(track.height, track.width * slider.fraction)
                height: parent.height
                radius: track.radius
                color: Skin.colors.lit
                clip: true
                SliderWords { width: track.width; height: track.height; slider: slider; ink: Skin.colors.litText }
            }
        }
        MouseArea {
            anchors.fill: parent
            preventStealing: true
            function place(mx) {
                const f = Math.max(0, Math.min(1, mx / track.width))
                const v = slider.from + f * (slider.to - slider.from)
                slider.dragValue = Math.round(v / slider.step) * slider.step
            }
            onPressed: function (e) { place(e.x); slider.dragging = true }
            onPositionChanged: function (e) { place(e.x) }
            onReleased: function (e) { place(e.x); slider.send(); slider.dragging = false }
            onCanceled: slider.dragging = false
        }
        Timer {
            interval: 150
            repeat: true
            running: slider.dragging
            onTriggered: slider.send()
        }
    }

    // A widget with a name over its controls, as a home screen tile.
    component Panel: Rectangle {
        id: panel
        property string title
        default property alias content: body.data
        radius: 40 * qs.ws
        color: qs.surface(Skin.colors.panel)
        border.color: Skin.colors.border
        border.width: 3 * qs.ws
        ColumnLayout {
            id: body
            anchors.verticalCenter: parent.verticalCenter
            x: qs.pad * qs.placed.k + 6 * qs.ws
            width: panel.width - 2 * x
            spacing: qs.spacing * qs.placed.k
            Title {
                visible: panel.title !== ""
                text: panel.title
            }
        }
    }

    // ---- The widgets ----
    Component {
        id: wifiWidget
        Toggle {
            label: "Wi-Fi"
            icon: "wifi"
            on: !!qs.quick.wifi && qs.quick.wifi.enabled
            state_: on ? "On" : "Off"
            onTapped: {
                const want = !on
                qs.quick = Object.assign({}, qs.quick, { wifi: { enabled: want } })
                qs.setQuick({ wifi: want })
            }
        }
    }
    Component {
        id: bluetoothWidget
        Toggle {
            label: "Bluetooth"
            icon: "bluetooth"
            on: !!qs.quick.bluetooth && qs.quick.bluetooth.enabled
            state_: on ? "On" : "Off"
            onTapped: {
                const want = !on
                qs.quick = Object.assign({}, qs.quick, { bluetooth: { enabled: want } })
                qs.setQuick({ bluetooth: want })
            }
        }
    }
    Component {
        id: screenshotWidget
        Toggle {
            label: "Screenshot"
            icon: "camera"
            state_: "Top screen"
            onTapped: {
                qs.captureNote = "Taking a screenshot…"
                qs.setQuick({ screenshot: true })
            }
        }
    }
    Component {
        id: recordWidget
        Toggle {
            label: qs.capture.recording ? "Stop" : "Record"
            icon: qs.capture.recording ? "square" : "record"
            on: !!qs.capture.recording
            state_: qs.capture.recording ? qs.clock(qs.recordingSeconds) : "Top screen"
            onTapped: {
                const start = !qs.capture.recording
                qs.captureNote = start ? "" : "Saving the recording…"
                if (start)
                    qs.recordingSince = Date.now()
                qs.setQuick({ record: start })
            }
        }
    }
    Component {
        id: brightnessWidget
        Panel {
            // Without a top slider (Steam's is the top screen's): the bottom's alone.
            Slider {
                visible: qs.topSlider
                icon: "sun"
                label: "Both screens"
                value: qs.both >= 0 ? qs.both : qs.screens ? Math.round((qs.screens.top + qs.screens.bottom) / 2) : 0
                onMoved: function (v) { qs.setBoth(v) }
            }
            Slider {
                visible: qs.topSlider
                icon: "sun"
                label: "Top screen"
                value: qs.screens ? qs.screens.top : 0
                onMoved: function (v) { qs.setBrightness("top", v) }
            }
            Slider {
                icon: "sun"
                label: "Bottom screen"
                value: qs.screens ? qs.screens.bottom : 0
                onMoved: function (v) { qs.setBrightness("bottom", v) }
            }
        }
    }
    Component {
        id: screenoffWidget
        Toggle {
            label: "Screen off"
            icon: "moon"
            state_: "Touch wakes it"
            onTapped: qs.setQuick({ screenOff: true })
        }
    }
    Component {
        id: volumeWidget
        Panel {
            Slider {
                icon: "volume"
                label: "Top screen" + (qs.quick.volume && qs.quick.volume.muted ? " (muted)" : "")
                value: qs.quick.volume ? qs.quick.volume.system : 0
                onMoved: function (v) { qs.setVolume("system", v) }
            }
            Slider {
                icon: "volume"
                label: "Bottom screen apps"
                value: qs.quick.volume ? qs.quick.volume.apps : 0
                onMoved: function (v) { qs.setVolume("apps", v) }
            }
        }
    }
    Component {
        id: lightsWidget
        Panel {
            title: "Lights"
            Item {
                id: spectrum
                Layout.fillWidth: true
                Layout.preferredHeight: 46 * qs.ws
                property bool dragging: false
                property real dragPos: 0
                property string sent: ""
                readonly property real pos: dragging ? dragPos : (qs.light ? qs.positionOf(qs.light.color) : 0)
                opacity: qs.light && qs.light.enabled ? 1 : 0.35

                function send() {
                    const hex = qs.colorAt(dragPos)
                    if (hex !== sent) {
                        sent = hex
                        qs.setLight({ enabled: true, color: hex })
                    }
                }
                Rectangle {
                    id: hues
                    anchors.verticalCenter: parent.verticalCenter
                    x: hueKnob.width / 2
                    width: parent.width - hueKnob.width
                    height: 22 * qs.ws
                    radius: height / 2
                    gradient: Gradient {
                        orientation: Gradient.Horizontal
                        GradientStop { position: 0; color: "white" }
                        GradientStop { position: qs.whiteEnd; color: "white" }
                        GradientStop { position: qs.whiteEnd + 0.001; color: "#ff0000" }
                        GradientStop { position: qs.whiteEnd + (1 - qs.whiteEnd) / 6; color: "#ffff00" }
                        GradientStop { position: qs.whiteEnd + (1 - qs.whiteEnd) * 2 / 6; color: "#00ff00" }
                        GradientStop { position: qs.whiteEnd + (1 - qs.whiteEnd) * 3 / 6; color: "#00ffff" }
                        GradientStop { position: qs.whiteEnd + (1 - qs.whiteEnd) * 4 / 6; color: "#0000ff" }
                        GradientStop { position: qs.whiteEnd + (1 - qs.whiteEnd) * 5 / 6; color: "#ff00ff" }
                        GradientStop { position: 1; color: "#ff0000" }
                    }
                }
                Rectangle {
                    id: hueKnob
                    width: 44 * qs.ws
                    height: width
                    radius: width / 2
                    anchors.verticalCenter: hues.verticalCenter
                    x: spectrum.pos * (parent.width - width)
                    color: "#" + qs.colorAt(spectrum.pos)
                    border.color: Skin.colors.text
                    border.width: 4 * qs.ws
                }
                MouseArea {
                    anchors.fill: parent
                    preventStealing: true
                    function place(mx) {
                        spectrum.dragPos = Math.max(0, Math.min(1, (mx - hueKnob.width / 2) / hues.width))
                    }
                    onPressed: function (e) { place(e.x); spectrum.dragging = true }
                    onPositionChanged: function (e) { place(e.x) }
                    onReleased: function (e) { place(e.x); spectrum.send(); spectrum.dragging = false }
                    onCanceled: spectrum.dragging = false
                }
                // The lights follow the finger, a few times a second.
                Timer {
                    interval: 150
                    repeat: true
                    running: spectrum.dragging
                    onTriggered: spectrum.send()
                }
            }
            Segmented {
                options: [{ id: "on", label: qs.light && qs.light.enabled ? "On" : "Off" },
                          { id: 25, label: "25 %" }, { id: 50, label: "50 %" }, { id: 100, label: "100 %" }]
                current: qs.light && qs.light.enabled ? qs.light.brightness : null
                onChosen: function (id) {
                    if (id === "on")
                        qs.setLight({ enabled: !qs.light.enabled })
                    else
                        qs.setLight({ enabled: true, brightness: id })
                }
            }
        }
    }
    Component {
        id: fanWidget
        Panel {
            title: "Fan"
            Segmented {
                options: (qs.controls.fan ? qs.controls.fan.profiles : []).map(p => ({ id: p, label: qs.fanNames[p] || p }))
                current: qs.controls.fan ? qs.controls.fan.profile : ""
                onChosen: function (id) {
                    qs.statsdRequest("POST", "/controls", { fanProfile: id }, function (c) { if (c) qs.controls = c })
                }
            }
        }
    }
    Component {
        id: standbyWidget
        Panel {
            title: "Back to the game art"
            Segmented {
                options: qs.standbyChoices.map(v => ({ id: v, label: v === 0 ? "Never" : v < 60 ? v + " s" : v / 60 + " min" }))
                current: qs.standbySeconds
                onChosen: function (id) {
                    qs.statsdRequest("POST", "/settings", { standbySeconds: id }, function (v) {
                        if (v && typeof v.standbySeconds === "number")
                            qs.standbyChosen(v.standbySeconds)
                    })
                }
            }
        }
    }
    Component {
        id: batteryWidget
        Panel {
            title: "Charging"
            Segmented {
                options: qs.chargeModes.filter(m => !m.feature || (qs.charge && qs.charge.features && qs.charge.features[m.feature]))
                                      .map(m => ({ id: m.id, label: m.label }))
                current: qs.charge ? qs.charge.settings.mode : ""
                onChosen: function (id) { qs.setCharge({ mode: id }) }
            }
            Slider {
                visible: !!qs.charge && qs.charge.settings.mode === "hold"
                label: "Hold at"
                from: qs.charge && qs.charge.limits ? qs.charge.limits.hold_at[0] : 50
                to: qs.charge && qs.charge.limits ? qs.charge.limits.hold_at[1] : 90
                step: 5
                value: qs.charge ? qs.charge.settings.hold_at : 80
                onMoved: function (v) { qs.setCharge({ hold_at: v }) }
            }
            Segmented {
                visible: !!qs.charge && !!qs.charge.features && !!qs.charge.features.slow
                options: [{ id: false, label: "Full speed" }, { id: true, label: "Slow charging" }]
                current: !!qs.charge && !!qs.charge.settings.slow
                onChosen: function (id) { qs.setCharge({ slow: id }) }
            }
            Note {
                text: {
                    const live = qs.charge && qs.charge.state ? qs.charge.state : {}
                    const mode = qs.chargeModes.find(m => qs.charge && m.id === qs.charge.settings.mode)
                    const status = !live.plugged ? "Not on a charger."
                        : live.held ? "On the charger, holding at " + live.capacity + " %."
                        : "On the charger, " + String(live.status || "charging").toLowerCase() + " at " + live.capacity + " %."
                    return (mode ? mode.desc + " " : "") + status
                }
            }
        }
    }
    Component {
        id: gameWidget
        Panel {
            title: "Game profile"
            Note {
                text: qs.game === "" ? "No game yet: start one, or play one first."
                    : SteamGame.profileName + (SteamGame.running ? " (playing)" : " (played last)") + ". Applies the next time it starts."
            }
            Segmented {
                visible: qs.game !== ""
                options: {
                    const presets = qs.profiles && qs.profiles.presets && qs.profiles.presets.presets ? qs.profiles.presets.presets : {}
                    const out = []
                    for (const id in presets)
                        out.push({ id: id, label: presets[id].label || id })
                    if (!out.some(p => p.id === "default"))
                        out.unshift({ id: "default", label: "Default" })
                    // Which row is which: "x86" on its default.
                    return out.map(p => p.id === "default" ? { id: p.id, label: "x86: " + p.label } : p)
                }
                current: qs.profile.fex || "default"
                onChosen: function (id) {
                    qs.setProfile(id === "default" ? { fex: undefined, fexConfig: undefined } : { fex: id, fexConfig: undefined })
                }
            }
            Segmented {
                visible: qs.game !== ""
                options: [{ id: "", label: "Cores: Default" }].concat(
                    qs.coreChoices.filter(c => qs.profiles && qs.profiles.cores && qs.profiles.cores[c.id]))
                current: qs.profile.cores || ""
                onChosen: function (id) { qs.setProfile({ cores: id === "" ? undefined : id }) }
            }
        }
    }
    // A widget pack's: its QML in a tile (README.md in the widgets folder).
    // It gets s (its scale) and shown (the page is on screen) if it has them.
    Component {
        id: packWidget
        Rectangle {
            id: frame
            property string widgetId
            readonly property var pack: qs.packs[widgetId]
            radius: 40 * qs.ws
            color: qs.surface(Skin.colors.panel)
            border.color: Skin.colors.border
            border.width: 3 * qs.ws
            Loader {
                id: inner
                anchors.fill: parent
                anchors.margins: qs.pad * qs.placed.k + 6 * qs.ws
                source: frame.pack ? frame.pack.url : ""
                onLoaded: {
                    if (item.s !== undefined)
                        item.s = Qt.binding(() => qs.ws)
                    if (item.shown !== undefined)
                        item.shown = Qt.binding(() => qs.shown && !qs.editing)
                }
            }
            Text {
                anchors.centerIn: parent
                width: parent.width - 40 * qs.ws
                visible: inner.status === Loader.Error
                text: (frame.pack ? frame.pack.title : "This widget") + " didn't load."
                color: Skin.colors.text
                opacity: 0.7
                wrapMode: Text.Wrap
                horizontalAlignment: Text.AlignHCenter
                font { family: qs.font; pixelSize: 20 * qs.ws }
            }
        }
    }
    readonly property var widgets: ({
        wifi: wifiWidget, bluetooth: bluetoothWidget, screenshot: screenshotWidget, record: recordWidget,
        brightness: brightnessWidget, volume: volumeWidget, lights: lightsWidget, fan: fanWidget,
        standby: standbyWidget, battery: batteryWidget, game: gameWidget, screenoff: screenoffWidget,
    })

    // ---- Customizing: like a phone's home screen ----
    // A long press anywhere on a widget (not on a slider, which keeps its
    // touch) and they all wiggle: ✕ takes one off the page, − + make it
    // narrower or wider, and one held and dragged moves, the others making
    // room as it passes over them. The page swipe is off meanwhile
    // (Shell.qml). Done (or leaving the page) ends it.
    property string dragId: ""
    property string overId: ""   // the widget last swapped with, until the finger leaves it
    function dragOver(id, cx, cy) {
        const over = placed.list.find(p => p.id !== id && cx >= p.x && cx < p.x + p.w && cy >= p.y && cy < p.y + p.h)
        if (!over) {
            overId = ""
            return
        }
        if (over.id === overId)
            return
        overId = over.id
        // Into its place: the ones between shift one along.
        const list = layout.slice()
        const i = indexOf(id), j = indexOf(over.id)
        if (i < 0 || j < 0)
            return
        const moved = list.splice(i, 1)[0]
        list.splice(j, 0, moved)
        layout = list
    }
    function dropped() {
        dragId = ""
        overId = ""
        saveLayout(layout)
    }
    // The wiggle, shared: each widget tilts by it, every other one the
    // other way, the wide ones less.
    property real wiggle: 0
    SequentialAnimation on wiggle {
        running: qs.editing
        loops: Animation.Infinite
        NumberAnimation { to: 1; duration: 120; easing.type: Easing.InOutSine }
        NumberAnimation { to: -1; duration: 240; easing.type: Easing.InOutSine }
        NumberAnimation { to: 0; duration: 120; easing.type: Easing.InOutSine }
        onStopped: qs.wiggle = 0
    }

    // ---- The page ----
    Repeater {
        model: qs.shownIds.split(" ").filter(id => id !== "")
        delegate: Item {
            id: cell
            required property string modelData
            required property int index
            readonly property var where: qs.place(modelData)
            readonly property bool held: drag.active
            property real heldX: 0
            property real heldY: 0
            x: held ? heldX : where.x
            y: held ? heldY : where.y
            width: where.w
            height: where.h
            z: held ? 10 : 0
            Behavior on x { enabled: qs.editing && !cell.held; NumberAnimation { duration: 220; easing.type: Easing.OutCubic } }
            Behavior on y { enabled: qs.editing && !cell.held; NumberAnimation { duration: 220; easing.type: Easing.OutCubic } }
            Behavior on width { enabled: qs.editing; NumberAnimation { duration: 220; easing.type: Easing.OutCubic } }

            Item {
                id: body
                anchors.fill: parent
                scale: cell.held ? 1.04 : 1
                opacity: cell.held ? 0.92 : 1
                rotation: qs.editing && !cell.held ? qs.wiggle * 0.9 * Math.min(1, 320 * qs.ws / Math.max(1, cell.width))
                                                     * (cell.index % 2 ? 1 : -1) : 0
                Behavior on scale { NumberAnimation { duration: 140 } }
                Loader {
                    anchors.fill: parent
                    sourceComponent: qs.widgets[cell.modelData] || packWidget
                    enabled: !qs.editing
                    // Editing: a little faded, under its ✕ and − +.
                    opacity: qs.editing ? 0.6 : 1
                    Behavior on opacity { NumberAnimation { duration: 160 } }
                    onLoaded: if (item.widgetId !== undefined) item.widgetId = cell.modelData
                }
                // ✕: off the page (it waits in the Add bar, and in the PB-OS
                // tab's Widgets page).
                Rectangle {
                    visible: qs.editing && !cell.held
                    x: -14 * qs.ws
                    y: -14 * qs.ws
                    width: 56 * qs.ws
                    height: width
                    radius: width / 2
                    color: xTap.pressed ? Skin.colors.pressed : Skin.colors.raised
                    border.color: Skin.colors.border
                    border.width: 2 * qs.ws
                    Icon {
                        anchors.centerIn: parent
                        width: 24 * qs.ws
                        height: width
                        kind: "close"
                        color: Skin.colors.text
                        lineWidth: 4 * qs.ws
                    }
                    TapHandler {
                        id: xTap
                        gesturePolicy: TapHandler.ReleaseWithinBounds
                        onTapped: qs.hide(cell.modelData)
                    }
                }
                // − +: its width, off the corner opposite the ✕.
                Rectangle {
                    visible: qs.editing && !cell.held
                    x: parent.width - width + 14 * qs.ws
                    y: parent.height - height + 14 * qs.ws
                    width: sizeRow.width + 12 * qs.ws
                    height: 56 * qs.ws
                    radius: height / 2
                    color: Skin.colors.raised
                    border.color: Skin.colors.border
                    border.width: 2 * qs.ws
                    Row {
                        id: sizeRow
                        anchors.centerIn: parent
                        Repeater {
                            model: [
                                { by: -1, label: "−", ok: cell.where.span > qs.catalog[cell.modelData].min },
                                { by: 1, label: "+", ok: cell.where.span < 4 },
                            ]
                            delegate: Item {
                                id: sz
                                required property var modelData
                                width: 52 * qs.ws
                                height: 52 * qs.ws
                                opacity: modelData.ok ? 1 : 0.3
                                Rectangle {
                                    anchors.fill: parent
                                    radius: width / 2
                                    color: Skin.colors.pressed
                                    visible: szTap.pressed
                                }
                                Text {
                                    anchors.centerIn: parent
                                    text: sz.modelData.label
                                    color: Skin.colors.text
                                    font { family: qs.font; pixelSize: 32 * qs.ws; weight: Font.Bold }
                                }
                                TapHandler {
                                    id: szTap
                                    enabled: sz.modelData.ok
                                    gesturePolicy: TapHandler.ReleaseWithinBounds
                                    onTapped: qs.resize(cell.modelData, sz.modelData.by)
                                }
                            }
                        }
                    }
                }
            }

            // Not editing: a long press starts it. A button keeps its touch
            // from this (Toggle and Segmented start it themselves); a slider
            // keeps it for good.
            TapHandler {
                enabled: !qs.editing
                longPressThreshold: 0.6
                onLongPressed: qs.editing = true
            }
            // Editing: hold and drag it somewhere else.
            DragHandler {
                id: drag
                enabled: qs.editing
                target: null
                property real startX: 0
                property real startY: 0
                onActiveChanged: {
                    if (active) {
                        startX = cell.where.x
                        startY = cell.where.y
                        cell.heldX = startX
                        cell.heldY = startY
                        qs.dragId = cell.modelData
                    } else {
                        qs.dropped()
                    }
                }
                onTranslationChanged: {
                    if (!active)
                        return
                    cell.heldX = startX + translation.x
                    cell.heldY = startY + translation.y
                    qs.dragOver(cell.modelData, cell.heldX + cell.width / 2, cell.heldY + cell.height / 2)
                }
            }
        }
    }

    // A note from a screenshot or recording, over the page's foot.
    Rectangle {
        visible: qs.captureNote !== "" && !qs.editing
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 6 * qs.s
        width: noteText.implicitWidth + 40 * qs.s
        height: noteText.implicitHeight + 14 * qs.s
        radius: height / 2
        color: Skin.colors.raised
        Text {
            id: noteText
            anchors.centerIn: parent
            text: qs.captureNote
            color: Skin.colors.text
            font { family: qs.font; pixelSize: 19 * qs.s }
        }
        TapHandler { onTapped: qs.captureNote = "" }
    }
    Timer {
        interval: 6000
        running: qs.captureNote !== "" && !qs.capture.recording && !qs.captureNote.endsWith("…")
        onTriggered: qs.captureNote = ""
    }

    // The edit bar: the hidden widgets to add, Reset and Done.
    Rectangle {
        visible: qs.editing
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: qs.margin
        height: qs.barHeight
        radius: 40 * qs.s
        color: Skin.colors.panel
        border.color: Skin.colors.accent
        border.width: 3 * qs.s
        RowLayout {
            anchors.fill: parent
            anchors.margins: 16 * qs.s
            spacing: 10 * qs.s
            Text {
                text: qs.hidden.length ? "Drag to move · Add:" : "Drag to move · all widgets shown"
                color: Skin.colors.text
                opacity: 0.75
                font { family: qs.font; pixelSize: 22 * qs.s; weight: Font.DemiBold }
            }
            // Scrolled by Barry's touch rules (Barry.Input's TouchScroll).
            TouchScroll {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Flickable {
                    anchors.fill: parent
                    contentWidth: addRow.width
                    clip: true
                    flickableDirection: Flickable.HorizontalFlick
                    Row {
                        id: addRow
                        height: parent.height
                        spacing: 8 * qs.s
                        Repeater {
                            model: qs.hidden
                            delegate: Rectangle {
                                id: chip
                                required property string modelData
                                height: parent.height
                                width: chipText.implicitWidth + 36 * qs.s
                                radius: height / 2
                                color: chipTap.pressed ? Skin.colors.pressed : Skin.colors.raised
                                Text {
                                    id: chipText
                                    anchors.centerIn: parent
                                    text: "+ " + qs.catalog[chip.modelData].title
                                    color: Skin.colors.text
                                    font { family: qs.font; pixelSize: 21 * qs.s; weight: Font.DemiBold }
                                }
                                TapHandler {
                                    id: chipTap
                                    gesturePolicy: TapHandler.ReleaseWithinBounds
                                    onTapped: qs.add(chip.modelData)
                                }
                            }
                        }
                    }
                }
            }
            Repeater {
                model: [{ act: "reset", label: "Reset" }, { act: "done", label: "Done" }]
                delegate: Rectangle {
                    id: barBtn
                    required property var modelData
                    Layout.fillHeight: true
                    Layout.preferredWidth: barText.implicitWidth + 44 * qs.s
                    radius: height / 2
                    color: barTap.pressed ? Skin.colors.pressed : modelData.act === "done" ? Skin.colors.lit : Skin.colors.raised
                    Text {
                        id: barText
                        anchors.centerIn: parent
                        text: barBtn.modelData.label
                        color: barBtn.modelData.act === "done" ? Skin.colors.litText : Skin.colors.text
                        font { family: qs.font; pixelSize: 22 * qs.s; weight: Font.DemiBold }
                    }
                    TapHandler {
                        id: barTap
                        gesturePolicy: TapHandler.ReleaseWithinBounds
                        onTapped: {
                            if (barBtn.modelData.act === "done")
                                qs.editing = false
                            else
                                qs.saveLayout(qs.defaultLayout)
                        }
                    }
                }
            }
        }
    }
}
