#!/usr/bin/env bash
# build-framework.sh [ssh-target] — rebuild payload/lepton-<ver>/ for the
# Lepton Android image installed on the device (run again after Valve
# updates it). Needs Colima (Java + smali) and the device with Android up
# (`pbos-apk store` once).
#
# Lepton comments SystemServer services out of services.jar; normal apps and
# Play services crash without some of them (restore-services.py lists which).
# Changing services.jar invalidates everything compiled against it, and
# zygote then tries to dexopt system_server before installd runs and dies,
# so this also rebuilds (compile-odex.sh, inside Android with its dex2oat):
# services.{odex,vdex,art}, ethernet-service.{odex,vdex} and the two apex
# jars Valve prebakes into /data/dalvik-cache.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
TARGET="${1:-root@frame.local}"
SSH=(ssh -i ~/.ssh/pbos_steamos "$TARGET")
LEPTON=/home/steamos/.local/share/Steam/steamapps/common/Lepton
CTR=lepton-steamlaunch-pbos-android
WORK=/work/lepton-framework
SMALI=https://bitbucket.org/JesusFreke/smali/downloads

ver="$("${SSH[@]}" cat "$LEPTON/images/version.txt")"
out="${HERE}/../payload/lepton-${ver}"
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
echo "== Lepton Android image ${ver}"

"${SSH[@]}" cat "$LEPTON/images/rootfs/system/framework/services.jar" >"$tmp/orig.jar"
colima ssh -- sudo bash -c "set -e
  command -v java >/dev/null || (apt-get update -qq && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq openjdk-17-jre-headless >/dev/null)
  mkdir -p $WORK/tools; cd $WORK
  for j in smali baksmali; do [ -s tools/\$j.jar ] || curl -sfL -o tools/\$j.jar $SMALI/\$j-2.5.2.jar; done
  rm -rf jar classes && mkdir jar"
colima ssh -- sudo tee "$WORK/orig.jar" <"$tmp/orig.jar" >/dev/null
colima ssh -- sudo tee "$WORK/restore-services.py" <"$HERE/restore-services.py" >/dev/null
colima ssh -- sudo tee "$WORK/repack-jar.py" <"$HERE/repack-jar.py" >/dev/null
colima ssh -- sudo bash -c "set -e; cd $WORK
  (cd jar && unzip -qq ../orig.jar classes.dex)
  java -jar tools/baksmali.jar d -a 30 jar/classes.dex -o classes
  python3 restore-services.py classes/com/android/server/SystemServer.smali
  java -jar tools/smali.jar a -a 30 classes -o classes.new.dex
  python3 repack-jar.py orig.jar services.jar classes.dex=classes.new.dex"
colima ssh -- sudo cat "$WORK/services.jar" >"$tmp/services.jar"

echo "== compiling against it inside Android"
scp -q -i ~/.ssh/pbos_steamos "$tmp/services.jar" "$HERE/compile-odex.sh" "$TARGET:/tmp/"
"${SSH[@]}" "export XDG_RUNTIME_DIR=/run/user/1000; cd /tmp; P='sudo -Eu steamos HOME=/home/steamos podman'
  \$P cp /tmp/services.jar $CTR:/data/local/tmp/services.jar
  \$P cp /tmp/compile-odex.sh $CTR:/data/local/tmp/compile-odex.sh
  \$P exec $CTR /system/bin/sh /data/local/tmp/compile-odex.sh >/tmp/compile-odex.log 2>&1
  rm -rf /tmp/kout; \$P cp $CTR:/data/local/tmp/out /tmp/kout
  cp /tmp/services.jar /tmp/kout/system/framework/services.jar
  tar cf /tmp/kout.tar -C /tmp/kout ."
rm -rf "$out"; mkdir -p "$out"
"${SSH[@]}" cat /tmp/kout.tar | tar xf - -C "$out"
[[ "$(find "$out" -type f | wc -l)" == 10 ]] || { echo "unexpected output in $out" >&2; exit 1; }
find "$out" -name '._*' -delete
echo "== $out"; find "$out" -type f | sort
