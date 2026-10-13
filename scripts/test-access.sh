#!/usr/bin/env bash
# Test images only: remote access without any per-device setup.
#
#   test-access.sh <rootfs> remove          take every trace out (every build)
#   test-access.sh <rootfs> install <dir>   add it (test builds only)
#
# <dir> is made by the build host for one test tag and never lives in git:
#   user          the login name (one word), e.g. pbtest
#   shadow-hash   the user's password hash, as /etc/shadow wants it
#   tag           the build tag, e.g. onetest7
#   authkey       a Tailscale auth key: reusable, ephemeral, pre-authorized,
#                 tagged (the tailnet's ACL fences what the tag may do)
#
# What the image gets:
#   - the user, in wheel (sudo asks for the password), home made at boot;
#   - tailscaled enabled, the auth key root-only under /etc/pbos-test/;
#   - on the first network connection (NetworkManager dispatcher) and at
#     every boot, `tailscale up --ssh` with that key: the device joins the
#     tailnet as <model>-<tag> (pbos-hostname adds the tag) and serves SSH
#     over Tailscale only. No OpenSSH, no keys to rotate: the tailnet
#     identity is the login.
# Release images never get this: install refuses a release tag, and
# apply-overlays runs `remove` on every build before anything else.
set -euo pipefail

R="$(cd "${1:?rootfs}" && pwd)"
MODE="${2:?remove|install}"
DIR="${3:-}"

log() { echo "== test access: $*"; }
die() { echo "ERROR: test access: $*" >&2; exit 1; }

STAMP=/etc/pbos-test
JOIN=/usr/lib/pbos/pbos-test-tailscale
UNIT=/usr/lib/systemd/system/pbos-test-tailscale.service
WANTS=(/usr/lib/systemd/system/multi-user.target.wants/pbos-test-tailscale.service
       /usr/lib/systemd/system/multi-user.target.wants/tailscaled.service)
DISPATCH=/etc/NetworkManager/dispatcher.d/90-pbos-test-tailscale
TMPFILES=/usr/lib/tmpfiles.d/pbos-test-user.conf

user_of_image() { # the user a previous install left, if any
  [[ -f "$R$STAMP/user" ]] && cat "$R$STAMP/user" || true
}

remove_user() {
  local u="$1"
  [[ -n "$u" ]] || return 0
  for f in passwd shadow; do
    [[ -f "$R/etc/$f" ]] && sed -i "/^${u}:/d" "$R/etc/$f"
    [[ -f "$R/var/lib/overlays/etc/upper/$f" ]] && sed -i "/^${u}:/d" "$R/var/lib/overlays/etc/upper/$f"
  done
  for f in "$R/etc/group" "$R/var/lib/overlays/etc/upper/group" \
    "$R/etc/gshadow" "$R/var/lib/overlays/etc/upper/gshadow"; do
    [[ -f "$f" ]] || continue
    sed -i "/^${u}:/d" "$f"
    # drop the user from any member list (…,user / user,… / :user$)
    sed -i -E "s/([:,])${u}(,|$)/\1\2/; s/,$//; s/:,/:/" "$f"
  done
}

do_remove() {
  remove_user "$(user_of_image)"
  rm -rf "$R$STAMP" "$R$JOIN" "$R$UNIT" "$R$DISPATCH" "$R$TMPFILES"
  for p in "${WANTS[@]}"; do rm -f "$R$p"; done
  rm -f "$R/var/lib/overlays/etc/upper/NetworkManager/dispatcher.d/90-pbos-test-tailscale"
}

do_install() {
  [[ -d "$DIR" ]] || die "no access dir: $DIR"
  local user hash tag key
  user="$(tr -d '[:space:]' < "$DIR/user")"
  hash="$(tr -d '[:space:]' < "$DIR/shadow-hash")"
  tag="$(tr -d '[:space:]' < "$DIR/tag")"
  key="$(tr -d '[:space:]' < "$DIR/authkey")"
  [[ "$user" =~ ^[a-z_][a-z0-9_-]{1,31}$ ]] || die "bad user name"
  [[ "$hash" == \$* ]] || die "shadow-hash is not a password hash"
  [[ "$tag" =~ ^[A-Za-z0-9._-]+$ ]] || die "bad tag"
  [[ "$key" == tskey-auth-* ]] || die "authkey is not a Tailscale auth key"
  [[ "${PB_OS_VERSION:-}" =~ ^(alpha-)?v[0-9] ]] && die "refusing test access on a release tag (${PB_OS_VERSION})"
  [[ "$tag" =~ ^(alpha-)?v[0-9] ]] && die "refusing test access on a release tag ($tag)"
  [[ -x "$R/usr/sbin/tailscaled" && -x "$R/usr/sbin/tailscale" ]] || die "Tailscale is not in the image"
  grep -q '^%wheel ' "$R/etc/sudoers.d/wheel" 2>/dev/null || die "no %wheel sudo rule in the image"
  ! grep -q "^${user}:" "$R/etc/passwd" || die "user ${user} already exists in the image"
  getent_free() { ! grep -qE "^[^:]*:[^:]*:$1:" "$R/etc/$2"; }
  local id=1001
  while ! getent_free "$id" passwd || ! getent_free "$id" group; do id=$((id + 1)); done

  log "user ${user} (uid ${id}, wheel), tag ${tag}"
  echo "${user}:x:${id}:${id}:PB-OS test access:/home/${user}:/bin/bash" >> "$R/etc/passwd"
  echo "${user}:${hash}:$(( $(date +%s) / 86400 )):0:99999:7:::" >> "$R/etc/shadow"
  echo "${user}:x:${id}:" >> "$R/etc/group"
  sed -i -E "/^wheel:/{ s/:$/:${user}/; t; s/$/,${user}/ }" "$R/etc/group"
  grep -qE "^wheel:.*[:,]${user}(,|$)" "$R/etc/group" || die "could not add ${user} to wheel"
  # /etc/gshadow mirrors /etc/group; without these lines grpck (shadow.service)
  # fails at every boot ("'pbtest' is a member of 'wheel' ... but not in gshadow").
  if [[ -f "$R/etc/gshadow" ]]; then
    echo "${user}:!::" >> "$R/etc/gshadow"
    sed -i -E "/^wheel:/{ s/:$/:${user}/; t; s/$/,${user}/ }" "$R/etc/gshadow"
  fi
  install -d -m0755 "$(dirname "$R$TMPFILES")"
  printf 'd /home/%s 0700 %s %s -\n' "$user" "$id" "$id" > "$R$TMPFILES"

  install -d -m0700 "$R$STAMP"
  printf '%s\n' "$user" > "$R$STAMP/user"
  printf '%s\n' "$tag" > "$R$STAMP/tag"
  ( umask 077; printf '%s\n' "$key" > "$R$STAMP/authkey" )
  chmod 0600 "$R$STAMP/authkey"

  install -d -m0755 "$(dirname "$R$JOIN")"
  cat > "$R$JOIN" <<'EOF'
#!/bin/sh
# Test images: join the tailnet with the build's auth key and serve SSH over
# Tailscale. Safe to run any time: it does nothing once the node is up.
KEY=/etc/pbos-test/authkey
[ -s "$KEY" ] || exit 0
state=$(tailscale status --json 2>/dev/null | sed -n 's/.*"BackendState": *"\([A-Za-z]*\)".*/\1/p' | head -1)
case "$state" in Running|Starting) exit 0 ;; esac
exec tailscale up --ssh --auth-key="file:${KEY}" --hostname="$(hostname)" --timeout=90s
EOF
  chmod 0755 "$R$JOIN"

  cat > "$R$UNIT" <<EOF
[Unit]
Description=PB-OS test image: join the tailnet (Tailscale SSH)
After=network-online.target tailscaled.service pbos-hostname.service
Wants=network-online.target
Requires=tailscaled.service
ConditionPathExists=${STAMP}/authkey

[Service]
Type=oneshot
ExecStart=${JOIN}

[Install]
WantedBy=multi-user.target
EOF
  for p in "${WANTS[@]}"; do
    install -d -m0755 "$(dirname "$R$p")"
    ln -sf "../$(basename "$p")" "$R$p"
  done
  # NetworkManager runs this on every connection change: the device joins
  # as soon as Wi-Fi is set up in Steam, no reboot needed.
  install -d -m0755 "$(dirname "$R$DISPATCH")"
  cat > "$R$DISPATCH" <<'EOF'
#!/bin/sh
case "$2" in
  up|connectivity-change|dhcp4-change|dhcp6-change) systemctl start --no-block pbos-test-tailscale.service ;;
esac
exit 0
EOF
  chmod 0755 "$R$DISPATCH"
  log "tailscaled enabled, join service + dispatcher installed"
}

case "$MODE" in
  remove) do_remove ;;
  install) do_remove; do_install ;;
  *) die "mode must be remove or install" ;;
esac
