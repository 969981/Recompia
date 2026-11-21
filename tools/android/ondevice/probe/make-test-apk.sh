#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Measurement helper (not shipped): a debuggable, code-less APK (org.recompia.ondevicetest) carrying the tool executables
# from $D/out/jniLibs/arm64-v8a with extractNativeLibs=true, and testrun.c: starting its activity runs <filesDir>/run.sh in
# the app's sandbox (untrusted_app domain, app uid; run-as is only used to put files in place). Output: $D/test-apk/ondevicetest.apk
set -euo pipefail
: "${D:?set D}" "${ANDROID_HOME:?set ANDROID_HOME}" "${ANDROID_NDK_HOME:?set ANDROID_NDK_HOME}"
BT="$ANDROID_HOME/build-tools/$(ls "$ANDROID_HOME/build-tools" | sort -V | tail -1)"
PLATFORM="$ANDROID_HOME/platforms/$(ls "$ANDROID_HOME/platforms" | grep -E '^android-[0-9]+$' | sort -V | tail -1)/android.jar"
O=$D/test-apk
rm -rf "$O" && mkdir -p "$O/apk/lib/arm64-v8a"
cp "$D"/out/jniLibs/arm64-v8a/lib*.so "$O/apk/lib/arm64-v8a/"
"$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin/clang" --target=aarch64-linux-android30 -O2 -fPIC -shared \
  -Wl,-z,max-page-size=16384 "$(dirname "$0")/testrun.c" -landroid -o "$O/apk/lib/arm64-v8a/libtestrun.so"
cat > "$O/AndroidManifest.xml" <<'XML'
<?xml version="1.0" encoding="utf-8"?>
<manifest xmlns:android="http://schemas.android.com/apk/res/android" package="org.recompia.ondevicetest"
    android:versionCode="1" android:versionName="1">
  <uses-sdk android:minSdkVersion="30" android:targetSdkVersion="35"/>
  <application android:label="ondevicetest" android:hasCode="false" android:debuggable="true"
      android:extractNativeLibs="true">
    <activity android:name="android.app.NativeActivity" android:exported="true">
      <meta-data android:name="android.app.lib_name" android:value="testrun"/>
    </activity>
  </application>
</manifest>
XML
"$BT/aapt2" link -o "$O/base.apk" -I "$PLATFORM" --manifest "$O/AndroidManifest.xml"
(cd "$O/apk" && zip -q -r ../base.apk lib)
"$BT/zipalign" -f -P 16 4 "$O/base.apk" "$O/aligned.apk"
KS=$D/test-apk.keystore
[ -f "$KS" ] || keytool -genkeypair -keystore "$KS" -storepass android -keypass android -alias t -keyalg RSA -keysize 2048 \
    -validity 3650 -dname "CN=ondevicetest" >/dev/null 2>&1
"$BT/apksigner" sign --ks "$KS" --ks-pass pass:android --ks-key-alias t --out "$O/ondevicetest.apk" "$O/aligned.apk"
ls -l "$O/ondevicetest.apk"
