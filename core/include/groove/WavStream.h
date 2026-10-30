// Presents any decodable file as a seekable, byte-addressable PCM WAV stream.
//
// Used by the Android app: the WebView's <audio> element is given Groove's own decoded PCM instead of the
// original file, so playback no longer depends on the WebView's demuxers/decoders (the source of the
// stalls), and formats the WebView can't decode at all (DSD) play. Every byte range is produced on demand:
// read(offset) maps to a frame, seeks the decoder if needed and converts — nothing is decoded ahead or cached
// beyond one block, nothing is written to storage.
//
// Output: 16-bit for ≤16-bit sources, 24-bit for 17–24-bit (and DSD→PCM), 32-bit for 32-bit sources. PCM is
// written exactly (shifts only), so the WAV carries the decoder's samples bit-for-bit.
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "groove/Decoder.h"

namespace groove {

class WavStream {
    struct PrivateTag { explicit PrivateTag() = default; };

public:
    static Result<std::unique_ptr<WavStream>> open(std::unique_ptr<ByteSource> src) noexcept;
    WavStream(PrivateTag, std::unique_ptr<Decoder> dec, uint16_t outBits);

    uint64_t size() const noexcept { return header_.size() + dataBytes_; }
    uint32_t headerSize() const noexcept { return static_cast<uint32_t>(header_.size()); }
    const StreamFormat& format() const noexcept { return dec_->format(); }
    uint16_t outBits() const noexcept { return outBits_; }
    uint64_t frames() const noexcept { return frames_; }
    const char* codecName() const noexcept { return dec_->codecName(); }

    /// Copies up to len bytes starting at offset. Returns bytes copied (0 at the end) or an error.
    Result<size_t> read(uint64_t offset, uint8_t* dst, size_t len) noexcept;

private:
    Result<void> fill(uint64_t frame) noexcept;  // decode the block that starts at `frame`

    std::unique_ptr<Decoder> dec_;
    uint16_t outBits_;
    uint32_t blockAlign_;
    uint64_t frames_;
    uint64_t dataBytes_;
    std::vector<uint8_t> header_;
    std::vector<int32_t> pcm_;      // one decoded block
    std::vector<uint8_t> packed_;   // same block in output format
    uint64_t blockFrame_ = UINT64_MAX;  // first frame held in packed_
    uint32_t blockFrames_ = 0;
    uint64_t nextDecoderFrame_ = 0;     // where the decoder will read next (avoids needless seeks)
    static constexpr uint32_t kBlockFrames = 8192;
};

}  // namespace groove
