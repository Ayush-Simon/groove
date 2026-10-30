// Decoder interface. Implementations: FLAC (libFLAC), WAV/AIFF/W64/CAF/Ogg/Opus/MP3 (libsndfile),
// DSF/DFF (built-in DSD parser + DSD→PCM conversion). AAC/ALAC are delegated to the platform
// (AudioToolbox on iOS, NDK MediaCodec on Android) through registerPlatformDecoder().
#pragma once

#include <cstdint>
#include <functional>
#include <memory>

#include "groove/ByteSource.h"
#include "groove/Result.h"
#include "groove/Types.h"

namespace groove {

class Decoder {
public:
    virtual ~Decoder() = default;
    virtual const StreamFormat& format() const noexcept = 0;
    /// Total length in frames, 0 if unknown.
    virtual uint64_t totalFrames() const noexcept = 0;
    /// Decodes up to `frames` interleaved, left-justified int32 frames into `dst`
    /// (which holds frames * channels samples). Returns frames written; 0 = end of stream.
    virtual Result<uint32_t> read(int32_t* dst, uint32_t frames) noexcept = 0;
    virtual Result<void> seek(uint64_t frame) noexcept = 0;
    virtual const char* codecName() const noexcept = 0;
};

/// Opens the right decoder for the bytes (sniffs magic numbers; the file name is only a hint).
Result<std::unique_ptr<Decoder>> openDecoder(std::unique_ptr<ByteSource> src) noexcept;

/// Platform decoders get first refusal for containers the core doesn't handle (MP4/M4A).
using PlatformDecoderFactory = std::function<Result<std::unique_ptr<Decoder>>(std::unique_ptr<ByteSource>&)>;
void registerPlatformDecoder(PlatformDecoderFactory factory);

// Individual decoders (exposed for tests).
Result<std::unique_ptr<Decoder>> openFlacDecoder(std::unique_ptr<ByteSource> src) noexcept;
Result<std::unique_ptr<Decoder>> openSndfileDecoder(std::unique_ptr<ByteSource> src) noexcept;
Result<std::unique_ptr<Decoder>> openDsdDecoder(std::unique_ptr<ByteSource> src) noexcept;

}  // namespace groove
