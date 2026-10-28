import subprocess

# Track 1: 24-bit / 96kHz FLAC Audiophile Acoustic Harmony
cmd1 = [
    "ffmpeg", "-y", "-f", "lavfi",
    "-i", "anoisesrc=d=20:c=pink:r=96000:a=0.03",
    "-f", "lavfi",
    "-i", "sine=f=130.81:r=96000:d=20",
    "-f", "lavfi",
    "-i", "sine=f=196.00:r=96000:d=20",
    "-f", "lavfi",
    "-i", "sine=f=246.94:r=96000:d=20",
    "-f", "lavfi",
    "-i", "sine=f=329.63:r=96000:d=20",
    "-filter_complex", "[1:a][2:a][3:a][4:a]amix=inputs=4:duration=first:dropout_transition=2[tones];[tones]volume=0.4[vtones];[vtones][0:a]amix=inputs=2[out]",
    "-map", "[out]",
    "-c:a", "flac", "-sample_fmt", "s32", "-ar", "96000",
    "sample-music/chopin_harmonic_master_24bit_96k.flac"
]
subprocess.run(cmd1, check=True)

# Track 2: 24-bit / 96kHz High-Res Sub-Bass Boost Test
cmd2 = [
    "ffmpeg", "-y", "-f", "lavfi",
    "-i", "sine=f=50:r=96000:d=20",
    "-f", "lavfi",
    "-i", "sine=f=100:r=96000:d=20",
    "-filter_complex", "[0:a][1:a]amix=inputs=2[out]",
    "-map", "[out]",
    "-c:a", "flac", "-sample_fmt", "s32", "-ar", "96000",
    "sample-music/subbass_punch_test_24bit_96k.flac"
]
subprocess.run(cmd2, check=True)

# Track 3: 24-bit / 48kHz WAV Test Track
cmd3 = [
    "ffmpeg", "-y", "-f", "lavfi",
    "-i", "sine=f=440:r=48000:d=15",
    "-c:a", "pcm_s24le", "-ar", "48000",
    "sample-music/a440_reference_master_24bit.wav"
]
subprocess.run(cmd3, check=True)

print("Generated all audiophile test tracks successfully!")
