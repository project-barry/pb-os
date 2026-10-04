# Offline SteamOS updates

Installs a new pb-os version over the current one without reflashing. Games, saves, Steam accounts, Wi-Fi, Decky settings and your own Decky plugins stay.

There are two kinds of package. A **delta** carries only what changed since one earlier version, usually tens to hundreds of MB, and is quick to download and install. A **full** package carries the whole system (about 4 GB) and installs over any earlier version. The updater takes the delta from the installed version when the release has one, and the full package otherwise.

## How it works

1. In Game Mode, open Quick Access → Decky → **PB-OS Update** ([plugin](../Decky/pbos-update/), on every image). It looks for the newest pb-os release with a package for this device and shows a toast when one comes out. Press **Download and install**; the download carries on with the menu closed, can be paused, and resumes where it stopped. Then **Restart and install**.
   Desktop Mode has the same in the **SteamOS Update** app, which also installs a package from a file (paste its SHA-256 from the release's `SHA256SUMS`).
   Without internet, PB-OS Update takes the update from a microSD card or USB drive instead (below).
2. Preparation runs before the restart, while Steam keeps running, with its progress in PB-OS Update. It checks the archive and works out exactly what changes: for a full package it compares each file's header with the installed system and unpacks only what differs (a delta carries only what changed anyway). It checks those files against the manifest, backs up the `/usr` and `/opt` files it will replace, checks free space and copies a private recovery runtime to HOME. It saves the current boot image and installs the new one, which carries the recovery hook.
3. On restart, the initramfs mounts the same root, boot and HOME filesystems, verifies their UUIDs, and runs the package's updater. It shows **Installing update** with a progress bar and pulsing dots on the screen, backs up the `/etc` files it will replace, then writes and deletes only the planned files. The restart step takes about as long as the change is big, not as long as the system is big.
4. It installs the Decky plugins the package carries, removes the device plugins it lists for removal, checks every file it wrote, then boots SteamOS. Accounts, passwords, machine-id, hostname, fstab, SSH keys and network connections in `/etc` are kept.
5. An interrupted apply or rollback is recovered on the next boot. A failed apply restores the backup and reboots into the previous kernel. If recovery itself cannot finish, normal boot stops and the log is left on HOME.

No partition table is changed and no filesystem is formatted. SD and internal installs use the same process. HOME needs room for the download plus about twice the size of the files that change. The root partition must also fit the new system.

A delta applies only to the exact version it was built from (`/usr/share/pb-os/version`). Updaters from before deltas (alpha v0.3 and v0.3a) don't see them and take the full package.

The apply step runs the updater that comes in the package, so fixes to it reach devices with the update itself.

## Updating from a microSD card or USB drive

For a device without internet. On a computer, download from the release page:

- the update for your device: the full package (`pb-os-<tag>-<device>.update.tar.gz.001`, `.002`, ...) or, if the release has one from the version you have, the delta (`pb-os-<tag>-<device>.from-<your version>.delta.tar.gz.001`, ...). The joined `.tar.gz` works too;
- `SHA256SUMS` and `SHA256SUMS.sig`.

`<device>` is `rp6` for the Retroid Pocket 6 and Nova, `pocketfit` for the KONKR Pocket FIT and AYANEO Pocket S2, and `thor` for the AYN Thor.

Copy them, unchanged, to the top folder of a microSD card or USB drive formatted FAT32, exFAT, NTFS or ext4. The parts are each under 2 GB, so FAT32 works. Put the card or drive in the device. When pb-os runs from internal storage, use a microSD card or a USB drive; when it runs from a microSD card, use a USB drive.

PB-OS Update notices the drive and shows a toast. Open it and press **Install from the drive**. It copies the update to HOME (keep the drive in until it says you can take it out), then prepares it like a download. Then **Restart and install**.

Like a download, it installs only when `SHA256SUMS.sig` is from the pb-os release key and the package matches its line in `SHA256SUMS`, so the files from one release must stay together. PB-OS Update says what is missing when they don't match. It mounts the drive read-only while it looks and copies, and never looks at the disk pb-os runs from.

From Konsole: `sudo python3 /usr/share/konkr-update/konkr-update.py local-check` shows what it finds, and `local-update` installs it.

## From Konsole

`sudo python3 /usr/share/konkr-update/konkr-update.py update`, then restart. `check` (no sudo) prints what is installed and what the releases offer.

## Releasing an update

One package per image: Pocket FIT / Pocket S2 (`pocketfit`), Retroid Pocket 6 (`rp6`) and AYN Thor (`thor`, SM8550 with the bottom-screen extras). A package only installs on the DTB models in its manifest.

1. Build the image with `PB_OS_VERSION=<release tag>` (e.g. `alpha-v0.3`). The image records it in `/usr/share/pb-os/version`; the updater offers a release whose tag differs from it.
2. From the same rootfs, with the same SoC and device, and the state file of each earlier release to make a delta from:
   ```
   sudo scripts/build-update-package.py --soc sm8550 [--device thor] \
     --rootfs /work/rootfs-... --kernel <kernel output>/boot/KERNEL --release <dir> \
     --from-state pb-os-<previous tag>-<image>.state.json.gz
   ```
   This writes `pb-os-<tag>-<image>.update.tar.gz.001`, `.002`, ... (under GitHub's 2 GiB limit), `pb-os-<tag>-<image>.from-<previous tag>.delta.tar.gz.001`, ..., a `.sha256` line for each, and `pb-os-<tag>-<image>.state.json.gz` for the next release's deltas. Keep the state files; uploading them to the release keeps them with it.
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

Transaction data and logs are under `/home/.konkr-updates/<id>/`, accessible to root. Keep a working microSD as a recovery option for an internal install. A recovery error must be investigated before deleting the pending marker or backup.

From a recovery SD, mount the affected root, boot and HOME partitions and run the same helper as root with the corresponding paths:

```
python3 /usr/share/konkr-update/konkr-update.py recover \
  --root /mnt/root --boot /mnt/boot --home /mnt/home \
  --work /mnt/home/.konkr-updates/<id>
```

A return code of 10 means rollback completed: reboot into the restored boot image. A nonzero error leaves the pending transaction and backup intact.

The fault-injection tests exercise recovery state transitions and preservation. They do not establish immunity to storage hardware failure or every possible power-loss timing.
