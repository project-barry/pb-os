#!/usr/bin/env python3
"""pbos_stickcal — the AYN Thor stick calibration's hardware side.

EXPERIMENTAL, for testing. Nothing here lasts past a restart: rsinput is
built into the kernel, so its parameters start from the driver's defaults
at every boot, and the evdev ranges and the state in RUN go with them.

The Stick Calibration plugin runs this with the system's native Python
(Decky's own Python is emulated on these devices) as root:

  status          JSON: the pad, whether a calibration is on, the driver's
                  centres and the ranges now
  rest SECONDS    the sticks at rest: per axis the mean, lowest and highest
                  value, in centre-0 terms (see below); one JSON line
  edges [MAX_S]   the sticks pushed round the edge: a JSON line every
                  0.1 s with each axis's value now and its lowest and
                  highest so far, until stopped (SIGTERM) or MAX_S. Steam
                  doesn't see the sticks meanwhile (see HOLD)
  apply           calibration JSON on stdin (as the plugin saves it): sets
                  the centres and ranges, then restarts InputPlumber
  restore         puts back what the pad had before the first apply

What the driver reports (drivers/input/joystick/rsinput.c): per stick axis
-raw + axis_*_center (negated again if the device tree says invert-*), with
no filtering. Every value here is in "centre-0 terms", the value with that
centre parameter at 0, so a calibration doesn't depend on the centres that
were set while it was measured. Apply then sets
  - axis_*_center so the resting stick reads 0;
  - each axis's range to ± its shorter half-travel (× EDGE): InputPlumber
    reads the range when it opens the pad and scales around its middle, so
    the range must be symmetric round the resting stick, and the shorter
    side must still reach the edge.
The pad is grabbed by InputPlumber, so no events reach a second reader:
rest and edges read each axis's current value with EVIOCGABS, which the
input core keeps up to date whoever has the grab. They poll, but only
while the plugin's calibration screen is open.

HOLD: while edges runs, InputPlumber's FilteredEvents keeps both sticks
from its virtual pad, so pushing them round doesn't move Steam's menus;
the buttons still reach Steam (Save, Cancel). edges puts the previous
filter back when it ends, however it ends short of SIGKILL, and at the
latest after MAX_S; apply's InputPlumber restart clears it too.

sm8550-fixpad (at boot and at each Steam session start) leaves the pad
alone while ACTIVE exists; ACTIVE is in /run, so it is gone after a restart.
"""
from __future__ import annotations

import fcntl
import glob
import json
import os
import signal
import struct
import subprocess
import sys
import time

PARAMS = os.environ.get("PBOS_STICKCAL_PARAMS", "/sys/module/rsinput/parameters")
RUN = os.environ.get("PBOS_STICKCAL_RUN", "/run/pbos-stickcal")
ACTIVE = os.path.join(RUN, "active")      # sm8550-fixpad checks this path
STOCK = os.path.join(RUN, "stock.json")   # the pad before the first apply
DT = os.environ.get("PBOS_STICKCAL_DT", "/proc/device-tree")
PHYS = os.environ.get("PBOS_STICKCAL_PHYS", "rsinput-gamepad/input0")
RESTART = os.environ.get("PBOS_STICKCAL_RESTART", "systemctl restart inputplumber.service")
BUSCTL = os.environ.get("PBOS_STICKCAL_BUSCTL", "busctl")
IP_BUS = "org.shadowblip.InputPlumber"
IP_IFACE = "org.shadowblip.Input.CompositeDevice"
HOLD_EVENTS = ("Gamepad:Axis:LeftStick", "Gamepad:Axis:RightStick")

EDGE = 0.98          # range = 98 % of the shorter half-travel
POLL_S = 0.005       # 200 Hz while measuring
REPORT_S = 0.1
MAX_REST = 300       # a resting stick further off than this is not at rest
MIN_HALF = 600       # each direction must reach at least this far
MAX_HALF = 2000

ABS_X, ABS_Y, ABS_RX, ABS_RY = 0, 1, 3, 4
# (name, driver parameter prefix, ABS code, device tree invert property)
AXES = (
    ("left_x", "axis_leftx", ABS_X, "invert-x"),
    ("left_y", "axis_lefty", ABS_Y, "invert-y"),
    ("right_x", "axis_rightx", ABS_RX, "invert-rx"),
    ("right_y", "axis_righty", ABS_RY, "invert-ry"),
)

ABSINFO = "iiiiii"
ABSINFO_SIZE = struct.calcsize(ABSINFO)


def out(obj: dict) -> None:
    print(json.dumps(obj, separators=(",", ":")), flush=True)


# ---------------------------------------------------------------- evdev --
def _ioc(direction: int, nr: int) -> int:
    return (direction << 30) | (ord("E") << 8) | nr | (ABSINFO_SIZE << 16)


def get_abs(fd: int, code: int) -> list[int]:
    buf = bytearray(ABSINFO_SIZE)
    fcntl.ioctl(fd, _ioc(2, 0x40 + code), buf)
    return list(struct.unpack(ABSINFO, buf))


def set_abs(fd: int, code: int, lo: int, hi: int) -> None:
    info = get_abs(fd, code)
    info[1], info[2] = lo, hi
    fcntl.ioctl(fd, _ioc(1, 0xC0 + code), struct.pack(ABSINFO, *info))


def pad() -> str | None:
    try:
        with open("/proc/bus/input/devices", encoding="utf-8", errors="replace") as fh:
            text = fh.read()
    except OSError:
        return None
    for block in text.split("\n\n"):
        if f"P: Phys={PHYS}" not in block:
            continue
        for line in block.splitlines():
            if line.startswith("H: Handlers="):
                for tok in line.split("=", 1)[1].split():
                    if tok.startswith("event"):
                        return f"/dev/input/{tok}"
    return None


# --------------------------------------------------------------- driver --
def read_param(name: str) -> int:
    with open(os.path.join(PARAMS, name), encoding="utf-8") as fh:
        return int(fh.read().strip())


def write_param(name: str, value: int) -> None:
    with open(os.path.join(PARAMS, name), "w", encoding="utf-8") as fh:
        fh.write(str(int(value)))


def inverted() -> dict[str, bool]:
    """Per axis: does the device tree tell rsinput to negate it?"""
    node = None
    for compat in glob.glob(os.path.join(DT, "**", "compatible"), recursive=True):
        try:
            with open(compat, "rb") as fh:
                if b"gamepad,rsinput" in fh.read():
                    node = os.path.dirname(compat)
                    break
        except OSError:
            continue
    return {name: bool(node) and os.path.exists(os.path.join(node, prop))
            for name, _, _, prop in AXES}


def sign(inv: dict[str, bool], name: str) -> int:
    return -1 if inv[name] else 1


def snapshot(fd: int) -> dict:
    """The driver's stick parameters and the pad's ranges now."""
    snap: dict = {"params": {}, "ranges": {}}
    for name, par, code, _ in AXES:
        for suffix in ("center", "min", "max"):
            snap["params"][f"{par}_{suffix}"] = read_param(f"{par}_{suffix}")
        _, lo, hi, *_ = get_abs(fd, code)
        snap["ranges"][name] = [lo, hi]
    return snap


def open_pad(flags: int = os.O_RDONLY) -> int:
    dev = pad()
    if not dev:
        out({"ok": False, "error": "no rsinput pad"})
        raise SystemExit(1)
    return os.open(dev, flags)


# ----------------------------------------------------------- InputPlumber --
def busctl(*args: str) -> str | None:
    if not BUSCTL:
        return None
    try:
        r = subprocess.run([BUSCTL, *args], capture_output=True, text=True, timeout=5)
    except (OSError, subprocess.TimeoutExpired):
        return None
    return r.stdout if r.returncode == 0 else None


def ip_get(path: str, prop: str):
    text = busctl("--json=short", "get-property", IP_BUS, path, IP_IFACE, prop)
    try:
        return json.loads(text)["data"] if text else None
    except (ValueError, KeyError):
        return None


def ip_composite(dev: str) -> str | None:
    """InputPlumber's composite device that has the pad as a source."""
    tree = busctl("tree", "--list", IP_BUS) or ""
    for path in tree.split():
        if path.rsplit("/", 1)[-1].startswith("CompositeDevice"):
            if dev in (ip_get(path, "SourceDevicePaths") or []):
                return path
    return None


def ip_set_filter(path: str, filt: dict[str, list[str]]) -> bool:
    args = ["set-property", IP_BUS, path, IP_IFACE, "FilteredEvents", "a{sas}", str(len(filt))]
    for source, events in filt.items():
        args += [source, str(len(events)), *events]
    return busctl(*args) is not None


class Hold:
    """Keeps the sticks from Steam (InputPlumber's FilteredEvents) while open."""

    def __init__(self, dev: str):
        self.path = ip_composite(dev)
        self.before: dict[str, list[str]] | None = None
        if not self.path:
            print("pbos_stickcal: no InputPlumber device for the pad, Steam still sees the sticks",
                  file=sys.stderr)
            return
        self.before = ip_get(self.path, "FilteredEvents") or {}
        source = "evdev://" + os.path.basename(dev)
        filt = {k: list(v) for k, v in self.before.items()}
        filt[source] = sorted(set(filt.get(source, [])) | set(HOLD_EVENTS))
        if not ip_set_filter(self.path, filt):
            print("pbos_stickcal: InputPlumber refused the stick filter", file=sys.stderr)
            self.before = None

    def release(self) -> None:
        if self.path and self.before is not None:
            ip_set_filter(self.path, self.before)
            self.before = None


# ------------------------------------------------------------- measuring --
def offsets() -> dict[str, int]:
    """Per axis: what its centre parameter adds to the value now."""
    inv = inverted()
    return {name: sign(inv, name) * read_param(f"{par}_center") for name, par, _, _ in AXES}


def centre0(fd: int, off: dict[str, int]) -> dict[str, int]:
    """Each axis now, as it would read with its centre parameter at 0."""
    return {name: get_abs(fd, code)[0] - off[name] for name, _, code, _ in AXES}


def rest(seconds: float) -> int:
    off = offsets()
    fd = open_pad()
    try:
        sums = {name: 0 for name, *_ in AXES}
        lo: dict[str, int] = {}
        hi: dict[str, int] = {}
        n = 0
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            for name, v in centre0(fd, off).items():
                sums[name] += v
                lo[name] = min(lo.get(name, v), v)
                hi[name] = max(hi.get(name, v), v)
            n += 1
            time.sleep(POLL_S)
    finally:
        os.close(fd)
    out({"ok": True, "samples": n,
         "axes": {name: {"mean": sums[name] / n, "lo": lo[name], "hi": hi[name]}
                  for name, *_ in AXES}})
    return 0


def edges(max_s: float) -> int:
    stop = False

    def done(*_):
        nonlocal stop
        stop = True

    # Before the hold: a SIGTERM from now on still ends in hold.release().
    signal.signal(signal.SIGTERM, done)
    signal.signal(signal.SIGINT, done)
    off = offsets()
    dev = pad()
    fd = open_pad()
    hold = Hold(dev)
    try:
        now = centre0(fd, off)
        lo, hi = dict(now), dict(now)
        end = time.monotonic() + max_s
        next_report = 0.0
        while not stop and time.monotonic() < end:
            now = centre0(fd, off)
            for name, v in now.items():
                lo[name] = min(lo[name], v)
                hi[name] = max(hi[name], v)
            t = time.monotonic()
            if t >= next_report:
                out({"now": now, "lo": lo, "hi": hi})
                next_report = t + REPORT_S
            time.sleep(POLL_S)
        out({"now": now, "lo": lo, "hi": hi, "done": True})
    finally:
        hold.release()
        os.close(fd)
    return 0


# --------------------------------------------------------------- applying --
def check(cal: dict) -> str | None:
    """Why a calibration can't be used, or None."""
    for name, *_ in AXES:
        a = cal.get(name)
        if not isinstance(a, dict) or not all(isinstance(a.get(k), (int, float))
                                              for k in ("rest", "lo", "hi")):
            return f"{name} missing"
        r, lo, hi = a["rest"], a["lo"], a["hi"]
        if abs(r) > MAX_REST:
            return f"{name} rests too far off centre ({r:.0f})"
        for half in (hi - r, r - lo):
            if not MIN_HALF <= half <= MAX_HALF:
                return f"{name} travel {half:.0f} is out of range"
    return None


def plan(cal: dict, inv: dict[str, bool]) -> dict[str, dict[str, int]]:
    """Per axis: the centre parameter and the half-range to set."""
    res = {}
    for name, *_ in AXES:
        a = cal[name]
        r = a["rest"]
        res[name] = {"center": -sign(inv, name) * round(r),
                     "range": int(min(a["hi"] - r, r - a["lo"]) * EDGE)}
    return res


def restart_inputplumber() -> str | None:
    if not RESTART:
        return None
    r = subprocess.run(RESTART.split(), capture_output=True, text=True)
    return None if r.returncode == 0 else (r.stderr.strip() or f"exit {r.returncode}")


def apply() -> int:
    cal = json.load(sys.stdin)
    why = check(cal)
    if why:
        out({"ok": False, "error": why})
        return 1
    inv = inverted()
    fd = open_pad(os.O_RDWR)
    try:
        os.makedirs(RUN, exist_ok=True)
        if not os.path.exists(STOCK):
            with open(STOCK + ".tmp", "w", encoding="utf-8") as fh:
                json.dump(snapshot(fd), fh)
            os.replace(STOCK + ".tmp", STOCK)
        # Flag first: a Steam session starting now must not put ±740 back.
        with open(ACTIVE, "w", encoding="utf-8") as fh:
            fh.write(f"{time.time():.0f}\n")
        p = plan(cal, inv)
        for name, par, code, _ in AXES:
            half = p[name]["range"]
            write_param(f"{par}_center", p[name]["center"])
            write_param(f"{par}_min", -half)
            write_param(f"{par}_max", half)
            set_abs(fd, code, -half, half)
    finally:
        os.close(fd)
    err = restart_inputplumber()
    out({"ok": True, "plan": p, "restart_error": err})
    return 0


def restore() -> int:
    try:
        with open(STOCK, encoding="utf-8") as fh:
            snap = json.load(fh)
    except OSError:
        snap = None
    if snap:
        fd = open_pad(os.O_RDWR)
        try:
            for name, value in snap["params"].items():
                write_param(name, value)
            for name, _, code, _ in AXES:
                lo, hi = snap["ranges"][name]
                set_abs(fd, code, lo, hi)
        finally:
            os.close(fd)
    for path in (ACTIVE, STOCK):
        try:
            os.remove(path)
        except FileNotFoundError:
            pass
    err = restart_inputplumber() if snap else None
    out({"ok": True, "restored": bool(snap), "restart_error": err})
    return 0


def status() -> int:
    dev = pad()
    res: dict = {"ok": True, "pad": dev, "active": os.path.exists(ACTIVE)}
    if dev:
        fd = os.open(dev, os.O_RDONLY)
        try:
            res.update(snapshot(fd))
        finally:
            os.close(fd)
    out(res)
    return 0


def main() -> int:
    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    if cmd == "status":
        return status()
    if cmd == "rest":
        return rest(float(sys.argv[2]) if len(sys.argv) > 2 else 1.5)
    if cmd == "edges":
        return edges(float(sys.argv[2]) if len(sys.argv) > 2 else 120)
    if cmd == "apply":
        return apply()
    if cmd == "restore":
        return restore()
    print("usage: pbos_stickcal.py status | rest [S] | edges [MAX_S] | apply < cal.json | restore",
          file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main())
