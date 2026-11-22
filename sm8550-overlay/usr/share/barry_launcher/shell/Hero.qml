pragma ComponentBehavior: Bound
// The bottom screen while Barry Launcher is closed: the hero art of the
// game selected on the top screen, with its logo floating in front
// (Barry.Input's SteamGame: the PB-OS tab follows Steam's focus, and
// pbos-qamd finds the art: the user's own, Steam's, or SteamGridDB's). A
// new game crossfades in once its pictures have loaded.
//
// The wide hero pans slowly from side to side, and the logo bobs. Both stop
// while a game runs: the bottom screen then costs the GPU nothing.
//
// While a game runs (and Barry is closed) the art dims: the game on the top
// screen has the focus. A tap wakes Barry, and the art is back at full
// brightness behind it.
//
// While Barry is open its apps sit over the art as if behind frosted glass:
// `frost` (0..1, Shell.qml animates it) blurs and dims the art and fades
// the logo away.
// Without a logo the game's name stands in; without any game, a hint.
// Colors are the skin's; a skin can also replace this file (SkinView.qml).
import QtQuick
import QtQuick.Effects
import Barry.Input

Rectangle {
    id: hero
    property real s: 1
    // Shown (Barry closed): animations run only then.
    property bool shown: true
    // 0: the art in focus; 1: frosted behind Barry, the logo gone.
    property real frost: 0
    // A game running: the art dims (less as Barry comes over it).
    property real gameDim: SteamGame.running ? 1 : 0
    Behavior on gameDim { NumberAnimation { duration: 800; easing.type: Easing.InOutQuad } }
    readonly property bool moving: shown && !SteamGame.running

    color: Skin.colors.background
    clip: true

    component Layer: Item {
        id: layer
        property string name: ""
        property url heroUrl
        property url logoUrl
        // Both pictures done loading (or failed): it may fade in.
        readonly property bool ready: heroImg.status !== Image.Loading && logoImg.status !== Image.Loading
        property real pan: 0.5

        anchors.fill: parent
        opacity: 0

        Image {
            id: heroImg
            readonly property real aspect: implicitHeight > 0 ? implicitWidth / implicitHeight : 0
            // As tall as the screen; wider than it, it pans; narrower, it fills.
            readonly property bool wide: aspect * parent.height > parent.width
            height: wide ? parent.height : parent.width / Math.max(0.1, aspect)
            width: wide ? height * aspect : parent.width
            anchors.verticalCenter: parent.verticalCenter
            x: wide ? -(width - parent.width) * layer.pan : 0
            source: layer.heroUrl
            sourceSize.height: hero.height
            fillMode: Image.PreserveAspectFit
            asynchronous: true
            visible: status === Image.Ready
        }
        // A darker floor, so the logo and name read on bright art.
        Rectangle {
            anchors.fill: parent
            visible: heroImg.visible
            gradient: Gradient {
                GradientStop { position: 0.0; color: Qt.rgba(0, 0, 0, 0.15) }
                GradientStop { position: 0.55; color: Qt.rgba(0, 0, 0, 0.25) }
                GradientStop { position: 1.0; color: Qt.rgba(0, 0, 0, 0.6) }
            }
        }

        Item {
            id: floater
            opacity: 1 - hero.frost
            anchors.centerIn: parent
            width: parent.width * 0.74
            height: parent.height * 0.42
            property real bob: 0
            transform: Translate { y: floater.bob * 14 * hero.s }

            Image {
                id: logoImg
                anchors.fill: parent
                source: layer.logoUrl
                sourceSize.width: width
                fillMode: Image.PreserveAspectFit
                asynchronous: true
                visible: status === Image.Ready
            }
            Text {
                anchors.fill: parent
                visible: !logoImg.visible && layer.name !== ""
                text: layer.name
                color: Skin.colors.text
                style: Text.Raised
                styleColor: Qt.rgba(0, 0, 0, 0.7)
                font { family: Skin.font || "Noto Sans"; pixelSize: 84 * hero.s; weight: Font.Bold }
                fontSizeMode: Text.Fit
                minimumPixelSize: 30 * hero.s
                wrapMode: Text.Wrap
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
            SequentialAnimation on bob {
                running: hero.moving && layer.opacity > 0
                loops: Animation.Infinite
                NumberAnimation { from: -1; to: 1; duration: 3200; easing.type: Easing.InOutSine }
                NumberAnimation { from: 1; to: -1; duration: 3200; easing.type: Easing.InOutSine }
            }
        }

        SequentialAnimation on pan {
            running: hero.moving && layer.opacity > 0 && heroImg.wide
            loops: Animation.Infinite
            NumberAnimation { to: 1; duration: 24000; easing.type: Easing.InOutSine }
            NumberAnimation { to: 0; duration: 24000; easing.type: Easing.InOutSine }
        }
    }

    // Two layers: the one showing, and the next game's, fading in over it.
    // Frosted together (blur, a little less color, barely darker); with
    // the software renderer, which has no shader effects, only a light
    // tint. The layer is always on (switching it on as Barry opened showed
    // a black frame, and was a suspect for stray specks on the Thor's GPU);
    // it costs nothing while nothing moves, as while a game runs.
    Item {
        id: art
        anchors.fill: parent
        Layer { id: layerA }
        Layer { id: layerB }
        layer.enabled: GraphicsInfo.api !== GraphicsInfo.Software
        layer.effect: MultiEffect {
            autoPaddingEnabled: false
            blurEnabled: true
            blurMax: 48
            blur: hero.frost
            saturation: -0.15 * hero.frost
        }
    }
    Rectangle {
        anchors.fill: parent
        color: Skin.colors.background
        opacity: 0.1 * hero.frost
        visible: opacity > 0
    }
    Rectangle {
        anchors.fill: parent
        color: "#000000"
        opacity: 0.6 * hero.gameDim * (1 - hero.frost)
        visible: opacity > 0
    }
    property Item showing: null
    property Item incoming: null

    function show() {
        const next = showing === layerA ? layerB : layerA
        if (next === fadeIn.old) {
            fadeIn.stop()
            fadeIn.old = null
            if (showing)
                showing.opacity = 1
        }
        next.opacity = 0
        next.name = SteamGame.name
        next.heroUrl = SteamGame.hero
        next.logoUrl = SteamGame.logo
        next.pan = 0.5
        incoming = next
        if (next.ready)
            reveal()
    }
    // The new art fades in over the old, which stays fully shown under it
    // until then: fading both at once let the black behind show through
    // halfway.
    function reveal() {
        if (!incoming)
            return
        fadeIn.stop()
        if (fadeIn.old && fadeIn.old !== incoming)
            fadeIn.old.opacity = 0
        const old = showing
        showing = incoming
        incoming = null
        if (old)
            old.z = 0
        showing.z = 1
        fadeIn.old = old
        fadeIn.target = showing
        fadeIn.start()
    }
    NumberAnimation {
        id: fadeIn
        property Item old: null
        property: "opacity"
        from: 0
        to: 1
        duration: 450
        easing.type: Easing.InOutQuad
        onFinished: {
            if (old && old !== hero.showing)
                old.opacity = 0
            old = null
        }
    }
    Connections {
        target: hero.incoming
        function onReadyChanged() { if (hero.incoming && hero.incoming.ready) hero.reveal() }
    }
    Connections {
        target: SteamGame
        function onChanged() { if (SteamGame.available) hero.show() }
    }
    Component.onCompleted: if (SteamGame.available) show()

    // No game told yet (Steam starting, or no PB-OS tab).
    Column {
        anchors.centerIn: parent
        visible: !SteamGame.available
        spacing: 18 * hero.s
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: "Barry Launcher"
            color: Skin.colors.text
            font { family: Skin.font || "Noto Sans"; pixelSize: 64 * hero.s; weight: Font.Bold }
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            // The device's Barry button; without one, a tap on the art.
            text: Device.barryButton !== ""
                  ? "press " + Device.barryButton + " to open  ·  hold " + Device.barryButton + ": screen off"
                  : "tap to open"
            color: Skin.colors.text
            opacity: 0.55
            font { family: Skin.font || "Noto Sans"; pixelSize: 30 * hero.s }
        }
    }
}
