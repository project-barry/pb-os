#!/usr/bin/env python3
"""PB-OS Utils' light engine: the Retroid Pocket 6 / Nova stick lights and
the shared lighting definitions.

Decky's plugin loader is an x86-64 program run by box64 here, so the plugin
backend's Python is emulated: Breathing cost 46 % of a core there and under
1 % on the system's own Python (measured on the Nova). So the effects run
in their own process on /usr/bin/python3 (`python3 pbos_lights.py`, the
transient unit pbos-utils-lights, started by the plugin): it shows what
LIGHTS_STATE says, again on SIGHUP (the plugin sends one after each
change), and fades the lights back in when their devices come back after a
sleep. The plugin imports this module only for the definitions and the
saved settings.
"""
from __future__ import annotations

import array
import glob
import itertools
import json
import math
import os
import pwd
import random
import re
import select
import signal
import subprocess
import sys
import threading
import time
from typing import Any

PBOSD_STATE = "/var/lib/pbosd/state.json"
LIGHTS_STATE = "/var/lib/pbos-utils/lights.json"
# Retroid Pocket 6 / Nova: four RGB groups around each stick
# (leds-group-multicolor, functions l1..l4 and r1..r4).
MULTICOLOR = "/sys/class/leds/rgb:[lr][1-4]"


def log(msg: str) -> None:
    print(msg, file=sys.stderr, flush=True)


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


# ------------------------------------------------------------------ lights --
# Effects, named like the RGB apps gamers know (Armoury Crate, Synapse,
# iCUE). On the KONKR Pocket FIT the stick MCU renders them itself; on the
# Retroid Pocket 6 and Nova, Animator does, here. "color": the effect uses
# the picked colour; "speed": it has a speed; "turns": it goes round the
# sticks, and each stick's direction can be reversed.
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
        {"id": "wave", "label": "Rainbow Wave", "color": False, "speed": True, "turns": True},
        {"id": "spin", "label": "Spin", "color": True, "speed": True, "turns": True},
        {"id": "starlight", "label": "Starlight", "color": True, "speed": True},
    ],
}
# pbosd's state names for the MCU's modes.
PBOSD_MODES = {"static": "static", "breathing": "breath", "rainbow": "rainbow"}
DEFAULT_COLOR = "ff3c00"
DEFAULT_COLOR2 = "ffffff"        # the audio effects' intensity colour
# Retroid Pocket 6 / Nova out of the box (no saved settings yet): purple
# breathing, as tuned on a Nova (2026-10-08).
MULTICOLOR_DEFAULT_EFFECT = "breathing"
MULTICOLOR_DEFAULT_COLOR = "8c00ff"


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


# Experimental, switched on in the Lighting tab's Experimental section
# (Audio Effects); they follow what the device plays.
AUDIO_EFFECTS = [
    {"id": "audio", "label": "Audio Meter", "color": True, "speed": False, "experimental": True},
    {"id": "spectrum", "label": "Audio Spectrum", "color": True, "speed": False, "experimental": True},
]
AUDIO_MODES = {"audio": "pulse", "spectrum": "spectrum"}


def multicolor_effects(audio: bool) -> list[dict[str, Any]]:
    return EFFECTS["multicolor"] + (AUDIO_EFFECTS if audio else [])


def multicolor_lights() -> dict[str, Any]:
    st = read_json(LIGHTS_STATE)
    # Before effects there was a mode: static or off.
    on = st.get("on", st.get("mode") != "off")
    audio = bool(st.get("experimental_audio", False))
    effect = st.get("effect", MULTICOLOR_DEFAULT_EFFECT)
    if effect not in {e["id"] for e in multicolor_effects(audio)}:
        effect = "static"
    return {"on": bool(on), "effect": effect, "color": clean_color(st.get("color", MULTICOLOR_DEFAULT_COLOR)),
            "color2": clean_color(st.get("color2", DEFAULT_COLOR2)), "experimental_audio": audio, "effect_before_audio": st.get("effect_before_audio", "static"),
            "brightness": max(0, min(255, int(st.get("brightness", 160)))),
            "speed": max(1, min(10, int(st.get("speed", 5)))),
            "reverse_left": bool(st.get("reverse_left", False)), "reverse_right": bool(st.get("reverse_right", False)),
            "available": bool(glob.glob(MULTICOLOR))}


# Each ring clockwise from the top, left stick then right, by device model.
# The zones' numbers don't follow the rings the same way on every stick or
# device, though both share a device tree (mapped by eye 2026-10-07: on the
# RP6 the left ring sits a quarter turn on from the Nova's and the right is
# numbered the other way round).
RING_ORDERS = {
    "Retroid Pocket Nova": ("l3", "l2", "l1", "l4", "r4", "r1", "r2", "r3"),
    "Retroid Pocket 6": ("l2", "l1", "l4", "l3", "r3", "r2", "r1", "r4"),
}
RING_ORDER = RING_ORDERS["Retroid Pocket Nova"]


def ring_order() -> tuple[str, ...]:
    model = rd("/sys/firmware/devicetree/base/model").rstrip("\0")
    return next((o for name, o in RING_ORDERS.items() if model.startswith(name)), RING_ORDER)


def zones() -> list[str]:
    """The stick LEDs in ring order (ring_order()), so Spin and Rainbow Wave
    go round each stick, both sticks in step."""
    found = {p.rsplit(":", 1)[-1]: p for p in glob.glob(MULTICOLOR)}
    order = ring_order()
    if set(found) == set(order):
        return [found[z] for z in order]
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
# Audio effects, as the eye sees it: the base colour rests at a dim
# background (or its dimmest true level, if higher); loudness lifts a zone
# AUDIO_LEVEL_SHARE of the way from there to the brightness setting, and
# only hits go all the way, so they stand out over loud sound.
AUDIO_BACKGROUND = 0.2
AUDIO_LEVEL_SHARE = 0.6
# Audio Meter: the share of the level at which each place round a ring (0
# top, clockwise) fills: the bottom first, then both sides, then the top.
METER_FILL = ((2 / 3, 1.0), (1 / 3, 2 / 3), (0.0, 1 / 3), (1 / 3, 2 / 3))
# Audio Spectrum: which band (0 bass, 1 low mids, 2 high mids, 3 treble)
# each place round a ring shows (0 top, clockwise), per stick: bass at the
# bottom, treble at the top, low mids on the outer side, high mids inner.
SPECTRUM_PLACES = ((3, 2, 0, 1), (3, 1, 0, 2))


def even_hue(h: float) -> tuple[float, float, float]:
    """A full colour of hue h (0-1) at the same total power for every hue:
    red to green to blue and back, each pair crossing over linearly. The
    usual rainbow lights two channels fully for yellow, cyan and magenta,
    twice the power of red, green or blue, and looked brighter there."""
    h = (h % 1.0) * 3
    i, f = int(h) % 3, h - int(h)
    ch = [0.0, 0.0, 0.0]
    ch[i], ch[(i + 1) % 3] = 255 * (1 - f), 255 * f
    return tuple(ch)

# Seconds the stick lights take to come back after a sleep: the sleep hook's
# fade-out (sm8550-sleep), the other way round.
FADE_IN = 0.3
# A colour's main channels: those at least this share of its strongest one.
MAIN = 0.25
# How much light each channel gives the eye (Rec. 709 luma: red, green, blue).
LUMA = (0.2126, 0.7152, 0.0722)
# Breathing goes no dimmer than where the weakest main channel has this many
# power steps left.
FLOOR_STEPS = 4
# How far (in zones) a zone's light reaches during Spin: 1 = only two zones
# share the light, each handing over as the other takes it.
SPIN_WIDTH = 1.5


def frame(effect: str, t: float, rgb: tuple[int, int, int], speed: int, n: int,
          stars: list[float], floor: float = 0.0,
          reverse: tuple[bool, bool] = (False, False), level: Any = 0.0,
          rgb2: tuple[int, int, int] | None = None) -> list[tuple[int, int, int]]:
    """Each zone's colour at time t (seconds). Zones go round each ring of
    n // 2; both sticks show the same."""
    ring = max(1, n // 2)
    fast = (speed - 1) / 9

    def place(i):
        # Zone i's place round its ring (0 = top, clockwise); a reversed
        # stick is mirrored, so its light starts at the top and turns the
        # other way.
        p = i % ring
        return (ring - p) % ring if reverse[min(i // ring, 1)] else p                     # 0 slowest .. 1 fastest

    def scale(c, v):
        # v is how bright it should look; LEDs look bright at low power, so
        # the power is v ** 2.2 (as the sleep hook's fade), or fades would
        # seem to snap on and hang before going off.
        return tuple(x * v ** GAMMA for x in c)   # rounded once, in Animator

    if effect == "breathing":
        # Breathes between floor (Animator.floor(): the dimmest level that
        # still shows the colour) and full.
        # Steady through the dim end and slowing only towards full: a sine
        # lingered at the bottom, where one power step is a visible jump, so
        # it held there and then jumped (Nova, brightness 70).
        period = 8 - 6.5 * fast
        w = 1 - abs(2 * ((t / period) % 1) - 1)          # 0 -> 1 -> 0, straight
        v = floor + (1 - floor) * math.sin(math.pi / 2 * w)
        return [scale(rgb, v)] * n
    if effect == "cycle":
        return [even_hue(t / (24 - 21 * fast))] * n
    if effect == "wave":
        period = 6 - 5 * fast
        # Hues laid round the ring the other way from its places, so the
        # rainbow turns clockwise like Spin.
        return [even_hue(t / period + (ring - place(i)) % ring / ring) for i in range(n)]
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
            d = abs((head - place(i) + ring / 2) % ring - ring / 2)   # distance round the ring
            v = math.cos(math.pi * d / (2 * SPIN_WIDTH)) ** 2 if d < SPIN_WIDTH else 0.0
            out.append(scale(rgb, v))
        return out
    if effect in ("audio", "spectrum"):
        # level (AudioMeter.current()): (levels, accents), 0-1 per channel.
        # The zone rests at the background in the base colour; loudness
        # lifts it, a hit (accent) takes it towards rgb2, the intensity
        # colour, and up to full: the brightness setting is the peak.
        if isinstance(level, tuple) and len(level) == 2 and isinstance(level[0], (list, tuple)):
            lv, ac = list(level[0]), list(level[1])
        else:
            lv = list(level) if isinstance(level, (list, tuple)) else [level, level]
            ac = [0.0] * len(lv)
        hot = rgb2 or rgb
        out = []
        for i in range(n):
            side = min(i // ring, 1)
            if effect == "audio":
                # Audio Meter: each stick its side, filling its ring like a
                # VU meter, bottom, then both sides, then the top (the peak,
                # in the intensity colour); a beat kicks it up and turns the
                # lit LEDs towards the intensity colour.
                c = side if len(lv) > side else 0
                lvl = max(lv[c], ac[c])
                place = i % ring
                lo, hi = METER_FILL[place] if ring == 4 else (0.0, 1.0)
                f = min(1.0, max(0.0, (lvl - lo) / (hi - lo)))
                bg = max(floor, AUDIO_BACKGROUND)
                mix = 1.0 if (place == 0 and ring == 4) else ac[c]
                col = tuple(x + (y - x) * mix for x, y in zip(rgb, hot))
                out.append(scale(col if f else rgb, bg + (1 - bg) * f))
                continue
            else:                                 # each stick its side's four bands
                c = side * 4 + SPECTRUM_PLACES[side][i % ring] if len(lv) >= 8 else -1
            v_level, a = (lv[c], ac[c]) if c >= 0 else (0.0, 0.0)
            bg = max(floor, AUDIO_BACKGROUND)
            v = bg + (1 - bg) * max(AUDIO_LEVEL_SHARE * v_level, a)
            col = tuple(x + (y - x) * a for x, y in zip(rgb, hot))
            out.append(scale(col, v))
        return out
    if effect == "starlight":
        # Each zone twinkles on its own: every twinkle has its own length
        # (around `life`) and peak, and the wait before the next is drawn
        # like natural random events (exponential), so no rhythm forms.
        # stars[i] = [start, length, peak] of the zone's current or next
        # twinkle; the times are seconds, so the frame rate doesn't matter.
        life = 2.4 - 1.8 * fast
        out = []
        for i in range(n):
            if i >= len(stars) or not isinstance(stars[i], list):
                stars[i:i + 1] = [[t + random.expovariate(1 / life), 0.0, 0.0]]
            st = stars[i]
            if st[1] == 0.0 or t > st[0] + st[1]:    # new twinkle after a random wait
                st[0] = max(t, st[0] + st[1]) + random.expovariate(1 / (0.8 * life))
                st[1] = life * random.uniform(0.6, 1.4)
                st[2] = random.uniform(0.45, 1.0)
            age = t - st[0]
            v = st[2] * math.sin(math.pi * age / st[1]) if 0 <= age <= st[1] else 0.0
            out.append(scale(rgb, v))
        return out
    return [rgb] * n


# Audio Spectrum's bands come from plain listeners at falling sample rates:
# PipeWire's resampler drops everything above half the rate, in native code
# and steeply, so each listener holds the energy below one edge and the
# bands are differences (energy in separate ranges adds up). A tone lands
# in one band, the others 17-150 dB down. A filter-chain process did this
# first, but with it the first sound after the speaker suspended could not
# start for 5 s or more (every other sound, Nova, 15 tries); these: none.
#
# The edges, Hz: below the first nothing counts; then bass, low mids, high
# mids (a snare's snap), treble (hi-hats). Through the built-in speakers
# only what they can play counts: small handheld drivers give little below
# about SPEAKER_LOW_HZ (an estimate, not measured on these devices).
SPEAKER_LOW_HZ = 300
EDGES_FULL = (0, 250, 1000, 5000, 12000)
EDGES_SPEAKER = (SPEAKER_LOW_HZ, 600, 1500, 5000, 12000)


def on_speakers(sink: str) -> bool:
    """The sound goes out of the built-in speakers: the default output is the
    device's own card and nothing is in the headphone jack or video out."""
    if not sink.startswith("alsa_output.platform-sound"):
        return False                    # Bluetooth, USB, ...
    for jack in ("Headphone Jack", "DP0 Jack"):
        r = subprocess.run(["amixer", "-c", "0", "cget", f"iface=CARD,name={jack}"],
                           capture_output=True, text=True, timeout=5)
        if "values=on" in r.stdout:
            return False
    return True


def listener_rates(edges: tuple[int, ...]) -> tuple[int, ...]:
    """One listener per edge (none for 0): a rate of twice the edge."""
    return tuple(2 * e for e in edges if e)


try:
    # C speed (the Python loop cost 11 % of a core for 24 kHz stereo on the
    # Nova); in Python 3.12, gone in 3.13, hence the fallback.
    import warnings
    with warnings.catch_warnings():
        warnings.simplefilter("ignore", DeprecationWarning)
        import audioop
except ImportError:
    audioop = None


def mean_squares(frames: bytes) -> list[float]:
    """Mean square per side of stereo s16 sound."""
    if not frames:
        return [0.0, 0.0]
    if audioop:
        return [float(audioop.rms(audioop.tomono(frames, 2, 1, 0), 2)) ** 2,
                float(audioop.rms(audioop.tomono(frames, 2, 0, 1), 2)) ** 2]
    # Every 4th sample: still the mean square of the sound, for a quarter.
    s = array.array("h", frames)
    out = []
    for side in (0, 1):
        part = s[side::8]
        out.append(sum(x * x for x in part) / len(part) if part else 0.0)
    return out


class AudioMeter:
    """How the device's sound output moves, for the audio effects, as the
    user that owns the sound session; never a microphone.

    "pulse" (Audio Meter): the default output's monitor in stereo at 24 kHz
    (through the speakers less a listener at the lowest edge); channels
    [left, right]. "spectrum" (Audio Spectrum):
    listeners at listener_rates(EDGES_SPEAKER or EDGES_FULL, by
    on_speakers()); channels [left bass, low mids, high mids, treble, right
    bass, ...].

    Per channel it gives a level (loudness in dB against its own loudest of
    the last few seconds, rising at once, falling over 0.3 s; in "spectrum"
    a band far below its side's loudest stays dark) and an accent: a hit, a
    sudden rise against its own recent average, which flashes and fades. The
    high mids and treble (snare, hi-hats) hit easiest and fastest and take
    their full level from a narrower loudness range, so they show most.

    Every listener is passive (node.passive): it never keeps the sound card
    running, so with nothing playing it gets no data and the light rests at
    its glow; and can't be moved (node.dont-move), so the sleep hook's
    shuffle of streams onto its silent stand-in can't land it on another
    source after a wake. They run only while an audio effect shows, and
    start again if one ends, or the output or its kind (speakers or not)
    changes. pw-record runs unbuffered: it writes a pipe in 4 kB blocks, and
    measured on the Nova the light came 0.9 s after the sound; unbuffered
    about 0.1 s."""

    # Per band (spectrum) or for the meter (pulse): the quietest that lights,
    # below its loudest; the rise over its recent average that is a hit; how
    # long a hit fades; how long a level falls. The meter falls fast, so it
    # drops between beats instead of trailing them.
    RANGE_DB = (30.0, 30.0, 22.0, 22.0)
    METER_RANGE_DB = 24.0
    METER_HIT_DB = 6.0
    METER_HIT_FADE = 0.15
    METER_FALL = 0.12
    SPECTRUM_FALL = 0.15
    GATE_DB = 15.0                # spectrum: a band this far below its side's loudest is dark
    HIT_DB = (9.0, 9.0, 5.0, 5.0)       # per band: rise over its recent average that is a hit
    HIT_FADE = (0.30, 0.30, 0.15, 0.12)  # per band: seconds a hit fades over
    # A hit is a rise over the band's average of about the last HIT_AVG
    # seconds (floored at -60 dB): short, so a held note stops counting
    # within a few frames (over 0.5 s, a tone after silence held a zone at
    # full for a second on the Nova).
    HIT_AVG = 0.12
    WINDOW = 1 / 30               # seconds of sound each energy is measured over

    def __init__(self) -> None:
        self.lock = threading.Lock()
        self.procs: list[subprocess.Popen] = []
        self.running = threading.Event()
        self.stopped = threading.Event()     # ends the waits between tries at once
        self.thread: threading.Thread | None = None
        self.mode = "pulse"
        self.edges = EDGES_FULL
        self._reset()

    def _reset(self) -> None:
        n = 2 if self.mode == "pulse" else 8
        self.levels = [0.0] * n
        self.accents = [0.0] * n
        self.peaks = [-60.0] * n
        self.averages = [-60.0] * n
        self.at = 0.0                 # when the levels were last measured

    def start(self, mode: str = "pulse") -> None:
        if self.running.is_set() and mode == self.mode:
            return
        self.stop()
        self.mode = mode if mode in ("pulse", "spectrum") else "pulse"
        with self.lock:
            self._reset()
        self.stopped.clear()
        self.running.set()
        self.thread = threading.Thread(target=self._supervise, name="pbos-audio", daemon=True)
        self.thread.start()

    def stop(self) -> None:
        self.running.clear()
        self.stopped.set()
        self._kill()
        if self.thread:
            self.thread.join(timeout=2)
            self.thread = None
        with self.lock:
            self.levels = [0.0] * len(self.levels)
            self.accents = [0.0] * len(self.accents)

    def current(self) -> tuple[list[float], list[float]]:
        """(levels, accents) now; they fall away when no sound has come."""
        with self.lock:
            return self.current_unlocked(time.monotonic())

    def _band(self, c: int) -> int:
        return 0 if self.mode == "pulse" else c % 4

    def _range(self, c: int) -> float:
        return self.METER_RANGE_DB if self.mode == "pulse" else self.RANGE_DB[c % 4]

    def _hit_db(self, c: int) -> float:
        return self.METER_HIT_DB if self.mode == "pulse" else self.HIT_DB[c % 4]

    def _hit_fade(self, c: int) -> float:
        return self.METER_HIT_FADE if self.mode == "pulse" else self.HIT_FADE[c % 4]

    def _fall(self) -> float:
        return self.METER_FALL if self.mode == "pulse" else self.SPECTRUM_FALL

    def current_unlocked(self, now: float) -> tuple[list[float], list[float]]:
        if not self.at:
            return [0.0] * len(self.levels), [0.0] * len(self.accents)
        idle = max(0.0, now - self.at)
        k = math.exp(-max(0.0, idle - 0.05) / self._fall())
        acc = [a * math.exp(-idle / self._hit_fade(c)) for c, a in enumerate(self.accents)]
        return [v * k for v in self.levels], acc

    def _kill(self) -> None:
        procs, self.procs = self.procs, []
        for p in procs:
            if p.poll() is None:
                p.terminate()
        for p in procs:
            try:
                p.wait(timeout=1)
            except subprocess.TimeoutExpired:
                p.kill()

    @staticmethod
    def _session() -> tuple[str, str] | None:
        """The user with the sound session (Game Mode's steamos) and its runtime dir."""
        for user in ("steamos",):
            try:
                uid = pwd.getpwnam(user).pw_uid
            except KeyError:
                continue
            rt = f"/run/user/{uid}"
            if os.path.exists(f"{rt}/pipewire-0"):
                return user, rt
        return None

    @staticmethod
    def _default_sink(user: str, rt: str) -> str:
        r = subprocess.run(["runuser", "-u", user, "--", "env", f"XDG_RUNTIME_DIR={rt}", "pactl", "get-default-sink"],
                           capture_output=True, text=True, timeout=5)
        return r.stdout.strip()

    @staticmethod
    def _command(user: str, rt: str, rate: int) -> list[str]:
        return ["runuser", "-u", user, "--", "env", f"XDG_RUNTIME_DIR={rt}", "stdbuf", "-o0",
                "pw-record", "--raw", "--rate", str(rate), "--channels", "2", "--format", "s16", "--latency", "16ms",
                "-P", "{ stream.capture.sink=true node.passive=true node.dont-move=true "
                      f"node.name=pbos-utils-lighting-{rate} media.name=\"PB-OS Utils lighting\" }}", "-"]

    def _rates(self) -> tuple[int, ...]:
        if self.mode == "pulse":
            # Up to 12 kHz, the hi-hats too (at 8 kHz, so below 4 kHz, they
            # never showed); through the speakers less what they can't play
            # (a listener at the lowest edge, subtracted).
            top = 2 * self.edges[-1]
            return (2 * self.edges[0], top) if self.edges[0] else (top,)
        return listener_rates(self.edges)

    def _supervise(self) -> None:
        while self.running.is_set():
            sess = self._session()
            if not sess:
                self.stopped.wait(5)
                continue
            user, rt = sess
            try:
                sink = self._default_sink(user, rt)
                speakers = on_speakers(sink)
                self.edges = EDGES_SPEAKER if speakers else EDGES_FULL
                self.procs = [subprocess.Popen(self._command(user, rt, rate), stdout=subprocess.PIPE,
                                               stderr=subprocess.DEVNULL) for rate in self._rates()]
            except (OSError, subprocess.SubprocessError) as e:
                log(f"Audio effects: can't listen: {e}")
                self._kill()
                self.stopped.wait(5)
                continue
            self._read(user, rt, sink, speakers)
            self._kill()
            self.stopped.wait(1)

    def _read(self, user: str, rt: str, sink: str, speakers: bool) -> None:
        rates = self._rates()
        fds = {p.stdout.fileno(): i for i, p in enumerate(self.procs)}
        bufs = [b""] * len(rates)
        # Each listener: its last WINDOW of sound (stereo s16 bytes), and its
        # energy (mean square) per side.
        windows = [b""] * len(rates)
        energy = [[0.0, 0.0] for _ in rates]
        checked = time.monotonic()
        while self.running.is_set():
            ready, _, _ = select.select(list(fds), [], [], 1.0)
            for fd in ready:
                i = fds[fd]
                data = os.read(fd, 1 << 16)
                if not data:
                    return              # a listener ended: start them all again
                bufs[i] += data
                n = len(bufs[i]) // 4 * 4
                keep = max(8, int(rates[i] * self.WINDOW)) * 4
                windows[i] = (windows[i] + bufs[i][:n])[-keep:]
                bufs[i] = bufs[i][n:]
                energy[i] = mean_squares(windows[i])
                if i == len(rates) - 1:     # the fastest listener: once per frame
                    self._measure(energy)
            if time.monotonic() - checked > 5:
                checked = time.monotonic()
                now_sink = self._default_sink(user, rt)
                if now_sink != sink or on_speakers(now_sink) != speakers:
                    return              # other output, or headphones in or out: listen again

    def band_power(self, energy: list[list[float]]) -> list[float]:
        """Mean-square power per channel from the listeners' energies."""
        if self.mode == "pulse":
            if len(energy) == 2:            # the speakers: less what they can't play
                return [max(0.0, energy[1][s] - energy[0][s]) for s in (0, 1)]
            return [energy[0][0], energy[0][1]]
        # e[k]: energy below the k-th non-zero edge; with a lowest edge (the
        # speakers) everything below it is taken away first.
        power = []
        skip = 1 if self.edges[0] else 0
        for side in (0, 1):
            e = [0.0] * (1 - skip) + [energy[i][side] for i in range(len(energy))]
            power += [max(0.0, e[k + 1] - e[k]) for k in range(4)]
        return power

    def _measure(self, energy: list[list[float]]) -> None:
        full = 32768.0 ** 2
        dbs = [10 * math.log10(max(x / full, 1e-12)) for x in self.band_power(energy)]
        now = time.monotonic()
        with self.lock:
            dt = now - self.at if self.at else 0.0
            prev, prev_acc = self.current_unlocked(now)
            n = len(dbs)
            side = n // 2 if n > 2 else 1
            for c, db in enumerate(dbs):
                # The loudest of the last few seconds, falling 3 dB a second.
                self.peaks[c] = max(db, self.peaks[c] - 3 * dt, -60.0)
                p, rng = self.peaks[c], self._range(c)
                target = 0.0 if p <= -55 else min(1.0, max(0.0, (db - (p - rng)) / rng))
                gated = n > 2 and db < max(dbs[c // side * side:(c // side + 1) * side]) - self.GATE_DB
                if gated:
                    target = 0.0
                self.levels[c] = target if target >= prev[c] else prev[c] + (target - prev[c]) * min(1.0, dt / self._fall())
                # A hit: a sudden rise over the band's own recent average.
                rise = db - self.averages[c]
                acc = prev_acc[c]
                if not gated and p > -55 and rise > self._hit_db(c):
                    acc = max(acc, min(1.0, 0.5 + (rise - self._hit_db(c)) / 12))
                self.accents[c] = acc
                self.averages[c] = max(-60.0, self.averages[c] + (db - self.averages[c]) * min(1.0, dt / self.HIT_AVG))
            self.at = now


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
        self.tops: dict[str, int] = {}
        self.level = 1.0
        self.meter = AudioMeter()

    def apply(self, st: dict[str, Any], fade: float = 0.0) -> None:
        """Show st; with fade, brighten from off to it over that many seconds
        (the effect already running underneath)."""
        self.halt()
        self.failed = set()
        self.order, self.tops = {}, {}   # read again: the devices may be new
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
            first = [0, 0, 0] if fade else frame(effect, 0.0, rgb, int(st.get("speed", 5)), len(leds), [-100.0] * len(leds),
                                                  0.0, (bool(st.get("reverse_left")), bool(st.get("reverse_right"))))[i]
            self._write(led, "multi_intensity", self.power(led, first))
            self._write(led, "brightness", rd(f"{led}/max_brightness", "255"))
        if effect == "static" and not fade:
            return
        self.stop = threading.Event()
        reverse = (bool(st.get("reverse_left")), bool(st.get("reverse_right")))
        rgb2 = rgb_of(clean_color(st.get("color2", DEFAULT_COLOR2)))
        self.thread = threading.Thread(target=self._run, args=(leds, effect, rgb, int(st.get("speed", 5)), self.stop, fade, reverse, rgb2),
                                       name="pbos-lights", daemon=True)
        self.thread.start()

    def halt(self) -> None:
        if self.thread:
            self.stop.set()
            self.thread.join(timeout=2)
            self.thread = None
        self.meter.stop()

    def _run(self, leds: list[str], effect: str, rgb, speed: int, stop: threading.Event,
             fade: float = 0.0, reverse: tuple[bool, bool] = (False, False), rgb2=None) -> None:
        shown: list[Any] = [None] * len(leds)
        stars: list[Any] = [None] * len(leds)
        audio = AUDIO_MODES.get(effect)
        floor = self.floor(leds[0], rgb) if leds and (effect == "breathing" or audio) else 0.0
        fine = effect in ("breathing", "starlight") or bool(audio)    # fades: power_smooth
        if audio:
            self.meter.start(audio)
        smooth: dict[str, Any] = {}     # Breathing: last pick per zone (power_smooth)
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
            level = self.meter.current() if audio else 0.0
            for i, c in enumerate(frame(effect, k / self.FPS, rgb, speed, len(leds), stars, floor, reverse, level, rgb2)):
                c = [x * ramp for x in c]
                value = self.power_smooth(leds[i], c, smooth) if fine else self.power(leds[i], c)
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

    def top(self, led: str) -> int:
        """The zone's max_brightness, read once per apply (each power() runs
        30 times a second per zone)."""
        if led not in self.tops:
            self.tops[led] = int(rd(f"{led}/max_brightness", "255") or 255)
        return self.tops[led]

    def channels(self, led: str) -> list[int]:
        """Where red, green and blue go in multi_intensity: its multi_index,
        which on the Retroid Pocket 6 / Nova is "blue green red"."""
        if led not in self.order:
            names = rd(f"{led}/multi_index").split()
            self.order[led] = [("red", "green", "blue").index(n) for n in names] \
                if sorted(names) == ["blue", "green", "red"] else [0, 1, 2]
        return self.order[led]

    def floor(self, led: str, rgb) -> float:
        """The dimmest level (as it looks, 0-1) at which rgb at the set
        brightness still has FLOOR_STEPS power steps in its weakest main
        channel. Below that the few whole steps left can't hold the colour
        (a pink at brightness 70 turned red: blue 1, red 3) and every step is
        a visible jump."""
        top = self.top(led)
        main = [c * self.level * top / 255 for c in rgb if c and c >= MAIN * max(rgb)]
        if not main or min(main) <= FLOOR_STEPS:
            return 1.0 if main else 0.0
        return (FLOOR_STEPS / min(main)) ** (1 / GAMMA)

    def power_smooth(self, led: str, rgb, last: dict[str, Any]) -> str:
        """power() for a slow fade: each channel rounded up or down, picking
        the mix whose light (LUMA) is nearest the target, which gives about
        twice as many brightness steps at the dim end as rounding each
        channel alone (Nova, pink at 50: largest step 25 % -> 13 %). While
        the target dims the light may only stay or dim, and the other way
        round, so it never wobbles. No channel is more than one step off."""
        top = self.top(led)
        want = [c * self.level * top / 255 for c in rgb]
        light = lambda ch: sum(w * x for w, x in zip(LUMA, ch))
        main = [i for i, x in enumerate(want) if x and x >= MAIN * max(want)]
        prev, prev_want = last.get(led, (None, None))
        going = 0 if prev_want is None else (light(want) > light(prev_want)) - (light(want) < light(prev_want))
        best = None
        for ch in itertools.product(*[(math.floor(x), math.ceil(x)) for x in want]):
            if any(ch) and not all(ch[i] for i in main):
                continue                                # a main channel dark: tinted
            if prev is not None and going * (light(ch) - light(prev)) < -1e-9:
                continue                                # against the fade
            cost = (light(ch) - light(want)) ** 2 + 0.02 * sum((a - b) ** 2 for a, b in zip(ch, want))
            if best is None or cost < best[0]:
                best = (cost, ch)
        ch = best[1] if best else (prev or (0, 0, 0))
        last[led] = (ch, want)
        return " ".join(str(ch[i]) for i in self.channels(led))

    def power(self, led: str, rgb) -> str:
        """multi_intensity for rgb (0-255, may be fractional) at the set
        brightness: each channel's final power, rounded once. Near the end of
        a fade a main channel of the colour (MAIN of the strongest or more)
        would round to 0 while another is still lit, tinting the zone, so the
        zone is off from there."""
        top = self.top(led)
        ch = [c * self.level * top / 255 for c in rgb]
        strongest = max(ch)
        if any(x < 0.5 for x in ch if x >= MAIN * strongest):
            ch = [0.0, 0.0, 0.0]
        return " ".join(str(round(ch[i])) for i in self.channels(led))

    def _write(self, led: str, attr: str, value: Any) -> None:
        try:
            with open(f"{led}/{attr}", "w") as fh:
                fh.write(str(value))
        except OSError as e:
            # Gone while the controller is unbound for sleep: say so once.
            if led not in self.failed:
                self.failed.add(led)
                log(f"{led}/{attr}: {e}")


# ------------------------------------------------------------------ engine --
class Engine:
    """Shows LIGHTS_STATE on the stick lights: at start, on reload() (SIGHUP
    from the plugin), and again, fading in, when the LED devices were made
    again (the SM8550 sleep test unbinds their controllers for sleep; they
    come back white and off, and the sleep hook's own fade-in finds none)."""

    def __init__(self) -> None:
        self.animator = Animator()
        self.last = lights_signature()
        self.changed_at = 0.0

    def reload(self, fade: float = 0.0) -> None:
        self.animator.apply(multicolor_lights(), fade)

    def poll(self) -> bool:
        """Every 0.5 s: True when it set the lights again after a sleep."""
        now = lights_signature()
        if now == self.last or 0 in now or len(now) != len(self.last):
            self.changed_at = 0.0
            return False                # same devices, or still coming back
        if not self.changed_at:
            self.changed_at = time.monotonic()
            return False
        if time.monotonic() - self.changed_at < 0.3:
            return False                # let them settle
        self.last, self.changed_at = now, 0.0
        log("stick lights: set again after sleep")
        self.reload(FADE_IN)
        return True


def run() -> None:
    if lights_kind() != "multicolor":
        log("no stick lights to drive here")
        return
    wake = threading.Event()
    stop = threading.Event()
    reload = threading.Event()
    signal.signal(signal.SIGHUP, lambda *_: (reload.set(), wake.set()))
    signal.signal(signal.SIGTERM, lambda *_: (stop.set(), wake.set()))
    signal.signal(signal.SIGINT, lambda *_: (stop.set(), wake.set()))
    engine = Engine()
    engine.reload()
    while not stop.is_set():
        wake.wait(0.5)
        wake.clear()
        if reload.is_set():
            reload.clear()
            engine.reload()
        else:
            engine.poll()
    engine.animator.halt()


if __name__ == "__main__":
    run()
