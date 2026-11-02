#!/usr/bin/env python3
"""barry_launcher_shelld's event stream (SHELLD-EVENTS.md), off the device.

Runs inside run.sh's sandbox (a SteamOS rootfs, Xvfb, stub xdotool,
firefox, qml6, flatpak and gamescopectl on PATH):

  shelld_events.py <new shelld> <old shelld>

1. The new shelld alone: the snapshot on connect is every topic, in order,
   each byte for byte its GET; a change (Barry opened by POST /barry and by
   the AYN flag, the keyboard, keyboard_height, an app stub started and
   closed, the home screen's order by POST and by a file written beside
   shelld) gives exactly one line for each topic it changes, and the GETs
   then answer the same; a second client's snapshot is the GETs; nothing
   comes for IDLE_S while idle; the keyboard window is marked an overlay
   once gamescope's window list changes (xprop -spy), and that xprop ends.
   Barry opening over a tool (the trackpad, the Deck controls; qml6_stub.py
   plays ToolStage.qml's part): one that said {"tool": true} is asked to
   slide away (the tools topic, by pid) before Barry's home line, which
   waits for its {"tool_left": true}, or TOOL_LEAVE_S for one that never
   answers; it is ended after Barry's line either way; an older tool (says
   nothing) goes at once, as before.
   Apps in the background (run.sh's stub systemd-run makes real cgroup v2
   scopes; FREEZE_S and CLOSE_S shortened): the AYN button and the standby
   timeout close Barry over an app to the art (home: open and app false),
   the app running on; a side page swiped away goes back to it; FREEZE_S
   later its scope is frozen (cgroup.events), a tile tap thaws it before
   /launch answers; a sound stream of its own (stub pactl) keeps it awake,
   and FREEZE_S after the stream is corked it freezes; CLOSE_S after it
   left the screen it is closed (thawed first, so its SIGTERM ends it), as
   is one closed by hand while frozen; "keepAwake" keeps a user app awake;
   each memory pressure trigger (the FIFO in PSI's place) closes the frozen
   app least recently on the screen.
   Glide typing's dictionary: none is loaded at start (its log line); a "keep" on POST /type writes the personal
   words file without loading one; the first POST /suggest and /glide load
   English's and answer as barry_glide itself does; GLIDE_IDLE_S without a
   call drops it (a call meanwhile puts that off) and the next call loads
   it again. RSS is printed at each step.
2. Idle CPU (utime+stime, children included) over CPU_S: the old shelld
   polled as Shell.qml and Keyboard.qml polled it (GET /home every 50 ms,
   /keyboard?h= every 100 ms, /apps every 1 s); the new one with a socket
   client; the new one polled the old way (the HTTP API's cost). Also: the
   old and the new answer the same GETs at start. RSS at idle after
   start: the new one's, and the one before it (run.sh's BEFORE_SHELLD,
   which loaded English's dictionary at start) when given.
"""
from __future__ import annotations

import http.client
import json
import os
import select
import shutil
import signal
import socket
import subprocess
import sys
import threading
import time

PORT = 47990
TOPICS = ("home", "keyboard", "top-keyboard", "keyd", "apps", "tools", "updates")
OLD_TOPICS = TOPICS[:5]  # the old shelld's (no /tools)
TOOL_SLIDE_S = 0.24  # qml6_stub.py's slide, as ToolStage.qml's
TOOL_LEAVE_S = 0.4  # shelld's cap
IDLE_S = float(os.environ.get("IDLE_S", "30"))
CPU_S = float(os.environ.get("CPU_S", "60"))
SETTLE_S = 8.0  # after start: Dino installs
GLIDE_IDLE_S = 3.0  # shelld's BARRY_GLIDE_IDLE_S here (600 s on the Thor)
BEFORE = "/t/lib/barry_launcher_shelld_before"  # run.sh's BEFORE_SHELLD
RUN = "/t/run"
SOCK = f"{RUN}/barry_launcher/shelld.sock"
FLAG = "/run/sm8550-thor/overlay"
TOP_INPUT = "/run/sm8550-thor/top-input"
KEYBOARD_TITLE = "Barry Launcher Keyboard"
DINO = "io.github.project-barry.dino"
FREEZE_S = 2.0  # shelld's BARRY_FREEZE_S here (30 s on the Thor)
CLOSE_S = 8.0  # BARRY_BACKGROUND_CLOSE_S (30 min)
PSI = "/t/psi"
BG, SLEEPY, AWAKE = "io.github.test.bg", "io.github.test.sleepy", "io.github.test.awake"  # user apps
PACTL_EVENTS = "/t/pactl-events"
PACTL_STREAMS = "/t/pactl-sink-inputs.json"

failures: list[str] = []


def check(ok: bool, what: str) -> None:
    print(("PASS " if ok else "FAIL ") + what, flush=True)
    if not ok:
        failures.append(what)


def env_for(home: str) -> dict:
    return dict(os.environ, HOME=home, XDG_RUNTIME_DIR=RUN, XDG_CONFIG_HOME=f"{home}/.config",
                XDG_DATA_HOME=f"{home}/.local/share", XDG_CACHE_HOME=f"{home}/.cache",
                BARRY_PORT=str(PORT), DISPLAY=":99", PYTHONUNBUFFERED="1",
                NO_AT_BRIDGE="1", BARRY_FREEZE_S=str(FREEZE_S), BARRY_BACKGROUND_CLOSE_S=str(CLOSE_S),
                BARRY_PSI_PATH=PSI, BARRY_GLIDE_IDLE_S=str(GLIDE_IDLE_S))


def start_shelld(path: str, tag: str) -> tuple[subprocess.Popen, str]:
    home = "/t/home"  # the same for each run: GETs that name it compare
    shutil.rmtree(home, ignore_errors=True)
    os.makedirs(home)
    shutil.rmtree(f"{RUN}/barry_launcher", ignore_errors=True)
    log = open(f"/t/shelld-{tag}.log", "w")
    # Beside the libraries it imports (barry_apps; barry_glide on first use).
    proc = subprocess.Popen(["python3", path], env=env_for(home), stdout=log, stderr=subprocess.STDOUT,
                            start_new_session=True)
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        try:
            get("/home")
            return proc, home
        except OSError:
            time.sleep(0.1)
    raise SystemExit(f"{tag}: shelld did not answer; see /t/shelld-{tag}.log")


def stop_shelld(proc: subprocess.Popen) -> None:
    os.killpg(proc.pid, signal.SIGTERM)
    try:
        proc.wait(5)
    except subprocess.TimeoutExpired:
        os.killpg(proc.pid, signal.SIGKILL)
        proc.wait()


def get(path: str, conn: http.client.HTTPConnection | None = None) -> bytes:
    c = conn or http.client.HTTPConnection("127.0.0.1", PORT, timeout=5)
    c.request("GET", path)
    r = c.getresponse()
    body = r.read()
    if conn is None:
        c.close()
    return body


def post(path: str, body: dict) -> dict:
    c = http.client.HTTPConnection("127.0.0.1", PORT, timeout=10)
    c.request("POST", path, json.dumps(body), {"Content-Type": "application/json"})
    out = json.loads(c.getresponse().read() or b"{}")
    c.close()
    return out


def gets() -> dict[str, bytes]:
    return {t: get("/" + t) for t in TOPICS}


class Client:
    def __init__(self) -> None:
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.connect(SOCK)
        self.buf = b""

    def read(self, seconds: float) -> list[tuple[str, bytes]]:
        """The lines that come within seconds: (topic, the data's raw JSON)."""
        return [(t, d) for _, t, d in self.read_timed(seconds)]

    def read_timed(self, seconds: float) -> list[tuple[float, str, bytes]]:
        """As read, each line with the monotonic time it came."""
        out = []
        end = time.monotonic() + seconds
        while (left := end - time.monotonic()) > 0:
            r, _, _ = select.select([self.sock], [], [], left)
            if not r:
                break
            data = self.sock.recv(65536)
            if not data:
                break
            self.buf += data
            while b"\n" in self.buf:
                line, self.buf = self.buf.split(b"\n", 1)
                topic = json.loads(line)["topic"]
                head = b'{"topic": "' + topic.encode() + b'", "data": '
                assert line.startswith(head) and line.endswith(b"}"), line
                out.append((time.monotonic(), topic, line[len(head):-1]))
        return out

    def send(self, obj: dict) -> None:
        self.sock.sendall(json.dumps(obj).encode() + b"\n")


def expect(client: Client, what: str, topics: set[str], seconds: float = 1.0) -> list[tuple[str, bytes]]:
    lines = client.read(seconds)
    got = [t for t, _ in lines]
    check(sorted(got) == sorted(topics), f"{what}: one line each for {sorted(topics)} (got {got})")
    now = gets()
    check(all(now[t] == d for t, d in lines), f"{what}: the lines' data == the GETs after")
    return lines


def write_replace(path: str, text: str) -> None:
    with open(path + ".x", "w", encoding="utf-8") as fh:
        fh.write(text)
    os.replace(path + ".x", path)


def read(path: str) -> str:
    try:
        with open(path, encoding="utf-8") as fh:
            return fh.read().strip()
    except OSError:
        return "?"


def keyboard_window() -> subprocess.Popen:
    """A stand-in for Keyboard.qml's window: an X window with its title."""
    code = ("import gi; gi.require_version('Gtk', '3.0'); from gi.repository import Gtk\n"
            f"w = Gtk.Window(title={KEYBOARD_TITLE!r}); w.show_all(); Gtk.main()\n")
    return subprocess.Popen(["python3", "-c", code], env=dict(os.environ, DISPLAY=":99", NO_AT_BRIDGE="1"),
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def xprop(*args: str) -> str:
    return subprocess.run(["xprop", "-display", ":99", *args], capture_output=True, text=True).stdout.strip()


def spies() -> int:
    n = 0
    for pid in filter(str.isdigit, os.listdir("/proc")):
        try:
            with open(f"/proc/{pid}/cmdline", "rb") as fh:
                if b"-spy" in fh.read().split(b"\0"):
                    n += 1
        except OSError:
            pass
    return n


def rss_kb(pid: int) -> int:
    with open(f"/proc/{pid}/status") as fh:
        return next(int(l.split()[1]) for l in fh if l.startswith("VmRSS:"))


def glide_expected(home: str) -> tuple[list, list]:
    """barry_glide's own answers for English's pack: /suggest "helo" and a
    glide through h-e-l-l-o's keys."""
    sys.path.insert(0, "/t/lib")
    import barry_glide
    folder = "/usr/share/barry_launcher/keyboards/en"
    with open(f"{folder}/keyboard.json", encoding="utf-8") as fh:
        pack = json.load(fh)
    decoder = barry_glide.Decoder(f"{folder}/{pack['words']}", True)
    rows = [[k for k in r if isinstance(k, str)] for r in pack["rows"]]
    corrector = barry_glide.Corrector(decoder, f"{home}/.local/share/barry_launcher/personal-words.txt", rows, {})
    suggest = corrector.suggest("helo")
    keys, path, size = glide_keys()
    glide = decoder.decode(path, keys, size, fold={})
    return suggest, glide


def glide_keys() -> tuple[dict, list, float]:
    size = 40.0
    keys = {}
    for y, (row, off) in enumerate((("qwertyuiop", 0.0), ("asdfghjkl", 0.5), ("zxcvbnm", 1.5))):
        for x, c in enumerate(row):
            keys[c] = [(x + off) * size + size / 2, y * size + size / 2]
    path = []
    for a, b in zip("helo", "elo"):
        (ax, ay), (bx, by) = keys[a], keys[b]
        path += [[ax + (bx - ax) * t / 8, ay + (by - ay) * t / 8] for t in range(8)]
    path.append(keys["o"])
    return keys, path, size


def glide_lazy(proc: subprocess.Popen, home: str) -> None:
    pid = proc.pid
    log_has = lambda text: text in shelld_log()
    start_kb = rss_kb(pid)
    print(f"  RSS at idle after start: {start_kb / 1024:.1f} MB", flush=True)
    check(not log_has("keyboard en:"), "no dictionary loaded at start")

    personal = f"{home}/.local/share/barry_launcher/personal-words.txt"
    r = post("/type", {"keep": "Zorbleflax", "lang": "en"})
    check(r.get("ok") is True and read(personal).split() == ["zorbleflax"],
          f"/type keep writes the personal words file ({r})")
    post("/type", {"keep": "zorbleflax", "lang": "en"})
    check(read(personal).split() == ["zorbleflax"], "a word kept twice is written once")
    check(not log_has("keyboard en:"), "a keep loads no dictionary")

    want_suggest, want_glide = glide_expected(home)
    t0 = time.monotonic()
    r = post("/suggest", {"prefix": "helo", "lang": "en"})
    first_ms = (time.monotonic() - t0) * 1000
    check(r.get("words") == want_suggest and bool(want_suggest),
          f"first /suggest loads English's and answers as barry_glide: {r.get('words')} ({first_ms:.0f} ms)")
    check(log_has("keyboard en:"), "its log line on first use")
    t0 = time.monotonic()
    r = post("/suggest", {"prefix": "helo", "lang": "en"})
    print(f"  /suggest again: {(time.monotonic() - t0) * 1000:.0f} ms", flush=True)
    keys, path, size = glide_keys()
    r = post("/glide", {"path": path, "keys": keys, "size": size, "lang": "en"})
    check(r.get("words") == want_glide and "hello" in r.get("words", []),
          f"/glide answers as barry_glide: {r.get('words')}")
    check(post("/suggest", {"prefix": "x", "lang": "nope"}).get("words") == [], "an unknown pack: no words")
    loaded_kb = rss_kb(pid)
    print(f"  RSS with English's dictionary: {loaded_kb / 1024:.1f} MB", flush=True)

    # Calls put the drop off; GLIDE_IDLE_S after the last, it goes.
    time.sleep(GLIDE_IDLE_S * 0.7)
    post("/suggest", {"prefix": "hel", "lang": "en"})
    time.sleep(GLIDE_IDLE_S * 0.7)
    check(not log_has("dictionaries dropped"), "a call meanwhile puts the drop off")
    check(wait_for(lambda: log_has("dictionaries dropped"), GLIDE_IDLE_S + 2),
          f"dropped {GLIDE_IDLE_S:.0f} s after the last call")
    time.sleep(0.5)
    dropped_kb = rss_kb(pid)
    print(f"  RSS after the drop: {dropped_kb / 1024:.1f} MB", flush=True)
    r = post("/suggest", {"prefix": "helo", "lang": "en"})
    check(r.get("words") == want_suggest, f"/suggest after the drop loads it again: {r.get('words')}")
    check(shelld_log().count("keyboard en: ") - shelld_log().count("keyboard en: cannot") == 2
          and shelld_log().count("dictionaries dropped") == 1, "loaded twice, dropped once in all")
    with open(personal, "a", encoding="utf-8") as fh:
        fh.write("helo\n")  # Barry.Input keeps a word in the same file
    time.sleep(0.05)
    r = post("/suggest", {"prefix": "helo", "lang": "en"})
    print(f"  /suggest helo once kept beside shelld: {r.get('words')}", flush=True)
    print(f"RSS new: start {start_kb / 1024:.1f} MB, English's loaded {loaded_kb / 1024:.1f} MB, "
          f"dropped {dropped_kb / 1024:.1f} MB", flush=True)


def functional(new: str) -> None:
    proc, home = start_shelld(new, "new")
    try:
        time.sleep(SETTLE_S)
        glide_lazy(proc, home)
        # The keyboard window turns up after shelld: marked once gamescope's
        # window list changes.
        check(spies() == 1, "one xprop -spy waits for the keyboard window")
        kb = keyboard_window()
        time.sleep(1.5)
        check(xprop("-name", KEYBOARD_TITLE, "STEAM_OVERLAY").endswith("= 1") is False,
              "not marked before the window list changes")
        xprop("-root", "-f", "GAMESCOPE_FOCUSABLE_WINDOWS", "32c", "-set", "GAMESCOPE_FOCUSABLE_WINDOWS", "1")
        time.sleep(1.0)
        check(xprop("-name", KEYBOARD_TITLE, "STEAM_OVERLAY").endswith("= 1"),
              "keyboard window marked STEAM_OVERLAY=1 on the window list's change")
        check(spies() == 0, "xprop -spy gone once marked")

        a = Client()
        snap = a.read(1.0)
        now = gets()
        check([t for t, _ in snap] == list(TOPICS), f"snapshot order {[t for t, _ in snap]}")
        check(all(now[t] == d for t, d in snap), "snapshot data == GETs, byte for byte")
        print("  snapshot:", *(f"{t}={d.decode()[:160]}" for t, d in snap), sep="\n    ")

        post("/barry", {"open": True, "why": "test"})
        expect(a, "POST /barry open", {"home", "keyd"})
        check(read(FLAG) == "1", "BARRY_FLAG written 1")
        write_replace(FLAG, "0")  # the AYN button (sm8550-thor-backlightd)
        expect(a, "AYN flag 0 (file monitor) closes", {"home", "keyd"})
        write_replace(FLAG, "1")
        expect(a, "AYN flag 1 opens", {"home", "keyd"})
        post("/barry", {"open": False, "why": "test"})
        expect(a, "POST /barry close", {"home", "keyd"})

        post("/keyboard", {"visible": True})
        expect(a, "POST /keyboard visible", {"keyboard", "keyd"})
        a.send({"keyboard_height": 320})
        expect(a, "client keyboard_height 320", {"keyd"})
        check(xprop("-root", "GAMESCOPE_FOCUS_BOTTOM_INSET").endswith("= 320"), "inset 320 set as it shows")
        a.send({"keyboard_height": 320})
        expect(a, "same keyboard_height again", set())
        get("/keyboard?h=330")
        expect(a, "GET /keyboard?h=330", {"keyd"})
        post("/keyboard", {"visible": False})
        expect(a, "POST /keyboard hidden", {"keyboard", "keyd"})
        post("/keyd", {"do": "top", "why": "test"})
        expect(a, "POST /keyd top", {"top-keyboard", "keyd"})

        post("/launch", {"app": "trackpad"})
        expect(a, "launch trackpad (a tool: home unchanged)", {"apps", "keyd"})
        check(read(TOP_INPUT) == "1", "top-input flag 1")
        post("/close", {"app": "trackpad"})
        expect(a, "trackpad closed (pidfd)", {"apps", "keyd"})
        check(read(TOP_INPUT) == "0", "top-input flag 0")

        slide_away(a, "trackpad", "answer")
        slide_away(a, "trackpad", "silent")
        slide_away(a, "trackpad", "old")
        slide_away(a, "deck", "answer")
        os.remove("/t/tool-mode")

        post("/launch", {"app": DINO})
        lines = expect(a, "launch Dino, its cover after 0.7 s", {"home", "apps", "keyboard"}, 1.5)
        cover = [json.loads(d) for t, d in lines if t == "keyboard"]
        check(bool(cover) and cover[0]["curtain"] == "Dino", f"cover says Dino ({cover})")
        os.kill(dino_pid(), signal.SIGKILL)  # it ends by itself (a crash)
        expect(a, "Dino killed: gone, cover off", {"home", "apps", "keyboard"})

        post("/layout", {"order": [DINO], "hidden": [DINO]})
        expect(a, "POST /layout hides Dino", {"apps"})
        layout = f"{home}/.config/barry_launcher/home.json"
        write_replace(layout, json.dumps({"order": [], "hidden": []}))
        expect(a, "home.json written beside shelld (file monitor)", {"apps"})

        # barry-app installs and removes by itself when shelld does not answer.
        app_dir = "/t/test-app"
        shutil.rmtree(app_dir, ignore_errors=True)
        shutil.copytree("/usr/share/barry_launcher/apps/dino", app_dir)
        with open(f"{app_dir}/barry-app.json", encoding="utf-8") as fh:
            m = json.load(fh)
        m.update(id="io.github.test.app", name="Test")
        with open(f"{app_dir}/barry-app.json", "w", encoding="utf-8") as fh:
            json.dump(m, fh)
        lib = ["python3", "-c", "import sys; sys.path.insert(0, '/t/lib'); import barry_apps; " + "{}"]
        run = lambda code: subprocess.run(lib[:2] + [lib[2].format(code)], env=env_for(home), check=True)
        run(f"barry_apps.install({app_dir!r})")
        lines = expect(a, "user app installed beside shelld (folder monitor)", {"apps"})
        check(any(b"io.github.test.app" in d for _, d in lines), "the new app is in apps")
        run("barry_apps.remove('io.github.test.app')")
        expect(a, "user app removed beside shelld", {"apps"})

        background(a, run)

        b = Client()
        snap = b.read(1.0)
        now = gets()
        check([t for t, _ in snap] == list(TOPICS) and all(now[t] == d for t, d in snap),
              "second client's snapshot == GETs")
        a.read(0.5)

        idle = a.read(IDLE_S) + b.read(0.1)
        check(idle == [], f"no lines while idle for {IDLE_S:.0f} s (got {len(idle)})")
        kb.kill()
    finally:
        stop_shelld(proc)


def without_frozen(topic: str, body: bytes) -> bytes:
    """GET /apps without each app's "frozen" (added last, after the event
    stream), as the old shelld answered."""
    if topic != "apps":
        return body
    apps = json.loads(body)
    for v in apps.values():
        v.pop("frozen", None)
    return json.dumps(apps).encode()


def jget(path: str) -> dict:
    return json.loads(get(path))


def app_pid(app: str) -> int:
    """A running app's process (the stub firefox with its profile, the
    qml6 stub with a user app's id); 0 if none."""
    for pid in filter(str.isdigit, os.listdir("/proc")):
        try:
            with open(f"/proc/{pid}/cmdline", "rb") as fh:
                args = fh.read().split(b"\0")
            with open(f"/proc/{pid}/stat") as fh:
                if fh.read().rsplit(")", 1)[1].split()[0] == "Z":
                    continue
        except OSError:
            continue
        if any(a.endswith(b"/firefox") for a in args) and any(a.endswith(b"/" + app.encode()) for a in args):
            return int(pid)
        if any(a.endswith(b"/qml6") for a in args) and app.encode() in args:
            return int(pid)
    return 0


def cgroup_of(pid: int) -> str:
    with open(f"/proc/{pid}/cgroup", encoding="utf-8") as fh:
        return "/sys/fs/cgroup" + next(l[3:].strip() for l in fh if l.startswith("0::"))


def cg_frozen(cg: str) -> bool:
    with open(f"{cg}/cgroup.events", encoding="utf-8") as fh:
        return "frozen 1" in fh.read().split("\n")


def shelld_log() -> str:
    with open("/t/shelld-new.log", encoding="utf-8") as fh:
        return fh.read()


def streams(pid: int, corked: bool | None) -> None:
    """The stub pactl's sound streams (one of pid's, or none), and an event."""
    data = [] if corked is None else [{"index": 7, "client": "3", "corked": corked,
                                       "properties": {"application.process.id": str(pid)}}]
    write_replace(PACTL_STREAMS, json.dumps(data))
    with open(PACTL_EVENTS, "a", encoding="utf-8") as fh:
        fh.write(f"Event '{'remove' if corked is None else 'change'}' on sink-input #7\n")


def to_background(a: Client, app: str, what: str) -> float:
    """The AYN button over the app in front, twice: Barry opens over it,
    then closes to the art, the app in the background. When it left the
    screen (Barry came over it), monotonic."""
    left = time.monotonic()
    write_replace(FLAG, "1")
    a.read(0.6)
    h = jget("/home")
    check(h["open"] and h["over"], f"{what}: AYN opens Barry over {app} ({h})")
    write_replace(FLAG, "0")
    lines = a.read(0.6)
    homes = [json.loads(d) for t, d in lines if t == "home"]
    check(bool(homes) and homes[-1]["open"] is False and homes[-1]["app"] is False and homes[-1]["over"] is False,
          f"{what}: AYN closes to the art: home open, app and over false ({homes})")
    check(jget("/apps")[app]["running"], f"{what}: {app} still runs")
    return left


def quit_thawed(app: str) -> bool:
    """In shelld's log: app was last thawed to be closed, and was not frozen
    again before it exited."""
    log = shelld_log()
    i = log.rfind(f"{app} thawed (closing)")
    j = log.find(f"{app} exited", i)
    return i >= 0 and j > i and f"{app} frozen" not in log[i:j]


def background(a: Client, run) -> None:
    """Apps in the background (see the top), with user QML apps (the qml6
    stub; Firefox's opening asks AT-SPI, which this sandbox has not)."""
    with open("/usr/share/barry_launcher/apps/dino/barry-app.json", encoding="utf-8") as fh:
        base = json.load(fh)
    for app_id, keep in ((BG, False), (SLEEPY, False), (AWAKE, True)):
        d = f"/t/{app_id}"
        shutil.rmtree(d, ignore_errors=True)
        shutil.copytree("/usr/share/barry_launcher/apps/dino", d)
        with open(f"{d}/barry-app.json", "w", encoding="utf-8") as fh:
            json.dump(dict(base, id=app_id, name=app_id.split(".")[-1], keepAwake=keep), fh)
        run(f"barry_apps.install({d!r})")
    a.read(1.0)
    check("apps start in scopes of their own" in shelld_log(), "shelld's scope probe worked (stub systemd-run)")
    post("/launch", {"app": BG})
    a.read(1.0)
    pid = app_pid(BG)
    cg = cgroup_of(pid) if pid else ""
    check(os.path.basename(cg).startswith(f"barry-app-{BG}-") and cg.endswith(".scope"),
          f"the app in a scope of its own, same pid as started ({cg})")
    check(jget("/home")["app"] is True, "the app in front: home app true")

    left = to_background(a, BG, "AYN")
    check(not jget("/apps")[BG]["frozen"] and not cg_frozen(cg), "not frozen at once")
    lines = a.read(left + FREEZE_S - 0.4 - time.monotonic())
    check(not cg_frozen(cg), f"not frozen before FREEZE_S ({FREEZE_S} s) off the screen")
    lines += a.read(left + FREEZE_S + 0.6 - time.monotonic())
    check(cg_frozen(cg) and jget("/apps")[BG]["frozen"] is True,
          "frozen FREEZE_S after it left the screen (cgroup.events frozen 1, apps frozen true)")
    check(any(t == "apps" and json.loads(d)[BG]["frozen"] for t, d in lines), "an apps line says frozen")

    post("/launch", {"app": BG})
    check(not cg_frozen(cg) and jget("/apps")[BG]["frozen"] is False,
          "tile tap: thawed by the time /launch answers")
    a.read(0.5)
    check(jget("/home")["app"] is True and app_pid(BG) == pid, "back in front, the same process")

    # The standby timeout (Shell.qml: POST /barry closed) over an app.
    write_replace(FLAG, "1")
    a.read(0.6)
    post("/barry", {"open": False, "why": "untouched for 15 s"})
    a.read(0.6)
    h = jget("/home")
    check(not h["open"] and h["app"] is False and read(FLAG) == "0" and jget("/apps")[BG]["running"],
          f"standby with an app behind: to the art, the app running on ({h})")

    # A side page over the app, swiped away: back to it.
    post("/launch", {"app": BG})
    a.read(0.5)
    post("/barry", {"open": True, "page": 1, "why": "swipe in from the left edge"})
    a.read(2.5)  # no picture comes (stub gamescopectl): shelld waits for one
    h = jget("/home")
    check(h["open"] and h["page"] == 1 and h["over"], f"side page open over the app ({h})")
    post("/barry", {"open": False, "why": "side panel swiped away"})
    a.read(0.6)
    check(jget("/home")["app"] is True, "side page swiped away: back to the app")

    # Sound: a stream of its own, not corked, keeps it awake.
    streams(pid, False)
    a.read(0.6)
    to_background(a, BG, "playing")
    a.read(FREEZE_S + 1.0)
    check(not cg_frozen(cg) and not jget("/apps")[BG]["frozen"], "playing sound: not frozen past FREEZE_S")
    check(f"{BG} plays sound: kept awake" in shelld_log(), "shelld saw it play (pactl subscribe)")
    streams(pid, True)
    a.read(FREEZE_S - 0.6)
    check(not cg_frozen(cg), "corked: FREEZE_S from then, not at once")
    a.read(1.4)
    check(cg_frozen(cg), "corked: frozen FREEZE_S after the sound stopped")
    streams(pid, None)

    # CLOSE_S after it left the screen (the sound's end): closed.
    a.read(CLOSE_S - FREEZE_S)
    check(wait_for(lambda: not running_pid(pid), 2.0) and not jget("/apps")[BG]["running"],
          "frozen CLOSE_S: closed (thawed first: its SIGTERM ended it)")
    check(quit_thawed(BG) and "off the screen for" in shelld_log(),
          "logged: thawed, then closed; not frozen again as it quit")

    # Closed by hand while frozen.
    post("/launch", {"app": BG})
    a.read(1.0)
    pid = app_pid(BG)
    cg = cgroup_of(pid)
    to_background(a, BG, "closed by hand")
    a.read(FREEZE_S + 0.8)
    check(cg_frozen(cg), "frozen again")
    post("/close", {"app": BG})
    check(wait_for(lambda: not running_pid(pid), 1.0), "POST /close of a frozen app: it ends at once")
    a.read(0.5)
    check(quit_thawed(BG), "thawed, then closed; not frozen again as it quit")

    # keepAwake, and memory pressure.
    for app in (SLEEPY, BG, AWAKE):
        post("/launch", {"app": app})
        a.read(1.2)
    pids = {app: app_pid(app) for app in (SLEEPY, BG, AWAKE)}
    check(all(pids.values()), f"three apps running ({pids})")
    to_background(a, AWAKE, "keepAwake")
    a.read(FREEZE_S + 1.0)
    apps = jget("/apps")
    check(apps[SLEEPY]["frozen"] and apps[BG]["frozen"], "the other two frozen")
    check(not apps[AWAKE]["frozen"] and not cg_frozen(cgroup_of(pids[AWAKE])),
          '"keepAwake": true: never frozen')
    for n, gone in ((1, SLEEPY), (2, BG)):
        with open(PSI, "w") as fh:
            fh.write("1\n")
        check(wait_for(lambda: not running_pid(pids[gone]), 1.5),
              f"memory pressure {n}: {gone} closed (frozen, least recently on the screen)")
        a.read(0.3)
        check(quit_thawed(gone), f"memory pressure {n}: thawed first, not frozen again as it quit")
    check(running_pid(pids[AWAKE]), "memory pressure: a kept-awake app stays")
    with open(PSI, "w") as fh:
        fh.write("1\n")
    a.read(0.5)
    check("memory pressure: no frozen app to close" in shelld_log(), "memory pressure 3: nothing frozen, logged")
    post("/close", {"app": AWAKE})
    wait_for(lambda: not running_pid(pids[AWAKE]), 2.0)
    for app_id in (BG, SLEEPY, AWAKE):
        run(f"barry_apps.remove({app_id!r})")
    a.read(1.0)
    check(not any(v["running"] for k, v in jget("/apps").items() if k not in ("trackpad", "deck", "keyboard")),
          "no app left running")
    for line in shelld_log().splitlines():
        if any(w in line for w in ("frozen", "thawed", "closing", "memory pressure", "sound", "scope", "Barry closes")):
            print("   ", line)


def tool_pid(app: str) -> int:
    """The qml6 stub started as app (the trackpad, the Deck controls)."""
    mark = b"trackpad" if app == "trackpad" else b"/DeckApp.qml"
    for pid in filter(str.isdigit, os.listdir("/proc")):
        try:
            with open(f"/proc/{pid}/cmdline", "rb") as fh:
                args = fh.read().split(b"\0")
        except OSError:
            continue
        if any(a.endswith(b"/qml6") for a in args) and any(a == mark or a.endswith(mark) for a in args):
            return int(pid)
    raise RuntimeError(f"no {app} stub")


def tool_log(pid: int) -> dict[str, float]:
    """What qml6_stub.py did, as it logged it: {"asked": t, "left": t, ...}."""
    try:
        with open(f"/t/tool-{pid}.log", encoding="utf-8") as fh:
            return {w: float(t) for w, t in (line.split() for line in fh)}
    except OSError:
        return {}


def wait_for(cond, seconds: float) -> bool:
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        if cond():
            return True
        time.sleep(0.02)
    return cond()


def slide_away(a: Client, app: str, mode: str) -> None:
    """Barry opens over a tool (POST /barry: the swipe up from the bottom
    edge and the AYN button open it the same way), the tool playing mode
    (qml6_stub.py: answer, silent, or old: says nothing)."""
    what = f"Barry over {app} ({mode})"
    write_replace("/t/tool-mode", mode)
    post("/launch", {"app": app})
    a.read(0.5)
    pid = tool_pid(app)
    if mode != "old":
        wait_for(lambda: "ready" in tool_log(pid), 5)
        time.sleep(0.2)  # its {"tool": true} taken in
    t0 = time.monotonic()
    post("/barry", {"open": True, "why": "test"})
    lines = a.read_timed(1.5)
    wait_for(lambda: "term" in tool_log(pid), 2)
    log = tool_log(pid)
    tools = [(t, json.loads(d)) for t, topic, d in lines if topic == "tools"]
    homes = [t for t, topic, d in lines if topic == "home" and json.loads(d)["open"]]
    check(len(homes) == 1, f"{what}: one home line, Barry open (got {[t for _, t, _ in lines]})")
    home_t = homes[0] if homes else float("inf")
    print(f"  {what}: home line {home_t - t0:.3f} s after POST /barry; tool log "
          + " ".join(f"{w}={t - t0:+.3f}" for w, t in sorted(log.items(), key=lambda x: x[1])))
    if mode == "old":
        check(tools == [], f"{what}: not asked (tools unchanged)")
        check(home_t - t0 < TOOL_SLIDE_S, f"{what}: Barry at once ({home_t - t0:.3f} s)")
    else:
        check(len(tools) == 2 and tools[0][1]["leaving"] == {app: pid} and tools[1][1]["leaving"] == {}
              and tools[1][1]["serial"] == tools[0][1]["serial"],
              f"{what}: tools says {{{app}: {pid}}} leaving, then nothing ({[d for _, d in tools]})")
        check(bool(tools) and tools[0][0] < home_t, f"{what}: asked before Barry's home line")
        check("asked" in log, f"{what}: the tool heard it was asked")
        if mode == "answer":
            check("left" in log and log["left"] <= home_t, f"{what}: Barry's home line after its tool_left")
            waited = home_t - log.get("asked", t0)
            check(TOOL_SLIDE_S - 0.02 <= waited < TOOL_LEAVE_S,
                  f"{what}: Barry waited for the slide, not the cap ({waited:.3f} s)")
        else:
            check("left" not in log and home_t - t0 >= TOOL_LEAVE_S - 0.02,
                  f"{what}: Barry after the {TOOL_LEAVE_S} s cap ({home_t - t0:.3f} s)")
    check("term" in log and log["term"] >= home_t,
          f"{what}: ended by shelld (SIGTERM) after Barry's home line")
    check(json.loads(get("/tools"))["leaving"] == {}, f"{what}: GET /tools leaving nothing after")
    post("/barry", {"open": False, "why": "test"})
    a.read(0.5)
    check(wait_for(lambda: not running_pid(pid), 2), f"{what}: the tool's process is gone")


def running_pid(pid: int) -> bool:
    try:
        with open(f"/proc/{pid}/stat") as fh:
            return fh.read().rsplit(")", 1)[1].split()[0] != "Z"
    except OSError:
        return False


def dino_pid() -> int:
    for pid in filter(str.isdigit, os.listdir("/proc")):
        try:
            with open(f"/proc/{pid}/cmdline", "rb") as fh:
                args = fh.read().split(b"\0")
        except OSError:
            continue
        if any(a.endswith(b"/qml6") for a in args) and DINO.encode() in b" ".join(args):
            return int(pid)
    raise RuntimeError("no Dino stub")


def cpu(pid: int) -> float:
    with open(f"/proc/{pid}/stat") as fh:
        f = fh.read().rsplit(")", 1)[1].split()
    return sum(int(x) for x in f[11:15]) / os.sysconf("SC_CLK_TCK")


def pollers(stop: threading.Event) -> list[threading.Thread]:
    """Shell.qml and Keyboard.qml as they were: one kept-alive connection
    each, as Qt's XMLHttpRequest keeps one."""
    def loop(paths: list[tuple[str, float]]) -> None:
        conn = http.client.HTTPConnection("127.0.0.1", PORT, timeout=5)
        due = {p: 0.0 for p, _ in paths}
        while not stop.is_set():
            now = time.monotonic()
            for p, every in paths:
                if now >= due[p]:
                    get(p, conn)
                    due[p] = now + every
            stop.wait(max(0.0, min(due.values()) - time.monotonic()))
    threads = [threading.Thread(target=loop, args=([("/home", 0.05), ("/apps", 1.0)],), daemon=True),
               threading.Thread(target=loop, args=([("/keyboard?h=300", 0.1)],), daemon=True)]
    for t in threads:
        t.start()
    return threads


def measure(path: str, tag: str, poll: bool, socket_client: bool) -> tuple[float, dict]:
    proc, _ = start_shelld(path, tag)
    try:
        time.sleep(SETTLE_S)
        start_gets = gets()
        stop = threading.Event()
        threads = pollers(stop) if poll else []
        client = Client() if socket_client else None
        if client:
            client.read(1.0)
        time.sleep(2)
        c0, t0 = cpu(proc.pid), time.monotonic()
        lines = []
        end = t0 + CPU_S
        while time.monotonic() < end:
            if client:
                lines += client.read(end - time.monotonic())
            else:
                time.sleep(end - time.monotonic())
        used = (cpu(proc.pid) - c0) / (time.monotonic() - t0) * 60 * 1000
        stop.set()
        for t in threads:
            t.join()
        print(f"CPU {tag}: {used:.0f} ms/min" + (f" ({len(lines)} event lines)" if client else ""), flush=True)
        return used, start_gets
    finally:
        stop_shelld(proc)


def main() -> int:
    new, old = sys.argv[1], sys.argv[2]
    subprocess.Popen(["Xvfb", ":99", "-nolisten", "tcp", "-noreset"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    while subprocess.run(["xprop", "-display", ":99", "-root", "-len", "0"], capture_output=True).returncode:
        time.sleep(0.1)  # Xvfb not answering yet
    os.makedirs("/run/sm8550-thor", exist_ok=True)
    # gamescope's root property, there before shelld starts (xprop -spy
    # stops at once for an atom no one has made).
    xprop("-root", "-f", "GAMESCOPE_FOCUSABLE_WINDOWS", "32c", "-set", "GAMESCOPE_FOCUSABLE_WINDOWS", "0")
    os.makedirs(RUN, mode=0o700, exist_ok=True)
    os.mkfifo(PSI)
    open(PACTL_EVENTS, "w").close()
    if "--cpu-only" not in sys.argv:
        try:
            functional(new)
        except Exception:
            with open("/t/shelld-new.log", encoding="utf-8") as fh:
                print("--- shelld-new.log (tail)", *fh.read().splitlines()[-60:], sep="\n")
            raise
    if os.path.exists(BEFORE):
        proc, _ = start_shelld(BEFORE, "before")
        try:
            time.sleep(SETTLE_S)
            print(f"RSS before (dictionary at start): {rss_kb(proc.pid) / 1024:.1f} MB at idle after start", flush=True)
        finally:
            stop_shelld(proc)
        proc, _ = start_shelld(new, "new-rss")
        try:
            time.sleep(SETTLE_S)
            print(f"RSS new (no dictionary at start): {rss_kb(proc.pid) / 1024:.1f} MB at idle after start", flush=True)
        finally:
            stop_shelld(proc)
    old_ms, old_gets = measure(old, "old", poll=True, socket_client=False)
    new_ms, new_gets = measure(new, "new-socket", poll=False, socket_client=True)
    compat_ms, _ = measure(new, "new-polled", poll=True, socket_client=False)
    for t in OLD_TOPICS:
        check(old_gets[t] == without_frozen(t, new_gets[t]),
              f"GET /{t} at start: old == new, byte for byte" + (" (but apps' added frozen)" if t == "apps" else ""))
    print(f"idle CPU: old polled {old_ms:.0f} ms/min -> new + socket client {new_ms:.0f} ms/min "
          f"(new, polled the old way: {compat_ms:.0f})")
    if failures:
        for tag in ("new", "old"):
            with open(f"/t/shelld-{tag}.log", encoding="utf-8") as fh:
                print(f"--- shelld-{tag}.log (tail)", *fh.read().splitlines()[-60:], sep="\n")
    print("ALL PASS" if not failures else f"{len(failures)} FAILED: {failures}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
