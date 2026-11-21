#!/usr/bin/env python3
"""Add an EFI-stub boot entry, made from KERNEL, to a PB-OS BOOT partition.

OpenABL newer than 1.2 no longer boots Android boot images (\\KERNEL). It
starts Linux only through its EFI stub, from a Boot Loader Specification
entry (\\loader\\entries\\*.conf), with the device tree its *Device model*
menu picks from \\dtbs\\qcom\\*.dtb. OpenABL 1.2 reads those entries too,
before \\KERNEL, so one card can carry both; 1.2's *Boot entries* menu still
offers \\KERNEL by hand.

Everything comes from the header v0 KERNEL the image builder already wrote
(with the card's own root PARTUUID patched in):
  kernel section = gzip(Image) + the appended DTBs -> pbos/Image.gz, dtbs/qcom/
  ramdisk                                          -> pbos/initrd.gz
  cmdline                                          -> the entry's options

Usage: bls-from-bootimg.py <BOOT>/KERNEL <BOOT mount>
"""
from __future__ import annotations

import re
import struct
import sys
import zlib
from pathlib import Path

ENTRY = "pbos.conf"
SUBDIR = "pbos"
FDT_MAGIC = 0xD00DFEED


def split_bootimg(data: bytes) -> tuple[bytes, bytes, str]:
    if data[:8] != b"ANDROID!":
        raise SystemExit("not an Android boot image")
    kernel_size, _, ramdisk_size = struct.unpack_from("<III", data, 8)
    page = struct.unpack_from("<I", data, 36)[0]
    version = struct.unpack_from("<I", data, 40)[0]
    if version != 0:
        raise SystemExit(f"header v{version}; only v0 (OpenABL's KERNEL) is handled")
    cmdline = data[64:64 + 512].split(b"\0", 1)[0].decode("ascii")
    extra = data[608:608 + 1024].split(b"\0", 1)[0].decode("ascii")
    if extra:
        cmdline = f"{cmdline} {extra}"

    def up(n: int) -> int:
        return (n + page - 1) // page * page

    k_off = page
    r_off = k_off + up(kernel_size)
    kernel = data[k_off:k_off + kernel_size]
    ramdisk = data[r_off:r_off + ramdisk_size]
    return kernel, ramdisk, cmdline


def split_kernel(kernel: bytes) -> tuple[bytes, list[bytes]]:
    """gzip(Image) + DTBs -> (the gzip stream, [DTB, ...])."""
    d = zlib.decompressobj(16 + zlib.MAX_WBITS)
    image = d.decompress(kernel)
    if not d.eof:
        raise SystemExit("kernel is not one complete gzip stream")
    if image[0x38:0x3C] != b"ARMd" or image[:2] != b"MZ":
        raise SystemExit("Image has no EFI stub (MZ/ARMd header missing)")
    rest = d.unused_data
    gz = kernel[:len(kernel) - len(rest)]
    dtbs = []
    while rest:
        magic, total = struct.unpack_from(">II", rest, 0)
        if magic != FDT_MAGIC:
            raise SystemExit(f"junk after DTB {len(dtbs)}")
        dtbs.append(rest[:total])
        rest = rest[total:]
    if not dtbs:
        raise SystemExit("KERNEL has no appended DTBs")
    return gz, dtbs


def root_props(dtb: bytes) -> dict[str, bytes]:
    """The root node's properties (enough to name the file)."""
    off_struct, off_strings = struct.unpack_from(">II", dtb, 8)
    pos, depth, props = off_struct, 0, {}
    while True:
        tok = struct.unpack_from(">I", dtb, pos)[0]
        pos += 4
        if tok == 1:  # BEGIN_NODE
            end = dtb.index(b"\0", pos)
            pos = (end + 4) & ~3
            depth += 1
            if depth > 1:
                return props
        elif tok == 3:  # PROP
            length, nameoff = struct.unpack_from(">II", dtb, pos)
            pos += 8
            name_end = dtb.index(b"\0", off_strings + nameoff)
            name = dtb[off_strings + nameoff:name_end].decode()
            props[name] = dtb[pos:pos + length]
            pos = (pos + length + 3) & ~3
        elif tok == 4:  # NOP
            continue
        else:  # END_NODE / END
            return props


def dtb_name(dtb: bytes, taken: set[str]) -> tuple[str, str]:
    props = root_props(dtb)
    model = props.get("model", b"").split(b"\0")[0].decode(errors="replace")
    compat = props.get("compatible", b"").split(b"\0")[0].decode(errors="replace")
    stem = re.sub(r"[^a-z0-9]+", "-", (compat or model or "board").lower()).strip("-")
    name, n = f"{stem}.dtb", 2
    while name in taken:
        name, n = f"{stem}-{n}.dtb", n + 1
    taken.add(name)
    return name, model


def main() -> None:
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    kernel_img, boot = Path(sys.argv[1]), Path(sys.argv[2])
    kernel, ramdisk, cmdline = split_bootimg(kernel_img.read_bytes())
    gz, dtbs = split_kernel(kernel)
    if "root=" not in cmdline:
        raise SystemExit("KERNEL's cmdline has no root=")

    (boot / SUBDIR).mkdir(parents=True, exist_ok=True)
    (boot / SUBDIR / "Image.gz").write_bytes(gz)
    (boot / SUBDIR / "initrd.gz").write_bytes(ramdisk)

    # Only the DTBs appended to KERNEL (the OpenABL devices). Stale ones from
    # an older kernel would show up in the Device model menu, so replace all.
    dtdir = boot / "dtbs" / "qcom"
    dtdir.mkdir(parents=True, exist_ok=True)
    for old in dtdir.glob("*.dtb"):
        old.unlink()
    taken: set[str] = set()
    for dtb in dtbs:
        name, model = dtb_name(dtb, taken)
        (dtdir / name).write_bytes(dtb)
        print(f"  dtbs/qcom/{name}: {model}")

    # No devicetree key: the Device model menu's choice is used.
    entries = boot / "loader" / "entries"
    entries.mkdir(parents=True, exist_ok=True)
    (entries / ENTRY).write_text(
        "title PB-OS\n"
        "sort-key pbos\n"
        "architecture aa64\n"
        f"linux /{SUBDIR}/Image.gz\n"
        f"initrd /{SUBDIR}/initrd.gz\n"
        f"options {cmdline}\n"
    )
    print(f"  loader/entries/{ENTRY}: {len(gz) >> 20} MiB kernel, "
          f"{len(ramdisk) >> 10} KiB initrd, {len(dtbs)} DTBs")


if __name__ == "__main__":
    main()
