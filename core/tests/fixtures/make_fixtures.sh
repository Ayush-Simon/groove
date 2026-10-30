#!/usr/bin/env bash
# Regenerates the small tagged test files used by test_metadata.cpp (needs ffmpeg with libmp3lame/libopus/libvorbis).
set -euo pipefail
cd "$(dirname "$0")"
F="ffmpeg -loglevel error -y"
$F -f lavfi -i "color=c=0x3f8c5f:s=32x32" -frames:v 1 cover.jpg
$F -f lavfi -i "color=c=0xf2aa3c:s=32x32" -frames:v 1 cover.png
SRC="-f lavfi -i sine=frequency=440:sample_rate=48000:duration=1 -ac 2"
TAGS=(-metadata "title=Groove Test" -metadata "artist=Test Artist" -metadata "album=Test Album" -metadata "album_artist=Album Artist"
      -metadata "genre=Electronic" -metadata "date=2024" -metadata "track=3/12" -metadata "disc=1/2")
$F $SRC -sample_fmt s32 -c:a flac "${TAGS[@]}" tmp.flac
$F -i tmp.flac -i cover.png -map 0:a -map 1 -c copy -disposition:v attached_pic -metadata:s:v comment="Cover (front)" tagged.flac
$F $SRC -c:a libmp3lame -b:a 128k -id3v2_version 3 "${TAGS[@]}" -metadata "MOOD=Happy" tmp.mp3
$F -i tmp.mp3 -i cover.jpg -map 0:a -map 1 -c copy -id3v2_version 3 -metadata:s:v comment="Cover (front)" tagged_v23.mp3
$F $SRC -c:a libmp3lame -b:a 128k -id3v2_version 4 "${TAGS[@]}" tagged_v24.mp3
$F $SRC -c:a aac -b:a 128k "${TAGS[@]}" tmp.m4a
$F -i tmp.m4a -i cover.jpg -map 0:a -map 1 -c copy -disposition:v attached_pic tagged_aac.m4a
$F -f lavfi -i sine=frequency=440:sample_rate=96000:duration=1 -ac 2 -sample_fmt s32p -c:a alac "${TAGS[@]}" tagged_alac.m4a
$F $SRC -c:a libvorbis -q:a 3 "${TAGS[@]}" tagged.ogg
$F $SRC -c:a libopus -b:a 96k "${TAGS[@]}" tagged.opus
$F -f lavfi -i sine=frequency=440:sample_rate=44100:duration=1 -ac 2 -c:a pcm_s24le "${TAGS[@]}" tagged.wav
$F -f lavfi -i sine=frequency=440:sample_rate=44100:duration=1 -ac 2 -c:a pcm_s16be -write_id3v2 1 "${TAGS[@]}" tagged.aiff
rm -f tmp.flac tmp.mp3 tmp.m4a
ls -la
