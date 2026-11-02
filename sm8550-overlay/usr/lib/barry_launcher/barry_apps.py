"""barry_apps: Barry Launcher's user apps, packaged as a zip or tar archive
of a folder and installed into ~/.local/share/barry_launcher/apps/ID.

A package is a folder with barry-app.json at its top (or an archive of
such a folder: the archive may hold it directly or inside one folder):

  {
    "format": 1,
    "id": "io.github.someone.dice",  lower case, dotted (reverse domain)
    "name": "Dice",                  the tile's label, up to 20 characters
    "version": "1.0.0",
    "main": "main.qml",              the app: its root item fills the screen
    "icon": "icon.png",              optional: PNG or SVG, square
    "description": "...", "author": "...", "homepage": "..."   optional
    "service": "service.py"          optional: see below
    "keepAwake": true                optional: see below
    "minApi": 2                      optional: see below
  }

A QML app may come with a service: a Python 3 script (standard library
only) for what QML cannot do, such as talking to a game over the network.
It starts before the app's window and stops when the window closes
(start_service). It must serve HTTP on 127.0.0.1, port BARRY_SERVICE_PORT,
and answer only requests that carry BARRY_SERVICE_TOKEN in an
X-Barry-Token header (or a "token" query parameter, for an Image's
source), so web pages open on the device cannot use it. (The token is in
the app window's command line, which other local programs can read: it is
no defence against them.)
The app finds it at barry.serviceUrl and calls it with barry.request
(shell/AppHost.qml). A service runs as the user, like any program the user
installs: the Barry Launcher plugin asks before installing one, and
barry-app says so.

A web app has no QML: "type": "web" and a "url" (https) instead of
"main", with an optional "zoom" (CSS zoom on the bottom screen, 0.5-4)
and "allow" (["microphone"], ["camera"]: given without asking).
barry_launcher_shelld opens it in Firefox, kiosk mode, with a profile of
its own in the app's data folder (logins and all go with the app).

An app in the background (another app in front, or Barry or the game art
over it) is frozen after 30 s, its service with it: it gets no CPU time
until its tile brings it back, and one left there for 30 minutes, or under
memory pressure, is closed. Not while it plays sound. "keepAwake": true
keeps it running in the background all the time (a chat app that must
hear new messages, an app talking to a game): a cost in battery, so only
for apps that need it.

Touch is Barry Launcher's: an app scrolls, pans and zooms with Barry.Input's
TouchScroll (around a Flickable, ListView or GridView) and PanZoomArea (a
view, such as a map), never with touch handling of its own, so it feels
like the Launcher and every touch fix in Barry Launcher reaches it
(Barry.Input's GESTURES.md). check and install warn about QML that still
scrolls or pinches by itself (touch_warnings); it installs all the same.

The app's icon follows the skin's theme like the built-in icons: one-color
white (#ffffff) line art on transparent, named icon-symbolic.svg, which
every Barry Launcher tints with the theme's icon color (icon_symbolic).
check and install warn about an icon that would not (icon_warnings): a
picture or several colors, a color other than white, or a name without
-symbolic (older Barry Launchers tint only those). It installs all the same.

"minApi": the Barry Launcher API level the app needs (API below; 1 when
left out). Level 2 is Barry.Input's TouchScroll and PanZoomArea: an app
that imports them says "minApi": 2, and a Barry Launcher older than that
refuses to install it, saying PB-OS needs an update first, instead of
installing an app whose window cannot load. (Barry Launchers from before
the field ignore it: there the app installs and its window fails to load,
as before.) The level goes up by one each time Barry Launcher gives apps
something new they may need.

Barry Launcher runs main.qml in a full-screen window of its own
(shell/AppHost.qml), the way it runs its own QML apps, with its config
and data XDG folders pointed into its data folder (app_env): whatever it or
Qt saves for it (Settings, LocalStorage) stays in
~/.local/share/barry_launcher/app-data/ID. Installing an app that is
already there (same id) updates it and keeps that data; removing it
deletes the app, its data, its log and its place on the home screen.

Apps that come with Barry Launcher (BUNDLED, folders) are installed at
its start unless the user removed them (REMOVED lists those), and
updated when Barry Launcher brings a newer version. The apps Project Barry
makes elsewhere (Pip-Boy, ...) are offered as they get new versions
(barry_updates): once after each PB-OS update, and only on the user's tap.

barry_launcher_shelld lists and starts the installed apps; the barry-app
command and the Barry Launcher Decky plugin install and remove them.
"""
from __future__ import annotations

import json
import os
import re
import secrets
import shutil
import socket
import stat
import subprocess
import sys
import tarfile
import tempfile
import zipfile

HOME = os.path.expanduser("~")
DATA = os.path.join(os.environ.get("XDG_DATA_HOME", f"{HOME}/.local/share"), "barry_launcher")
APPS_DIR = os.path.join(DATA, "apps")
APP_DATA_DIR = os.path.join(DATA, "app-data")
CONFIG = os.path.join(os.environ.get("XDG_CONFIG_HOME", f"{HOME}/.config"), "barry_launcher")
HOME_LAYOUT = os.path.join(CONFIG, "home.json")  # barry_launcher_shelld's
REMOVED = os.path.join(CONFIG, "removed-apps.json")
LOGS = os.path.join(os.environ.get("XDG_CACHE_HOME", f"{HOME}/.cache"), "barry_launcher")
BUNDLED = "/usr/share/barry_launcher/apps"
MANIFEST = "barry-app.json"
FORMAT = 1
# The Barry Launcher API level this one has (an app's "minApi", above):
# 1 the first apps, 2 Barry.Input's TouchScroll and PanZoomArea.
API = 2
# A dot keeps every user app's id apart from Barry Launcher's own apps.
ID_RE = re.compile(r"^[a-z0-9][a-z0-9_-]*(\.[a-z0-9][a-z0-9_-]*)+$")
ID_MAX = 64
NAME_MAX = 20
ICON_TYPES = (".png", ".svg")
TYPES = ("qml", "web")
WEB_ALLOW = ("microphone", "camera")
# Limits against an archive that unpacks into far more than it looks.
MAX_ARCHIVE = 100 * 1024 * 1024
MAX_UNPACKED = 300 * 1024 * 1024
MAX_FILES = 5000
ARCHIVE_TYPES = (".zip", ".tar", ".tar.gz", ".tgz", ".tar.xz", ".txz", ".tar.bz2", ".tbz2")


class AppError(Exception):
    """A package that cannot be installed, and why (shown to the user)."""


def _str(m: dict, key: str, required: bool = True) -> str | None:
    v = m.get(key)
    if v is None and not required:
        return None
    if not isinstance(v, str) or not v.strip():
        raise AppError(f'{MANIFEST}: "{key}" must be a non-empty string')
    return v.strip()


def _inside(root: str, rel: str, what: str) -> str:
    """The file rel names in the package at root; it must be in the
    package."""
    if os.path.isabs(rel) or ".." in rel.replace("\\", "/").split("/"):
        raise AppError(f'{MANIFEST}: "{what}" must be a path inside the app folder')
    path = os.path.normpath(os.path.join(root, rel))
    if not os.path.isfile(path):
        raise AppError(f'{MANIFEST}: "{what}" names {rel}, which is not in the app folder')
    return path


def read_manifest(root: str) -> dict:
    """The checked manifest of the app folder at root."""
    try:
        with open(os.path.join(root, MANIFEST), encoding="utf-8") as fh:
            m = json.load(fh)
    except FileNotFoundError:
        raise AppError(f"no {MANIFEST} in the app folder") from None
    except (OSError, ValueError) as err:
        raise AppError(f"cannot read {MANIFEST}: {err}") from None
    if not isinstance(m, dict):
        raise AppError(f"{MANIFEST} must hold a JSON object")
    fmt = m.get("format", FORMAT)
    if fmt != FORMAT:
        raise AppError(f'{MANIFEST}: "format" {fmt!r} is not one this Barry Launcher knows ({FORMAT})')
    app_id = _str(m, "id")
    if len(app_id) > ID_MAX or not ID_RE.match(app_id):
        raise AppError(f'{MANIFEST}: "id" {app_id!r} must be lower case and dotted, like "io.github.you.myapp"')
    name = _str(m, "name")
    if len(name) > NAME_MAX:
        raise AppError(f'{MANIFEST}: "name" is {len(name)} characters; the tile has room for {NAME_MAX}')
    version = _str(m, "version")
    kind = m.get("type", "qml")
    if kind not in TYPES:
        raise AppError(f'{MANIFEST}: "type" must be one of {", ".join(TYPES)}')
    web = None
    main = None
    service = None
    if kind == "qml":
        main = _str(m, "main")
        if not main.endswith(".qml"):
            raise AppError(f'{MANIFEST}: "main" must be a .qml file')
        _inside(root, main, "main")
        service = _str(m, "service", required=False)
        if service is not None:
            if not service.endswith(".py"):
                raise AppError(f'{MANIFEST}: "service" must be a .py file')
            _inside(root, service, "service")
    elif "service" in m:
        raise AppError(f'{MANIFEST}: a web app has no "service"')
    else:
        url = _str(m, "url")
        if not re.match(r"^https://[^\s/]+(/\S*)?$", url):
            raise AppError(f'{MANIFEST}: "url" must be an https:// address')
        zoom = m.get("zoom", 1.25)
        if isinstance(zoom, bool) or not isinstance(zoom, (int, float)) or not 0.5 <= zoom <= 4:
            raise AppError(f'{MANIFEST}: "zoom" must be a number from 0.5 to 4')
        allow = m.get("allow", [])
        if not isinstance(allow, list) or any(a not in WEB_ALLOW for a in allow):
            raise AppError(f'{MANIFEST}: "allow" may list {", ".join(WEB_ALLOW)}')
        web = {"url": url, "zoom": float(zoom), "allow": sorted(set(allow))}
    icon = _str(m, "icon", required=False)
    if icon is not None:
        if not icon.lower().endswith(ICON_TYPES):
            raise AppError(f'{MANIFEST}: "icon" must be a PNG or SVG file')
        _inside(root, icon, "icon")
    keep_awake = m.get("keepAwake", False)
    if not isinstance(keep_awake, bool):
        raise AppError(f'{MANIFEST}: "keepAwake" must be true or false')
    min_api = m.get("minApi", 1)
    if isinstance(min_api, bool) or not isinstance(min_api, int) or min_api < 1:
        raise AppError(f'{MANIFEST}: "minApi" must be a whole number, 1 or more')
    if min_api > API:
        raise AppError(f"{name} {version} needs a newer Barry Launcher (API level {min_api}; this one has "
                       f"{API}): update PB-OS first")
    out = {"format": FORMAT, "id": app_id, "name": name, "version": version, "type": kind,
           "main": main, "web": web, "service": service, "icon": icon, "keepAwake": keep_awake,
           "minApi": min_api}
    for key in ("description", "author", "homepage"):
        v = m.get(key)
        if isinstance(v, str) and v.strip():
            out[key] = v.strip()
    return out


def _check_member(name: str) -> None:
    parts = name.replace("\\", "/").split("/")
    if name.startswith(("/", "\\")) or ".." in parts or re.match(r"^[A-Za-z]:", name):
        raise AppError(f"the archive has a file outside its folder: {name}")


def _unpack(archive: str, dest: str) -> None:
    """Unpack archive into dest: plain files and folders only, all inside
    dest, within the size limits."""
    size = os.path.getsize(archive)
    if size > MAX_ARCHIVE:
        raise AppError(f"the archive is {size // 1048576} MB; the limit is {MAX_ARCHIVE // 1048576} MB")
    lower = archive.lower()
    total = files = 0
    if lower.endswith(".zip"):
        try:
            zf = zipfile.ZipFile(archive)
        except zipfile.BadZipFile:
            raise AppError("this is not a zip file Barry Launcher can read") from None
        with zf:
            for info in zf.infolist():
                _check_member(info.filename)
                mode = info.external_attr >> 16
                if stat.S_ISLNK(mode):
                    raise AppError(f"the archive has a link, which apps may not have: {info.filename}")
                total += info.file_size
                files += 1
                if total > MAX_UNPACKED or files > MAX_FILES:
                    raise AppError("the archive unpacks into too much")
            zf.extractall(dest)
        return
    if not lower.endswith(ARCHIVE_TYPES):
        raise AppError("apps come as .zip, .tar.gz, .tar.xz or .tar.bz2 archives")
    try:
        tf = tarfile.open(archive)
    except (tarfile.TarError, OSError):
        raise AppError("this is not an archive Barry Launcher can read") from None
    with tf:
        members = tf.getmembers()
        for m in members:
            _check_member(m.name)
            if not (m.isfile() or m.isdir()):
                raise AppError(f"the archive has a link or special file, which apps may not have: {m.name}")
            total += m.size
            files += 1
            if total > MAX_UNPACKED or files > MAX_FILES:
                raise AppError("the archive unpacks into too much")
        for m in members:
            # No owners, modes or times from the archive: plain user files.
            m.mode = 0o755 if m.isdir() else 0o644
            m.uid = m.gid = 0
            m.uname = m.gname = ""
        if hasattr(tarfile, "data_filter"):
            tf.extractall(dest, members, filter="data")
        else:
            tf.extractall(dest, members)


def _package_root(unpacked: str) -> str:
    """Where barry-app.json is: the archive's top, or its one folder."""
    if os.path.isfile(os.path.join(unpacked, MANIFEST)):
        return unpacked
    entries = [e for e in os.listdir(unpacked) if e not in ("__MACOSX", ".DS_Store")]
    if len(entries) == 1 and os.path.isdir(os.path.join(unpacked, entries[0])):
        inner = os.path.join(unpacked, entries[0])
        if os.path.isfile(os.path.join(inner, MANIFEST)):
            return inner
    raise AppError(f"no {MANIFEST} at the top of the archive, or in its one folder")


# touch_warnings: QML that scrolls, pans or zooms by itself instead of with
# Barry.Input. A simple look at the text, per .qml file, with comments and
# strings left out; it warns and never refuses (apps made before
# Barry.Input still install):
#   - a PinchHandler or PinchArea: a pinch of its own (PanZoomArea);
#   - a Flickable, ListView or GridView that is not inside a TouchScroll,
#     is not the one a TouchScroll names (flickable: its id) and does not
#     say "interactive: false": a drag scroll of its own (TouchScroll).
# DragHandler, MouseArea and MultiPointTouchArea are left alone: most are
# taps, sliders or drag and drop, not scrolling.
SCROLLERS = ("Flickable", "ListView", "GridView")
PINCHES = ("PinchHandler", "PinchArea")
_QML_NOISE = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'|`(?:\\.|[^`\\])*`',
                        re.S)
_QML_TYPE = re.compile(r"(?:^|[^\w.])((?:[A-Za-z_]\w*\.)*([A-Z]\w*))\s*$")


def _blank(match: re.Match) -> str:
    """The comment or string, as spaces (its lines kept)."""
    return re.sub(r"[^\n]", " ", match.group(0))


def _own(block: str) -> str:
    """A QML object's block (its braces' inside) without its children."""
    out, depth = [], 0
    for c in block:
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
        elif depth == 0:
            out.append(c)
    return "".join(out)


def _qml_touch(text: str) -> list[tuple[int, str, str]]:
    """(line, type, what) for each object in QML text that scrolls or
    pinches by itself."""
    code = _QML_NOISE.sub(_blank, text)
    stack: list[tuple[str | None, int]] = []
    scrollers: list[tuple[int, str, bool, str]] = []  # at, type, in a TouchScroll, own text
    named: set[str] = set()  # ids TouchScrolls name with flickable:
    found = []
    for i, c in enumerate(code):
        if c == "{":
            m = _QML_TYPE.search(code, max(0, i - 200), i)
            stack.append((m.group(2) if m else None, i))
        elif c == "}" and stack:
            kind, at = stack.pop()
            if kind in SCROLLERS:
                inside = any(k == "TouchScroll" for k, _ in stack)
                scrollers.append((at, kind, inside, _own(code[at + 1:i])))
            elif kind == "TouchScroll":
                m = re.search(r"\bflickable\s*:\s*([A-Za-z_]\w*)", _own(code[at + 1:i]))
                if m:
                    named.add(m.group(1))
            elif kind in PINCHES:
                found.append((code.count("\n", 0, at) + 1, kind, "pinch"))
    for at, kind, inside, own in scrollers:
        ident = re.search(r"\bid\s*:\s*([A-Za-z_]\w*)", own)
        if inside or (ident and ident.group(1) in named) or re.search(r"\binteractive\s*:\s*false\b", own):
            continue
        found.append((code.count("\n", 0, at) + 1, kind, "scroll"))
    return sorted(found)


_PAINT = re.compile(r"(?:fill|stroke|stop-color|color)\s*[:=]\s*[\"']?\s*([^;\"'\s>]+)", re.I)


def icon_symbolic(path: str) -> bool:
    """Whether the app's icon takes the theme's icon color on the home
    screen, like the built-in icons: one named *-symbolic.* (white on
    transparent, as in icon themes), or an SVG drawn in a single color
    (line art). A picture in several colors keeps its own."""
    if re.search(r"-symbolic\.\w+$", path, re.I):
        return True
    colors = _icon_colors(path)
    return colors is not None and len(colors) == 1


def _icon_colors(path: str) -> set[str] | None:
    """The colors an SVG icon paints with, or None for a picture (not an
    SVG, or one with an image, gradient or pattern) or an unreadable file."""
    if not path.lower().endswith(".svg"):
        return None
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            svg = f.read(256 * 1024)
    except OSError:
        return None
    if re.search(r"<\s*(image|linearGradient|radialGradient|pattern)\b", svg, re.I):
        return None
    return {c.lower() for c in _PAINT.findall(svg)} - {"none", "transparent", "currentcolor", "inherit"}


WHITE = {"#fff", "#ffffff", "#ffffffff", "white"}


def icon_warnings(root: str, m: dict) -> list[str]:
    """Why the app's icon (manifest m, folder root) would not follow the
    skin's theme like the built-in icons, as "file: ..."; none for an app
    without an icon (its tile shows the plain one, which does). A warning,
    never a refusal: the icon then shows in its own colors."""
    rel = m.get("icon")
    if not rel:
        return []
    colors = _icon_colors(os.path.join(root, rel))
    if colors is None or len(colors) != 1:
        return [f"{rel}: not one-color line art, so it keeps its own colors instead of the theme's; "
                "draw it in white (#ffffff) only, on transparent, as icon-symbolic.svg"]
    out = []
    if not colors <= WHITE:
        out.append(f"{rel}: drawn in {next(iter(colors))}, not white, so the theme's tint comes out "
                   "darker than the built-in icons; draw it in #ffffff")
    if not re.search(r"-symbolic\.svg$", rel, re.I):
        out.append(f"{rel}: older Barry Launchers tint only icons named *-symbolic.svg; "
                   "name it icon-symbolic.svg")
    return out


def touch_warnings(root: str) -> list[str]:
    """What in the app folder at root's QML scrolls, pans or zooms by
    itself instead of with Barry.Input (see above), as "file:line: ..."."""
    out = []
    for folder, dirs, files in os.walk(root):
        dirs[:] = sorted(d for d in dirs if not d.startswith("."))
        for f in sorted(files):
            if not f.endswith(".qml"):
                continue
            path = os.path.join(folder, f)
            try:
                with open(path, encoding="utf-8", errors="replace") as fh:
                    text = fh.read()
            except OSError:
                continue
            rel = os.path.relpath(path, root).replace(os.sep, "/")
            for line, kind, what in _qml_touch(text):
                if what == "pinch":
                    out.append(f"{rel}:{line}: {kind} pinches by itself; pan and zoom with "
                               "Barry.Input's PanZoomArea, so it follows Barry Launcher's touch rules")
                else:
                    out.append(f"{rel}:{line}: {kind} scrolls by its own drag; put it in "
                               "Barry.Input's TouchScroll, so it follows Barry Launcher's touch rules")
    return out


def _warned(root: str, m: dict) -> dict:
    """m with "touchWarnings" (touch_warnings) and "iconWarnings"
    (icon_warnings) for the app folder at root."""
    return dict(m, touchWarnings=touch_warnings(root), iconWarnings=icon_warnings(root, m))


def check(path: str) -> dict:
    """The manifest of the app in archive or folder path, if it would
    install, with "touchWarnings" (touch_warnings) and "iconWarnings"
    (icon_warnings)."""
    if os.path.isdir(path):
        return _warned(path, read_manifest(path))
    with tempfile.TemporaryDirectory(prefix="barry-app-") as tmp:
        _unpack(path, tmp)
        root = _package_root(tmp)
        return _warned(root, read_manifest(root))


def install(archive: str, reserved: set[str] = frozenset()) -> dict:
    """Install (or update) the app in archive (or an app folder); its
    manifest, with "updated": whether it replaced an installed version,
    "touchWarnings" (touch_warnings) and "iconWarnings" (icon_warnings)."""
    if not os.path.isfile(archive) and not os.path.isdir(archive):
        raise AppError(f"no file {archive}")
    os.makedirs(APPS_DIR, exist_ok=True)
    tmp = tempfile.mkdtemp(prefix=".new-", dir=APPS_DIR)
    try:
        if os.path.isdir(archive):
            read_manifest(archive)  # before copying anything
            root = os.path.join(tmp, "app")
            shutil.copytree(archive, root, symlinks=True)
            if any(os.path.islink(os.path.join(d, f)) for d, ds, fs in os.walk(root) for f in ds + fs):
                raise AppError("the app folder has a link, which apps may not have")
        else:
            _unpack(archive, tmp)
            root = _package_root(tmp)
        m = _warned(root, read_manifest(root))
        if m["id"] in reserved:
            raise AppError(f'"{m["id"]}" is one of Barry Launcher\'s own apps')
        dest = os.path.join(APPS_DIR, m["id"])
        old = None
        if os.path.exists(dest):
            old = tempfile.mkdtemp(prefix=".old-", dir=APPS_DIR)
            os.rename(dest, os.path.join(old, "app"))
        os.rename(root, dest)
        os.chmod(dest, 0o755)
        os.makedirs(os.path.join(APP_DATA_DIR, m["id"]), exist_ok=True)
        if old:
            shutil.rmtree(old, ignore_errors=True)
        _set_removed(m["id"], False)
        return dict(m, updated=old is not None)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def remove(app_id: str, keep_data: bool = False) -> bool:
    """Remove an installed app: its files, its log, its place on the home
    screen, and (unless keep_data) everything it saved. False if it was not
    installed. The app must not be running (it could save again)."""
    if not ID_RE.match(app_id or ""):
        return False
    dest = os.path.join(APPS_DIR, app_id)
    if not os.path.isdir(dest):
        return False
    shutil.rmtree(dest)
    if not keep_data:
        shutil.rmtree(os.path.join(APP_DATA_DIR, app_id), ignore_errors=True)
    for log in (os.path.join(LOGS, f"{app_id}.log"),):
        try:
            os.remove(log)
        except OSError:
            pass
    _forget_layout(app_id)
    if bundled().get(app_id):
        _set_removed(app_id, True)  # not back at the next start
    return True


def app_env(app_id: str) -> dict[str, str]:
    """The environment an app runs with: its ids and folders, and the
    config and data XDG folders inside its data folder, so that what Qt
    saves for it (Settings without a location, LocalStorage) goes there
    too. The cache stays the user's: font and shader caches are shared, not
    the app's (a folder of its own would rebuild them, about 5 MB, for each
    app), and Qt's compiled-QML cache, which would be the app's, is off."""
    data = os.path.join(APP_DATA_DIR, app_id)
    return {"BARRY_APP_ID": app_id, "BARRY_APP_DIR": os.path.join(APPS_DIR, app_id), "BARRY_APP_DATA": data,
            "XDG_CONFIG_HOME": os.path.join(data, ".config"),
            "XDG_DATA_HOME": os.path.join(data, ".local", "share"),
            "QML_DISABLE_DISK_CACHE": "1"}


def start_service(app_dir: str, service: str, env: dict, out,
                  wrap: list[str] | None = None) -> tuple[subprocess.Popen, str, str]:
    """Start an app's service (the script service, in app_dir) with env
    and its output to out: the process, its address and its token. The
    address is for the app's window (barry.serviceUrl); the token, which
    the service must ask for, keeps other programs out. wrap: what its
    command starts with (barry_launcher_shelld's scope, which execs it in
    place)."""
    with socket.socket() as s:  # a free port; the service binds it at once
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]
    token = secrets.token_urlsafe(24)
    # This Python, without the user's site-packages: what the app brings,
    # and the standard library.
    proc = subprocess.Popen(
        [*(wrap or []), sys.executable or "python3", "-s", "-u", os.path.join(app_dir, service)],
        cwd=app_dir, env=dict(env, BARRY_SERVICE_PORT=str(port), BARRY_SERVICE_TOKEN=token),
        stdin=subprocess.DEVNULL, stdout=out, stderr=subprocess.STDOUT,
    )
    return proc, f"http://127.0.0.1:{port}/", token


def stop_service(proc: subprocess.Popen | None, wait_s: float = 3.0) -> None:
    """Stop a service started by start_service, and wait until it has
    ended."""
    if proc is None or proc.poll() is not None:
        return
    proc.terminate()
    try:
        proc.wait(wait_s)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(2)


def _read_json(path: str, default):
    try:
        with open(path, encoding="utf-8") as fh:
            return json.load(fh)
    except (OSError, ValueError):
        return default


def _write_json(path: str, value) -> None:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as fh:
        json.dump(value, fh, indent=1)
        fh.write("\n")
    os.replace(tmp, path)


def _forget_layout(app_id: str) -> None:
    layout = _read_json(HOME_LAYOUT, None)
    if not isinstance(layout, dict):
        return
    changed = False
    for key in ("order", "hidden"):
        if isinstance(layout.get(key), list) and app_id in layout[key]:
            layout[key] = [a for a in layout[key] if a != app_id]
            changed = True
    if changed:
        _write_json(HOME_LAYOUT, layout)


def _set_removed(app_id: str, removed: bool) -> None:
    ids = _read_json(REMOVED, [])
    ids = [a for a in ids if isinstance(a, str)] if isinstance(ids, list) else []
    if removed == (app_id in ids):
        return
    _write_json(REMOVED, sorted(set(ids) | {app_id}) if removed else [a for a in ids if a != app_id])


def bundled() -> dict[str, dict]:
    """id -> manifest, with "dir", of the apps that come with Barry Launcher."""
    apps = {}
    try:
        names = sorted(os.listdir(BUNDLED))
    except OSError:
        return apps
    for name in names:
        root = os.path.join(BUNDLED, name)
        try:
            m = read_manifest(root)
        except AppError:
            continue
        apps[m["id"]] = dict(m, dir=root)
    return apps


def _version(v: str) -> tuple:
    return tuple(int(p) if p.isdigit() else 0 for p in re.split(r"[.+-]", v))


def install_bundled() -> list[str]:
    """Install the apps that come with Barry Launcher and are not there, or
    are older; not the ones the user removed. The ids installed."""
    removed = _read_json(REMOVED, [])
    have = installed()
    done = []
    for app_id, m in bundled().items():
        if app_id in removed:
            continue
        if app_id in have and _version(have[app_id]["version"]) >= _version(m["version"]):
            continue
        try:
            install(m["dir"])
            done.append(app_id)
        except (AppError, OSError):
            continue
    return done


def installed() -> dict[str, dict]:
    """id -> manifest, with "dir", of every installed app that still reads
    cleanly (a broken one is left out)."""
    apps = {}
    try:
        names = sorted(os.listdir(APPS_DIR))
    except OSError:
        return apps
    for name in names:
        root = os.path.join(APPS_DIR, name)
        if name.startswith(".") or not os.path.isdir(root):
            continue
        try:
            m = read_manifest(root)
        except AppError:
            continue
        if m["id"] != name:
            continue
        apps[name] = dict(m, dir=root)
    return apps


def stamp() -> float | None:
    """Changes when an app is installed, updated or removed."""
    try:
        return os.stat(APPS_DIR).st_mtime
    except OSError:
        return None
