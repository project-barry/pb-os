# Barry's touch rules

**Rule: touch is Barry Launcher's.** Every touch problem is fixed in Barry
Launcher's core code (this module, Barry.Input), and the fix reaches every
Barry app with it. Apps carry no touch math or tuning of their own: they bind
to the values the items below expose (a view, a scroll position) and react to
their signals. No slop, speed, friction, zoom step or timing lives in an app's
QML.

The rules are in `touchrules.h` (the numbers) and `panzoom.h`
(`PanZoomGesture`, the behavior). The trackpads (`TrackpadGesture`) and
`EdgeSwipe` read the same numbers. All of it is in the `Barry.Input` QML
module that devices already have (`/usr/lib/qt6/qml/Barry/Input`); nothing
else is installed for it.

## The model (Android's)

- **One transform for all fingers.** Each frame, the content point under the
  fingers' previous midpoint moves to their new midpoint (pan), and the zoom
  changes by the ratio of their spread (pinch, around that midpoint).
- **Fingers landing or lifting change nothing.** A frame moves the view only
  by the fingers that were down before and still are; the next frame
  measures from the fingers down now. A pinch flows into a pan (and back)
  without a lift and without a jump.
- **Touch slop.** A touch is no drag until its midpoint has moved `SLOP`
  (14 design px, the trackpad's tap slop), or two fingers' spread has changed
  by it; it then moves from there (no jump by the slop). On a list, only along
  an axis that scrolls: sideways on a vertical list is left to the page swipe.
- **Taps and long presses.** One finger, inside the slop, up before
  `LONG_PRESS_S` (0.5 s, the Launcher's tiles): a tap. Held that long: a long
  press, and the touch does nothing more. Double taps are off unless a view
  asks (`doubleTapZoom`); then a tap waits `TAP_S` (0.22 s) for a second one,
  and a double tap zooms in a step (x2, eased) around it.
- **Fling.** Lifted while moving: the view glides on along Android's scroller
  spline (OverScroller's distance and duration for the speed, `FLING_FRICTION`
  at `DESIGN_PPI`). The speed is a least-squares fit over the last
  `VELOCITY_WINDOW_S` (0.1 s, as EdgeSwipe); a finger that rested
  `STOPPED_S` (0.04 s) before lifting throws nothing. A pinch still zooming
  as it lifts throws nothing; nor does the finger left after a pinch, until
  it moves the slop by itself.
- **A touch stops a glide at once** and is a drag from there: it taps
  nothing under it (Android's RecyclerView).
- **Bounds.** Lists stop at their ends (no rubber band, no overscroll); a
  glide that reaches an end stops there. A view's zoom stays in
  `minZoom .. maxZoom`; its center can be kept in `bounds`.
- **Children keep their taps.** Buttons and tiles inside a surface get their
  touches exactly as before. Only once a touch is a drag is it taken (its tap
  handlers tap nothing) and kept (a page swipe around does not take a list's
  scroll back). A touch something else takes first (the page swipe, a slider)
  is let go.
- **Mouse and wheel.** A mouse drags as one finger. A wheel notch zooms a
  view by `WHEEL_ZOOM` around the pointer, or scrolls a list `WHEEL_PX`.

## QML API (`import Barry.Input`)

Both items share: `interactive` (bool, default true), and read-only
`active` (fingers or the mouse down), `dragging` (a finger moves it),
`moving` (dragging or gliding).

### PanZoomArea: a view

| | |
|---|---|
| `centerX`, `centerY` | the content point at the area's center (content units; read/write) |
| `zoom` | area pixels per content unit (read/write, kept in range) |
| `minZoom`, `maxZoom` | the zoom range (default 0.1 .. 10) |
| `bounds` | rect the center stays in; unset / `Qt.rect(0, 0, 0, 0)`: anywhere |
| `doubleTapZoom` | double taps zoom in a step (default false: taps are at once) |
| `tapped(x, y)`, `doubleTapped(x, y)`, `longPressed(x, y)` | where, area pixels |
| `mapToContent(x, y)`, `mapFromContent(x, y)` | area pixels <-> content units |
| `zoomBy(factor[, x, y])` | an eased zoom step (a +/- button), around the center or (x, y) |

Content is drawn at `area = (content - center) * zoom + size / 2`. Writing
`centerX`, `centerY` or `zoom` (following a player, a saved view) stops a
glide.

Example consumer (a map that keeps its view as `cx`, `cz`, `zoom` in world
units, such as Wayfarer's MapView): its PinchHandler, DragHandler and
WheelHandler go; its own math (`pan()`, `zoomBy()`, `clampZoom()`) is no
longer needed for touch.

```qml
import Barry.Input

Item {
    id: view
    // The view is the area's: one source of truth.
    property alias cx: pz.centerX
    property alias cz: pz.centerY
    property alias zoom: pz.zoom
    function sx(x) { return (x - cx) * zoom + width / 2 }
    function sy(z) { return (z - cz) * zoom + height / 2 }

    PanZoomArea {
        id: pz
        anchors.fill: parent
        minZoom: view.fitZoom * 0.9
        maxZoom: 4.5
        onTapped: (x, y) => view.pick(x, y)
        onLongPressed: (x, y) => view.pinAt(pz.mapToContent(x, y))
        onDraggingChanged: if (dragging) view.follow = false
    }
    // Tiles, markers and the player drawn with sx() / sy(), on top or
    // inside; a "+" button: pz.zoomBy(1.25).
}
```

### TouchScroll: a list

Goes around a Flickable, which keeps its look (content, size, clip,
`contentY` bindings) and stops taking input itself (`interactive` is set to
false); TouchScroll scrolls it along the axes its content overflows (and its
`flickableDirection` allows).

```qml
TouchScroll {
    anchors { fill: parent; bottomMargin: 70 * home.s }
    Flickable {
        id: flick
        anchors.fill: parent
        contentHeight: grid.height
        clip: true
        Grid { id: grid; ... }
    }
}
```

`flickable` names the Flickable when it is not TouchScroll's first child.
A Flickable that is a file's root (ONI Companion's ScreenView) becomes the
TouchScroll's child, with property aliases for what callers set on it.

## In the Launcher

| Surface | Now |
|---|---|
| Home's apps (`shell/Home.qml`, skins `steamos`, `cloud`, `sage`) | TouchScroll |
| Quick settings' edit bar, the hidden widgets row (`shell/QuickSettings.qml`) | TouchScroll |
| Page swipe (`Shell.qml`, PageSwipe) | its own (pages, not a view); shares the tap-cancel rule and knows a TouchScroll at work |
| Quick settings' widget move while editing (DragHandler) | unchanged: a drag-and-drop, not a pan or scroll |
| Quick settings' sliders and color picker (MouseArea) | unchanged: a value under the finger, not a pan or scroll |
| Keyboards (GridPanel's MultiPointTouchArea, KeyPanel's glide), trackpads | their own controllers in this module |
| Dino (`apps/dino`) | taps only (MouseArea); nothing scrolls |

## In Barry apps

The same items, for every app (`import Barry.Input`; the module is on every
device): a list scrolls in a TouchScroll, a map or a picture pans and zooms
in a PanZoomArea. An app has no Flickable drag, PinchHandler, DragHandler
for scrolling or fling math of its own; a touch that feels wrong in an app is
fixed here, for all of them.

`barry-app check`, `pack` and `install` (and shelld's install, in its log)
warn, and install all the same, when an app's QML scrolls or pinches by
itself (`barry_apps.touch_warnings`): a `PinchHandler` or `PinchArea`, or a
`Flickable`, `ListView` or `GridView` that is not inside a `TouchScroll`, is
not the one a TouchScroll names (`flickable:`) and does not set
`interactive: false`. A plain look at each `.qml` file's text (comments and
strings left out); DragHandler, MouseArea and MultiPointTouchArea are not
looked at (mostly taps, sliders and drag and drop).

| App | Now |
|---|---|
| ONI Companion (its five Flickables; ScreenView's is a file's root) | TouchScroll |

## Tuning

Only in `touchrules.h`: `SLOP`, `TAP_S`, `TAP_MAX_S`, `LONG_PRESS_S`,
`DOUBLE_TAP_SLOP`, `VELOCITY_WINDOW_S`, `STOPPED_S`, `FLING_MIN`, `FLING_MAX`,
`FLING_FRICTION`, `DESIGN_PPI`, `PINCH_STILL`, `WHEEL_PX`, `WHEEL_ZOOM`,
`DOUBLE_TAP_ZOOM`, `ZOOM_ANIM_S`. Design pixels: an item multiplies them by
its window's scale as the Launcher's layout (`min(w / 1240, h / 1080)`).
A longer or shorter glide: `FLING_FRICTION` (Android's 0.015) or
`DESIGN_PPI`.

## Tests

`tests/gestures/run.sh <rootfs> <work dir>` (as root in the build VM): builds
and runs `pan-zoom-test` (the core on frames: slop, pan, pinch, pinch to pan,
lift and land, quick pinch release, tap and long press, fling and catch,
clamps, list bounds and axes, wheel, double tap) and `touch-surfaces-test`
(TouchScroll in a PageSwipe and PanZoomArea under Qt's touch events,
offscreen). The rootfs needs `/t` (`/work/rootfs-stable` has it).

## Hand test on a device

Build (Mac Mini VM, from a checkout of the branch):

```sh
sudo external-and-mods/barry-launcher-input/build.sh /work/rootfs-stable /tmp/gesture-mod/Barry/Input
```

The rootfs's Qt must match the device's (6.8 on current images).

On the device (as `steamos`, over ssh):

```sh
# Back up
mkdir -p ~/gesture-backup/qml ~/gesture-backup/launcher/shell ~/gesture-backup/launcher/skins
cp -a /usr/lib/qt6/qml/Barry/Input/. ~/gesture-backup/qml/
cp -a /usr/share/barry_launcher/shell/Home.qml /usr/share/barry_launcher/shell/QuickSettings.qml ~/gesture-backup/launcher/shell/
for s in steamos cloud sage; do cp -a /usr/share/barry_launcher/dashboard/skins/$s/Home.qml ~/gesture-backup/launcher/skins/$s-Home.qml; done

# Install (the rootfs is read-only until unlocked)
sudo steamos-readonly disable
sudo install -m0755 ~/gesture-test/Barry/Input/libbarryinput.so /usr/lib/qt6/qml/Barry/Input/
sudo install -m0644 ~/gesture-test/Barry/Input/qmldir ~/gesture-test/Barry/Input/barryinput.qmltypes /usr/lib/qt6/qml/Barry/Input/
sudo install -m0644 ~/gesture-test/launcher/shell/Home.qml ~/gesture-test/launcher/shell/QuickSettings.qml /usr/share/barry_launcher/shell/
for s in steamos cloud sage; do sudo install -m0644 ~/gesture-test/launcher/skins/$s/Home.qml /usr/share/barry_launcher/dashboard/skins/$s/; done
systemctl --user restart barry_launcher.service

# Revert
sudo install -m0755 ~/gesture-backup/qml/libbarryinput.so /usr/lib/qt6/qml/Barry/Input/
sudo install -m0644 ~/gesture-backup/qml/qmldir ~/gesture-backup/qml/barryinput.qmltypes /usr/lib/qt6/qml/Barry/Input/
sudo install -m0644 ~/gesture-backup/launcher/shell/*.qml /usr/share/barry_launcher/shell/
for s in steamos cloud sage; do sudo install -m0644 ~/gesture-backup/launcher/skins/$s-Home.qml /usr/share/barry_launcher/dashboard/skins/$s/Home.qml; done
systemctl --user restart barry_launcher.service
sudo steamos-readonly enable
```

`~/gesture-test` holds the built `Barry/Input` (libbarryinput.so, qmldir,
barryinput.qmltypes) and the five QML files from the branch
(`launcher/shell/`, `launcher/skins/<skin>/Home.qml`). A Barry app on the
module (after it adopts PanZoomArea or TouchScroll) needs only the module
files. The new module still serves the old QML (it only adds types), so the
module alone can be tried first.
