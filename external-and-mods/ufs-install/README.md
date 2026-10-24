# Install to internal storage

Moves the SteamOS you're running from the microSD card onto the internal
storage, next to Android. For the KONKR Pocket FIT (and the AYANEO Pocket S2,
untested) and the Snapdragon 8 Gen 2 handhelds (Retroid Pocket 6; AYN Thor
untested) with [OpenABL](https://github.com/OpenHandheldCollective/OpenABL)
(`openabl/README` on the SD card). The Retroid
Pocket 6 and the KONKR Pocket FIT have been running from internal storage for
several days.

> **This erases Android's user data** (apps, photos, accounts), the way a
> factory reset does: the old data's encryption key is removed too, so it
> cannot be read again. It changes the
> internal partition table. Android itself stays and sets itself up again.
> Read [DISCLAIMER.md](DISCLAIMER.md) first.

## How to

Boot SteamOS from the SD card, then either open Quick Access → Decky →
**PB-OS Utils**, Install tab (Game Mode, no password needed), or **Easy UFS
Installer** in Desktop Mode, or in a terminal:

```bash
sudo install-masios-to-internal.sh --dry-run     # shows the plan, writes nothing
sudo install-masios-to-internal.sh               # asks for the Android size
```

PB-OS Utils runs the installer as a background service with sleep held off,
so it carries on with Quick Access closed and shows a toast when it ends.
The installer window shows each step with a progress bar and the live log,
and won't close while it's installing. It ends with a pop-up: the next steps,
or on failure the error and whether internal storage was already changed. The
log is saved as `~/easy-ufs-install-<date>.log`; after a failure a copy goes
on the desktop.

When it's done: power off and take the SD card out. In the OpenABL menu
(hold Volume Down at power-on) set **Boot source** to **Internal** (or
**Auto**, which starts internal storage first) and **Boot mode** to **Linux**,
then **Start**. With Boot source left on **SD card** and no card in, nothing
boots and the menu opens to show why. Android: hold Volume Up at power-on, or
set **Boot mode** to **Android**.

> **Never pick Uninstall CFW** in the OpenABL menu unless you want PB-OS
> gone: it erases the internal install (`ROCKNIX`, `STORAGE`, `HOME`) **and
> Android's user data**.

You pick how much of the SD's `/home` comes along:

| `--home` | What you get |
|---|---|
| `all` (default) | everything, installed games included |
| `essentials` | Steam login, settings, saves and plugins; games get downloaded again |
| `none` | a clean start |

## Back to a microSD card

From the internal install, PB-OS Utils' Install tab (or
`sudo copy-to-sd.sh`) copies the running SteamOS to a microSD card, laid out
like a flashed image (`BOOT` FAT with the `KERNEL`, `root`, `home` filling the
card). The card is erased; internal storage is only read. It brings the same
`/home` choices as above, plus the partition-table backup in `/boot/ufs-backup`,
so the card can later give the internal space back to Android (below).

```bash
sudo copy-to-sd.sh --probe       # the card it would use, as KEY=VALUE
sudo copy-to-sd.sh --dry-run     # the plan, writes nothing
sudo copy-to-sd.sh --home essentials
```

It unmounts the card the way SteamOS's own Format does, holding the
auto-mounter's locks so nothing remounts mid-copy, and stops if a game still
uses the card. Then set **Boot source** to **SD card** in the OpenABL menu
(with **Auto**, internal storage starts first).

## Installing over PB-OS or ArmadaOS

From the SD card it also goes over a Linux install already on internal
storage, in that install's space. Android's size, apps and data are not
changed, and nothing before `userdata` is written. PB-OS's Install tab and
the Easy UFS Installer offer it when their check finds one; from a terminal it
is `--replace`:

- **PB-OS** (`ROCKNIX` / `STORAGE` / `HOME`): the partitions are reused and
  the table isn't written. `--home keep` (the default in both apps, **Keep
  internal**) formats only `ROCKNIX` and `STORAGE` and leaves `HOME` (games,
  saves, Steam login) as it is; only the Android apps files come from the
  card. Any other `--home` erases `HOME` too. The partitions must be at least
  256 MiB, 18 GiB and 16 GiB.
- **ArmadaOS** (`ARMADA` / `ARMADA_BOOT` / `ARMADA_ROOT`): its space becomes
  `ROCKNIX` 2G, `STORAGE` 20G and `HOME` the rest, in the same table slots
  (`ufs-partition.py relayout`, the old table saved to `/boot/ufs-backup/`
  first). Everything on ArmadaOS is erased. It needs at least 38 GiB.

This is the same as PB-OS Installer's "Install Over Internal Linux". To keep
an internal PB-OS and only update it, use **Software Update** there instead.
`--resume` is `--replace` for PB-OS only, kept for older notes.

## Layout

```
before:  ... | userdata ─────────────────────────────────────────── |
after:   ... | userdata (you pick) | ROCKNIX 2G | STORAGE 20G | HOME |
```

- Nothing before `userdata` is touched. `userdata` keeps its start, type, GUID
  and attributes; only its end moves.
- `ROCKNIX` (FAT32) holds the `KERNEL` OpenABL boots, `STORAGE` is `/`,
  `HOME` is `/home`. Everything is mounted by partition name, so the internal
  install doesn't depend on the SD card.

## Safety

- Nothing is written until you type `ERASE` (or press Install in the app),
  and only if the partition table still matches what was shown to you.
- It refuses to run from internal storage, if anything on `userdata` is in
  use, or if anything other than PB-OS's or ArmadaOS's partitions follows
  `userdata` (remove those with **Uninstall CFW** in the OpenABL menu first;
  it also erases Android's user data).
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
next boot. **Uninstall CFW** in the OpenABL menu also removes the Linux
partitions, and erases Android's user data as well.

## If internal Linux doesn't boot

Boot the SD card and run `sudo ufs-diagnose.sh`, then
`sudo ufs-fix-internal-boot.sh` (rewrites the KERNEL and fstab), or
`sudo install-masios-to-internal.sh --replace --home keep` to copy the system
again onto the existing partitions, keeping HOME.

## Files

| File | What it does |
|---|---|
| `install-masios-to-internal.sh` | the installer |
| `copy-to-sd.sh` | the other direction: internal install to a microSD card |
| `home-copy.sh` | what of `/home` both bring (all, essentials, none) |
| `ufs-partition.py` | reads, repartitions and restores the internal partition table |
| `ufs-bootimg.py` | reads, checks and retargets the ABL `KERNEL` |
| `easy-ufs-installer.py` | the Desktop Mode app |
| `ufs-probe-sizes.sh` | sizes for the app |
| `ufs-diagnose.sh`, `ufs-fix-internal-boot.sh` | repair tools |

GPL-2.0-or-later, see [LICENSE](LICENSE).
