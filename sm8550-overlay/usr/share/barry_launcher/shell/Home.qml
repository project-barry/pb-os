pragma ComponentBehavior: Bound
// Home screen: app tiles. Black around them (pixels off on the AMOLED).
// A running app's tile brings it forward and has a close badge (not
// Trackpad and Keyboard, which close when Barry opens); the AYN button opens
// Barry over any app, and closes back to it. Trackpad and Keyboard open the
// same app (TopInputApp.qml), the top screen's trackpad and keyboard:
// Keyboard with the keys up. Colors are the skin's; a skin can also
// replace this file (SkinView.qml), keeping these properties and signals.
import QtQuick
import QtQuick.Effects
import Barry.Input

Rectangle {
    id: home
    property real s: 1
    property var running: ({})
    property var status: ({})  // app -> "Installing…" etc. while it gets ready
    // The tiles in order, without the hidden ones (shelld's /apps; the
    // Barry Launcher plugin's Apps tab sets them). These until it answers.
    property var tiles: [
        { app: "browser", name: "Firefox", icon: "firefox", closable: true },
        { app: "discord", name: "Discord", icon: "discord", closable: true },
        { app: "signal", name: "Signal", icon: "signal", closable: true },
        { app: "trackpad", name: "Trackpad", icon: "trackpad", closable: false },
        { app: "keyboard", name: "Keyboard", icon: "keyboard", closable: false },
    ]
    readonly property int rows: Math.max(1, Math.ceil(tiles.length / 3))
    // The tiles' scale: as big as before for two rows, smaller to fit three.
    readonly property real ts: s * Math.min(1, (height - 110 * s) / (Math.min(3, rows) * 370 * s - 30 * s))
    signal launch(string app)
    signal close(string app)

    // Over the game art, frosted (Shell.qml sets it): how solid this page's
    // own background is, 1 fully.
    property real glass: 1
    function tint(color) {
        const c = Qt.color(color)
        return Qt.rgba(c.r, c.g, c.b, c.a * glass)
    }
    // The tiles over the art: see-through too, but less than the page.
    readonly property real tileGlass: glass < 1 ? 0.6 : 1
    function tileTint(color) {
        const c = Qt.color(color)
        return Qt.rgba(c.r, c.g, c.b, c.a * tileGlass)
    }

    color: tint(Skin.colors.background)

    // Barry opening: the tiles fly in toward the screen, the middle ones
    // first (Barry.Input's TileDrop; Shell.qml calls dropIn()).
    property real dropClock: -1
    // Tiles take no taps before this (ms since the epoch): set as Barry opens.
    property double armedAt: 0
    function dropIn() {
        armedAt = Date.now() + 600
        const m = grid.mapToItem(home, grid.width / 2, grid.height / 2)
        const tiles = []
        for (let i = 0; i < tilesRep.count; i++)
            if (tilesRep.itemAt(i))
                tiles.push(tilesRep.itemAt(i))
        let near = Infinity, reach = 0
        for (const t of tiles) {
            const c = t.mapToItem(home, t.width / 2, t.height / 2)
            const d = Math.hypot(c.x - m.x, c.y - m.y)
            near = Math.min(near, d)
            reach = Math.max(reach, d)
        }
        for (const t of tiles)
            t.drop.aim(home, m.x, m.y, near, reach)
        dropAnim.stop()
        if (!tiles.length)
            return
        dropAnim.to = tiles[0].drop.length
        dropAnim.duration = tiles[0].drop.length
        dropClock = 0
        dropAnim.start()
    }
    NumberAnimation {
        id: dropAnim
        target: home
        property: "dropClock"
        from: 0
        onFinished: home.dropClock = -1
    }

    component Tile: Item {
        id: tile
        property string app
        property string name
        property string iconKind  // an Icon kind, or a user app's icon file (file://)
        readonly property bool imageIcon: iconKind.startsWith("file:")
        readonly property bool symbolicIcon: imageIcon && /(-symbolic\.\w+|#symbolic)$/i.test(iconKind)
        property bool closable: true  // Trackpad and Keyboard close on Home by themselves
        readonly property bool isRunning: closable && home.running[app] === true
        readonly property string statusText: home.status[app] || ""

        width: 300 * home.ts
        height: 340 * home.ts

        // Flies in as Barry opens (Barry.Input's TileDrop).
        property alias drop: dropper
        TileDrop {
            id: dropper
            clock: home.dropClock

            Rectangle {
                id: face
                anchors.horizontalCenter: parent.horizontalCenter
                width: 260 * home.ts
                height: 260 * home.ts
                radius: 56 * home.ts
                color: home.tileTint(tap.pressed ? Skin.colors.raised : Skin.colors.panel)
                border.color: tile.isRunning || tile.statusText ? Skin.colors.accent : Skin.colors.border
                border.width: 3 * home.ts

                SequentialAnimation on opacity {
                    running: tile.statusText !== ""
                    loops: Animation.Infinite
                    alwaysRunToEnd: true
                    NumberAnimation { to: 0.45; duration: 700; easing.type: Easing.InOutQuad }
                    NumberAnimation { to: 1; duration: 700; easing.type: Easing.InOutQuad }
                }

                Icon {
                    anchors.centerIn: parent
                    visible: !tile.imageIcon
                    width: 150 * home.ts
                    height: 150 * home.ts
                    kind: tile.imageIcon ? "" : tile.iconKind
                    color: Skin.colors.icon
                    lineWidth: 9 * home.ts
                }
                // A user app's own icon (barry_apps): a picture file. One
                // named *-symbolic.* (white on transparent, as in icon themes), or
                // drawn in one color (shelld adds #symbolic to its address),
                // takes the skin's icon color like the built-in icons.
                Image {
                    anchors.centerIn: parent
                    visible: tile.imageIcon
                    width: 170 * home.ts
                    height: 170 * home.ts
                    source: tile.imageIcon ? tile.iconKind : ""
                    sourceSize: Qt.size(width, height)
                    fillMode: Image.PreserveAspectFit
                    smooth: true
                    asynchronous: true
                    layer.enabled: tile.symbolicIcon
                    layer.effect: MultiEffect {
                        colorization: 1
                        colorizationColor: Skin.colors.icon
                    }
                }
                // A quick tap only, and not while Barry is still coming in:
                // thumbs resting on the screen's lower corners started
                // Trackpad and Deck (2026-10-17).
                TapHandler {
                    id: tap
                    longPressThreshold: 0.5
                    onTapped: if (Date.now() >= home.armedAt) home.launch(tile.app)
                }
            }

            // Close badge on a running app. Its touch area is wider than the
            // badge, so a finger needn't hit the badge itself.
            Item {
                visible: tile.isRunning
                x: badge.x - (width - badge.width) / 2
                y: badge.y - (height - badge.height) / 2
                width: 150 * home.ts
                height: 150 * home.ts
                TapHandler {
                    id: closeTap
                    gesturePolicy: TapHandler.ReleaseWithinBounds
                    onTapped: home.close(tile.app)
                }
            }
            Rectangle {
                id: badge
                visible: tile.isRunning
                x: face.x + face.width - width * 0.7
                y: -height * 0.3
                width: 76 * home.ts
                height: 76 * home.ts
                radius: width / 2
                color: closeTap.pressed ? Skin.colors.danger : Skin.colors.border
                border.color: Skin.colors.background
                border.width: 4 * home.ts
                Icon {
                    anchors.centerIn: parent
                    width: 40 * home.ts
                    height: 40 * home.ts
                    kind: "close"
                    color: Skin.colors.text
                    lineWidth: 6 * home.ts
                }
            }

            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.top: face.bottom
                anchors.topMargin: 24 * home.ts
                // Long names (user apps have up to 20 characters) shrink to fit.
                width: tile.width + 50 * home.ts
                horizontalAlignment: Text.AlignHCenter
                fontSizeMode: Text.HorizontalFit
                minimumPixelSize: 26 * home.ts
                elide: Text.ElideRight
                text: tile.statusText || tile.name
                color: Skin.colors.text
                font { family: Skin.font; pixelSize: 40 * home.ts; weight: Font.DemiBold }
            }
        }
    }

    // Up to three rows (nine apps) fit the screen, the tiles a little
    // smaller for the third; with more (user apps) they scroll, above the
    // hint at the bottom.
    // Barry's touch rules scroll it (Barry.Input's TouchScroll).
    TouchScroll {
        anchors { fill: parent; bottomMargin: 70 * home.s }

        Flickable {
            id: flick
            anchors.fill: parent
            contentWidth: width
            contentHeight: Math.max(height, grid.height + 60 * home.s)
            boundsBehavior: Flickable.StopAtBounds
            clip: true

            Grid {
                id: grid
                anchors.horizontalCenter: parent.horizontalCenter
                // Centred on the screen when there is room, as high as needed
                // when not.
                y: Math.max(20 * home.s, Math.min((flick.height - height) / 2 + 35 * home.s,
                                                  flick.height - height - 20 * home.s))
                columns: 3
                columnSpacing: 60 * home.ts
                rowSpacing: 30 * home.ts

                Repeater {
                    id: tilesRep
                    model: home.tiles
                    delegate: Tile {
                        required property var modelData
                        app: modelData.app
                        name: modelData.name
                        iconKind: modelData.icon
                        closable: modelData.closable
                    }
                }
            }
        }
    }

    Text {
        visible: home.tiles.length === 0
        anchors.centerIn: parent
        width: parent.width * 0.8
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.WordWrap
        text: "All apps are hidden: Quick Access → Barry Launcher → Apps shows them again."
        color: Skin.colors.text
        opacity: 0.6
        font { family: Skin.font; pixelSize: 36 * home.s }
    }

    Text {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 30 * home.s
        text: "swipe right: performance  ·  swipe left: quick settings  ·  AYN: close"
        // Game Mode's pages and button (no side pages in Desktop Mode).
        visible: Qt.application.arguments.indexOf("desktop") < 0
        color: Skin.colors.text
        opacity: 0.4
        font { family: Skin.font; pixelSize: 24 * home.s }
    }
}
