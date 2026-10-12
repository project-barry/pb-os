# KWin 6.2.5 without the Desktop Mode input prompt

## The problem

In Desktop Mode, the first controller or Steam Input movement makes KDE ask
"An application requested access to input devices" on every login, and the
prompt has to be accepted by touch (pb-os issue #6).

Steam moves the pointer through X11 XTest. KWin starts Xwayland with
`-enable-ei-portal`, so XTest input goes through the RemoteDesktop portal,
and xdg-desktop-portal-kde shows its dialog.

## How the Steam Deck avoids it

Valve's Deck SteamOS 3.8 ships KWin 6.4, where the kwinrc setting
`[Xwayland] XwaylandEisNoPrompt=true` makes KWin create its own libei
socket, hand it to Xwayland as `LIBEI_SOCKET`, and start Xwayland without
`-enable-ei-portal`. Upstream commit bead0e3419 (Plasma 6.4). Valve's kwinrc
sets the key on the Frame image too, but the Frame base ships KWin 6.2.5,
which ignores it.

Before 6.4, Valve carried the same change as three patches in its Deck
package `kwin 6.2.5-1.2` (source: steamdeck-packages.steamos.cloud,
jupiter-main). They are David Redondo's upstream commits, backported to
6.2.5, and they take the socket route unconditionally:

- `0001-Allow-passing-extra-environment-variables-to-Xwaylan.patch`
- `0002-Allow-specifying-file-descriptors-that-are-passed-to.patch`
- `0003-Enable-Xwayland-xtest-input-emulation-without-going-.patch`

`patches/` holds them unchanged (LGPL-2.0-or-later, like KWin).

## What PB-OS does

`scripts/build-kwin-6.2.5.sh` builds upstream KWin 6.2.5 plus those three
patches inside the image rootfs (bubblewrap, same Qt/KF6/glibc), caches the
result in `$WORK/kwin-pbos/`, and copies the package files over the base's
kwin. `scripts/apply-overlays.sh` runs it for every image.

The Frame's own package (`kwin 6.2.5-3.1`, a hotfix build) is replaced
whole. Its source is not published, so any Valve-only change in it is lost;
the files it ships are the same set.

## Every base bump

The script reads the base's kwin version from the pacman database:

- `6.2.5-*`: build and install (this backport).
- `6.4.0` or newer: skips with a notice. The fix is upstream and the kwinrc
  key does the work. Delete this directory and the script then.
- anything else: fails. Re-check the patches against that version.

Valve's rolling Frame repo already has kwin 6.6.2 (no patches); the 0.4.x
and 0.5.x hotfix channels still ship 6.2.5-3.1 (checked 2026-10-11).
