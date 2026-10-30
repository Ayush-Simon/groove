#!/usr/bin/env bash
# Builds android/build/native/arm64-v8a/libgroove.so (Groove's decoders + engine) for the APK.
#   With the official NDK:      ANDROID_NDK=/path/to/ndk scripts/build-android-native.sh
#   With clang ≥ 20 + a sysroot: GROOVE_NDK_PREBUILT=<ndk>/toolchains/llvm/prebuilt/<host> scripts/build-android-native.sh
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
b="$root/build/android-arm64"
if [ -n "${ANDROID_NDK:-}" ]; then
  tc=(-DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake" -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26)
else
  : "${GROOVE_NDK_PREBUILT:?set ANDROID_NDK or GROOVE_NDK_PREBUILT}"
  tc=(-DCMAKE_TOOLCHAIN_FILE="$root/scripts/android-clang-toolchain.cmake" -DGROOVE_NDK_PREBUILT="$GROOVE_NDK_PREBUILT")
fi
cmake -S "$root/platform/android/jni" -B "$b" -G Ninja -DCMAKE_BUILD_TYPE=Release -Wno-dev "${tc[@]}" >/dev/null
cmake --build "$b" --target groove
mkdir -p "$root/android/build-native/arm64-v8a"
strip=$(command -v llvm-strip-20 || command -v llvm-strip || echo "$ANDROID_NDK/toolchains/llvm/prebuilt/*/bin/llvm-strip")
$strip --strip-unneeded -o "$root/android/build-native/arm64-v8a/libgroove.so" "$b/libgroove.so"
ls -la "$root/android/build-native/arm64-v8a/libgroove.so"
