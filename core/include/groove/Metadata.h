// Byte-level tag + stream-info parser: ID3v2.2/2.3/2.4 (+ID3v1), FLAC metadata blocks, Vorbis comments
// (FLAC, Ogg Vorbis, Opus), MP4/M4A atoms, WAV (fmt, LIST INFO, id3), AIFF/AIFC (COMM, NAME, AUTH, ID3),
// DSF (fmt + trailing ID3) and DFF (PROP + ID3), MP3 frame headers with Xing/Info/VBRI.
//
// Every read is bounds-checked; malformed input yields partial results or an error, never a crash
// (fuzzed under AddressSanitizer: core/fuzz/fuzz_metadata.cpp). Unknown values stay 0/empty — nothing is guessed.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "groove/ByteSource.h"
#include "groove/Result.h"

namespace groove {

struct ArtworkRef {
    std::string mime;              // "image/jpeg", "image/png", …
    uint8_t pictureType = 0;       // ID3/FLAC picture type; 3 = front cover
    uint64_t offset = 0;           // where the image bytes start in the file (0 + inlineOnly if not addressable)
    uint64_t size = 0;
    bool inlineOnly = false;       // stored transformed (ID3 unsynchronisation, base64): only `data` is valid
    std::vector<uint8_t> data;     // filled when ParseOptions::loadArtwork or inlineOnly
};

struct TrackMetadata {
    std::string title, artist, album, albumArtist, genre, composer;
    int year = 0, trackNo = 0, trackTotal = 0, discNo = 0, discTotal = 0;

    std::string container;         // FLAC, MP3, MP4, WAV, AIFF, Ogg, DSF, DFF
    std::string codec;             // FLAC, ALAC, AAC, PCM, MP3, Vorbis, Opus, DSD, …
    uint32_t sampleRate = 0;       // for DSD: the 1-bit rate (2 822 400 = DSD64)
    uint16_t bitDepth = 0;         // 0 = not applicable (lossy) or unknown; DSD = 1
    uint16_t channels = 0;
    double durationSeconds = 0;
    uint32_t bitrateKbps = 0;
    bool lossless = false;

    std::optional<ArtworkRef> artwork;
    std::vector<std::pair<std::string, std::string>> extra;  // custom tags: TXXX, unknown Vorbis keys, MP4 '----'
};

struct ParseOptions {
    bool loadArtwork = false;
    size_t maxArtworkBytes = 16u << 20;
    size_t maxTagBytes = 64u << 20;   // tags larger than this are skipped
};

Result<TrackMetadata> readMetadata(ByteSource& src, const ParseOptions& opts = {}) noexcept;
/// Fetch artwork bytes later (e.g. when a cover scrolls into view) without re-parsing tags.
Result<std::vector<uint8_t>> readArtwork(ByteSource& src, const ArtworkRef& ref) noexcept;

}  // namespace groove
