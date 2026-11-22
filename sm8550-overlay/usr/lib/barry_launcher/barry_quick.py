"""barry_quick: the system side of Barry Launcher's quick settings, served by
barry_launcher_statsd on /quick (its docstring has the JSON).

  Wi-Fi       NetworkManager's radio switch (nmcli radio wifi), as Steam's
  Bluetooth   the adapter's Powered property on BlueZ's D-Bus (busctl), as
              Steam switches it
  Volume      "system": the default output's volume (wpctl), the one
              Steam's volume bar moves: on the AYN Thor that is "Top
              screen", the top screen's own output; "apps": the bottom
              screen's apps' volume, kept in APPS_STATE. barry_launcher_audio
              plays their streams past "Top screen", straight on the real
              output at that volume, so the two never move each other; it
              rereads APPS_STATE when told (SIGUSR1)
  Screenshot  the top screen (gamescopectl screenshot on the top screen's
              gamescope) to ~/Pictures/Screenshots
  Record      the top screen with everything playing (barry_capture, on the
              top gamescope's PipeWire stream) to ~/Videos
  Screen off  on a device without a Barry button (the Retroid Pocket Duo):
              the bottom screen asleep now, dark with its touchscreen on (a
              touch wakes it), through sm8550-thor-backlightd's SLEEP_FLAG

A bottom-screen app is any process under the gamescope Barry Launcher runs
in (barry_launcher_session, statsd too). A stream knows only the process id its app
gives, which inside a Flatpak sandbox (Signal, Discord) is the sandbox's own,
so a stream is an app's when its id and program match one of those
processes, by its id outside or inside its sandbox.

All of this runs as the seat user: NetworkManager and BlueZ allow the active
session's user, as they do for Steam.
"""
from __future__ import annotations

import json
import os
import re
import shutil
import signal
import subprocess
import threading
import time

HOME = os.path.expanduser("~")
APPS_STATE = os.path.join(os.environ.get("XDG_CONFIG_HOME", f"{HOME}/.config"), "barry_launcher/quick.json")
SHOTS = os.path.join(HOME, "Pictures/Screenshots")
VIDEOS = os.path.join(HOME, "Videos")
CAPTURE = "/usr/lib/barry_launcher/barry_capture"
SINK = "@DEFAULT_AUDIO_SINK@"
# gamescope's process name ("gamescope-wl" since it names its threads).
GAMESCOPE_NAMES = ("gamescope", "gamescope-wl")
RUNTIME = os.environ.get("XDG_RUNTIME_DIR", f"/run/user/{os.getuid()}")
MIN_FREE_TO_RECORD = 2 * 1024**3
AUDIO_UNIT = "barry_launcher_audio.service"
# "1": sm8550-thor-backlightd puts the bottom screen to sleep (it makes the
# file, group input, only where the device has no Barry button).
SLEEP_FLAG = "/run/sm8550-thor/sleep"


def run(*cmd: str, timeout: float = 4) -> subprocess.CompletedProcess | None:
    try:
        return subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    except (OSError, subprocess.TimeoutExpired):
        return None


def log(msg: str) -> None:
    print(f"quick: {msg}", flush=True)


# Wi-Fi and Bluetooth

def wifi() -> dict | None:
    r = run("nmcli", "-t", "-f", "WIFI-HW,WIFI", "radio")
    if not r or r.returncode != 0 or ":" not in r.stdout:
        return None
    hw, sw = r.stdout.strip().split(":", 1)
    if hw == "missing":
        return None
    return {"enabled": sw == "enabled"}


def set_wifi(on: bool) -> str | None:
    r = run("nmcli", "radio", "wifi", "on" if on else "off")
    return None if r and r.returncode == 0 else f"Wi-Fi: {(r.stderr.strip() if r else '') or 'nmcli failed'}"


BLUEZ = ("org.bluez", "org.bluez.Adapter1")


def bt_adapter() -> str | None:
    """The first Bluetooth adapter's object path on BlueZ's D-Bus (/org/bluez/hci0)."""
    r = run("busctl", "--system", "--list", "tree", BLUEZ[0])
    paths = re.findall(r"^(/org/bluez/hci\d+)$", r.stdout if r and r.returncode == 0 else "", re.M)
    return sorted(paths)[0] if paths else None


def bluetooth() -> dict | None:
    path = bt_adapter()
    if not path:
        return None
    r = run("busctl", "--system", "get-property", BLUEZ[0], path, BLUEZ[1], "Powered")
    if not r or r.returncode != 0:
        return None
    return {"enabled": r.stdout.strip() == "b true"}


def set_bluetooth(on: bool) -> str | None:
    path = bt_adapter()
    if not path:
        return "No Bluetooth adapter (is the bluetooth service running?)."
    want = "true" if on else "false"
    r = run("busctl", "--system", "set-property", BLUEZ[0], path, BLUEZ[1], "Powered", "b", want, timeout=8)
    if on and not (r and r.returncode == 0):
        # Turned off by its rfkill switch (Desktop Mode, airplane mode):
        # the seat user may lift it.
        run("rfkill", "unblock", "bluetooth")
        time.sleep(0.5)
        r = run("busctl", "--system", "set-property", BLUEZ[0], path, BLUEZ[1], "Powered", "b", want, timeout=8)
    if r and r.returncode == 0:
        return None
    why = (r.stderr.strip() or r.stdout.strip()) if r else "busctl failed"
    log(f"bluetooth {want}: {why}")
    return f"Bluetooth didn't switch {'on' if on else 'off'}: {why}"


# Volume

def system_volume() -> tuple[int, bool] | None:
    r = run("wpctl", "get-volume", SINK)
    m = re.match(r"Volume: ([0-9.]+)", r.stdout if r else "")
    if not m:
        return None
    return min(100, round(float(m.group(1)) * 100)), "[MUTED]" in r.stdout


def set_system_volume(pct: int) -> str | None:
    r = run("wpctl", "set-volume", SINK, f"{pct / 100:.2f}")
    if pct > 0:
        run("wpctl", "set-mute", SINK, "0")
    return None if r and r.returncode == 0 else "The volume didn't change."


def proc_tree() -> dict[int, int]:
    """pid -> parent pid, for every process we can see."""
    out = {}
    for name in os.listdir("/proc"):
        if not name.isdigit():
            continue
        try:
            with open(f"/proc/{name}/stat", encoding="utf-8") as fh:
                stat = fh.read()
            out[int(name)] = int(stat.rsplit(")", 1)[1].split()[1])
        except (OSError, ValueError, IndexError):
            pass
    return out


def comm(pid: int) -> str:
    try:
        with open(f"/proc/{pid}/comm", encoding="utf-8") as fh:
            return fh.read().strip()
    except OSError:
        return ""


def ns_pid(pid: int) -> int | None:
    """The process's id in its own pid namespace (a sandbox's), if other."""
    try:
        with open(f"/proc/{pid}/status", encoding="utf-8") as fh:
            for line in fh:
                if line.startswith("NSpid:"):
                    ids = line.split()[1:]
                    return int(ids[-1]) if len(ids) > 1 else None
    except (OSError, ValueError):
        pass
    return None


def bottom_gamescope(tree: dict[int, int], pid: int | None = None) -> int | None:
    """The gamescope `pid` (this process by default) runs in."""
    pid = os.getpid() if pid is None else pid
    while pid > 1:
        if comm(pid) in GAMESCOPE_NAMES:
            return pid
        pid = tree.get(pid, 1)
    return None


def barry_gamescope(tree: dict[int, int]) -> int | None:
    """Barry Launcher's gamescope, seen from outside it: the one
    barry_launcher_session runs in."""
    for pid in tree:
        try:
            with open(f"/proc/{pid}/cmdline", "rb") as fh:
                if b"/barry_launcher_session" not in fh.read():
                    continue
        except OSError:
            continue
        root = bottom_gamescope(tree, pid)
        if root is not None:
            return root
    return None


def bottom_apps(inside: bool = True) -> set[tuple[int, str]]:
    """(process id, program) for every bottom-screen process, by its id
    outside its sandbox and inside it. `inside`: this process runs in
    Barry's gamescope (statsd); else it is looked up."""
    tree = proc_tree()
    root = bottom_gamescope(tree) if inside else barry_gamescope(tree)
    if root is None:
        return set()
    children: dict[int, list[int]] = {}
    for pid, ppid in tree.items():
        children.setdefault(ppid, []).append(pid)
    out, todo = set(), list(children.get(root, []))
    while todo:
        pid = todo.pop()
        todo.extend(children.get(pid, []))
        name = comm(pid)
        out.add((pid, name))
        inner = ns_pid(pid)
        if inner is not None:
            out.add((inner, name))
    return out


def is_app_stream(props: dict, apps: set[tuple[int, str]]) -> bool:
    """A stream's (pactl's properties) is a bottom-screen app's."""
    try:
        pid = int(props.get("application.process.id", ""))
    except ValueError:
        return False
    # /proc's comm is the program's name cut to 15 characters.
    program = os.path.basename(props.get("application.process.binary", ""))[:15]
    return (pid, program) in apps or (not program and any(a[0] == pid for a in apps))


def apps_volume() -> int:
    """The bottom screen's apps' volume, 0-100 (100 when never set)."""
    try:
        with open(APPS_STATE, encoding="utf-8") as fh:
            v = json.load(fh).get("appsVolume", 100)
        return int(v) if isinstance(v, (int, float)) and 0 <= v <= 100 else 100
    except (OSError, ValueError, AttributeError):
        return 100


class AppsVolume:
    """The bottom screen's apps' volume: kept in APPS_STATE, which
    barry_launcher_audio applies to their streams."""

    def __init__(self) -> None:
        self.lock = threading.Lock()
        self.pct = apps_volume()

    def set(self, pct: int) -> None:
        with self.lock:
            self.pct = pct
            os.makedirs(os.path.dirname(APPS_STATE), exist_ok=True)
            tmp = APPS_STATE + ".tmp"
            with open(tmp, "w", encoding="utf-8") as fh:
                json.dump({"appsVolume": pct}, fh)
            os.replace(tmp, APPS_STATE)
        run("systemctl", "--user", "kill", "--signal=USR1", AUDIO_UNIT)


# Screenshot and recording: the top screen

def top_gamescope() -> tuple[int | None, str | None]:
    """The top screen's gamescope: its pid and its Wayland socket name."""
    tree = proc_tree()
    bottom = bottom_gamescope(tree)
    tops = [p for p in tree if p != bottom and comm(p) in GAMESCOPE_NAMES and os.stat(f"/proc/{p}").st_uid == os.getuid()]
    mine = os.environ.get("GAMESCOPE_WAYLAND_DISPLAY", "")
    sockets = sorted(n for n in os.listdir(RUNTIME) if re.fullmatch(r"gamescope-\d+", n) and n != mine) \
        if os.path.isdir(RUNTIME) else []
    return (tops[0] if len(tops) == 1 else None), (sockets[0] if sockets else None)


def stamp() -> str:
    return time.strftime("%Y-%m-%d_%H-%M-%S")


def screenshot() -> tuple[str | None, str | None]:
    """-> (file, error)"""
    _pid, sock = top_gamescope()
    if not sock:
        return None, "The top screen's gamescope isn't running."
    os.makedirs(SHOTS, exist_ok=True)
    path = os.path.join(SHOTS, f"Screenshot_{stamp()}.png")
    env = dict(os.environ, GAMESCOPE_WAYLAND_DISPLAY=sock)
    try:
        r = subprocess.run(["gamescopectl", "screenshot", path], env=env, capture_output=True,
                           text=True, timeout=8)
    except (OSError, subprocess.TimeoutExpired):
        r = None
    # gamescope writes the file itself, a moment after it says so.
    for _ in range(30):
        if os.path.exists(path) and os.path.getsize(path) > 0:
            return path, None
        time.sleep(0.1)
    return None, "No screenshot: " + ((r.stderr.strip() or r.stdout.strip()) if r else "gamescopectl failed")


def gamescope_node(pid: int) -> str | None:
    """The object.serial of the PipeWire video node gamescope `pid` makes."""
    r = run("pw-dump", timeout=6)
    try:
        objs = json.loads(r.stdout) if r and r.returncode == 0 else []
    except ValueError:
        return None
    clients = {o["id"] for o in objs if o.get("type") == "PipeWire:Interface:Client"
               and str(o.get("info", {}).get("props", {}).get("pipewire.sec.pid")) == str(pid)}
    for o in objs:
        props = o.get("info", {}).get("props", {})
        if (o.get("type") == "PipeWire:Interface:Node" and props.get("node.name") == "gamescope"
                and props.get("media.class") == "Video/Source" and props.get("client.id") in clients):
            return str(props.get("object.serial", o["id"]))
    return None


class Recorder:
    def __init__(self) -> None:
        self.lock = threading.Lock()
        self.proc: subprocess.Popen | None = None
        self.path = ""
        self.started = 0.0
        self.last = ""     # what became of the last recording, for the page

    def _reap(self) -> None:
        """A recording that ended by itself (the disk filled, the stream went)."""
        if self.proc and self.proc.poll() is not None:
            self._finished(self.proc.returncode)

    def _finished(self, code: int | None) -> None:
        name = os.path.basename(self.path)
        if code in (0, 3) and os.path.exists(self.path) and os.path.getsize(self.path) > 0:
            self.last = f"Saved {name} in Videos" + (" (the disk is nearly full)" if code == 3 else "")
        else:
            self.last = "The recording failed" + ("" if code is None else f" ({code})")
            try:
                if os.path.getsize(self.path) == 0:
                    os.remove(self.path)
            except OSError:
                pass
        log(f"recording ended ({code}): {self.last}")
        self.proc = None

    def state(self) -> dict:
        with self.lock:
            self._reap()
            return {"recording": self.proc is not None,
                    "seconds": int(time.time() - self.started) if self.proc else 0,
                    "last": self.last}

    def start(self) -> str | None:
        with self.lock:
            self._reap()
            if self.proc:
                return None
            if not os.access(CAPTURE, os.X_OK):
                return "barry_capture is missing."
            os.makedirs(VIDEOS, exist_ok=True)
            if shutil.disk_usage(VIDEOS).free < MIN_FREE_TO_RECORD:
                return "Less than 2 GB free: no room to record."
            pid, _sock = top_gamescope()
            node = gamescope_node(pid) if pid else None
            if not node:
                return "The top screen's video stream isn't there."
            self.path = os.path.join(VIDEOS, f"Recording_{stamp()}.mp4")
            self.proc = subprocess.Popen([CAPTURE, node, self.path], stdout=subprocess.DEVNULL,
                                         stderr=None, start_new_session=True)
            self.started = time.time()
            self.last = ""
            log(f"recording node {node} to {self.path}")
        # A capture that cannot start ends at once: say so now.
        time.sleep(0.6)
        with self.lock:
            self._reap()
            return None if self.proc else self.last

    def stop(self) -> None:
        with self.lock:
            proc = self.proc
            if not proc:
                return
            proc.send_signal(signal.SIGINT)
            try:
                code = proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
                code = proc.wait()
            self._finished(code)


# Screen off

def screen_off_here() -> bool:
    """No Barry button (barry_device's BARRY_BUTTON empty) and the daemon's
    flag is there."""
    return os.environ.get("BARRY_BUTTON", "AYN") == "" and os.path.exists(SLEEP_FLAG)


def screen_off() -> str | None:
    if not screen_off_here():
        return "Screen off isn't here: it is for devices without a Barry button, with sm8550-thor-backlightd running."
    try:
        with open(SLEEP_FLAG, "w", encoding="utf-8") as fh:
            fh.write("1")
    except OSError as err:
        return f"The bottom screen didn't turn off: {err}"
    log("screen off")
    return None


class Quick:
    def __init__(self) -> None:
        self.apps = AppsVolume()
        self.recorder = Recorder()

    def get(self) -> dict:
        vol = system_volume()
        return {
            "wifi": wifi(),
            "bluetooth": bluetooth(),
            "volume": {"system": vol[0], "muted": vol[1], "apps": self.apps.pct} if vol else None,
            "capture": self.recorder.state(),
            "screenOff": {} if screen_off_here() else None,
        }

    def set(self, body: dict) -> tuple[int, dict]:
        known = {"wifi", "bluetooth", "volume", "screenshot", "record", "screenOff"}
        if not body or set(body) - known:
            return 400, {"error": f"known: {sorted(known)}"}
        error, saved = None, None
        if "wifi" in body:
            if not isinstance(body["wifi"], bool):
                return 400, {"error": "wifi is true or false"}
            error = set_wifi(body["wifi"])
        if "bluetooth" in body:
            if not isinstance(body["bluetooth"], bool):
                return 400, {"error": "bluetooth is true or false"}
            error = set_bluetooth(body["bluetooth"]) or error
        if "volume" in body:
            v = body["volume"]
            pct = lambda x: isinstance(x, (int, float)) and not isinstance(x, bool) and 0 <= x <= 100
            if not isinstance(v, dict) or not v or set(v) - {"system", "apps"} or not all(pct(x) for x in v.values()):
                return 400, {"error": "volume is {system, apps}: 0-100"}
            if "system" in v:
                error = set_system_volume(int(v["system"])) or error
            if "apps" in v:
                self.apps.set(int(v["apps"]))
        if body.get("screenshot") is True:
            saved, err = screenshot()
            error = err or error
        if body.get("screenOff") is True:
            error = screen_off() or error
        if "record" in body:
            if not isinstance(body["record"], bool):
                return 400, {"error": "record is true or false"}
            if body["record"]:
                error = self.recorder.start() or error
            else:
                self.recorder.stop()
        out = self.get()
        if saved:
            out["saved"] = saved
        if error:
            out["error"] = error
        return 200, out
