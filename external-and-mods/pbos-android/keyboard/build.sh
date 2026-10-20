#!/bin/bash
# Build PbosKeyboard.apk (see README.md) into
# ../payload/common/system/app/PbosKeyboard/. Needs the Android command-line
# tools (build-tools 35.0.0, platform android-35) and a JDK 17.
#   ANDROID_HOME  default /opt/homebrew/share/android-commandlinetools
#   KEYSTORE      default ~/.android/pbos-keyboard.keystore (made if missing)
set -euo pipefail
cd "$(dirname "$0")"
SDK="${ANDROID_HOME:-/opt/homebrew/share/android-commandlinetools}"
BT="$SDK/build-tools/35.0.0"
JAR="$SDK/platforms/android-35/android.jar"
KEYSTORE="${KEYSTORE:-$HOME/.android/pbos-keyboard.keystore}"
OUT=../payload/common/system/app/PbosKeyboard/PbosKeyboard.apk
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

if [[ ! -f "$KEYSTORE" ]]; then
  keytool -genkeypair -keystore "$KEYSTORE" -storepass android -keypass android \
    -alias pbos-keyboard -keyalg RSA -keysize 3072 -validity 36500 \
    -dname "CN=PB-OS Keyboard"
fi

"$BT/aapt2" compile --dir res -o "$work/res.zip"
"$BT/aapt2" link -I "$JAR" --manifest AndroidManifest.xml "$work/res.zip" -o "$work/base.apk"
mkdir -p "$work/classes"
javac --release 11 -classpath "$JAR" -d "$work/classes" $(find src -name '*.java')
"$BT/d8" --release --min-api 30 --lib "$JAR" --output "$work" $(find "$work/classes" -name '*.class')
(cd "$work" && zip -q -j base.apk classes.dex)
"$BT/zipalign" -f 4 "$work/base.apk" "$work/aligned.apk"
"$BT/apksigner" sign --ks "$KEYSTORE" --ks-pass pass:android --ks-key-alias pbos-keyboard \
  --out "$work/signed.apk" "$work/aligned.apk"
mkdir -p "$(dirname "$OUT")"
cp "$work/signed.apk" "$OUT"
echo "$OUT"
