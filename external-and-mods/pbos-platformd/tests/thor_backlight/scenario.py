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
    """thor_backlight.h's linear_bottom (panel_value)."""
    f = max(0.0, min(1.0, f))
    return max(10, min(mx, int(1 + f * 4094) * mx // 4095))


def steam_pos(raw, mx=4095):
    """thor_backlight.h's steam_position of a top panel value."""
    lo = 0.01 ** (1 / 2.2)
    return max(0.0, ((raw / mx) ** (1 / 2.2) - lo) / (1 - lo))


def steam_lvl(f):
    lo = 0.01 ** (1 / 2.2)
    return (lo + f * (1 - lo)) ** 2.2


def c_duo_boot(c):
    c.duo_panels(bottom=2000, top=1500)
    c.devices(key=False)
    c.start()
    s = json.loads(rd(SCREENS) or "{}")
    f = 2000 / 3445
    c.check("SCREENS made from the panels (the top as Steam's slider)",
            abs(s.get("bottom", -1) - f) < 1e-9 and abs(s.get("top", -1) - steam_pos(1500)) < 1e-9
            and s.get("steam") == 1500)
    c.check("bottom panel about where it was", abs(int(rd(f"{DUO_BOTTOM}/brightness")) - 2000) <= 2)
    c.check("top panel untouched", rd(f"{DUO_TOP}/brightness") == "1500")
    c.check("nothing mounted over the panels", not any(
        line.split()[4].startswith("/sys/class/backlight/") for line in open("/proc/self/mountinfo")))
    c.check("no Steam file, no level",
            rd(f"{RUN}/steam-bottom-brightness") is None and rd(f"{RUN}/steam-brightness") is None)
    c.check("Screen off flag made, 0, group-writable",
            rd(f"{RUN}/sleep") == "0" and os.stat(f"{RUN}/sleep").st_mode & 0o777 == 0o664)
    c.snap("started, nothing saved")
    replace(SCREENS, json.dumps(dict(s, bottom=0.5)))
    time.sleep(0.8)
    c.check("bottom follows SCREENS' bottom, linearly", rd(f"{DUO_BOTTOM}/brightness") == str(linear(0.5)))
    c.check("top panel still untouched", rd(f"{DUO_TOP}/brightness") == "1500")
    c.snap("bottom slider 50")
    wr(f"{DUO_TOP}/brightness", "3000")  # Steam's slider, on the top panel
    time.sleep(0.6)
    c.check("Steam's top write left alone", rd(f"{DUO_TOP}/brightness") == "3000")
    c.check("bottom not moved by it", rd(f"{DUO_BOTTOM}/brightness") == str(linear(0.5)))
    s = json.loads(rd(SCREENS))
    c.check("the top slider moves along", s["bottom"] == 0.5 and abs(s["top"] - steam_pos(3000)) < 1e-9
            and s["steam"] == 3000)
    c.snap("Steam moved the top screen")
    replace(SCREENS, json.dumps(dict(s, bottom=0.0)))
    time.sleep(0.8)
    c.check("top not moved by a bottom change", rd(f"{DUO_TOP}/brightness") == "3000")
    c.check("bottom at its lowest", rd(f"{DUO_BOTTOM}/brightness") == str(linear(0.0)))
    c.snap("bottom slider 0")
    replace(SCREENS, json.dumps(dict(s, top=0.5, bottom=0.5)))  # Barry's, both screens
    time.sleep(0.8)
    want = round(steam_lvl(0.5) * 4095)
    c.check("Barry's top slider moves the top panel", rd(f"{DUO_TOP}/brightness") == str(want))
    time.sleep(0.4)
    s = json.loads(rd(SCREENS))
    c.check("and stays where it was put", s["top"] == 0.5 and rd(f"{DUO_TOP}/brightness") == str(want))
    c.snap("Barry's top slider")
    for v in (1200, 1300, 1400):  # Steam's slider again, ten a second
        wr(f"{DUO_TOP}/brightness", str(v))
        time.sleep(0.1)
    time.sleep(0.6)
    s = json.loads(rd(SCREENS))
    c.check("Steam's slider has it again", rd(f"{DUO_TOP}/brightness") == "1400" and s["steam"] == 1400
            and abs(s["top"] - steam_pos(1400)) < 1e-9)
    c.snap("Steam again")


def c_duo_restart(c):
    """Game Mode starts: Steam fades its level back in, the top screen keeps its own."""
    c.duo_panels(bottom=2000, top=700)
    replace(SCREENS, '{"top": 0.5, "bottom": 0.5, "steam": 1500}\n')
    c.devices(key=False)
    c.start()
    for v in (750, 1100, 1500):
        wr(f"{DUO_TOP}/brightness", str(v))
        time.sleep(0.4)
    c.check("not while Steam fades", rd(f"{DUO_TOP}/brightness") == "1500")
    c.check("no move of its slider", json.loads(rd(SCREENS))["top"] == 0.5)
    time.sleep(1.6)
    c.check("then SCREENS' top", rd(f"{DUO_TOP}/brightness") == str(round(steam_lvl(0.5) * 4095)))
    wr(f"{DUO_TOP}/brightness", "900")
    time.sleep(0.6)
    c.check("Steam's slider after", json.loads(rd(SCREENS))["steam"] == 900 and rd(f"{DUO_TOP}/brightness") == "900")
    c.snap("restored")


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
    wr(STATE, "off")
    time.sleep(0.8)
    c.check("switch off: dark", rd(f"{DUO_BOTTOM}/bl_power") == "4")
    c.check("switch off: touch inhibited", c.touch.inhibited() == "1")
    c.check("top panel untouched",
            rd(f"{DUO_TOP}/bl_power") == "0" and rd(f"{DUO_TOP}/brightness") == "1500")
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


def c_duo_desktop(c):
    c.duo_panels(bottom=2000, top=1500)
    replace(SCREENS, '{"top": 0.9, "bottom": 0.5, "steam": 0}\n')
    c.devices(key=False)
    c.start()
    c.proc("kwin_wayland")
    time.sleep(0.8)
    c.check("Plasma's helper sees the top panel alone", rd("/sys/class/backlight/ae94000.dsi.0/type") == "firmware"
            and rd("/sys/class/backlight/ae96000.dsi.0/type") == "raw")
    c.check("the bottom shows the top's range", rd("/sys/class/backlight/ae96000.dsi.0/max_brightness") == "4095")
    s = json.loads(rd(f"{RUN}/desktop-start.json") or "{}")
    c.check("starts from what the top shows", abs(s.get("top", -1) - 1500 / 4095) < 1e-9 and s.get("bottom") == 0.5)
    c.check("bottom still from SCREENS, no helper yet", rd(f"{DUO_BOTTOM}/brightness") == str(linear(0.5)))
    c.check("top panel untouched", rd(f"{DUO_TOP}/brightness") == "1500")
    cpu0 = usage(c.daemon.pid)[0]
    time.sleep(2)
    cpu = usage(c.daemon.pid)[0] - cpu0
    c.check(f"idle in Desktop Mode ({cpu} ms in 2 s)", cpu <= 30)
    c.snap("Desktop Mode")
    replace(f"{RUN}/desktop-bottom", "0.50000\n")
    replace(f"{RUN}/desktop-ready", "1\n")
    time.sleep(0.8)
    c.check("the helper has the bottom slider: panel at its highest", rd(f"{DUO_BOTTOM}/brightness") == "3445")
    replace(f"{RUN}/desktop-bottom", "0.25000\n")
    replace(f"{RUN}/desktop-top", "0.80000\n")
    time.sleep(0.6)
    s = json.loads(rd(SCREENS))
    c.check("both sliders saved", abs(s["top"] - steam_pos(0.8 * 4095)) < 1e-9 and s["bottom"] == 0.25
            and s["steam"] == 1500)
    c.check("top panel still untouched", rd(f"{DUO_TOP}/brightness") == "1500")
    wr(f"{DUO_TOP}/brightness", "3276")  # what Plasma's slider did
    wr(STATE, "off")
    time.sleep(0.6)
    c.check("bottom off in Desktop Mode", rd(f"{DUO_BOTTOM}/bl_power") == "4")
    wr(STATE, "on")
    time.sleep(0.8)
    c.check("back on, at its highest",
            rd(f"{DUO_BOTTOM}/bl_power") == "0" and rd(f"{DUO_BOTTOM}/brightness") == "3445")
    c.snap("Plasma's sliders")
    c.kill("kwin_wayland")
    time.sleep(0.8)
    c.check("nothing mounted over the panels", not any(
        line.split()[4].startswith("/sys/class/backlight/") for line in open("/proc/self/mountinfo")))
    c.check("the session's files gone", not any(f.startswith("desktop-") and f != "desktop-switch" for f in os.listdir(RUN)))
    c.check("bottom at Desktop Mode's level", rd(f"{DUO_BOTTOM}/brightness") == str(linear(0.25)))
    c.proc("gamescope", "--steam")
    for v in (750, 1100, 1500, 1500):  # Steam fades its own level back in
        wr(f"{DUO_TOP}/brightness", str(v))
        time.sleep(0.3)
    c.check("not while Steam fades", rd(f"{DUO_TOP}/brightness") == "1500")
    time.sleep(1.5)
    c.check("then Desktop Mode's level", abs(int(rd(f"{DUO_TOP}/brightness")) - round(0.8 * 4095)) <= 1)
    wr(f"{DUO_TOP}/brightness", "1400")  # Steam's slider
    time.sleep(1.8)
    c.check("Steam's slider left alone after", rd(f"{DUO_TOP}/brightness") == "1400")
    c.snap("Game Mode again")


def c_duo_desktop_killed(c):
    """Died in Desktop Mode: the next start clears its mounts."""
    c.duo_panels(bottom=2000, top=1500)
    c.devices(key=False)
    c.start()
    c.proc("kwin_wayland")
    time.sleep(0.8)
    c.daemon.kill()
    c.daemon.wait()
    c.kill("kwin_wayland")
    c.start()
    time.sleep(0.5)
    c.check("ranges read past the old mount", "(max 3445)" in (rd(LOG) or "").splitlines()[-1] or
            (rd(LOG) or "").count("ae96000.dsi.0 (max 3445)") == 2)
    c.check("nothing mounted over the panels", not any(
        line.split()[4].startswith("/sys/class/backlight/") for line in open("/proc/self/mountinfo")))
    c.snap("started again")


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
