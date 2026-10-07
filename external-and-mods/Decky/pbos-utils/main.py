#!/usr/bin/env python3
"""Decky backend — PB-OS Utils: pb-os tools for Game Mode, in tabs.

Update   pb-os updates (was the PB-OS Update plugin). Front for
         /usr/share/konkr-update/konkr-update.py (the SteamOS Update app's
         helper). The download and staging run as the transient unit
         pbos-update, so they carry on when Quick Access closes or Steam
         restarts; this reads their log. The update installs on the next
         restart. Without internet, an update on a microSD card or USB drive
         works the same way: this looks at drives when one is put in or taken
         out (local-check) and installs from it with local-update. Each start
         frees what finished updates left on HOME (cleanup).

Install  moves SteamOS between the microSD card and internal storage, with
         the Easy UFS Installer's scripts: from the card,
         install-masios-to-internal.sh; from internal storage, copy-to-sd.sh.
         Either runs as the transient unit pbos-utils-move, with sleep held
         off, and writes its log to MOVE_LOG.

Lights   stick lighting: on/off, a colour, brightness and effects. On the
         KONKR Pocket FIT pbosd owns it (its state file, applied on SIGHUP,
         shared with the K button and pbosctl) and the stick MCU renders the
         effects; on the Retroid Pocket 6 and Nova this drives the multicolor
         LEDs itself (Animator) and sets them again at each start. The AYN
         Thor's lights stay in Barry Launcher.
"""
from __future__ import annotations

import asyncio
import colorsys
import glob
import json
import math
import os
import random
import re
import subprocess
import threading
import time
from typing import Any

import decky

UPDATER = "/usr/share/konkr-update/konkr-update.py"
UNIT = "pbos-update"
LOG = "/run/pbos-update.log"
PENDING = "/var/lib/konkr-update/pending"
VERSION = "/usr/share/pb-os/version"
CHECK_EVERY = 6 * 3600
# Drives come and go: compare the block devices this often.
DRIVES_EVERY = 3

UFS = "/usr/share/easy-ufs-install"
TO_INTERNAL = f"{UFS}/install-masios-to-internal.sh"
TO_SD = f"{UFS}/copy-to-sd.sh"
PROBE_INTERNAL = f"{UFS}/ufs-probe-sizes.sh"
HOME_COPY = f"{UFS}/home-copy.sh"
MOVE_UNIT = "pbos-utils-move"
MOVE_LOG = "/run/pbos-utils-move.log"
MOVE_ARGS = "/run/pbos-utils-move.json"
# What the scripts print as "[ufs] ..." -> the step shown, in order. Step 2
# is the first that writes.
STEPS = {
    "internal": [
        (r"\[ufs\] .*: internal disk", "Checking the device and internal storage"),
        (r"\[ufs\] repartitioning", "Repartitioning internal storage"),
        (r"\[ufs\] formatting", "Formatting the new partitions"),
        (r"\[ufs\] copying the system", "Copying the system"),
        (r"\[ufs\] copying .*/home", "Copying the home folder"),
        (r"\[ufs\] installing KERNEL", "Installing the boot kernel"),
        (r"\[ufs\] checking the result", "Checking the result"),
    ],
    "sd": [
        (r"\[ufs\] .*: microSD card", "Checking the device and the card"),
        (r"\[ufs\] preparing the card", "Unmounting the card"),
        (r"\[ufs\] partitioning the card", "Partitioning the card"),
        (r"\[ufs\] formatting", "Formatting the card"),
        (r"\[ufs\] copying the system", "Copying the system"),
        (r"\[ufs\] copying .*/home", "Copying the home folder"),
        (r"\[ufs\] installing KERNEL", "Installing the boot kernel"),
        (r"\[ufs\] checking the result", "Checking the result"),
    ],
}
ANSI = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
PERCENT = re.compile(r"\s(\d{1,3})%\s")
# Sizes of /home for the "what to bring" choice; du over a big game library
# takes a while, so they are kept this long.
SIZES_TTL = 600

PBOSD_STATE = "/var/lib/pbosd/state.json"
LIGHTS_STATE = "/var/lib/pbos-utils/lights.json"
# Retroid Pocket 6 / Nova: four RGB groups around each stick
# (leds-group-multicolor, functions l1..l4 and r1..r4).
MULTICOLOR = "/sys/class/leds/rgb:[lr][1-4]"
CLEAN_ENV = {"PATH": "/usr/bin:/usr/sbin", "LANG": "C.UTF-8"}


def rd(path: str, default: str = "") -> str:
    try:
        with open(path, encoding="utf-8") as fh:
            return fh.read().strip()
    except OSError:
        return default


def write_json(path: str, data: dict[str, Any]) -> None:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path + ".tmp", "w", encoding="utf-8") as fh:
        json.dump(data, fh, indent=2)
    os.replace(path + ".tmp", path)


def read_json(path: str) -> dict[str, Any]:
    try:
        with open(path, encoding="utf-8") as fh:
            data = json.load(fh)
        return data if isinstance(data, dict) else {}
    except (OSError, ValueError):
        return {}


def active(unit: str) -> bool:
    r = subprocess.run(["systemctl", "is-active", f"{unit}.service"], capture_output=True, text=True)
    return r.stdout.strip() in ("active", "activating", "deactivating")


def keyvals(text: str) -> dict[str, str]:
    out: dict[str, str] = {}
    for line in text.splitlines():
        k, sep, v = line.partition("=")
        if sep and re.fullmatch(r"[A-Z_]+", k):
            out[k] = v
    return out


# ------------------------------------------------------------------ update --
def update_job() -> dict[str, Any]:
    """What the last download/staging run printed."""
    st: dict[str, Any] = {"step": "", "have": 0, "total": 0, "dl_have": 0, "dl_total": 0,
                          "error": "", "staged": False, "note": "", "drive_done": False}
    for line in rd(LOG).splitlines():
        if line.startswith("PROGRESS "):
            parts = line.split()
            if len(parts) == 3:
                st["have"], st["total"] = int(parts[1]), int(parts[2])
                if st["step"].startswith("Downloading"):  # what a resumed download continues from
                    st["dl_have"], st["dl_total"] = st["have"], st["total"]
        elif line.startswith("STEP "):
            # Each step (download, comparing, unpacking, backing up) has its own bar.
            st["step"] = line[5:]; st["have"] = st["total"] = 0
        elif line.startswith("ERROR: "):
            st["error"] = line[7:]
        elif line.startswith("Update staged"):
            st["staged"] = True
        elif line == "DRIVE_DONE":
            st["drive_done"] = True
        elif line.startswith("pb-os ") and line.endswith("is already installed"):
            st["note"] = line
    return st


def ask_updater(command: str, timeout: int) -> dict[str, Any]:
    # Decky's own Python environment must not leak into the system one.
    r = subprocess.run(["/usr/bin/python3", UPDATER, command], capture_output=True, text=True,
                       timeout=timeout, env=CLEAN_ENV)
    if r.returncode != 0:
        err = (r.stderr or r.stdout).strip().splitlines()
        return {"ok": False, "error": (err[-1] if err else "check failed").removeprefix("ERROR: "), "time": time.time()}
    return {"ok": True, **json.loads(r.stdout), "time": time.time()}


def cleanup() -> None:
    """Free what finished updates (from any version) left on HOME; the updater
    skips it while an update is being prepared."""
    r = subprocess.run(["/usr/bin/python3", UPDATER, "cleanup"], capture_output=True, text=True,
                       timeout=600, env=CLEAN_ENV)
    out = (r.stdout + r.stderr).strip()
    if out:
        decky.logger.info(out)


def block_devices() -> list[str]:
    try:
        return sorted(os.listdir("/sys/class/block"))
    except OSError:
        return []


# ----------------------------------------------------------------- install --
def running_from() -> str:
    """"sd" or "internal": the disk / is on."""
    r = subprocess.run(["findmnt", "-no", "SOURCE", "/"], capture_output=True, text=True)
    src = r.stdout.strip()
    r = subprocess.run(["lsblk", "-no", "PKNAME", src], capture_output=True, text=True)
    disk = (r.stdout.split() or [""])[0]
    return "sd" if disk.startswith("mmcblk") else "internal"


def run_probe(argv: list[str]) -> dict[str, str]:
    try:
        r = subprocess.run(argv, capture_output=True, text=True, timeout=60, env=CLEAN_ENV)
    except subprocess.TimeoutExpired:
        return {"ERROR": "the check took too long"}
    out = keyvals(r.stdout)
    if r.returncode != 0 and "ERROR" not in out:
        err = (r.stderr or r.stdout).strip().splitlines()
        out["ERROR"] = ANSI.sub("", err[-1] if err else f"exit {r.returncode}").split("ERROR:", 1)[-1].strip()
    return out


def probe_internal() -> dict[str, str]:
    return run_probe(["/usr/bin/bash", PROBE_INTERNAL])


def probe_card() -> dict[str, str]:
    return run_probe(["/usr/bin/bash", TO_SD, "--probe"])


def home_sizes() -> dict[str, int]:
    r = subprocess.run(["/usr/bin/bash", HOME_COPY, "sizes"], capture_output=True, text=True,
                       timeout=900, env=CLEAN_ENV)
    kv = keyvals(r.stdout)
    total = int(kv.get("HOME_ALL_BYTES") or 0)
    games = int(kv.get("HOME_GAMES_BYTES") or 0)
    return {"all": total, "essentials": max(0, total - games), "none": max(0, total - games), "time": time.time()}


class MoveLog:
    """Follows MOVE_LOG as it grows: rsync's progress lines arrive by the
    thousand, so each read starts where the last stopped."""

    def __init__(self) -> None:
        self.lock = threading.Lock()
        self.reset()

    def reset(self) -> None:
        self.ino = None
        self.pos = 0
        self.buf = ""
        self.step = 0
        self.pct = 0
        self.error = ""
        self.exit: int | None = None
        self.lines: list[str] = []

    def read(self, direction: str) -> dict[str, Any]:
        with self.lock:
            return self._read(direction)

    def _read(self, direction: str) -> dict[str, Any]:
        try:
            st = os.stat(MOVE_LOG)
        except OSError:
            self.reset()
            return self.state()
        if st.st_ino != self.ino or st.st_size < self.pos:
            self.reset()
            self.ino = st.st_ino
        if st.st_size > self.pos:
            with open(MOVE_LOG, "rb") as fh:
                fh.seek(self.pos)
                data = fh.read(st.st_size - self.pos)
            self.pos += len(data)
            self.buf += data.decode("utf-8", "replace")
            self._parse(direction)
        return self.state()

    def _parse(self, direction: str) -> None:
        steps = STEPS.get(direction, [])
        *done, self.buf = re.split(r"[\r\n]", self.buf)
        for seg in done:
            seg = ANSI.sub("", seg)
            m = PERCENT.search(f" {seg} ")
            if m and "[ufs]" not in seg:
                self.pct = min(int(m.group(1)), 100)
                continue
            for i, (pattern, _label) in enumerate(steps, start=1):
                if i > self.step and re.search(pattern, seg):
                    self.step, self.pct = i, 0
            if "ERROR:" in seg:
                self.error = seg.split("ERROR:", 1)[1].strip()
            if seg.startswith("PBOS_EXIT="):
                self.exit = int(seg[10:] or 1)
            elif seg.strip():
                self.lines = (self.lines + [seg])[-6:]

    def state(self) -> dict[str, Any]:
        return {"step": self.step, "pct": self.pct, "error": self.error,
                "exit": self.exit, "lines": self.lines}


# ------------------------------------------------------------------ lights --
# Effects, named like the RGB apps gamers know (Armoury Crate, Synapse,
# iCUE). On the KONKR Pocket FIT the stick MCU renders them itself; on the
# Retroid Pocket 6 and Nova, Animator does, here. "color": the effect uses
# the picked colour; "speed": it has a speed.
EFFECTS = {
    "pbosd": [
        {"id": "static", "label": "Static", "color": True, "speed": False},
        {"id": "breathing", "label": "Breathing", "color": True, "speed": False},
        {"id": "rainbow", "label": "Rainbow", "color": False, "speed": False},
    ],
    "multicolor": [
        {"id": "static", "label": "Static", "color": True, "speed": False},
        {"id": "breathing", "label": "Breathing", "color": True, "speed": True},
        {"id": "cycle", "label": "Color Cycle", "color": False, "speed": True},
        {"id": "wave", "label": "Rainbow Wave", "color": False, "speed": True},
        {"id": "spin", "label": "Spin", "color": True, "speed": True},
        {"id": "starlight", "label": "Starlight", "color": True, "speed": True},
    ],
}
# pbosd's state names for the MCU's modes.
PBOSD_MODES = {"static": "static", "breathing": "breath", "rainbow": "rainbow"}
DEFAULT_COLOR = "ff3c00"


def lights_kind() -> str:
    compat = rd("/sys/firmware/devicetree/base/compatible")
    if "ayn,thor" in compat:
        return ""   # Barry Launcher's Lights tab
    # pbosd ships in the SM8550 image too, but only runs on these (its unit's
    # ExecCondition).
    if re.search(r"KONKR Pocket FIT|AYANEO Pocket S2", rd("/sys/firmware/devicetree/base/model")):
        return "pbosd"
    if glob.glob(MULTICOLOR):
        return "multicolor"
    return ""


def clean_color(color: Any) -> str:
    color = str(color).lstrip("#").lower()[:6]
    return color if re.fullmatch(r"[0-9a-f]{6}", color) else DEFAULT_COLOR


def rgb_of(color: str) -> tuple[int, int, int]:
    return tuple(int(color[i:i + 2], 16) for i in (0, 2, 4))  # type: ignore[return-value]


def pbosd_lights() -> dict[str, Any]:
    st = read_json(PBOSD_STATE)
    rgb = st.get("rgb") or {}
    mode = rgb.get("mode", "static")
    on = mode != "off"
    # Off keeps what was on in last_mode/last_color, as pbosd's K button does.
    shown = mode if on else rgb.get("last_mode", "static")
    effect = next((e for e, m in PBOSD_MODES.items() if m == shown), "static")
    color = rgb.get("color") if on else rgb.get("last_color", rgb.get("color"))
    return {"on": on, "effect": effect, "color": clean_color(color or DEFAULT_COLOR),
            "brightness": int(rgb.get("brightness", 160)), "speed": 5,
            "power_led": st.get("power_led", True) is not False,
            "available": bool(glob.glob("/sys/class/leds/*joysticks*")),
            "daemon": subprocess.run(["systemctl", "is-active", "--quiet", "pbosd"]).returncode == 0}


def pbosd_set(**changes: Any) -> None:
    # Only the lighting keys: PB-OS Control writes the same file for the rest.
    st = read_json(PBOSD_STATE)
    st.update(changes)
    write_json(PBOSD_STATE, st)
    subprocess.run(["systemctl", "kill", "-s", "HUP", "pbosd.service"], check=False)


def pbosd_lights_set(on: bool, effect: str, color: str, brightness: int) -> None:
    old = read_json(PBOSD_STATE).get("rgb") or {}
    mode = PBOSD_MODES.get(effect, "static")
    if on:
        rgb = {"mode": mode, "color": color, "brightness": brightness}
    else:
        rgb = {"mode": "off", "color": color, "brightness": brightness,
               "last_mode": mode, "last_color": color}
    for k in ("last_mode", "last_color"):
        if on and k in old:
            rgb[k] = old[k]
    pbosd_set(rgb=rgb)


def multicolor_lights() -> dict[str, Any]:
    st = read_json(LIGHTS_STATE)
    # Before effects there was a mode: static or off.
    on = st.get("on", st.get("mode") != "off")
    effect = st.get("effect", "static")
    if effect not in {e["id"] for e in EFFECTS["multicolor"]}:
        effect = "static"
    return {"on": bool(on), "effect": effect, "color": clean_color(st.get("color", DEFAULT_COLOR)),
            "brightness": max(0, min(255, int(st.get("brightness", 160)))),
            "speed": max(1, min(10, int(st.get("speed", 5)))), "available": bool(glob.glob(MULTICOLOR))}


# Each ring clockwise from the top, left stick then right. The device tree
# numbers the zones differently on each stick (seen on the Retroid Pocket
# Nova, 2026-10-07; the RP6 has the same tree).
RING_ORDER = ("l3", "l2", "l1", "l4", "r4", "r1", "r2", "r3")


def zones() -> list[str]:
    """The stick LEDs in ring order (RING_ORDER), so Spin and Rainbow Wave
    go round each stick."""
    found = {p.rsplit(":", 1)[-1]: p for p in glob.glob(MULTICOLOR)}
    if set(found) == set(RING_ORDER):
        return [found[z] for z in RING_ORDER]
    return sorted(found.values(), key=lambda p: (p[-2], p[-1]))


def lights_signature() -> tuple[int, ...]:
    """Changes when the stick LED devices were made again, which loses what
    was set (white and off): the SM8550 sleep test unbinds their controllers
    for sleep. A plain sleep keeps the LEDs; the sleep hook fades them."""
    inodes = []
    for z in zones():
        try:
            inodes.append(os.stat(z).st_ino)
        except OSError:
            inodes.append(0)
    return tuple(inodes)


GAMMA = 2.2
# Seconds the stick lights take to come back after a sleep: the sleep hook's
# fade-out (sm8550-sleep), the other way round.
FADE_IN = 0.3
# A colour's main channels: those at least this share of its strongest one.
MAIN = 0.25
# How far (in zones) a zone's light reaches during Spin: 1 = only two zones
# share the light, each handing over as the other takes it.
SPIN_WIDTH = 1.5


def frame(effect: str, t: float, rgb: tuple[int, int, int], speed: int, n: int,
          stars: list[float]) -> list[tuple[int, int, int]]:
    """Each zone's colour at time t (seconds). Zones go round each ring of
    n // 2; both sticks show the same."""
    ring = max(1, n // 2)
    fast = (speed - 1) / 9                     # 0 slowest .. 1 fastest

    def scale(c, v):
        # v is how bright it should look; LEDs look bright at low power, so
        # the power is v ** 2.2 (as the sleep hook's fade), or fades would
        # seem to snap on and hang before going off.
        return tuple(x * v ** GAMMA for x in c)   # rounded once, in Animator

    def hue(h):
        return tuple(int(x * 255) for x in colorsys.hsv_to_rgb(h % 1.0, 1.0, 1.0))

    if effect == "breathing":
        period = 8 - 6.5 * fast
        v = 0.04 + 0.96 * (1 - math.cos(2 * math.pi * t / period)) / 2
        return [scale(rgb, v)] * n
    if effect == "cycle":
        return [hue(t / (24 - 21 * fast))] * n
    if effect == "wave":
        period = 6 - 5 * fast
        return [hue(t / period + (i % ring) / ring) for i in range(n)]
    if effect == "spin":
        # One light going round. Each zone fades up as the light comes within
        # SPIN_WIDTH zones of it and down as it leaves, so the next zone is
        # already rising while this one peaks and the last is still fading:
        # through the Nova's clear shell, no gap between zones. The raised
        # cosines add up to the same total wherever the light is.
        # One turn takes 6 s at speed 1 down to 0.5 s at 10, each notch the
        # same factor faster.
        head = (t / (6 * (0.5 / 6) ** fast)) * ring
        out = []
        for i in range(n):
            d = abs((head - i % ring + ring / 2) % ring - ring / 2)   # distance round the ring
            v = math.cos(math.pi * d / (2 * SPIN_WIDTH)) ** 2 if d < SPIN_WIDTH else 0.0
            out.append(scale(rgb, v))
        return out
    if effect == "starlight":
        # Each zone lights up at random, fades in and out over `life`.
        life = 2.4 - 1.8 * fast
        out = []
        for i in range(n):
            age = t - stars[i]
            if age > life:
                if random.random() < 0.02 + 0.05 * fast:
                    stars[i] = t
                    age = 0.0
            v = math.sin(math.pi * age / life) if 0 <= age <= life else 0.0
            out.append(scale(rgb, v))
        return out
    return [rgb] * n


class Animator:
    """Drives the Retroid Pocket 6 / Nova stick LEDs. Static and off are
    written once; effects run in a thread at FPS, writing only zones whose
    colour changed (each write is an I2C transfer to the LED driver)."""

    # Measured on the Nova (Spin, 20 s each): 20 and 30 fps both cost ~1.7 %
    # of one core, 60 fps 3.7 %; a frame's writes take under 1 ms. Battery
    # draw was below what the rest of the system varies by at all three.
    FPS = 30

    def __init__(self) -> None:
        self.thread: threading.Thread | None = None
        self.stop = threading.Event()
        self.failed: set[str] = set()
        self.order: dict[str, list[int]] = {}
        self.level = 1.0

    def apply(self, st: dict[str, Any], fade: float = 0.0) -> None:
        """Show st; with fade, brighten from off to it over that many seconds
        (the effect already running underneath)."""
        self.halt()
        self.failed = set()
        self.order = {}   # read again: the devices may be new
        leds = zones()
        on = st.get("on", True)
        rgb = rgb_of(clean_color(st.get("color", DEFAULT_COLOR)))
        # The zones' own brightness stays at full and the set brightness goes
        # into the channels (power()): the kernel would otherwise scale each
        # channel again and round it down, and a fading pink came out as red
        # alone near the end (blue already 0, red still 1).
        self.level = max(0, min(255, int(st.get("brightness", 160)))) / 255
        if not on:
            for led in leds:
                self._write(led, "brightness", 0)
            return
        effect = st.get("effect", "static")
        # The colour first, then the brightness: LEDs made again (after a
        # sleep) start at full white, and brightness first flashed it.
        for i, led in enumerate(leds):
            first = [0, 0, 0] if fade else frame(effect, 0.0, rgb, int(st.get("speed", 5)), len(leds), [-100.0] * len(leds))[i]
            self._write(led, "multi_intensity", self.power(led, first))
            self._write(led, "brightness", rd(f"{led}/max_brightness", "255"))
        if effect == "static" and not fade:
            return
        self.stop = threading.Event()
        self.thread = threading.Thread(target=self._run, args=(leds, effect, rgb, int(st.get("speed", 5)), self.stop, fade),
                                       name="pbos-lights", daemon=True)
        self.thread.start()

    def halt(self) -> None:
        if self.thread:
            self.stop.set()
            self.thread.join(timeout=2)
            self.thread = None

    def _run(self, leds: list[str], effect: str, rgb, speed: int, stop: threading.Event,
             fade: float = 0.0) -> None:
        shown: list[Any] = [None] * len(leds)
        stars = [-100.0] * len(leds)
        # Frame k is drawn for exactly k / FPS and written at that time, so
        # the steps are even and the rate is FPS, not FPS minus the time the
        # writes take. Behind by more than a frame (busy, or paused): skip
        # to the current one rather than rush to catch up.
        t0 = time.monotonic()
        k = 0
        while not stop.is_set():
            # Fading in: the sleep hook's fade-out reversed (x ** 2.2).
            r = min(1.0, k / self.FPS / fade) if fade else 1.0
            ramp = r ** GAMMA
            for i, c in enumerate(frame(effect, k / self.FPS, rgb, speed, len(leds), stars)):
                value = self.power(leds[i], [x * ramp for x in c])
                if value != shown[i]:
                    self._write(leds[i], "multi_intensity", value)
                    shown[i] = value
            if effect == "static" and r >= 1:
                return          # faded in; static needs no more frames
            if self.failed:
                return          # LEDs gone (unbound for sleep): the fade-in starts it again
            k += 1
            late = time.monotonic() - (t0 + k / self.FPS)
            if late > 1 / self.FPS:
                k += int(late * self.FPS)
            stop.wait(max(0.0, t0 + k / self.FPS - time.monotonic()))

    def power(self, led: str, rgb) -> str:
        """multi_intensity for rgb (0-255, may be fractional) at the set
        brightness: each channel's final power, rounded once. Near the end of
        a fade a main channel of the colour (MAIN of the strongest or more)
        would round to 0 while another is still lit, tinting the zone, so the
        zone is off from there."""
        top = int(rd(f"{led}/max_brightness", "255") or 255)
        # multi_intensity follows the zone's multi_index, which on the Retroid
        # Pocket 6 / Nova is "blue green red".
        if led not in self.order:
            names = rd(f"{led}/multi_index").split()
            self.order[led] = [("red", "green", "blue").index(n) for n in names] \
                if sorted(names) == ["blue", "green", "red"] else [0, 1, 2]
        ch = [c * self.level * top / 255 for c in rgb]
        strongest = max(ch)
        if any(x < 0.5 for x in ch if x >= MAIN * strongest):
            ch = [0.0, 0.0, 0.0]
        return " ".join(str(round(ch[i])) for i in self.order[led])

    def _write(self, led: str, attr: str, value: Any) -> None:
        try:
            with open(f"{led}/{attr}", "w") as fh:
                fh.write(str(value))
        except OSError as e:
            # Gone while the controller is unbound for sleep: say so once.
            if led not in self.failed:
                self.failed.add(led)
                decky.logger.info(f"{led}/{attr}: {e}")


class Plugin:
    async def _main(self) -> None:
        self.last: dict[str, Any] = {}
        self.local: dict[str, Any] = {}
        self.announced = ""
        self.announced_local = ""
        self.card: dict[str, str] | None = None
        self.internal: dict[str, str] | None = None
        self.sizes: dict[str, int] | None = None
        self.sizing = False
        self.movelog = MoveLog()
        self.move_was_running = await asyncio.to_thread(active, MOVE_UNIT)
        self.lights = await asyncio.to_thread(lights_kind)
        self.animator = Animator()
        if self.lights == "multicolor":
            await asyncio.to_thread(self.animator.apply, await asyncio.to_thread(multicolor_lights))
        self.tasks = [asyncio.create_task(t) for t in
                      (self._watch(), self._watch_drives(), self._cleanup(), self._watch_move(),
                       self._watch_lights())]

    async def _unload(self) -> None:
        for t in self.tasks:
            t.cancel()
        await asyncio.to_thread(self.animator.halt)

    # -------------------------------------------------------------- update --
    async def _cleanup(self) -> None:
        # Once per start, after Game Mode has settled.
        await asyncio.sleep(30)
        try:
            await asyncio.to_thread(cleanup)
        except Exception as e:
            decky.logger.info(f"cleanup failed: {e}")

    async def _watch_drives(self) -> None:
        # A drive put in or taken out changes the block devices; looking at
        # the drives (which mounts them) does not.
        seen: list[str] = []
        while True:
            now = await asyncio.to_thread(block_devices)
            if now != seen:
                seen = now
                self.card = None  # the Install tab looks again
                await asyncio.sleep(2)  # let the drive settle (partitions, SteamOS's own mount)
                try:
                    if not await asyncio.to_thread(active, MOVE_UNIT):
                        await self.check_local()
                except Exception as e:
                    decky.logger.info(f"drive check failed: {e}")
            await asyncio.sleep(DRIVES_EVERY)

    async def _watch(self) -> None:
        # Look now and then; a new release gets one toast per version.
        await asyncio.sleep(60)
        while True:
            try:
                await self.check()
            except Exception as e:  # offline, GitHub down: try again later
                decky.logger.info(f"update check failed: {e}")
            await asyncio.sleep(CHECK_EVERY)

    async def check(self, **_: Any) -> dict[str, Any]:
        self.last = await asyncio.to_thread(ask_updater, "check", 90)
        up = self.last.get("update")
        if self.last.get("available") and up and up["version"] != self.announced and not os.path.exists(PENDING):
            self.announced = up["version"]
            await decky.emit("pbos_update_available", up["title"])
        return self.last

    async def check_local(self, **_: Any) -> dict[str, Any]:
        if await asyncio.to_thread(active, UNIT):
            return self.local  # it may be copying from the drive right now
        self.local = await asyncio.to_thread(ask_updater, "local-check", 60)
        up = self.local.get("update")
        key = f"{up['version']} {up['drive']}" if up else ""
        if up and key != self.announced_local and not os.path.exists(PENDING):
            await decky.emit("pbos_local_available", up["version"], up["where"])
        self.announced_local = key
        return self.local

    async def get_state(self, **_: Any) -> dict[str, Any]:
        return {
            "installed": rd(VERSION) or "unknown",
            "release": self.last,
            "local": self.local,
            "running": await asyncio.to_thread(active, UNIT),
            "pending": os.path.exists(PENDING),
            "job": await asyncio.to_thread(update_job),
            "lights": self.lights,
        }

    async def start(self, **_: Any) -> bool:
        return await self._run_update("update")

    async def start_local(self, **_: Any) -> bool:
        return await self._run_update("local-update")

    async def _run_update(self, command: str) -> bool:
        if await asyncio.to_thread(active, UNIT) or os.path.exists(PENDING) \
                or await asyncio.to_thread(active, MOVE_UNIT):
            return False
        try:
            os.remove(LOG)
        except FileNotFoundError:
            pass
        subprocess.run(["systemctl", "reset-failed", f"{UNIT}.service"], capture_output=True)
        r = subprocess.run(["systemd-run", f"--unit={UNIT}", "--collect", "-p", f"StandardOutput=file:{LOG}",
                            "-p", "StandardError=inherit", "/usr/bin/python3", "-u", UPDATER, command],
                           capture_output=True, text=True)
        return r.returncode == 0

    # Only while downloading: the download resumes on the next start. Staging
    # is not interrupted (it writes the boot image at the end).
    async def pause(self, **_: Any) -> bool:
        if update_job()["step"].startswith("Downloading"):
            subprocess.run(["systemctl", "stop", f"{UNIT}.service"], check=False)
            return True
        return False

    async def restart(self, **_: Any) -> bool:
        if not os.path.exists(PENDING):
            return False
        subprocess.Popen(["systemctl", "reboot"])
        return True

    # ------------------------------------------------------------- install --
    async def _watch_move(self) -> None:
        # A toast when a move ends, with Quick Access closed too.
        while True:
            await asyncio.sleep(2)
            now = await asyncio.to_thread(active, MOVE_UNIT)
            if self.move_was_running and not now:
                args = read_json(MOVE_ARGS)
                job = await asyncio.to_thread(self.movelog.read, args.get("direction", ""))
                await decky.emit("pbos_move_done", args.get("direction", ""), job["exit"] == 0)
                self.internal = self.card = None
            self.move_was_running = now

    async def get_install(self, refresh: bool = False, **_: Any) -> dict[str, Any]:
        where = await asyncio.to_thread(running_from)
        running = await asyncio.to_thread(active, MOVE_UNIT)
        args = read_json(MOVE_ARGS)
        job = await asyncio.to_thread(self.movelog.read, args.get("direction", ""))
        steps = STEPS.get(args.get("direction", ""), [])
        job["count"] = len(steps)
        job["label"] = steps[job["step"] - 1][1] if 0 < job["step"] <= len(steps) else "Starting"
        out: dict[str, Any] = {"from": where, "running": running, "args": args, "job": job,
                               "sizes": self.sizes, "updating": await asyncio.to_thread(active, UNIT)}
        if running:
            return out
        if where == "sd":
            if refresh or self.internal is None:
                self.internal = await asyncio.to_thread(probe_internal)
            out["internal"] = self.internal
        else:
            if refresh or self.card is None:
                self.card = await asyncio.to_thread(probe_card)
            out["card"] = self.card
        stale = not self.sizes or time.time() - self.sizes.get("time", 0) > SIZES_TTL
        if (refresh or stale) and not self.sizing:
            self.sizing = True
            asyncio.create_task(self._size_home())
        return out

    async def _size_home(self) -> None:
        try:
            self.sizes = await asyncio.to_thread(home_sizes)
        except Exception as e:
            decky.logger.info(f"home sizes failed: {e}")
        finally:
            self.sizing = False

    async def start_move(self, direction: str = "", home: str = "all", android_gb: int = 0,
                         expect: str = "", **_: Any) -> dict[str, Any]:
        if await asyncio.to_thread(active, MOVE_UNIT):
            return {"ok": False, "error": "a copy is already running"}
        if await asyncio.to_thread(active, UNIT):
            return {"ok": False, "error": "an update is being prepared; wait until it is ready"}
        if home not in ("all", "essentials", "none"):
            return {"ok": False, "error": "unknown home choice"}
        where = await asyncio.to_thread(running_from)
        if direction == "internal" and where == "sd":
            argv = ["/usr/bin/bash", TO_INTERNAL, "--force", "--android-gb", str(int(android_gb)),
                    "--expect", expect, "--home", home]
        elif direction == "sd" and where == "internal":
            argv = ["/usr/bin/bash", TO_SD, "--force", "--expect", expect, "--home", home]
        else:
            return {"ok": False, "error": "SteamOS moved since this was checked; check again"}
        for path in (MOVE_LOG, MOVE_ARGS):
            try:
                os.remove(path)
            except FileNotFoundError:
                pass
        stamp = time.strftime("%Y%m%d-%H%M%S")
        write_json(MOVE_ARGS, {"direction": direction, "home": home, "android_gb": int(android_gb),
                               "started": time.time(), "saved_log": f"/home/steamos/pbos-move-{stamp}.log"})
        # The log goes to HOME afterwards too, for a report; sleep, the power
        # button and the lid are held off until the script ends.
        wrapper = ('systemd-inhibit --what=sleep:idle:shutdown:handle-power-key:handle-suspend-key:handle-lid-switch '
                   '--who="PB-OS Utils" --why="Copying SteamOS" --mode=block "$@"; rc=$?; '
                   'echo "PBOS_EXIT=$rc"; '
                   f'install -o 1000 -g 1000 -m 0644 {MOVE_LOG} /home/steamos/pbos-move-{stamp}.log 2>/dev/null; '
                   'exit $rc')
        subprocess.run(["systemctl", "reset-failed", f"{MOVE_UNIT}.service"], capture_output=True)
        r = subprocess.run(["systemd-run", f"--unit={MOVE_UNIT}", "--collect",
                            "-p", f"StandardOutput=file:{MOVE_LOG}", "-p", "StandardError=inherit",
                            "-E", "PATH=/usr/sbin:/usr/bin:/sbin:/bin", "-E", "LANG=C.UTF-8",
                            "/usr/bin/bash", "-c", wrapper, "pbos-utils-move", *argv],
                           capture_output=True, text=True)
        if r.returncode != 0:
            return {"ok": False, "error": (r.stderr or "could not start").strip().splitlines()[-1]}
        with self.movelog.lock:
            self.movelog.reset()
        self.move_was_running = True
        return {"ok": True}

    async def power_off(self, **_: Any) -> bool:
        if await asyncio.to_thread(active, MOVE_UNIT):
            return False
        subprocess.Popen(["systemctl", "poweroff"])
        return True

    # -------------------------------------------------------------- lights --
    async def _watch_lights(self) -> None:
        # pbosd puts the KONKR lights back itself; the multicolor ones are set
        # again here when their devices come back after a sleep, fading in
        # (the sleep hook's own fade-in found no LEDs).
        if self.lights != "multicolor":
            return
        last = await asyncio.to_thread(lights_signature)
        while True:
            await asyncio.sleep(0.5)
            now = await asyncio.to_thread(lights_signature)
            if now == last:
                continue
            if 0 in now or len(now) != len(last):
                continue        # still coming back
            await asyncio.sleep(0.3)
            if await asyncio.to_thread(lights_signature) != now:
                continue
            last = now
            decky.logger.info("stick lights: set again after sleep")
            await asyncio.to_thread(self.animator.apply, await asyncio.to_thread(multicolor_lights), FADE_IN)

    async def get_lights(self, **_: Any) -> dict[str, Any]:
        if self.lights == "pbosd":
            st = await asyncio.to_thread(pbosd_lights)
        elif self.lights == "multicolor":
            st = await asyncio.to_thread(multicolor_lights)
        else:
            return {"kind": ""}
        return {"kind": self.lights, "effects": EFFECTS[self.lights], **st}

    async def set_lights(self, on: bool = True, effect: str = "static", color: str = DEFAULT_COLOR,
                         brightness: int = 160, speed: int = 5, **_: Any) -> dict[str, Any]:
        if self.lights not in EFFECTS:
            return {"kind": ""}
        if effect not in {e["id"] for e in EFFECTS[self.lights]}:
            effect = "static"
        color = clean_color(color)
        brightness = max(0, min(255, int(brightness)))
        speed = max(1, min(10, int(speed)))
        if self.lights == "pbosd":
            await asyncio.to_thread(pbosd_lights_set, bool(on), effect, color, brightness)
        else:
            st = {"on": bool(on), "effect": effect, "color": color, "brightness": brightness, "speed": speed}
            await asyncio.to_thread(write_json, LIGHTS_STATE, st)
            await asyncio.to_thread(self.animator.apply, st)
        return await self.get_lights()

    async def set_power_led(self, on: bool = True, **_: Any) -> dict[str, Any]:
        if self.lights == "pbosd":
            await asyncio.to_thread(pbosd_set, power_led=bool(on))
        return await self.get_lights()
