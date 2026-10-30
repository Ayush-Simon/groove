# Groove

A web player for your own audio files with a bold, colourful design that works on phones, tablets and laptops. It has DJ-style waveforms, key and BPM detection, crossfading and a two-deck Mix screen.

- **Plays files as they are.** FLAC, ALAC, WAV, AIFF, MP3, AAC and Opus go straight to the browser's audio element. No EQ, no resampling, no "enhancement".
- **Honest file info.** Format, sample rate, bit depth and bitrate are read from the file itself. If something isn't in the file, Groove doesn't make it up.
- **DJ waveform.** A 3-band waveform (blue = bass < 250 Hz, orange = mids, white = highs > 4 kHz) with a beat grid, a scrolling zoomed view and a full-track overview you can click to seek.
- **Key and BPM.** Detected in a background worker. Keys are shown as names and Camelot codes (e.g. `A minor · 8A`). If a track has no steady beat or no clear key, Groove shows "—" instead of guessing. You can double or halve the BPM from Now Playing.
- **Nothing is copied.** Added songs play straight from the files on your disk. In the browser, Groove only keeps references to them in memory, so it uses no extra storage, and the list (with playlists and analysis) is cleared when you close or reload the tab. In the Android app the library is kept (see below). Your original files are never touched. Only small settings (volume, EQ, crossfade) are remembered. Copies saved by older versions (named Aura) are deleted automatically.
- **Equaliser.** 10 bands (31 Hz – 16 kHz) with presets: Flat, Bass Boost, Bass Reducer, Treble Boost, Vocal, Acoustic, Electronic, Hip-Hop, Rock, Pop, Jazz, Classical, Late Night, Small Speakers. Drag any band for a custom curve. The level is lowered automatically by the curve's peak boost, so nothing clips.
- **Sound stage.** *3D Spatial* places the music around you, with an ambient tail behind and above. *Headphones* moves the sound out of your head onto two virtual speakers in front of you. *Speakers* widens the stereo image on laptop or desktop speakers. All three have an intensity control. The 3D and Headphones modes use the browser's HRTF (head-related) spatialiser. They aren't Dolby Atmos, which is licensed technology a web page can't use.
- **Library views.** Songs, Artists, Albums and Playlists tabs, with artist and album pages. Tap the heart to add a song to Loved Songs.
- **Crossfade.** In Playback settings (the sliders button), set a 1–12 s crossfade. Near the end of each song, Groove fades into the next one. When both tempos are close, it waits for the next bar and lines up the beats (pitch stays the same).
- **Mix screen.** Cue *any* song on the Next deck while the current one keeps playing. You can preview it ("Listen"), scrub its waveform to pick a starting point, sync its tempo and beats, then crossfade by hand or press **Mix** for an automatic 4/8/16/32 s blend. "What fits next" ranks your library by Camelot key match and tempo difference. In Chrome on a computer you can send the preview to a second output, such as headphones.
- **Every screen size.** Laptop: library and Now Playing side by side. Tablet and phone: mini player plus a full-screen Now Playing. Lock-screen and headphone-button controls work too.

## Run it

```bash
python3 server.py           # http://localhost:8080
python3 server.py 9000      # pick another port
python3 server.py --lan     # also open it from your phone on the same Wi-Fi
```

Then drag music files (or a whole folder) onto the page, or click **Add Music**.

## Browser support

| Format | Chrome / Edge | Safari | Firefox |
|---|---|---|---|
| FLAC, WAV, MP3, AAC (.m4a), Opus | ✓ | ✓ | ✓ |
| ALAC (.m4a) | ✗ | ✓ | ✗ |
| AIFF | ✗ | ✓ | ✗ |

The browser hands audio to your operating system, which decides the final output sample rate. A browser can't take exclusive control of a DAC. With the equaliser off and the sound stage off, outside crossfades and mixes, files play untouched. Turning on the equaliser or a sound stage routes audio through the browser's audio mixer. During a fade, Groove only changes each song's volume (and, when beat-syncing, its speed by a few percent with pitch kept). On iPhone and iPad, Safari doesn't let pages change an audio element's volume, so turning on crossfade or using Mix routes playback through Safari's audio mixer.

## Project layout

```
server.py                 Local server: serves mobile-app/ only, supports seeking (HTTP Range)
mobile-app/
  index.html              App shell + icon sprite
  styles.css              Colour themes; phone, tablet and laptop layouts
  fonts/                  Hanken Grotesk (SIL Open Font License), self-hosted
  js/app.js               UI, library views, playlists, Now Playing, crossfade, Mix screen
  js/sound.js             10-band EQ, presets, sound stage (3D Spatial / Headphones / Speakers)
  js/engine.js            Two-deck playback: equal-power fades, auto crossfade, cue/preview, tempo sync + beat lock
  js/metadata.js          Tag/format/artwork reader (FLAC, ID3/MP3, MP4/ALAC/AAC, WAV, AIFF, Ogg/Opus)
  js/storage.js           In-memory, session-only library (file references, nothing written to disk)
  js/analyzer.js          Decodes a track at 22.05 kHz for analysis (playback always uses the original)
  js/analyzer-worker.js   3-band waveform, BPM + beat grid, key detection (runs off the main thread)
  js/waveform.js          Deck (scrolling) and overview waveform canvases
sample-music/             Synthetic test tones (not real music) for quick testing
_archive/                 Earlier prototype code, kept for reference; not used
```

## Security

- The server only serves files inside `mobile-app/`, blocks `../` paths and hidden files, and listens on `127.0.0.1` unless you pass `--lan`.
- A strict Content Security Policy blocks inline scripts, and song tags are always inserted as plain text, never as HTML.

## Android app

`android/` wraps the web app in a native Android shell (Android 7+, built for Android 10):
the web files are bundled inside the APK. You add a music folder (or single songs) with the
system picker; Groove keeps Android's read permission for them and streams the files from
storage with seeking support (nothing is copied). The library — song details, key/BPM and
waveform analysis, playlists, cover thumbnails — is saved, so it's there next time, and new
songs put into a saved folder are picked up on launch. Remove folders or clear the library
in Settings. Android 10 only remembers about 128 single files per app, so folders are the
better choice for a big library. A foreground media service gives a notification,
lock-screen controls, headphone/Bluetooth buttons, audio focus (pauses for calls) and
pause-on-unplug. The back button closes sheets/pages, then sends the app to the background
without stopping the music.

Build without Android Studio: `TOOLS=/path/to/tools android/build.sh` (needs aapt2,
android-29.jar, dx.jar and apksigner.jar — see the comments in the script). The output is
`android/build/Groove.apk`. Keep `android/keystore/` safe: future updates must be signed with
the same key or Android will refuse to install them over the old version.

## Native audio engine (in progress)

`core/` is a shared C++ engine for the Android and iOS apps: gapless playback, a bit-perfect output path where the
OS allows one, a 10-band parametric EQ that can't clip, DSD (DSF/DFF), a byte-level tag parser and a background
SQLite library with smart playlists. `platform/` holds the JNI/AAudio (Android) and Objective-C++/CoreAudio (iOS)
bridges. The current web UI stays as it is on top.

```bash
scripts/validate-core.sh            # build + 33 tests + sanitizers + Valgrind + fuzzing + Android compile check
cmake -S core -B build -DGROOVE_TOOLS=ON && cmake --build build
build/groove-cli meta song.flac     # tags and stream info
```

Design, platform limits (when bit-perfect is and isn't possible) and the roadmap: [docs/ENGINE.md](docs/ENGINE.md).

