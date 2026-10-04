"""barry_desktop: Barry Launcher under KWin (Plasma Desktop Mode on the AYN
Thor), for barry_launcher_shelld when BARRY_DESKTOP=1.

In Game Mode the bottom screen is a nested gamescope of its own; here it is
one of KWin's outputs (BARRY_OUTPUT, DSI-1). KWin scripts, loaded over
D-Bus, keep Barry's windows full screen on it (the keyboard along its
bottom edge), bring one forward, and push the active app up above the
keyboard. Text goes through barry_launcher_imd, KWin's input method, which
says when a text field is active and what text surrounds the caret.
"""
from __future__ import annotations

import json
import os
import socket
import threading
import time

from gi.repository import Gio, GLib

OUTPUT = os.environ.get("BARRY_OUTPUT", "DSI-1")
RUNTIME = os.environ.get("XDG_RUNTIME_DIR", "/tmp")
IM_SOCKET = os.path.join(RUNTIME, "barry_launcher_im.sock")
KEYBOARD_TITLE = "Barry Launcher Keyboard"
HOME_TITLE = "Barry Launcher"
# Window classes (Wayland app ids) of the apps Barry keeps on the bottom
# output: its Firefox profiles run with --name barry-<app>. Signal has one
# window, wherever it was opened from: here it is a top screen app.
APP_CLASSES = ["barry-browser", "barry-discord"]

# Shared by the scripts: the bottom output, and which windows are Barry's.
_COMMON = """
const OUTPUT = %(output)s, CLASSES = %(classes)s, KEYBOARD = %(keyboard)s;
function bottom() {
    for (const o of workspace.screens) if (o.name === OUTPUT) return o;
    return null;
}
function lower(s) { return (s || "").toString().toLowerCase(); }
function ours(w) {
    return (w.caption || "").startsWith("Barry Launcher") ||
        CLASSES.indexOf(lower(w.resourceClass)) >= 0 || CLASSES.indexOf(lower(w.resourceName)) >= 0;
}
function barry(w) { return w.normalWindow && ours(w); }
"""

PLACE_JS = _COMMON + """
// Barry's windows: full screen on the bottom output; the keyboard along
// its bottom edge, above the rest. Other windows stay off the bottom
// output, which is Barry's: one opened there goes to another output, and
// one dragged until it hangs over it slides back onto the output its title
// bar is on, once the drag ends (not sent off mid-drag, as that jumps it to
// the top). Dialogs of Barry's apps stay with them.
function elsewhere(o) {
    for (const s of workspace.screens) if (s !== o) return s;
    return null;
}
function outputAt(x, y) {
    for (const s of workspace.screens) {
        const g = s.geometry;
        if (x >= g.x && x < g.x + g.width && y >= g.y && y < g.y + g.height) return s;
    }
    return null;
}
function overlaps(f, g) {
    return f.x < g.x + g.width && f.x + f.width > g.x && f.y < g.y + g.height && f.y + f.height > g.y;
}
function inside(pos, size, start, length) {
    return size >= length ? start : Math.max(start, Math.min(pos, start + length - size));
}
function keepOff(w, o) {
    if (w.move || w.resize) return;
    if (w.fullScreen) {
        // Full screen on the bottom output (as it was left): the same, elsewhere.
        const t = elsewhere(o);
        if (w.output === o && t) workspace.sendClientToScreen(w, t);
        return;
    }
    const f = w.frameGeometry;
    if (!overlaps(f, o.geometry)) return;
    const home = outputAt(f.x + Math.min(40, f.width / 2), f.y + Math.min(20, f.height / 2));
    if (home && home !== o) {
        const g = home.geometry;
        const width = Math.min(f.width, g.width), height = Math.min(f.height, g.height);
        const x = inside(f.x, width, g.x, g.width), y = inside(f.y, height, g.y, g.height);
        if (x !== f.x || y !== f.y || width !== f.width || height !== f.height)
            w.frameGeometry = {x: x, y: y, width: width, height: height};
        return;
    }
    const t = elsewhere(o);
    if (t) workspace.sendClientToScreen(w, t);
}
function place(w) {
    const o = bottom();
    if (!o) return;
    if (!barry(w)) {
        const app = w.normalWindow || w.dialog || w.utility;
        const owned = w.transient && w.transientFor && ours(w.transientFor);
        if (app && !owned && !ours(w)) keepOff(w, o);
        return;
    }
    if (w.output !== o) workspace.sendClientToScreen(w, o);
    w.noBorder = true;
    // Barry's apps too: the top screen's taskbar would list them (its
    // Firefox icon taking in Barry's browser), and they are the bottom
    // screen's, switched there.
    w.skipTaskbar = true; w.skipPager = true; w.skipSwitcher = true;
    const g = o.geometry;
    if (w.caption === KEYBOARD) {
        w.keepAbove = true;
        // Along the lower edge, at whatever height it has now (it grows
        // once it knows its width): move it only when it is out of place.
        const f = w.frameGeometry, h = f.height;
        if (f.x !== g.x || f.y !== g.y + g.height - h || f.width !== g.width)
            w.frameGeometry = {x: g.x, y: g.y + g.height - h, width: g.width, height: h};
        return;
    }
    if (!w.fullScreen) w.fullScreen = true;
}
function watch(w) {
    place(w);
    w.captionChanged.connect(function () { place(w); });
    // KWin may first put a new window on the output with the pointer, and
    // the keyboard changes height: put Barry's windows back.
    w.outputChanged.connect(function () { place(w); });
    w.frameGeometryChanged.connect(function () {
        // The keyboard changes height; other windows may be dragged over.
        if (w.caption === KEYBOARD || !ours(w)) place(w);
    });
    w.interactiveMoveResizeFinished.connect(function () { place(w); });
}
for (const w of workspace.windowList()) watch(w);
workspace.windowAdded.connect(function (w) {
    watch(w);
    // A new Barry app or tool goes on top. KWin would stack a window it may
    // not activate (the trackpad and keyboards refuse focus) under the
    // active one, which here is the full-screen home screen.
    if (barry(w) && w.caption !== "Barry Launcher" && w.caption !== KEYBOARD)
        workspace.raiseWindow(w);
    w.captionChanged.connect(function () {
        if (barry(w) && w.caption !== "Barry Launcher" && w.caption !== KEYBOARD)
            workspace.raiseWindow(w);
    });
});
"""

REQUESTS_JS = """
// Barry's requests: bring a window forward, or fit the active app above
// the keyboard. A script loaded and unloaded for each of them crashed KWin
// (tearing down a script's engine; 2026-10-03), so this one script, never
// unloaded, runs them all: barry_launcher_shelld queues them and fires a
// shortcut with no keys, and the script takes the queue over D-Bus.
function activate(M) {
    for (const w of workspace.windowList()) {
        if (!w.normalWindow) continue;
        if (M.pid && w.pid !== M.pid) continue;
        if (M.caption && !(new RegExp(M.caption)).test(w.caption || "")) continue;
        if (M.classes && M.classes.indexOf(lower(w.resourceClass)) < 0 && M.classes.indexOf(lower(w.resourceName)) < 0) continue;
        w.minimized = false;
        workspace.raiseWindow(w);  // the tools refuse focus, so activating alone would not
        workspace.activeWindow = w;
        break;
    }
}
// The keyboard is up or down: the active Barry app on the bottom output
// shrinks to the space above the keyboard window (its height as KWin has
// it, whatever scale the keyboard drew at), or goes back to full screen.
function inset(up) {
    const o = bottom(), w = workspace.activeWindow;
    let height = 0;
    for (const k of workspace.windowList()) if (k.caption === KEYBOARD && k.output === o) height = k.frameGeometry.height;
    if (!o || !w || !barry(w) || w.caption === KEYBOARD || w.caption === "Barry Launcher" || w.output !== o) return;
    const g = o.geometry;
    if (up && height > 0) {
        w.fullScreen = false;
        w.frameGeometry = {x: g.x, y: g.y, width: g.width, height: g.height - height};
    } else {
        w.fullScreen = true;
    }
}
registerShortcut(%(shortcut)s, "Barry Launcher: run queued requests", "", function () {
    callDBus(%(service)s, %(path)s, %(service)s, "Take", function (queued) {
        for (const r of JSON.parse(queued)) {
            if (r.activate) activate(r.activate);
            else if ("inset" in r) inset(r.inset);
        }
    });
});
"""

SCRIPT_NAME = "barry-desktop"
SERVICE = "org.barry_launcher.Desktop"
SERVICE_PATH = "/org/barry_launcher/Desktop"
SHORTCUT = "barry-launcher-requests"
_SERVICE_XML = f"""<node><interface name="{SERVICE}">
<method name="Take"><arg type="s" direction="out"/></method>
</interface></node>"""
_QUEUE: list[dict] = []
_QUEUE_LOCK = threading.Lock()
_SERVING = [False]


def _js(template: str, **values) -> str:
    params = {"output": json.dumps(OUTPUT), "classes": json.dumps(APP_CLASSES),
              "keyboard": json.dumps(KEYBOARD_TITLE), "shortcut": json.dumps(SHORTCUT),
              "service": json.dumps(SERVICE), "path": json.dumps(SERVICE_PATH)}
    params.update(values)
    return template % params


def _kwin(bus, obj: str, iface: str, method: str, args, reply):
    return bus.call_sync("org.kde.KWin", obj, iface, method, args, reply,
                         Gio.DBusCallFlags.NONE, 3000, None)


def _serve(bus) -> None:
    """Hand the queued requests to the script (on the GLib main loop)."""
    if _SERVING[0]:
        return
    def on_call(_conn, _sender, _path, _iface, method, _params, invocation) -> None:
        with _QUEUE_LOCK:
            queued = json.dumps(_QUEUE)
            _QUEUE.clear()
        invocation.return_value(GLib.Variant("(s)", (queued,)))
    node = Gio.DBusNodeInfo.new_for_xml(_SERVICE_XML)
    bus.register_object(SERVICE_PATH, node.interfaces[0], on_call, None, None)
    Gio.bus_own_name_on_connection(bus, SERVICE, Gio.BusNameOwnerFlags.REPLACE, None, None)
    _SERVING[0] = True


def place_windows() -> bool:
    """Load the script that places Barry's windows and runs its requests,
    once per KWin session. It is never unloaded (see REQUESTS_JS): one
    already loaded, by an earlier barry_launcher_shelld, stays, so a change
    to it takes effect at the next login."""
    path = os.path.join(RUNTIME, f"{SCRIPT_NAME}.js")
    try:
        bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
        _serve(bus)
        loaded = _kwin(bus, "/Scripting", "org.kde.kwin.Scripting", "isScriptLoaded",
                       GLib.Variant("(s)", (SCRIPT_NAME,)), GLib.VariantType("(b)")).unpack()[0]
        if loaded:
            return True
        with open(path, "w", encoding="utf-8") as fh:
            fh.write(_js(PLACE_JS + REQUESTS_JS))
        sid = _kwin(bus, "/Scripting", "org.kde.kwin.Scripting", "loadScript",
                    GLib.Variant("(ss)", (path, SCRIPT_NAME)), GLib.VariantType("(i)")).unpack()[0]
        if sid < 0:
            return False
        _kwin(bus, f"/Scripting/Script{sid}", "org.kde.kwin.Script", "run", None, None)
    except (GLib.Error, OSError) as err:
        print(f"kwin script {SCRIPT_NAME}: {err}", flush=True)
        return False
    return True


def _request(request: dict) -> bool:
    with _QUEUE_LOCK:
        _QUEUE.append(request)
    try:
        bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
        bus.call_sync("org.kde.kglobalaccel", "/component/kwin", "org.kde.kglobalaccel.Component",
                      "invokeShortcut", GLib.Variant("(s)", (SHORTCUT,)), None,
                      Gio.DBusCallFlags.NONE, 3000, None)
    except GLib.Error as err:
        print(f"kwin request {request}: {err.message}", flush=True)
        return False
    return True


def activate(caption: str | None = None, classes: list[str] | None = None, pid: int | None = None) -> bool:
    match = {"caption": caption, "classes": [c.lower() for c in classes] if classes else None, "pid": pid}
    return _request({"activate": match})


def set_inset(inset: int) -> bool:
    """The keyboard is up (inset > 0) or down. The keyboard window may not
    be mapped yet when it goes up: again once it is."""
    up = inset > 0
    ok = _request({"inset": up})
    if up:
        threading.Timer(0.4, lambda: _request({"inset": up})).start()
    return ok


class InputMethod:
    """barry_launcher_imd's side: whether a text field is active, the text
    around its caret, and typing into it. Events are handed to on_event
    (on the GLib main loop) as dicts."""

    def __init__(self, on_event) -> None:
        self.on_event = on_event
        self.lock = threading.Lock()
        self.sock: socket.socket | None = None
        self.active = False
        self.text: str | None = None
        self.cursor = 0  # bytes into text

    @property
    def connected(self) -> bool:
        return self.sock is not None

    def run(self) -> None:
        while True:
            try:
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                s.connect(IM_SOCKET)
            except OSError:
                time.sleep(1.0)
                continue
            with self.lock:
                self.sock = s
            print("connected to barry_launcher_imd", flush=True)
            buf = b""
            try:
                while True:
                    data = s.recv(4096)
                    if not data:
                        break
                    buf += data
                    while b"\n" in buf:
                        line, buf = buf.split(b"\n", 1)
                        try:
                            ev = json.loads(line)
                        except ValueError:
                            continue
                        self._update(ev)
                        GLib.idle_add(self._deliver, ev)
            except OSError:
                pass
            with self.lock:
                self.sock = None
                self.active = False
                self.text = None
            s.close()
            time.sleep(1.0)

    def _update(self, ev: dict) -> None:
        kind = ev.get("event")
        with self.lock:
            if kind == "activate":
                self.active, self.text, self.cursor = True, None, 0
            elif kind == "deactivate":
                self.active, self.text, self.cursor = False, None, 0
            elif kind == "surrounding":
                self.text, self.cursor = ev.get("text", ""), int(ev.get("cursor", 0))

    def _deliver(self, ev: dict) -> bool:
        self.on_event(ev)
        return False

    def before_cursor(self) -> str | None:
        with self.lock:
            if self.text is None:
                return None
            return self.text.encode("utf-8")[: self.cursor].decode("utf-8", errors="ignore")

    def send(self, line: str) -> bool:
        line = line.replace("\n", " ")
        with self.lock:
            if self.sock is None:
                return False
            try:
                self.sock.sendall((line + "\n").encode("utf-8"))
                return True
            except OSError:
                return False

    def commit(self, text: str) -> bool:
        return self.send(f"COMMIT {text}")

    def back(self, n: int, text: str = "") -> bool:
        return self.send(f"BACK {n} {text}" if text else f"BACK {n}")

    def key(self, name: str) -> bool:
        return self.send(f"KEY {name}")
