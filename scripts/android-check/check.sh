#!/usr/bin/env bash
# Syntax/type-checks the Android JNI + AAudio sources on a desktop machine (no NDK needed): the real AAudio.h
# (AOSP, via the LineageOS mirror), the JDK's jni.h (same JNI API; GROOVE_DESKTOP_JNI papers over its two
# char*/void** spelling differences) and a one-line <android/log.h> stub.
# This proves the bridge compiles against the actual APIs; it does not replace an NDK build + device test.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"; root="$here/../.."
# JDK headers: Linux keeps them under /usr/lib/jvm, macOS under java_home; jni_md.h sits in linux/ or darwin/.
if [ -d /usr/lib/jvm ]; then JNI_INC="$(dirname "$(find /usr/lib/jvm -name jni.h | head -1)")"
else JNI_INC="${JAVA_HOME:-$(/usr/libexec/java_home)}/include"; fi
JNI_MD="$(dirname "$(find "$JNI_INC" -name jni_md.h | head -1)")"
for f in GrooveJni.cpp AAudioOutput.cpp; do
  # shellcheck disable=SC2086  # EXTRA_CXXFLAGS is a list of flags
  clang++ -std=c++17 -fsyntax-only ${EXTRA_CXXFLAGS:-} -Wall -Wextra -Wpedantic -Wshadow -Werror \
    -D__ANDROID__ -D'__INTRODUCED_IN(x)=' -D'__DEPRECATED_IN(x)=' -D'__REMOVED_IN(x)=' \
    -isystem "$here" -isystem "$JNI_INC" -isystem "$JNI_MD" -DGROOVE_DESKTOP_JNI -I"$root/core/include" -I"$root/platform/android/jni" \
    "$root/platform/android/jni/$f"
  echo "ok  $f"
done
