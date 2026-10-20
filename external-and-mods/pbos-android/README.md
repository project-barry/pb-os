# pbos-android

Android apps on PB-OS run in Lepton, Valve's Android layer (Steam app
3029110), which Steam downloads on first use. Lepton runs APKs as they are:
`pbos-apk install` (or opening an APK in Dolphin) hands them to Android's own
package installer, and each app becomes a Steam title.

## payload/common (in the image)

The only Lepton overlay PB-OS ships, installed to
`~/.local/share/pbos-android` (home, not the root partition; update
packages carry it like the bundled Decky plugins, and replace the folder):

| File | Why |
| --- | --- |
| `system/etc/permissions/android.software.freeform_window_management.xml` | empty: Waydroid opens every app as a freeform window, whose caption bar crashes apps like Instagram |
| `system/usr/keylayout/Vendor_28de_Product_11ff.kl` | Steam's virtual pad as an Xbox 360 pad; Generic.kl maps the triggers to the right stick |

## Google Play: the user's choice

PB-OS ships no Google apps, as Waydroid doesn't. Valve's Lepton has none
either: it exists for Android games sold on Steam. A user who wants the Play
Store launches **Add Google Play** in the Steam library (`pbos-apk gapps`),
and the device fetches everything from its own source into
`~/.local/share/pbos-google-play`, which updates never touch.
`/usr/lib/pbos/google-play-fetch` refuses any file whose SHA-256 differs
from the pinned one:

| What | From |
| --- | --- |
| Play services, Services Framework, permission and sysconfig XMLs | MindTheGapps 11.0.0 arm64 (20230922), read out of the zip with range requests |
| Play Store (`Phonesky`) | MindTheGapps 14.0.0 arm64 (20250203): 11's has no arm64 libs, 14's runs on Lepton's Android 11 |
| Simple Keyboard | F-Droid, version 148: Lepton ships no keyboard |
| Java runtime, smali / baksmali 2.5.2 | Eclipse Temurin 17 JRE (aarch64) and JesusFreke's smali, only to build the fixes below; deleted afterwards |

### lepton-&lt;version&gt; (built on the device)

Lepton comments services out of `SystemServer`. Play services and the
keyboard crash without some of them (with `RestrictionsManager` null the
keyboard dies in `onCreate`, so there is no way to sign in), so
`framework/restore-services.py` starts clipboard, restrictions, biometric +
auth, NSD, cross-profile apps and hardware properties again (smali edit of
`services.jar`). Everything compiled against the old jar must be rebuilt too,
or zygote re-dexopts system_server before installd is up and dies:
`framework/compile-odex.sh` does that inside Android with its own dex2oat.

That ties the result to one Lepton Android image (`images/version.txt`).
`pbos-apk` builds it on the device (`build_fixes`, with the scripts the image
installs to `/usr/lib/pbos/android-framework`) the first time Android starts
with Google Play and again after Valve updates Lepton, then restarts Android
with it. A failed build is retried when Add Google Play is launched again.

With Google Play, Android also presents the certified Pixel 5 build
(`props_overlay`; Google refuses Play Store sign-in on uncertified builds);
without it, Lepton keeps Valve's identity.

`framework/build-framework.sh` does the same build from a Mac with Colima,
for debugging (any Java 17+ works: baksmali `-a 30`, `restore-services.py`,
smali `-a 30`, `repack-jar.py`, then `compile-odex.sh` on the device).
`build-payload.sh` fetches the same Google files on a build machine; images
no longer use it.

## What pbos-apk does at run time

Not files, but part of the same fixes (see `sm8650-overlay/usr/bin/pbos-apk`):

- one shared Android (`pbos-android.service`) with its data kept, every
  installed app a Steam title with its icon
- binder in the kernel (`kernel-sm8650/steamos.config`)
- unlimited stack rlimit for the container: legacy mmap layout below 2^47,
  or LuaJIT (many Unity games) can't start on our 52-bit VA kernel
- the host's `/dev/input` mounted, Steam's current virtual pad linked in
- with Google Play: touch keyboard selected; without it, updates of the
  Google apps earlier images shipped are uninstalled from `/data`
- "setup complete" set (else Android ignores Home)
- a starting title in Steam: Add Google Play, or the Play Store once it is
  added; the PB-OS tab (`androidTitles` in `pbos-qam/sections/tools.js`)
  removes the one that no longer fits
- gamescope patches: Android window gets the Steam title's app id, is
  reported to Steam, gets real touch (not click emulation)
- edits to Steam's Lepton, re-applied every start (Steam updates overwrite
  it). Valve rewords those files between releases, so each edit carries one
  wording per Lepton version it knows: v3.0.3 quotes the two overlay paths
  and dropped the `ro.product.brand` line, which silently cost the extra
  rootfs overlay and the certified Pixel 5 identity until both were taught
  the new shape. The identity edit is gated on `PBOS_LEPTON_GOOGLE`, and
  files patched before Google Play was optional are rewritten to that
