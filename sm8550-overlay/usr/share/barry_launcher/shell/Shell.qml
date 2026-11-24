// Barry Launcher on the AYN Thor's bottom screen during Game Mode: the first
// window of barry_launcher_session, under everything else. Because this
// window is always shown, gamescope falls back to it when an app quits,
// instead of freezing on the app's last frame.
//
// Its base is the hero art of the game selected on the top screen
// (Hero.qml). Over it Barry opens and closes: a short press of the AYN
// button, or a swipe up from the bottom edge (barry_launcher_shelld says so
// in GET /home: "open", and "serial" counting each opening). Barry is three
// pages side by side: the performance dashboard, the apps (Home.qml) and
// the quick settings (QuickSettings.qml), left to right; it opens on the
// apps. A left-to-right swipe from anywhere brings in the page to the left,
// right to left the one to the right; Barry.Input's PageSwipe tells a swipe
// from a tap and says where it goes, and here it slides there, springing
// past a little and back, as the keyboard does coming up. An app's tile
// starts it (or brings it forward) through barry_launcher_shelld, and Barry
// closes; the AYN button opens Barry over it and closes to the game art, the
// app staying in the background (its tile brings it back).
//
// Over an app, a swipe in from the left edge pulls in the dashboard, from
// the right edge the quick settings, like a phone's side panels, following
// the finger (Barry.Input's EdgeSwipe reads the touchscreen while the app has
// the touches); let go far enough or fast enough it stays, else it goes back.
// Swiped back toward the apps, Barry closes back to the app. Only one window
// shows on the bottom screen, so shelld takes a picture of the app as the
// finger comes down (POST /side-prepare; GET /home's under) and the page
// slides over that, and back off it, instead of the app giving way to the
// game art.
//
// The game art is standby. A tap on it wakes Barry too: its pages come over
// the art, which stays behind as if behind frosted glass (Hero.qml's
// frost), its logo gone. Untouched for standbySeconds (Barry's settings,
// 15 by default; 0: never), Barry goes back to the art, an app it opened
// over staying in the background, unless it is on a side page (the
// dashboard and the quick settings stay until swiped back to the apps).
//
// The game art can be turned off (Barry's settings, heroArt, the PB-OS tab's
// Dual Screen page): Barry's pages then stay up on their own background, as
// before there was art, and the AYN button only goes to and from apps. In
// Desktop Mode ("desktop" argument, barry_launcher_desktop) it is always
// off: the art is Game Mode's, for the game selected in Steam.
//
// On a device without a Barry button (Device.barryButton "", the Retroid
// Pocket Duo) the swipe up from the bottom edge is Barry.Input's (EdgeSwipe,
// here; shelld watches it on the AYN Thor): over an app it opens Barry, and
// while Barry is open over the game art it closes it to the art, as the AYN
// button would.
//
// New versions of Project Barry's apps (shelld's updates topic, the first
// start after a PB-OS update found some) show as a card over Barry's pages
// (AppUpdates.qml), whenever Barry shows, until the user answers it.
//
// Opening, the apps fly in toward the screen, the middle ones first
// (Home.qml's dropIn, Barry.Input's TileDrop; a skin's home screen without
// it slides up instead): shelld counts the openings, this puts Barry at
// the start while still hidden and says so (/home-ready), and then the
// window is brought forward. Closing, it fades down and the hero shows
// again.
import QtQuick
import QtQuick.Window
import Barry.Input
import "../dashboard"

Window {
    id: win
    title: "Barry Launcher"
    color: Skin.colors.background
    visibility: Window.FullScreen
    visible: true

    readonly property string api: "http://127.0.0.1:47824"
    readonly property real s: Math.min(width / 1240, height / 1080)

    function post(path, body) {
        const x = new XMLHttpRequest()
        x.onreadystatechange = function () {
            if (x.readyState === XMLHttpRequest.DONE)
                win.refresh()
        }
        x.open("POST", api + path)
        x.setRequestHeader("Content-Type", "application/json")
        x.send(JSON.stringify(body))
    }

    // After a change made here: with an older shelld (no event socket),
    // ask for the apps now; else shelld pushes the change itself.
    function refresh() {
        ShellState.refresh("apps")
    }

    // shelld's apps (GET /apps; ShellState pushes them as they change).
    function takeApps(apps) {
        if (!apps || typeof apps !== "object")
            return
        try {
            const r = {}, st = {}
            for (const name in apps) {
                r[name] = apps[name].running
                st[name] = apps[name].status || ""
            }
            win.running = r
            win.status = st
            // Only when it changed: a new model rebuilds the tiles.
            const tiles = Object.keys(apps)
                .filter(name => apps[name].name !== undefined && !apps[name].hidden)
                .sort((a, b) => apps[a].pos - apps[b].pos)
                .map(name => ({ app: name, name: apps[name].name, icon: apps[name].icon,
                                closable: apps[name].closable !== false }))
            if (tiles.length || Object.keys(apps).some(name => apps[name].hidden))
                if (JSON.stringify(tiles) !== JSON.stringify(win.tiles))
                    win.tiles = tiles
        } catch (e) {}  // an entry not as expected: as before, left as it was
    }

    // What Home shows (shelld's /apps); null: Home's own until it answers.
    property var running: ({})
    property var status: ({})
    property var tiles: null

    // What shelld says (GET /home).
    property bool barryOpen: false
    property bool barryOver: false  // open over an app it closes back to
    property int openPage: 0        // the page it opened on (a side swipe over an app)
    property string underShot: ""   // the app as it showed, under a side panel (file URL)
    property bool underCover: false // that picture over everything, Barry closed (see underClear)
    property bool appFront: false   // an app in front, Barry closed: side swipes are on (GET /home)
    property bool anyFront: false   // an app or a tool in front, Barry closed: the swipe up is on
    property bool following: false  // the side page follows the finger (EdgeSwipe)
    property string edgeResult: ""  // let go before Barry showed: "open" or "cancel"
    // Opened on a side page from an app: swiped back, it closes to the app.
    readonly property bool sidePanel: barryOpen && barryOver && openPage !== 0
    // The art behind Barry: 0 in focus, 1 frosted (animated as Barry comes
    // and goes).
    property real frost: 0
    // Barry opening as a tool (the Trackpad, the Deck controls) slides away
    // (shelld's tools topic): it comes in from black over the blurred art,
    // never over the sharp art for a moment.
    property bool fromTool: false
    // Barry's settings (settings.json, read again as it changes).
    readonly property int standbySeconds: typeof Skin.settings.standbySeconds === "number" ? Skin.settings.standbySeconds : 15
    readonly property bool desktop: Qt.application.arguments.indexOf("desktop") >= 0
    readonly property bool heroOn: !desktop && Skin.settings.heroArt !== false
    // Barry's pages on screen: open, or always without the art.
    readonly property bool barryShown: barryOpen || !heroOn
    onHeroOnChanged: {
        // From heroOn itself: barryShown's binding may not have caught up yet.
        slide.stop()
        slideOut.stop()
        barryShift.y = 0
        barry.opacity = barryOpen || !heroOn ? 1 : 0
        frost = heroOn && barryOpen ? 1 : 0
    }

    function setBarry(open, why) {
        win.post("/barry", { open: open, why: why })
    }

    // A side page pulled in over an app (see the top).
    EdgeSwipe {
        id: edgeSwipe
        armed: win.appFront && !win.barryOpen
        edge: 45
        threshold: 24
        onEdgeTouched: win.post("/side-prepare", {})
        onStarted: function (side) {
            win.edgeResult = ""
            win.post("/barry", { open: true, page: side,
                                 why: side > 0 ? "swipe in from the left edge" : "swipe in from the right edge" })
        }
        onReleased: function (side, progress, velocity) {
            // Far enough, or flicked inward: it stays.
            const stays = progress > 0.3 || velocity > 500
            if (win.following) {
                win.following = false
                pages.goTo(stays ? side : 0)
            } else {
                win.edgeResult = stays ? "open" : "cancel"
            }
        }
        onCancelled: {
            if (win.following) {
                win.following = false
                pages.goTo(0)
            } else {
                win.edgeResult = "cancel"
            }
        }
        // No Barry button: the swipe up from the bottom edge (see the top).
        bottomArmed: Device.barryButton === "" && !win.desktop
                     && ((win.anyFront && !win.barryOpen) || (win.barryOpen && win.heroOn && !win.sidePanel))
        onSwipedUp: function (x, y) {
            if (win.barryOpen)
                win.post("/barry", { open: false, back: false, why: "swipe up from the bottom edge" })
            else if (win.anyFront)
                win.post("/barry", { open: true, why: "swipe up from the bottom edge", lifted: [Math.round(x), Math.round(y)] })
        }
    }
    Binding {
        target: pages
        property: "progress"
        value: edgeSwipe.side * edgeSwipe.progress
        when: win.following && edgeSwipe.active
        restoreMode: Binding.RestoreNone
    }

    // The hero art: what shows while Barry is closed.
    SkinView {
        id: hero
        file: "Hero.qml"
        anchors.fill: parent
        visible: win.heroOn
    }
    Binding { target: hero.item; property: "s"; value: win.s }
    // Not under an app in front: the art's pan and bob would keep drawing
    // frames nobody sees.
    Binding { target: hero.item; property: "shown"; value: !win.appFront && (!win.barryOpen || barry.opacity < 1) }
    Binding { target: hero.item; property: "frost"; value: win.frost }
    Rectangle {
        id: blackout
        anchors.fill: parent
        color: "black"
        opacity: 0
        visible: opacity > 0
    }
    // Barry did not open after all: no black left over the art.
    Timer {
        id: blackoutGuard
        interval: 1500
        onTriggered: if (win.fromTool || !win.barryOpen) {
            win.fromTool = false
            unblack.start()
            if (!win.barryOpen)
                win.frost = 0
        }
    }
    SequentialAnimation {
        id: unblack
        PauseAnimation { duration: 120 }
        NumberAnimation { target: blackout; property: "opacity"; to: 0; duration: 280; easing.type: Easing.OutQuad }
    }
    // Standby: a tap on the art wakes Barry.
    Item {
        anchors.fill: hero
        TapHandler {
            enabled: win.heroOn && !win.barryOpen
            onTapped: win.setBarry(true, "tap on the game art")
        }
    }
    // The pages go see-through over the art (with no game, they stay solid).
    readonly property real glass: win.heroOn && SteamGame.available ? 0.2 : 1

    // Barry: its three pages over the hero.
    Item {
        id: barry
        anchors.fill: parent
        opacity: 0
        visible: opacity > 0
        transform: Translate { id: barryShift }

        // A side panel's app, as it showed: the page slides over it.
        Image {
            id: under
            anchors.fill: parent
            source: win.underShot
            visible: win.underShot !== ""
            asynchronous: false
            cache: false
            fillMode: Image.Stretch
            Rectangle {
                anchors.fill: parent
                color: "black"
                opacity: 0.3 * Math.min(1, Math.abs(pages.progress))
            }
        }

        PageSwipe {
            id: pages
            anchors.fill: parent
            // Desktop Mode: the apps only (the dashboard and quick settings
            // are Game Mode's: their controls do nothing under KWin).
            minPage: win.desktop ? 0 : -1
            maxPage: win.desktop ? 0 : 1
            // Not while the quick settings are being arranged (widgets drag).
            swipeEnabled: win.barryShown && !win.following && !(quick.item && quick.item.editing)
            onDragStarted: settleAnim.stop()
            onSettle: function (target, velocity, duration) {
                settleAnim.stop()
                // As the keyboard: toward a side page, a quick move with a
                // visible overshoot; back to the apps, a softer settle.
                const opening = target !== 0
                settleAnim.to = target
                // A thrown page goes at the finger's pace (faster thrown, sooner there).
                const usual = opening ? 460 : 380
                settleAnim.duration = duration > 0 ? Math.min(usual, duration) : usual
                // A side panel going back to its app: no spring past the
                // middle (the other side page would peek in over the app).
                settleAnim.easing.overshoot = win.sidePanel && !opening ? 0 : opening ? 1.4 : 0.8
                settleAnim.start()
            }

            // Home.qml, or the skin's: the apps, in the middle.
            Item {
                width: pages.width
                height: pages.height
                x: pages.progress * pages.width
                SkinView {
                    id: home
                    file: "Home.qml"
                    anchors.fill: parent
                    // A side panel over an app slides back to the app, not the apps.
                    visible: !win.sidePanel
                    onLoaded: if (win.tiles !== null) item.tiles = win.tiles
                }
            }

            Dashboard {
                id: dashboard
                width: pages.width
                height: pages.height
                x: (pages.progress - 1) * pages.width
                shown: win.barryShown && (pages.progress > 0.001 || (pages.dragging && pages.page >= 0))
                visible: pages.progress > 0.001
                onHideRequested: pages.goTo(0)
            }

            Item {
                width: pages.width
                height: pages.height
                x: (pages.progress + 1) * pages.width
                visible: pages.progress < -0.001
                SkinView {
                    id: quick
                    file: "QuickSettings.qml"
                    anchors.fill: parent
                }
            }
        }
    }
    Binding { target: quick.item; property: "s"; value: win.s }
    Binding { target: quick.item; property: "glass"; value: win.glass }
    Binding { target: quick.item; property: "standbySeconds"; value: win.standbySeconds }
    Binding { target: home.item; property: "glass"; value: win.glass }
    Binding { target: quick.item; property: "shown"; value: win.barryShown && pages.progress < -0.001 }

    NumberAnimation {
        id: settleAnim
        target: pages
        property: "progress"
        easing.type: Easing.OutBack
        // A side panel swiped (or sent) back to the middle: Barry closes
        // to the app once the page is off it.
        onFinished: if (win.sidePanel && pages.page === 0) win.setBarry(false, "side panel swiped away")
    }
    Binding { target: home.item; property: "s"; value: win.s }
    Binding { target: home.item; property: "running"; value: win.running }
    Binding { target: home.item; property: "status"; value: win.status }
    // Only once shelld has answered: until then Home keeps its own list (a
    // Binding with "when" still writes null into a view loaded later).
    onTilesChanged: if (tiles !== null && home.item) home.item.tiles = tiles
    Connections {
        target: home.item
        ignoreUnknownSignals: true
        function onLaunch(app) { win.post("/launch", { app: app }) }
        function onClose(app) { win.post("/close", { app: app }) }
    }

    property int homeSerial: -1  // the last opening seen; -1: none yet

    // Opening on the apps, no tile takes a tap while they fly in; opened by
    // a swipe up from the bottom edge, none for a while where that thumb
    // lifted (lifted: [x, y] from shelld), for it comes back down there,
    // onto Trackpad, Keyboard or Deck (Barry.Input's PageSwipe).
    function holdTaps(page, lifted) {
        if (page !== 0)
            return
        pages.holdTaps(600)
        if (Array.isArray(lifted) && lifted.length === 2)
            pages.holdTapsBelow(lifted[0], lifted[1], 200 * win.s, 1500)
    }

    function slideIn(serial, page, under) {
        const fromTool = win.fromTool || blackout.opacity > 0
        win.fromTool = false
        frostOff.stop()
        slideOut.stop()
        slide.stop()
        sideIn.stop()
        win.underCover = false
        if (page !== 0) {
            // A side panel over an app: the app's picture at once (or, had
            // none come, the art), and the page slides in from its edge over
            // it once the window shows.
            win.underShot = under ? "file://" + under + "?" + serial : ""
            settleAnim.stop()
            pages.goTo(page)
            settleAnim.stop()
            pages.progress = 0
            barryShift.y = 0
            barry.opacity = 1
            win.frost = 0
            unblack.stop()
            blackout.opacity = 0  // no picture came: the art, as before
            const r = new XMLHttpRequest()
            r.onreadystatechange = function () {
                if (r.readyState !== XMLHttpRequest.DONE)
                    return
                if (edgeSwipe.active) {
                    win.following = true  // the finger is still on it
                } else if (win.edgeResult === "cancel") {
                    pages.goTo(0)         // let go short: back to the app
                } else {
                    sideIn.to = page
                    sideIn.start()
                }
            }
            r.open("POST", api + "/home-ready")
            r.setRequestHeader("Content-Type", "application/json")
            r.send(JSON.stringify({ serial: serial }))
            return
        }
        win.underShot = ""
        unblack.stop()
        if (fromTool) {
            blackoutGuard.stop()
            blackout.opacity = 1
            win.frost = 1
        }
        // Barry opens on the apps, or the side page a swipe in asked for.
        settleAnim.stop()
        pages.goTo(page)
        settleAnim.stop()
        pages.progress = page
        // A home screen with dropIn() (the built-in one, and skins that
        // use Barry.Input's TileDrop): its tiles fly in over a quick fade.
        // Else Barry slides up into place; a side page just fades in.
        const drops = page === 0 && !!home.item && typeof home.item.dropIn === "function"
        barryShift.y = drops || page !== 0 ? 0 : 0.18 * height
        barry.opacity = 0
        const x = new XMLHttpRequest()
        x.onreadystatechange = function () {
            if (x.readyState !== XMLHttpRequest.DONE)
                return
            slide.start()
            if (fromTool)
                unblack.start()
            if (drops)
                home.item.dropIn()
        }
        x.open("POST", api + "/home-ready")
        x.setRequestHeader("Content-Type", "application/json")
        x.send(JSON.stringify({ serial: serial }))
    }

    // A side panel coming in over its app's picture.
    NumberAnimation {
        id: sideIn
        target: pages
        property: "progress"
        duration: 340
        easing.type: Easing.OutCubic
    }
    // Back at the app (shelld shows it again as Barry closes), Barry's
    // window keeps the app's picture over everything (underCover; it takes
    // no touches): gamescope shows a window's last frame for a moment as it
    // comes forward, and the next side swipe then shows the app (the game
    // art flashed there on the test Thor). Hiding the apps page instead
    // kept PageSwipe from seeing swipes. The picture goes, and Barry looks
    // closed again (as slideOut leaves it), as Barry opens or once no app is
    // in front (it quit: Barry's window shows on its own).
    Image {
        anchors.fill: parent
        z: 50
        source: win.underShot
        visible: win.underCover && win.underShot !== ""
        asynchronous: false
        cache: false
        fillMode: Image.Stretch
    }
    // New versions of Project Barry's apps (see the top): over the pages,
    // while Barry shows; untouched, Barry stays up meanwhile (standbyWanted).
    SkinView {
        id: appUpdates
        file: "AppUpdates.qml"
        anchors.fill: parent
        z: 60
        visible: win.barryShown && !win.sidePanel
    }
    readonly property bool updatesShown: appUpdates.item !== null && appUpdates.item.visible && appUpdates.visible
    Binding { target: appUpdates.item; property: "s"; value: win.s }
    Binding { target: appUpdates.item; property: "updates"; value: ShellState.updates }
    Connections {
        target: appUpdates.item
        ignoreUnknownSignals: true
        function onAnswer(what) { win.post("/updates", { do: what }) }
    }
    function underClear() {
        win.underCover = false
        win.underShot = ""
        if (win.heroOn) {
            barry.opacity = 0
            win.frost = 0
        }
    }
    // Only while the picture covers a closed Barry: as a side page opens,
    // /home stops saying "app" before it says "open" (the side page showed
    // the art on the test Thor).
    onAppFrontChanged: {
        if (!appFront && underCover)
            underClear()
        if (appFront && !barryOpen && heroOn && !underCover)
            toBlack()
        else if (!appFront && !barryOpen && blackout.opacity > 0) {
            // The app quit with Barry closed: back to the art.
            unblack.start()
            frostOff.start()
        }
    }
    function toBlack() {
        slide.stop()
        slideOut.stop()
        unblack.stop()
        frostOff.stop()
        barry.opacity = 0
        blackout.opacity = 1
        win.frost = 1
    }
    NumberAnimation { id: frostOff; target: win; property: "frost"; to: 0; duration: 520; easing.type: Easing.InOutCubic }

    ParallelAnimation {
        id: slide
        NumberAnimation { target: barryShift; property: "y"; to: 0; duration: 300; easing.type: Easing.OutCubic }
        NumberAnimation { target: barry; property: "opacity"; to: 1; duration: 220; easing.type: Easing.OutQuad }
        NumberAnimation { target: win; property: "frost"; to: 1; duration: 380; easing.type: Easing.OutCubic }
    }
    // Back to the art: the apps fade out, the art comes back into focus and
    // the logo fades back in.
    ParallelAnimation {
        id: slideOut
        NumberAnimation { target: barry; property: "opacity"; to: 0; duration: 320; easing.type: Easing.InOutQuad }
        NumberAnimation { target: win; property: "frost"; to: 0; duration: 520; easing.type: Easing.InOutCubic }
    }

    // Any touch counts as use (a passive look: the touch goes on to what
    // is under it).
    Item {
        anchors.fill: parent
        z: 100
        PointHandler {
            onActiveChanged: if (active) win.standbyAgain()
        }
    }
    // Back to the art after standbySeconds untouched: only on the apps page;
    // over an app too (shelld closes to the art, the app in the background:
    // GET /home's "app" is false, and slideOut plays). Started and stopped
    // here, never bound: restart() on a Timer whose running is bound breaks
    // the binding, and a touch or a page change then ran it on the side
    // pages too (they closed after 15 s).
    readonly property bool standbyWanted: heroOn && barryOpen && standbySeconds > 0
                                          && pages.page === 0 && !pages.dragging && !updatesShown
    function standbyAgain() {
        if (standbyWanted)
            standby.restart()
        else
            standby.stop()
    }
    onStandbyWantedChanged: standbyAgain()
    Timer {
        id: standby
        interval: Math.max(1, win.standbySeconds) * 1000
        onTriggered: if (win.standbyWanted) win.setBarry(false, "untouched for " + win.standbySeconds + " s")
    }
    Connections {
        target: pages
        function onPageChanged() { win.standbyAgain() }
    }

    // What shelld says about Barry (GET /home), as it changes.
    function takeHome(answer) {
        if (!answer || typeof answer !== "object")
            return
        const open = answer.open === true
        const page = answer.page === 1 || answer.page === -1 ? answer.page : 0
        if (win.homeSerial >= 0 && answer.serial !== win.homeSerial && open) {
            win.openPage = page
            win.holdTaps(page, answer.lifted)
            win.slideIn(answer.serial, page, typeof answer.under === "string" ? answer.under : "")
        } else if (win.barryOpen && !open && win.sidePanel) {
            // A side panel closing back to its app: nothing fades
            // (that showed the game art); the app comes forward.
            sideIn.stop()
            win.underCover = true
        } else if (win.barryOpen && !open && win.heroOn && answer.app === true) {
            // An app takes the screen: black over the blurred art, never the
            // sharp art while the app's window comes (or this window's last
            // frame as Barry opens over it again).
            win.toBlack()
        } else if (win.barryOpen && !open && win.heroOn) {
            slide.stop()
            slideOut.start()
        } else if (win.homeSerial < 0 && (open || !win.heroOn)) {
            // Open already as this started (or always, without art).
            barryShift.y = 0
            barry.opacity = 1
            win.frost = win.heroOn ? 1 : 0
        }
        win.barryOver = answer.over === true
        if (!open)
            win.openPage = 0
        win.homeSerial = answer.serial
        // Open first: onAppFrontChanged must not take Barry opening over an
        // app for the app quitting with Barry closed (that faded the frost
        // off, the logo showing over Barry).
        win.barryOpen = open
        win.appFront = answer.app === true
        win.anyFront = answer.front === true
    }

    // Barry.Input's ShellState: shelld pushes /home and /apps as they change
    // (or, with an older shelld, it asks for them as this once did).
    Connections {
        target: ShellState
        function onHomeChanged() { win.takeHome(ShellState.home) }
        function onAppsChanged() { win.takeApps(ShellState.apps) }
        function onToolsChanged() {
            const t = ShellState.tools
            if (!(t && typeof t === "object" && t.leaving && Object.keys(t.leaving).length > 0))
                return
            // Black already, behind the tool as it slides: drawn by the time
            // Barry's window comes forward (set only as slideIn ran, the
            // window was sometimes shown with its last frame, the sharp art).
            win.fromTool = true
            unblack.stop()
            blackout.opacity = 1
            win.frost = 1
            blackoutGuard.restart()
        }
    }
    Component.onCompleted: {
        ShellState.watch("home")
        ShellState.watch("apps")
        win.takeHome(ShellState.home)
        win.takeApps(ShellState.apps)
    }
}
