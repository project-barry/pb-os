#!/usr/bin/env python3
"""Decky backend — PB-OS Utils: pb-os tools for Game Mode, in tabs.

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

Lights   stick lighting: on/off, a colour, brightness and effects. On the
         KONKR Pocket FIT pbosd owns it (its state file, applied on SIGHUP,
         shared with the K button and pbosctl) and the stick MCU renders the
         effects; on the Retroid Pocket 6 and Nova the light engine
         (py_modules/pbos_lights.py, its own unit on the system's native
         Python) drives the multicolor LEDs and sets them again after a
         sleep; this saves the settings and nudges it. The AYN Thor's lights
         stay in Barry Launcher.
"""
from __future__ import annotations

import asyncio
import json
import os
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
    AUDIO_MODES, DEFAULT_COLOR, EFFECTS, LIGHTS_STATE, MULTICOLOR_DEFAULT_EFFECT, clean_color, lights_kind,
    multicolor_effects, multicolor_lights, pbosd_lights, pbosd_lights_set, pbosd_set)

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
        if self.lights == "multicolor":
            await asyncio.to_thread(start_engine)
        self.tasks = [asyncio.create_task(t) for t in
                      (self._watch(), self._watch_drives(), self._cleanup(), self._watch_move())]

    async def _unload(self) -> None:
        for t in self.tasks:
            t.cancel()
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

    async def set_channel(self, channel: str = "prod", **_: Any) -> dict[str, Any]:
        """Dev updates on (dev) or off (prod), then look again: dev also offers
        test builds from pb-os-dev."""
        if channel not in ("dev", "prod"):
            return {"ok": False, "error": f"unknown channel {channel}"}
        await asyncio.to_thread(subprocess.run, ["/usr/bin/python3", UPDATER, "channel", channel],
                                capture_output=True, timeout=30, env=CLEAN_ENV)
        self.announced = ""
        return await self.check()

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
                st["color2"] = clean_color(color2)     # the audio effects' intensity colour
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
        cur = st.get("effect", MULTICOLOR_DEFAULT_EFFECT)
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

    async def set_power_led(self, on: bool = True, **_: Any) -> dict[str, Any]:
        if self.lights == "pbosd":
            await asyncio.to_thread(pbosd_set, power_led=bool(on))
        return await self.get_lights()
