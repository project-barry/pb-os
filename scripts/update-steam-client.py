#!/usr/bin/env python3
"""Bring a Steam ARM64 client folder to Valve's latest build of a channel.

  update-steam-client.py STEAM_HOME [--channel steamdeck_stable] [--check]

Steam's own updater (the bootstrapper) is off on PB-OS: on ARM its self-update
fails (http error 0) and leaves first-time setup on Retry, so RUNSTEAM.sh and
steam.cfg inhibit it. This does the same job from outside the client:

  1. reads Valve's manifest for the channel
     (client-update.steamstatic.com/steam_client_<channel>_linuxarm64);
  2. downloads every package zip into STEAM_HOME/package and checks its size
     and SHA-256 (packages already there with the right checksum are kept);
  3. only when all of them check out, unpacks them into STEAM_HOME (each file
     written aside and moved into place, so a running Steam keeps its open
     files), removes the files the previous build listed and this one does
     not, and writes package/steam_client_<channel>_linuxarm64.installed and
     .manifest the way the bootstrapper does, package/beta and steam.inf.

Package zips, .installed and .manifest files of other builds are removed.
Nothing outside STEAM_HOME is touched, and no account data is read.

--check only compares: exit 0 when STEAM_HOME has the channel's latest build,
1 when it is behind, 2 on an error.
Steam must be restarted afterwards to run the new build.
"""
from __future__ import annotations

import argparse
import hashlib
import os
import re
import stat
import sys
import urllib.request
import zipfile
from pathlib import Path

CDN = "https://client-update.steamstatic.com/"
PLATFORM = "linuxarm64"
AGENT = "pb-os-steam-client/1"


def log(msg: str) -> None:
    print(f"update-steam-client: {msg}", flush=True)


def fetch(url: str) -> bytes:
    req = urllib.request.Request(url, headers={"User-Agent": AGENT})
    with urllib.request.urlopen(req, timeout=60) as r:
        return r.read()


def parse_vdf(text: str) -> dict:
    """Valve's text KeyValues: quoted keys and values, { } for blocks."""
    tokens = re.findall(r'"((?:[^"\\]|\\.)*)"|([{}])', text)
    stack: list[dict] = [{}]
    key = None
    for quoted, brace in tokens:
        if brace == "{":
            block: dict = {}
            stack[-1][key] = block
            stack.append(block)
            key = None
        elif brace == "}":
            stack.pop()
        elif key is None:
            key = quoted
        else:
            stack[-1][key] = quoted
            key = None
    return stack[0]


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def installed_version(home: Path, channel: str) -> str | None:
    f = home / "package" / f"steam_client_{channel}_{PLATFORM}.installed"
    if not f.is_file():
        return None
    first = f.read_text(errors="replace").split("\n", 1)[0]
    m = re.search(r";(\d+);", first)
    return m.group(1) if m else None


def old_paths(home: Path) -> set[str]:
    """Every client file the builds now in package/ installed."""
    paths: set[str] = set()
    for f in (home / "package").glob(f"steam_client_*_{PLATFORM}.installed"):
        for line in f.read_text(errors="replace").splitlines():
            path = line.rsplit(",", 1)[0]
            if path and not path.endswith("/"):
                paths.add(path)
    return paths


def safe_target(home: Path, name: str) -> Path:
    """The entry's own path: links already on disk are not followed (a
    package that replaces a link must replace the link, not its target)."""
    rel = os.path.normpath(name)
    if os.path.isabs(rel) or rel == ".." or rel.startswith("../"):
        raise SystemExit(f"package entry outside the Steam folder: {name}")
    return home / rel


def link_target(entry: str, link: str) -> str:
    """A package's symbolic link may only point inside the Steam folder."""
    dest = os.path.normpath(os.path.join(os.path.dirname(entry), link))
    if os.path.isabs(link) or dest == ".." or dest.startswith("../"):
        raise SystemExit(f"package link outside the Steam folder: {entry} -> {link}")
    return link


def owner_of(path: Path) -> tuple[int, int] | None:
    """Files written as root get the Steam folder's owner (image builds)."""
    if os.geteuid() != 0:
        return None
    st = path.stat()
    return st.st_uid, st.st_gid


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("steam_home")
    ap.add_argument("--channel", default="steamdeck_stable")
    ap.add_argument("--check", action="store_true")
    a = ap.parse_args()

    home = Path(a.steam_home).resolve()
    if not home.is_dir():
        log(f"no Steam folder at {home}")
        return 2
    pkgdir = home / "package"
    name = f"steam_client_{a.channel}_{PLATFORM}"

    try:
        manifest_text = fetch(CDN + name).decode("utf-8")
    except OSError as e:
        log(f"cannot read the {a.channel} manifest: {e}")
        return 2
    manifest = parse_vdf(manifest_text).get(PLATFORM, {})
    version = manifest.get("version")
    packages = {k: v for k, v in manifest.items() if isinstance(v, dict) and "file" in v}
    if not version or not packages:
        log("the manifest has no version or no packages")
        return 2

    have = installed_version(home, a.channel)
    others = [p.name for p in pkgdir.glob(f"steam_client_*_{PLATFORM}.installed")
              if p.name != f"{name}.installed"]
    log(f"{a.channel}: latest {version}, installed {have or 'none'}"
        + (f" (also {', '.join(others)})" if others else ""))
    current = have == version and not others
    if a.check:
        return 0 if current else 1
    if current:
        log("already up to date")
        return 0

    pkgdir.mkdir(parents=True, exist_ok=True)
    owner = owner_of(home)

    # 1. Every package downloaded and checked before anything changes.
    zips: list[Path] = []
    for key, p in packages.items():
        dest = pkgdir / p["file"]
        want_sha, want_size = p.get("sha2", "").lower(), int(p.get("size", "-1"))
        if dest.is_file() and dest.stat().st_size == want_size and sha256(dest) == want_sha:
            zips.append(dest)
            continue
        log(f"download {p['file']} ({want_size // 1024} KiB)")
        part = dest.with_name(dest.name + ".part")
        try:
            req = urllib.request.Request(CDN + p["file"], headers={"User-Agent": AGENT})
            with urllib.request.urlopen(req, timeout=120) as r, open(part, "wb") as f:
                while chunk := r.read(1 << 20):
                    f.write(chunk)
        except OSError as e:
            part.unlink(missing_ok=True)
            log(f"download failed for {key}: {e}")
            return 2
        if part.stat().st_size != want_size or sha256(part) != want_sha:
            part.unlink(missing_ok=True)
            log(f"checksum mismatch for {key}: nothing changed")
            return 2
        os.replace(part, dest)
        zips.append(dest)

    # 2. Unpack; each file written aside and moved into place.
    stale = old_paths(home)
    lines: list[str] = []
    seen: set[str] = set()
    for z in zips:
        with zipfile.ZipFile(z) as zf:
            for info in zf.infolist():
                # Valve's zips separate folders with backslashes.
                entry = info.filename.replace("\\", "/")
                target = safe_target(home, entry)
                if entry.endswith("/"):
                    target.mkdir(parents=True, exist_ok=True)
                    if owner:
                        os.chown(target, *owner)
                    if entry not in seen:
                        lines.append(f"{entry},-1;{version};0")
                        seen.add(entry)
                    continue
                target.parent.mkdir(parents=True, exist_ok=True)
                tmp = target.with_name(f".{target.name}.pbos-new")
                if os.path.lexists(tmp):
                    os.unlink(tmp)
                # Unix entries may be symbolic links (the codec libraries'
                # video/ folders); the bootstrapper lists them with size -2.
                if info.create_system == 3 and stat.S_ISLNK(info.external_attr >> 16):
                    os.symlink(link_target(entry, zf.read(info).decode()), tmp)
                    if owner:
                        os.lchown(tmp, *owner)
                    os.replace(tmp, target)
                    if entry not in seen:
                        lines.append(f"{entry},-2;{version};{info.CRC}")
                        seen.add(entry)
                    continue
                with zf.open(info) as src, open(tmp, "wb") as dst:
                    while chunk := src.read(1 << 20):
                        dst.write(chunk)
                # The zips carry no Unix modes; Steam's own updater leaves
                # every client file at 0775.
                os.chmod(tmp, 0o775)
                if owner:
                    os.chown(tmp, *owner)
                os.replace(tmp, target)
                if entry not in seen:
                    lines.append(f"{entry},{info.file_size};{version};{info.CRC}")
                    seen.add(entry)

    # 3. Files of the previous build that this one no longer has.
    removed = 0
    for path in sorted(stale - seen):
        target = safe_target(home, path)
        if target.is_symlink() or target.is_file():
            target.unlink()
            removed += 1

    # 4. The bootstrapper's own records, and only this build's packages.
    keep = {p["file"] for p in packages.values()}
    for f in pkgdir.iterdir():
        if f.is_file() and ".zip" in f.name and f.name not in keep:
            f.unlink()
        elif f.name.startswith("steam_client_") and f.name.endswith((".installed", ".manifest")) \
                and not f.name.startswith(name + "."):
            f.unlink()
    (pkgdir / f"{name}.installed").write_text("\n".join(lines) + "\n")
    (pkgdir / f"{name}.manifest").write_text(manifest_text)
    (pkgdir / "beta").write_text(a.channel + "\n")
    if "steam.inf" not in seen:
        for d in (home, home / "steamrtarm64"):
            if d.is_dir():
                (d / "steam.inf").write_text(f"ClientVersion={version}\n")
    if owner:
        for f in (pkgdir / f"{name}.installed", pkgdir / f"{name}.manifest", pkgdir / "beta",
                  home / "steam.inf", home / "steamrtarm64" / "steam.inf"):
            if f.exists():
                os.chown(f, *owner)

    log(f"installed {a.channel} {version}: {len(zips)} packages, {len(seen)} entries, "
        f"{removed} old files removed; restart Steam to run it")
    return 0


if __name__ == "__main__":
    sys.exit(main())
