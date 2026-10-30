# Groove native audio engine

A shared C++17 audio core for Groove on Android and iOS: decoding (FLAC, WAV/AIFF/W64/CAF, Ogg, MP3, DSF/DFF,
plus AAC/ALAC via the platform), gapless playback, a bit-perfect output path, a 10-band parametric EQ, a byte-level
tag parser, a background SQLite library scanner and smart playlists. It sits **under** the existing Groove UI. The
web UI in `mobile-app/` stays exactly as it is and is hosted by the native shells.

## Status: what is verified and what isn't

| Part | State |
|---|---|
| `core/` (engine, decoders, DSP, metadata, library) | **Built and tested on Linux**: GCC and Clang with `-Werror`, 33 unit tests, AddressSanitizer + UndefinedBehaviorSanitizer + LeakSanitizer, ThreadSanitizer, Valgrind memcheck (0 errors, every allocation freed), libFuzzer. Run `scripts/validate-core.sh`. |
| Mobile dependency stack (`GROOVE_FETCH_DEPS`) | **Built and tested**: libFLAC 1.4.3, libsndfile 1.2.2 (patched), SQLite 3.49.2 fetched from pinned sources; all tests pass under ASan on it. |
| Android JNI + AAudio (`platform/android/jni`) | **Type-checked** against the real AAudio header with `-Werror` (`scripts/android-check/check.sh`). **Not yet built with the NDK or run on a device.** |
| Android Kotlin (`platform/android/kotlin`) | Written; **not compiled** (no Kotlin compiler was available where this was written). |
| iOS Obj-C++ / Swift (`platform/ios`) | Written; **not compiled** (needs Xcode on a Mac). |
| Hooking the web UI to the native engine | **Not done yet.** The shipping app (Groove.apk) still plays through the WebView. See roadmap phase 3. |

"Zero runtime errors" is the design goal, enforced by the rules below and checked by the tools above. No test
suite can prove there are no bugs. Real devices will find things that desktop tests can't, so phase 4 exists.

## Directory layout

```
core/                              shared C++ engine: no platform headers, builds everywhere
  include/groove/
    AudioEngine.h                  thread-safe engine: FSM, command queue, gapless, bit-perfect path
    AudioOutput.h                  output interface (AAudio / CoreAudio / Null)
    Decoder.h  ByteSource.h        decoder interface, scoped file access (UniqueFd, pread)
    Equalizer.h                    10-band parametric EQ, headroom modes, lock-free TripleBuffer
    SpscRing.h                     lock-free single-producer/single-consumer ring
    Metadata.h                     ID3v2/v1, Vorbis comments, MP4 atoms, RIFF, AIFF, DSF, DFF parser
    Library.h                      SQLite index, background scanner, smart playlists
    NullOutput.h  Result.h  Types.h
  src/{engine,dsp,decode,io,meta,library,output}/
  tests/                           doctest suite + tagged fixture files (make_fixtures.sh regenerates them)
  fuzz/                            libFuzzer targets (metadata, decoders)
  tools/groove_cli.cpp             meta / render / scan / hires from a terminal
  third_party/patches/             fixes applied to fetched dependencies
platform/
  android/
    jni/GrooveJni.cpp              JNI bridge (handle registry, RegisterNatives, weak listener)
    jni/AAudioOutput.{h,cpp}       AAudio output
    jni/CMakeLists.txt             NDK build of libgroove.so
    kotlin/app/groove/engine/      GrooveNative, GrooveEngine, GrooveLibrary, SafFileOpener, BitPerfectRouting
    proguard-groove.pro            R8 keep rules for JNI names
  ios/
    GrooveCore/GRVEngine.{h,mm}    Objective-C++ bridge (pure Obj-C header for Swift)
    GrooveCore/CoreAudioOutput.*   RemoteIO + AVAudioSession output, AudioToolbox AAC/ALAC decoder
    Swift/GrooveEngine.swift       ObservableObject wrapper
    Swift/WebHostView.swift        SwiftUI host for the existing web UI
    CMakeLists.txt
android/                           the current shipping Java WebView shell (unchanged)
mobile-app/                        the current web UI (unchanged)
scripts/validate-core.sh           every check below, in order
scripts/android-check/check.sh     JNI/AAudio compile check without an NDK
scripts/build-ios-xcframework.sh   GrooveCore.xcframework
```

## Architecture

### Threads

```
UI / JNI / Obj-C++ ──post(cmd)──▶ [command queue] ──▶ CONTROL thread ──── the only thread that changes state
                                                        │  opens files, (re)opens the output, runs the FSM
                                                        ▼
                                     DECODER thread ──write──▶ [SPSC ring ≈2 s] ──read──▶ RENDER callback (OS RT thread)
                                     current/next decoder      [marker ring]               no locks, no allocation,
                                     pointer swap at EOF                                    no I/O, noexcept
```

* **Commands are serialised.** `play()`, `seek()` and the rest only enqueue. The control thread executes them one at a
  time, so two UI threads can never interleave half a transition.
* **The render callback is real-time safe.** It reads the lock-free ring and applies EQ/volume only if active. It
  converts with exact shifts and flags events through atomics; the control thread polls them every 10 ms.
* **Parameter changes are lock-free.** EQ coefficients reach the audio thread through a triple buffer, so it never
  waits and never sees a half-written filter.

### State machine

`Stopped → Loading → Playing ⇄ Paused`, with `Error` reachable from any loaded state.
`AudioEngine::isTransitionAllowed()` is the single table. An invalid request is ignored, never thrown. Seek keeps
Playing/Paused and refills the pipeline instead.

### Gapless (double-buffered)

The app calls `setNext()` when a track starts. The engine opens and parses the next file ahead of time. At end of
stream the decoder thread swaps its current and next pointers and keeps writing into the same ring. A marker tells
the render thread exactly which sample the new track starts on, which drives the "track started" event and position.
Tested bit-exact: track A followed by track B arrives at the device with no sample missing, added or changed.

If the next track has a different sample rate, staying bit-perfect means reopening the output at that rate. That
transition has a short clean pause instead of a resample. A `setNext()` arriving in the last ~150 ms still plays,
after a brief restart.

### Bit-perfect

With the EQ transparent and volume at 1.0, samples go decoder → ring → device buffer with no arithmetic: 16-bit to
Int16 by shift, ≤24-bit to Float32 exactly, 24-bit packed or 32-bit by shift/copy. Tests check every sample.
`EngineSnapshot::bitPerfect` is only true when all of these hold:
- the output reports no OS resampling;
- the device rate equals the source rate;
- the device format holds every source bit;
- no DSP is active;
- the source isn't DSD.

**What the OS does after that is outside any app's control.** Here is what Groove can honestly achieve:

| Platform | Bit-perfect? |
|---|---|
| Android 8–13 (Poco F1 on Android 10) | **No.** Music goes through the system mixer, which runs at its own rate (usually 48 kHz) and mixes in float. AAudio EXCLUSIVE/MMAP can bypass it on some devices, but only at the mixer's native rate. Groove reports `osResamples = true`. |
| Android 14+ with a USB DAC | **Yes**, via `AudioManager.setPreferredMixerAttributes(… MIXER_BEHAVIOR_BIT_PERFECT …)` (`BitPerfectRouting.kt`). |
| iOS with a USB DAC | **Yes** in practice: `setPreferredSampleRate` switches the DAC to the track's rate. Keep the system volume at 100 %. |
| iOS built-in speaker / Bluetooth | No: a fixed hardware rate (48 kHz) and codec processing. Reported as `osResamples`. |

Software volume and EQ switch bit-perfect off, and the snapshot says so. For bit-perfect listening, use the
device's hardware volume.

### EQ without clipping

The EQ is a 10-band parametric biquad cascade (RBJ, transposed direct form II, double precision) with denormal
flushing. It has two headroom modes:
- **PeakResponse** (default): the preamp drops by the highest point of the combined response. Full-scale sines can't
  clip; rare transient overshoot is clamped and counted in `clippedSamples`.
- **Guaranteed**: the preamp drops by the L1 norm of the impulse response, which is mathematically the worst-case
  gain. It cannot clip for any input (tested with the adversarial signal), but boosts come out quieter.

Integer outputs get TPDF dither whenever DSP is active.

### DSD

DSF and DFF are parsed natively. DST-compressed DFF is rejected cleanly. DSD is converted to 24-bit PCM at 176.4 kHz
(or 192 kHz for 48 kHz-family rates) by a linear-phase Kaiser FIR (−6 dB at 40 kHz, ~80 dB stopband) applied with
byte lookup tables. Tested: a 1 kHz DSD64 tone decodes at the right level, with harmonics below −60 dB and noise
below −80 dB. DoP (sending DSD to the DAC untouched) needs a truly bit-perfect path and a DAC that supports it, so it
is roadmap phase 5. DSD is never reported as bit-perfect.

### File I/O and storage removal

Decoders read through `ByteSource`: an owned descriptor (`UniqueFd`) with `pread()` and a 256 KB read-ahead.
**`mmap` is deliberately not used.** On removable storage, pulling the SD card or USB drive under a memory-mapped
file raises `SIGBUS`, which can't be recovered safely. With `pread()` it is an ordinary `ErrorCode::Io`.

Reads only happen on the decoder and scanner threads, never the audio thread, and the ring keeps ~2 s buffered, so
blocking I/O can't cause an underrun.

On a read failure the engine reports `onTrackFailed` and **falls back to the preloaded next track**. With no next
track it moves to `Error`, and a new `load()` recovers. On output loss (headphones unplugged, DAC removed, AAudio
disconnect) it reopens the output up to 3 times with backoff, then reports `DeviceLost`. All of this is covered by tests.

### Memory and exceptions

* No `new`/`delete`/`malloc` in Groove code. Everything is owned by `unique_ptr`/`shared_ptr`/containers, including
  C handles: `FLAC__StreamDecoder`, `SNDFILE`, `sqlite3`, statements, `AAudioStream`, `AudioUnit`, file descriptors.
  Private constructors use a passkey so objects can only come from `create()`/`open()` factories.
* No exception crosses a module boundary. Public API is `noexcept` and returns `Result<T>`. Thread bodies catch
  everything and turn it into `ErrorCode::Internal` plus the Error state. The render path never allocates or throws.
* Valgrind: every allocation in the test run is freed. LeakSanitizer agrees.

### Library index

The scanner uses `std::filesystem` with error codes only, on desktop/iOS. On Android, Kotlin lists the SAF tree
(`GrooveLibrary.listTree`) and the scanner reads tags through file descriptors.

Each scan runs on its own thread with its own SQLite connection (WAL mode), so the UI keeps querying during a scan.
That is tested.

- **Incremental:** files whose size and modification time haven't changed aren't re-read.
- **Batched:** 100 files per transaction.
- **Safe deletes:** rows are only removed after a complete walk, so a drive removed mid-scan doesn't wipe the index.

Smart playlists compile to SQL from whitelisted columns and operators only. Every value is a bound parameter, so an
injection attempt is just a string that matches nothing (tested).

## Bridging rules

### Android (JNI)

* **Handles, not pointers.** Kotlin holds a `Long` id into a native registry of `shared_ptr`s. Stale or
  double-closed handles are no-ops.
* **Load-time checking.** `RegisterNatives` runs with explicit signatures in `JNI_OnLoad`. Callback method IDs are
  cached there too. Any Kotlin/C++ mismatch fails at `System.loadLibrary`, and `GrooveEngine.create()` returns null,
  so the app falls back to the web player.
* **PCM never crosses JNI during playback.** For visualisers, `readTap()` copies into a direct `ByteBuffer` that
  Kotlin allocated once (`GetDirectBufferAddress`). No `GetByteArrayElements`, no critical sections, no GC pinning.
* **Weak references both ways.** Native callbacks go through a `java.lang.ref.WeakReference`, so native code never
  keeps a Kotlin object alive. Kotlin keeps its listener weakly too.
* **No exceptions leak across.** Kotlin exceptions inside callbacks are cleared, and strings are converted as real
  UTF-16, not "modified UTF-8".
* **Thread management.** Engine threads attach to the JVM once and detach automatically when they exit.
* **File descriptors.** SAF descriptors are detached in Kotlin and owned by C++ (`UniqueFd`).

### iOS (Objective-C++)

* **Swift-friendly header.** `GRVEngine.h` is pure Objective-C, so Swift imports it. All C++ lives in `.mm`.
* **No retain cycles.**
  - Engine → listener is a `std::weak_ptr`; listener → `GRVEngine` is `__weak`.
  - Delegate calls hop to the main queue capturing only the weak reference.
  - `WKUserContentController` retains its handlers strongly, so `WeakScriptMessageHandler` breaks the
    WebView → controller → handler cycle.
* **Real-time safe callback.** The RemoteIO render callback is plain C: no Objective-C messaging, ARC or allocation.
* **Route changes.** Route-change and media-services-reset notifications make the engine reopen at the new device's
  rate. Unplugged headphones pause playback first.

### Keeping the UI unchanged

Both shells keep showing `mobile-app/` (Android `WebView`, iOS `WKWebView` in SwiftUI). The JS ↔ native contract is
the one the Android app already uses: `GrooveNative.*` calls, and `window.grooveRemote(…)` events. Phase 3 adds a
`NativeEngine` adapter in `engine.js` with the same interface as today's `<audio>` engine, chosen at runtime when
the native engine is available. The screens don't change.

## Roadmap

**Phase 0 — done here.** Core engine, decoders, metadata, library and smart playlists. Sanitizers, Valgrind and
fuzzing are clean. Bridges written and the Android one type-checked. One upstream libsndfile leak found and patched.

**Phase 1 — Android build (≈1 week).**
1. Install Android Studio, NDK r26+ and CMake 3.22+.
2. Move the shell from `android/build.sh` to Gradle (AGP). Kotlin and `externalNativeBuild` need it. Point
   `externalNativeBuild.cmake.path` at `platform/android/jni/CMakeLists.txt`, add `platform/android/kotlin` to
   source sets and add `proguard-groove.pro`. ABIs: `arm64-v8a`, `armeabi-v7a`.
3. Raise `minSdk` to 26 for the engine (AAudio). On older phones `GrooveEngine.create()` returns null and the web
   player keeps working.
4. CI: `./gradlew assembleRelease`, plus `scripts/validate-core.sh` on Linux.

**Phase 2 — iOS build (≈1 week).**
1. `scripts/build-ios-xcframework.sh` (uses `xcodebuild -create-xcframework`).
2. Create an Xcode app target that embeds `GrooveCore.xcframework` and a bridging header importing `GRVEngine.h`.
   Bundle `mobile-app/` as a `web` folder reference and use `WebHostView` as the root view.
3. Licensing: ship libsndfile (LGPL-2.1) as a dynamic framework, or replace that decoder with AudioToolbox for
   WAV/AIFF.
4. CI: `xcodebuild test` on a simulator.

**Phase 3 — connect the UI (≈1 week).**
- Add a `NativeEngine` adapter in `engine.js` (load/play/pause/seek/setNext and events).
- Move the queue's "next track" to `setNext()` for gapless.
- Use `GrooveLibrary` for scanning on phones; IndexedDB stays for UI state.
- The Mix screen (two decks with tempo sync) stays on Web Audio until the native engine gets a second deck and
  time-stretching (phase 5).

**Phase 4 — device validation.**
- **Android:**
  - HWAddressSanitizer build (`-DGROOVE_SANITIZE=hwaddress`, arm64, Android 14+), or ASan with `wrap.sh` on older
    devices.
  - StrictMode in debug builds.
  - LeakCanary for the Kotlin side.
  - Perfetto traces to confirm no callback overruns.
- **iOS:** Xcode Address/Thread/Undefined sanitizers, Zombies, and Instruments (Leaks, Allocations, Audio System
  Trace).
- **Test plan, on a Poco F1 (Android 10) and a recent Pixel/iPhone with a USB DAC:**
  - 16/44.1, 24/96 and 24/192 files, plus DSD64/128;
  - pulling the SD card or DAC mid-song;
  - incoming calls;
  - screen off for an hour;
  - a 20 000-file scan.
- Valgrind doesn't run on Android/iOS. HWASan/ASan and Instruments are the on-device equivalents.

**Phase 5 — audiophile extras.**
- DoP output on bit-perfect paths.
- Native AAC/ALAC on Android via NDK `AMediaCodec`.
- ReplayGain from tags.
- Crossfade and a second deck in the native engine.
- Optional upsampling with a proper SRC.

## Licences

| Component | Licence | Notes |
|---|---|---|
| libFLAC | BSD-3 | Static linking is fine. |
| libsndfile | LGPL-2.1 | Dynamic linking (or replacement) needed for iOS App Store builds. |
| SQLite | Public domain | |
| doctest | MIT | Tests only. |
| Hanken Grotesk | OFL | Already in the web UI. |
