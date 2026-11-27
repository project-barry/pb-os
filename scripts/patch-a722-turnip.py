#!/usr/bin/env python3
"""Give Turnip's Adreno 722 entry its second CCU (Retroid Pocket Duo).

usage: patch-a722-turnip.py <libvulkan_freedreno.so>...

Valve's Mesa describes the Adreno 722 (Retroid Pocket Duo) with num_ccu = 1;
the chip has 2 CCUs and 2 SPs. Private memory is sized per SP from num_ccu,
so with 1 the second SP runs off the end of its buffer and shaders that spill
fault. Upstream Mesa fixed the table in 8c589b897b3 (in 26.3.0 final).

This changes one 32-bit field, num_ccu 1 -> 2, inside the A722 fd_dev_info
only. Each GPU has its own entry in the library, so the Adreno 740/750 and
every other GPU read exactly what they read before: the library is shared by
all devices' images, the change reaches only the Duo.

The entry is found by its exact field values (chip generation through
num_ccu). The library is patched only when that pattern occurs exactly once.
Already patched (a reused rootfs, or a Mesa that has the fix with the same
layout): left as is. Anything else (no match, several matches, a different
layout in a newer Mesa): a WARNING, the library is left untouched and the
build goes on. The exit status is 0 in every case except bad usage.
"""
import hashlib
import os
import struct
import sys

# chip, tile_align_w/h, gmem_align_w/h, tile_max_w/h, vsc_draw_strm_pitch_align,
# cs_shared_mem_size, wave_granularity, highest_bank_bit, ubwc_swizzle,
# macrotile_mode, fibers_per_sp, threadsize_base, max_waves, line_buffer size,
# num_ccu (last field).
HEAD = (7, 0x40, 0x10, 0x10, 4, 0x400, 0x400, 0x20, 0x8000, 2, 15, 6, 1,
        0x1000, 0x40, 0x10, 0)
OLD = struct.pack("<18I", *HEAD, 1)
NEW = struct.pack("<18I", *HEAD, 2)
NUM_CCU_OFF = 17 * 4


def count(data, pat):
    n, i = 0, data.find(pat)
    while i >= 0:
        n += 1
        i = data.find(pat, i + 4)
    return n


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def patch(path):
    tag = "patch-a722-turnip"
    if not os.path.isfile(path):
        print(f"{tag}: WARNING: {path} not found, nothing patched")
        return
    with open(path, "rb") as f:
        data = bytearray(f.read())
    old_n, new_n = count(data, OLD), count(data, NEW)
    if old_n == 0 and new_n == 1:
        print(f"{tag}: {path}: A722 num_ccu is already 2, left as is "
              f"(sha256 {sha256(data)})")
        return
    if old_n != 1 or new_n != 0:
        print(f"{tag}: WARNING: {path}: A722 entry found {old_n} time(s) with "
              f"num_ccu 1 and {new_n} with 2 (need exactly one), library "
              f"left untouched (sha256 {sha256(data)}). Check whether this "
              f"Mesa has upstream 8c589b897b3 or a changed fd_dev_info layout.")
        return
    before = sha256(data)
    off = data.find(OLD) + NUM_CCU_OFF
    struct.pack_into("<I", data, off, 2)
    st = os.stat(path)
    tmp = path + ".a722.tmp"
    with open(tmp, "wb") as f:
        f.write(data)
    os.chmod(tmp, st.st_mode & 0o7777)
    os.replace(tmp, path)
    print(f"{tag}: {path}: A722 num_ccu 1 -> 2 at file offset {off:#x}")
    print(f"{tag}:   sha256 before {before}")
    print(f"{tag}:   sha256 after  {sha256(data)}")


def main():
    if len(sys.argv) < 2:
        print(__doc__.strip().splitlines()[2], file=sys.stderr)
        return 2
    for path in sys.argv[1:]:
        patch(path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
