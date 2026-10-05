#!/usr/bin/env python3
"""Decky backend — PB-OS Update: pb-os updates from Game Mode.

Front for /usr/share/konkr-update/konkr-update.py (the SteamOS Update app's
helper). The download and staging run as the transient unit pbos-update, so
they carry on when Quick Access closes or Steam restarts; this reads their
log. The update installs on the next restart.

Without internet, an update on a microSD card or USB drive works the same way:
this looks at drives when one is put in or taken out (local-check) and
installs from it with local-update.

Each start frees what finished updates left on HOME (cleanup).
"""
from __future__ import annotations

import asyncio
import json
import os
import subprocess
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


def rd(path: str, default: str = "") -> str:
    try:
        with open(path, encoding="utf-8") as fh:
            return fh.read().strip()
    except OSError:
        return default


def running() -> bool:
    r = subprocess.run(["systemctl", "is-active", f"{UNIT}.service"], capture_output=True, text=True)
    return r.stdout.strip() in ("active", "activating", "deactivating")


def job() -> dict[str, Any]:
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
    r = subprocess.run(["/usr/bin/python3", UPDATER, command], capture_output=True, text=True, timeout=timeout,
                       env={"PATH": "/usr/bin:/usr/sbin", "LANG": "C.UTF-8"})
    if r.returncode != 0:
        err = (r.stderr or r.stdout).strip().splitlines()
        return {"ok": False, "error": (err[-1] if err else "check failed").removeprefix("ERROR: "), "time": time.time()}
    return {"ok": True, **json.loads(r.stdout), "time": time.time()}


def cleanup() -> None:
    """Free what finished updates (from any version) left on HOME; the updater
    skips it while an update is being prepared."""
    r = subprocess.run(["/usr/bin/python3", UPDATER, "cleanup"], capture_output=True, text=True, timeout=600,
                       env={"PATH": "/usr/bin:/usr/sbin", "LANG": "C.UTF-8"})
    out = (r.stdout + r.stderr).strip()
    if out:
        decky.logger.info(out)


def check_releases() -> dict[str, Any]:
    return ask_updater("check", 90)


def check_drives() -> dict[str, Any]:
    return ask_updater("local-check", 60)


def block_devices() -> list[str]:
    try:
        return sorted(os.listdir("/sys/class/block"))
    except OSError:
        return []


class Plugin:
    async def _main(self) -> None:
        self.last: dict[str, Any] = {}
        self.local: dict[str, Any] = {}
        self.announced = ""
        self.announced_local = ""
        self.watcher = asyncio.create_task(self._watch())
        self.drive_watcher = asyncio.create_task(self._watch_drives())
        self.cleaner = asyncio.create_task(self._cleanup())

    async def _unload(self) -> None:
        self.watcher.cancel()
        self.drive_watcher.cancel()
        self.cleaner.cancel()

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
                await asyncio.sleep(2)  # let the drive settle (partitions, SteamOS's own mount)
                try:
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
        self.last = await asyncio.to_thread(check_releases)
        up = self.last.get("update")
        if self.last.get("available") and up and up["version"] != self.announced and not os.path.exists(PENDING):
            self.announced = up["version"]
            await decky.emit("pbos_update_available", up["title"])
        return self.last

    async def check_local(self, **_: Any) -> dict[str, Any]:
        if await asyncio.to_thread(running):
            return self.local  # it may be copying from the drive right now
        self.local = await asyncio.to_thread(check_drives)
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
            "running": await asyncio.to_thread(running),
            "pending": os.path.exists(PENDING),
            "job": await asyncio.to_thread(job),
        }

    async def start(self, **_: Any) -> bool:
        return await self._run("update")

    async def start_local(self, **_: Any) -> bool:
        return await self._run("local-update")

    async def _run(self, command: str) -> bool:
        if await asyncio.to_thread(running) or os.path.exists(PENDING):
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
        if job()["step"].startswith("Downloading"):
            subprocess.run(["systemctl", "stop", f"{UNIT}.service"], check=False)
            return True
        return False

    async def restart(self, **_: Any) -> bool:
        if not os.path.exists(PENDING):
            return False
        subprocess.Popen(["systemctl", "reboot"])
        return True
