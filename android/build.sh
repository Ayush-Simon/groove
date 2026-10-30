#!/usr/bin/env bash
# Builds Groove.apk without Gradle or Android Studio.
#
# Needs (set TOOLS to a folder containing them):
#   aapt2            Android resource compiler (linux x64)
#   android-29.jar   Android 10 platform library
#   dx.jar           dex compiler (built from AOSP dalvik/dx source)
#   apksigner.jar    APK signer (v1 + v2 signatures)
# and a JDK (javac, keytool).
#
#   TOOLS=/path/to/tools ./build.sh
set -euo pipefail
cd "$(dirname "$0")"
TOOLS="${TOOLS:?set TOOLS to the folder with aapt2, android-29.jar, dx.jar, apksigner.jar}"
OUT=build
VERSION_CODE="${VERSION_CODE:-5}"
VERSION_NAME="${VERSION_NAME:-3.0}"
unset JAVA_TOOL_OPTIONS || true

rm -rf "$OUT"
mkdir -p "$OUT"/{flat,gen,classes,dex,assets/web}

echo "1/6 Bundling the web app"
cp -R ../mobile-app/. "$OUT/assets/web/"

echo "2/6 Compiling resources"
"$TOOLS/aapt2" compile --dir res -o "$OUT/flat/res.zip"
"$TOOLS/aapt2" link -o "$OUT/unsigned.apk" \
  -I "$TOOLS/android-29.jar" \
  --manifest AndroidManifest.xml \
  --min-sdk-version 24 --target-sdk-version 29 \
  --version-code "$VERSION_CODE" --version-name "$VERSION_NAME" \
  -A "$OUT/assets" \
  --java "$OUT/gen" \
  "$OUT/flat/res.zip"

echo "3/6 Compiling Java"
find src "$OUT/gen" -name '*.java' > "$OUT/sources.txt"
javac -nowarn -encoding UTF-8 -source 8 -target 8 -Xlint:-options \
  -bootclasspath "$TOOLS/android-29.jar" -classpath "$TOOLS/android-29.jar" \
  -d "$OUT/classes" @"$OUT/sources.txt"

echo "4/6 Converting to dex"
java -jar "$TOOLS/dx.jar" --dex --min-sdk-version=24 --output="$OUT/dex/classes.dex" "$OUT/classes"
(cd "$OUT/dex" && zip -q ../unsigned.apk classes.dex)
# Groove's native decoders/engine (scripts/build-android-native.sh). Without it the app still works,
# decoding through the WebView as before.
if [ -f build-native/arm64-v8a/libgroove.so ]; then
  mkdir -p "$OUT/native/lib/arm64-v8a"
  cp build-native/arm64-v8a/libgroove.so "$OUT/native/lib/arm64-v8a/"
  (cd "$OUT/native" && zip -q ../unsigned.apk lib/arm64-v8a/libgroove.so)
  echo "   + lib/arm64-v8a/libgroove.so"
fi

echo "5/6 Aligning"
python3 - "$OUT/unsigned.apk" "$OUT/aligned.apk" <<'PY'
# Minimal zipalign: stored (uncompressed) entries start on 4-byte boundaries.
import sys, zipfile
src, dst = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(src) as zin, open(dst, 'wb') as raw:
    zout = zipfile.ZipFile(raw, 'w')
    for info in zin.infolist():
        data = zin.read(info.filename)
        ni = zipfile.ZipInfo(info.filename, date_time=(2008, 1, 1, 0, 0, 0))
        ni.compress_type = info.compress_type
        ni.external_attr = info.external_attr
        if ni.compress_type == zipfile.ZIP_STORED:
            header_len = 30 + len(ni.filename.encode('utf-8'))
            pad = (4 - (raw.tell() + header_len) % 4) % 4
            ni.extra = b'\x00' * pad
        zout.writestr(ni, data)
    zout.close()
PY

echo "6/6 Signing"
KS=keystore/groove-release.jks
if [ ! -f "$KS" ]; then
  mkdir -p keystore
  PASS=$(python3 -c "import secrets;print(secrets.token_urlsafe(18))")
  keytool -genkeypair -keystore "$KS" -alias groove -keyalg RSA -keysize 2048 -validity 10000 \
    -storepass "$PASS" -keypass "$PASS" -dname "CN=Groove, O=Groove, C=IN" >/dev/null 2>&1
  echo "$PASS" > keystore/password.txt
  echo "   Created a new signing key in android/keystore/ — keep it: updates must be signed with the same key."
fi
PASS=$(cat keystore/password.txt)
java -jar "$TOOLS/apksigner.jar" sign --ks "$KS" --ks-key-alias groove --ks-pass "pass:$PASS" --key-pass "pass:$PASS" \
  --v1-signing-enabled true --v2-signing-enabled true --out "$OUT/Groove.apk" "$OUT/aligned.apk"
java -jar "$TOOLS/apksigner.jar" verify -v "$OUT/Groove.apk" | head -4
ls -la "$OUT/Groove.apk"
