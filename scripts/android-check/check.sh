#!/usr/bin/env bash
# Syntax/type-checks the Android JNI + AAudio sources on a desktop machine (no NDK needed): the real AAudio.h
# (AOSP, via the LineageOS mirror), the JDK's jni.h (same JNI API; GROOVE_DESKTOP_JNI papers over its two
# char*/void** spelling differences) and a one-line <android/log.h> stub.
# This proves the bridge compiles against the actual APIs; it does not replace an NDK build + device test.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"; root="$here/../.."
JNI_INC="$(dirname "$(find /usr/lib/jvm -name jni.h | head -1)")"
for f in GrooveJni.cpp AAudioOutput.cpp; do
  clang++ -std=c++17 -fsyntax-only -Wall -Wextra -Wpedantic -Wshadow -Werror \
    -D__ANDROID__ -D'__INTRODUCED_IN(x)=' -D'__builtin_available(...)=1' -D'__DEPRECATED_IN(x)=' -D'__REMOVED_IN(x)=' \
    -isystem "$here" -isystem "$JNI_INC" -isystem "$JNI_INC/linux" -DGROOVE_DESKTOP_JNI -I"$root/core/include" -I"$root/platform/android/jni" \
    "$root/platform/android/jni/$f"
  echo "ok  $f"
done
