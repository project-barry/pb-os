#!/usr/bin/env python3
"""Like ../thor-ch13726a/swapkernel.py, but also replaces the KPF DTB among
the appended DTBs (matched by its model string). Keeps the ramdisk, cmdline,
other header fields and the other DTBs.

usage: swapkernel-dtb.py OLD_KERNEL NEW_IMAGE NEW_THOR_DTB OUT"""
import hashlib, struct, subprocess, sys, zlib

old_path, image_path, dtb_path, out_path = sys.argv[1:5]
MODEL = b"KONKR Pocket FIT"
old = open(old_path, "rb").read()
assert old[:8] == b"ANDROID!"
ksize, kaddr, rsize, raddr, ssize, saddr, tags, page, hver, osver = struct.unpack_from("<10I", old, 8)
assert ssize == 0 and hver == 0, (ssize, hver)
pad = lambda n: (n + page - 1) // page * page
kernel = old[page:page + ksize]
ramdisk = old[page + pad(ksize):page + pad(ksize) + rsize]
d = zlib.decompressobj(16 + zlib.MAX_WBITS)
d.decompress(kernel)
blob = d.unused_data

dtbs, off = [], 0
while off < len(blob):
    assert blob[off:off + 4] == b"\xd0\x0d\xfe\xed", f"no DTB magic at {off}"
    size = struct.unpack_from(">I", blob, off + 4)[0]
    dtbs.append(blob[off:off + size])
    off += size
new_dtb = open(dtb_path, "rb").read()
assert new_dtb[:4] == b"\xd0\x0d\xfe\xed" and MODEL in new_dtb
hits = [i for i, b in enumerate(dtbs) if MODEL in b]
assert len(hits) == 1, f"KPF DTB matches: {hits}"
print(f"{len(dtbs)} DTBs, Thor is #{hits[0]} ({len(dtbs[hits[0]])} -> {len(new_dtb)} bytes)")
dtbs[hits[0]] = new_dtb

new_kernel = subprocess.run(["gzip", "-9", "-n", "-c", image_path],
                            capture_output=True, check=True).stdout + b"".join(dtbs)
sha = hashlib.sha1()
for part in (new_kernel, ramdisk, b""):
    sha.update(part); sha.update(struct.pack("<I", len(part)))
hdr = bytearray(old[:page])
struct.pack_into("<I", hdr, 8, len(new_kernel))
id_off = 8 + 40 + 16 + 512
hdr[id_off:id_off + 32] = sha.digest().ljust(32, b"\0")
out = bytes(hdr) + new_kernel + b"\0" * (pad(len(new_kernel)) - len(new_kernel)) \
      + ramdisk + b"\0" * (pad(rsize) - rsize)
open(out_path, "wb").write(out)
print(f"ramdisk {rsize} kept; image {len(old)} -> {len(out)}")
