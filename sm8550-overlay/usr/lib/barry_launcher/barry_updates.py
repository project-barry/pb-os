"""barry_updates: new versions of the Barry Launcher apps Project Barry
makes outside the image (Pip-Boy, Stardew Dual Screen, ...), offered once
after each PB-OS update.

The apps it knows are in CATALOG, a file the image brings:

  {
    "format": 1,
    "apps": [
      {
        "id": "io.github.project-barry.pipboy",  the app's barry-app.json id
        "name": "Pip-Boy",                      shown when not installed (never, today)
        "repo": "project-barry/PyPipboyApp",    its GitHub repository
        "asset": "^pipboy-[0-9][0-9A-Za-z.+-]*\\.zip$",  the release file (a regex)
        "manifest": "barry/barry-app.json",     optional: see below
        "tags": "^v[0-9]"                       optional: which releases (a regex; this the default)
      }, ...
    ]
  }

Only installed apps are looked at, and never one the image brings itself
(barry_apps.BUNDLED: Dino), which comes with the next image instead.

When: barry_launcher_shelld asks at its start (Barry Launcher starting)
whether a check is due (due): the PB-OS version (OS_VERSION, the image's
or the in-place update's) is not the one in STATE, the user's
~/.local/share/barry_launcher/app-updates.json. STATE takes the version
once the user has answered an offer (Update or Not now), or once a check
went through with nothing to offer. A check that could not finish
(offline, GitHub not answering: NetError) leaves it: the next start checks
again, and so does shelld when the network comes up meanwhile (Gio's
network monitor). Nothing else, and no timers: one check per PB-OS version
that gets through, with nothing on the screen unless there is something
new.

The check (check): for each such app, its repository's releases
(api.github.com, no token: 60 requests an hour is plenty); the newest
release (not a draft or pre-release) whose tag matches "tags" and that has
a file matching "asset". Its version, to compare with the installed
manifest's (newer): the app's own barry-app.json at that release's tag,
"manifest" in the repository (raw.githubusercontent.com), which must name
the same id; its "minApi" too (barry_apps.API: a release this Barry
Launcher cannot install is not offered). Without "manifest": the tag,
vX.Y.Z or X.Y.Z. (One repository's tag is not its apps' versions when it
holds several: barry-launcher-apps v1.2.0 has Dice 1.0.0.) Repositories
without releases (or gone) offer nothing, quietly.

The offer is the "updates" topic (payload) the shell shows
(shell/AppUpdates.qml): the apps with their versions, Update and Not now.
Update (update): each app's file is downloaded into a temporary folder
(https only, GitHub's hosts, at most barry_apps.MAX_ARCHIVE), its size
checked against the release's, its SHA-256 against GitHub's digest when
the release has one, then barry_apps.check reads it: a zip, its id the
app's, its version newer than the installed one. Then it installs the way
every app install goes (shelld's install_app: barry_apps.install, which
keeps the app's data, closes a running old copy and refreshes the home
screen). Each app says how it went; one that fails keeps its old version.
Nothing from the download runs here: it is an archive barry_apps unpacks
and checks like any other.
"""
from __future__ import annotations

import hashlib
import json
import os
import re
import shutil
import socket
import tempfile
import threading
import urllib.error
import urllib.parse
import urllib.request
import zipfile

import barry_apps

CATALOG = "/usr/share/barry_launcher/app-updates.json"
STATE = os.path.join(barry_apps.DATA, "app-updates.json")
OS_VERSION = "/usr/share/pb-os/version"  # make-steamos-sm8650.sh's, and pbos-update's
CATALOG_FORMAT = 1
API_URL = "https://api.github.com"
RAW_URL = "https://raw.githubusercontent.com"
# Where a release's file may come from: GitHub's download link, and the
# hosts it sends the download on to.
DOWNLOAD_HOSTS = re.compile(r"^(github\.com|([a-z0-9-]+\.)*githubusercontent\.com)$")
TIMEOUT_S = 20
JSON_MAX = 4 * 1024 * 1024  # an answer bigger than that is not GitHub's
RELEASES_PER_PAGE = 30
DEFAULT_TAGS = r"^v?[0-9]"
REPO_RE = re.compile(r"^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$")
USER_AGENT = "Barry-Launcher-app-updates"


class NetError(Exception):
    """GitHub could not be asked (offline, a timeout, an error page): the
    check did not finish, and is tried again."""


class UpdateError(Exception):
    """A download that will not install, and why (shown to the user)."""


# --- When ------------------------------------------------------------------

def os_version(path: str = OS_VERSION) -> str:
    """The PB-OS version this system runs, or "" (not PB-OS)."""
    try:
        with open(path, encoding="utf-8") as fh:
            return fh.read().strip()
    except OSError:
        return ""


def read_state(path: str = STATE) -> dict:
    try:
        with open(path, encoding="utf-8") as fh:
            state = json.load(fh)
    except (OSError, ValueError):
        return {}
    return state if isinstance(state, dict) else {}


def write_state(version: str, path: str = STATE) -> None:
    """This PB-OS version's check is done: answered, or nothing to offer."""
    state = read_state(path)
    state["checked"] = version
    barry_apps._write_json(path, state)


def due(version: str, state: dict) -> bool:
    """A check is due: a PB-OS version that has not had its check yet."""
    return bool(version) and state.get("checked") != version


# --- The catalog -------------------------------------------------------------

def _rel_path(p) -> bool:
    return (isinstance(p, str) and p != "" and not p.startswith("/")
            and ".." not in p.split("/") and "\\" not in p)


def load_catalog(path: str = CATALOG, log=print) -> list[dict]:
    """The catalog's apps, each checked; one that is not as it should be is
    left out (and logged), never the rest."""
    try:
        with open(path, encoding="utf-8") as fh:
            data = json.load(fh)
    except FileNotFoundError:
        return []
    except (OSError, ValueError) as err:
        log(f"app updates: cannot read {path}: {err}")
        return []
    if not isinstance(data, dict) or data.get("format") != CATALOG_FORMAT or not isinstance(data.get("apps"), list):
        log(f"app updates: {path} is not a format {CATALOG_FORMAT} catalog")
        return []
    out, seen = [], set()
    for e in data["apps"]:
        try:
            if not isinstance(e, dict):
                raise ValueError("not an object")
            app_id, repo = e.get("id"), e.get("repo")
            if not isinstance(app_id, str) or not barry_apps.ID_RE.match(app_id) or len(app_id) > barry_apps.ID_MAX:
                raise ValueError(f"id {app_id!r}")
            if app_id in seen:
                raise ValueError(f"{app_id} twice")
            if not isinstance(repo, str) or not REPO_RE.match(repo):
                raise ValueError(f"repo {repo!r}")
            if not isinstance(e.get("asset"), str):
                raise ValueError("no asset")
            asset = re.compile(e["asset"])
            tags = re.compile(e.get("tags", DEFAULT_TAGS)) if isinstance(e.get("tags", DEFAULT_TAGS), str) else None
            if tags is None:
                raise ValueError("tags")
            manifest = e.get("manifest")
            if manifest is not None and not _rel_path(manifest):
                raise ValueError(f"manifest {manifest!r}")
            name = e.get("name") if isinstance(e.get("name"), str) and e["name"].strip() else app_id
        except (ValueError, re.error) as err:
            log(f"app updates: {path}: an entry left out ({err})")
            continue
        seen.add(app_id)
        out.append({"id": app_id, "name": name.strip(), "repo": repo, "asset": asset, "tags": tags,
                    "manifest": manifest})
    return out


# --- Versions and releases -----------------------------------------------------

def tag_version(tag) -> str | None:
    """A release tag's version: v1.2.0 or 1.2.0 -> "1.2.0"; else None."""
    m = re.match(r"^v?([0-9]+(?:\.[0-9]+)*)$", tag) if isinstance(tag, str) else None
    return m.group(1) if m else None


def newer(candidate: str, installed: str) -> bool:
    """candidate is a later version than installed (barry_apps._version,
    with trailing zeros alike: 1.0 is 1.0.0)."""
    def key(v: str) -> tuple:
        k = list(barry_apps._version(v))
        while len(k) > 1 and k[-1] == 0:
            k.pop()
        return tuple(k)
    return key(candidate) > key(installed)


def pick_release(releases, entry: dict) -> tuple[dict, dict] | None:
    """The newest release for entry (not a draft or pre-release, its tag
    matching "tags", with a file matching "asset"), and that file."""
    best = None
    for rel in releases if isinstance(releases, list) else []:
        if not isinstance(rel, dict) or rel.get("draft") or rel.get("prerelease"):
            continue
        tag = rel.get("tag_name")
        if not isinstance(tag, str) or not entry["tags"].search(tag):
            continue
        asset = next((a for a in rel.get("assets") or [] if isinstance(a, dict)
                      and isinstance(a.get("name"), str) and entry["asset"].search(a["name"])
                      and isinstance(a.get("browser_download_url"), str)), None)
        if asset is None:
            continue
        key = barry_apps._version(tag_version(tag) or re.sub(r"^\D*", "", tag) or "0")
        if best is None or key > best[0]:
            best = (key, rel, asset)
    return (best[1], best[2]) if best else None


# --- GitHub ------------------------------------------------------------------

class _HttpsOnly(urllib.request.HTTPRedirectHandler):
    """Redirects only to https, and only to GitHub's hosts."""

    def redirect_request(self, req, fp, code, msg, headers, newurl):
        u = urllib.parse.urlsplit(newurl)
        if u.scheme != "https" or not DOWNLOAD_HOSTS.match(u.hostname or ""):
            raise urllib.error.HTTPError(newurl, code, f"redirect to {u.scheme}://{u.hostname}", headers, fp)
        return super().redirect_request(req, fp, code, msg, headers, newurl)


class GitHub:
    """What the check and the downloads ask of GitHub (a fake one in the
    tests): over https, with timeouts, without a token."""

    def __init__(self) -> None:
        self.opener = urllib.request.build_opener(_HttpsOnly)

    def _open(self, url: str, accept: str):
        if not url.startswith("https://"):
            raise NetError(f"not https: {url}")
        req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT, "Accept": accept})
        return self.opener.open(req, timeout=TIMEOUT_S)

    def get_json(self, url: str):
        """url's JSON, or None when it is not there (404: no releases, or
        no such file at that tag). NetError when it cannot be asked."""
        try:
            with self._open(url, "application/vnd.github+json") as r:
                body = r.read(JSON_MAX + 1)
        except urllib.error.HTTPError as err:
            if err.code in (404, 410, 451):
                return None
            raise NetError(f"{url}: HTTP {err.code}") from None
        except (urllib.error.URLError, socket.timeout, TimeoutError, ConnectionError, OSError) as err:
            raise NetError(f"{url}: {getattr(err, 'reason', err)}") from None
        if len(body) > JSON_MAX:
            raise NetError(f"{url}: the answer is too big")
        try:
            return json.loads(body)
        except ValueError:
            raise NetError(f"{url}: not JSON") from None

    def download(self, url: str, dest: str, limit: int) -> None:
        """url into the file dest, at most limit bytes."""
        u = urllib.parse.urlsplit(url)
        if u.scheme != "https" or not DOWNLOAD_HOSTS.match(u.hostname or ""):
            raise UpdateError("the release's file is not on GitHub")
        try:
            with self._open(url, "application/octet-stream") as r, open(dest, "wb") as out:
                got = 0
                while True:
                    chunk = r.read(65536)
                    if not chunk:
                        break
                    got += len(chunk)
                    if got > limit:
                        raise UpdateError("the download is bigger than the release says")
                    out.write(chunk)
        except urllib.error.HTTPError as err:
            raise UpdateError(f"the download failed (HTTP {err.code})") from None
        except (urllib.error.URLError, socket.timeout, TimeoutError, ConnectionError) as err:
            raise UpdateError(f"the download failed ({getattr(err, 'reason', err)})") from None


def releases_url(repo: str) -> str:
    return f"{API_URL}/repos/{repo}/releases?per_page={RELEASES_PER_PAGE}"


def manifest_url(repo: str, tag: str, path: str) -> str:
    return f"{RAW_URL}/{repo}/{urllib.parse.quote(tag, safe='')}/{urllib.parse.quote(path)}"


# --- The check -------------------------------------------------------------------

def check(catalog: list[dict], installed: dict[str, dict], bundled_ids, gh, log=print) -> tuple[list[dict], bool]:
    """The offers (newer versions of the installed catalog apps), and
    whether the check went through (False: some of GitHub could not be
    asked; NetError)."""
    offers, complete = [], True
    releases: dict[str, object] = {}  # a repository's, asked once (or its NetError)
    for entry in catalog:
        app_id = entry["id"]
        have = installed.get(app_id)
        if have is None or app_id in bundled_ids:
            continue
        try:
            repo = entry["repo"]
            if repo not in releases:
                try:
                    releases[repo] = gh.get_json(releases_url(repo))
                except NetError as err:
                    releases[repo] = err
                    raise
            elif isinstance(releases[repo], NetError):
                complete = False
                continue  # logged already
            picked = pick_release(releases[repo], entry)
            if picked is None:
                continue  # no release yet (or none with its file): nothing to offer
            rel, asset = picked
            tag = rel["tag_name"]
            if entry["manifest"]:
                m = gh.get_json(manifest_url(repo, tag, entry["manifest"]))
                if not isinstance(m, dict) or m.get("id") != app_id or not isinstance(m.get("version"), str):
                    log(f"app updates: {repo} {tag}: no barry-app.json for {app_id} at {entry['manifest']}")
                    continue
                version = m["version"].strip()
                min_api = m.get("minApi", 1)
                if isinstance(min_api, int) and not isinstance(min_api, bool) and min_api > barry_apps.API:
                    log(f"app updates: {app_id} {version} needs Barry Launcher API level {min_api}; not offered")
                    continue
            else:
                version = tag_version(tag)
                if version is None:
                    log(f"app updates: {repo}: tag {tag} is no version")
                    continue
        except NetError as err:
            log(f"app updates: {err}")
            complete = False
            continue
        if not newer(version, have["version"]):
            continue
        size = asset.get("size")
        digest = asset.get("digest")
        offers.append({"id": app_id, "name": have.get("name") or entry["name"], "installed": have["version"],
                       "version": version, "repo": entry["repo"], "tag": tag, "file": asset["name"],
                       "url": asset["browser_download_url"],
                       "size": size if isinstance(size, int) and not isinstance(size, bool) else None,
                       "digest": digest if isinstance(digest, str) else None})
    return offers, complete


def fetch(offer: dict, gh, folder: str) -> tuple[str, dict]:
    """The offer's file, downloaded into folder and checked (see the top):
    its path and manifest. UpdateError when it will not do."""
    size = offer.get("size")
    if size is not None and not 0 < size <= barry_apps.MAX_ARCHIVE:
        raise UpdateError(f"the release's file is {size // 1048576} MB; the limit is "
                          f"{barry_apps.MAX_ARCHIVE // 1048576} MB")
    name = os.path.basename(offer.get("file") or "")
    if not name.lower().endswith(".zip"):
        raise UpdateError("the release's file is not a zip")
    path = os.path.join(folder, "app.zip")
    gh.download(offer["url"], path, size if size is not None else barry_apps.MAX_ARCHIVE)
    got = os.path.getsize(path)
    if size is not None and got != size:
        raise UpdateError(f"the download has {got} bytes; the release says {size}")
    digest = offer.get("digest") or ""
    if digest.startswith("sha256:"):
        h = hashlib.sha256()
        with open(path, "rb") as fh:
            for chunk in iter(lambda: fh.read(65536), b""):
                h.update(chunk)
        if h.hexdigest() != digest[len("sha256:"):].lower():
            raise UpdateError("the download does not match GitHub's checksum")
    if not zipfile.is_zipfile(path):
        raise UpdateError("the download is not a zip")
    try:
        m = barry_apps.check(path)
    except barry_apps.AppError as err:
        raise UpdateError(str(err)) from None
    if m["id"] != offer["id"]:
        raise UpdateError(f"the download is another app ({m['id']})")
    if not newer(m["version"], offer["installed"]):
        raise UpdateError(f"the download is version {m['version']}, not newer than {offer['installed']}")
    return path, m


# --- The offer, as shelld keeps it -------------------------------------------------

class Updater:
    """The check and the offer for barry_launcher_shelld. install(path)
    installs a checked archive ((ok, error)); changed() says payload()
    changed (shelld publishes the topic). Thread-safe."""

    def __init__(self, gh, install, changed, catalog: str = CATALOG, state: str = STATE,
                 version: str | None = None, log=print, background: bool = True) -> None:
        self.gh, self.install, self.changed, self.log = gh, install, changed, log
        self.catalog, self.state = catalog, state
        self.version = os_version() if version is None else version
        self.background = background
        self.lock = threading.Lock()
        self.phase = "idle"  # idle, offer, updating, done
        self.apps: list[dict] = []
        self.serial = 0  # counts the offers
        self.checking = False

    def payload(self) -> dict:
        """The "updates" topic: phase, and the apps offered with each one's
        status ("", "updating", "updated", "failed") and error."""
        with self.lock:
            return {"serial": self.serial, "phase": self.phase,
                    "apps": [{"id": a["id"], "name": a["name"], "installed": a["installed"],
                              "version": a["version"], "status": a.get("status", ""),
                              "error": a.get("error", "")} for a in self.apps]}

    def due(self) -> bool:
        return due(self.version, read_state(self.state))

    def run_check(self) -> bool:
        """Check if due, and offer what is new. True when nothing is left
        to try again (done, or not due); False when GitHub could not be
        asked and nothing is offered."""
        with self.lock:
            if self.checking or self.phase != "idle" or not self.due():
                return True
            self.checking = True
        try:
            offers, complete = check(load_catalog(self.catalog, self.log), barry_apps.installed(),
                                     set(barry_apps.bundled()), self.gh, self.log)
        finally:
            with self.lock:
                self.checking = False
        if offers:
            with self.lock:
                self.apps = offers
                self.phase = "offer"
                self.serial += 1
            self.log("app updates: " + ", ".join(f"{a['id']} {a['installed']} -> {a['version']}" for a in offers))
            self.changed()
            return True
        if complete:
            write_state(self.version, self.state)
            self.log(f"app updates: none for PB-OS {self.version}")
            return True
        self.log("app updates: the check did not get through; again when the network is up, or next start")
        return False

    def answer(self, body: dict) -> tuple[int, dict]:
        """POST /updates {"do": "update" | "later" | "close"}."""
        do = body.get("do")
        with self.lock:
            phase = self.phase
            if do == "update" and phase == "offer":
                self.phase = "updating"
            elif do == "later" and phase == "offer":
                self.phase, self.apps = "idle", []
            elif do == "close" and phase == "done":
                self.phase, self.apps = "idle", []
            else:
                return 409, {"ok": False, "error": f"cannot {do!r} now ({phase})"}
        if do in ("update", "later"):
            try:
                write_state(self.version, self.state)  # not asked again until the next PB-OS version
            except OSError as err:
                self.log(f"app updates: cannot write {self.state}: {err}")
        self.changed()
        if do == "update":
            if self.background:
                threading.Thread(target=self.run_updates, daemon=True).start()
            else:
                self.run_updates()
        return 200, {"ok": True}

    def _set(self, app: dict, status: str, error: str = "") -> None:
        with self.lock:
            app["status"], app["error"] = status, error
        self.changed()

    def run_updates(self) -> None:
        for app in list(self.apps):
            self._set(app, "updating")
            have = barry_apps.installed().get(app["id"])
            if have is None:
                self._set(app, "failed", "it is not installed any more")
                continue
            with self.lock:
                app["installed"] = have["version"]
            folder = tempfile.mkdtemp(prefix="barry-app-update-")
            try:
                path, m = fetch(app, self.gh, folder)
                with self.lock:
                    app["version"] = m["version"]  # the file's own word
                ok, error = self.install(path)
            except UpdateError as err:
                ok, error = False, str(err)
            except OSError as err:
                ok, error = False, f"cannot download: {err.strerror or err}"
            finally:
                shutil.rmtree(folder, ignore_errors=True)
            if ok:
                self.log(f"app updates: {app['id']} updated to {app['version']}")
                self._set(app, "updated")
            else:
                self.log(f"app updates: {app['id']} not updated: {error}")
                self._set(app, "failed", error or "it did not install")
        with self.lock:
            self.phase = "done"
        self.changed()
