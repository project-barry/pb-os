# SPDX-License-Identifier: GPL-2.0-or-later
# What of /home a copy brings, shared by install-masios-to-internal.sh (SD to
# internal) and copy-to-sd.sh (internal to SD). Sourced, not run:
#
#   home_bytes MODE        bytes the copy needs
#   copy_home MODE DEST    copy /home into DEST (a mounted, empty home)
#
# MODE: all          everything, installed games included
#       essentials   Steam login, settings, saves and plugins; no games
#       none         Steam client and Decky only; Steam starts from its setup
#
# Run directly with "sizes", it prints HOME_ALL_BYTES and HOME_GAMES_BYTES
# (what essentials and none leave out), for PB-OS Utils.

STEAMAPPS=/home/steamos/.local/share/Steam/steamapps

GAMES_EXCLUDES=(
  --exclude='/steamos/.local/share/Steam/steamapps/common/'
  --exclude='/steamos/.local/share/Steam/steamapps/shadercache/'
  --exclude='/steamos/.local/share/Steam/steamapps/compatdata/'
  --exclude='/steamos/.local/share/Steam/steamapps/downloading/'
  --exclude='/steamos/.local/share/Steam/steamapps/appmanifest_*.acf'
)
# "Start fresh" still needs the Steam client and Decky from /home, or Steam
# can't start at all (black screen). Leave out everything personal instead.
FRESH_EXCLUDES=(
  "${GAMES_EXCLUDES[@]}"
  --exclude='/steamos/.local/share/Steam/config/loginusers.vdf'
  --exclude='/steamos/.local/share/Steam/config/config.vdf'
  --exclude='/steamos/.local/share/Steam/config/DialogConfig.vdf'
  --exclude='/steamos/.local/share/Steam/config/libraryfolders.vdf'
  --exclude='/steamos/.local/share/Steam/config/remoteclients.vdf'
  --exclude='/steamos/.local/share/Steam/config/avatarcache/'
  --exclude='/steamos/.local/share/Steam/config/htmlcache/'
  --exclude='/steamos/.local/share/Steam/appcache/httpcache/'
  --exclude='/steamos/.local/share/Steam/appcache/cefdata/'
  --exclude='/steamos/.local/share/Steam/userdata/'
  --exclude='/steamos/.local/share/Steam/ssfn*'
  --exclude='/steamos/.local/share/Steam/logs/'
  --exclude='/steamos/.steam/registry.vdf'
  --exclude='/steamos/Android/'
  --exclude='/steamos/.local/share/konkr-apk/'
  --exclude='/steamos/.local/share/Trash/'
  --exclude='/steamos/.var/'
  --exclude='/steamos/.mozilla/'
  --exclude='/steamos/Documents/*' --exclude='/steamos/Downloads/*'
  --exclude='/steamos/Pictures/*' --exclude='/steamos/Music/*' --exclude='/steamos/Videos/*'
)

games_bytes() {
  du -scxb "$STEAMAPPS/common" "$STEAMAPPS/shadercache" "$STEAMAPPS/compatdata" \
    "$STEAMAPPS/downloading" 2>/dev/null | tail -1 | awk '{print $1}'
}

home_bytes() {
  local t g
  t=$(du -sxb /home | awk '{print $1}')
  case "$1" in
    all) echo "$t" ;;
    essentials|none) g=$(games_bytes); echo $(( t - ${g:-0} )) ;;
  esac
}

copy_home() {
  local mode="$1" dest="$2"
  case "$mode" in
    all)
      rsync -aAXH --numeric-ids --info=progress2 --exclude=/lost+found /home/ "$dest/" ;;
    essentials)
      rsync -aAXH --numeric-ids --info=progress2 --exclude=/lost+found "${GAMES_EXCLUDES[@]}" \
        /home/ "$dest/" ;;
    none)
      rsync -aAXH --numeric-ids --info=progress2 --exclude=/lost+found "${FRESH_EXCLUDES[@]}" \
        /home/ "$dest/"
      # Same first-run state as a freshly flashed card: Steam's setup runs, and
      # Wi-Fi stays on wpa_supplicant (see install-complete-steam-client.sh).
      mkdir -p "$dest/steamos/.steam" "$dest/steamos/.local/share/Steam/config"
      printf '%b\n' '"Registry"' '{' '\t"HKCU"' '\t{' '\t\t"Software"' '\t\t{' \
        '\t\t\t"Valve"' '\t\t\t{' '\t\t\t\t"Steam"' '\t\t\t\t{' \
        '\t\t\t\t\t"CompletedOOBEStage1"\t\t"0"' '\t\t\t\t\t"CompletedOOBE"\t\t"0"' \
        '\t\t\t\t}' '\t\t\t}' '\t\t}' '\t}' '}' >"$dest/steamos/.steam/registry.vdf"
      printf '%b\n' '"InstallConfigStore"' '{' '\t"SteamOS"' '\t{' \
        '\t\t"WifiForceWPASupplicant"\t\t"1"' '\t}' '}' \
        >"$dest/steamos/.local/share/Steam/config/config.vdf"
      chown 1000:1000 "$dest/steamos" "$dest/steamos/.steam" \
        "$dest/steamos/.steam/registry.vdf" "$dest/steamos/.local/share/Steam/config" \
        "$dest/steamos/.local/share/Steam/config/config.vdf" ;;
  esac
}

if [[ "${BASH_SOURCE[0]}" == "$0" && "${1:-}" == sizes ]]; then
  echo "HOME_ALL_BYTES=$(home_bytes all)"
  echo "HOME_GAMES_BYTES=$(games_bytes)"
fi
