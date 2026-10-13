#!/usr/bin/env python3
"""Decky backend — PB-OS Tools (plugin pbos-utils): pb-os tools for Game Mode, in tabs.

Games    per-game profiles (port of Armada's per-game compatibility
         settings, armada-os/armada, GPL-2.0-or-later): an x86 emulation
         (FEX) preset or the user's own FEX switches, the CPU cores the game
         runs on (with Wine's CPU topology to match), and environment
         variables, from a list of common ones or the user's own. Kept in
         ~/.config/pb-os/game-profiles.json; /usr/bin/pbos-game-launch applies
         them when the game starts. The frontend puts the wrapper in a game's
         launch options when the game gets a profile and takes it out when
         the profile is reset; games without one are never touched.

Update   pb-os updates (was the PB-OS Update plugin). Front for
         /usr/share/konkr-update/konkr-update.py (the SteamOS Update app's
         helper). The download and staging run as the transient unit
         pbos-update, so they carry on when Quick Access closes or Steam
         restarts; this reads their log. The update installs on the next
         restart. Without internet, an update on a microSD card or USB drive,
         or downloaded to ~/Downloads in Desktop Mode, works the same way:
         this looks when a drive is put in or taken out or the Downloads
         folder changes (local-check) and installs from there with
         local-update, which then deletes it from ~/Downloads. Each start
         frees what finished updates left on HOME (cleanup).

Install  moves SteamOS between the microSD card and internal storage, with
         the Easy UFS Installer's scripts: from the card,
         install-masios-to-internal.sh; from internal storage, copy-to-sd.sh.
         Either runs as the transient unit pbos-utils-move, with sleep held
         off, and writes its log to MOVE_LOG.

Lights   stick lighting: on/off, a color, brightness and effects. On the
         KONKR Pocket FIT pbosd owns it (its state file, applied on SIGHUP,
         shared with the K button and pbosctl) and the stick MCU renders the
         effects; on the Retroid Pocket 6 and Nova the light engine
         (py_modules/pbos_lights.py, its own unit on the system's native
         Python) drives the multicolor LEDs and sets them again after a
         sleep; this saves the settings and nudges it. The AYN Thor's lights
         stay in Barry Launcher.

Performance and Hardware (KONKR Pocket FIT and AYANEO Pocket S2; were the
         PB-OS Control plugin): the performance profile, the fan, the extra
         buttons and the controller MCU link. A front for pbosd: every
         setting lives in its state file and pbosd applies it on SIGHUP, so
         the buttons, pbosctl and this panel stay in sync. A profile change
         from anywhere shows a toast (pbos_mode).

Power    the charging indicator (SM8550: Retroid Pocket 6 / Nova, AYN Thor):
         while the device rests on the charger (sm8550-charge-rest, instead
         of kernel sleep), the stick lights pulse until it's charged, then
         stay lit. Its settings and ranges live in sm8550-charge-rest
         (`settings`, `save`); Preview shows it once.

Proton   follows Valve's Proton pick per game: Valve's Proton 11.0 (ARM64)
         -> Proton 11.0-2 PB-OS (ARM), Proton Experimental (ARM64) ->
         Proton Experimental PB-OS (ARM); set per game like a user's pick,
         since Valve's pick outranks Steam's global default. Games on the
         Steam Linux Runtime (Linux ports) and games the user set stay as they
         are. The global default becomes Proton 11.0-2 PB-OS (ARM) once, if
         it is still Valve's at the first check; that decision is recorded
         ("global_set") and the global default is never touched again, so
         whatever the user picks later stays. Event-driven, from the
         frontend: Steam start, game installs/updates, Play. /usr/lib/steamos/pbos-proton builds the tools.

Face buttons (Hardware tab), for people who physically moved the button
         caps: Nintendo positions (A on the right; the default on the Retroid
         Pocket 6 / Nova and AYN Thor, printed that way) or Xbox positions
         (A at the bottom; the default on the KONKR Pocket FIT). A front for
         /usr/lib/steamos/sm8550-face-buttons, which swaps the InputPlumber
         map and restarts InputPlumber; its unit re-applies the choice at
         boot.

Sticks    (Hardware tab on the Retroid Pocket 6 / Nova and AYN Thor; was the
         Stick Calibration plugin): "Use my calibration" (AYN's Android stick
         calibration in Game Mode: let go of the sticks, push them round the
         edge, save; each stick's center and range are set from that) and
         "Fine stick steps" (the controller's finer reporting), both kept
         across restarts. The work is done by
         /usr/lib/steamos-sm8550/sm8550-stickcal on the system's native
         Python, which also keeps the switches and sets them again at boot
         (sm8550-stickcal.service).

Desktop  (Hardware tab on the AYN Thor): Barry Launcher on the bottom
         screen in Desktop Mode, or off, which makes the bottom screen a
         plain desktop screen (the taskbar, Plasma's keyboard). Game Mode
         keeps Barry. /usr/lib/barry_launcher/barry_launcher_desktop_switch
         keeps the setting and applies it; Desktop Mode has the same switch
         as "Barry Launcher On/Off" in its applications menu.
"""
from __future__ import annotations

import asyncio
import glob
import json
import os
import pwd
import re
import subprocess
import sys
import threading
import time
from typing import Any

import decky

# The light engine module, shared with its native process (see ENGINE).
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "py_modules"))
from pbos_lights import (  # noqa: E402
    AUDIO_MODES, DEFAULT_COLOR, DEFAULT_EFFECT, EFFECTS, LIGHTS_STATE, clean_color, lights_kind,
    PBOSD_STATE, multicolor_effects, multicolor_lights, pbosd_lights, pbosd_lights_set, pbosd_set)

UPDATER = "/usr/share/konkr-update/konkr-update.py"
UNIT = "pbos-update"
LOG = "/run/pbos-update.log"
PENDING = "/var/lib/konkr-update/pending"
VERSION = "/usr/share/pb-os/version"
CHECK_EVERY = 6 * 3600
# Drives come and go: compare the block devices this often.
DRIVES_EVERY = 3
DOWNLOADS = "/home/steamos/Downloads"

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


def downloaded() -> list[tuple[str, int]]:
    """Update files in the Downloads folder, with their sizes. A browser's
    file in progress (.part, .crdownload) counts by name only, so a running
    download does not set off a check every few seconds."""
    out: list[tuple[str, int]] = []
    try:
        with os.scandir(DOWNLOADS) as it:
            for e in it:
                if e.name.startswith(("pb-os-", "SHA256SUMS")):
                    try:
                        busy = e.name.endswith((".part", ".crdownload", ".download"))
                        out.append((e.name, 0 if busy else e.stat(follow_symlinks=False).st_size))
                    except OSError:
                        pass
    except OSError:
        pass
    return sorted(out)


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


# The light engine (py_modules/pbos_lights.py) runs on the system's native
# Python as its own unit: the plugin's Python is emulated (box64), where the
# effects cost up to half a core. It reads LIGHTS_STATE; SIGHUP after a change.
ENGINE_UNIT = "pbos-utils-lights"
ENGINE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "py_modules", "pbos_lights.py")


def start_engine() -> None:
    """(Re)start the engine, so it runs this plugin's code."""
    subprocess.run(["systemctl", "stop", f"{ENGINE_UNIT}.service"], capture_output=True)
    subprocess.run(["systemctl", "reset-failed", f"{ENGINE_UNIT}.service"], capture_output=True)
    r = subprocess.run(["systemd-run", f"--unit={ENGINE_UNIT}", "--collect", "-p", "Restart=on-failure",
                        "-p", "RestartSec=2", "/usr/bin/python3", "-u", ENGINE],
                       capture_output=True, text=True, env=CLEAN_ENV)
    if r.returncode != 0:
        decky.logger.info(f"light engine: {r.stderr.strip()}")


def stop_engine() -> None:
    subprocess.run(["systemctl", "stop", f"{ENGINE_UNIT}.service"], capture_output=True)


def nudge_engine() -> None:
    """Show the saved settings now."""
    if not active(ENGINE_UNIT):
        start_engine()
        return
    subprocess.run(["systemctl", "kill", "-s", "HUP", f"{ENGINE_UNIT}.service"], capture_output=True)


# ------------------------------------------------- performance, hardware --
# The KONKR Pocket FIT's pbosd settings (was the PB-OS Control plugin), with
# pbosd's own defaults.
MCU_BLACKLIST = "/etc/modprobe.d/konkr-mcu.conf"
PROFILES = ("lowpower", "balanced")
LEGACY_PROFILES = {"silent": "lowpower", "turbo": "balanced"}
BUTTON_ACTIONS = ("profile-next", "rgb-next", "sticks-toggle", "none")
BUTTON_MODES = ("steam", "system")
# The Performance button changes the profile in pbosd directly: look this often.
PROFILE_WATCH_S = 0.25


def control_state() -> dict[str, Any]:
    st = read_json(PBOSD_STATE)
    profile = LEGACY_PROFILES.get(st.get("profile"), st.get("profile"))
    fan = st.get("fan") if isinstance(st.get("fan"), dict) else {}
    buttons = st.get("buttons") if isinstance(st.get("buttons"), dict) else {}
    return {
        "profile": profile if profile in PROFILES else "balanced",
        "fan": {"mode": "fixed" if fan.get("mode") == "fixed" else "auto", "fixed": int(fan.get("fixed", 50))},
        "buttons": {"F13": "rgb-next", "F14": "profile-next", **buttons},
        "buttons_mode": st.get("buttons_mode") if st.get("buttons_mode") in BUTTON_MODES else "steam",
    }


def telemetry() -> dict[str, Any]:
    out: dict[str, Any] = {"fan_rpm": None, "fan_pwm": None, "gpu_mhz": None, "temp_c": None}
    for d in glob.glob("/sys/class/hwmon/hwmon*"):
        if rd(f"{d}/name") == "pwmfan":
            out["fan_rpm"] = int(rd(f"{d}/fan1_input", "0") or 0)
            out["fan_pwm"] = int(rd(f"{d}/pwm1", "0") or 0)
    for d in glob.glob("/sys/class/devfreq/*gpu*"):
        cur = rd(f"{d}/cur_freq")
        if cur.isdigit():
            out["gpu_mhz"] = int(cur) // 1_000_000
    temps = []
    for z in glob.glob("/sys/class/thermal/thermal_zone*"):
        if rd(f"{z}/type").startswith(("cpu", "gpu")):
            v = rd(f"{z}/temp")
            if v.lstrip("-").isdigit():
                temps.append(int(v) / 1000)
    if temps:
        out["temp_c"] = round(max(temps), 1)
    return out


def set_mcu_link(enabled: bool) -> None:
    if enabled:
        if os.path.exists(MCU_BLACKLIST):
            os.remove(MCU_BLACKLIST)
        subprocess.run(["modprobe", "konkr_sysbtn"], check=False)
    else:
        with open(MCU_BLACKLIST, "w", encoding="utf-8") as fh:
            fh.write("# Pocket FIT MCU UART driver — opt-in (pbosctl mcu enable)\nblacklist konkr_sysbtn\n")
        subprocess.run(["modprobe", "-r", "konkr_sysbtn"], check=False)
    subprocess.run(["systemctl", "restart", "inputplumber.service"], check=False)

# The charging indicator (Power tab): sm8550-charge-rest owns its settings.
CHARGE_REST = "/usr/lib/steamos-sm8550/sm8550-charge-rest"
PREVIEW_UNIT = "pbos-utils-charge-preview"


def charge_rest(*args: str) -> dict[str, Any]:
    """sm8550-charge-rest's settings/save, on the system's native Python."""
    if not os.access(CHARGE_REST, os.X_OK):
        return {"supported": False}
    r = subprocess.run(["/usr/bin/python3", CHARGE_REST, *args], capture_output=True, text=True,
                       env=CLEAN_ENV, timeout=20)
    try:
        out = json.loads(r.stdout)
    except ValueError:
        decky.logger.info(f"charge rest {args[:1]}: {r.stderr.strip()}")
        return {"supported": False}
    return {"supported": bool(out.get("lights")), **out}


FACE_BUTTONS = "/usr/lib/steamos/sm8550-face-buttons"


def face_buttons(*args: str) -> dict[str, Any]:
    """sm8550-face-buttons get / set nintendo|xbox: {"supported", "layout", "saved", "default"}."""
    if not os.access(FACE_BUTTONS, os.X_OK):
        return {"supported": False}
    r = subprocess.run([FACE_BUTTONS, *args], capture_output=True, text=True, env=CLEAN_ENV, timeout=30)
    try:
        return json.loads(r.stdout.strip().splitlines()[-1])
    except (ValueError, IndexError):
        decky.logger.info(f"face buttons {args[:1]}: {r.stderr.strip()}")
        return {"supported": False}


# Sticks: sm8550-stickcal (SM8550 only; absent elsewhere).
STICKCAL = "/usr/lib/steamos-sm8550/sm8550-stickcal"
STICK_AXES = ("left_x", "left_y", "right_x", "right_y")
STICK_REST_S = 1.5
STICK_REST_SPREAD = 80    # a resting stick moving more than this was touched
STICK_EDGES_MAX_S = 180


def stickcal(*args: str, stdin: str | None = None, timeout: float = 30) -> dict[str, Any]:
    if not os.access(STICKCAL, os.R_OK):
        return {"ok": False, "supported": False, "error": "not on this device"}
    try:
        r = subprocess.run(["/usr/bin/python3", STICKCAL, *args], input=stdin, capture_output=True,
                           text=True, timeout=timeout)
    except (OSError, subprocess.TimeoutExpired) as exc:
        return {"ok": False, "error": str(exc)}
    lines = r.stdout.strip().splitlines()
    try:
        res = json.loads(lines[-1]) if lines else {}
    except ValueError:
        res = {}
    if r.returncode != 0 or not res:
        res.setdefault("ok", False)
        res.setdefault("error", r.stderr.strip() or f"exit {r.returncode}")
    return res


def charge_preview() -> bool:
    subprocess.run(["systemctl", "stop", f"{PREVIEW_UNIT}.service"], capture_output=True)
    subprocess.run(["systemctl", "reset-failed", f"{PREVIEW_UNIT}.service"], capture_output=True)
    r = subprocess.run(["systemd-run", f"--unit={PREVIEW_UNIT}", "--collect",
                        "/usr/bin/python3", CHARGE_REST, "preview"], capture_output=True, text=True, env=CLEAN_ENV)
    return r.returncode == 0


# Charge limit and bypass charging (Power tab, Retroid Pocket 6 / Nova):
# sm8550-charge-control owns its settings and applies them.
CHARGE_CONTROL = "/usr/lib/steamos-sm8550/sm8550-charge-control"


def charge_control(*args: str) -> dict[str, Any]:
    if not os.access(CHARGE_CONTROL, os.X_OK):
        return {"supported": False}
    r = subprocess.run(["/usr/bin/python3", CHARGE_CONTROL, *args], capture_output=True, text=True,
                       env=CLEAN_ENV, timeout=20)
    try:
        return json.loads(r.stdout)
    except ValueError:
        decky.logger.info(f"charge control {args[:1]}: {r.stderr.strip()}")
        return {"supported": False}


# Barry Launcher in Desktop Mode (Hardware tab, AYN Thor): its own switch,
# barry_launcher_desktop_switch, run as the desktop user; it applies at
# once in a running Desktop session, else at the next Desktop start.
BARRY_SWITCH = "/usr/lib/barry_launcher/barry_launcher_desktop_switch"


def barry_desktop(*args: str) -> dict[str, Any]:
    if not os.access(BARRY_SWITCH, os.X_OK):
        return {"supported": False}
    user = getattr(decky, "DECKY_USER", "steamos")
    home = getattr(decky, "DECKY_USER_HOME", f"/home/{user}")
    try:
        uid = pwd.getpwnam(user).pw_uid
    except KeyError:
        return {"supported": False}
    run = f"/run/user/{uid}"
    env = {**CLEAN_ENV, "HOME": home, "XDG_RUNTIME_DIR": run, "DBUS_SESSION_BUS_ADDRESS": f"unix:path={run}/bus"}
    try:
        r = subprocess.run(["runuser", "-u", user, "--", BARRY_SWITCH, *args], capture_output=True, text=True,
                           env=env, timeout=30)
        return json.loads(r.stdout.strip().splitlines()[-1])
    except (OSError, subprocess.TimeoutExpired, ValueError, IndexError):
        decky.logger.info(f"barry desktop {args[:1]} failed")
        return {"supported": False}


# --------------------------------------------------------------- proton --
# PB-OS twins of Valve's Proton tools, built by /usr/lib/steamos/pbos-proton.
# Valve's per-game Proton pick outranks Steam's global default, so the
# frontend sets them per game (the user's own picks stay); this says which
# twin stands for which Valve tool and whether it's ready.
PBOS_PROTON = "/usr/lib/steamos/pbos-proton"


def proton_state() -> dict[str, Any]:
    if not os.access(PBOS_PROTON, os.X_OK):
        return {"tools": {}}
    user = getattr(decky, "DECKY_USER", "steamos")
    home = getattr(decky, "DECKY_USER_HOME", f"/home/{user}")
    r = subprocess.run(["runuser", "-u", user, "--", "/usr/bin/python3", PBOS_PROTON, "status", "--json"],
                       capture_output=True, text=True, env={**CLEAN_ENV, "HOME": home}, timeout=30)
    try:
        st = json.loads(r.stdout)
    except ValueError:
        return {"tools": {}}
    # Valve tool name -> PB-OS twin, for twins that are built and patched.
    return {"tools": {s["valve_name"]: name for name, s in st.items()
                      if isinstance(s, dict) and s.get("tool") and s.get("patched") and s.get("valve_name")}}


# Games PB-OS switched to a twin ({appid: {"twin", "valve"}}), games it
# keeps its hands off because the user changed them since ("hands_off"),
# and "global_set": the global default PB-OS found at its one global check
# (the twin when PB-OS set it). Once recorded, PB-OS never changes Steam's
# global default again; it is never cleared.
def proton_games_path() -> str:
    return os.path.join(getattr(decky, "DECKY_USER_HOME", "/home/steamos"), ".config/pb-os/proton-games.json")


def proton_games_read() -> dict[str, Any]:
    st = read_json(proton_games_path())
    return {"switched": st.get("switched", {}), "hands_off": st.get("hands_off", []),
            "global_set": st.get("global_set") or ""}


def proton_games_save(state: dict[str, Any]) -> None:
    # A recorded global check stays recorded, whatever the frontend sends.
    old = proton_games_read()
    proton_games_write({"switched": state.get("switched", {}),
                        "hands_off": state.get("hands_off", []),
                        "global_set": old["global_set"] or str(state.get("global_set") or "")})


def proton_games_write(st: dict[str, Any]) -> None:
    path = proton_games_path()
    os.makedirs(os.path.dirname(path), exist_ok=True)
    write_json(path, st)
    user = getattr(decky, "DECKY_USER", "steamos")
    subprocess.run(["chown", "-R", f"{user}:{user}", os.path.dirname(path)], capture_output=True)


# ---------------------------------------------------------------- games --
# Per-game profiles, applied at launch by GAME_LAUNCH (see the Games tab).
GAME_LAUNCH = "/usr/bin/pbos-game-launch"
ENV_NAME = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
CPULIST = re.compile(r"^\d+(-\d+)?(,\d+(-\d+)?)*$")


def game_profiles_path() -> str:
    return os.path.join(getattr(decky, "DECKY_USER_HOME", "/home/steamos"), ".config/pb-os/game-profiles.json")


def game_info() -> dict[str, Any]:
    # The wrapper knows the presets and this device's core groups; it runs on
    # the system's native Python (this backend is emulated).
    if not os.access(GAME_LAUNCH, os.X_OK):
        return {}
    r = subprocess.run(["/usr/bin/python3", GAME_LAUNCH, "--info"], capture_output=True, text=True,
                       env=CLEAN_ENV, timeout=15)
    try:
        info = json.loads(r.stdout)
    except ValueError:
        return {}
    return info if isinstance(info, dict) else {}


def game_profiles() -> dict[str, Any]:
    info = game_info()
    games = read_json(game_profiles_path()).get("games", {})
    return {**info, "available": bool(info), "games": games if isinstance(games, dict) else {}}


def clean_game_profile(p: dict[str, Any], info: dict[str, Any]) -> dict[str, Any]:
    """What is worth keeping of a profile from the frontend: known settings
    only, and none at their defaults (an empty profile means no profile)."""
    doc = info.get("presets", {})
    presets = doc.get("presets", {}) if isinstance(doc, dict) else {}
    options = {o.get("key") for o in (doc.get("options", []) if isinstance(doc, dict) else []) if isinstance(o, dict)}
    out: dict[str, Any] = {}
    fex = p.get("fex")
    if fex == "custom" and isinstance(p.get("fexConfig"), dict):
        out["fex"] = "custom"
        out["fexConfig"] = {k: "1" if str(v) == "1" else "0" for k, v in p["fexConfig"].items() if k in options}
    elif fex in presets and fex != "default":
        out["fex"] = fex
    cores = str(p.get("cores") or "").replace(" ", "")
    if cores and (cores in info.get("cores", {}) or CPULIST.match(cores)):
        out["cores"] = cores
        if p.get("wineTopology") is False:
            out["wineTopology"] = False
    env = p.get("env")
    if isinstance(env, dict):
        clean = {k: str(v) for k, v in env.items() if isinstance(k, str) and ENV_NAME.match(k) and "\0" not in str(v)}
        if clean:
            out["env"] = clean
    if out:
        out["name"] = str(p.get("name") or "")[:200]
    return out


def game_profile_save(appid: str, profile: dict[str, Any] | None) -> dict[str, Any]:
    if not str(appid).isdigit():
        return {"ok": False, "error": "bad app id"}
    path = game_profiles_path()
    st = read_json(path)
    games = st.get("games") if isinstance(st.get("games"), dict) else {}
    clean = clean_game_profile(profile, game_info()) if isinstance(profile, dict) else {}
    if clean:
        games[str(appid)] = clean
    else:
        games.pop(str(appid), None)
    write_json(path, {"games": games})
    user = getattr(decky, "DECKY_USER", "steamos")
    subprocess.run(["chown", "-R", f"{user}:{user}", os.path.dirname(path)], capture_output=True)
    return {"ok": True, "profile": clean or None}


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
        self.stick_edges: subprocess.Popen | None = None
        self.stick_reader: threading.Thread | None = None
        self.stick_live: dict[str, Any] | None = None
        self.stick_rest: dict[str, Any] | None = None
        self.stick_lock = threading.Lock()
        if self.lights == "multicolor":
            await asyncio.to_thread(start_engine)
        watchers = [self._watch(), self._watch_drives(), self._cleanup(), self._watch_move()]
        if self.lights == "pbosd":
            watchers.append(self._watch_profile())
        self.tasks = [asyncio.create_task(t) for t in watchers]

    async def _unload(self) -> None:
        for t in self.tasks:
            t.cancel()
        self._stick_stop()
        if self.lights == "multicolor":
            await asyncio.to_thread(stop_engine)

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
        # the drives (which mounts them) does not. Update files coming into
        # or leaving the Downloads folder count too.
        seen: list[str] = []
        seen_dl: list[tuple[str, int]] = []
        while True:
            now = await asyncio.to_thread(block_devices)
            now_dl = await asyncio.to_thread(downloaded)
            if now != seen or now_dl != seen_dl:
                if now != seen:
                    self.card = None  # the Install tab looks again
                seen, seen_dl = now, now_dl
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

    async def set_channel(self, channel: str = "prod", passphrase: str = "", **_: Any) -> dict[str, Any]:
        """Dev updates on (dev, needs a tester's passphrase) or off (prod),
        then look again: dev also offers test builds from pb-os-dev. The
        passphrase goes to the updater on stdin, never on its command line."""
        if channel not in ("dev", "prod"):
            return {"ok": False, "error": f"unknown channel {channel}"}
        r = await asyncio.to_thread(subprocess.run, ["/usr/bin/python3", UPDATER, "channel", channel],
                                    input=passphrase + "\n", capture_output=True, text=True,
                                    timeout=60, env=CLEAN_ENV)
        if r.returncode != 0:
            err = (r.stderr or r.stdout).strip().splitlines()
            return {"ok": False, "error": (err[-1] if err else "failed").removeprefix("ERROR: ")}
        self.announced = ""
        return {"ok": True, "release": await self.check()}

    async def check_local(self, **_: Any) -> dict[str, Any]:
        if await asyncio.to_thread(active, UNIT):
            return self.local  # it may be copying from the drive right now
        self.local = await asyncio.to_thread(ask_updater, "local-check", 60)
        up = self.local.get("update")
        key = f"{up['version']} {up['drive']}" if up else ""
        if up and key != self.announced_local and not os.path.exists(PENDING):
            await decky.emit("pbos_local_available", up["version"], up.get("place") or f"on the {up['where']}")
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
                   '--who="PB-OS Tools" --why="Copying SteamOS" --mode=block "$@"; rc=$?; '
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
    async def get_lights(self, **_: Any) -> dict[str, Any]:
        if self.lights == "pbosd":
            st = await asyncio.to_thread(pbosd_lights)
        elif self.lights == "multicolor":
            st = await asyncio.to_thread(multicolor_lights)
            return {"kind": self.lights, "effects": multicolor_effects(st["experimental_audio"]), **st}
        else:
            return {"kind": ""}
        return {"kind": self.lights, "effects": EFFECTS[self.lights], **st}

    async def set_lights(self, on: bool = True, effect: str = "static", color: str = DEFAULT_COLOR,
                         brightness: int = 160, speed: int = 5, reverse_left: bool = False,
                         reverse_right: bool = False, color2: str = "", **_: Any) -> dict[str, Any]:
        if self.lights not in EFFECTS:
            return {"kind": ""}
        known = EFFECTS[self.lights]
        if self.lights == "multicolor":
            known = multicolor_effects(bool(read_json(LIGHTS_STATE).get("experimental_audio")))
        if effect not in {e["id"] for e in known}:
            effect = "static"
        color = clean_color(color)
        brightness = max(0, min(255, int(brightness)))
        speed = max(1, min(10, int(speed)))
        if self.lights == "pbosd":
            await asyncio.to_thread(pbosd_lights_set, bool(on), effect, color, brightness)
        else:
            st = read_json(LIGHTS_STATE)       # keeps the Experimental switches
            st.update({"on": bool(on), "effect": effect, "color": color, "brightness": brightness, "speed": speed,
                       "reverse_left": bool(reverse_left), "reverse_right": bool(reverse_right)})
            if color2:
                st["color2"] = clean_color(color2)     # the audio effects' intensity color
            st.pop("mode", None)
            await asyncio.to_thread(write_json, LIGHTS_STATE, st)
            await asyncio.to_thread(nudge_engine)
        return await self.get_lights()

    async def set_audio_pulse(self, enabled: bool = False, **_: Any) -> dict[str, Any]:
        """The Experimental switch (Audio Effects): on adds Audio Meter and
        Audio Spectrum to the effects and picks Audio Meter (so it's plain
        that it happened), keeping the effect it replaced; off takes them
        away and, if one was showing, brings that effect back."""
        if self.lights != "multicolor":
            return await self.get_lights()
        st = read_json(LIGHTS_STATE)
        cur = st.get("effect", DEFAULT_EFFECT)
        if enabled:
            if cur not in AUDIO_MODES:
                st["effect_before_audio"] = cur
            st.update(experimental_audio=True, effect="audio", on=True)
        else:
            if cur in AUDIO_MODES:
                st["effect"] = st.get("effect_before_audio", "static")
            st["experimental_audio"] = False
        st.pop("mode", None)
        await asyncio.to_thread(write_json, LIGHTS_STATE, st)
        await asyncio.to_thread(nudge_engine)
        return await self.get_lights()

    # --------------------------------------------------------------- power --
    async def get_charging(self, **_: Any) -> dict[str, Any]:
        return await asyncio.to_thread(charge_rest, "settings")

    async def set_charging(self, changes: dict[str, Any] | None = None, **_: Any) -> dict[str, Any]:
        if not isinstance(changes, dict):
            return await self.get_charging()
        return await asyncio.to_thread(charge_rest, "save", json.dumps(changes))

    async def get_charge_control(self, **_: Any) -> dict[str, Any]:
        return await asyncio.to_thread(charge_control, "settings")

    async def set_charge_control(self, changes: dict[str, Any] | None = None, **_: Any) -> dict[str, Any]:
        if not isinstance(changes, dict):
            return await self.get_charge_control()
        return await asyncio.to_thread(charge_control, "save", json.dumps(changes))

    async def preview_charging(self, **_: Any) -> bool:
        return await asyncio.to_thread(charge_preview)

    async def set_power_led(self, on: bool = True, **_: Any) -> dict[str, Any]:
        if self.lights == "pbosd":
            await asyncio.to_thread(pbosd_set, power_led=bool(on))
        return await self.get_lights()

    # ------------------------------------------- performance, hardware --
    # The Performance button goes straight to pbosd, so the panel would only
    # see a change once opened. Watch pbosd's state and tell the frontend,
    # which shows a toast over whatever is running.
    async def _watch_profile(self) -> None:
        try:
            stamp, profile = None, control_state()["profile"]
            while True:
                await asyncio.sleep(PROFILE_WATCH_S)
                try:
                    cur = os.stat(PBOSD_STATE).st_mtime_ns
                except OSError:
                    continue
                if cur == stamp:
                    continue
                stamp = cur
                new = control_state()["profile"]
                if new != profile:
                    decky.logger.info(f"profile {profile} -> {new}")
                    profile = new
                    await decky.emit("pbos_mode", new)
        except asyncio.CancelledError:
            raise
        except Exception:
            decky.logger.exception("profile watcher stopped")

    async def get_control(self, **_: Any) -> dict[str, Any]:
        if self.lights != "pbosd":
            return {"available": False}
        return {
            "available": True, **control_state(),
            "mcu_enabled": not os.path.exists(MCU_BLACKLIST),
            "daemon": await asyncio.to_thread(active, "pbosd"),
            **await asyncio.to_thread(telemetry),
        }

    async def set_profile(self, profile: str = "balanced", **_: Any) -> dict[str, Any]:
        if self.lights == "pbosd" and profile in PROFILES:
            await asyncio.to_thread(pbosd_set, profile=profile)
        return await self.get_control()

    async def set_fan(self, mode: str = "auto", fixed: int = 50, **_: Any) -> dict[str, Any]:
        if self.lights == "pbosd":
            fan = {"mode": "fixed" if mode == "fixed" else "auto", "fixed": max(0, min(100, int(fixed)))}
            await asyncio.to_thread(pbosd_set, fan=fan)
        return await self.get_control()

    async def set_button(self, key: str = "F13", action: str = "none", **_: Any) -> dict[str, Any]:
        if self.lights == "pbosd" and key in ("F13", "F14") and action in BUTTON_ACTIONS:
            buttons = {**control_state()["buttons"], key: action}
            await asyncio.to_thread(pbosd_set, buttons=buttons)
        return await self.get_control()

    # "steam": Custom Function and K are trackpad clicks Steam can remap.
    # "system": they run the actions above. pbosd swaps the InputPlumber map
    # and restarts it.
    async def set_buttons_mode(self, mode: str = "steam", **_: Any) -> dict[str, Any]:
        if self.lights == "pbosd" and mode in BUTTON_MODES:
            await asyncio.to_thread(pbosd_set, buttons_mode=mode)
        return await self.get_control()

    async def set_mcu(self, enabled: bool = False, **_: Any) -> dict[str, Any]:
        if self.lights == "pbosd":
            await asyncio.to_thread(set_mcu_link, bool(enabled))
        return await self.get_control()

    # -------------------------------------------------------- face buttons --
    async def get_face_buttons(self, **_: Any) -> dict[str, Any]:
        return await asyncio.to_thread(face_buttons, "get")

    async def set_face_buttons(self, layout: str = "nintendo", **_: Any) -> dict[str, Any]:
        if layout not in ("nintendo", "xbox"):
            return await self.get_face_buttons()
        decky.logger.info(f"face buttons -> {layout}")
        return await asyncio.to_thread(face_buttons, "set", layout)

    # ------------------------------------------------- barry desktop mode --
    async def get_barry_desktop(self, **_: Any) -> dict[str, Any]:
        return await asyncio.to_thread(barry_desktop, "get")

    async def set_barry_desktop(self, enabled: bool = True, **_: Any) -> dict[str, Any]:
        decky.logger.info(f"barry launcher in desktop mode -> {'on' if enabled else 'off'}")
        return await asyncio.to_thread(barry_desktop, "on" if enabled else "off")

    # --------------------------------------------------------------- sticks --
    async def stick_state(self, **_: Any) -> dict[str, Any]:
        if not os.access(STICKCAL, os.R_OK):
            return {"supported": False}
        return {"supported": True, **await asyncio.to_thread(stickcal, "status")}

    async def stick_rest(self, **_: Any) -> dict[str, Any]:
        """The sticks at rest; refuses if one was touched."""
        res = await asyncio.to_thread(stickcal, "rest", str(STICK_REST_S))
        if not res.get("ok"):
            return res
        moved = [n for n, a in res["axes"].items() if a["hi"] - a["lo"] > STICK_REST_SPREAD]
        if moved:
            return {"ok": False, "error": "A stick moved. Let go of both sticks and try again."}
        self.stick_rest = {n: a["mean"] for n, a in res["axes"].items()}
        decky.logger.info("sticks rest: " + ", ".join(f"{n} {v:+.1f}" for n, v in self.stick_rest.items()))
        return {"ok": True, "rest": self.stick_rest}

    async def stick_edges(self, **_: Any) -> dict[str, Any]:
        self._stick_stop()
        with self.stick_lock:
            self.stick_live = None
        try:
            self.stick_edges = subprocess.Popen(["/usr/bin/python3", STICKCAL, "edges", str(STICK_EDGES_MAX_S)],
                                                stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        except OSError as exc:
            return {"ok": False, "error": str(exc)}
        self.stick_reader = threading.Thread(target=self._stick_read, args=(self.stick_edges,), daemon=True)
        self.stick_reader.start()
        return {"ok": True}

    def _stick_read(self, proc: subprocess.Popen) -> None:
        for line in proc.stdout:
            try:
                data = json.loads(line)
            except ValueError:
                continue
            with self.stick_lock:
                self.stick_live = data

    def _stick_stop(self) -> None:
        proc, self.stick_edges = getattr(self, "stick_edges", None), None
        if proc and proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                proc.kill()
        # Its last line (the typical reach) is read before anything uses it.
        if getattr(self, "stick_reader", None):
            self.stick_reader.join(timeout=3)
            self.stick_reader = None

    async def stick_live_values(self, **_: Any) -> dict[str, Any] | None:
        with self.stick_lock:
            return self.stick_live

    async def stick_cancel(self, **_: Any) -> None:
        self._stick_stop()
        self.stick_rest = None

    async def stick_save(self, **_: Any) -> dict[str, Any]:
        """Stop measuring, keep the result and turn it on."""
        self._stick_stop()
        with self.stick_lock:
            live = self.stick_live
        if not live or not self.stick_rest:
            return {"ok": False, "error": "Nothing measured yet."}
        # The typical reach of each direction (the helper's last line); the
        # furthest values only if the helper died before writing it.
        reach = live.get("reach") or {n: {"lo": live["lo"][n], "hi": live["hi"][n]} for n in STICK_AXES}
        cal = {n: {"rest": self.stick_rest[n], "lo": reach[n]["lo"], "hi": reach[n]["hi"],
                   "furthest": [live["lo"][n], live["hi"][n]]} for n in STICK_AXES}
        res = await asyncio.to_thread(stickcal, "save", stdin=json.dumps(cal))
        if not res.get("ok"):
            res["error"] = f"Not saved: {res.get('error')}. Push each stick all the way round the edge."
            return res
        self.stick_rest = None
        decky.logger.info(f"sticks saved and on: {json.dumps(res.get('plan'))}")
        return res

    async def stick_switch(self, which: str = "", on: bool = False, **_: Any) -> dict[str, Any]:
        if which not in ("calibration", "fine"):
            return {"ok": False, "error": f"unknown switch {which}"}
        res = await asyncio.to_thread(stickcal, "set", which, "on" if on else "off")
        decky.logger.info(f"sticks {which} {'on' if on else 'off'}: {json.dumps(res)}")
        return res

    async def stick_delete(self, **_: Any) -> dict[str, Any]:
        res = await asyncio.to_thread(stickcal, "delete")
        decky.logger.info(f"stick calibration deleted: {json.dumps(res)}")
        return res

    # --------------------------------------------------------------- games --
    async def get_game_profiles(self, **_: Any) -> dict[str, Any]:
        return await asyncio.to_thread(game_profiles)

    async def set_game_profile(self, appid: str = "", profile: dict[str, Any] | None = None, **_: Any) -> dict[str, Any]:
        res = await asyncio.to_thread(game_profile_save, str(appid), profile)
        decky.logger.info(f"game profile {appid}: {json.dumps(res.get('profile'))}")
        return res

    # -------------------------------------------------------------- proton --
    async def get_proton(self, **_: Any) -> dict[str, Any]:
        return await asyncio.to_thread(proton_state)

    async def get_proton_games(self, **_: Any) -> dict[str, Any]:
        return await asyncio.to_thread(proton_games_read)

    async def set_proton_games(self, state: dict[str, Any] | None = None, note: str = "", **_: Any) -> bool:
        if isinstance(state, dict):
            await asyncio.to_thread(proton_games_save, state)
        if note:
            decky.logger.info(f"Proton PB-OS: {note}")
        return True
