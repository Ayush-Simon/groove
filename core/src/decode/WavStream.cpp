#include "groove/WavStream.h"

#include <algorithm>
#include <cstring>
#include <new>

namespace groove {
namespace {
void put16(std::vector<uint8_t>& v, uint32_t x) { v.push_back(static_cast<uint8_t>(x)); v.push_back(static_cast<uint8_t>(x >> 8)); }
void put32(std::vector<uint8_t>& v, uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back(static_cast<uint8_t>(x >> (8 * i))); }
void tag(std::vector<uint8_t>& v, const char* t) { v.insert(v.end(), t, t + 4); }
}  // namespace

WavStream::WavStream(PrivateTag, std::unique_ptr<Decoder> dec, uint16_t outBits) : dec_(std::move(dec)), outBits_(outBits) {
    const StreamFormat& f = dec_->format();
    blockAlign_ = static_cast<uint32_t>(f.channels) * (outBits_ / 8);
    frames_ = dec_->totalFrames();
    dataBytes_ = frames_ * blockAlign_;
    // WAVE_FORMAT_EXTENSIBLE (needed for >16-bit or >2 channels to be unambiguous).
    const bool ext = outBits_ > 16 || f.channels > 2;
    const uint32_t fmtSize = ext ? 40 : 16;
    header_.reserve(12 + 8 + fmtSize + 8);
    tag(header_, "RIFF");
    put32(header_, static_cast<uint32_t>(std::min<uint64_t>(0xFFFFFFFFu, 4 + 8 + fmtSize + 8 + dataBytes_)));
    tag(header_, "WAVE");
    tag(header_, "fmt ");
    put32(header_, fmtSize);
    put16(header_, ext ? 0xFFFE : 1);
    put16(header_, f.channels);
    put32(header_, f.sampleRate);
    put32(header_, f.sampleRate * blockAlign_);
    put16(header_, blockAlign_);
    put16(header_, outBits_);
    if (ext) {
        put16(header_, 22);
        put16(header_, outBits_);                                    // valid bits
        static const uint32_t masks[9] = {0, 0x4, 0x3, 0x7, 0x33, 0x37, 0x3F, 0x13F, 0x63F};
        put32(header_, masks[std::min<uint16_t>(f.channels, 8)]);
        // KSDATAFORMAT_SUBTYPE_PCM {00000001-0000-0010-8000-00aa00389b71}
        static const uint8_t guid[16] = {1, 0, 0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xAA, 0, 0x38, 0x9B, 0x71};
        header_.insert(header_.end(), guid, guid + 16);
    }
    tag(header_, "data");
    put32(header_, static_cast<uint32_t>(std::min<uint64_t>(0xFFFFFFFFu, dataBytes_)));
}

Result<std::unique_ptr<WavStream>> WavStream::open(std::unique_ptr<ByteSource> src) noexcept {
    using R = Result<std::unique_ptr<WavStream>>;
    auto d = openDecoder(std::move(src));
    if (!d) return d.error();
    const StreamFormat f = d.value()->format();
    if (d.value()->totalFrames() == 0) return R::fail(ErrorCode::Unsupported, "length unknown: can't build a seekable stream");
    const uint16_t bits = f.bitsPerSample <= 16 ? 16 : f.bitsPerSample <= 24 ? 24 : 32;
    if (d.value()->totalFrames() * f.channels * (bits / 8) > 0xFFFFFFF0ull) return R::fail(ErrorCode::Unsupported, "longer than a WAV file can describe");
    try {
        auto w = std::make_unique<WavStream>(PrivateTag{}, std::move(d).value(), bits);
        w->pcm_.resize(static_cast<size_t>(kBlockFrames) * f.channels);
        w->packed_.resize(static_cast<size_t>(kBlockFrames) * w->blockAlign_);
        return R(std::move(w));
    } catch (const std::bad_alloc&) {
        return R::fail(ErrorCode::OutOfMemory, "wav stream");
    }
}

Result<void> WavStream::fill(uint64_t frame) noexcept {
    if (frame != nextDecoderFrame_) {
        auto s = dec_->seek(frame);
        if (!s) return s;
        nextDecoderFrame_ = frame;
    }
    const uint64_t want64 = std::min<uint64_t>(kBlockFrames, frames_ - frame);
    const uint32_t want = static_cast<uint32_t>(want64);
    uint32_t got = 0;
    while (got < want) {
        auto r = dec_->read(pcm_.data() + static_cast<size_t>(got) * format().channels, want - got);
        if (!r) return r.error();
        if (r.value() == 0) break;
        got += r.value();
    }
    nextDecoderFrame_ = frame + got;
    // A stream shorter than its header said (truncated file): pad with silence so the byte count stays true.
    std::fill(pcm_.begin() + static_cast<std::ptrdiff_t>(got) * format().channels,
              pcm_.begin() + static_cast<std::ptrdiff_t>(want) * format().channels, 0);
    const size_t n = static_cast<size_t>(want) * format().channels;
    uint8_t* o = packed_.data();
    switch (outBits_) {
        case 16:
            for (size_t i = 0; i < n; ++i) { const int32_t v = pcm_[i] >> 16; o[2 * i] = static_cast<uint8_t>(v); o[2 * i + 1] = static_cast<uint8_t>(v >> 8); }
            break;
        case 24:
            for (size_t i = 0; i < n; ++i) { const int32_t v = pcm_[i] >> 8; o[3 * i] = static_cast<uint8_t>(v); o[3 * i + 1] = static_cast<uint8_t>(v >> 8); o[3 * i + 2] = static_cast<uint8_t>(v >> 16); }
            break;
        default:
            for (size_t i = 0; i < n; ++i) { const uint32_t v = static_cast<uint32_t>(pcm_[i]); for (int b = 0; b < 4; ++b) o[4 * i + b] = static_cast<uint8_t>(v >> (8 * b)); }
            break;
    }
    blockFrame_ = frame;
    blockFrames_ = want;
    return Result<void>::success();
}

Result<size_t> WavStream::read(uint64_t offset, uint8_t* dst, size_t len) noexcept {
    const uint64_t total = size();
    if (offset >= total || len == 0) return size_t{0};
    len = static_cast<size_t>(std::min<uint64_t>(len, total - offset));
    size_t done = 0;
    while (done < len) {
        const uint64_t pos = offset + done;
        if (pos < header_.size()) {
            const size_t n = std::min<size_t>(len - done, header_.size() - static_cast<size_t>(pos));
            std::memcpy(dst + done, header_.data() + pos, n);
            done += n;
            continue;
        }
        const uint64_t dataPos = pos - header_.size();
        const uint64_t frame = dataPos / blockAlign_;
        if (blockFrame_ == UINT64_MAX || frame < blockFrame_ || frame >= blockFrame_ + blockFrames_) {
            // Blocks are aligned to multiples of kBlockFrames so sequential reads never re-decode.
            auto r = fill(frame - frame % kBlockFrames);
            if (!r) return r.error();
            if (blockFrames_ == 0) break;
        }
        const uint64_t inBlock = dataPos - blockFrame_ * blockAlign_;
        const size_t avail = static_cast<size_t>(static_cast<uint64_t>(blockFrames_) * blockAlign_ - inBlock);
        const size_t n = std::min(len - done, avail);
        std::memcpy(dst + done, packed_.data() + inBlock, n);
        done += n;
    }
    return done;
}

}  // namespace groove
