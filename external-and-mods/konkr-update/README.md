# Offline SteamOS updates

Installs a new pb-os version over the current one without reflashing. Games, saves, Steam accounts, Wi-Fi, Decky settings and your own Decky plugins stay.

There are two kinds of package. A **full** package carries the whole system (about 4 GB) and installs over any earlier version. A **delta** carries only what changed, usually tens to hundreds of MB, and is quick to download and install.

Releases come in two kinds as well. A **feature release** (e.g. `alpha-v0.4`) ships full packages only. Each **patch release** after it (`alpha-v0.4.1`, `alpha-v0.4.2`, ...) ships one delta that goes from the feature release to the patch and also installs on every patch in between, so a device on the feature release or on any of its patches gets there in one step. A device on an older feature release first takes the newest feature release's full package, then its newest patch.

The updater only offers versions newer than the installed one, so a release without a package for the device never leads to a downgrade.

## How it works

1. In Game Mode, open Quick Access → Decky → **PB-OS Update** ([plugin](../Decky/pbos-update/), on every image). It looks for the newest pb-os release with a package for this device and shows a toast when one comes out. Press **Download and install**; the download carries on with the menu closed, can be paused, and resumes where it stopped. Then **Restart and install**.
   Desktop Mode has the same in the **SteamOS Update** app, which also installs a package from a file (paste its SHA-256 from the release's `SHA256SUMS`).
   Without internet, PB-OS Update takes the update from a microSD card or USB drive instead (below).
2. Preparation runs before the restart, while Steam keeps running, with its progress in PB-OS Update. It checks the archive and works out exactly what changes: for a full package it compares each file's header with the installed system and unpacks only what differs (a delta carries only what changed anyway). It checks those files against the manifest, backs up the `/usr` and `/opt` files it will replace, checks free space and copies a private recovery runtime to HOME. It saves the current boot image and installs the new one, which carries the recovery hook.
3. On restart, the initramfs mounts the same root, boot and HOME filesystems, verifies their UUIDs, and runs the package's updater. It shows **Installing update** with a progress bar and pulsing dots on the screen, backs up the `/etc` files it will replace, then writes and deletes only the planned files. The restart step takes about as long as the change is big, not as long as the system is big.
4. It installs the Decky plugins the package carries, removes the device plugins it lists for removal, checks every file it wrote, then boots SteamOS. Accounts, passwords, machine-id, hostname, fstab, SSH keys and network connections in `/etc` are kept.
5. An interrupted apply or rollback is recovered on the next boot. A failed apply restores the backup and reboots into the previous kernel. If recovery itself cannot finish, normal boot stops and the log is left on HOME.

No partition table is changed and no filesystem is formatted. SD and internal installs use the same process. HOME needs room for the download plus about twice the size of the files that change. The root partition must also fit the new system.

A device knows its version (`/usr/share/pb-os/version`) and its feature release (`/usr/share/pb-os/base`; images from before have none and count as their own). A delta installs only on the versions its manifest lists. Updaters from before deltas (alpha v0.3 and v0.3a) don't see them and take the full package.

The apply step runs the updater that comes in the package, so fixes to it reach devices with the update itself.

## Cleaning up

An update needs its download, unpacked files and backup only until it has installed or rolled back. Each time Game Mode starts, PB-OS Update deletes them for every finished update, including those installed by older versions of the updater; preparing a new update does the same first. A finished update keeps only its records and logs (`transaction.json`, `state.json`, `recovery.log`, `failure.txt`). The folder of a preparation that never finished (power lost) goes, as do downloads of versions older than the installed one. A pending update, and one that stopped halfway, are never touched. From Konsole: `sudo python3 /usr/share/konkr-update/konkr-update.py cleanup`.

A device takes this with the first update whose updater has it: that update installs the new PB-OS Update, which cleans up after the restart.

## Updating from a microSD card or USB drive

For a device without internet. On a computer, download from the release page:

- the update for your device: the full package (`pb-os-<tag>-<device>.update.tar.gz.001`, `.002`, ...) or, for a patch of the feature release you have, the delta (`pb-os-<tag>-<device>.from-<that feature release>.delta.tar.gz.001`, ...). The joined `.tar.gz` works too;
- `SHA256SUMS` and `SHA256SUMS.sig`.

`<device>` is `rp6` for the Retroid Pocket 6 and Nova, `pocketfit` for the KONKR Pocket FIT and AYANEO Pocket S2, and `thor` for the AYN Thor.

Copy them, unchanged, to the top folder of a microSD card or USB drive formatted FAT32, exFAT, NTFS or ext4. The parts are each under 2 GB, so FAT32 works. Put the card or drive in the device. When pb-os runs from internal storage, use a microSD card or a USB drive; when it runs from a microSD card, use a USB drive.

PB-OS Update notices the drive and shows a toast. Open it and press **Install from the drive**. It copies the update to HOME (keep the drive in until it says you can take it out), then prepares it like a download. Then **Restart and install**.

Like a download, it installs only when `SHA256SUMS.sig` is from the pb-os release key and the package matches its line in `SHA256SUMS`, so the files from one release must stay together. PB-OS Update says what is missing when they don't match. It mounts the drive read-only while it looks and copies, and never looks at the disk pb-os runs from.

From Konsole: `sudo python3 /usr/share/konkr-update/konkr-update.py local-check` shows what it finds, and `local-update` installs it.

## From Konsole

`sudo python3 /usr/share/konkr-update/konkr-update.py update`, then restart. `check` (no sudo) prints what is installed and what the releases offer.

## Releasing an update

One package per image, and one image per SoC: Pocket FIT / Pocket S2 (`pocketfit`) and SM8550, i.e. Retroid Pocket 6, Retroid Pocket Nova and AYN Thor (`sm8550`). A package only installs on the DTB models in its manifest.

Older channel names, for devices whose updater predates them (each adds hard links of the same parts, plus their `.sha256` lines; the updater takes either name):
- `--rp6-bridge`: also names every package (full and deltas) `pb-os-<tag>-rp6...`. Until the rename (alpha-v0.5.1 and the test builds after it), SM8550 packages were named `rp6`, and those updaters look for nothing else. Add it to each SM8550 release until devices have an updater that reads `sm8550`.
- `--thor-bridge`: also names the full package `pb-os-<tag>-thor.update.tar.gz`, for Thors still on the separate Thor image of releases before alpha-v0.5.1.

1. Build the image with `PB_OS_VERSION=<release tag>`. For a patch release, also `PB_OS_BASE=<its feature release>` (e.g. `PB_OS_VERSION=alpha-v0.4.2 PB_OS_BASE=alpha-v0.4`). The image records them in `/usr/share/pb-os/version` and `/usr/share/pb-os/base`. The updater offers a release whose tag is newer than the installed version.
2. From the same rootfs, with the same SoC:
   - **Feature release:** the full package.
     ```
     sudo scripts/build-update-package.py --soc sm8550 \
       --rootfs /work/rootfs-... --kernel <kernel output>/boot/KERNEL --release <dir>
     ```
     This writes `pb-os-<tag>-<image>.update.tar.gz.001`, `.002`, ... (under GitHub's 2 GiB limit).
   - **Patch release:** the delta, from the feature release's state file and the state file of every patch since:
     ```
     sudo scripts/build-update-package.py --soc sm8550 \
       --rootfs /work/rootfs-... --kernel <kernel output>/boot/KERNEL --release <dir> \
       --base-state pb-os-alpha-v0.4-<image>.state.json.gz \
       --patch-state pb-os-alpha-v0.4.1-<image>.state.json.gz
     ```
     This writes `pb-os-<tag>-<image>.from-<feature release>.delta.tar.gz.001`, ... and no full package. It carries every file that differs from the feature release or from any of those patches, and deletes what any of them had that this release does not. Leave out a patch's state and devices on that patch can't take the delta.

   Both write a `.sha256` line for each package and `pb-os-<tag>-<image>.state.json.gz` for the next patch's delta. Keep the state files; uploading them to the release keeps them with it. (`--from-state <state>` still builds a delta from one earlier version on its own.)
   A release built before state files: `--state-only --version <tag> --rootfs <its image's root partition, mounted read-only> --kernel <the KERNEL it was built with>` writes one.
3. Add the `.sha256` lines to the release's `SHA256SUMS`, then sign it on the Mac (asks for the key's passphrase):
   ```
   scripts/sign-release.sh <folder with SHA256SUMS>
   ```
4. Upload the parts, the state files, `SHA256SUMS` and `SHA256SUMS.sig` to the release with that tag.

Devices take the newest non-draft release (pre-releases included) that has their parts, `SHA256SUMS` and `SHA256SUMS.sig`.

## Release key

Downloads install only when `SHA256SUMS.sig` is an OpenSSH signature (`ssh-keygen -Y`, namespace `pb-os-update`) from a key in [allowed_signers](allowed_signers), which every image carries. The private key never goes in the repo: it lives at `~/.ssh/pb-os-release` on the releasing Mac, protected by a passphrase, with a backup in a password manager. Without it, devices accept no further downloads until an image or a file-installed update ships a new `allowed_signers`. To replace the key, add the new public key to `allowed_signers`, release an update signed with the old key, then remove the old one.

The package carries the managed system directories, the boot image and the bundled Decky plugins. It contains no proprietary Lossless Scaling DLL, game data, Steam accounts, SSH keys or the build's machine-id. The package hash comes from the release's signed `SHA256SUMS`, so a download installs only when it is exactly the package the key holder released. **Install from a file** trusts the hash you paste instead.

## Images from before the AYN Thor packages

Their SteamOS Update app has no download button, and their updater refuses a Thor package (`unsupported device list`). Download the parts and `SHA256SUMS` from the release, then in Konsole (Thor example):

```
cat pb-os-<tag>-thor.update.tar.gz.0* > pb-os-<tag>-thor.update.tar.gz && rm pb-os-<tag>-thor.update.tar.gz.0*
tar -xzOf pb-os-<tag>-thor.update.tar.gz root/usr/share/konkr-update/konkr-update.py > /tmp/konkr-update.py
sudo python3 /tmp/konkr-update.py stage pb-os-<tag>-thor.update.tar.gz --sha256 <its line in SHA256SUMS>
systemctl reboot
```

On the RP6 and Pocket FIT, the joined file also works in the old app. After that one update, the app downloads updates itself.

## Diagnostics and recovery

Transaction data and logs are under `/home/.konkr-updates/<id>/`, accessible to root (a finished update keeps only its records and logs, see [Cleaning up](#cleaning-up)). Keep a working microSD as a recovery option for an internal install. A recovery error must be investigated before deleting the pending marker or backup.

From a recovery SD, mount the affected root, boot and HOME partitions and run the same helper as root with the corresponding paths:

```
python3 /usr/share/konkr-update/konkr-update.py recover \
  --root /mnt/root --boot /mnt/boot --home /mnt/home \
  --work /mnt/home/.konkr-updates/<id>
```

A return code of 10 means rollback completed: reboot into the restored boot image. A nonzero error leaves the pending transaction and backup intact.

The fault-injection tests exercise recovery state transitions and preservation. They do not establish immunity to storage hardware failure or every possible power-loss timing.
