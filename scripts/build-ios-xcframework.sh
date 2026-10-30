#!/usr/bin/env bash
# Builds GrooveCore.xcframework (device arm64 + simulator arm64/x86_64) from core/ + platform/ios/GrooveCore.
# Run on a Mac with Xcode 15+ and CMake 3.22+. Dependencies are fetched from pinned sources (GROOVE_FETCH_DEPS).
# Licensing: libsndfile is LGPL-2.1 — for App Store builds ship it as a separate dynamic framework (or swap the
# sndfile decoder for AudioToolbox, roadmap phase 2). See docs/ENGINE.md.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
out="$root/build/ios"
rm -rf "$out"
mkdir -p "$out"

build() {  # sdk, archs, dir
  cmake -S "$root/platform/ios" -B "$out/$3" -G Xcode \
    -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT="$1" -DCMAKE_OSX_ARCHITECTURES="$2" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 -DCMAKE_XCODE_ATTRIBUTE_ONLY_ACTIVE_ARCH=NO
  cmake --build "$out/$3" --config Release
}
build iphoneos arm64 device
build iphonesimulator "arm64;x86_64" sim

xcodebuild -create-xcframework \
  -library "$out/device/Release-iphoneos/libGrooveCore.a" -headers "$root/platform/ios/GrooveCore/include" \
  -library "$out/sim/Release-iphonesimulator/libGrooveCore.a" -headers "$root/platform/ios/GrooveCore/include" \
  -output "$out/GrooveCore.xcframework"
echo "Built $out/GrooveCore.xcframework"
