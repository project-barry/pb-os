# Offline SteamOS updates

Installs a new pb-os version over the current one without reflashing. Games, saves, Steam accounts, Wi-Fi, Decky settings and your own Decky plugins stay.

## How it works

1. In Desktop Mode, open **SteamOS Update**. Select the update package for your device and paste its SHA-256 from the pb-os release.
2. Preparation checks the archive, hashes every payload file, checks free space, and copies a private recovery runtime to HOME. It saves the current boot image and installs the new one, which carries the recovery hook.
3. On restart, the initramfs mounts the same root, boot and HOME filesystems. It verifies their UUIDs and takes a rollback copy before replacing system files.
4. It replaces `/usr`, `/opt` and `/etc`, installs the Decky plugins the package carries, removes the device plugins it lists for removal, verifies the installed files, then boots SteamOS. Accounts, passwords, machine-id, hostname, fstab, SSH keys and network connections in `/etc` are kept.
5. An interrupted apply or rollback is recovered on the next boot. A failed apply restores the backup and reboots into the previous kernel. If recovery itself cannot finish, normal boot stops and the log is left on HOME.

No partition table is changed and no filesystem is formatted. SD and internal installs use the same process. HOME needs room for the package, its unpacked payload and a copy of the current system (about 25 GB); the root partition must also fit the new system.

The apply step runs the updater that comes in the package, so fixes to it reach devices with the update itself.

## Packages

One package per image: Pocket FIT / Pocket S2 (SM8650), Retroid Pocket 6 (SM8550), and AYN Thor (SM8550 with the bottom-screen extras). A package only installs on the DTB models in its manifest. Build one from the rootfs that went into the image, with the same SoC and device:

```
sudo scripts/build-update-package.py --soc sm8550 [--device thor] \
  --rootfs /work/rootfs-... --kernel <kernel output>/boot/KERNEL \
  --version <version> --output pb-os-<device>-<version>.tar.gz
```

The package carries the managed system directories, the boot image and the bundled Decky plugins. It contains no proprietary Lossless Scaling DLL, game data, Steam accounts, SSH keys or the build's machine-id. The expected hash must come from the release. A local checksum proves integrity; it does not authenticate an untrusted download.

## Images from before the AYN Thor packages

Their updater refuses a Thor package (`unsupported device list`). Run the updater from the package once, in Konsole:

```
tar -xzOf pb-os-thor-<version>.tar.gz root/usr/share/konkr-update/konkr-update.py > /tmp/konkr-update.py
sudo python3 /tmp/konkr-update.py stage pb-os-thor-<version>.tar.gz --sha256 <hash from the release>
systemctl reboot
```

The installed updater is current after that.

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
