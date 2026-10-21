#!/usr/bin/env python3
"""Build a verified offline update bundle from a completed rootfs and boot image.

The rootfs is the one make-steamos-sm8650.sh packed into the image, with the
same SOC. Devices install it with PB-OS Utils (Decky) or the
SteamOS Update app, which find it in a GitHub release as
  pb-os-<version>-<image>.update.tar.gz.001, .002, ...              full system
  pb-os-<version>-<image>.from-<old version>.delta.tar.gz.001, ...  what changed
(--release DIR writes those parts and the lines for SHA256SUMS).

Every build also writes pb-os-<version>-<image>.state.json.gz: each system
file with its hash and permissions. A later build turns it into a delta with
--from-state. --state-only writes one for an image built before this, from
its root partition mounted read-only.

Feature and patch releases: a feature release (e.g. alpha-v0.4) ships the full
package. Each patch release after it (alpha-v0.4.1, alpha-v0.4.2, ...)
ships one cumulative delta named from the feature release, which also installs
on every patch before it:
  --base-state <feature release state> --patch-state <each earlier patch's state>
The delta then carries every file that differs from the feature release or
from any of those patches, and deletes what any of them had that this release
does not. The rootfs must be stamped with that base (PB_OS_BASE).
A feature release can also ship the same kind of delta from the previous
feature release and its patches, beside its full package, so devices on that
line download only what changed (the updater looks for the delta from its
installed base first):
  --prev-base-state <previous feature release state> --prev-patch-state <each of its patches' state>
"""
import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import stat
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
import root_trim

ap = argparse.ArgumentParser()
ap.add_argument('--rootfs', required=True)
ap.add_argument('--kernel', required=True, help='the kernel build output KERNEL (before the image retargets it)')
ap.add_argument('--version', help='default: the rootfs /usr/share/pb-os/version')
ap.add_argument('--output', help='package file (or use --release); with --state-only, the state file')
ap.add_argument('--release', metavar='DIR', help='write release parts named for the updater into DIR')
ap.add_argument('--from-state', action='append', default=[], metavar='FILE',
                help='also build a delta from the release this state file describes (repeatable)')
ap.add_argument('--base-state', metavar='FILE',
                help='patch release: the state of the feature release it patches; builds the cumulative delta and no full package')
ap.add_argument('--patch-state', action='append', default=[], metavar='FILE',
                help='with --base-state: the state of each earlier patch of that feature release (repeatable)')
ap.add_argument('--prev-base-state', metavar='FILE',
                help='feature release: also build the cumulative delta from the previous feature release this state describes')
ap.add_argument('--prev-patch-state', action='append', default=[], metavar='FILE',
                help='with --prev-base-state: the state of each patch of that feature release (repeatable)')
ap.add_argument('--state-only', action='store_true', help='only write the state file of --rootfs')
ap.add_argument('--soc', choices=('sm8650', 'sm8550', 'all'), default='sm8650',
                help="all: the one image's package ('pb-os') for every device")
ap.add_argument('--device', choices=('thor',), help='ignored: the SM8550 image includes the AYN Thor')
ap.add_argument('--thor-bridge', action='store_true',
                help='SM8550 with --release: also name the full package for the AYN Thor\'s old channel '
                     '(pb-os-<version>-thor.update.tar.gz), which Thors on a separate Thor image still look for')
ap.add_argument('--rp6-bridge', action='store_true',
                help='SM8550 with --release: also name every package (full and deltas) for the old SM8550 '
                     'channel (pb-os-<version>-rp6...), the only one updaters from before the rename look for')
a = ap.parse_args()
if a.thor_bridge and (a.soc != 'sm8550' or not a.release or a.base_state):
    raise SystemExit('--thor-bridge needs --soc sm8550, --release and a full package (no --base-state)')
if a.rp6_bridge and (a.soc != 'sm8550' or not a.release):
    raise SystemExit('--rp6-bridge needs --soc sm8550 and --release')
# Device models (DTB `model`) each package may install on: the one image
# (SOC all) takes every device; earlier releases had one image per SoC.
SOC_DEVICES = {'sm8650': ['KONKR Pocket FIT', 'AYANEO Pocket S2'],
               'sm8550': ['Retroid Pocket 6', 'Retroid Pocket 6 TOP-DPAD', 'Retroid Pocket Nova', 'AYN Thor',
                          'AYN Odin 2', 'AYN Odin 2 Mini', 'AYN Odin 2 Portal']}
DEVICES = SOC_DEVICES['sm8550'] + SOC_DEVICES['sm8650'] if a.soc == 'all' else SOC_DEVICES[a.soc]
root = Path(a.rootfs).resolve()
stamped = root / 'usr/share/pb-os/version'
version = a.version or (stamped.read_text().strip() if stamped.is_file() else '')
if not version: raise SystemExit('no --version and the rootfs has no /usr/share/pb-os/version')
if stamped.is_file() and stamped.read_text().strip() != version:
    raise SystemExit(f'--version {version} differs from the rootfs version {stamped.read_text().strip()}')
# The feature release this one belongs to (make-steamos-sm8650.sh PB_OS_BASE).
stamped_base = root / 'usr/share/pb-os/base'
release_base = stamped_base.read_text().strip() if stamped_base.is_file() else version
if a.patch_state and not a.base_state: raise SystemExit('--patch-state needs --base-state')
if a.prev_patch_state and not a.prev_base_state: raise SystemExit('--prev-patch-state needs --prev-base-state')
if a.prev_base_state and a.base_state: raise SystemExit('--prev-base-state is for a feature release, not with --base-state')
# Image name in release assets; the updater maps DTB models to it (IMAGES).
# SM8550 packages were named 'rp6' before the rename (alpha-v0.5.1 and
# earlier; pbos-update.py OLD_NAMES); their state files still serve as delta bases.
# The one image's packages are 'pb-os' (pbos-update.py IMAGE; updaters from
# alpha-v0.5.2.3 on look for it).
image = {'sm8650': 'pocketfit', 'sm8550': 'sm8550', 'all': 'pb-os'}[a.soc]
OLD_IMAGES = {'sm8550': ('rp6',)}.get(a.soc, ())
if bool(a.output) == bool(a.release): raise SystemExit('give --output or --release')
out_dir = Path(a.release).resolve() if a.release else Path(a.output).resolve().parent
# The trees an update manages, and what in them is per installation.
MANAGED = ('usr', 'opt', 'etc', 'var/lib/overlays/etc/upper')
IDENTITY = ('etc/machine-id', 'var/lib/overlays/etc/upper/machine-id', 'opt/steamos-sm8650/IMAGE.txt')
# setuid root that make-steamos-sm8650.sh (restore_image_suid) sets on the image copy only.
SUID = {p: 0o4755 for p in ('usr/bin/pkexec', 'usr/sbin/pkexec', 'usr/bin/sudo', 'usr/sbin/sudo',
        'usr/lib/polkit-1/polkit-agent-helper-1', 'usr/bin/su', 'usr/bin/passwd', 'usr/bin/newgrp',
        'usr/bin/chsh', 'usr/bin/chfn', 'usr/bin/gpasswd', 'usr/bin/unix_chkpwd', 'usr/bin/mount',
        'usr/bin/umount')}
SUID['usr/lib/dbus-1.0/dbus-daemon-launch-helper'] = 0o4750
# Only the dbus group may run the launch helper (the dbus package ships it root:dbus).
SUID_GROUP = {'usr/lib/dbus-1.0/dbus-daemon-launch-helper': 'dbus'}


def suid_gid(root, rel):
    """Group of a setuid file: root, or its group by the rootfs's own /etc/group."""
    name = SUID_GROUP.get(rel)
    if not name: return 0
    for line in (root / 'etc/group').read_text().splitlines():
        f = line.split(':')
        if f[0] == name: return int(f[2])
    raise SystemExit(f'{name} group missing from the rootfs /etc/group')


def sha256(p):
    h = hashlib.sha256()
    with open(p, 'rb') as f:
        for b in iter(lambda: f.read(4 << 20), b''): h.update(b)
    return h.hexdigest()


def tree_state(base, hashes=None):
    """{path: entry} of the managed trees under base, as a device has them
    after a full update: setuid as on the image, no per-install identity."""
    entries = {}
    for top in MANAGED:
        if not (base / top).is_dir(): continue
        for dirpath, dirs, names in os.walk(base / top):
            for name in dirs + names:
                p = Path(dirpath) / name
                rel = str(p.relative_to(base))
                if rel in IDENTITY: continue
                st = p.lstat()
                if stat.S_ISLNK(st.st_mode): e = ['l', os.readlink(p), st.st_uid, st.st_gid]
                elif stat.S_ISDIR(st.st_mode): e = ['d', st.st_mode & 0o7777, st.st_uid, st.st_gid]
                elif stat.S_ISREG(st.st_mode):
                    sha = (hashes or {}).get('root/' + rel) or sha256(p)
                    e = ['f', st.st_mode & 0o7777, st.st_uid, st.st_gid, sha]
                    if rel in SUID: e[1:4] = [SUID[rel], 0, suid_gid(base, rel)]
                else: raise SystemExit(f'special file in the system tree: {rel}')
                entries[rel] = e
    return entries


# Android apps payload (Lepton overlays for pbos-apk): in home, not on the root
# partition. Packages carry it like the bundled plugins; deltas only when it changed.
ANDROID = 'home/steamos/.local/share/pbos-android'


def tree_digest(top):
    """One checksum over a directory's paths, modes and file contents."""
    h = hashlib.sha256()
    for p in sorted(top.rglob('*')) if top.is_dir() else []:
        st = p.lstat()
        h.update(f'{p.relative_to(top)}\0{st.st_mode:o}\0'.encode())
        if stat.S_ISREG(st.st_mode): h.update(sha256(p).encode())
        elif stat.S_ISLNK(st.st_mode): h.update(os.readlink(p).encode())
    return h.hexdigest() if top.is_dir() else None


LSFG_LAYER = 'usr/share/vulkan/implicit_layer.d/VkLayer_LS_frame_generation_arm64.json'


# Decky Loader itself, in home like the plugins: full packages always carry it,
# deltas when it differs from any release they install on (states from before
# this have no 'decky', so the first deltas after it carry it). The updater
# installs it only over an older Decky (manifest 'decky_loader').
DECKY = 'home/steamos/homebrew/services'
DECKY_FILES = ('PluginLoader', '.loader.version')


def decky_state(top):
    """The Decky Loader under top (a rootfs or the stage): version and checksum; None without one."""
    d = top / DECKY
    if not all((d / n).is_file() for n in DECKY_FILES): return None
    return {'version': (d / '.loader.version').read_text().strip(), 'sha256': sha256(d / 'PluginLoader')}


def write_state(path, entries, android, decky):
    with gzip.open(path, 'wt') as f:
        json.dump({'format': 1, 'version': version, 'base': release_base, 'image': image, 'kernel': sha256(a.kernel),
                   'android': android, 'decky': decky, 'entries': entries}, f, separators=(',', ':'))
    print('state', path)


def finish(output, aliases=()):
    """Checksum line, and with --release the parts under GitHub's 2 GiB limit.
    aliases: other channel names to give the same parts (--thor-bridge, --rp6-bridge)."""
    h = sha256(output)
    output.with_name(output.name + '.sha256').write_text(h + '  ' + output.name + '\n')
    print(output, h)
    if not a.release: return
    n = 0; left = output.stat().st_size
    with output.open('rb') as f:
        while left:
            n += 1; size = min(left, 1900 << 20); left -= size
            with output.with_name(f'{output.name}.{n:03d}').open('wb') as part:
                while size:
                    b = f.read(min(size, 4 << 20)); part.write(b); size -= len(b)
    output.unlink()
    print(f'{n} parts; add {output.name}.sha256 to the release SHA256SUMS')
    prefix = f'pb-os-{version}-{image}.'
    assert output.name.startswith(prefix), output.name
    for alias in aliases:
        other = f'pb-os-{version}-{alias}.' + output.name[len(prefix):]
        for i in range(1, n + 1):
            dst = output.with_name(f'{other}.{i:03d}'); dst.unlink(missing_ok=True)
            os.link(output.with_name(f'{output.name}.{i:03d}'), dst)
        output.with_name(other + '.sha256').write_text(h + '  ' + other + '\n')
        print(f'{n} parts as {other} too; add {other}.sha256 to the release SHA256SUMS')


def pack(stage, output, manifest, members, aliases=()):
    (stage / 'manifest.json').write_text(json.dumps(manifest, separators=(',', ':')))  # updaters refuse over 32 MiB
    gz = ['--use-compress-program=pigz'] if shutil.which('pigz') else ['-z']
    subprocess.run(['tar', '--xattrs', '--acls', '--numeric-owner', *gz, '-cf', str(output) + '.part',
                    '-C', str(stage), 'manifest.json', *members], check=True)
    os.replace(str(output) + '.part', output)
    finish(output, aliases)


if a.state_only:
    keep = set(root_trim.kept(root))
    write_state(Path(a.output).resolve() if a.output else out_dir / f'pb-os-{version}-{image}.state.json.gz',
                {rel: e for rel, e in tree_state(root).items() if not root_trim.left_out(rel, keep)},
                tree_digest(root / ANDROID), decky_state(root))
    raise SystemExit(0)

output = (Path(a.output) if a.output else out_dir / f'pb-os-{version}-{image}.update.tar.gz').resolve()
if not (root / 'usr/lib/liblsfg-vk-arm64.so').is_file(): raise SystemExit('missing lsfg-vk 1.x ARM layer')
# Decky plugins: the image's bundle (install-system-fixes.sh) plus decky-lsfg-vk,
# which updaters from before 2026-10 require, and SteamGridDB (apply-overlays.sh,
# the Decky store's newest at build time), the Thor's hero art source. Device
# plugins a package does not carry are removed, so a plugin left from another
# image goes too.
bundle = root / 'usr/share/steamos-odin/decky-plugins'
bundled = [p.name for p in bundle.iterdir() if p.is_dir()] if bundle.is_dir() else []
PLUGINS = sorted({'decky-lsfg-vk', 'decky-steamgriddb', *bundled})
# pbos-utils and dual-screen: the PB-OS tab in Quick Access (pbos-qamd) now.
REMOVE = sorted({'pbos-control', 'pbos-utils', 'dual-screen', 'thor-stick-calibration', 'stick-calibration', 'thor-screens', 'pbos-update'} - set(PLUGINS))
def load_state(f):
    b = json.load(gzip.open(f, 'rt'))
    if b.get('image') not in (image, *OLD_IMAGES): raise SystemExit(f'{f}: state of {b.get("image")}, building {image}')
    if b.get('version') == version: raise SystemExit(f'{f}: state is of this version ({version})')
    return b
bases = [load_state(f) for f in a.from_state]
def feature_line(base_state, patch_states, option):
    """States of a feature release and its patches, for one cumulative delta."""
    feature = load_state(base_state)
    patches = [load_state(f) for f in patch_states]
    for b in patches:
        # States from before bases were recorded: trust the release manager.
        if b.get('base', feature['version']) != feature['version']:
            raise SystemExit(f'{b["version"]} patches {b.get("base")}, not {feature["version"]}')
    names = [feature['version']] + [b['version'] for b in patches]
    if len(set(names)) != len(names): raise SystemExit(f'the same version twice: {names}')
    if feature['version'] in [b['version'] for b in bases]:
        raise SystemExit(f'--from-state {feature["version"]} would be built twice (it is the {option})')
    return [feature] + patches
# Patch release: one delta from the feature release that also covers its patches.
cumulative = None
if a.base_state:
    cumulative = feature_line(a.base_state, a.patch_state, '--base-state')
    if cumulative[0]['version'] != release_base:
        raise SystemExit(f'--base-state is {cumulative[0]["version"]}, the rootfs says its base is {release_base} (PB_OS_BASE)')
elif release_base != version:
    raise SystemExit(f'the rootfs is a patch of {release_base}: give --base-state (and --patch-state)')
# Feature release: also one delta from the previous feature release and its patches.
elif a.prev_base_state:
    cumulative = feature_line(a.prev_base_state, a.prev_patch_state, '--prev-base-state')
with tempfile.TemporaryDirectory(prefix='pbos-package-', dir=output.parent) as temp:
    stage = Path(temp) / 'full'
    def copy(src, dst):
        dst.mkdir(parents=True, exist_ok=True)
        # The completed build tree must remain unchanged throughout packaging.
        # Same-filesystem hard links retain exact metadata without another full copy.
        if src.stat().st_dev == dst.stat().st_dev:
            subprocess.run(['cp', '-a', '--link', str(src) + '/.', str(dst) + '/'], check=True)
        else:
            subprocess.run(['rsync', '-aHAX', '--numeric-ids', str(src) + '/', str(dst) + '/'], check=True)
    for rel in MANAGED:
        src = root / rel
        if src.exists(): copy(src, stage / 'root' / rel)
    # Same paths the image leaves out. Unlinking hard links leaves the build tree alone.
    root_trim.apply(stage / 'root')
    for rel, mode in SUID.items():
        p = stage / 'root' / rel
        if p.is_symlink() or not p.is_file(): continue
        # A new inode: chmod on the hard link would change the build tree.
        p.unlink(); shutil.copy2(root / rel, p)
        os.chown(p, 0, suid_gid(root, rel)); os.chmod(p, mode)
    # Each installation keeps its own identity; the build's is not shipped.
    for rel in ('etc/machine-id', 'var/lib/overlays/etc/upper/machine-id'):
        (stage / 'root' / rel).unlink(missing_ok=True)
    for name in PLUGINS:
        src = root / 'home/steamos/homebrew/plugins' / name
        if not src.is_dir(): raise SystemExit(f'missing Decky plugin in rootfs home: {name}')
        copy(src, stage / 'home/steamos/homebrew/plugins' / name)
    if (root / ANDROID).is_dir(): copy(root / ANDROID, stage / ANDROID)
    decky = decky_state(root)
    if decky is None: raise SystemExit(f'missing Decky Loader in rootfs: {DECKY}')
    (stage / DECKY).mkdir(parents=True)
    for n in DECKY_FILES: subprocess.run(['cp', '-a', str(root / DECKY / n), str(stage / DECKY / n)], check=True)
    if a.soc == 'sm8650':
        # Updaters from before the PB-OS Control rename refuse a package without
        # konkr-control; an empty one empties the old plugin, pbosd removes it.
        (stage / 'home/steamos/homebrew/plugins/konkr-control').mkdir()
    (stage / 'boot').mkdir()
    subprocess.run(['cp', a.kernel, str(stage / 'boot/KERNEL')], check=True)
    files = {}
    for p in sorted(stage.rglob('*')):
        if not p.is_file() or p.is_symlink(): continue
        files[str(p.relative_to(stage))] = sha256(p)
    common = {'architecture': 'aarch64', 'devices': DEVICES, 'version': version, 'remove_plugins': REMOVE}
    new = tree_state(stage / 'root', files)
    android = tree_digest(stage / ANDROID)
    write_state(out_dir / f'pb-os-{version}-{image}.state.json.gz', new, android, decky)
    # A patch release ships only its delta (feature releases bring the full system).
    if not a.base_state:
        pack(stage, output, {'format': 1, **common, 'decky_loader': decky['version'], 'files': files}, ['root', 'home', 'boot'],
             ('thor',) * a.thor_bridge + ('rp6',) * a.rp6_bridge)

    def make_delta(olds):
        """One delta that installs on each of olds (states): every entry that
        differs from any of them, and deletes what any of them has that this
        release does not."""
        changed = sorted(rel for rel, e in new.items() if any(o['entries'].get(rel) != e for o in olds))
        # Updaters through 0.5.4.7 deleted the lsfg-vk layer manifest on every delta
        # that did not carry it: deltas always bring it back.
        if LSFG_LAYER in new and LSFG_LAYER not in changed: changed = sorted(changed + [LSFG_LAYER])
        # Gone, or a different kind of entry now (a file that became a directory):
        # the device removes the old one before the new one is copied in.
        deleted = sorted({rel for o in olds for rel, e in o['entries'].items()
                          if rel not in new or e[0] != new[rel][0]},
                         key=lambda r: (-r.count('/'), r))
        # The kernel goes along when it changed, and always with changed modules.
        kernel = (any(sha256(a.kernel) != o.get('kernel') for o in olds)
                  or any(r.startswith('usr/lib/modules/') for r in changed + deleted))
        base = olds[0]
        delta = Path(temp) / f'delta-{base["version"]}'
        def place(rel):
            """rel from the full stage into the delta, parents with their metadata."""
            for part in [*reversed(PurePosixPath(rel).parents)][1:] + [PurePosixPath(rel)]:
                src, dst = stage / 'root' / part, delta / 'root' / part
                if dst.exists() or dst.is_symlink(): continue
                st = src.lstat()
                if stat.S_ISDIR(st.st_mode):
                    dst.mkdir(parents=True)
                    os.chown(dst, st.st_uid, st.st_gid); os.chmod(dst, st.st_mode & 0o7777)
                elif stat.S_ISLNK(st.st_mode):
                    os.symlink(os.readlink(src), dst); os.chown(dst, st.st_uid, st.st_gid, follow_symlinks=False)
                else:
                    os.link(src, dst)  # same inode: data, owner, mode and xattrs as staged
        for rel in changed: place(rel)
        for name in PLUGINS:
            copy(stage / 'home/steamos/homebrew/plugins' / name, delta / 'home/steamos/homebrew/plugins' / name)
        # States from before it moved to home have no checksum: it goes along.
        if android and any(o.get('android') != android for o in olds): copy(stage / ANDROID, delta / ANDROID)
        with_decky = any(o.get('decky') != decky for o in olds)
        if with_decky:
            (delta / DECKY).mkdir(parents=True)
            for n in DECKY_FILES: os.link(stage / DECKY / n, delta / DECKY / n)
        members = ['home']
        if (delta / 'root').exists(): members.insert(0, 'root')
        if kernel:
            (delta / 'boot').mkdir(); os.link(stage / 'boot/KERNEL', delta / 'boot/KERNEL'); members.append('boot')
        dfiles = {str(p.relative_to(delta)): files[str(p.relative_to(delta))]
                  for p in sorted(delta.rglob('*')) if p.is_file() and not p.is_symlink()}
        name = f'pb-os-{version}-{image}.from-{base["version"]}.delta.tar.gz'
        also = [o['version'] for o in olds[1:]]
        print(f'delta from {base["version"]}{" (also " + ", ".join(also) + ")" if also else ""}: '
              f'{len(changed)} changed, {len(deleted)} deleted, kernel {"yes" if kernel else "no"}, '
              f'Decky Loader {decky["version"] if with_decky else "no"}')
        # also_from: the patches it also installs on (updaters without it check `from` only).
        pack(delta, out_dir / name, {'format': 2, 'kind': 'delta', 'from': base['version'],
                                    **({'also_from': also} if also else {}), **common,
                                    **({'decky_loader': decky['version']} if with_decky else {}),
                                    'delete': deleted, 'files': dfiles}, members, ('rp6',) * a.rp6_bridge)

    for base in bases: make_delta([base])
    if cumulative: make_delta(cumulative)
