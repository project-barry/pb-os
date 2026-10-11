#!/usr/bin/env python3
"""Decky backend — Stick Calibration (EXPERIMENTAL): AYN Thor, Retroid Pocket 6, Nova.

Two switches for the rsinput pad, both kept across restarts:
  Use my calibration  AYN's Android stick calibration in Game Mode: let go
                      of the sticks, push them round the edge, save; each
                      stick's centre and range are set from that.
  Fine stick steps    the controller's finer reporting (AYN's and Retroid's
                      Android setting).
The work is done by /usr/lib/steamos-sm8550/sm8550-stickcal on the
system's native Python (Decky's own is emulated here), which also keeps the
switches and sets them again at boot (sm8550-stickcal.service).
"""
from __future__ import annotations

import asyncio
import json
import subprocess
import threading
from typing import Any

import decky

HELPER = "/usr/lib/steamos-sm8550/sm8550-stickcal"
PYTHON = "/usr/bin/python3"
AXES = ("left_x", "left_y", "right_x", "right_y")
REST_S = 1.5
REST_SPREAD = 80          # a resting stick moving more than this was touched
EDGES_MAX_S = 180


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


class Plugin:
    def __init__(self) -> None:
        self.edges: subprocess.Popen | None = None
        self.reader: threading.Thread | None = None
        self.live: dict[str, Any] | None = None
        self.rest: dict[str, Any] | None = None
        self.lock = threading.Lock()

    async def _main(self) -> None:
        decky.logger.info("Stick Calibration ready")

    async def _unload(self) -> None:
        self._stop_edges()

    async def get_state(self, **_: Any) -> dict[str, Any]:
        return await asyncio.to_thread(helper, "status")

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
        res = await asyncio.to_thread(helper, "save", stdin=json.dumps(cal))
        if not res.get("ok"):
            res["error"] = f"Not saved: {res.get('error')}. Push each stick all the way round the edge."
            return res
        self.rest = None
        decky.logger.info(f"saved and on: {json.dumps(res.get('plan'))}")
        return res

    # ------------------------------------------------------- switches --
    async def set_switch(self, which: str = "", on: bool = False, **_: Any) -> dict[str, Any]:
        if which not in ("calibration", "fine"):
            return {"ok": False, "error": f"unknown switch {which}"}
        res = await asyncio.to_thread(helper, "set", which, "on" if on else "off")
        decky.logger.info(f"{which} {'on' if on else 'off'}: {json.dumps(res)}")
        return res

    async def delete(self, **_: Any) -> dict[str, Any]:
        res = await asyncio.to_thread(helper, "delete")
        decky.logger.info(f"calibration deleted: {json.dumps(res)}")
        return res
