# Install to internal storage

Moves the SteamOS you're running from the microSD card onto the internal
storage, next to Android. For the KONKR Pocket FIT (and the AYANEO Pocket S2,
untested) with ROCKNIX ABL 1.1.8 or newer, and the Snapdragon 8 Gen 2
handhelds (Retroid Pocket 6; AYN Thor untested) with ROCKNIX ABL. The Retroid
Pocket 6 and the KONKR Pocket FIT have been running from internal storage for
several days.

> **This erases Android's user data** (apps, photos, accounts) and changes the
> internal partition table. Android itself stays and sets itself up again.
> Read [DISCLAIMER.md](DISCLAIMER.md) first.

## How to

Boot SteamOS from the SD card, then either open **Easy UFS Installer** in
Desktop Mode, or in a terminal:

```bash
sudo install-masios-to-internal.sh --dry-run     # shows the plan, writes nothing
sudo install-masios-to-internal.sh               # asks for the Android size
```

The installer window shows each step with a progress bar and the live log,
and won't close while it's installing. It ends with a pop-up: the next steps,
or on failure the error and whether internal storage was already changed. The
log is saved as `~/easy-ufs-install-<date>.log`; after a failure a copy goes
on the desktop.

When it's done: power off and take the SD card out. In the ABL menu (hold
Volume Down at power-on) set **Boot source** to **Internal**, then boot Linux.
With Boot source left on SD, the ABL stops at "no volumes match boot source".
Android is still in the same menu.

You pick how much of the SD's `/home` comes along:

| `--home` | What you get |
|---|---|
| `all` (default) | everything, installed games included |
| `essentials` | Steam login, settings, saves and plugins; games get downloaded again |
| `none` | a clean start |

## Reinstall versus update

Use **SteamOS Update** for an existing installation. The installer’s `--resume` option resumes a failed initial deployment by formatting boot, system and HOME again. It erases internal Linux games, saves and account data before copying the selected SD data. It is not an in-place update or a data-preserving repair.

## Layout

```
before:  ... | userdata ─────────────────────────────────────────── |
after:   ... | userdata (you pick) | ROCKNIX 2G | STORAGE 20G | HOME |
```

- Nothing before `userdata` is touched. `userdata` keeps its start, type, GUID
  and attributes; only its end moves.
- `ROCKNIX` (FAT32) holds the `KERNEL` the ABL boots, `STORAGE` is `/`,
  `HOME` is `/home`. Everything is mounted by partition name, so the internal
  install doesn't depend on the SD card.

## Safety

- Nothing is written until you type `ERASE` (or press Install in the app),
  and only if the partition table still matches what was shown to you.
- It refuses to run from internal storage, if anything on `userdata` is in
  use, or if anything other than its own partitions follows `userdata`
  (remove those with **UNINSTALL CFW** in the ABL menu first).
- The whole new table is written in one go and checked against the plan,
  both on disk and in the kernel's view, before anything is formatted.
- The KERNEL is the SD card's own, with only `root=` changed and its header
  checksum rebuilt, then checked again.

## Giving the space back to Android

The original partition table is saved to the SD card (`/boot/ufs-backup/`,
with a copy in `/home/.ufs-backup/` on the internal install). Boot the SD card
and run:

```bash
sudo ufs-partition.py restore --backup /boot/ufs-backup/ufs-gpt-<date>.sfdisk
```

`userdata` gets its full size back and Android sets itself up again on its
next boot. **UNINSTALL CFW** in the ABL menu also removes the Linux partitions.

## If internal Linux doesn't boot

Boot the SD card and run `sudo ufs-diagnose.sh`, then
`sudo ufs-fix-internal-boot.sh` (rewrites the KERNEL and fstab), or
`sudo install-masios-to-internal.sh --resume` to copy the system again onto the
existing partitions.

## Files

| File | What it does |
|---|---|
| `install-masios-to-internal.sh` | the installer |
| `ufs-partition.py` | reads, repartitions and restores the internal partition table |
| `ufs-bootimg.py` | reads, checks and retargets the ABL `KERNEL` |
| `easy-ufs-installer.py` | the Desktop Mode app |
| `ufs-probe-sizes.sh` | sizes for the app |
| `ufs-diagnose.sh`, `ufs-fix-internal-boot.sh` | repair tools |

GPL-2.0-or-later, see [LICENSE](LICENSE).
