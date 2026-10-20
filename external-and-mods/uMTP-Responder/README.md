# uMTP-Responder (MTP over USB)

[uMTP-Responder](https://github.com/viveris/uMTP-Responder) by Viveris
Technologies (GPL-3.0): a userspace MTP responder for Linux USB gadgets. It
gives PB-OS devices MTP, so a PC can browse and copy the user's files over
the USB-C cable with no drive letter, no credentials and nothing to install
on either side — the way a phone does it.

Upstream at the commit in [REF](REF) (v1.9.1, 2026-10-11), built by
`scripts/build-umtprd-in-rootfs.sh` with the one patch in `patches/`.

## Patches

- `0001` refuses new and renamed names that start with a dot while
  `show_hidden_files` is off. Hiding dot files only stops a PC seeing them:
  upstream keeps a leading dot on names a host sends, so with Home shared
  read-write a PC could still overwrite `~/.bashrc` or `~/.profile` by name.

## Why MTP and not a disk

A `mass_storage` gadget hands the host a block device, which means
unmounting the filesystem on the device first: the user cannot keep playing
while the PC has the card. MTP is file-by-file over the running system, so
`/home` stays mounted and in use.

## Build options that matter

| Option | Why |
|---|---|
| `CONFIG_USB_SS_SUPPORT=1` | **Required.** Upstream builds Full and High speed descriptors only (`inc/buildconf.h`). On a SuperSpeed port the gadget then enumerates and sticks at UDC state `addressed` forever, because the composite layer has no SuperSpeed config to hand the host. |
| `SYSTEMD=1` | `sd_notify(READY=1)` right after the FunctionFS descriptors are written, so `pbos-mtp.service` is `Type=notify` and the UDC is bound in `ExecStartPost` at exactly the right moment. |
| `USE_SYSLOG=1` | Its log goes to the journal under the unit instead of a file. |

`CONFIG_USB_NON_BLOCKING_WRITE` is **not** used: despite the name it is a
`poll()` loop for responsive cancellation, not pipelining, and it adds a
syscall per transfer.

## Throughput

Measured on the AYN Thor (SM8550, internal UFS reading at 1.4 GB/s) over a
SuperSpeed+ link, with the buffer sizes in `umtprd.conf`, and verified
byte-identical over a 13.8 GB round trip:

| Direction | Speed |
|---|---|
| PC -> device | 364 MB/s |
| device -> PC | 118 MB/s |

Reads are the slower side and stay that way. The IN endpoint only moves data
while userspace has a request queued, and the responder does one synchronous
`write()` per transfer, so the wire idles between them; the OUT direction is
absorbed by the controller whether or not the responder is ready. Fixing it
means several outstanding requests (FunctionFS AIO, `io_submit`) upstream.
Transfer size still matters a lot, and that is what `umtprd.conf` tunes:
reads measured 70 / 97 / 108 / 118 / 116 MB/s at 64 KB / 256 KB / 1 MB /
2 MB / 4 MB per transfer, against upstream's 8 KB default.

## Which folders the PC sees

The PB-OS tab's **USB File Transfer** section adds and removes them. The list
lives in `/var/lib/pbos-mtp/shares.json`, and `pbos-mtp storages` turns it
into the `storage` lines of the generated config; without that file it is the
home folder and the Steam folder. `pbos-mtp shares` prints the effective list
as JSON, which is how pbos-qamd reads it back — the defaults and the checking
are written down once, in the helper, so the two cannot drift.

Unusable entries are dropped rather than allowed to break the config (gone,
not a folder, a duplicate, a quote or a newline in the path), labels are made
unique because an MTP store name is what the PC shows, and the list is cut to
umtprd's 16 stores (`MAX_STORAGE_NB`).

umtprd reads its stores once at startup, so changing the list restarts the
service: the host sees the device leave and come back with the new folders.
Its `lock`/`unlock` IPC can take a store away and bring it back live
(`MTP_EVENT_STORE_REMOVED` / `_ADDED`), but only for stores already declared
in the config, so it is no help for a folder the user has just picked.

## The FunctionFS instance may not be called `mtp`

`pbos-mtp` creates `functions/ffs.umtp` and mounts `-t functionfs umtp`. The
name `mtp` is unusable: on the PB-OS kernel `mount -t functionfs mtp` fails
with ENOENT from a fresh instance, while `umtp`, `adb`, `mtp2` and others
mount from an identical sequence. Nothing in the kernel claims the name (no
MTP gadget function is built in, nothing in `kallsyms`), so the cause is
unknown — but it reproduces every time.
