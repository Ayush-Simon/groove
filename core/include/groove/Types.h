// Groove audio core — shared value types.
// No platform headers here: this file compiles unchanged for Android NDK, iOS and desktop tests.
#pragma once

#include <cstdint>
#include <string>

namespace groove {

/// Playback finite state machine. See AudioEngine.h for the allowed transitions.
enum class PlaybackState : uint8_t { Stopped, Loading, Playing, Paused, Error };

const char* stateName(PlaybackState s) noexcept;

enum class ErrorCode : int32_t {
    None = 0,
    FileNotFound,      // path/URI doesn't exist or permission was revoked
    Io,                // read failed (storage removed, file truncated)
    Unsupported,       // codec/container we can't decode
    Corrupt,           // malformed stream
    DeviceLost,        // audio output disconnected / stream died
    DeviceOpenFailed,  // couldn't open an output with any format
    InvalidArgument,
    InvalidState,
    OutOfMemory,
    Internal,          // a bug: an unexpected exception was caught at a boundary
};

const char* errorName(ErrorCode e) noexcept;

/// Format of decoded PCM as delivered by a decoder.
/// Samples inside the engine are interleaved, LEFT-JUSTIFIED int32: full scale is INT32_MIN..INT32_MAX
/// whatever the source bit depth. `bitsPerSample` records how many of the top bits are significant,
/// so conversion to the output format can be exact (bit-perfect) when no DSP is active.
struct StreamFormat {
    uint32_t sampleRate = 0;
    uint16_t channels = 0;
    uint16_t bitsPerSample = 0;  // significant bits (16, 24, 32). DSD→PCM reports 24.
    bool fromDsd = false;        // decoded from 1-bit DSD (conversion, therefore never bit-perfect)

    bool valid() const noexcept { return sampleRate >= 8000 && sampleRate <= 768000 && channels >= 1 && channels <= 8 && bitsPerSample >= 8 && bitsPerSample <= 32; }
    /// Two streams can be played back-to-back on one output stream without reopening it.
    bool gaplessCompatible(const StreamFormat& o) const noexcept { return sampleRate == o.sampleRate && channels == o.channels; }
    bool operator==(const StreamFormat& o) const noexcept { return sampleRate == o.sampleRate && channels == o.channels && bitsPerSample == o.bitsPerSample && fromDsd == o.fromDsd; }
    bool operator!=(const StreamFormat& o) const noexcept { return !(*this == o); }
};

/// Sample formats an output device can accept.
enum class SampleFormat : uint8_t { Int16, Int24Packed, Int32, Float32 };

inline uint32_t bytesPerSample(SampleFormat f) noexcept {
    switch (f) {
        case SampleFormat::Int16: return 2;
        case SampleFormat::Int24Packed: return 3;
        case SampleFormat::Int32: return 4;
        case SampleFormat::Float32: return 4;
    }
    return 4;
}

/// A playable item. `uri` is a filesystem path on desktop/iOS, or a content:// URI on Android
/// (opened through the platform's SourceOpener, which hands the core a file descriptor).
struct TrackSource {
    uint64_t id = 0;
    std::string uri;
};

}  // namespace groove
