#!/usr/bin/env python3
"""One case of the thor-backlight equivalence test, for one implementation
(inside run.sh's sandbox, root).

  scenario.py py|cpp CASE     writes /tmp/out/CASE.IMPL.json
  scenario.py --list          the cases

The panels are plain files (/sys/class/backlight is a tmpfs; /tmp/bl is the
same tmpfs, the real panel under the daemon's bind mount); the AYN button
and the bottom touchscreen are uinput devices; Game Mode and Plasma are
fake processes named gamescope (--steam) and kwin_wayland; runuser is a
stub that logs (and answers xprop). After each step the case waits for
things to settle and takes a snapshot: both panels, bl_power, the
touchscreen's inhibited, the flags and files, the mounts, the commands run
and the daemon's log (prefixes and event numbers normalized).

Cases named duo-* are the Retroid Pocket Duo's profile (the module only;
run.sh fakes its compatible): the panels the other way round (bottom
ae96000 max 3445, top ae94000 max 4095, Steam's), no AYN button, Barry's
Screen off flag. They have no Python to compare with: each snapshot carries
"expect", the checks it made, which compare.py reports.
"""
import fcntl
import json
import math
import os
import re
import shutil
import signal
import struct
import subprocess
import sys
import time

BL = "/tmp/bl"  # the panels' own files (also /sys/class/backlight)
BOTTOM_SYS = "/sys/class/backlight/ae94000.dsi.0"
CTL = "/tmp/ctl"
RUN = "/run/sm8550-thor"
VAR = "/var/lib/steamos-sm8550"
STATE = f"{VAR}/thor-bottom-screen"
SCREENS = f"{VAR}/thor-screen-brightness.json"
SETTINGS = "/home/steamos/.config/barry_launcher/settings.json"
LOG = f"{CTL}/daemon.log"
KEY_F24, BTN_TOUCH = 194, 330
EV_SYN, EV_KEY, EV_ABS = 0, 1, 3

UI_SET_EVBIT = 0x40045564
UI_SET_KEYBIT = 0x40045565
UI_SET_ABSBIT = 0x40045567
UI_DEV_CREATE = 0x5501
UI_DEV_DESTROY = 0x5502


def rd(path, default=None):
    try:
        with open(path, encoding="utf-8", errors="replace") as fh:
            return fh.read().strip()
    except OSError:
        return default


def wr(path, value):
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(value)


def replace(path, value):
    """As the plugin and Barry write: a temp file renamed over."""
    wr(path + ".new", value)
    os.replace(path + ".new", path)


class Dev:
    def __init__(self, name, keys, absaxes=()):
        self.fd = os.open("/dev/uinput", os.O_WRONLY | os.O_NONBLOCK)
        fcntl.ioctl(self.fd, UI_SET_EVBIT, EV_KEY)
        for k in keys:
            fcntl.ioctl(self.fd, UI_SET_KEYBIT, k)
        absmax = [0] * 64
        if absaxes:
            fcntl.ioctl(self.fd, UI_SET_EVBIT, EV_ABS)
            for a in absaxes:
                fcntl.ioctl(self.fd, UI_SET_ABSBIT, a)
                absmax[a] = 1000
        os.write(self.fd, struct.pack("80sHHHHi", name.encode(), 0x06, 1, 1, 1, 0)
                 + struct.pack("64i", *absmax) + bytes(3 * 64 * 4))
        fcntl.ioctl(self.fd, UI_DEV_CREATE)
        self.name = name
        time.sleep(0.3)  # udev-less: the node is there at once, the uevent too

    def emit(self, etype, code, value):
        t = time.time()
        os.write(self.fd, struct.pack("llHHi", int(t), int((t % 1) * 1e6), etype, code, value))

    def key(self, value):
        self.emit(EV_KEY, KEY_F24, value)
        self.emit(EV_SYN, 0, 0)

    def touch(self):
        self.emit(EV_ABS, 0, 500)
        self.emit(EV_ABS, 1, 500)
        self.emit(EV_KEY, BTN_TOUCH, 1)
        self.emit(EV_SYN, 0, 0)
        time.sleep(0.05)
        self.emit(EV_KEY, BTN_TOUCH, 0)
        self.emit(EV_SYN, 0, 0)

    def destroy(self):
        fcntl.ioctl(self.fd, UI_DEV_DESTROY)
        os.close(self.fd)

    def inhibited(self):
        for d in sorted(os.listdir("/sys/class/input")):
            if d.startswith("input") and rd(f"/sys/class/input/{d}/name") == self.name:
                return rd(f"/sys/class/input/{d}/inhibited")
        return None


class Case:
    def __init__(self, impl):
        self.impl = impl
        self.snaps = []
        self.log_seen = 0
        self.cmds_seen = 0
        self.procs = []
        self.daemon = None
        self.key = self.touch = None
        self.steam_fd = None
        # The panels' backlights (the Duo's: duo_panels).
        self.bottom_name, self.top_name = "ae94000.dsi.0", "ae96000.dsi.0"
        self.expect = []  # [what, ok] since the last snapshot (duo-* cases)

    # --- setup ---

    def panels(self, bottom=30000, top=2000):
        for name, mx, val in (("ae94000.dsi.0", 65535, bottom), ("ae96000.dsi.0", 4095, top)):
            d = f"{BL}/{name}"
            os.makedirs(d, exist_ok=True)
            for f, v in (("max_brightness", mx), ("brightness", val), ("actual_brightness", val),
                         ("bl_power", 0), ("type", "raw")):
                wr(f"{d}/{f}", f"{v}\n")
        os.makedirs(VAR, exist_ok=True)
        os.makedirs(os.path.dirname(SETTINGS), exist_ok=True)
        replace(SETTINGS, json.dumps({"bottomSleepSeconds": 0}))

    def devices(self, key=True, touch=True):
        if key:
            self.key = Dev("gpio-keys-ayn", [KEY_F24])
        if touch:
            self.touch = Dev("bottom_touchscreen", [BTN_TOUCH], (0, 1))

    def proc(self, name, *args):
        p = subprocess.Popen([f"/tmp/t/{name}", *args])
        self.procs.append((name, p))
        return p

    def kill(self, name):
        for n, p in list(self.procs):
            if n == name:
                p.kill()
                p.wait()
                self.procs.remove((n, p))

    def start(self):
        env = dict(os.environ, LD_PRELOAD="/tmp/t/preload.so")
        if self.impl == "cpp":
            argv = ["/tmp/t/pbos-platformd", "--system", "--only", "thor-backlight"]
            env["PBOS_PLATFORMD_TEST_THOR_SYSFS"] = "1"
        else:
            argv = ["python3", "/tmp/t/sm8550-thor-backlightd"]
        self.logfh = open(LOG, "ab")
        self.daemon = subprocess.Popen(argv, stdout=self.logfh, stderr=subprocess.STDOUT, env=env)
        for _ in range(100):
            if "following" in (rd(LOG) or ""):
                break
            time.sleep(0.05)
        time.sleep(0.4)

    def stop(self):
        self.daemon.send_signal(signal.SIGTERM)
        try:
            self.daemon.wait(5)
        except subprocess.TimeoutExpired:
            self.daemon.kill()
            self.daemon.wait()
        time.sleep(0.2)
        self.snap("stopped")

    def cleanup(self):
        if self.daemon and self.daemon.poll() is None:
            self.daemon.kill()
        for _, p in self.procs:
            p.kill()
            p.wait()
        for d in (self.key, self.touch):
            if d:
                try:
                    d.destroy()
                except OSError:
                    pass

    # --- what the session does ---

    def press(self, hold=0.05):
        self.key.key(1)
        time.sleep(hold)
        self.key.key(0)

    def steam(self, value):
        """Steam's slider: its file kept open, written at a growing offset."""
        if self.steam_fd is None:
            self.steam_fd = os.open(f"{BOTTOM_SYS}/brightness", os.O_WRONLY)
        os.write(self.steam_fd, f"{value}\n".encode())

    def end_restore(self):
        """Steam puts back the level SCREENS has from it: no slider move."""
        self.steam(json.loads(rd(SCREENS))["steam"])
        time.sleep(0.4)

    # --- snapshots ---

    def snap(self, label):
        log = (rd(LOG) or "").splitlines()
        new = []
        for line in log[self.log_seen:]:
            if line.startswith("pbos-platformd:"):
                continue
            line = re.sub(r"^thor-backlight: ", "", line)
            new.append(re.sub(r"event\d+", "eventN", line))
        self.log_seen = len(log)
        cmds = (rd(f"{CTL}/cmds") or "").splitlines()
        newcmds = cmds[self.cmds_seen:]
        self.cmds_seen = len(cmds)
        mounts = []
        with open("/proc/self/mountinfo") as fh:
            for line in fh:
                mp = line.split()[4]
                if mp.startswith(BOTTOM_SYS + "/"):
                    mounts.append(mp)
        sb = rd(f"{RUN}/steam-bottom-brightness")
        words = [w for w in (sb or "").replace("\0", " ").split() if w.isdigit()]
        self.snaps.append({
            "label": label,
            "bottom": rd(f"{BL}/{self.bottom_name}/brightness"),
            "top": rd(f"{BL}/{self.top_name}/brightness"),
            "bl_power": rd(f"{BL}/{self.bottom_name}/bl_power"),
            "inhibited": self.touch.inhibited() if self.touch else None,
            "bottom_seen": (rd(f"{BOTTOM_SYS}/brightness") or "").replace("\0", "").split()[-1:],
            "type_seen": rd(f"{BOTTOM_SYS}/type"),
            "max_seen": rd(f"{BOTTOM_SYS}/max_brightness"),
            "state": rd(STATE),
            "overlay": rd(f"{RUN}/overlay"),
            "top_input": rd(f"{RUN}/top-input"),
            "level": rd(f"{RUN}/steam-brightness"),
            "screens": rd(SCREENS),
            "steam_file": words[-1:] if words else None,
            "desktop_start": rd(f"{RUN}/desktop-start.json"),
            "desktop_ready": os.path.exists(f"{RUN}/desktop-ready"),
            "run_mode": oct(os.stat(RUN).st_mode & 0o7777) if os.path.exists(RUN) else None,
            "flag_mode": oct(os.stat(f"{RUN}/overlay").st_mode & 0o777) if os.path.exists(f"{RUN}/overlay") else None,
            "mounts": sorted(mounts),
            "cmds": newcmds,
            "log": new,
        })
        if self.expect:
            self.snaps[-1]["expect"] = self.expect
            self.expect = []

    def check(self, what, ok):
        """A duo-* case's expectation, kept with the next snapshot."""
        self.expect.append([what, bool(ok)])

    def duo_panels(self, bottom=2000, top=1500):
        """The Retroid Pocket Duo's: top ae94000 (max 4095, Steam's), bottom
        ae96000 (max 3445)."""
        self.bottom_name, self.top_name = "ae96000.dsi.0", "ae94000.dsi.0"
        for name, mx, val in (("ae94000.dsi.0", 4095, top), ("ae96000.dsi.0", 3445, bottom)):
            d = f"{BL}/{name}"
            os.makedirs(d, exist_ok=True)
            for f, v in (("max_brightness", mx), ("brightness", val), ("actual_brightness", val),
                         ("bl_power", 0), ("type", "raw")):
                wr(f"{d}/{f}", f"{v}\n")
        os.makedirs(VAR, exist_ok=True)
        os.makedirs(os.path.dirname(SETTINGS), exist_ok=True)
        replace(SETTINGS, json.dumps({"bottomSleepSeconds": 0}))


# --- the cases ---

def c_boot_fresh(c):
    c.panels(bottom=30000, top=2000)
    c.devices()
    c.start()
    c.snap("started, nothing saved")


def c_boot_saved(c):
    c.panels()
    replace(SCREENS, '{"top": 0.3, "bottom": 0.6, "steam": 20000}\n')
    os.makedirs(RUN, exist_ok=True)
    wr(f"{RUN}/steam-brightness", "0.4")
    c.devices()
    c.start()
    c.snap("started with SCREENS")
    c.steam(10000)
    time.sleep(0.5)
    c.snap("fade-in 10000 ignored")
    c.steam(15000)
    time.sleep(0.5)
    c.snap("fade-in 15000 ignored")
    c.steam(20000)
    time.sleep(0.5)
    c.snap("back at 20000")
    c.steam(40000)
    time.sleep(0.6)
    c.snap("slider 40000")
    for v in range(41000, 50001, 1000):
        c.steam(v)
        time.sleep(0.1)
    time.sleep(0.6)
    c.snap("slider dragged to 50000")
    c.steam(655)
    time.sleep(0.6)
    c.snap("slider at its lowest")


def c_restore_timeout(c):
    c.panels()
    replace(SCREENS, '{"top": 0.3, "bottom": 0.6, "steam": 20000}\n')
    c.devices()
    c.start()
    c.steam(10000)
    time.sleep(1)
    c.snap("fade-in ignored")
    time.sleep(121)
    c.steam(12000)
    time.sleep(0.6)
    c.snap("after 120 s a new value is a move")


def c_gamescope_top(c):
    c.panels()
    replace(SCREENS, '{"top": 0.5, "bottom": 0.5, "steam": 30000}\n')
    c.devices()
    c.start()
    c.end_restore()
    c.snap("restored")
    wr(f"{BOTTOM_SYS}/../ae96000.dsi.0/brightness", "1000")
    time.sleep(0.6)
    c.snap("gamescope wrote 1000 to the top panel")
    wr(f"{BL}/ae96000.dsi.0/brightness", "0")
    time.sleep(0.6)
    c.snap("top panel 0: not a level")


def c_screens_edit(c):
    c.panels()
    replace(SCREENS, '{"top": 0.5, "bottom": 0.5, "steam": 30000}\n')
    c.devices()
    c.start()
    c.end_restore()
    replace(SCREENS, '{"top": 0.2, "bottom": 0.9, "steam": 30000}\n')
    time.sleep(0.6)
    c.snap("plugin: top 0.2, bottom 0.9")
    replace(SCREENS, '{"top": 0.2, "bottom": ')
    time.sleep(0.6)
    c.snap("broken SCREENS: kept")
    replace(SCREENS, '{"top": "0.7", "bottom": 1, "steam": "30000"}\n')
    time.sleep(0.6)
    c.snap("strings and ints")
    replace(SCREENS, '{"top": -3, "bottom": 7.5}\n')
    time.sleep(0.6)
    c.snap("out of range, no steam")


def c_ayn_press(c):
    c.panels()
    replace(SCREENS, '{"top": 0.5, "bottom": 0.5, "steam": 30000}\n')
    c.devices()
    c.proc("gamescope", "--steam")
    c.start()
    c.snap("Game Mode up")
    c.press()
    time.sleep(0.6)
    c.snap("short press: Barry opens")
    c.press()
    time.sleep(0.6)
    c.snap("short press: Barry closes")
    c.press(hold=0.9)
    time.sleep(0.6)
    c.snap("held: bottom screen off")
    c.press()
    time.sleep(0.9)
    c.snap("short press while off: on with Barry")
    c.press(hold=0.9)
    time.sleep(0.6)
    c.snap("held: off, Barry closed")
    c.press(hold=0.9)
    time.sleep(0.6)
    c.snap("held: on again")
    wr(f"{RUN}/top-input", "1")
    time.sleep(0.6)
    c.snap("top input: yield")
    wr(f"{RUN}/top-input", "0")
    time.sleep(0.6)
    c.snap("top input closed")
    c.key.key(1)
    time.sleep(0.3)
    c.key.key(2)  # autorepeat: nothing
    time.sleep(0.1)
    c.key.key(0)
    time.sleep(0.6)
    c.snap("0.4 s press with a repeat: short")


def c_overlay_delay(c):
    c.panels()
    replace(SCREENS, '{"top": 0.5, "bottom": 0.5, "steam": 30000}\n')
    wr(STATE, "off")
    c.devices()
    c.start()
    c.snap("switch off")
    wr(f"{RUN}/overlay", "1")
    time.sleep(0.25)
    c.snap("Barry opening: still dark")
    time.sleep(0.6)
    c.snap("Barry open: lit")
    wr(f"{RUN}/overlay", "0")
    time.sleep(0.5)
    c.snap("Barry closed: dark")


def c_sleep(c):
    c.panels()
    replace(SCREENS, '{"top": 0.5, "bottom": 0.5, "steam": 30000}\n')
    replace(SETTINGS, json.dumps({"bottomSleepSeconds": 2}))
    c.devices()
    c.proc("gamescope", "--steam")
    t0 = time.monotonic()
    c.start()

    def at(t):
        time.sleep(max(0, t0 + t - time.monotonic()))

    at(2.9)
    c.snap("asleep after 2 s")
    c.touch.touch()
    time.sleep(0.5)
    c.snap("touch wakes (and nudges Steam)")
    at(7.9)
    c.snap("asleep again (5 s after the last look)")
    c.press()
    time.sleep(0.5)
    c.snap("AYN press only wakes")
    wr(f"{CTL}/showing", "1")
    at(13.0)
    c.snap("a game on the bottom screen: awake")
    replace(SETTINGS, json.dumps({"bottomSleepSeconds": 2, "other": True}))
    wr(f"{CTL}/showing", "0")
    at(18.0)
    c.snap("asleep once the game is gone")
    replace(SETTINGS, json.dumps({"bottomSleepSeconds": 0}))
    time.sleep(0.5)
    c.snap("sleep turned off: awake")


def c_desktop(c):
    c.panels()
    replace(SCREENS, '{"top": 0.5, "bottom": 0.5, "steam": 30000}\n')
    c.devices()
    c.start()
    c.end_restore()
    c.proc("kwin_wayland")
    time.sleep(2.8)
    c.snap("Desktop Mode: held at SCREENS")
    wr(f"{RUN}/desktop-ready", "")
    time.sleep(0.6)
    c.snap("helper ready: top at its highest")
    wr(f"{BOTTOM_SYS}/brightness", "1000")  # Plasma's bottom slider (PowerDevil)
    time.sleep(0.6)
    c.snap("Plasma's bottom slider")
    wr(f"{RUN}/desktop-top", "0.25")
    time.sleep(0.6)
    c.snap("Plasma's top slider")
    wr(f"{BL}/ae96000.dsi.0/brightness", "100")
    time.sleep(0.6)
    c.snap("someone dimmed the top panel: back at its highest")
    c.press()
    time.sleep(0.6)
    c.snap("AYN press: Barry switch in Desktop Mode")
    c.kill("kwin_wayland")
    c.proc("gamescope", "--steam")
    time.sleep(2.8)
    c.steam_fd = None
    c.snap("Game Mode again")
    c.steam(15000)
    time.sleep(0.5)
    c.snap("Steam's fade-in ignored again")
    c.steam(30000)
    time.sleep(0.5)
    c.snap("Steam back at its level")


def c_desktop_timeout(c):
    c.panels()
    replace(SCREENS, '{"top": 0.4, "bottom": 0.7, "steam": 30000}\n')
    c.devices()
    c.start()
    c.proc("kwin_wayland")
    time.sleep(2.8)
    c.snap("Desktop Mode, no helper")
    time.sleep(10.5)
    c.snap("10 s on: top at its highest")


def c_einval(c):
    c.panels()
    replace(SCREENS, '{"top": 0.5, "bottom": 0.5, "steam": 30000}\n')
    c.devices()
    c.start()
    c.end_restore()
    wr(f"{CTL}/einval-top", "")
    replace(SCREENS, '{"top": 0.8, "bottom": 0.5, "steam": 30000}\n')
    time.sleep(1.6)
    c.snap("top refuses: kept, no new level")
    os.remove(f"{CTL}/einval-top")
    time.sleep(0.8)
    c.snap("top takes it")
    wr(f"{CTL}/einval-bottom", "")
    replace(SCREENS, '{"top": 0.8, "bottom": 0.1, "steam": 30000}\n')
    time.sleep(1.6)
    c.snap("bottom refuses")
    os.remove(f"{CTL}/einval-bottom")
    time.sleep(0.8)
    c.snap("bottom takes it")


def c_bl_enforce(c):
    c.panels()
    replace(SCREENS, '{"top": 0.5, "bottom": 0.5, "steam": 30000}\n')
    wr(STATE, "off")
    c.devices()
    c.start()
    c.snap("switch off")
    wr(f"{BL}/ae94000.dsi.0/bl_power", "0")  # a modeset powered it up
    time.sleep(0.8)
    c.snap("backlight powered down again")
    for d in sorted(os.listdir("/sys/class/input")):
        if rd(f"/sys/class/input/{d}/name") == "bottom_touchscreen":
            wr(f"/sys/class/input/{d}/inhibited", "0")  # a start script
    time.sleep(0.8)
    c.snap("touchscreen inhibited again")
    wr(STATE, "on")
    time.sleep(0.8)
    c.snap("switch on")


def c_steam_append(c):
    c.panels()
    replace(SCREENS, '{"top": 0.5, "bottom": 0.5, "steam": 30000}\n')
    c.devices()
    c.start()
    c.end_restore()
    for i in range(900):
        c.steam(20000 + i)
    c.steam(22222)
    time.sleep(0.8)
    c.snap("5 KB of slider values: the last counts")
    c.steam(33333)
    time.sleep(0.8)
    c.snap("after the trim")


def c_charge_rest(c):
    c.panels()
    replace(SCREENS, '{"top": 0.5, "bottom": 0.5, "steam": 30000}\n')
    c.devices()
    c.start()
    c.end_restore()
    rest = subprocess.Popen(["sleep", "1000"])
    wr("/run/sm8550-charge-rest", str(rest.pid))
    time.sleep(0.4)
    replace(SCREENS, '{"top": 0.1, "bottom": 0.9, "steam": 30000}\n')
    c.press()
    c.touch.touch()
    time.sleep(0.6)
    c.snap("resting: hands off")
    rest.kill()
    rest.wait()
    time.sleep(0.8)
    c.snap("rest over: the press and SCREENS taken")


def c_hotplug(c):
    c.panels()
    replace(SCREENS, '{"top": 0.5, "bottom": 0.5, "steam": 30000}\n')
    c.devices(key=False)
    c.start()
    c.snap("no AYN button yet")
    c.key = Dev("gpio-keys-ayn", [KEY_F24])
    time.sleep(6)
    c.snap("AYN button came")
    c.press()
    time.sleep(0.6)
    c.snap("its press works")
    c.key.destroy()
    c.key = None
    time.sleep(0.6)
    c.snap("gone again")


def c_nudge(c):
    c.panels()
    replace(SCREENS, '{"top": 0.5, "bottom": 0.5, "steam": 30000}\n')
    c.devices()
    c.start()
    c.touch.touch()
    time.sleep(0.5)
    c.snap("touch without Game Mode")
    c.proc("gamescope", "--steam")
    time.sleep(2.5)
    c.touch.touch()
    time.sleep(0.5)
    c.snap("touch within 20 s: no nudge")
    time.sleep(17.5)
    c.touch.touch()
    time.sleep(0.5)
    c.snap("20 s on: nudge")


def steps(since, until):
    """Each panel's writes in a time span (from the write log): how often,
    and the biggest step, as it is and as a ratio per 16 ms."""
    out = {}
    rows = []
    for line in (rd(f"{CTL}/wlog") or "").splitlines():
        t, panel, value = line.split()[:3]
        if since <= float(t) <= until and panel in ("top", "bottom") and value.isdigit():
            rows.append((float(t), panel, int(value)))
    for panel in ("bottom", "top"):
        w = [(t, v) for t, p, v in rows if p == panel]
        biggest = per16 = 1.0
        for (t0, a), (t1, b) in zip(w, w[1:]):
            r = max(a, b) / max(1, min(a, b))
            biggest = max(biggest, r)
            per16 = max(per16, r ** (0.016 / max(t1 - t0, 0.016)))
        out[panel] = {"writes_per_s": round(len(w) / (until - since), 1), "max_step_ratio": round(biggest, 4),
                      "max_ratio_per_16ms": round(per16, 4)}
    return out


def c_drag(c):
    """Steam's slider dragged for 1 s (a value every 40 ms), with the
    bottom panel's writes taking 25 ms as on the Thor; settled values are
    compared, the steps reported."""
    c.panels(bottom=30000)
    replace(SCREENS, '{"top": 1.0, "bottom": 1.0, "steam": 65266}\n')
    c.devices()
    c.start()
    # Game Mode's start: a fade-in value, then its own level back (the
    # restore window closes; from now on its values are slider moves).
    c.steam(60000)
    time.sleep(0.3)
    c.steam(65266)
    time.sleep(0.8)
    wr(f"{CTL}/slow-bottom", "25")
    t0 = time.monotonic()
    for i in range(26):
        c.steam(65266 - i * 1800)
        time.sleep(max(0, t0 + (i + 1) * 0.04 - time.monotonic()))
    t1 = time.monotonic()
    time.sleep(1.0)
    c.snap("dragged from 65266 to 20266 in 1 s")
    c.snaps[-1]["measure"] = {"drag": steps(t0, t1), "drag+settle": steps(t0, t1 + 0.6)}
    os.remove(f"{CTL}/slow-bottom")


def usage(pid):
    """CPU (user + system, ms) and RSS (kB) of the daemon so far."""
    st = rd(f"/proc/{pid}/stat").rsplit(")", 1)[1].split()
    ticks = int(st[11]) + int(st[12])
    rss = next(int(l.split()[1]) for l in open(f"/proc/{pid}/status") if l.startswith("VmRSS:"))
    return ticks * 1000 // os.sysconf("SC_CLK_TCK"), rss


def c_idle(c):
    """Not compared: what each costs doing nothing (Game Mode up, screen
    lit), then with the bottom screen switched off."""
    c.panels()
    replace(SCREENS, '{"top": 0.5, "bottom": 0.5, "steam": 30000}\n')
    c.devices()
    c.proc("gamescope", "--steam")
    c.start()
    time.sleep(3)
    for label, secs in (("lit", 60), ("dark", 60)):
        if label == "dark":
            wr(STATE, "off")
            time.sleep(2)
        cpu0, _ = usage(c.daemon.pid)
        time.sleep(secs)
        cpu1, rss = usage(c.daemon.pid)
        c.snap(f"idle {label}")
        c.snaps[-1]["measure"] = {"cpu_ms_per_min": (cpu1 - cpu0) * 60 // secs, "rss_kb": rss}


# --- the Retroid Pocket Duo (DUO profile) ---

DUO_BOTTOM = f"{BL}/ae96000.dsi.0"
DUO_TOP = f"{BL}/ae94000.dsi.0"


def linear(f, mx=3445):
    """The bottom panel on Retroid's Android (thor_backlight.h's duo_bottom)."""
    return max(10, int(1 + max(0.0, min(1.0, f)) * (mx - 1)))


LOW, HIGH = 9 / 254, 0.85
GS = f"{RUN}/gamescope-backlight"


def top_raw(f, mx=4095):
    """The top panel on Retroid's Android (duo_top)."""
    level = LOW + max(0.0, min(1.0, f)) * (HIGH - LOW)
    return max(10, int(1 + level * 8190) * mx // 8191)


def steam_pos(raw, mx=4095):
    """thor_backlight.h's steam_position of what Steam wrote."""
    lo, mid, high = 0.01, 0.29730177875068026, 0.8408964152537145
    v = raw / mx
    if v <= lo:
        return 0.0
    if v <= mid:
        return 0.5 * (math.log2(v / lo) / 4.893856189774724)
    if v <= high:
        return 0.5 + 0.4 * (math.log2(v / mid) / 1.5)
    return 0.9 + 0.1 * (math.log2(v / high) / 0.25)


def duo_is(c, what, top, bottom, steam=None):
    """Both panels and SCREENS at these slider places."""
    got = rd(f"{DUO_TOP}/brightness"), rd(f"{DUO_BOTTOM}/brightness")
    s = json.loads(rd(SCREENS) or "{}")
    c.check(f"{what} {got} {s}", got == (str(top_raw(top)), str(linear(bottom)))
            and abs(s["top"] - top) < 1e-9 and abs(s["bottom"] - bottom) < 1e-9
            and (steam is None or s["steam"] == steam))


def duo_mounts():
    return sorted(m.split("/")[-1] for m in (line.split()[4] for line in open("/proc/self/mountinfo"))
                  if m.startswith("/sys/class/backlight/ae9"))


def duo_end_restore(c):
    """Steam fades its level in, up to the one SCREENS has from it."""
    steam = json.loads(rd(SCREENS))["steam"]
    c.steam(steam // 2)
    time.sleep(0.2)
    c.steam(steam)
    time.sleep(0.4)


def gamescope_writes(value):
    """As gamescope: a file kept open, written from its start, not cut."""
    with open(f"{GS}/brightness", "r+") as fh:
        fh.write(f"{value}\n")


def c_duo_boot(c):
    c.duo_panels(bottom=2000, top=1500)
    c.devices(key=False)
    c.start()
    f = steam_pos(1500)
    duo_is(c, "both screens at Steam's level", f, f, 1500)
    c.check("Steam's file over the top panel alone", duo_mounts() == ["brightness"]
            and (rd(f"{BOTTOM_SYS}/brightness") or "").split()[-1:] == ["1500"])
    st = os.stat(f"{RUN}/steam-bottom-brightness")
    c.check("Steam (video) may write it", st.st_mode & 0o777 == 0o664)
    c.check("Screen off flag made, 0, group-writable",
            rd(f"{RUN}/sleep") == "0" and os.stat(f"{RUN}/sleep").st_mode & 0o777 == 0o664)
    c.check("gamescope's folder", rd(f"{GS}/max_brightness") == "4095" and rd(f"{GS}/brightness") == "1500"
            and rd(f"{GS}/actual_brightness") == "1500" and os.stat(f"{GS}/brightness").st_mode & 0o777 == 0o664)
    c.snap("started, nothing saved")
    c.stop()
    c.check("stopped: Steam writes the panel again", duo_mounts() == [])
    wr(f"{BOTTOM_SYS}/brightness", "777")
    c.check("for real", rd(f"{DUO_TOP}/brightness") == "777")
    c.snap("uncovered")


def c_duo_steam(c):
    """Steam's slider is Android's both-screens one; the others set their own."""
    c.duo_panels(bottom=2000, top=1500)
    replace(SCREENS, '{"top": 0.3, "bottom": 0.6, "steam": 1500}\n')
    c.devices(key=False)
    c.start()
    duo_is(c, "each its own at the start", 0.3, 0.6, 1500)
    for v in (750, 1100, 1300):
        c.steam(v)
        time.sleep(0.3)
    duo_is(c, "not while Steam fades its level in", 0.3, 0.6, 1500)
    c.steam(1500)
    time.sleep(0.4)
    duo_is(c, "nor once it is back", 0.3, 0.6, 1500)
    c.snap("restored")
    for v in range(1600, 2401, 100):
        c.steam(v)
        time.sleep(0.08)
    time.sleep(0.8)
    f = steam_pos(2400)
    duo_is(c, "Steam's slider sets both", f, f, 2400)
    c.snap("dragged")
    replace(SCREENS, json.dumps({"top": 0.5, "bottom": f, "steam": 2400}))
    time.sleep(0.9)
    duo_is(c, "Barry's top slider", 0.5, f, 2400)
    replace(SCREENS, json.dumps({"top": 0.5, "bottom": 0.2, "steam": 2400}))
    time.sleep(0.9)
    duo_is(c, "Barry's bottom slider", 0.5, 0.2, 2400)
    replace(SCREENS, json.dumps({"top": 0.7, "bottom": 0.7, "steam": 2400}))
    time.sleep(0.9)
    duo_is(c, "Barry's both slider", 0.7, 0.7, 2400)
    c.snap("Barry's sliders")
    for v, f in ((41, 0.0), (4095, 1.0)):
        c.steam(v)
        time.sleep(0.9)
        s = json.loads(rd(SCREENS))
        c.check(f"Steam at {v}: Android's end ({s})", abs(s["top"] - f) < 1e-3 and s["top"] == s["bottom"]
                and rd(f"{DUO_TOP}/brightness") == str(top_raw(f)) and rd(f"{DUO_BOTTOM}/brightness") == str(linear(s["bottom"])))
    c.snap("ends")


def c_duo_linked(c):
    """Barry's link: off, Steam's slider is the top screen's alone."""
    c.duo_panels()
    replace(SCREENS, '{"top": 0.5, "bottom": 0.3, "steam": 1218}\n')
    replace(SETTINGS, json.dumps({"bottomSleepSeconds": 0, "screensLinked": False}))
    c.devices(key=False)
    c.start()
    duo_end_restore(c)
    time.sleep(1.2)
    for v in range(1300, 2048, 83):
        c.steam(v)
        time.sleep(0.05)
    time.sleep(0.9)
    s = json.loads(rd(SCREENS))
    v = s["steam"]
    duo_is(c, "not linked: the top one", steam_pos(v), 0.3, v)
    time.sleep(1.2)
    duo_fade(c, v, 600)
    s = json.loads(rd(SCREENS))
    c.check(f"the dim is both screens' still {s}", s["bottom"] == 0.3 and s["steam"] == v
            and int(rd(f"{DUO_BOTTOM}/brightness")) < linear(0.3))
    c.steam(v)
    time.sleep(0.9)
    replace(SETTINGS, json.dumps({"bottomSleepSeconds": 0, "screensLinked": True}))
    time.sleep(0.3)
    c.steam(1000)
    time.sleep(0.9)
    f = steam_pos(1000)
    duo_is(c, "linked: both", f, f, 1000)
    c.snap("linked")


def told():
    try:
        return float(rd(f"{RUN}/steam-tell") or "-1")
    except ValueError:
        return -1


def c_duo_tell(c):
    """Steam's slider follows the top screen's brightness from Barry."""
    c.duo_panels()
    replace(SCREENS, '{"top": 0.5, "bottom": 0.5, "steam": 1218}\n')
    c.devices(key=False)
    c.start()
    duo_end_restore(c)
    c.check("nothing to tell", told() == -1)
    replace(SCREENS, json.dumps({"top": 0.5, "bottom": 0.2, "steam": 1218}))
    time.sleep(0.5)
    c.check("nor for the bottom screen", told() == -1)
    replace(SCREENS, json.dumps({"top": 0.7, "bottom": 0.2, "steam": 1218}))
    time.sleep(0.1)
    c.check(f"told {told()}", told() == 0.7)
    c.steam(2047)
    time.sleep(0.9)
    duo_is(c, "Steam following is no move", 0.7, 0.2, 2047)
    for i in range(1, 11):  # Barry's drag, Steam a little behind
        replace(SCREENS, json.dumps({"top": 0.7 + i * 0.02, "bottom": 0.2, "steam": 2047}))
        time.sleep(0.12)
        if i in (3, 6, 10):
            c.steam({3: 2444, 6: 2860, 10: 3443}[i])
    time.sleep(1)
    c.check(f"told the last {told()}", told() == 0.9)
    duo_is(c, "nor while dragged", 0.9, 0.2, 3443)
    c.snap("followed")
    c.steam(2000)
    time.sleep(0.9)
    f = steam_pos(2000)
    duo_is(c, "its slider sets both", f, f, 2000)
    c.check("nothing told of its own move", told() == 0.9)
    replace(SCREENS, json.dumps({"top": 0.3, "bottom": f, "steam": 2000}))
    time.sleep(1)
    c.steam(313)  # a busy Steam answers late
    time.sleep(0.9)
    duo_is(c, "late", 0.3, f, 313)
    time.sleep(1)
    duo_fade(c, 313, 122)
    s = json.loads(rd(SCREENS))
    c.check(f"dims as ever {s}", s["top"] == 0.3 and s["steam"] == 313)
    c.steam(313)
    time.sleep(0.9)
    duo_is(c, "and back", 0.3, f, 313)
    c.snap("own moves")


def duo_fade(c, start, end):
    """Steam's idle dim, as recorded on a Duo: -1 first, 12 at most."""
    v, step = start, 1
    while v > end:
        v = max(end, v - min(12, step))
        step += 1
        c.steam(v)
        time.sleep(0.02)
    time.sleep(0.9)


def duo_dimmed(c, what, top, bottom, f):
    """The panels dimmed by f, SCREENS as they were."""
    got = int(rd(f"{DUO_TOP}/brightness")), int(rd(f"{DUO_BOTTOM}/brightness"))
    s = json.loads(rd(SCREENS))
    c.check(f"{what} {got} {s}", abs(got[0] - top_raw(top * f)) <= 1 and abs(got[1] - linear(bottom * f)) <= 1
            and (s["top"], s["bottom"], s["steam"]) == (top, bottom, 3071))


def c_duo_dim(c):
    """Steam's idle dim takes both screens down, each from its own."""
    c.duo_panels()
    replace(SCREENS, '{"top": 0.8, "bottom": 0.3, "steam": 3071}\n')
    c.devices(key=False)
    c.start()
    duo_end_restore(c)
    time.sleep(1.2)
    duo_fade(c, 3071, 122)
    duo_dimmed(c, "both dimmed", 0.8, 0.3, steam_pos(122) / steam_pos(3071))
    c.steam(3071)
    time.sleep(0.9)
    duo_is(c, "back at their own", 0.8, 0.3, 3071)
    c.snap("dimmed and back")
    time.sleep(1.2)
    duo_fade(c, 3071, 2000)
    for v in range(2100, 3071, 100):
        c.steam(v)
        time.sleep(0.03)
    c.steam(3071)
    time.sleep(0.9)
    duo_is(c, "fading back up, at their own", 0.8, 0.3, 3071)
    time.sleep(2.5)
    duo_is(c, "and they stay", 0.8, 0.3, 3071)
    c.snap("faded back")
    duo_fade(c, 3071, 2500)
    c.daemon.kill()
    c.daemon.wait()
    c.start()
    duo_is(c, "kept through a crash while dimmed", 0.8, 0.3, 3071)
    duo_end_restore(c)
    time.sleep(1.2)
    duo_fade(c, 3071, 2500)
    c.steam(1500)
    time.sleep(0.9)
    f = steam_pos(1500)
    duo_is(c, "its slider while dimmed sets both", f, f, 1500)
    replace(SCREENS, '{"top": 0.8, "bottom": 0.3, "steam": 1500}\n')
    time.sleep(1.2)
    c.steam(1450)
    time.sleep(0.9)
    f = steam_pos(1450)
    duo_is(c, "a step of its slider sets both", f, f, 1450)
    replace(SCREENS, '{"top": 0.8, "bottom": 0.3, "steam": 1450}\n')
    time.sleep(1.2)
    for e in ((EV_ABS, 0, 500), (EV_KEY, BTN_TOUCH, 1), (EV_SYN, 0, 0)):
        c.touch.emit(*e)
    time.sleep(0.3)
    c.steam(1449)
    time.sleep(0.9)
    f = steam_pos(1449)
    duo_is(c, "a hair with a finger down is the slider", f, f, 1449)
    c.touch.emit(EV_KEY, BTN_TOUCH, 0)
    c.touch.emit(EV_SYN, 0, 0)
    replace(SCREENS, '{"top": 0.8, "bottom": 0.3, "steam": 1449}\n')
    time.sleep(1.2)
    duo_fade(c, 1449, 800)
    c.steam(1000)
    time.sleep(3)
    f = steam_pos(1000)
    duo_is(c, "up, but not to its level: the slider", f, f, 1000)
    c.snap("slider")


def c_duo_restart(c):
    """The service starts again (or died): what is kept and what is covered."""
    c.duo_panels(bottom=2000, top=700)
    replace(SCREENS, '{"top": 0.5, "bottom": 0.25, "steam": 1500}\n')
    c.devices(key=False)
    c.start()
    duo_end_restore(c)
    c.proc("gamescope", "--steam")
    c.daemon.kill()
    c.daemon.wait()
    c.start()
    duo_is(c, "each its own after a crash", 0.5, 0.25, 1500)
    c.check("covered once", duo_mounts() == ["brightness"])
    c.steam(2000)  # Game Mode was up all along: Steam's file, open since before
    time.sleep(0.9)
    f = steam_pos(2000)
    duo_is(c, "Steam's open file still sets both", f, f, 2000)
    c.snap("again")


def c_duo_gamescope(c):
    c.duo_panels()
    replace(SCREENS, '{"top": 0.5, "bottom": 0.5, "steam": 1500}\n')
    c.devices(key=False)
    c.start()
    gamescope_writes(99)
    time.sleep(0.6)
    duo_is(c, "not while Steam puts its level back", 0.5, 0.5, 1500)
    duo_end_restore(c)
    gamescope_writes(3000)
    time.sleep(0.005)
    c.steam(2000)
    time.sleep(0.9)
    f = steam_pos(2000)
    duo_is(c, "with Steam's move, Steam's counts", f, f, 2000)
    c.steam(2100)
    time.sleep(0.02)
    gamescope_writes(3100)
    time.sleep(1.2)
    f = steam_pos(2100)
    duo_is(c, "also just after it", f, f, 2100)
    c.snap("with Steam")
    gamescope_writes(900)  # a brightness key
    time.sleep(0.9)
    f = steam_pos(900)
    duo_is(c, "gamescope alone sets both", f, f)
    gamescope_writes(63)  # over "900\n"
    time.sleep(0.9)
    f = steam_pos(63)
    duo_is(c, "fewer digits over more", f, f)
    c.snap("gamescope's own")
    wr(f"{DUO_TOP}/brightness", "1000")  # a file opened before ours went over
    time.sleep(0.9)
    f = steam_pos(1000)
    duo_is(c, "a write past the file, as gamescope's on the Thor", f, f)
    wr(f"{DUO_TOP}/brightness", "0")
    time.sleep(0.6)
    duo_is(c, "0: not a level", f, f)
    c.snap("past the file")


def c_duo_charge_rest(c):
    c.duo_panels()
    replace(SCREENS, '{"top": 0.5, "bottom": 0.5, "steam": 1500}\n')
    c.devices(key=False)
    c.start()
    duo_end_restore(c)
    p = c.proc("gamescope")
    wr("/run/sm8550-charge-rest", str(p.pid))
    time.sleep(0.2)
    # charge_rest: the bottom to 0, the covered top by bl_power alone.
    wr(f"{DUO_BOTTOM}/brightness", "0")
    wr(f"{DUO_TOP}/bl_power", "4")
    c.steam(3000)
    time.sleep(0.6)
    c.check("hands off while resting", rd(f"{DUO_TOP}/brightness") == str(top_raw(0.5))
            and rd(f"{DUO_BOTTOM}/brightness") == "0" and rd(f"{DUO_TOP}/bl_power") == "4")
    wr(f"{DUO_BOTTOM}/brightness", str(linear(0.5)))
    wr(f"{DUO_TOP}/bl_power", "0")
    os.unlink("/run/sm8550-charge-rest")
    time.sleep(0.9)
    f = steam_pos(3000)
    duo_is(c, "then Steam's level", f, f, 3000)
    c.snap("rested")


def c_duo_screen_off(c):
    c.duo_panels()
    replace(SCREENS, '{"top": 0.5, "bottom": 0.5, "steam": 0}\n')
    c.devices(key=False)
    c.proc("gamescope", "--steam")
    c.start()
    wr(f"{RUN}/overlay", "1")
    time.sleep(0.6)
    c.snap("Barry open")
    wr(f"{RUN}/sleep", "1")  # Barry's Screen off tile (statsd)
    time.sleep(0.6)
    c.check("dark", rd(f"{DUO_BOTTOM}/bl_power") == "4")
    c.check("touch still on", c.touch.inhibited() == "0")
    c.check("Barry closed", rd(f"{RUN}/overlay") == "0")
    c.check("flag taken", rd(f"{RUN}/sleep") == "0")
    c.snap("Screen off")
    time.sleep(1.5)
    c.check("stays dark with Sleep after: Never", rd(f"{DUO_BOTTOM}/bl_power") == "4")
    c.snap("still asleep")
    c.touch.touch()
    time.sleep(0.6)
    c.check("a touch wakes it", rd(f"{DUO_BOTTOM}/bl_power") == "0")
    c.check("at its level", rd(f"{DUO_BOTTOM}/brightness") == str(linear(0.5)))
    c.snap("touch wakes")
    wr(f"{RUN}/sleep", "1")
    time.sleep(0.1)
    c.touch.touch()  # the tap's own finger, just after: no wake
    time.sleep(0.6)
    c.check("a touch within the grace keeps it asleep", rd(f"{DUO_BOTTOM}/bl_power") == "4")
    c.snap("grace")


def c_duo_switch(c):
    c.duo_panels()
    replace(SCREENS, '{"top": 0.5, "bottom": 0.5, "steam": 0}\n')
    c.devices(key=False)
    c.start()
    time.sleep(0.3)
    before = rd(f"{DUO_TOP}/brightness")
    c.check("the top where the panel was (an older version's SCREENS)", abs(int(before) - 1500) <= 1)
    wr(STATE, "off")
    time.sleep(0.8)
    c.check("switch off: dark", rd(f"{DUO_BOTTOM}/bl_power") == "4")
    c.check("switch off: touch inhibited", c.touch.inhibited() == "1")
    c.check("top panel as it was",
            rd(f"{DUO_TOP}/bl_power") == "0" and rd(f"{DUO_TOP}/brightness") == before)
    c.snap("switch off")
    wr(f"{RUN}/sleep", "1")
    time.sleep(0.6)
    c.check("Screen off while off: still off, touch inhibited", c.touch.inhibited() == "1")
    c.snap("Screen off while switched off")
    wr(STATE, "on")
    time.sleep(0.8)
    c.check("switch on: lit, touch on",
            rd(f"{DUO_BOTTOM}/bl_power") == "0" and c.touch.inhibited() == "0")
    c.snap("switch on")


def plasma(c, value):
    """PowerDevil's helper: it opens the top panel's file, cut and written."""
    wr(f"{BOTTOM_SYS}/brightness", str(value))


def c_duo_desktop(c):
    c.duo_panels(bottom=2000, top=1500)
    replace(SCREENS, '{"top": 0.9, "bottom": 0.5, "steam": 1500}\n')
    c.devices(key=False)
    c.start()
    duo_end_restore(c)
    c.proc("kwin_wayland")
    time.sleep(0.8)
    c.check("Plasma's helper sees the top panel alone", rd("/sys/class/backlight/ae94000.dsi.0/type") == "firmware"
            and rd("/sys/class/backlight/ae96000.dsi.0/type") == "raw")
    c.check("one range", all(rd(f"/sys/class/backlight/{d}/max_brightness") == "4095"
                             for d in ("ae96000.dsi.0", "ae94000.dsi.0")))
    s = json.loads(rd(f"{RUN}/desktop-start.json") or "{}")
    c.check("starts from SCREENS", s.get("top") == 0.9 and s.get("bottom") == 0.5)
    plasma(c, 3481)  # the helper's probe, 5 % off
    time.sleep(0.4)
    duo_is(c, "the probe moves nothing", 0.9, 0.5, 1500)
    cpu0 = usage(c.daemon.pid)[0]
    time.sleep(2)
    cpu = usage(c.daemon.pid)[0] - cpu0
    c.check(f"idle in Desktop Mode ({cpu} ms in 2 s)", cpu <= 30)
    plasma(c, 3686)
    replace(f"{RUN}/desktop-bottom", "0.50000\n")
    replace(f"{RUN}/desktop-ready", "1\n")
    time.sleep(0.8)
    c.check("the helper has the bottom slider: panel at its highest", rd(f"{DUO_BOTTOM}/brightness") == "3445")
    c.check("top about where it was", abs(int(rd(f"{DUO_TOP}/brightness")) - top_raw(0.9)) <= 2)
    c.snap("Desktop Mode")
    replace(f"{RUN}/desktop-bottom", "0.25000\n")
    replace(f"{RUN}/desktop-top", "0.10000\n")  # the helper's, not used here
    for raw, top in ((0, 145), (1024, 979), (2048, 1812), (3276, 2813), (4095, 3480), (3276, 2813)):
        plasma(c, raw)
        time.sleep(0.8)
        c.check(f"Plasma's top slider at {raw}: {rd(DUO_TOP + '/brightness')}",
                abs(int(rd(f"{DUO_TOP}/brightness")) - top) <= 1)
    s = json.loads(rd(SCREENS))
    c.check("both sliders saved", abs(s["top"] - 0.8) < 1e-9 and s["bottom"] == 0.25 and s["steam"] == 1500)
    replace(SCREENS, json.dumps(dict(s, top=0.6)))  # Barry's
    time.sleep(0.8)
    c.check("Barry's top slider", rd(f"{DUO_TOP}/brightness") == str(top_raw(0.6)))
    plasma(c, 3277)
    time.sleep(0.8)
    wr(STATE, "off")
    time.sleep(0.6)
    c.check("bottom off in Desktop Mode", rd(f"{DUO_BOTTOM}/bl_power") == "4")
    wr(STATE, "on")
    time.sleep(0.8)
    c.check("back on, at its highest",
            rd(f"{DUO_BOTTOM}/bl_power") == "0" and rd(f"{DUO_BOTTOM}/brightness") == "3445")
    c.snap("Plasma's sliders")
    top = 3277 / 4095
    c.kill("kwin_wayland")
    time.sleep(0.8)
    c.check("Steam's file alone left over the panels", duo_mounts() == ["brightness"])
    c.check("the session's files gone", not any(f.startswith("desktop-") and f != "desktop-switch" for f in os.listdir(RUN)))
    duo_is(c, "both at Desktop Mode's level", top, 0.25, 1500)
    c.check("Steam's file has Steam's value again", rd(f"{RUN}/steam-bottom-brightness") == "1500")
    c.proc("gamescope", "--steam")
    for v in (750, 1100, 1500):  # Steam fades its own level back in
        c.steam(v)
        time.sleep(0.4)
    duo_is(c, "kept as Steam's level comes back", top, 0.25, 1500)
    c.steam(1400)
    time.sleep(0.9)
    f = steam_pos(1400)
    duo_is(c, "Steam's slider sets both after", f, f, 1400)
    c.snap("Game Mode again")


def c_duo_desktop_timeout(c):
    """No helper: both hold SCREENS; after 10 s Plasma's slider still works."""
    c.duo_panels()
    replace(SCREENS, '{"top": 0.4, "bottom": 0.5, "steam": 1500}\n')
    c.devices(key=False)
    c.start()
    duo_end_restore(c)
    c.proc("kwin_wayland")
    time.sleep(0.8)
    plasma(c, 4095)  # Plasma's own last level
    time.sleep(0.5)
    duo_is(c, "held", 0.4, 0.5, 1500)
    time.sleep(10)
    duo_is(c, "still, when the wait is over", 0.4, 0.5, 1500)
    plasma(c, 1024)
    time.sleep(0.9)
    duo_is(c, "Plasma's slider, the bottom its own", 1024 / 4095, 0.5, 1500)
    c.snap("no helper")


def c_duo_desktop_killed(c):
    """Died in Desktop Mode: the next start clears its mounts."""
    c.duo_panels(bottom=2000, top=1500)
    c.devices(key=False)
    c.start()
    c.proc("kwin_wayland")
    time.sleep(0.8)
    c.check("Desktop Mode's mounts", duo_mounts() == ["brightness", "max_brightness", "type"])
    c.daemon.kill()
    c.daemon.wait()
    c.kill("kwin_wayland")
    c.start()
    time.sleep(0.5)
    c.check("ranges read past the old mount", (rd(LOG) or "").count("ae96000.dsi.0 (max 3445)") == 2)
    c.check("Steam's file alone over the panels", duo_mounts() == ["brightness"])
    c.snap("started again")


def c_duo_curve(c):
    """Both sliders on Retroid's Android curves."""
    c.duo_panels(bottom=2000, top=1500)
    c.devices(key=False)
    c.start()
    s = json.loads(rd(SCREENS))
    # Android's own values (RPDuo 1.0.0.157), not from a formula here.
    for f, top, bottom in ((0.0, 145, 10), (0.1, 478, 345), (0.25, 979, 862), (0.5, 1812, 1723),
                           (0.75, 2646, 2584), (1.0, 3480, 3445)):
        replace(SCREENS, json.dumps(dict(s, top=f, bottom=f)))
        time.sleep(0.9)
        got = rd(f"{DUO_TOP}/brightness"), rd(f"{DUO_BOTTOM}/brightness")
        c.check(f"both sliders at {f:.0%}: {got}", abs(int(got[0]) - top) <= 1 and abs(int(got[1]) - bottom) <= 1
                and got == (str(top_raw(f)), str(linear(f))))
    c.snap("curve")


CASES = {name[2:].replace("_", "-"): fn for name, fn in globals().items() if name.startswith("c_")}


def main():
    if sys.argv[1] == "--list":
        print(" ".join(CASES))
        return 0
    impl, name = sys.argv[1], sys.argv[2]
    c = Case(impl)
    try:
        CASES[name](c)
        c.stop()
    finally:
        c.cleanup()
    with open(f"/tmp/out/{name}.{impl}.json", "w") as fh:
        json.dump(c.snaps, fh, indent=1)
    for src, dst in ((f"{CTL}/wlog", "wlog"), (LOG, "log"), (f"{CTL}/cmds", "cmds")):
        if os.path.exists(src):
            shutil.move(src, f"/tmp/out/{name}.{impl}.{dst}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
