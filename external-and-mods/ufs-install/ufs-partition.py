#!/usr/bin/env python3
"""Inspect, repartition and restore internal UFS for the SteamOS install.

  ufs-partition.py detect  [--disk /dev/sda] [--storage-gib 20]
  ufs-partition.py apply   --android-gib N --expect FINGERPRINT --backup-dir DIR
                           [--disk /dev/sda] [--storage-gib 20] [--dry-run]
  ufs-partition.py restore --backup FILE [--disk /dev/sda] [--dry-run]

Layout after apply (everything before userdata is never touched):

  ... | userdata (N GiB) | ROCKNIX 2 GiB | STORAGE (20 GiB) | HOME (rest) |

Rules:
  - Only the partition named "userdata" changes, and only its end. Its
    number, start, type, GUID, name and attributes stay as they are.
  - The new table goes to the disk in a single sfdisk call, built from
    sfdisk's own dump of the current table.
  - apply refuses unless the table still matches the fingerprint that was
    shown to the user (detect prints it), and saves the old table first
    (sfdisk dump + JSON) so `restore` can put Android's layout back without
    the bootloader menu.
  - The result is checked twice: the table on disk and the kernel's view
    (/sys/class/block) must both match the plan.
  - Android's user data is then erased the way a factory reset does it:
    the head and tail of the new userdata are zeroed (no filesystem left
    to mount) and the metadata-encryption key in the "metadata" partition
    is removed, so the old encrypted blocks can never be read again.
    Android makes a new key and formats userdata on its next boot.

detect prints KEY=VALUE lines for the shell installer and the GUI.
"""
from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

GIB = 1 << 30
MIB = 1 << 20
BOOT_GIB = 2
DEFAULT_STORAGE_GIB = 20
# Android stayed black at 8 GiB on the Pocket FIT (2026-10-11); 16 GiB is the
# smallest size Android has booted with (Nova).
MIN_ANDROID_GIB = 16
MIN_HOME_GIB = 16
OURS = ("ROCKNIX", "STORAGE", "HOME")
# ROCKNIX: FAT32 as "basic data", the type the ABL boots from on the
# Pocket FIT install this layout was proven on.
TYPE_FAT = "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7"
TYPE_LINUX = "0FC63DAF-8483-4772-8E79-3D69D8477DE4"


def die(msg: str) -> None:
    print(f"ERROR: {msg}", file=sys.stderr)
    sys.exit(1)


def sh(*cmd: str, inp: str | None = None) -> str:
    r = subprocess.run(cmd, input=inp, capture_output=True, text=True)
    if r.returncode != 0:
        die(f"{' '.join(cmd)} failed: {r.stderr.strip() or r.stdout.strip()}")
    return r.stdout


# ------------------------------------------------------------- reading ----
def read_table(disk: str) -> dict:
    t = json.loads(sh("sfdisk", "--json", disk))["partitiontable"]
    if t.get("label") != "gpt":
        die(f"{disk}: not a GPT disk")
    t["sectorsize"] = int(t.get("sectorsize", 512))
    for p in t["partitions"]:
        p.setdefault("name", "")
        p.setdefault("attrs", "")
    t["partitions"].sort(key=lambda p: p["start"])
    return t


def fingerprint(t: dict) -> str:
    keys = ("node", "start", "size", "type", "uuid", "name", "attrs")
    norm = [{k: p.get(k) for k in keys} for p in t["partitions"]]
    blob = json.dumps({"id": t.get("id"), "lastlba": t.get("lastlba"),
                       "parts": norm}, sort_keys=True)
    return hashlib.sha256(blob.encode()).hexdigest()[:16]


def userdata(t: dict) -> dict:
    ud = [p for p in t["partitions"] if p["name"] == "userdata"]
    if len(ud) != 1:
        die(f"expected exactly one partition named userdata, found {len(ud)}")
    return ud[0]


def leftovers(t: dict) -> list[dict]:
    """Entries the ABL's UNINSTALL CFW leaves behind: it grows userdata back
    over our partitions but keeps their GPT slots, nameless and inside
    userdata, with an all-zero type (GPT's "unused entry"). Nothing can use
    them, so they are dropped."""
    ud = userdata(t)
    end = ud["start"] + ud["size"]
    return [p for p in t["partitions"]
            if p["start"] > ud["start"] and not p["name"]
            and (p["type"].strip("0-") == "" or p["start"] + p["size"] <= end)]


def classify(t: dict) -> tuple[str, dict, list[dict]]:
    parts = t["partitions"]
    ud = userdata(t)
    stale = {p["node"] for p in leftovers(t)}
    after = [p for p in parts if p["start"] > ud["start"] and p["node"] not in stale]
    if not after:
        return "fresh", ud, after
    if [p["name"] for p in after] == list(OURS):
        return "installed", ud, after
    return "occupied", ud, after


# ------------------------------------------------------------ planning ----
def align_up(x: int, a: int) -> int:
    return -(-x // a) * a


def plan(t: dict, ud: dict, android_gib: int, storage_gib: int) -> dict:
    ss = t["sectorsize"]
    mib = MIB // ss                     # sectors per MiB
    last = int(t["lastlba"])
    ud_end = ud["start"] + android_gib * GIB // ss          # exclusive
    boot_start = align_up(ud_end, mib)
    ud_size = boot_start - ud["start"]                      # grow into the gap
    boot_size = BOOT_GIB * GIB // ss
    st_start = boot_start + boot_size
    st_size = storage_gib * GIB // ss
    home_start = st_start + st_size
    home_end = (last + 1) // mib * mib                       # exclusive, aligned
    home_size = home_end - home_start
    return {
        "sector_size": ss, "userdata_start": ud["start"], "userdata_size": ud_size,
        "new": [
            ("ROCKNIX", boot_start, boot_size, TYPE_FAT),
            ("STORAGE", st_start, st_size, TYPE_LINUX),
            ("HOME", home_start, home_size, TYPE_LINUX),
        ],
        "home_gib": home_size * ss / GIB,
    }


def max_android_gib(t: dict, ud: dict, storage_gib: int) -> int:
    ss = t["sectorsize"]
    room = (int(t["lastlba"]) + 1 - ud["start"]) * ss
    return int((room - (BOOT_GIB + storage_gib + MIN_HOME_GIB) * GIB - 2 * MIB) // GIB)


# ------------------------------------------------------------- safety -----
def booted_disk() -> str | None:
    """The whole disk that holds / (so we never repartition it)."""
    try:
        src = sh("findmnt", "-no", "SOURCE", "/").strip()
        pk = sh("lsblk", "-no", "PKNAME", src).strip().splitlines()
        return f"/dev/{pk[0]}" if pk and pk[0] else None
    except SystemExit:
        return None


def partition_node(disk: str, num: int) -> str:
    return f"{disk}p{num}" if disk[-1].isdigit() else f"{disk}{num}"


def part_num(disk: str, node: str) -> int:
    tail = node[len(disk):].lstrip("p")
    if not tail.isdigit():
        die(f"can't parse partition number from {node}")
    return int(tail)


def blank_head(disk: str, node: str) -> None:
    """Zero the first 8 MiB so Android sees no filesystem and makes a new one
    that fits the partition, instead of mounting one that doesn't."""
    with open(partition_node(disk, part_num(disk, node)), "r+b") as f:
        f.write(b"\0" * (8 * MIB))
        f.flush()
        os.fsync(f.fileno())


def blank_tail(disk: str, node: str, size_bytes: int) -> None:
    """Zero the last 8 MiB of the (resized) userdata as well: nothing of the
    old filesystem's end survives inside the new partition."""
    n = min(8 * MIB, size_bytes)
    with open(partition_node(disk, part_num(disk, node)), "r+b") as f:
        f.seek(size_bytes - n)
        f.write(b"\0" * n)
        f.flush()
        os.fsync(f.fileno())


def metadata_node(t: dict) -> str | None:
    """Android's "metadata" partition: on the same disk if the table has it,
    else by partition label (it may sit on another UFS LUN)."""
    for p in t["partitions"]:
        if p["name"] == "metadata":
            return p["node"]
    link = Path("/dev/disk/by-partlabel/metadata")
    return str(link.resolve()) if link.exists() else None


def drop_metadata_key(t: dict) -> None:
    """Remove /metadata/vold/metadata_encryption, the key userdata was
    encrypted with (what a factory reset does by wiping /metadata). Without
    it the old blocks are unreadable, and Android creates a new key when it
    formats userdata on its next boot. Best effort: a device without the
    partition or key just keeps today's behaviour."""
    node = metadata_node(t)
    if node is None:
        print("METADATA_KEY=no-metadata-partition")
        return
    mnt = tempfile.mkdtemp(prefix="ufs-metadata.")
    try:
        r = subprocess.run(["mount", "-o", "rw,noatime", node, mnt],
                           capture_output=True, text=True)
        if r.returncode != 0:
            print(f"METADATA_KEY=mount-failed ({r.stderr.strip()})")
            return
        try:
            key = Path(mnt) / "vold" / "metadata_encryption"
            if key.is_dir():
                shutil.rmtree(key)
                os.sync()
                print("METADATA_KEY=removed")
            else:
                print("METADATA_KEY=absent")
        finally:
            subprocess.run(["umount", mnt], capture_output=True)
    finally:
        os.rmdir(mnt)


def ensure_idle(disk: str) -> None:
    out = sh("lsblk", "-rno", "NAME,MOUNTPOINTS", disk)
    busy = [l for l in out.splitlines() if len(l.split()) > 1]
    if busy:
        die(f"{disk} has mounted partitions ({'; '.join(busy)}); unmount them first")
    for line in Path("/proc/swaps").read_text().splitlines()[1:]:
        if line.split()[0].startswith(disk):
            die(f"{line.split()[0]} is in use as swap")
    for holders in Path("/sys/class/block").glob(f"{Path(disk).name}*/holders"):
        if any(holders.iterdir()):
            die(f"{holders.parent.name} is held by {', '.join(os.listdir(holders))}")


# -------------------------------------------------------------- writing ---
def new_dump(disk: str, t: dict, p: dict) -> str:
    """sfdisk's own dump of the current table, userdata's size changed and
    our three partitions appended. Every other byte of each line is kept."""
    lines = sh("sfdisk", "--dump", disk).splitlines()
    stale = {q["node"] for q in leftovers(t)}
    out, seen_ud = [], False
    for line in lines:
        if line.startswith("/dev/") and line.split(":", 1)[0].strip() in stale:
            continue
        if line.startswith("/dev/") and 'name="userdata"' in line:
            parts = [s.strip() for s in line.split(":", 1)[1].split(",")]
            fixed = []
            for f in parts:
                if f.startswith("size="):
                    f = f"size={p['userdata_size']}"
                fixed.append(f)
            line = line.split(":", 1)[0] + ": " + ", ".join(fixed)
            seen_ud = True
        out.append(line)
    if not seen_ud:
        die("userdata line not found in sfdisk dump")
    for name, start, size, typ in p["new"]:
        out.append(f"start={start}, size={size}, type={typ}, name=\"{name}\"")
    return "\n".join(out) + "\n"


def reread(disk: str) -> None:
    subprocess.run(["blockdev", "--rereadpt", disk], capture_output=True)
    subprocess.run(["partx", "-u", disk], capture_output=True)
    subprocess.run(["udevadm", "settle", "-t", "10"], capture_output=True)
    time.sleep(0.5)


def kernel_view(disk: str) -> dict[int, tuple[int, int]]:
    """partition number -> (start, size) in 512-byte units, from sysfs."""
    view = {}
    base = Path("/sys/class/block") / Path(disk).name
    for d in base.glob(f"{Path(disk).name}*"):
        try:
            view[int((d / "partition").read_text())] = (
                int((d / "start").read_text()), int((d / "size").read_text()))
        except (OSError, ValueError):
            pass
    return view


def verify(disk: str, before: dict, p: dict) -> None:
    t = read_table(disk)
    ss = t["sectorsize"]
    stale = {q["node"] for q in leftovers(before)}
    old = {q["node"]: q for q in before["partitions"] if q["node"] not in stale}
    for q in t["partitions"]:
        o = old.get(q["node"])
        if o is None:
            continue
        for k in ("start", "type", "uuid", "name", "attrs"):
            if q.get(k) != o.get(k):
                die(f"{q['node']}: {k} changed ({o.get(k)} -> {q.get(k)})")
        want = p["userdata_size"] if q["name"] == "userdata" else o["size"]
        if q["size"] != want:
            die(f"{q['node']}: size {q['size']} != planned {want}")
    got = [(q["name"], q["start"], q["size"]) for q in t["partitions"]
           if q["name"] in OURS]
    want = [(n, s, z) for n, s, z, _ in p["new"]]
    if got != want:
        die(f"new partitions don't match the plan: {got} != {want}")
    kv = kernel_view(disk)
    scale = ss // 512
    for q in t["partitions"]:
        num = part_num(disk, q["node"])
        if kv.get(num) != (q["start"] * scale, q["size"] * scale):
            die(f"kernel still sees partition {num} as {kv.get(num)}; "
                f"table says {(q['start'] * scale, q['size'] * scale)}. Reboot to "
                "SD before formatting.")


# ------------------------------------------------------------ commands ----
def cmd_detect(a) -> None:
    t = read_table(a.disk)
    mode, ud, after = classify(t)
    ss = t["sectorsize"]
    total = (int(t["lastlba"]) + 1) * ss
    avail = (int(t["lastlba"]) + 1 - ud["start"]) * ss
    mx = max_android_gib(t, ud, a.storage_gib)
    if mode == "fresh" and mx < MIN_ANDROID_GIB:
        mode = "toosmall"
    print(f"DISK={a.disk}")
    print(f"MODE={mode}")
    print(f"SECTOR_SIZE={ss}")
    print(f"DISK_GIB={total / GIB:.1f}")
    print(f"USERDATA_NODE={ud['node']}")
    print(f"USERDATA_GIB={ud['size'] * ss / GIB:.1f}")
    print(f"AVAILABLE_GIB={avail / GIB:.1f}")
    print(f"ANDROID_MIN_GIB={MIN_ANDROID_GIB}")
    print(f"ANDROID_MAX_GIB={mx}")
    print(f"STORAGE_GIB={a.storage_gib}")
    print(f"OCCUPIED={','.join(p['name'] or p['node'] for p in after)}")
    print(f"LEFTOVERS={','.join(p['node'] for p in leftovers(t))}")
    for q in t["partitions"]:
        if q["name"] in OURS:
            print(f"NODE_{q['name']}={partition_node(a.disk, part_num(a.disk, q['node']))}")
    print(f"TABLE_FINGERPRINT={fingerprint(t)}")
    bd = booted_disk()
    print(f"BOOTED_FROM_TARGET={'yes' if bd == a.disk else 'no'}")


def cmd_apply(a) -> None:
    t = read_table(a.disk)
    if fingerprint(t) != a.expect:
        die("the partition table changed since it was inspected; run detect again")
    mode, ud, _ = classify(t)
    if mode != "fresh":
        die(f"layout is '{mode}', not fresh; nothing was written")
    if booted_disk() == a.disk:
        die(f"the running system is on {a.disk}; boot from the microSD card")
    mx = max_android_gib(t, ud, a.storage_gib)
    if not MIN_ANDROID_GIB <= a.android_gib <= mx:
        die(f"Android size must be {MIN_ANDROID_GIB}..{mx} GiB")
    if a.android_gib * GIB // t["sectorsize"] > ud["size"]:
        die("userdata can only shrink")
    p = plan(t, ud, a.android_gib, a.storage_gib)
    dump = new_dump(a.disk, t, p)
    print(f"userdata -> {p['userdata_size'] * p['sector_size'] / GIB:.2f} GiB")
    for name, start, size, _ in p["new"]:
        print(f"{name:8s} start {start} size {size} ({size * p['sector_size'] / GIB:.2f} GiB)")
    if a.dry_run:
        print("--- new table (dry run, not written) ---")
        print(dump, end="")
        return
    ensure_idle(a.disk)

    bdir = Path(a.backup_dir)
    bdir.mkdir(parents=True, exist_ok=True)
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    backup = bdir / f"ufs-gpt-{stamp}.sfdisk"
    backup.write_text(sh("sfdisk", "--dump", a.disk))
    (bdir / f"ufs-gpt-{stamp}.json").write_text(json.dumps(t, indent=1))
    os.sync()
    print(f"BACKUP={backup}")

    sh("sfdisk", "--no-reread", "--wipe", "never", "--wipe-partitions", "never",
       a.disk, inp=dump)
    reread(a.disk)
    verify(a.disk, t, p)
    blank_head(a.disk, ud["node"])
    blank_tail(a.disk, ud["node"], p["userdata_size"] * p["sector_size"])
    drop_metadata_key(t)
    print("OK")


def cmd_restore(a) -> None:
    text = Path(a.backup).read_text()
    t = read_table(a.disk)
    # Only restore a backup of this disk: same GPT id and the same partitions
    # before userdata.
    ids = [l.split(":", 1)[1].strip() for l in text.splitlines() if l.startswith("label-id:")]
    if not ids or ids[0].upper() != str(t.get("id", "")).upper():
        die("backup is from another disk (label-id differs)")
    cur = [l for l in sh("sfdisk", "--dump", a.disk).splitlines()
           if l.startswith("/dev/") and "userdata" not in l]
    old = [l for l in text.splitlines() if l.startswith("/dev/") and "userdata" not in l]
    if cur[:len(old)] != old:
        die("the Android partitions differ from the backup; not restoring")
    if a.dry_run:
        print(text, end="")
        return
    if booted_disk() == a.disk:
        die(f"the running system is on {a.disk}; boot from the microSD card")
    ensure_idle(a.disk)
    sh("sfdisk", "--no-reread", "--wipe", "never", "--wipe-partitions", "never",
       a.disk, inp=text)
    reread(a.disk)
    ud = [q for q in read_table(a.disk)["partitions"] if q["name"] == "userdata"][0]
    blank_head(a.disk, ud["node"])
    print("OK: original table restored; Android will format userdata on its next boot")


def main() -> None:
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    d = sub.add_parser("detect")
    ap_ = sub.add_parser("apply")
    r = sub.add_parser("restore")
    for s in (d, ap_, r):
        s.add_argument("--disk", default="/dev/sda")
    for s in (d, ap_):
        s.add_argument("--storage-gib", type=int, default=DEFAULT_STORAGE_GIB)
    ap_.add_argument("--android-gib", type=int, required=True)
    ap_.add_argument("--expect", required=True)
    ap_.add_argument("--backup-dir", required=True)
    ap_.add_argument("--dry-run", action="store_true")
    r.add_argument("--backup", required=True)
    r.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()
    if os.geteuid() != 0 and not getattr(a, "dry_run", False) and a.cmd != "detect":
        die("run as root")
    {"detect": cmd_detect, "apply": cmd_apply, "restore": cmd_restore}[a.cmd](a)


if __name__ == "__main__":
    main()
