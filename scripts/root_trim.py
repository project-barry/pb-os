#!/usr/bin/env python3
"""System paths that images and update packages leave out, to keep room on root.

The build rootfs keeps them: the in-rootfs builds (gamescope, mangoapp, box64,
...) need the headers. Only what goes to devices is trimmed:

  root_trim.py filter ROOT   rsync filter rules for the image copy
  root_trim.py size ROOT     MiB the trim leaves out (for root auto-sizing)
  root_trim.py apply ROOT    delete the trimmed paths from a staged copy

A full update deletes installed paths its package no longer has, so devices
lose these on their next full update too.
"""
import os
from pathlib import Path
import shutil
import sys

TRIM = (
    'opt/steamvr',    # Frame SteamVR (~1.3 GiB): its services are masked, odin-bin/steamvr is a stub
    'usr/include',    # headers: nothing builds on a device
    'usr/share/doc',
)


def kept(root):
    """Trimmed files that /usr/share/licenses links to (xz, systemd): they stay."""
    keep = set()
    licenses = root / 'usr/share/licenses'
    for base, dirs, names in os.walk(licenses):
        for name in dirs + names:
            p = Path(base) / name
            if not p.is_symlink(): continue
            target = os.readlink(p)
            target = Path(target.lstrip('/')) if target.startswith('/') \
                else Path(os.path.normpath(p.parent.relative_to(root) / target))
            if any(str(target).startswith(t + '/') for t in TRIM): keep.add(str(target))
    return sorted(keep)


def rsync_filter(root):
    rules = []
    for rel in kept(root):
        parts = rel.split('/')
        rules += [f'+ /{"/".join(parts[:i])}/' for i in range(1, len(parts))] + [f'+ /{rel}']
    # Contents only: the directories themselves stay, empty.
    return list(dict.fromkeys(rules)) + [f'- /{t}/**' for t in TRIM]


def size_mib(root):
    keep = {root / k for k in kept(root)}
    total = 0
    for t in TRIM:
        for base, dirs, names in os.walk(root / t):
            for name in names:
                p = Path(base) / name
                if p not in keep and not p.is_symlink(): total += p.lstat().st_blocks * 512
    return total >> 20


def apply(root):
    keep = set(kept(root))
    for t in TRIM:
        if (root / t).is_dir() and not (root / t).is_symlink(): _prune(root, root / t, keep)


def _prune(root, d, keep):
    """Empty d except the kept files and their parent directories."""
    for p in sorted(d.iterdir()):
        rel = str(p.relative_to(root))
        if rel in keep: continue
        if any(k.startswith(rel + '/') for k in keep): _prune(root, p, keep)
        elif p.is_dir() and not p.is_symlink(): shutil.rmtree(p)
        else: p.unlink()


if __name__ == '__main__':
    if len(sys.argv) != 3 or sys.argv[1] not in ('filter', 'size', 'apply'):
        raise SystemExit(__doc__)
    root = Path(sys.argv[2])
    if sys.argv[1] == 'filter': print('\n'.join(rsync_filter(root)))
    elif sys.argv[1] == 'size': print(size_mib(root))
    else: apply(root)
