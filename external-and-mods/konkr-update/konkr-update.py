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
import subprocess
import sys
import tarfile
import time
import uuid
from pathlib import Path, PurePosixPath

FORMAT = 1
ROOT_DIRS = ('usr', 'opt', 'etc')
UPPER = 'var/lib/overlays/etc/upper'
PLUGINS = 'homebrew/plugins'
# Plugin folders from older images: removed on apply, restored on rollback.
# SM8650 packages still carry an empty konkr-control folder, which updaters
# from before the PB-OS Control rename require.
LEGACY_PLUGINS = ('konkr-control',)
PRESERVE = ('passwd', 'shadow', 'group', 'gshadow', 'machine-id', 'hostname', 'hosts',
            'fstab', 'crypttab', 'localtime', 'adjtime', 'resolv.conf', 'ssh',
            'NetworkManager/system-connections', 'sudoers.d')
PENDING = 'var/lib/konkr-update/pending'
# Per-install record written by the image build; not part of any package.
OPT_KEEP = ('steamos-sm8650/IMAGE.txt',)
UPDATER = 'usr/share/konkr-update/konkr-update.py'
# Written by make-steamos-sm8650.sh: the release tag the image was built as.
VERSION_FILE = 'usr/share/pb-os/version'
RELEASES = 'https://api.github.com/repos/project-barry/pb-os/releases'
# Release asset of each device's package: pb-os-<tag>-<image>.update.tar.gz,
# split into .001, .002, ... parts under GitHub's 2 GiB limit.
# Downloads install only when the release's SHA256SUMS carries a signature
# (SHA256SUMS.sig, scripts/sign-release.sh) from a key in this file.
SIGNERS = Path('/usr/share/konkr-update/allowed_signers')
SIGNER, NAMESPACE = 'pb-os-release', 'pb-os-update'
IMAGES = {'KONKR Pocket FIT': 'pocketfit', 'AYANEO Pocket S2': 'pocketfit',
          'Retroid Pocket 6': 'rp6', 'Retroid Pocket 6 TOP-DPAD': 'rp6', 'AYN Thor': 'thor'}


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


def mount_info(path):
    r = run('findmnt', '-J', '-o', 'SOURCE,FSTYPE,TARGET,UUID', '-T', path,
            capture_output=True, text=True)
    return json.loads(r.stdout)['filesystems'][0]


def validate_archive(package):
    seen = set(); links = set(); directories = set(); total = 0; manifest = None
    with tarfile.open(package, 'r:gz') as tf:
        for m in tf:
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
    if not {'root/usr', 'root/opt', 'root/etc', 'boot'}.issubset(directories):
        raise ValueError('required payload directories must be real directories')
    if not manifest or manifest.get('format') != FORMAT or manifest.get('architecture') != 'aarch64':
        raise ValueError('unsupported update format or architecture')
    devices = manifest.get('devices')
    if not isinstance(devices, list) or not devices or not all(isinstance(d, str) and d for d in devices):
        raise ValueError('unsupported device list')
    removed = manifest.get('remove_plugins', [])
    if not isinstance(removed, list) or not all(isinstance(n, str) and re.fullmatch('[A-Za-z0-9_-][A-Za-z0-9._-]*', n) for n in removed):
        raise ValueError('invalid plugin removal list')
    files = manifest.get('files', {})
    if not isinstance(files, dict): raise ValueError('invalid file manifest')
    # Every regular file must be covered, and file keys cannot escape extraction.
    with tarfile.open(package, 'r:gz') as tf:
        actual = {m.name.removeprefix('./'): m for m in tf if (m.isfile() or m.islnk()) and m.name.removeprefix('./') != 'manifest.json'}
    if set(actual) != set(files): raise ValueError('manifest does not cover all payload files')
    for name, sha in files.items():
        if not re.fullmatch('[0-9a-f]{64}', sha): raise ValueError(f'invalid checksum: {name}')
    return manifest, total


def verify_payload(payload, manifest):
    for name, sha in manifest['files'].items():
        p = payload / name
        if p.is_symlink() or not p.is_file() or digest(p) != sha:
            raise ValueError(f'payload checksum mismatch: {name}')
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


def storage():
    """Root-only update storage on HOME."""
    base = Path('/home/.konkr-updates')
    if base.is_symlink(): raise ValueError('invalid update storage directory')
    base.mkdir(mode=0o700, exist_ok=True)
    if base.stat().st_uid != 0: raise ValueError('update storage is not owned by root')
    os.chmod(base, 0o700)
    return base


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
        used = shutil.disk_usage('/').total - shutil.disk_usage('/').free
        if shutil.disk_usage('/home').free < size + used + (512 << 20):
            raise ValueError('not enough HOME space for payload and rollback backup')
        managed = sum(int(run('du', '-sx', '-B1', '/' + name, capture_output=True,
                             text=True).stdout.split()[0]) for name in ROOT_DIRS)
        if shutil.disk_usage('/').free + managed < size + (512 << 20):
            raise ValueError('root partition is too small for this update')
        payload = work / 'payload'; payload.mkdir()
        step('Unpacking the update')
        run('tar', '--xattrs', '--acls', '--numeric-owner', '-xzf', package, '-C', payload)
        verify_payload(payload, manifest)
        info = {'id': work.name, 'version': manifest['version'], 'sha256': expected,
                'root_uuid': root_info['uuid'], 'home_uuid': home_info['uuid'],
                'boot_uuid': boot_info['uuid'], 'manifest': manifest}
        write_json(work / 'transaction.json', info)
        shutil.copy2('/boot/KERNEL', work / 'previous-KERNEL')
        rootarg = next((x[5:] for x in Path('/proc/cmdline').read_text().split() if x.startswith('root=')), '')
        if not rootarg: raise ValueError('cannot identify the boot root argument')
        helper = bootimg_helper(payload)
        retarget_kernel(payload / 'boot/KERNEL', work / 'next-KERNEL', rootarg, helper)
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


def find_update():
    """Newest release with a package for this device, or None."""
    model = device_model()
    image = IMAGES.get(model)
    if not image: raise ValueError(f'no update channel for {model}')
    with fetch(RELEASES + '?per_page=30', Accept='application/vnd.github+json') as r:
        releases = json.load(r)
    for release in releases:  # newest first
        if release.get('draft'): continue
        name = f"pb-os-{release['tag_name']}-{image}.update.tar.gz"
        parts = sorted((a for a in release['assets'] if re.fullmatch(re.escape(name) + r'\.\d{3}', a['name'])),
                       key=lambda a: a['name'])
        sums = next((a for a in release['assets'] if a['name'] == 'SHA256SUMS'), None)
        sig = next((a for a in release['assets'] if a['name'] == 'SHA256SUMS.sig'), None)
        if parts and sums and sig:
            return {'version': release['tag_name'], 'title': release.get('name') or release['tag_name'],
                    'page': release['html_url'], 'name': name, 'sums': sums['browser_download_url'],
                    'sig': sig['browser_download_url'],
                    'parts': [{'url': a['browser_download_url'], 'size': a['size']} for a in parts],
                    'size': sum(a['size'] for a in parts)}
    return None


def check(args):
    current = installed_version()
    found = find_update()
    print(json.dumps({'current': current, 'update': found,
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
    step('Looking for updates')
    found = find_update()
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
    used = shutil.disk_usage('/').total - shutil.disk_usage('/').free
    need = found['size'] - have + found['size'] * 5 // 2 + used + (512 << 20)
    if shutil.disk_usage('/home').free < need:
        raise ValueError(f'not enough free space: this update needs about {need >> 30} GB free on HOME')
    step(f'Downloading pb-os {found["version"]}')
    download(found, package)
    stage(package, expected, move=True, version=found['version'])


def install_kernel(src, boot):
    temp = boot / 'KERNEL.new'
    shutil.copyfile(src, temp)
    with temp.open('rb') as f: os.fsync(f.fileno())
    if digest(temp) != digest(src): raise ValueError('boot copy checksum mismatch')
    os.replace(temp, boot / 'KERNEL')
    (boot / 'KERNEL.md5').write_text(hashlib.md5((boot / 'KERNEL').read_bytes()).hexdigest() + '  KERNEL\n')
    os.sync()


def plugin_dirs(work, manifest):
    """Plugin folders the update installs, and those it removes."""
    src = work / 'payload/home/steamos' / PLUGINS
    names = sorted(p.name for p in src.iterdir() if p.is_dir()) if src.is_dir() else []
    install = [n for n in names if n not in LEGACY_PLUGINS]
    remove = sorted(set(manifest.get('remove_plugins', [])) | set(LEGACY_PLUGINS))
    return [f'{PLUGINS}/{n}' for n in install], [f'{PLUGINS}/{n}' for n in remove if n not in install]


def snapshot(root, home, work, manifest):
    backup = work / 'backup'
    install, remove = plugin_dirs(work, manifest)
    write_json(backup / 'root-presence.json', {rel: (root / rel).exists() for rel in (*ROOT_DIRS, UPPER)})
    for rel in (*ROOT_DIRS, UPPER):
        src = root / rel
        if src.exists(): copy_tree(src, backup / 'root' / rel)
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


def apply(root, boot, home, work, manifest):
    payload = work / 'payload'
    copy_tree(payload / 'root/usr', root / 'usr', delete=True)
    copy_tree(payload / 'root/opt', root / 'opt', delete=True, excludes=OPT_KEEP)
    # Preserve identity, accounts, network credentials and the target partition layout.
    copy_tree(payload / 'root/etc', root / 'etc', delete=True, excludes=PRESERVE)
    upper = payload / 'root' / UPPER
    if upper.exists(): copy_tree(upper, root / UPPER, excludes=PRESERVE)
    install, remove = plugin_dirs(work, manifest)
    for rel in install:
        copy_tree(payload / 'home/steamos' / rel, home / 'steamos' / rel, delete=True)
        # Images stage users with numeric ownership; do not inherit root ownership.
        run('chown', '-R', '1000:1000', home / 'steamos' / rel)
    # Plugins of other devices or older images (the snapshot restores them on rollback).
    for rel in remove:
        if (home / 'steamos' / rel).exists(): shutil.rmtree(home / 'steamos' / rel)
    for prefix in (root / 'usr', root / 'usr/local', home / 'steamos/.local'):
        for name in ('VkLayer_LS_frame_generation.json', 'VkLayer_LS_frame_generation_arm64.json'):
            (prefix / 'share/vulkan/implicit_layer.d' / name).unlink(missing_ok=True)
    # Compare every managed regular file that was installed. No game/save paths included.
    for name, sha in manifest['files'].items():
        if name.startswith(('root/usr/', 'root/opt/')): target = root / name[5:]
        elif any(name.startswith('home/steamos/' + rel + '/') for rel in install): target = home / name[5:]
        elif name.startswith(('root/etc/', 'root/' + UPPER + '/')):
            prefix = 'root/etc/' if name.startswith('root/etc/') else 'root/' + UPPER + '/'
            rel = name[len(prefix):]
            if any(rel == p or rel.startswith(p + '/') for p in PRESERVE): continue
            target = root / name[5:]
        else: continue
        if digest(target) != sha: raise ValueError(f'installed checksum mismatch: {name}')
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
    if current == 'staged':
        try:
            verify_payload(work / 'payload', record['manifest'])
            snapshot(root, home, work, record['manifest'])
            current = 'backed-up'
        except Exception as e:
            print('RECOVERY ERROR:', e, flush=True)
            (work / 'failure.txt').write_text(str(e) + '\n')
            install_kernel(work / 'previous-KERNEL', boot)
            state(work, 'aborted')
            pending.unlink(); os.sync(); return 10
    if current == 'backed-up':
        try:
            state(work, 'applying')
            apply(root, boot, home, work, record['manifest'])
            state(work, 'committed')
            pending.unlink(); os.sync(); return 0
        except Exception as e:
            print('RECOVERY ERROR:', e, flush=True)
            (work / 'failure.txt').write_text(str(e) + '\n')
            current = 'applying'
    if current in ('applying', 'rolling-back', 'rollback-requested'):
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
    p = sub.add_parser('recover')
    for name in ('root', 'boot', 'home', 'work'): p.add_argument('--' + name, required=True)
    p = sub.add_parser('inspect'); p.add_argument('package')
    a = ap.parse_args()
    try:
        if a.command in ('stage', 'update'):
            with open('/run/konkr-update.lock', 'w') as lock:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                stage(a.package, a.sha256) if a.command == 'stage' else update(a)
        elif a.command == 'check': check(a)
        elif a.command == 'recover': return recover(a)
        else:
            m, size = validate_archive(a.package); print(json.dumps({'version': m['version'], 'bytes': size}, indent=2))
    except Exception as e:
        print('ERROR:', e, file=sys.stderr); return 1
    return 0


if __name__ == '__main__': sys.exit(main())
