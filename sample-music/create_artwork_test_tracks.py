#!/usr/bin/env python3
"""
Generate test audio files with embedded album artwork:
1. FLAC file with METADATA_BLOCK_PICTURE and Vorbis comments (Title, Artist, Album)
2. MP3 file with ID3v2 APIC attached picture frame
"""

import os
import subprocess
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

PROJECT_ROOT = Path(__file__).resolve().parent.parent
SAMPLES_DIR = PROJECT_ROOT / "mobile-app" / "samples"
ASSETS_DIR = PROJECT_ROOT / "mobile-app" / "assets"
SAMPLES_DIR.mkdir(parents=True, exist_ok=True)

# 1. Create a distinct high-resolution album cover artwork
art_path = ASSETS_DIR / "neon_horizon_cover.jpg"
width, height = 1000, 1000
img = Image.new("RGB", (width, height), color=(10, 15, 26))
draw = ImageDraw.Draw(img)

# Draw dark aesthetic gradient background
for y in range(height):
    r = int(12 + (y / height) * 20)
    g = int(18 + (y / height) * 35)
    b = int(32 + (y / height) * 60)
    draw.line([(0, y), (width, y)], fill=(r, g, b))

# Draw vinyl-style concentric circles
center_x, center_y = 500, 480
for radius in range(50, 450, 24):
    alpha = int(30 + (radius % 50))
    draw.ellipse(
        [center_x - radius, center_y - radius, center_x + radius, center_y + radius],
        outline=(0, 229, 255, 60),
        width=1
    )

# Inner glowing disc
draw.ellipse([center_x - 130, center_y - 130, center_x + 130, center_y + 130], fill=(18, 24, 38), outline=(0, 229, 255), width=3)
draw.ellipse([center_x - 30, center_y - 30, center_x + 30, center_y + 30], fill=(5, 7, 10), outline=(255, 255, 255), width=2)

# Text labels
draw.text((80, 80), "NEON HORIZON", fill=(255, 255, 255))
draw.text((80, 110), "SOLARIS ENSEMBLE • 24-BIT MASTER", fill=(0, 229, 255))
draw.text((80, 900), "HI-RES AUDIO DIRECT HARDWARE MASTERING", fill=(160, 175, 200))
draw.text((80, 930), "FLAC 96kHz / 24-Bit Lossless PCM", fill=(0, 255, 136))

img.save(art_path, "JPEG", quality=95)
print(f"Created cover artwork: {art_path}")

# 2. Generate 24-bit 96kHz FLAC file with embedded METADATA_BLOCK_PICTURE
flac_out = SAMPLES_DIR / "neon_horizon_24bit_with_artwork.flac"
cmd_flac = [
    "ffmpeg", "-y",
    "-f", "lavfi", "-i", "sine=f=220:r=96000:d=12",
    "-i", str(art_path),
    "-map", "0:a", "-map", "1:v",
    "-c:a", "flac", "-sample_fmt", "s32", "-ar", "96000",
    "-c:v", "copy",
    "-metadata:s:v", 'title="Album cover"',
    "-metadata:s:v", 'comment="Cover (front)"',
    "-disposition:v", "attached_pic",
    "-metadata", "title=Neon Horizon (Audiophile 24-Bit Mix)",
    "-metadata", "artist=Solaris Ensemble",
    "-metadata", "album=Quantum Sound Series",
    "-metadata", "date=2024",
    str(flac_out)
]
subprocess.run(cmd_flac, check=True)
print(f"Created FLAC with embedded picture: {flac_out} ({os.path.getsize(flac_out)} bytes)")

# 3. Generate MP3 file with ID3v2 APIC cover artwork
mp3_out = SAMPLES_DIR / "midnight_groove_with_artwork.mp3"
cmd_mp3 = [
    "ffmpeg", "-y",
    "-f", "lavfi", "-i", "sine=f=330:r=44100:d=10",
    "-i", str(art_path),
    "-map", "0:a", "-map", "1:v",
    "-c:a", "libmp3lame", "-b:a", "320k",
    "-c:v", "copy",
    "-id3v2_version", "3",
    "-metadata:s:v", 'title="Album cover"',
    "-metadata:s:v", 'comment="Cover (front)"',
    "-disposition:v", "attached_pic",
    "-metadata", "title=Midnight Groove (Analog Master)",
    "-metadata", "artist=Aura Sound Lab",
    "-metadata", "album=Studio Sessions Vol. 1",
    str(mp3_out)
]
subprocess.run(cmd_mp3, check=True)
print(f"Created MP3 with embedded picture: {mp3_out} ({os.path.getsize(mp3_out)} bytes)")
