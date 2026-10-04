#!/usr/bin/env python3
"""Decky backend: Barry Launcher (AYN Thor, SteamOS-ARM; the plugin's
folder is still dual-screen).

One switch for the bottom screen. Steam's brightness slider sets both panels
(sm8550-thor-backlightd follows it), each times its own dimmer from DIMMERS,
so the screens keep their ratio as Steam's slider moves. Turning the bottom
screen off powers its backlight down and disables its touchscreen until it
is turned back on. The daemon applies and enforces STATE and DIMMERS; this
only records them.

Also the stick lighting's dimmer (20-100%): sm8550-thor-controlsd lights the
LEDs at the dashboard's brightness times it.

Also stands in Barry Launcher's Keyboard app for Steam's on-screen keyboard:
the frontend asks whether it can (the bottom screen is on and Barry
Launcher's barry_launcher_shelld answers), and opens it there instead of
Steam's. Should Steam's gamescope be showing a dual-screen game's second
window on the bottom screen (GAMESCOPE_BOTTOM_SCREEN_SHOWING), it hands the
screen to Barry Launcher while the keyboard is open (sm8550-thor-backlightd);
if it has not within YIELD_WAIT_S the keyboard would type unseen, so it is
closed again and Steam's keyboard shows on the top screen instead.

Also "Two-screen emulators": the user's own emulators (none ship with PB-OS)
set to draw their second screen in a window of its own, which gamescope's
dual-screen branch shows on the bottom screen. Only their config files
change, a few keys each (TWO_SCREEN); the values found before are kept in
TWO_SCREEN_STATE and put back when the switch goes off. Nothing changes
while one of them runs: they write their settings back when they quit.
The switch is on until the user turns it off, and while it is on, an
emulator found without the settings (installed later, or reset by EmuDeck)
gets them within TWO_SCREEN_CHECK_S once it is not running. DS games that
Steam starts in RetroArch (one window for both screens) are only counted,
for a note: switching them to the standalone melonDS is the user's call.

Also Barry Launcher's home screen: which apps it shows, in what order
(barry_launcher_shelld keeps them in ~/.config/barry_launcher/home.json),
and user apps: an archive picked in Decky's file picker is installed by
barry_launcher_shelld (as the user, into ~/.local/share/barry_launcher/apps),
and a user app can be removed. And apps that open with games: the
frontend watches Steam's running games (Steam's and non-Steam ones alike,
by appid) and tells barry_launcher_shelld, which keeps the links.
"""
from __future__ import annotations

import asyncio
import glob
import json
import os
import pwd
import re
import socket
import subprocess
import threading
import time
import urllib.error
import urllib.request
from typing import Any

import decky

TOP = "/sys/class/backlight/ae96000.dsi.0"
BOTTOM = "/sys/class/backlight/ae94000.dsi.0"
STATE = "/var/lib/steamos-sm8550/thor-bottom-screen"
DIMMERS = "/var/lib/steamos-sm8550/thor-screen-dimmers.json"
CONTROLS = "/run/sm8550-thor/controls.sock"
DIM_MIN = 10
RGB_DIM_MIN = 20
SHELLD = "http://127.0.0.1:47824"
YIELD_WAIT_S = 2.0
TWO_SCREEN_STATE = "/var/lib/steamos-sm8550/thor-two-screen.json"
TWO_SCREEN_CHECK_S = 30
_two_screen_lock = threading.Lock()

# Each emulator: config files (globs under the user's home, AppImage and
# Flatpak), their kind, and (section, key, value) to set. Kinds: "ini" (Qt,
# key=value), "toml" (key = value), "xml" (<key>value</key>).
TWO_SCREEN = {
    "Azahar": {
        "configs": [".config/azahar-emu/qt-config.ini", ".var/app/*/config/azahar-emu/qt-config.ini"],
        "kind": "ini", "process": r"azahar",
        # View > Screen Layout > Separate Windows (LayoutOption 4).
        "keys": [("Layout", "layout_option", "4"), ("Layout", "layout_option\\default", "false")],
    },
    "Lime3DS": {
        "configs": [".config/lime3ds-emu/qt-config.ini", ".var/app/*/config/lime3ds-emu/qt-config.ini"],
        "kind": "ini", "process": r"lime3ds",
        "keys": [("Layout", "layout_option", "4"), ("Layout", "layout_option\\default", "false")],
    },
    "Citra": {
        "configs": [".config/citra-emu/qt-config.ini", ".var/app/*/config/citra-emu/qt-config.ini"],
        "kind": "ini", "process": r"citra",
        "keys": [("Layout", "layout_option", "4"), ("Layout", "layout_option\\default", "false")],
    },
    "melonDS": {
        "configs": [".config/melonDS/melonDS.toml", ".var/app/*/config/melonDS/melonDS.toml"],
        "kind": "toml", "process": r"melonds",
        # A second window, kept across launches: the top screen in the first
        # (screenSizing_TopOnly 4), the bottom one in the second (BotOnly 5).
        # Both with the Natural layout (0): Hybrid (3) ignores the sizing and
        # draws a big screen with both small ones in each window.
        "keys": [("Instance0.Window0", "ScreenLayout", "0"),
                 ("Instance0.Window0", "ScreenSizing", "4"),
                 ("Instance0.Window1", "Enabled", "true"),
                 ("Instance0.Window1", "ScreenLayout", "0"),
                 ("Instance0.Window1", "ScreenSizing", "5")],
    },
    "Cemu": {
        "configs": [".config/Cemu/settings.xml", ".var/app/*/config/Cemu/settings.xml"],
        "kind": "xml", "process": r"cemu",
        # Options > Separate GamePad view.
        "keys": [(None, "open_pad", "true")],
    },
}


def _steam_display_user() -> str | None:
    """The user running Game Mode's main gamescope (Steam's :0), if any."""
    for pid in os.listdir("/proc"):
        if not pid.isdigit():
            continue
        try:
            with open(f"/proc/{pid}/cmdline", "rb") as fh:
                args = fh.read().split(b"\0")
            if os.path.basename(args[0]) == b"gamescope" and b"--steam" in args:
                return pwd.getpwuid(os.stat(f"/proc/{pid}").st_uid).pw_name
        except (OSError, KeyError):
            continue
    return None


def _bottom_screen_showing() -> bool:
    """Steam's gamescope draws a game's window on the bottom screen."""
    user = _steam_display_user()
    if user is None:
        return False
    try:
        r = subprocess.run(
            ["runuser", "-u", user, "--", "env", "DISPLAY=:0", "xprop", "-root",
             "GAMESCOPE_BOTTOM_SCREEN_SHOWING"],
            capture_output=True, text=True, timeout=3)
    except (OSError, subprocess.TimeoutExpired):
        return False
    return r.stdout.strip().endswith("= 1")


def _shelld_post(path: str, body: dict) -> dict:
    req = urllib.request.Request(f"{SHELLD}{path}", json.dumps(body).encode(),
                                 {"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=2) as r:
        return json.load(r)


def _shelld_answer(path: str, body: dict, timeout: float = 2) -> dict:
    """barry_launcher_shelld's JSON answer, an error's included."""
    req = urllib.request.Request(f"{SHELLD}{path}", json.dumps(body).encode(),
                                 {"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return json.load(r)
    except urllib.error.HTTPError as err:
        try:
            return json.load(err)
        except ValueError:
            return {"ok": False, "error": f"Barry Launcher answered {err.code}."}


def _is_on() -> bool:
    try:
        with open(STATE, encoding="utf-8") as fh:
            return fh.read().strip() != "off"
    except OSError:
        return True


def _save(path: str, text: str) -> None:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as fh:
        fh.write(text)
    os.replace(tmp, path)


def _dimmers() -> dict[str, int]:
    try:
        with open(DIMMERS, encoding="utf-8") as fh:
            d = json.load(fh)
        return {k: max(DIM_MIN, min(100, int(d.get(k, 100)))) for k in ("top", "bottom")}
    except (OSError, ValueError, TypeError, AttributeError):
        return {"top": 100, "bottom": 100}


def _controlsd(req: dict) -> dict | None:
    try:
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
            sock.settimeout(2)
            sock.connect(CONTROLS)
            sock.sendall(json.dumps(req).encode() + b"\n")
            data = b""
            while b"\n" not in data:
                chunk = sock.recv(4096)
                if not chunk:
                    break
                data += chunk
        reply = json.loads(data)
        return reply if isinstance(reply, dict) and "error" not in reply else None
    except (OSError, ValueError) as err:
        decky.logger.warning(f"sm8550-thor-controlsd: {err}")
        return None


# Two-screen emulators --------------------------------------------------

def _user_home() -> str:
    home = getattr(decky, "DECKY_USER_HOME", None)
    if home:
        return home
    user = _steam_display_user()
    return pwd.getpwnam(user).pw_dir if user else os.path.expanduser("~")


def _sections(lines: list[str]) -> list[tuple[str | None, int, int]]:
    """(name, first line, end) of each [section] of an ini/toml file; None
    for the lines before the first."""
    out, name, start = [], None, 0
    for i, line in enumerate(lines):
        m = re.match(r"\s*\[([^\]]+)\]\s*$", line)
        if m:
            out.append((name, start, i))
            name, start = m.group(1), i + 1
    out.append((name, start, len(lines)))
    return out


def _key_line(line: str, key: str) -> bool:
    return "=" in line and line.split("=", 1)[0].strip() == key


def _eol(text: str) -> str:
    """The file's line ending, kept as found (Cemu writes CRLF)."""
    return "\r\n" if "\r\n" in text else "\n"


def _get(text: str, kind: str, section: str | None, key: str) -> str | None:
    """The key's whole line (ini/toml) or element (xml), None if absent."""
    if kind == "xml":
        m = re.search(rf"^[ \t]*<{re.escape(key)}>.*?</{re.escape(key)}>[ \t]*(?=\r?$)", text, re.M)
        return m.group(0) if m else None
    lines = text.split(_eol(text))
    for name, start, end in _sections(lines):
        if name == section:
            for line in lines[start:end]:
                if _key_line(line, key):
                    return line
    return None


def _line_for(kind: str, key: str, value: str) -> str:
    return {"ini": f"{key}={value}", "toml": f"{key} = {value}", "xml": f"    <{key}>{value}</{key}>"}[kind]


def _put(text: str, kind: str, section: str | None, key: str, line: str | None) -> str:
    """Sets the key's line (None: removes it), adding the section if needed."""
    eol = _eol(text)
    if kind == "xml":
        m = re.search(rf"^[ \t]*<{re.escape(key)}>.*?</{re.escape(key)}>[ \t]*(?:\r?\n)?", text, re.M)
        if m:
            return text[:m.start()] + ((line + eol) if line is not None else "") + text[m.end():]
        if line is None:
            return text
        at = text.find("</content>")
        return text if at < 0 else text[:at] + line + eol + text[at:]
    lines = text.split(eol)
    for name, start, end in _sections(lines):
        if name != section:
            continue
        for i in range(start, end):
            if _key_line(lines[i], key):
                if line is None:
                    del lines[i]
                else:
                    lines[i] = line
                return eol.join(lines)
        if line is None:
            return text
        # After the section's last non-blank line.
        at = end
        while at > start and not lines[at - 1].strip():
            at -= 1
        lines.insert(at, line)
        return eol.join(lines)
    if line is None:
        return text
    body = text.rstrip("\r\n")
    return f"{body}{eol}{eol}[{section}]{eol}{line}{eol}"


def _has_section(text: str, section: str | None) -> bool:
    return section is None or any(name == section for name, _, _ in _sections(text.split(_eol(text))))


def _drop_added_section(text: str, section: str) -> str:
    """Takes away a section _put appended at the end, once it is empty."""
    eol = _eol(text)
    tail = f"{eol}{eol}[{section}]{eol}"
    return text[:-len(tail)] + eol if text.endswith(tail) else text


def _write_as_owner(path: str, text: str) -> None:
    st = os.stat(path)
    tmp = path + ".dual-screen.tmp"
    with open(tmp, "w", encoding="utf-8", newline="") as fh:
        fh.write(text)
    os.chown(tmp, st.st_uid, st.st_gid)
    os.chmod(tmp, st.st_mode & 0o7777)
    os.replace(tmp, path)


def _two_screen_files() -> list[tuple[str, str, dict]]:
    """(emulator, config path, spec) for each config file present."""
    home = _user_home()
    out = []
    for name, spec in TWO_SCREEN.items():
        for pattern in spec["configs"]:
            for path in sorted(glob.glob(os.path.join(home, pattern))):
                out.append((name, path, spec))
    return out


def _two_screen_running() -> list[str]:
    """Emulators of TWO_SCREEN running now."""
    found = set()
    for pid in os.listdir("/proc"):
        if not pid.isdigit():
            continue
        try:
            with open(f"/proc/{pid}/cmdline", "rb") as fh:
                args = fh.read().split(b"\0")
        except OSError:
            continue
        exe = os.path.basename(args[0].decode(errors="replace")).lower()
        for name, spec in TWO_SCREEN.items():
            if re.search(spec["process"], exe):
                found.add(name)
    return sorted(found)


def _load_two_screen() -> dict:
    try:
        with open(TWO_SCREEN_STATE, encoding="utf-8") as fh:
            d = json.load(fh)
        return d if isinstance(d, dict) else {}
    except (OSError, ValueError):
        return {}


def _two_screen_applied(text: str, spec: dict) -> bool:
    return all(_get(text, spec["kind"], sec, key) is not None and
               _get(text, spec["kind"], sec, key).strip() == _line_for(spec["kind"], key, value).strip()
               for sec, key, value in spec["keys"])


def _vdf_map(data: bytes, i: int) -> tuple[dict, int]:
    """One map of Steam's binary VDF from data[i]; (map, index after it)."""
    out: dict = {}
    while i < len(data):
        kind = data[i]
        i += 1
        if kind == 0x08:
            return out, i
        end = data.index(b"\0", i)
        key = data[i:end].decode(errors="replace")
        i = end + 1
        if kind == 0x00:
            out[key], i = _vdf_map(data, i)
        elif kind == 0x01:
            end = data.index(b"\0", i)
            out[key] = data[i:end].decode(errors="replace")
            i = end + 1
        elif kind in (0x02, 0x03, 0x04, 0x06):
            i += 4
        elif kind in (0x07, 0x0a):
            i += 8
        else:
            raise ValueError(f"VDF type {kind:#x}")
    return out, i


def _ds_on_retroarch() -> int:
    """Steam shortcuts (Steam ROM Manager's) that run DS games in RetroArch,
    which draws both screens in one window. Only read, never changed: the
    user switches them to the standalone melonDS themselves."""
    home = _user_home()
    seen, count = set(), 0
    for path in glob.glob(os.path.join(home, ".local/share/Steam/userdata/*/config/shortcuts.vdf")):
        real = os.path.realpath(path)
        if real in seen:
            continue
        seen.add(real)
        try:
            with open(real, "rb") as fh:
                shortcuts, _ = _vdf_map(fh.read(), 0)
        except (OSError, ValueError):
            continue
        for entry in shortcuts.get("shortcuts", {}).values():
            if not isinstance(entry, dict):
                continue
            cmd = " ".join(str(entry.get(k, "")) for k in ("exe", "Exe", "LaunchOptions")).lower()
            if (("retroarch" in cmd or "libretro" in cmd) and
                    (re.search(r"(melonds|desmume)\w*_libretro", cmd) or re.search(r"\.nds\b", cmd))):
                count += 1
    return count


def _two_screen_status() -> dict[str, Any]:
    state = _load_two_screen()
    emulators = []
    for name, path, spec in _two_screen_files():
        try:
            with open(path, encoding="utf-8", newline="") as fh:
                applied = _two_screen_applied(fh.read(), spec)
        except OSError:
            continue
        emulators.append({"name": name, "path": path, "twoScreens": applied})
    return {"enabled": _two_screen_enabled(state), "emulators": emulators,
            "running": _two_screen_running(), "dsOnRetroArch": _ds_on_retroarch()}


def _two_screen_enabled(state: dict) -> bool:
    """On unless the user turned it off."""
    return bool(state.get("enabled", True))


def _two_screen_file(path: str, spec: dict, enabled: bool, originals: dict) -> bool:
    """Sets (or puts back) one config file's keys; True if it changed."""
    try:
        with open(path, encoding="utf-8", newline="") as fh:
            text = fh.read()
    except OSError:
        return False
    kind, new = spec["kind"], text
    if enabled:
        saved = originals.setdefault(path, {})
        for sec, key, value in spec["keys"]:
            if kind != "xml" and not _has_section(new, sec):
                saved.setdefault(f"{sec}\t", "added")
            saved.setdefault(f"{sec}\t{key}", _get(text, kind, sec, key))
            new = _put(new, kind, sec, key, _line_for(kind, key, value))
    elif path in originals:
        saved = originals.pop(path)
        for sec, key, _ in spec["keys"]:
            if f"{sec}\t{key}" in saved:
                new = _put(new, kind, sec, key, saved[f"{sec}\t{key}"])
        for sec in {sec for sec, _, _ in spec["keys"]}:
            if saved.get(f"{sec}\t") == "added":
                new = _drop_added_section(new, sec)
    if new == text:
        return False
    _write_as_owner(path, new)
    return True


def _set_two_screen(enabled: bool) -> dict[str, Any]:
    """The switch: sets (or puts back) every present emulator's keys;
    refuses while one of them runs. The reply says what happened."""
    with _two_screen_lock:
        files = _two_screen_files()
        running = [n for n in _two_screen_running() if any(n == f[0] for f in files)]
        if running:
            return {"ok": False, "error": f"Close {', '.join(running)} first: it writes its settings back when it quits."}
        state = _load_two_screen()
        originals: dict = state.get("originals", {})
        changed = [name for name, path, spec in files if _two_screen_file(path, spec, enabled, originals)]
        _save(TWO_SCREEN_STATE, json.dumps({"enabled": enabled, "originals": originals}, indent=1) + "\n")
    decky.logger.info(f"two-screen emulators {'on' if enabled else 'off'}: {', '.join(changed) or 'nothing to change'}")
    return {"ok": True, "changed": changed}


def _keep_two_screen() -> list[str]:
    """While the switch is on, gives the settings to each emulator found
    without them that is not running; returns those changed."""
    with _two_screen_lock:
        state = _load_two_screen()
        if not _two_screen_enabled(state):
            return []
        running = set(_two_screen_running())
        originals: dict = state.get("originals", {})
        changed = []
        for name, path, spec in _two_screen_files():
            if name in running:
                continue
            if _two_screen_file(path, spec, True, originals):
                changed.append(name)
        if changed or "enabled" not in state:
            _save(TWO_SCREEN_STATE, json.dumps({"enabled": True, "originals": originals}, indent=1) + "\n")
    if changed:
        decky.logger.info(f"two-screen emulators: set {', '.join(changed)}")
    return changed


def _rgb_dimmer(state: dict | None) -> int | None:
    light = state.get("lighting") if state else None
    return light.get("dimmer", 100) if light else None


class Plugin:
    async def _main(self) -> None:
        decky.logger.info("Barry Launcher plugin ready")
        self._keeper = asyncio.get_event_loop().create_task(self._keep_two_screen())

    async def _unload(self) -> None:
        keeper = getattr(self, "_keeper", None)
        if keeper:
            keeper.cancel()

    async def _keep_two_screen(self) -> None:
        while True:
            try:
                await asyncio.to_thread(_keep_two_screen)
            except Exception as err:  # never let the loop die
                decky.logger.warning(f"two-screen emulators: {err}")
            await asyncio.sleep(TWO_SCREEN_CHECK_S)

    async def get_state(self, **_: Any) -> dict[str, Any]:
        ok = os.path.exists(f"{TOP}/brightness") and os.path.exists(f"{BOTTOM}/brightness")
        return {"supported": ok, "on": _is_on(), "dimmers": _dimmers(),
                "rgbDimmer": _rgb_dimmer(_controlsd({"op": "get"}))}

    async def set_bottom_screen(self, on: bool = True, **_: Any) -> bool:
        os.makedirs(os.path.dirname(STATE), exist_ok=True)
        tmp = STATE + ".tmp"
        with open(tmp, "w", encoding="utf-8") as fh:
            fh.write("on" if on else "off")
        os.replace(tmp, STATE)
        decky.logger.info(f"bottom screen {'on' if on else 'off'}")
        return _is_on()

    async def set_dimmers(self, top: int = 100, bottom: int = 100, **_: Any) -> dict[str, int]:
        """Each screen's dimmer, percent of Steam's brightness."""
        d = {"top": max(DIM_MIN, min(100, int(top))), "bottom": max(DIM_MIN, min(100, int(bottom)))}
        _save(DIMMERS, json.dumps(d) + "\n")
        return d

    async def set_rgb_dimmer(self, value: int = 100, **_: Any) -> int | None:
        v = max(RGB_DIM_MIN, min(100, int(value)))
        return _rgb_dimmer(_controlsd({"op": "set", "lighting": {"dimmer": v}}))

    async def get_two_screen(self, **_: Any) -> dict[str, Any]:
        return await asyncio.to_thread(_two_screen_status)

    async def set_two_screen(self, enabled: bool = True, **_: Any) -> dict[str, Any]:
        return await asyncio.to_thread(_set_two_screen, bool(enabled))

    async def barry_keyboard_available(self, **_: Any) -> bool:
        """The bottom screen is on and Barry Launcher can open its keyboard."""
        if not _is_on():
            return False
        try:
            with urllib.request.urlopen(f"{SHELLD}/apps", timeout=0.5) as r:
                # The Trackpad app has the keyboard (and "keyboard" opens it so).
                apps = json.load(r)
                return "trackpad" in apps or "keyboard" in apps
        except (OSError, ValueError, TypeError):
            return False

    async def get_barry_apps(self, **_: Any) -> dict[str, Any]:
        """Barry Launcher's home apps in order, each shown or hidden."""
        try:
            def get() -> dict:
                with urllib.request.urlopen(f"{SHELLD}/layout", timeout=1) as r:
                    return json.load(r)
            return {"ok": True, **await asyncio.to_thread(get)}
        except (OSError, ValueError) as err:
            return {"ok": False, "error": f"Barry Launcher isn't answering ({err})."}

    async def set_barry_apps(self, order: list | None = None, hidden: list | None = None,
                             **_: Any) -> dict[str, Any]:
        try:
            r = await asyncio.to_thread(_shelld_post, "/layout",
                                        {"order": list(order or []), "hidden": list(hidden or [])})
            return {"ok": "apps" in r, **r}
        except (OSError, ValueError) as err:
            return {"ok": False, "error": f"Barry Launcher isn't answering ({err})."}

    async def barry_apps_folder(self, **_: Any) -> str:
        """Where the file picker starts: Downloads, or home."""
        home = _user_home()
        downloads = os.path.join(home, "Downloads")
        return downloads if os.path.isdir(downloads) else home

    async def check_barry_app(self, path: str = "", **_: Any) -> dict[str, Any]:
        """What the archive at path holds, if it would install."""
        try:
            return await asyncio.to_thread(_shelld_answer, "/apps/check", {"path": str(path)}, 30)
        except (OSError, ValueError) as err:
            return {"ok": False, "error": f"Barry Launcher isn't answering ({err}); is the bottom screen on?"}

    async def install_barry_app(self, path: str = "", **_: Any) -> dict[str, Any]:
        """Install (or update) the app in the archive at path."""
        try:
            return await asyncio.to_thread(_shelld_answer, "/apps/install", {"path": str(path)}, 60)
        except (OSError, ValueError) as err:
            return {"ok": False, "error": f"Barry Launcher isn't answering ({err}); is the bottom screen on?"}

    async def remove_barry_app(self, app: str = "", **_: Any) -> dict[str, Any]:
        try:
            return await asyncio.to_thread(_shelld_answer, "/apps/remove", {"app": str(app)}, 10)
        except (OSError, ValueError) as err:
            return {"ok": False, "error": f"Barry Launcher isn't answering ({err})."}

    async def get_game_links(self, **_: Any) -> dict[str, Any]:
        try:
            def get() -> dict:
                with urllib.request.urlopen(f"{SHELLD}/game-links", timeout=1) as r:
                    return json.load(r)
            return {"ok": True, **await asyncio.to_thread(get)}
        except (OSError, ValueError) as err:
            return {"ok": False, "error": f"Barry Launcher isn't answering ({err})."}

    async def set_game_links(self, links: list | None = None, **_: Any) -> dict[str, Any]:
        try:
            r = await asyncio.to_thread(_shelld_answer, "/game-links", {"links": list(links or [])})
            return {"ok": "links" in r, **r}
        except (OSError, ValueError) as err:
            return {"ok": False, "error": f"Barry Launcher isn't answering ({err})."}

    async def game_event(self, game: str = "", running: bool = False, **_: Any) -> dict[str, Any]:
        """A game started or stopped: Barry Launcher opens or closes its app."""
        try:
            return await asyncio.to_thread(_shelld_answer, "/game-event",
                                           {"game": str(game), "running": bool(running)}, 5)
        except (OSError, ValueError) as err:
            return {"ok": False, "error": str(err)}

    async def open_barry_keyboard(self, **_: Any) -> bool:
        """Opens Barry Launcher's keyboard; False when it cannot be seen, for
        Steam's keyboard then."""
        try:
            if not _shelld_post("/launch", {"app": "keyboard"}).get("ok"):
                return False
        except (OSError, ValueError, AttributeError) as err:
            decky.logger.warning(f"cannot open Barry Launcher's keyboard: {err}")
            return False
        deadline = time.monotonic() + YIELD_WAIT_S
        while await asyncio.to_thread(_bottom_screen_showing):
            if time.monotonic() >= deadline:
                decky.logger.warning("the bottom screen stayed with the game; Steam's keyboard instead")
                try:
                    _shelld_post("/close", {"app": "keyboard"})
                except (OSError, ValueError) as err:
                    decky.logger.warning(f"cannot close Barry Launcher's keyboard: {err}")
                return False
            await asyncio.sleep(0.1)
        return True
