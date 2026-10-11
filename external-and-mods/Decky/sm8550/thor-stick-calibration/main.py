#!/usr/bin/env python3
"""Decky backend — Stick Calibration (AYN Thor). EXPERIMENTAL, for testing.

The AYN Android tool in Game Mode: let go of the sticks, push them round
the edge a few times, save. The calibration then sets each stick's centre
and range (py_modules/pbos_stickcal.py, run on the system's native Python:
Decky's own is emulated here).

Nothing is permanent yet: the switch turns the calibration on and off, and
a restart always brings back PB-OS's normal stick settings with the switch
off. Only the measured values are kept (SAVED), so the switch can turn them
on again without calibrating again. If testing goes well, a later PB-OS
release applies a calibration at boot.
"""
from __future__ import annotations

import asyncio
import json
import os
import subprocess
import sys
import threading
import time
from typing import Any

import decky

HERE = os.path.dirname(os.path.abspath(__file__))
HELPER = os.path.join(HERE, "py_modules", "pbos_stickcal.py")
# Only its checks; everything that touches the pad runs in the helper.
sys.path.insert(0, os.path.dirname(HELPER))
from pbos_stickcal import check  # noqa: E402
PYTHON = "/usr/bin/python3"
SAVED = os.path.join(decky.DECKY_PLUGIN_SETTINGS_DIR, "calibration.json")
AXES = ("left_x", "left_y", "right_x", "right_y")
REST_S = 1.5
REST_SPREAD = 80          # a resting stick moving more than this was touched
EDGES_MAX_S = 180


def is_thor() -> bool:
    try:
        with open("/sys/firmware/devicetree/base/compatible", "rb") as fh:
            return b"ayn,thor" in fh.read()
    except OSError:
        return False


def helper(*args: str, stdin: str | None = None, timeout: float = 30) -> dict[str, Any]:
    try:
        r = subprocess.run([PYTHON, HELPER, *args], input=stdin, capture_output=True,
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


def load_saved() -> dict[str, Any] | None:
    try:
        with open(SAVED, encoding="utf-8") as fh:
            return json.load(fh)
    except (OSError, ValueError):
        return None


def summary(cal: dict[str, Any] | None) -> dict[str, Any] | None:
    """What the panel shows of a calibration."""
    if not cal:
        return None
    axes = {}
    for name in AXES:
        a = cal[name]
        r = a["rest"]
        axes[name] = {"rest": round(r), "half": round(min(a["hi"] - r, r - a["lo"]))}
    return {"axes": axes, "time": cal.get("time")}


class Plugin:
    def __init__(self) -> None:
        self.edges: subprocess.Popen | None = None
        self.reader: threading.Thread | None = None
        self.live: dict[str, Any] | None = None
        self.rest: dict[str, Any] | None = None
        self.lock = threading.Lock()

    async def _main(self) -> None:
        decky.logger.info(f"Stick Calibration ready (Thor: {is_thor()})")

    async def _unload(self) -> None:
        self._stop_edges()

    # ---------------------------------------------------------- state --
    async def get_state(self, **_: Any) -> dict[str, Any]:
        thor = is_thor()
        st = await asyncio.to_thread(helper, "status") if thor else {}
        return {"thor": thor, "pad": bool(st.get("pad")), "active": bool(st.get("active")),
                "saved": summary(load_saved()), "measuring": self.edges is not None}

    # ------------------------------------------------------ measuring --
    async def measure_rest(self, **_: Any) -> dict[str, Any]:
        """The sticks at rest; refuses if one was touched."""
        res = await asyncio.to_thread(helper, "rest", str(REST_S))
        if not res.get("ok"):
            return res
        moved = [n for n, a in res["axes"].items() if a["hi"] - a["lo"] > REST_SPREAD]
        if moved:
            return {"ok": False, "error": "A stick moved. Let go of both sticks and try again."}
        self.rest = {n: a["mean"] for n, a in res["axes"].items()}
        decky.logger.info("rest: " + ", ".join(f"{n} {v:+.1f}" for n, v in self.rest.items()))
        return {"ok": True, "rest": self.rest}

    async def start_edges(self, **_: Any) -> dict[str, Any]:
        self._stop_edges()
        with self.lock:
            self.live = None
        try:
            self.edges = subprocess.Popen([PYTHON, HELPER, "edges", str(EDGES_MAX_S)],
                                          stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                                          text=True)
        except OSError as exc:
            return {"ok": False, "error": str(exc)}
        self.reader = threading.Thread(target=self._read_edges, args=(self.edges,), daemon=True)
        self.reader.start()
        return {"ok": True}

    def _read_edges(self, proc: subprocess.Popen) -> None:
        for line in proc.stdout:
            try:
                data = json.loads(line)
            except ValueError:
                continue
            with self.lock:
                self.live = data

    def _stop_edges(self) -> None:
        proc, self.edges = self.edges, None
        if proc and proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                proc.kill()
        # Its last line (the typical reach) is read before anything uses it.
        if self.reader:
            self.reader.join(timeout=3)
            self.reader = None

    async def get_live(self, **_: Any) -> dict[str, Any] | None:
        with self.lock:
            return self.live

    async def cancel(self, **_: Any) -> None:
        self._stop_edges()
        self.rest = None

    async def save(self, **_: Any) -> dict[str, Any]:
        """Stop measuring, keep the result and turn it on."""
        self._stop_edges()
        with self.lock:
            live = self.live
        if not live or not self.rest:
            return {"ok": False, "error": "Nothing measured yet."}
        # The typical reach of each direction (the helper's last line);
        # the furthest values only if the helper died before writing it.
        reach = live.get("reach") or {n: {"lo": live["lo"][n], "hi": live["hi"][n]} for n in AXES}
        cal = {n: {"rest": self.rest[n], "lo": reach[n]["lo"], "hi": reach[n]["hi"],
                   "furthest": [live["lo"][n], live["hi"][n]]} for n in AXES}
        why = check(cal)
        if why:
            return {"ok": False, "error": f"Not saved: {why}. Push each stick all the way round the edge."}
        cal["time"] = int(time.time())
        os.makedirs(os.path.dirname(SAVED), exist_ok=True)
        with open(SAVED + ".tmp", "w", encoding="utf-8") as fh:
            json.dump(cal, fh)
        os.replace(SAVED + ".tmp", SAVED)
        decky.logger.info(f"saved: {json.dumps(summary(cal)['axes'])}")
        self.rest = None
        return await self.set_enabled(True)

    # ------------------------------------------------------- the switch --
    async def set_enabled(self, enabled: bool = False, **_: Any) -> dict[str, Any]:
        if not is_thor():
            return {"ok": False, "error": "This is for the AYN Thor only."}
        if enabled:
            cal = load_saved()
            if not cal:
                return {"ok": False, "error": "Calibrate the sticks first."}
            res = await asyncio.to_thread(helper, "apply", stdin=json.dumps(cal))
        else:
            res = await asyncio.to_thread(helper, "restore")
        decky.logger.info(f"calibration {'on' if enabled else 'off'}: {json.dumps(res)}")
        return res

    async def delete(self, **_: Any) -> dict[str, Any]:
        res = await self.set_enabled(False)
        try:
            os.remove(SAVED)
        except FileNotFoundError:
            pass
        decky.logger.info("calibration deleted")
        return res
