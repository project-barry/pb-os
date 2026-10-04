#!/usr/bin/env python3
"""Decky backend — PB-OS Update: pb-os updates from Game Mode.

Front for /usr/share/konkr-update/konkr-update.py (the SteamOS Update app's
helper). The download and staging run as the transient unit pbos-update, so
they carry on when Quick Access closes or Steam restarts; this reads their
log. The update installs on the next restart.
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
    st: dict[str, Any] = {"step": "", "have": 0, "total": 0, "error": "", "staged": False, "note": ""}
    for line in rd(LOG).splitlines():
        if line.startswith("PROGRESS "):
            parts = line.split()
            if len(parts) == 3:
                st["have"], st["total"] = int(parts[1]), int(parts[2])
        elif line.startswith("STEP "):
            st["step"] = line[5:]
        elif line.startswith("ERROR: "):
            st["error"] = line[7:]
        elif line.startswith("Update staged"):
            st["staged"] = True
        elif line.startswith("pb-os ") and line.endswith("is already installed"):
            st["note"] = line
    return st


def check_releases() -> dict[str, Any]:
    # Decky's own Python environment must not leak into the system one.
    r = subprocess.run(["/usr/bin/python3", UPDATER, "check"], capture_output=True, text=True, timeout=90,
                       env={"PATH": "/usr/bin:/usr/sbin", "LANG": "C.UTF-8"})
    if r.returncode != 0:
        err = (r.stderr or r.stdout).strip().splitlines()
        return {"ok": False, "error": (err[-1] if err else "check failed").removeprefix("ERROR: "), "time": time.time()}
    return {"ok": True, **json.loads(r.stdout), "time": time.time()}


class Plugin:
    async def _main(self) -> None:
        self.last: dict[str, Any] = {}
        self.announced = ""
        self.watcher = asyncio.create_task(self._watch())

    async def _unload(self) -> None:
        self.watcher.cancel()

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

    async def get_state(self, **_: Any) -> dict[str, Any]:
        return {
            "installed": rd(VERSION) or "unknown",
            "release": self.last,
            "running": await asyncio.to_thread(running),
            "pending": os.path.exists(PENDING),
            "job": await asyncio.to_thread(job),
        }

    async def start(self, **_: Any) -> bool:
        if await asyncio.to_thread(running) or os.path.exists(PENDING):
            return False
        try:
            os.remove(LOG)
        except FileNotFoundError:
            pass
        subprocess.run(["systemctl", "reset-failed", f"{UNIT}.service"], capture_output=True)
        r = subprocess.run(["systemd-run", f"--unit={UNIT}", "--collect", "-p", f"StandardOutput=file:{LOG}",
                            "-p", "StandardError=inherit", "/usr/bin/python3", "-u", UPDATER, "update"],
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
