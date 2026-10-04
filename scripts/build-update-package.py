#!/usr/bin/env python3
"""Build a verified offline update bundle from a completed rootfs and boot image.

The rootfs is the one make-steamos-sm8650.sh packed into the image, with the
same SOC and --device. Devices install it with the SteamOS Update app.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ap = argparse.ArgumentParser()
ap.add_argument('--rootfs', required=True)
ap.add_argument('--kernel', required=True)
ap.add_argument('--version', required=True)
ap.add_argument('--output', required=True)
ap.add_argument('--soc', choices=('sm8650', 'sm8550'), default='sm8650')
ap.add_argument('--device', choices=('thor',), help='image built with --device thor')
a = ap.parse_args()
if a.device == 'thor' and a.soc != 'sm8550': raise SystemExit('--device thor needs --soc sm8550')
# Device models (DTB `model`) each package may install on. A Thor image differs
# from the plain SM8550 one, so each gets its own package.
DEVICES = (['AYN Thor'] if a.device == 'thor' else
           {'sm8650': ['KONKR Pocket FIT', 'AYANEO Pocket S2'],
            'sm8550': ['Retroid Pocket 6', 'Retroid Pocket 6 TOP-DPAD']}[a.soc])
root = Path(a.rootfs).resolve(); output = Path(a.output).resolve()
if not (root / 'usr/lib/liblsfg-vk-layer-arm64.so').is_file(): raise SystemExit('missing LSFG v2 ARM layer')
# Decky plugins: the image's bundle (install-system-fixes.sh) plus decky-lsfg-vk,
# which updaters from before 2026-10 require. Device plugins a package does not
# carry are removed, so a plugin left from another image goes too.
bundle = root / 'usr/share/steamos-odin/decky-plugins'
bundled = [p.name for p in bundle.iterdir() if p.is_dir()] if bundle.is_dir() else []
PLUGINS = sorted({'decky-lsfg-vk', *bundled})
REMOVE = sorted({'pbos-control', 'dual-screen', 'thor-screens'} - set(PLUGINS))
# setuid root that make-steamos-sm8650.sh (restore_image_suid) sets on the image copy only.
SUID = {p: 0o4755 for p in ('usr/bin/pkexec', 'usr/sbin/pkexec', 'usr/bin/sudo', 'usr/sbin/sudo',
        'usr/lib/polkit-1/polkit-agent-helper-1', 'usr/bin/su', 'usr/bin/passwd', 'usr/bin/newgrp',
        'usr/bin/chsh', 'usr/bin/chfn', 'usr/bin/gpasswd', 'usr/bin/unix_chkpwd', 'usr/bin/mount',
        'usr/bin/umount')}
SUID['usr/lib/dbus-1.0/dbus-daemon-launch-helper'] = 0o4750
with tempfile.TemporaryDirectory(prefix='konkr-package-', dir=output.parent) as temp:
    stage = Path(temp)
    def copy(src, dst):
        dst.mkdir(parents=True, exist_ok=True)
        # The completed build tree must remain unchanged throughout packaging.
        # Same-filesystem hard links retain exact metadata without another full copy.
        if src.stat().st_dev == dst.stat().st_dev:
            subprocess.run(['cp', '-a', '--link', str(src) + '/.', str(dst) + '/'], check=True)
        else:
            subprocess.run(['rsync', '-aHAX', '--numeric-ids', str(src) + '/', str(dst) + '/'], check=True)
    for rel in ('usr', 'opt', 'etc', 'var/lib/overlays/etc/upper'):
        src = root / rel
        if src.exists(): copy(src, stage / 'root' / rel)
    for rel, mode in SUID.items():
        p = stage / 'root' / rel
        if p.is_symlink() or not p.is_file(): continue
        # A new inode: chmod on the hard link would change the build tree.
        p.unlink(); shutil.copy2(root / rel, p)
        os.chown(p, 0, 0); os.chmod(p, mode)
    # Each installation keeps its own identity; the build's is not shipped.
    for rel in ('etc/machine-id', 'var/lib/overlays/etc/upper/machine-id'):
        (stage / 'root' / rel).unlink(missing_ok=True)
    for name in PLUGINS:
        src = root / 'home/steamos/homebrew/plugins' / name
        if not src.is_dir(): raise SystemExit(f'missing Decky plugin in rootfs home: {name}')
        copy(src, stage / 'home/steamos/homebrew/plugins' / name)
    if a.soc == 'sm8650':
        # Updaters from before the PB-OS Control rename refuse a package without
        # konkr-control; an empty one empties the old plugin, pbosd removes it.
        (stage / 'home/steamos/homebrew/plugins/konkr-control').mkdir()
    (stage / 'boot').mkdir()
    subprocess.run(['cp', a.kernel, str(stage / 'boot/KERNEL')], check=True)
    files = {}
    for p in sorted(stage.rglob('*')):
        if not p.is_file() or p.is_symlink(): continue
        h = hashlib.sha256()
        with p.open('rb') as f:
            for b in iter(lambda: f.read(4 << 20), b''): h.update(b)
        files[str(p.relative_to(stage))] = h.hexdigest()
    (stage / 'manifest.json').write_text(json.dumps({'format': 1, 'architecture': 'aarch64',
        'devices': DEVICES, 'version': a.version, 'remove_plugins': REMOVE, 'files': files},
        separators=(',', ':')))  # updaters refuse a manifest over 32 MiB
    gzip = ['--use-compress-program=pigz'] if shutil.which('pigz') else ['-z']
    subprocess.run(['tar', '--xattrs', '--acls', '--numeric-owner', *gzip, '-cf', str(output) + '.part',
                    '-C', str(stage), 'manifest.json', 'root', 'home', 'boot'], check=True)
    os.replace(str(output) + '.part', output)
h = hashlib.sha256()
with output.open('rb') as f:
    for b in iter(lambda: f.read(4 << 20), b''): h.update(b)
output.with_name(output.name + '.sha256').write_text(h.hexdigest() + '  ' + output.name + '\n')
print(output, h.hexdigest())
