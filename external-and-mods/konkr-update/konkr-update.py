#!/usr/bin/env python3
"""Download, stage and recover offline SteamOS updates. Runs recovery from a private HOME root.
No partitioning or formatting commands are used. Games and Steam account data are excluded.
"""
from __future__ import annotations
import argparse
import fcntl
import hashlib
import json
import os
import re
import shutil
import stat
import subprocess
import threading
import sys
import tarfile
import time
import uuid
from pathlib import Path, PurePosixPath

# Format 1: a full system. Format 2: a delta, only what changed since the
# version in `from`; updaters from before deltas refuse format 2. A patch
# release's delta also lists in `also_from` the patches of the same feature
# release it installs on (see build-update-package.py).
FULL, DELTA = 1, 2
ROOT_DIRS = ('usr', 'opt', 'etc')
UPPER = 'var/lib/overlays/etc/upper'
PLUGINS = 'homebrew/plugins'
# Plugin folders from older images: removed on apply, restored on rollback.
# SM8650 packages still carry an empty konkr-control folder, which updaters
# from before the PB-OS Control rename require. PB-OS Update became PB-OS
# Utils.
LEGACY_PLUGINS = ('konkr-control', 'pbos-update')
PRESERVE = ('passwd', 'shadow', 'group', 'gshadow', 'machine-id', 'hostname', 'hosts',
            'fstab', 'crypttab', 'localtime', 'adjtime', 'resolv.conf', 'ssh',
            'NetworkManager/system-connections', 'sudoers.d')
PENDING = 'var/lib/konkr-update/pending'
# One folder per update, named by its id. A finished one keeps only these.
STORAGE = Path('/home/.konkr-updates')
TRANSACTION_RECORDS = ('transaction.json', 'state.json', 'recovery.log', 'failure.txt')
# Per-install record written by the image build; not part of any package.
OPT_KEEP = ('steamos-sm8650/IMAGE.txt',)
UPDATER = 'usr/share/konkr-update/konkr-update.py'
# Written by make-steamos-sm8650.sh: the release tag the image was built as,
# and the feature release it belongs to (patch releases' deltas are from it).
VERSION_FILE = 'usr/share/pb-os/version'
BASE_FILE = 'usr/share/pb-os/base'
# Where releases are looked for, all of them read and merged newest first:
# update packages move to pb-os-updates, which keeps the pb-os release page
# to the images people flash; earlier releases stay on pb-os.
RELEASES = ('https://api.github.com/repos/project-barry/pb-os-updates/releases',
            'https://api.github.com/repos/project-barry/pb-os/releases')
# Release assets of each device's packages, split into .001, .002, ... parts
# under GitHub's 2 GiB limit: pb-os-<tag>-<image>.update.tar.gz (full) and
# pb-os-<tag>-<image>.from-<installed tag>.delta.tar.gz (delta).
# Downloads install only when the release's SHA256SUMS carries a signature
# (SHA256SUMS.sig, scripts/sign-release.sh) from a key in this file.
SIGNERS = Path('/usr/share/konkr-update/allowed_signers')
SIGNER, NAMESPACE = 'pb-os-release', 'pb-os-update'
# One image for every device: its packages are named 'pb-os'. Earlier
# releases had one image per SoC, and each device still takes its own SoC's
# earlier names (never another SoC's): 'sm8550' (named 'rp6' before alpha
# v0.5.2) for the Retroid Pocket 6, Nova and AYN Thor, 'pocketfit' for the
# SM8650 handhelds.
IMAGE = 'pb-os'
OLD_NAMES = {'KONKR Pocket FIT': ('pocketfit',), 'AYANEO Pocket S2': ('pocketfit',),
             'Retroid Pocket 6': ('sm8550', 'rp6'), 'Retroid Pocket 6 TOP-DPAD': ('sm8550', 'rp6'),
             'Retroid Pocket Nova': ('sm8550', 'rp6'), 'AYN Thor': ('sm8550', 'rp6')}


def channel_names(model):
    """The package names a device takes, the current one first."""
    if model not in OLD_NAMES: raise ValueError(f'no update channel for {model}')
    return (IMAGE, *OLD_NAMES[model])


def run(*args, **kwargs):
    print('+', ' '.join(map(str, args)), flush=True)
    return subprocess.run(list(map(str, args)), check=True, **kwargs)


def digest(p):
    h = hashlib.sha256()
    with open(p, 'rb') as f:
        for b in iter(lambda: f.read(4 << 20), b''): h.update(b)
    return h.hexdigest()


def write_json(p, data):
    p = Path(p); p.parent.mkdir(parents=True, exist_ok=True)
    tmp = p.with_name(p.name + '.tmp')
    with tmp.open('w') as f:
        json.dump(data, f, indent=2); f.flush(); os.fsync(f.fileno())
    os.replace(tmp, p)
    fd = os.open(p.parent, os.O_RDONLY | os.O_DIRECTORY)
    try: os.fsync(fd)
    finally: os.close(fd)


def state(work, value):
    write_json(work / 'state.json', {'state': value, 'time': time.time()})
    print('STATE:', value, flush=True)


def copy_tree(src, dst, delete=False, excludes=()):
    dst.mkdir(parents=True, exist_ok=True)
    args = ['rsync', '-aHAX', '--checksum', '--numeric-ids']
    if delete: args.append('--delete')
    args += [f'--exclude=/{x}' for x in excludes]
    run(*args, str(src) + '/', str(dst) + '/')


def is_delta(manifest):
    return manifest.get('format') == DELTA


def preserved(rel):
    """rel (root-relative) is device identity the updater never replaces."""
    for prefix in ('etc/', UPPER + '/'):
        if rel.startswith(prefix):
            rest = rel[len(prefix):]
            return any(rest == p or rest.startswith(p + '/') for p in PRESERVE)
    return False


def mount_info(path):
    r = run('findmnt', '-J', '-o', 'SOURCE,FSTYPE,TARGET,UUID', '-T', path,
            capture_output=True, text=True)
    return json.loads(r.stdout)['filesystems'][0]


def validate_archive(package):
    seen = set(); links = set(); directories = set(); total = 0; manifest = None; actual = set()
    with tarfile.open(package, 'r:gz') as tf:
        for m in tf:
            if (m.isfile() or m.islnk()) and m.name.removeprefix('./') != 'manifest.json':
                actual.add(m.name.removeprefix('./'))
            name = m.name.removeprefix('./').rstrip('/')
            p = PurePosixPath(name)
            if not name or p.is_absolute() or '..' in p.parts or name in seen:
                raise ValueError(f'unsafe or duplicate archive path: {name}')
            if any(str(a) in links for a in p.parents):
                raise ValueError(f'archive writes through symlink: {name}')
            seen.add(name)
            if not (m.isfile() or m.isdir() or m.issym() or m.islnk()):
                raise ValueError(f'special file in package: {name}')
            if m.isdir(): directories.add(name)
            if m.issym(): links.add(name)
            elif m.islnk():
                q = PurePosixPath(m.linkname.removeprefix('./'))
                if q.is_absolute() or '..' in q.parts or str(q) not in seen:
                    raise ValueError(f'unsafe hard link: {name}')
            if m.isfile(): total += m.size
            if name == 'manifest.json':
                if not m.isfile() or m.size > 32 << 20: raise ValueError('invalid manifest')
                manifest = json.load(tf.extractfile(m))
            elif not (name == 'root' or name.startswith(('root/usr/', 'root/opt/', 'root/etc/',
                     'root/var/lib/overlays/etc/upper/', 'home/steamos/', 'boot/')) or
                     name in ('root/usr', 'root/opt', 'root/etc', 'root/var', 'root/var/lib',
                     'root/var/lib/overlays', 'root/var/lib/overlays/etc',
                     'root/var/lib/overlays/etc/upper', 'home', 'home/steamos', 'boot')):
                raise ValueError(f'unsupported payload path: {name}')
    if not manifest or manifest.get('format') not in (FULL, DELTA) or manifest.get('architecture') != 'aarch64':
        raise ValueError('unsupported update format or architecture')
    if is_delta(manifest):
        if not isinstance(manifest.get('from'), str) or not manifest['from']: raise ValueError('delta without a base version')
        also = manifest.get('also_from', [])
        if not isinstance(also, list) or not all(isinstance(v, str) and v for v in also):
            raise ValueError('invalid also_from list')
        deleted = manifest.get('delete', [])
        if not isinstance(deleted, list): raise ValueError('invalid delete list')
        for rel in deleted:
            q = PurePosixPath(rel) if isinstance(rel, str) else None
            if (q is None or q.is_absolute() or '..' in q.parts or str(q) != rel
                    or not rel.startswith(('usr/', 'opt/', 'etc/', UPPER + '/'))):
                raise ValueError(f'unsafe delete path: {rel}')
    elif not {'root/usr', 'root/opt', 'root/etc', 'boot'}.issubset(directories):
        raise ValueError('required payload directories must be real directories')
    devices = manifest.get('devices')
    if not isinstance(devices, list) or not devices or not all(isinstance(d, str) and d for d in devices):
        raise ValueError('unsupported device list')
    removed = manifest.get('remove_plugins', [])
    if not isinstance(removed, list) or not all(isinstance(n, str) and re.fullmatch('[A-Za-z0-9_-][A-Za-z0-9._-]*', n) for n in removed):
        raise ValueError('invalid plugin removal list')
    files = manifest.get('files', {})
    if not isinstance(files, dict): raise ValueError('invalid file manifest')
    # Every regular file must be covered, and file keys cannot escape extraction.
    if actual != set(files): raise ValueError('manifest does not cover all payload files')
    for name, sha in files.items():
        if not re.fullmatch('[0-9a-f]{64}', sha): raise ValueError(f'invalid checksum: {name}')
    return manifest, total


def verify_payload(payload, manifest):
    for name, sha in manifest['files'].items():
        p = payload / name
        if p.is_symlink() or not p.is_file() or digest(p) != sha:
            raise ValueError(f'payload checksum mismatch: {name}')
    if is_delta(manifest): return  # only what changed; the kernel only when it did
    for d in ROOT_DIRS:
        if (payload / 'root' / d).is_symlink() or not (payload / 'root' / d).is_dir(): raise ValueError(f'missing system directory: {d}')
    if not (payload / 'boot/KERNEL').is_file(): raise ValueError('missing KERNEL')


BOOTIMG = Path('/usr/share/easy-ufs-install/ufs-bootimg.py')


def bootimg_helper(payload=None):
    """v1.1 has no ufs-bootimg.py; the update payload carries the new one."""
    if BOOTIMG.is_file(): return BOOTIMG
    if payload is not None:
        bundled = payload / 'root' / str(BOOTIMG).lstrip('/')
        if bundled.is_file(): return bundled
    raise ValueError('boot image helper not found (ufs-bootimg.py)')


def recovery_runtime(dst, helper=BOOTIMG, updater=None):
    """Copy executables, Python stdlib, and all their resolved shared libraries."""
    dst.mkdir(parents=True, exist_ok=True)
    sources = [Path(shutil.which('python3')).resolve(), Path(shutil.which('rsync')).resolve(), Path(shutil.which('chown')).resolve(), Path(shutil.which('findmnt')).resolve()]
    lib = Path(sys.base_prefix) / 'lib' / f'python{sys.version_info.major}.{sys.version_info.minor}'
    copy_tree(lib, dst / str(lib).lstrip('/'), excludes=('site-packages', 'dist-packages'))
    sources += list((lib / 'lib-dynload').rglob('*.so'))
    deps = set()
    for src in sources:
        out = subprocess.run(['ldd', str(src)], text=True, capture_output=True)
        for line in out.stdout.splitlines():
            for token in line.split():
                if token.startswith('/') and Path(token).is_file(): deps.add(Path(token))
    for src in set(sources[:4]) | deps:
        dest = dst / str(src).lstrip('/'); dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src.resolve(), dest)
    py = dst / 'usr/bin/python3'; py.parent.mkdir(parents=True, exist_ok=True)
    if not py.exists(): py.symlink_to(str(sources[0]))
    rs = dst / 'usr/bin/rsync'
    if not rs.exists(): rs.symlink_to(str(sources[1]))
    for d in ('target', 'boot-target', 'home-target', 'transaction', 'dev', 'proc', 'tmp'):
        (dst / d).mkdir(parents=True, exist_ok=True)
    shutil.copy2(updater or __file__, dst / 'updater.py')
    shutil.copy2(helper, dst / 'bootimg.py')
    # Include NSS configuration for numeric lookup fallbacks; no credentials copied.
    (dst / 'etc').mkdir(exist_ok=True)
    (dst / 'etc/nsswitch.conf').write_text('passwd: files\ngroup: files\n')
    (dst / 'etc/passwd').write_text('root:x:0:0:root:/root:/bin/sh\n')
    (dst / 'etc/group').write_text('root:x:0:\n')


def retarget_kernel(src, dst, rootarg, helper=BOOTIMG):
    import importlib.util
    spec = importlib.util.spec_from_file_location('bootimg', helper)
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
    image = module.BootImg(src.read_bytes())
    if image.id != image.expected_id(): raise ValueError('KERNEL header checksum mismatch')
    raw = module.ramdisk_bytes(image.ramdisk)
    if b'konkr-update-recover' not in raw: raise ValueError('KERNEL has no update recovery hook')
    dst.write_bytes(image.build(module.retarget(image.cmdline, rootarg)))


def device_model():
    return Path('/sys/firmware/devicetree/base/model').read_text().rstrip('\0\n')


def installed_version():
    p = Path('/') / VERSION_FILE
    return p.read_text().strip() if p.is_file() else ''


def installed_base():
    """The feature release this system belongs to: the delta to look for. Images
    from before feature releases have none, so they are their own."""
    p = Path('/') / BASE_FILE
    return (p.read_text().strip() if p.is_file() else '') or installed_version()


def delta_bases(manifest):
    """The versions a delta installs on."""
    return [manifest['from'], *manifest.get('also_from', [])]


def newer(tag, current):
    """tag is an update for current: a later release. Development builds
    (dev-<date>) take any release."""
    if not current or current.startswith('dev-'): return tag != current
    return version_key(tag) > version_key(current)


def storage():
    """Root-only update storage on HOME."""
    base = STORAGE
    if base.is_symlink(): raise ValueError('invalid update storage directory')
    base.mkdir(mode=0o700, exist_ok=True)
    if base.stat().st_uid != 0: raise ValueError('update storage is not owned by root')
    os.chmod(base, 0o700)
    return base


def size_of(p):
    if p.is_symlink() or not p.is_dir():
        try: return p.lstat().st_size
        except OSError: return 0
    return sum(size_of(c) for c in p.iterdir())


def remove(p):
    if p.is_dir() and not p.is_symlink(): shutil.rmtree(p)
    else: p.unlink()


def cleanup():
    """Free what finished updates leave on HOME, from every updater version
    (the layout is the same since the first): a committed, rolled-back or
    aborted update keeps only its records and logs; the folder of a staging
    that never finished (power lost, killed) goes. A pending update, or one
    stopped halfway without its marker (to be investigated), is not touched.
    Downloads of versions older than the installed one go. Call with the lock
    held."""
    base = STORAGE
    if not base.is_dir() or base.is_symlink() or base.stat().st_uid != 0: return 0
    # Never delete into a mount (the restart step binds the system under recovery/).
    for line in Path('/proc/self/mountinfo').read_text().splitlines():
        point = line.split()[4].replace('\\040', ' ')
        if point == str(base) or point.startswith(str(base) + '/'):
            print('cleanup: something is mounted under', base, '- skipping', flush=True); return 0
    pending = Path('/') / PENDING
    busy = pending.read_text().strip() if pending.exists() else ''
    current = installed_version(); freed = 0
    for d in sorted(base.iterdir()):
        if d.name == 'downloads' and d.is_dir() and not d.is_symlink():
            for f in d.iterdir():
                m = re.fullmatch(r'pb-os-(.+?)-[a-z0-9]+\.(?:from-.+\.delta|update)\.tar\.gz', f.name)
                # Older than the installed version (a reinstall's download may resume).
                if m and current and not current.startswith('dev-') and newer(current, m.group(1)):
                    freed += size_of(f); remove(f)
            continue
        if d.name == busy or d.is_symlink() or not d.is_dir(): continue
        if not re.fullmatch('[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}', d.name): continue
        try: done = json.loads((d / 'state.json').read_text())['state']
        except (OSError, ValueError, KeyError, TypeError): done = None
        if done in ('committed', 'rolled-back', 'aborted'):
            for c in d.iterdir():
                if c.name not in TRANSACTION_RECORDS:
                    freed += size_of(c); remove(c)
        elif done in (None, 'staged'):
            # Staging writes the pending marker before the boot image: without
            # it, nothing of this folder reached the system.
            freed += size_of(d); shutil.rmtree(d)
    if freed: print(f'cleanup: freed {freed >> 20} MiB of finished updates', flush=True)
    os.sync()
    return freed


def stage(source_package, expected, move=False, version=None):
    """Stage a package. move: it already sits in root-only storage, so take it instead of copying."""
    if os.geteuid() != 0: raise ValueError('staging needs administrator access')
    model = device_model()
    if os.uname().machine != 'aarch64': raise ValueError('requires ARM64 SteamOS')
    import pwd
    if pwd.getpwnam('steamos').pw_uid != 1000: raise ValueError('unsupported SteamOS account layout')
    source_package = Path(source_package).resolve()
    expected = expected.lower()
    if not re.fullmatch('[0-9a-f]{64}', expected): raise ValueError('invalid expected SHA256')
    root_info, home_info, boot_info = [mount_info(x) for x in ('/', '/home', '/boot')]
    if root_info['fstype'] != 'ext4' or home_info['fstype'] != 'ext4' or boot_info['fstype'] != 'vfat':
        raise ValueError('requires separate ext4 root/home and FAT boot filesystems')
    if home_info['source'] == root_info['source']: raise ValueError('HOME must be a separate filesystem')
    pending = Path('/') / PENDING
    if pending.exists(): raise ValueError('an update is already pending; finish or recover it first')
    cleanup()
    base = storage()
    work = base / str(uuid.uuid4()); work.mkdir(mode=0o700)
    package = work / 'package.tar.gz'; corrupt = False
    try:
        # Validate and extract only a private, root-owned copy to avoid input races.
        if move: os.replace(source_package, package)
        else: shutil.copyfile(source_package, package)
        os.chmod(package, 0o600)
        step('Checking the update')
        if digest(package) != expected:
            corrupt = True; raise ValueError('package SHA256 mismatch')
        manifest, size = validate_archive(package)
        if model not in manifest['devices']: raise ValueError(f'package is not for this device ({model})')
        if version and manifest['version'] != version:
            raise ValueError(f'package is version {manifest["version"]}, the release is {version}')
        if is_delta(manifest) and installed_version() not in delta_bases(manifest):
            raise ValueError(f'this update is for pb-os {" or ".join(delta_bases(manifest))}, this device has '
                             f'{installed_version() or "an unknown version"}; use the full package')
        payload = work / 'payload'; payload.mkdir()
        if is_delta(manifest):
            if shutil.disk_usage('/home').free < 3 * size + (512 << 20):
                raise ValueError('not enough HOME space for payload and rollback backup')
            step('Unpacking the update')
            run('tar', '--xattrs', '--acls', '--numeric-owner', '-xzf', package, '-C', payload)
            plan = plan_from_tree(Path('/'), work, manifest)
        else:
            # Only what differs from the installed system is unpacked.
            step('Comparing with the installed system')
            plan = plan_from_package(package, Path('/'), work, manifest)
        # The plan's new files, plus a backup of at most as much replaced data.
        if shutil.disk_usage('/home').free < 2 * plan['bytes'] + (512 << 20):
            raise ValueError('not enough HOME space for the update and its rollback backup')
        if shutil.disk_usage('/').free < plan['bytes'] + (512 << 20):
            raise ValueError('root partition is too small for this update')
        step('Checking the new files')
        verify_planned(work, manifest, plan, counter(plan['bytes']))
        # System files do not change while it runs; /etc is backed up at the restart.
        step('Backing up the files it replaces')
        snapshot_paths(Path('/'), work, [r for r in plan_paths(plan) if r.startswith(('usr/', 'opt/'))],
                       counter(plan['bytes']))
        print(f"Plan: {len(plan['write'])} paths to write, {len(plan['delete'])} to delete, "
              f"{plan['bytes'] >> 20} MiB", flush=True)
        info = {'id': work.name, 'version': manifest['version'], 'sha256': expected,
                'root_uuid': root_info['uuid'], 'home_uuid': home_info['uuid'],
                'boot_uuid': boot_info['uuid'], 'manifest': manifest, 'screen': screen_info()}
        write_json(work / 'transaction.json', info)
        shutil.copy2('/boot/KERNEL', work / 'previous-KERNEL')
        rootarg = next((x[5:] for x in Path('/proc/cmdline').read_text().split() if x.startswith('root=')), '')
        if not rootarg: raise ValueError('cannot identify the boot root argument')
        helper = bootimg_helper(payload)
        # A delta without a kernel keeps the running one (it carries the hook).
        kernel = payload / 'boot/KERNEL'
        retarget_kernel(kernel if kernel.is_file() else Path('/boot/KERNEL'), work / 'next-KERNEL', rootarg, helper)
        # The package's own updater applies it, so apply fixes need no reflash.
        bundled = payload / 'root' / UPDATER
        recovery_runtime(work / 'recovery', helper, bundled if bundled.is_file() else None)
        state(work, 'staged')
        pending.parent.mkdir(parents=True, exist_ok=True)
        pending.write_text(work.name + '\n'); os.chmod(pending, 0o600)
        os.sync()
        # Bootstrap recovery for v1.1 installs; the original boot image is backed up.
        install_kernel(work / 'next-KERNEL', Path('/boot'))
        print('Update staged. Reboot to apply. Backup:', work)
    except BaseException:
        if not pending.exists():
            # A good download stays for the next try.
            if move and not corrupt and package.exists(): os.replace(package, source_package)
            shutil.rmtree(work)
        raise


def step(text):
    print('STEP', text, flush=True)


def fetch(url, **headers):
    import urllib.request
    request = urllib.request.Request(url, headers={'User-Agent': 'pb-os-update', **headers})
    return urllib.request.urlopen(request, timeout=60)


def find_update(reinstall=False):
    """Newest release with a package for this device, or None: the delta from
    the installed version when the release has one, else the full package."""
    names = channel_names(device_model())
    current, base = installed_version(), installed_base()
    releases, seen, errors = [], set(), []
    for url in RELEASES:
        try:
            with fetch(url + '?per_page=30', Accept='application/vnd.github+json') as r:
                listed = json.load(r)
        except OSError as e:
            errors.append(e); continue
        releases += [rel for rel in listed if rel['tag_name'] not in seen]
        seen.update(rel['tag_name'] for rel in listed)
    if len(errors) == len(RELEASES): raise errors[0]
    releases.sort(key=lambda rel: version_key(rel['tag_name']), reverse=True)
    def parts_of(release, name):
        return sorted((a for a in release['assets'] if re.fullmatch(re.escape(name) + r'\.\d{3}', a['name'])),
                      key=lambda a: a['name'])
    for release in releases:  # newest first
        if release.get('draft'): continue
        tag = release['tag_name']
        # Only later releases: an older one with this device's package is not an update.
        if current and not (newer(tag, current) or (reinstall and tag == current)): continue
        # A patch release's delta is from the feature release (and covers its patches).
        # The image's own package name first, then its earlier names.
        candidates = [('delta', f"pb-os-{tag}-{n}.from-{base}.delta.tar.gz")
                      for n in names if base and base != tag]
        candidates += [('full', f"pb-os-{tag}-{n}.update.tar.gz") for n in names]
        kind, name, parts = None, None, []
        for kind, name in candidates:
            parts = parts_of(release, name)
            if parts: break
        sums = next((a for a in release['assets'] if a['name'] == 'SHA256SUMS'), None)
        sig = next((a for a in release['assets'] if a['name'] == 'SHA256SUMS.sig'), None)
        if parts and sums and sig:
            return {'version': tag, 'title': release.get('name') or tag, 'kind': kind,
                    'page': release['html_url'], 'name': name, 'sums': sums['browser_download_url'],
                    'sig': sig['browser_download_url'],
                    'parts': [{'url': a['browser_download_url'], 'size': a['size']} for a in parts],
                    'size': sum(a['size'] for a in parts)}
    return None


def check(args):
    current = installed_version()
    found = find_update()
    print(json.dumps({'current': current, 'base': installed_base(), 'update': found,
                      'available': bool(found) and found['version'] != current}, indent=2))


def download(update, dest):
    """Append every part to one file; a later run resumes where this one stopped."""
    total = update['size']
    have = dest.stat().st_size if dest.exists() else 0
    if have > total: dest.unlink(); have = 0
    shown = -1; offset = 0
    with dest.open('ab') as out:
        for part in update['parts']:
            end = offset + part['size']
            if have < end:
                skip = have - offset
                with fetch(part['url'], **({'Range': f'bytes={skip}-'} if skip else {})) as r:
                    if skip and r.status != 206: raise ValueError('the download server cannot resume')
                    while chunk := r.read(1 << 20):
                        out.write(chunk); have += len(chunk)
                        if have * 200 // total != shown:
                            shown = have * 200 // total; print('PROGRESS', have, total, flush=True)
                out.flush(); os.fsync(out.fileno())
                if have != end: raise ValueError('download interrupted; start it again to resume')
            offset = end


def verify_signature(data, signature):
    """Raise unless signature is a pb-os release signature over data."""
    if not SIGNERS.is_file(): raise ValueError(f'missing {SIGNERS}: cannot check release signatures')
    import tempfile
    with tempfile.NamedTemporaryFile(suffix='.sig') as f:
        f.write(signature); f.flush()
        p = subprocess.run(['ssh-keygen', '-Y', 'verify', '-f', str(SIGNERS), '-I', SIGNER, '-n', NAMESPACE,
                            '-s', f.name], input=data, capture_output=True)
    if p.returncode != 0:
        raise ValueError('the release signature does not match the pb-os release key; not installing')


def update(args):
    if os.geteuid() != 0: raise ValueError('updating needs administrator access')
    if (Path('/') / PENDING).exists(): raise ValueError('an update is already pending; restart to install it')
    cleanup()
    step('Looking for updates')
    found = find_update(args.reinstall)
    if not found: raise ValueError('the pb-os releases have no update for this device')
    if found['version'] == installed_version() and not args.reinstall:
        print('pb-os', found['version'], 'is already installed'); return
    with fetch(found['sums']) as r: sums = r.read()
    with fetch(found['sig']) as r: sig = r.read()
    verify_signature(sums, sig)
    sums = [line.split() for line in sums.decode().splitlines()]
    expected = next((f[0] for f in sums if len(f) == 2 and f[1].lstrip('*') == found['name']), '')
    if not re.fullmatch('[0-9a-fA-F]{64}', expected): raise ValueError(f'SHA256SUMS has no checksum for {found["name"]}')
    downloads = storage() / 'downloads'; downloads.mkdir(mode=0o700, exist_ok=True)
    for old in downloads.iterdir():  # another release's download
        if old.name != found['name']: old.unlink()
    package = downloads / found['name']
    have = package.stat().st_size if package.exists() else 0
    # Rough room for the download, its unpacked payload and the rollback copy (stage checks exactly).
    if found['kind'] == 'delta':
        need = found['size'] - have + found['size'] * 5 + (512 << 20)
    else:
        used = shutil.disk_usage('/').total - shutil.disk_usage('/').free
        need = found['size'] - have + found['size'] * 5 // 2 + used + (512 << 20)
    check_space(found['kind'], found['size'], have)
    step(f'Downloading pb-os {found["version"]}')
    download(found, package)
    stage(package, expected, move=True, version=found['version'])


def check_space(kind, size, have=0):
    """Rough room for the package, its unpacked payload and the rollback copy (stage checks exactly)."""
    if kind == 'delta':
        need = size - have + size * 5 + (512 << 20)
    else:
        used = shutil.disk_usage('/').total - shutil.disk_usage('/').free
        need = size - have + size * 5 // 2 + used + (512 << 20)
    if shutil.disk_usage('/home').free < need:
        raise ValueError(f'not enough free space: this update needs about {max(1, round(need / 2**30))} GB free on HOME')


# ── Updates from a drive ─────────────────────────────────────────────────────
# For a device without internet: copy a release's package (its .001, .002, ...
# parts, or the joined file), SHA256SUMS and SHA256SUMS.sig to the top folder
# of a microSD card or USB drive. Like a download, it installs only when the
# signature is from the pb-os release key. SteamOS mounts only ext4 microSD
# cards, so the updater mounts the others itself, read-only, while it looks and
# copies. Drives on the disk the system runs from are never looked at.
# The same files downloaded to the steamos user's Downloads folder (a browser
# in Desktop Mode) work too; they are removed from there once the update is
# staged.
MEDIA = Path('/run/konkr-update/media')
MEDIA_FS = {'vfat': 'vfat', 'exfat': 'exfat', 'ext4': 'ext4', 'ntfs': 'ntfs3'}
DOWNLOADS = Path('/home/steamos/Downloads')
# build-update-package.py --release: every part but the last is this size.
PART_SIZE = 1900 << 20
# What browsers write while a download runs (Firefox, Chromium, Epiphany/WebKit).
UNFINISHED = ('.part', '.crdownload', '.download')


def quiet(*args):
    return subprocess.run(list(map(str, args)), check=True, capture_output=True, text=True).stdout


def drives():
    """Filesystems on removable drives (microSD, USB) other than the system's own disk."""
    devices = json.loads(quiet('lsblk', '-J', '-l', '-b', '-o',
                               'NAME,PATH,PKNAME,TYPE,FSTYPE,LABEL,SIZE,TRAN,MOUNTPOINTS'))['blockdevices']
    by_name = {d['name']: d for d in devices}
    def disk(d):
        while d.get('pkname') in by_name: d = by_name[d['pkname']]
        return d
    system = set()
    for target in ('/', '/home', '/boot'):
        source = quiet('findmnt', '-n', '-o', 'SOURCE', '-T', target).strip()
        hit = next((d for d in devices if d['path'] == source), None)
        if hit: system.add(disk(hit)['name'])
    found = []
    for d in devices:
        top = disk(d)
        if d.get('fstype') not in MEDIA_FS or top['name'] in system: continue
        if not (top['name'].startswith('mmcblk') or top.get('tran') == 'usb'): continue
        found.append({'name': d['name'], 'path': d['path'], 'fstype': d['fstype'],
                      'label': d.get('label') or '', 'usb': top.get('tran') == 'usb',
                      'mounts': [m for m in d.get('mountpoints') or [] if m]})
    return found


def drive_title(drive):
    kind = 'USB drive' if drive['usb'] else 'microSD card'
    return f'{kind} "{drive["label"]}"' if drive['label'] else kind


class Mounted:
    """The drive's top folder: where SteamOS mounted it, else a read-only mount of our own."""
    def __init__(self, drive):
        self.drive, self.own = drive, None

    def __enter__(self):
        if self.drive['mounts']: return Path(self.drive['mounts'][0])
        if os.geteuid() != 0: raise PermissionError('mounting a drive needs administrator access')
        point = MEDIA / self.drive['name']; point.mkdir(parents=True, exist_ok=True)
        options = 'ro,nosuid,nodev,noexec' + (',noload' if self.drive['fstype'] == 'ext4' else '')
        quiet('mount', '-t', MEDIA_FS[self.drive['fstype']], '-o', options, self.drive['path'], point)
        self.own = point
        return point

    def __exit__(self, *exc):
        if self.own:
            subprocess.run(['umount', str(self.own)], capture_output=True)
            try: self.own.rmdir()
            except OSError: pass


def packages_in(folder, names, current, base):
    """pb-os packages with one of names in the folder that update current (a
    delta: from its feature release, base): complete ones as {joined name:
    {version, kind, files, size}}, and the versions of incomplete ones. Parts
    must run .001, .002, ... unbroken, all but the last PART_SIZE, and none
    may still be downloading."""
    alts = '|'.join(map(re.escape, names))
    pattern = re.compile(rf'pb-os-(.+?)-(?:{alts})\.(?:from-(.+)\.delta|update)\.tar\.gz(?:\.(\d{{3}}))?')
    groups = {}; busy = set()
    for p in folder.iterdir():
        unfinished = p.name.endswith(UNFINISHED)
        m = pattern.fullmatch(p.name.rsplit('.', 1)[0] if unfinished else p.name)
        # Symlinks are not followed: the Downloads folder belongs to the user.
        if not m or p.is_symlink() or not p.is_file(): continue
        tag, frm, part = m.group(1), m.group(2), m.group(3)
        if not newer(tag, current) or (frm is not None and frm != base): continue
        name = m.group(0)[:-4] if part else m.group(0)
        g = groups.setdefault(name, {'version': tag, 'kind': 'delta' if frm else 'full', 'joined': None, 'parts': {}})
        if unfinished: busy.add(name); continue
        if part: g['parts'][int(part)] = p
        else: g['joined'] = p
    found, incomplete = {}, set()
    for name, g in groups.items():
        parts = [g['parts'][i] for i in sorted(g['parts'])]
        sizes = [f.stat().st_size for f in parts]
        if g['joined'] and name not in busy and g['joined'].stat().st_size: files = [g['joined']]
        elif (parts and name not in busy and sorted(g['parts']) == list(range(1, len(parts) + 1))
              and all(s == PART_SIZE for s in sizes[:-1]) and 0 < sizes[-1] < PART_SIZE): files = parts
        else: incomplete.add(g['version']); continue
        found[name] = {'version': g['version'], 'kind': g['kind'], 'files': files,
                       'size': sum(f.stat().st_size for f in files)}
    return found, incomplete


def version_key(tag):
    """alpha-v0.10 after alpha-v0.9: numbers compare as numbers."""
    return [(0, int(t), '') if t.isdigit() else (1, 0, t) for t in re.findall(r'\d+|\D+', tag)]


class Downloads:
    """The steamos user's Downloads folder, when it is a real folder on HOME."""
    def __enter__(self):
        if DOWNLOADS.is_symlink() or not DOWNLOADS.is_dir(): raise FileNotFoundError(DOWNLOADS)
        return DOWNLOADS

    def __exit__(self, *exc):
        pass


def sources():
    """Where an update can be: (id, where, place, folder context) for each
    drive, then the Downloads folder."""
    for drive in drives():
        where = drive_title(drive)
        yield drive['path'], where, f'on the {where}', Mounted(drive)
    if DOWNLOADS.is_dir() and not DOWNLOADS.is_symlink():
        yield 'downloads', 'Downloads folder', 'in the Downloads folder', Downloads()


def regular(path):
    """Read a file only if it is not a symlink."""
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    try:
        if not stat.S_ISREG(os.fstat(fd).st_mode): raise ValueError(f'{path.name} is not a file')
        os.set_blocking(fd, True)
        return os.fdopen(fd, 'rb')
    except BaseException:
        os.close(fd); raise


def find_local():
    """The newest update on any drive or in the Downloads folder, or None,
    and what is wrong with the packages that cannot be used."""
    names = channel_names(device_model())
    current = installed_version(); problems = []; best = None
    for source, where, place, opened in sources():
        try:
            with opened as top:
                found, incomplete = packages_in(top, names, current, installed_base())
                for version in sorted(incomplete - {p['version'] for p in found.values()}, key=version_key):
                    problems.append(f'pb-os {version} {place} is missing parts or is still being copied or '
                                    'downloaded. It shows here once every part is there.')
                if not found: continue
                sums, sig = top / 'SHA256SUMS', top / 'SHA256SUMS.sig'
                if not (sums.is_file() and sig.is_file()) or sums.is_symlink() or sig.is_symlink():
                    problems.append(f'The update {place} needs SHA256SUMS and SHA256SUMS.sig '
                                    'from the same release next to it.')
                    continue
                with regular(sums) as f: data = f.read(1 << 20)
                with regular(sig) as f: signature = f.read(1 << 20)
                try: verify_signature(data, signature)
                except ValueError:
                    problems.append(f'SHA256SUMS {place} is not signed with the pb-os release key.')
                    continue
                listed = {f[1].lstrip('*'): f[0].lower() for f in (l.split() for l in data.decode().splitlines())
                          if len(f) == 2 and re.fullmatch('[0-9a-fA-F]{64}', f[0])}
                usable = [n for n in found if n in listed]
                if not usable:
                    problems.append(f'SHA256SUMS {place} is from another release than the update next to it.')
                    continue
                # Newest version first; a delta before the full package of the same
                # version; the image's own name before an earlier name of it.
                rank = lambda n: (version_key(found[n]['version']), found[n]['kind'] == 'delta',
                                  f'-{names[0]}.' in n)
                name = max(usable, key=rank)
                if best and best[0] >= rank(name): continue
                p = found[name]
                best = (rank(name), {'version': p['version'], 'kind': p['kind'], 'name': name, 'sha256': listed[name],
                                     'size': p['size'], 'drive': source, 'where': where, 'place': place,
                                     'files': [f.name for f in p['files']]})
        except (OSError, ValueError, subprocess.CalledProcessError) as e:
            print(f'{source}: not readable: {e}', file=sys.stderr)
    return (best[1] if best else None), problems


def local_check(args):
    found, problems = find_local()
    print(json.dumps({'current': installed_version(), 'update': found, 'problems': problems}, indent=2))


def local_update(args):
    if os.geteuid() != 0: raise ValueError('updating needs administrator access')
    if (Path('/') / PENDING).exists(): raise ValueError('an update is already pending; restart to install it')
    cleanup()
    step('Looking for an update on a drive or in the Downloads folder')
    found, problems = find_local()
    if not found: raise ValueError(problems[0] if problems else 'no pb-os update for this device on a microSD card '
                                   'or USB drive or in the Downloads folder')
    opened = next((o for s, _, _, o in sources() if s == found['drive']), None)
    if not opened: raise ValueError('the drive was removed')
    downloads = storage() / 'downloads'; downloads.mkdir(mode=0o700, exist_ok=True)
    for old in downloads.iterdir(): old.unlink()  # a download of another release
    check_space(found['kind'], found['size'])
    package = downloads / found['name']
    step(f'Copying pb-os {found["version"]} from the {found["where"]}')
    with opened as top:
        add = counter(found['size'])
        with package.open('wb') as out:
            for name in found['files']:
                with regular(top / name) as src:
                    while chunk := src.read(4 << 20):
                        out.write(chunk); add(len(chunk))
            out.flush(); os.fsync(out.fileno())
    if package.stat().st_size != found['size']: raise ValueError(f'copying from the {found["where"]} failed; try again')
    if found['drive'] != 'downloads': print('DRIVE_DONE', flush=True)
    stage(package, found['sha256'], move=True, version=found['version'])
    if found['drive'] == 'downloads': remove_downloaded(found['files'])


def remove_downloaded(files):
    """After staging: the update's files go from the Downloads folder, and
    SHA256SUMS and SHA256SUMS.sig too once no other pb-os package is left
    there. Never fails the staged update."""
    try:
        fd = os.open(DOWNLOADS, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    except OSError as e:
        print('Could not clean the Downloads folder:', e, flush=True); return
    try:
        for name in files:
            try: os.unlink(name, dir_fd=fd)
            except FileNotFoundError: pass
        if not any(n.startswith('pb-os-') and '.tar.gz' in n for n in os.listdir(fd)):
            for name in ('SHA256SUMS', 'SHA256SUMS.sig'):
                try: os.unlink(name, dir_fd=fd)
                except FileNotFoundError: pass
        print('Removed the update files from the Downloads folder', flush=True)
    except OSError as e:
        print('Could not clean the Downloads folder:', e, flush=True)
    finally:
        os.close(fd)


def install_kernel(src, boot):
    temp = boot / 'KERNEL.new'
    shutil.copyfile(src, temp)
    with temp.open('rb') as f: os.fsync(f.fileno())
    if digest(temp) != digest(src): raise ValueError('boot copy checksum mismatch')
    os.replace(temp, boot / 'KERNEL')
    (boot / 'KERNEL.md5').write_text(hashlib.md5((boot / 'KERNEL').read_bytes()).hexdigest() + '  KERNEL\n')
    os.sync()


# Plugins one device of a shared image uses (sync-decky-bundled-plugins.sh's
# DEVICE_ONLY, by DTB model here); other devices get them removed.
DEVICE_PLUGINS = {'dual-screen': ('AYN Thor',)}


def plugin_dirs(work, manifest):
    """Plugin folders the update installs, and those it removes."""
    src = work / 'payload/home/steamos' / PLUGINS
    names = sorted(p.name for p in src.iterdir() if p.is_dir()) if src.is_dir() else []
    try: model = device_model()
    except OSError: model = None  # unknown device: leave them as the package has them
    other = {n for n, models in DEVICE_PLUGINS.items() if model and model not in models}
    install = [n for n in names if n not in LEGACY_PLUGINS and n not in other]
    remove = sorted(set(manifest.get('remove_plugins', [])) | set(LEGACY_PLUGINS) | other)
    return [f'{PLUGINS}/{n}' for n in install], [f'{PLUGINS}/{n}' for n in remove if n not in install]


# ── The change plan ──────────────────────────────────────────────────────────
# Every package, full or delta, is applied as a list of root paths to write and
# a list to delete, worked out before the restart. The restart then backs up,
# writes and checks only those, so it takes about as long as the change is big,
# not as long as the system is big. plan.json in the transaction holds it.
FULL_DELETE_TOPS = ('usr', 'opt', 'etc')  # a full package replaces these whole


def managed(rel):
    """rel (root-relative) is a path the updater writes or deletes."""
    if preserved(rel) or rel in tuple('opt/' + k for k in OPT_KEEP): return False
    return any(rel == t or rel.startswith(t + '/') for t in (*ROOT_DIRS, UPPER))


def lstat(p):
    try: return os.lstat(p)
    except FileNotFoundError: return None


def same(kind, mode, uid, gid, size, mtime, link, p):
    """The installed path p already is this entry: rsync's quick check (size and
    modification time) plus type, mode and owner."""
    st = lstat(p)
    if st is None: return False
    if kind != 'l' and (stat.S_IMODE(st.st_mode), st.st_uid, st.st_gid) != (mode, uid, gid): return False
    if kind == 'd': return stat.S_ISDIR(st.st_mode)
    if kind == 'l': return stat.S_ISLNK(st.st_mode) and os.readlink(p) == link
    return stat.S_ISREG(st.st_mode) and st.st_size == size and int(st.st_mtime) == int(mtime)


def tree_entry(p):
    st = os.lstat(p)
    kind = 'l' if stat.S_ISLNK(st.st_mode) else 'd' if stat.S_ISDIR(st.st_mode) else 'f'
    return (kind, stat.S_IMODE(st.st_mode), st.st_uid, st.st_gid, st.st_size, st.st_mtime,
            os.readlink(p) if kind == 'l' else None)


def full_deletions(root, present):
    """Installed paths under the replaced tops that the new system no longer has."""
    gone = []
    for top in FULL_DELETE_TOPS:
        for base, dirs, files in os.walk(root / top):
            for name in dirs + files:
                rel = os.path.relpath(os.path.join(base, name), root)
                if rel not in present and managed(rel): gone.append(rel)
    return gone


def plan_from_tree(root, work, manifest):
    """Plan for a payload already unpacked in full (staged by an updater from
    before plans): compare it with the installed system by metadata."""
    top = work / 'payload/root'
    write, present = [], set()
    for base, dirs, files in os.walk(top):
        for name in dirs + files:
            src = Path(base) / name
            rel = os.path.relpath(src, top)
            if not managed(rel): continue
            present.add(rel)
            if is_delta(manifest) or not same(*tree_entry(src), root / rel): write.append(rel)
    delete = ([r for r in manifest.get('delete', []) if managed(r)] if is_delta(manifest)
              else full_deletions(root, present))
    return finish_plan(work, write, delete)


def plan_from_package(package, root, work, manifest):
    """Full package: compare each entry's tar header with the installed system
    and unpack only what differs, plus home/, boot/ and the tools the restart
    runs. Reads the archive once more, not the installed files."""
    write, present, extract, members = [], set(), [], {}
    always = {'root/' + UPDATER, 'root/' + str(BOOTIMG).lstrip('/')}
    total = package.stat().st_size; shown = -1
    with tarfile.open(package, 'r:gz') as tf:
        for m in tf:
            name = m.name.removeprefix('./').rstrip('/')
            if name == 'manifest.json': continue
            members[name] = m
            pos = tf.fileobj.fileobj.tell()
            if pos * 200 // total != shown:
                shown = pos * 200 // total; print('PROGRESS', pos, total, flush=True)
            if not name.startswith('root/'):
                if name != 'root': extract.append(m.name)
                continue
            if name in always: extract.append(m.name)
            rel = name[5:]
            if not managed(rel): continue
            present.add(rel)
            if m.islnk():
                # Hard link: compare with the file it links to (validated: it came first).
                t = members[m.linkname.removeprefix('./')]
                if not same('f', t.mode & 0o7777, t.uid, t.gid, t.size, t.mtime, None, root / rel):
                    write.append(rel); extract += [t.name, m.name]
                continue
            kind = 'd' if m.isdir() else 'l' if m.issym() else 'f'
            if not same(kind, m.mode & 0o7777, m.uid, m.gid, m.size, m.mtime,
                        m.linkname if m.issym() else None, root / rel):
                write.append(rel); extract.append(m.name)
    listing = work / 'extract.list'
    listing.write_bytes(b'\0'.join(n.encode() for n in dict.fromkeys(extract)) + b'\0')
    step('Unpacking the changed files')
    # -C before -T: GNU tar reads the list relative to the -C in effect.
    run('tar', '-xzf', package, '-C', work / 'payload', '--xattrs', '--acls', '--numeric-owner',
        '--no-recursion', '--null', '-T', listing)
    return finish_plan(work, write, full_deletions(root, present),
                       extra=[n for n in always if n in members])


def counter(total):
    """PROGRESS lines for PB-OS Utils, from bytes done."""
    done = [0, -1]
    def add(n):
        done[0] += n
        if total and done[0] * 200 // total != done[1]:
            done[1] = done[0] * 200 // total; print('PROGRESS', min(done[0], total), total, flush=True)
    return add


def finish_plan(work, write, delete, extra=()):
    payload = work / 'payload/root'
    size = sum(os.lstat(payload / r).st_size for r in write if (payload / r).is_file() and not (payload / r).is_symlink())
    plan = {'write': sorted(write), 'delete': sorted(delete, key=lambda r: r.count('/'), reverse=True), 'bytes': size,
            'extra': sorted(extra)}
    write_json(work / 'plan.json', plan)
    return plan


def load_plan(work):
    p = work / 'plan.json'
    return json.loads(p.read_text()) if p.is_file() else None


def entry(p):
    st = p.lstat()
    if p.is_symlink(): return ['l', os.readlink(p), st.st_uid, st.st_gid]
    if p.is_dir(): return ['d', st.st_mode & 0o7777, st.st_uid, st.st_gid]
    if stat.S_ISREG(st.st_mode): return ['f', st.st_mode & 0o7777, st.st_uid, st.st_gid]
    # A device node: in the /etc overlay's upper layer, a whiteout (0:0
    # character device) for a file the device removed from /etc.
    return ['n', st.st_mode, st.st_uid, st.st_gid, st.st_rdev]


def snapshot_paths(root, work, rels, progress=None):
    """Back up what the plan replaces or deletes and record what it creates.
    Adds to an earlier partial backup (the part taken before the restart)."""
    backup = work / 'backup'; files = backup / 'files'
    path = backup / 'delta-backup.json'
    record = json.loads(path.read_text()) if path.is_file() else {}
    for rel in rels:
        if rel in record: continue
        p = root / rel
        if not (p.exists() or p.is_symlink()): record[rel] = None; continue
        record[rel] = entry(p)
        if record[rel][0] == 'f':
            dst = files / rel; dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(p, dst, follow_symlinks=False)
            if progress: progress(p.lstat().st_size)
    write_json(path, record)


def plan_paths(plan):
    return plan['write'] + plan['delete']


def restore_delta(root, work):
    backup = work / 'backup'
    record = json.loads((backup / 'delta-backup.json').read_text())
    depth = lambda rel: rel.count('/')
    # What the update created goes, deepest first.
    for rel in sorted(record, key=depth, reverse=True):
        p = root / rel
        if record[rel] is None and (p.exists() or p.is_symlink()):
            if p.is_dir() and not p.is_symlink(): shutil.rmtree(p)
            else: p.unlink()
    # Then directories, shallowest first, and the files and links in them.
    for kinds in (('d',), ('f', 'l', 'n')):
        for rel in sorted(record, key=depth):
            e = record[rel]
            if not e or e[0] not in kinds: continue
            p = root / rel
            if e[0] == 'd':
                if p.is_symlink() or (p.exists() and not p.is_dir()): p.unlink()
                p.mkdir(exist_ok=True)
            else:
                if p.is_dir() and not p.is_symlink(): shutil.rmtree(p)
                elif p.exists() or p.is_symlink(): p.unlink()
                if e[0] == 'l': os.symlink(e[1], p)
                elif e[0] == 'n': os.mknod(p, e[1], e[4])
                else: shutil.copy2(backup / 'files' / rel, p, follow_symlinks=False)
            os.chown(p, e[2], e[3], follow_symlinks=False)
            if e[0] != 'l': os.chmod(p, stat.S_IMODE(e[1]))  # after chown, which clears setuid


def apply_plan(root, work, plan, progress=None):
    # Deletions first (deepest first), so a path that turns from a directory
    # into a file (or back) is free when the new one is copied.
    for rel in plan['delete']:
        p = root / rel
        if not (p.exists() or p.is_symlink()): continue
        if p.is_dir() and not p.is_symlink():
            try: p.rmdir()
            except OSError: pass  # files the update does not manage stay
        else: p.unlink()
    if not plan['write']: return
    # One rsync for the whole list keeps hard links between written files.
    listing = work / 'write.list'
    listing.write_bytes(b'\0'.join(r.encode() for r in plan['write']) + b'\0')
    args = ['rsync', '-aHAX', '--ignore-times', '--numeric-ids', '--from0', '--files-from', str(listing),
            '--no-implied-dirs', '--info=progress2', '--no-inc-recursive', str(work / 'payload/root') + '/', str(root) + '/']
    print('+', ' '.join(args), flush=True)
    p = subprocess.Popen(args, stdout=subprocess.PIPE)
    buf = b''
    while chunk := p.stdout.read1(4096):
        buf += chunk
        *lines, buf = re.split(rb'[\r\n]', buf)
        for line in lines:
            m = re.search(rb'(\d+)%', line)
            if m and progress: progress(int(m.group(1)) / 100)
    if p.wait() != 0: raise ValueError(f'copying the new files failed (rsync {p.returncode})')


def verify_written(root, home, manifest, plan, install, progress=None):
    """Check every regular file the update wrote against the package manifest."""
    files = manifest['files']
    for rel in plan['write']:
        sha = files.get('root/' + rel)
        if sha is None: continue
        if digest(root / rel) != sha: raise ValueError(f'installed checksum mismatch: root/{rel}')
        if progress: progress((root / rel).stat().st_size)
    for name, sha in files.items():
        if any(name.startswith('home/steamos/' + rel + '/') for rel in install):
            if digest(home / name[5:]) != sha: raise ValueError(f'installed checksum mismatch: {name}')


def verify_planned(work, manifest, plan, progress=None):
    """Check the payload files the plan installs (the rest of the payload is
    never read at the restart)."""
    payload = work / 'payload'; files = manifest['files']
    names = ['root/' + r for r in plan['write'] if 'root/' + r in files]
    names += [n for n in files if not n.startswith('root/')]
    names += [n for n in plan.get('extra', []) if n in files and n not in names]
    for name in names:
        p = payload / name
        if p.is_symlink() or not p.is_file() or digest(p) != files[name]:
            raise ValueError(f'payload checksum mismatch: {name}')
        if progress: progress(p.stat().st_size)


def snapshot(root, home, work, manifest, plan, progress=None):
    backup = work / 'backup'
    install, remove = plugin_dirs(work, manifest)
    snapshot_paths(root, work, plan_paths(plan), progress)
    for rel in install + remove:
        src = home / 'steamos' / rel
        if src.exists(): copy_tree(src, backup / 'home/steamos' / rel)
    # Explicitly record absent directories so rollback removes newly introduced ones.
    write_json(backup / 'home-presence.json', {rel: (home / 'steamos' / rel).exists() for rel in install + remove})
    local = home / 'steamos/.local/share/vulkan/implicit_layer.d'
    if local.exists(): copy_tree(local, backup / 'layers')
    write_json(backup / 'layers-presence.json', {'exists': local.exists()})
    os.sync()
    state(work, 'backed-up')


def restore(root, boot, home, work):
    backup = work / 'backup'
    if (backup / 'delta-backup.json').exists():
        restore_delta(root, work)
    else:
        present_root = json.loads((backup / 'root-presence.json').read_text())
        for rel in (*ROOT_DIRS, UPPER):
            src = backup / 'root' / rel
            if present_root[rel]: copy_tree(src, root / rel, delete=True)
            elif (root / rel).exists(): shutil.rmtree(root / rel)
    present = json.loads((backup / 'home-presence.json').read_text())
    for rel in present:
        dest = home / 'steamos' / rel
        if present[rel]: copy_tree(backup / 'home/steamos' / rel, dest, delete=True)
        elif dest.exists(): shutil.rmtree(dest)
    layers = home / 'steamos/.local/share/vulkan/implicit_layer.d'
    if json.loads((backup / 'layers-presence.json').read_text())['exists']:
        copy_tree(backup / 'layers', layers, delete=True)
    elif layers.exists(): shutil.rmtree(layers)
    os.sync()
    install_kernel(work / 'previous-KERNEL', boot)


# ── Progress on screen during the restart step ───────────────────────────────
# The initramfs runs recover() before anything else draws, so this paints the
# framebuffer itself: a title, a progress bar and Barry Launcher's pulsing
# purple dots, so the device visibly keeps working. Drawing never stops the
# update; when the framebuffer is missing it simply shows nothing.
PURPLE = (0xA8, 0x55, 0xF7)
# 5x7 font, rows top to bottom.
GLYPHS = {k: v.replace(' ', '') for k, v in {
    'A': '.###. #...# #...# ##### #...# #...# #...#',
    'B': '####. #...# #...# ####. #...# #...# ####.',
    'C': '.###. #...# #.... #.... #.... #...# .###.',
    'D': '####. #...# #...# #...# #...# #...# ####.',
    'E': '##### #.... #.... ####. #.... #.... #####',
    'F': '##### #.... #.... ####. #.... #.... #....',
    'G': '.###. #...# #.... #.### #...# #...# .####',
    'H': '#...# #...# #...# ##### #...# #...# #...#',
    'I': '.###. ..#.. ..#.. ..#.. ..#.. ..#.. .###.',
    'J': '..### ...#. ...#. ...#. ...#. #..#. .##..',
    'K': '#...# #..#. #.#.. ##... #.#.. #..#. #...#',
    'L': '#.... #.... #.... #.... #.... #.... #####',
    'M': '#...# ##.## #.#.# #.#.# #...# #...# #...#',
    'N': '#...# #...# ##..# #.#.# #..## #...# #...#',
    'O': '.###. #...# #...# #...# #...# #...# .###.',
    'P': '####. #...# #...# ####. #.... #.... #....',
    'Q': '.###. #...# #...# #...# #.#.# #..#. .##.#',
    'R': '####. #...# #...# ####. #.#.. #..#. #...#',
    'S': '.#### #.... #.... .###. ....# ....# ####.',
    'T': '##### ..#.. ..#.. ..#.. ..#.. ..#.. ..#..',
    'U': '#...# #...# #...# #...# #...# #...# .###.',
    'V': '#...# #...# #...# #...# #...# .#.#. ..#..',
    'W': '#...# #...# #...# #.#.# #.#.# #.#.# .#.#.',
    'X': '#...# #...# .#.#. ..#.. .#.#. #...# #...#',
    'Y': '#...# #...# .#.#. ..#.. ..#.. ..#.. ..#..',
    'Z': '##### ....# ...#. ..#.. .#... #.... #####',
    '0': '.###. #...# #..## #.#.# ##..# #...# .###.',
    '1': '..#.. .##.. ..#.. ..#.. ..#.. ..#.. .###.',
    '2': '.###. #...# ....# ...#. ..#.. .#... #####',
    '3': '##### ...#. ..#.. ...#. ....# #...# .###.',
    '4': '...#. ..##. .#.#. #..#. ##### ...#. ...#.',
    '5': '##### #.... ####. ....# ....# #...# .###.',
    '6': '..##. .#... #.... ####. #...# #...# .###.',
    '7': '##### ....# ...#. ..#.. .#... .#... .#...',
    '8': '.###. #...# #...# .###. #...# #...# .###.',
    '9': '.###. #...# #...# .#### ....# ...#. .##..',
    '%': '##... ##..# ...#. ..#.. .#... #..## ...##',
    ':': '..... .##.. .##.. ..... .##.. .##.. .....',
    '.': '..... ..... ..... ..... ..... .##.. .##..',
    '-': '..... ..... ..... ##### ..... ..... .....',
    ' ': ' '.join(['.....'] * 7),
}.items()}


def screen_info():
    """Panel rotation from the device tree, for the restart step (no /sys there)."""
    try:
        for p in sorted(Path('/sys/firmware/devicetree/base').rglob('rotation')):
            if p.parent.name.startswith('panel') and len(p.read_bytes()) == 4:
                return {'rotation': int.from_bytes(p.read_bytes(), 'big')}
    except OSError:
        pass
    return {}


class Screen:
    def __init__(self, info, devices):
        self.ok = False; self.stop = threading.Event(); self.lock = threading.Lock()
        self.fraction = -1; self.text = ''
        try: self.open(info or {}, devices)
        except Exception as e: print('screen: not drawing:', e, flush=True)

    def open(self, info, devices):
        import mmap, struct
        self.fd = os.open('/dev/fb0', os.O_RDWR)
        var = fcntl.ioctl(self.fd, 0x4600, bytes(160))   # FBIOGET_VSCREENINFO
        fix = fcntl.ioctl(self.fd, 0x4602, bytes(80))    # FBIOGET_FSCREENINFO
        self.xres, self.yres, _, _, xoff, yoff, bpp = struct.unpack_from('7I', var)
        self.bpp = bpp // 8; self.stride = struct.unpack_from('I', fix, 48)[0]
        size = struct.unpack_from('I', fix, 24)[0]
        if self.bpp not in (2, 4) or not self.stride: raise ValueError(f'unsupported framebuffer ({bpp} bpp)')
        self.map = mmap.mmap(self.fd, size, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE)
        self.base = yoff * self.stride + xoff * self.bpp
        rot = info.get('rotation')
        if rot is None:  # staged by an updater that did not record it
            try: model = device_model()
            except OSError: model = devices[0] if len(devices) == 1 else ''
            rot = 0 if self.xres > self.yres else 270 if 'Retroid' in model else 90
        self.rot = rot % 360
        self.w, self.h = (self.yres, self.xres) if self.rot in (90, 270) else (self.xres, self.yres)
        try:  # keep the console cursor off the picture
            self.tty = os.open('/dev/tty0', os.O_RDWR); fcntl.ioctl(self.tty, 0x4B3A, 1)  # KDSETMODE KD_GRAPHICS
        except OSError:
            self.tty = None
        self.s = self.h / 1080
        self.ok = True

    def color(self, rgb):
        r, g, b = rgb
        if self.bpp == 4: return bytes((b, g, r, 0))
        v = (r >> 3) << 11 | (g >> 2) << 5 | b >> 3
        return v.to_bytes(2, 'little')

    def rect(self, x, y, w, h, rgb):
        """Fill a rectangle given in upright (landscape) coordinates."""
        x, y, w, h = int(x), int(y), int(w), int(h)
        if self.rot == 90: px, py, pw, ph = self.xres - y - h, x, h, w
        elif self.rot == 270: px, py, pw, ph = y, self.yres - x - w, h, w
        elif self.rot == 180: px, py, pw, ph = self.xres - x - w, self.yres - y - h, w, h
        else: px, py, pw, ph = x, y, w, h
        x0, y0 = max(0, px), max(0, py); x1, y1 = min(self.xres, px + pw), min(self.yres, py + ph)
        if x0 >= x1 or y0 >= y1: return
        row = self.color(rgb) * (x1 - x0)
        for yy in range(y0, y1):
            o = self.base + yy * self.stride + x0 * self.bpp
            self.map[o:o + len(row)] = row

    def circle(self, cx, cy, r, rgb):
        r = max(1, int(r))
        for dy in range(-r, r + 1):
            half = int((r * r - dy * dy) ** 0.5)
            self.rect(cx - half, cy + dy, 2 * half + 1, 1, rgb)

    def write(self, text, cy, px, rgb, clear_width=None):
        """Centred text, px screen pixels per font pixel."""
        text = text.upper(); px = max(1, int(px))
        width = len(text) * 6 * px - px; x = (self.w - width) // 2; y = int(cy - 3.5 * px)
        if clear_width: self.rect((self.w - clear_width) // 2, y - px, clear_width, 9 * px, (0, 0, 0))
        for ch in text:
            g = GLYPHS.get(ch, GLYPHS[' '])
            for i, c in enumerate(g):
                if c == '#': self.rect(x + (i % 5) * px, y + (i // 5) * px, px, px, rgb)
            x += 6 * px

    # Layout, upright: title, bar, percentage, dots, warning.
    def start(self, title):
        if not self.ok: return
        with self.lock:
            self.map[:] = bytes(len(self.map))
            self.text = title
            self.write(title, self.h * 0.34, 8 * self.s, (255, 255, 255))
            self.write('Do not power off', self.h * 0.84, 4 * self.s, (150, 150, 160))
        self.progress(0)
        threading.Thread(target=self.pulse, daemon=True).start()

    def label(self, text):
        if not self.ok: return
        with self.lock: self.write(text, self.h * 0.34, 8 * self.s, (255, 255, 255), clear_width=self.w)

    def progress(self, f):
        if not self.ok: return
        f = max(0.0, min(1.0, f)); permille = int(f * 1000)
        if permille == self.fraction: return
        self.fraction = permille
        bw, bh = int(self.w * 0.5), max(6, int(18 * self.s)); bx, by = (self.w - bw) // 2, int(self.h * 0.5)
        with self.lock:
            self.rect(bx, by, bw, bh, (48, 44, 60))
            self.rect(bx, by, bw * f, bh, PURPLE)
            self.write(f'{int(f * 100)}%', self.h * 0.58, 4 * self.s, (200, 200, 210), clear_width=int(200 * self.s))

    def pulse(self):
        import math
        r = 14 * self.s; gap = 56 * self.s; cy = self.h * 0.68; t0 = time.monotonic()
        while not self.stop.wait(1 / 30):
            t = time.monotonic() - t0
            with self.lock:
                for i in range(3):
                    v = 0.5 + 0.5 * math.sin(2 * math.pi * t / 1.2 - i * 0.7)
                    cx = self.w / 2 + (i - 1) * gap
                    self.rect(cx - r - 1, cy - r - 1, 2 * r + 3, 2 * r + 3, (0, 0, 0))
                    self.circle(cx, cy, r * (0.55 + 0.45 * v), tuple(int(c * (0.35 + 0.65 * v)) for c in PURPLE))

    def close(self):
        if not self.ok: return
        self.stop.set(); time.sleep(0.05)
        try:
            if self.tty is not None: fcntl.ioctl(self.tty, 0x4B3A, 0)  # KD_TEXT for what boots next
        except OSError:
            pass


class Progress:
    """One fraction for the whole restart step, from bytes done per phase."""
    def __init__(self, screen, phases):
        self.screen, self.phases = screen, phases  # [(name, bytes), ...]
        self.total = max(1, sum(b for _, b in phases))
        self.base = 0; self.done = 0; self.size = 0

    def phase(self, name):
        names = [n for n, _ in self.phases]
        i = names.index(name)
        self.base = sum(b for _, b in self.phases[:i]); self.size = self.phases[i][1]; self.done = 0
        self.show()

    def add(self, n):
        self.done = min(self.size, self.done + n); self.show()

    def fraction(self, f):
        self.done = int(self.size * f); self.show()

    def show(self):
        self.screen.progress((self.base + self.done) / self.total)


def apply(root, boot, home, work, manifest, plan, progress):
    payload = work / 'payload'
    progress.phase('write')
    apply_plan(root, work, plan, progress.fraction)
    install, remove = plugin_dirs(work, manifest)
    for rel in install:
        copy_tree(payload / 'home/steamos' / rel, home / 'steamos' / rel, delete=True)
        # Images stage users with numeric ownership; do not inherit root ownership.
        run('chown', '-R', '1000:1000', home / 'steamos' / rel)
    # Plugins of other devices or older images (the snapshot restores them on rollback).
    for rel in remove:
        if (home / 'steamos' / rel).exists(): shutil.rmtree(home / 'steamos' / rel)
    # Old frame-gen layer manifests, but never one this package installs:
    # lsfg-vk 1.x ships /usr/share/vulkan/implicit_layer.d/VkLayer_LS_frame_generation_arm64.json.
    for prefix in (root / 'usr', root / 'usr/local', home / 'steamos/.local'):
        for name in ('VkLayer_LS_frame_generation.json', 'VkLayer_LS_frame_generation_arm64.json'):
            layer = prefix / 'share/vulkan/implicit_layer.d' / name
            if layer.is_relative_to(root) and f'root/{layer.relative_to(root)}' in manifest['files']: continue
            layer.unlink(missing_ok=True)
    progress.phase('check')
    verify_written(root, home, manifest, plan, install, progress.add)
    os.sync()
    install_kernel(work / 'next-KERNEL', boot)


def recover(args):
    if os.geteuid() != 0: raise ValueError('recovery needs administrator access')
    root, boot, home, work = map(Path, (args.root, args.boot, args.home, args.work))
    if work.stat().st_uid != 0: raise ValueError('transaction is not owned by root')
    os.environ['PATH'] = '/usr/bin:/usr/sbin:/bin:/sbin'
    record = json.loads((work / 'transaction.json').read_text())
    for path, key in ((root, 'root_uuid'), (home, 'home_uuid'), (boot, 'boot_uuid')):
        expected = record.get(key)
        if not expected or mount_info(str(path)).get('uuid') != expected:
            raise ValueError(f'{key} does not match the staged installation')
    pending = root / PENDING
    if not pending.exists() or pending.read_text().strip() != record['id']:
        raise ValueError('transaction does not match this root filesystem')
    current = json.loads((work / 'state.json').read_text())['state']
    if current == 'committed':
        pending.unlink(); os.sync(); return 0
    if current == 'rolled-back':
        pending.unlink(); os.sync(); return 10
    manifest = record['manifest']
    screen = Screen(record.get('screen'), manifest.get('devices', []))
    try:
        return recover_steps(root, boot, home, work, manifest, pending, current, screen)
    finally:
        screen.close()


def recover_steps(root, boot, home, work, manifest, pending, current, screen):
    screen.start('Installing update')
    plan = load_plan(work)
    if plan is None and current == 'staged':
        # Staged by an updater from before plans: the whole payload is
        # unpacked; compare it with the system once, by metadata.
        screen.label('Preparing update')
        plan = plan_from_tree(root, work, manifest)
    size = (plan or {}).get('bytes', 0)
    progress = Progress(screen, [('verify', size), ('backup', size), ('write', size), ('check', size)])
    if current == 'staged':
        try:
            progress.phase('verify')
            verify_planned(work, manifest, plan, progress.add)
            progress.phase('backup')
            snapshot(root, home, work, manifest, plan, progress.add)
            current = 'backed-up'
        except Exception as e:
            print('RECOVERY ERROR:', e, flush=True)
            (work / 'failure.txt').write_text(str(e) + '\n')
            screen.label('Update failed: keeping the current version')
            install_kernel(work / 'previous-KERNEL', boot)
            state(work, 'aborted')
            pending.unlink(); os.sync(); return 10
    if current == 'backed-up':
        try:
            state(work, 'applying')
            apply(root, boot, home, work, manifest, plan, progress)
            state(work, 'committed')
            screen.label('Update installed')
            screen.progress(1)
            pending.unlink(); os.sync(); return 0
        except Exception as e:
            print('RECOVERY ERROR:', e, flush=True)
            (work / 'failure.txt').write_text(str(e) + '\n')
            current = 'applying'
    if current in ('applying', 'rolling-back', 'rollback-requested'):
        screen.label('Update failed: restoring the previous version')
        state(work, 'rolling-back')
        restore(root, boot, home, work)
        state(work, 'rolled-back')
        pending.unlink(); os.sync(); return 10
    raise ValueError(f'cannot recover state: {current}')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    sub = ap.add_subparsers(dest='command', required=True)
    p = sub.add_parser('stage'); p.add_argument('package'); p.add_argument('--sha256', required=True)
    sub.add_parser('check', help='newest release for this device, as JSON')
    p = sub.add_parser('update', help='download the newest release and stage it')
    p.add_argument('--reinstall', action='store_true', help='also when that version is installed')
    sub.add_parser('local-check', help='update on a microSD card or USB drive or in ~/Downloads, as JSON')
    sub.add_parser('local-update', help='copy the update from a microSD card, USB drive or ~/Downloads and stage it')
    p = sub.add_parser('recover')
    for name in ('root', 'boot', 'home', 'work'): p.add_argument('--' + name, required=True)
    p = sub.add_parser('inspect'); p.add_argument('package')
    sub.add_parser('cleanup', help='free what finished updates left on HOME')
    a = ap.parse_args()
    try:
        if a.command in ('stage', 'update', 'local-update', 'cleanup'):
            if os.geteuid() != 0: raise ValueError('needs administrator access')
            with open('/run/konkr-update.lock', 'w') as lock:
                try: fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                except BlockingIOError:
                    if a.command == 'cleanup': return 0  # an update is being prepared; next time
                    raise
                if a.command == 'stage': stage(a.package, a.sha256)
                elif a.command == 'update': update(a)
                elif a.command == 'local-update': local_update(a)
                else: cleanup()
        elif a.command == 'check': check(a)
        elif a.command == 'local-check': local_check(a)
        elif a.command == 'recover': return recover(a)
        else:
            m, size = validate_archive(a.package)
            print(json.dumps({'version': m['version'], 'kind': 'delta' if is_delta(m) else 'full',
                              **({'from': m['from']} if is_delta(m) else {}), 'bytes': size}, indent=2))
    except Exception as e:
        print('ERROR:', e, file=sys.stderr); return 1
    return 0


if __name__ == '__main__': sys.exit(main())
