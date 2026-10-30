// DSF (Sony) and DFF/DSDIFF (Philips) bitstream parser + DSD→PCM conversion.
//
// A 1-bit DSD stream can't be sent to a PCM DAC as-is. Two ways exist:
//  * DoP (DSD over PCM): pack DSD bits into 24-bit PCM frames with marker bytes. Only works when the
//    path to the DAC is truly bit-perfect AND the DAC supports DoP — i.e. iOS with a USB DAC, or Android 14+
//    USB bit-perfect mixer mode. Planned (docs/ENGINE.md, roadmap phase 5).
//  * Conversion (this file): a linear-phase FIR low-pass applied with byte-wise lookup tables, decimating
//    to 176.4 kHz (DSD64/128/256/512 at 44.1k multiples) or 192 kHz (48k multiples). Output is 24-bit.
//    Converted audio is reported as fromDsd, so the engine never calls it bit-perfect.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <new>
#include <vector>

#include "groove/Decoder.h"

namespace groove {
namespace {

constexpr double kPi = 3.14159265358979323846;

uint32_t le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }
uint64_t le64(const uint8_t* p) { return le32(p) | (static_cast<uint64_t>(le32(p + 4)) << 32); }
uint32_t be32(const uint8_t* p) { return (static_cast<uint32_t>(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
uint64_t be64(const uint8_t* p) { return (static_cast<uint64_t>(be32(p)) << 32) | be32(p + 4); }
uint16_t be16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }

double besselI0(double x) {
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 64; ++k) {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
        if (term < sum * 1e-17) break;
    }
    return sum;
}

std::array<uint8_t, 256> makeBitReverse() {
    std::array<uint8_t, 256> t{};
    for (int i = 0; i < 256; ++i) {
        uint8_t r = 0;
        for (int b = 0; b < 8; ++b)
            if (i & (1 << b)) r |= static_cast<uint8_t>(1 << (7 - b));
        t[i] = r;
    }
    return t;
}
const std::array<uint8_t, 256> kBitReverse = makeBitReverse();

enum class Layout { Planar, Interleaved };  // DSF: per-channel blocks; DFF: byte-interleaved

struct DsdInfo {
    uint32_t dsdRate = 0;
    uint16_t channels = 0;
    bool lsbFirst = false;
    Layout layout = Layout::Interleaved;
    uint64_t dataOffset = 0;
    uint64_t bytesPerChannel = 0;   // usable bytes per channel
    uint32_t blockSize = 4096;      // bytes per channel per block (DSF) / cache window (DFF)
};

class DsdDecoder final : public Decoder {
public:
    explicit DsdDecoder(std::unique_ptr<ByteSource> src) : src_(std::move(src)) {}

    Result<void> init() {
        uint8_t h[12] = {};
        auto r = src_->readAt(0, h, sizeof h);
        if (!r || r.value() < 12) return Result<void>::fail(ErrorCode::Corrupt, src_->name() + ": too short");
        Result<void> pr = std::memcmp(h, "DSD ", 4) == 0 ? parseDsf() : std::memcmp(h, "FRM8", 4) == 0 ? parseDff() : Result<void>::fail(ErrorCode::Unsupported, "not DSD");
        if (!pr) return pr;
        if (info_.channels == 0 || info_.channels > 8 || info_.dsdRate < 2822400 || info_.dsdRate > 2822400 * 8 * 2)
            return Result<void>::fail(ErrorCode::Unsupported, src_->name() + ": unsupported DSD layout");
        const uint32_t outRate = (info_.dsdRate % 44100 == 0) ? 176400 : 192000;
        if (info_.dsdRate % outRate != 0) return Result<void>::fail(ErrorCode::Unsupported, src_->name() + ": odd DSD rate");
        decim_ = info_.dsdRate / outRate;  // bits per output sample: 16 (DSD64) … 128 (DSD512)
        if (decim_ % 8 != 0) return Result<void>::fail(ErrorCode::Unsupported, src_->name() + ": odd DSD rate");
        bytesPerOut_ = decim_ / 8;
        fmt_.sampleRate = outRate;
        fmt_.channels = info_.channels;
        fmt_.bitsPerSample = 24;
        fmt_.fromDsd = true;
        total_ = info_.bytesPerChannel / bytesPerOut_;
        designFilter();
        cache_.assign(static_cast<size_t>(info_.blockSize) * info_.channels, 0);
        hist_.assign(static_cast<size_t>(info_.channels) * groups_ * 2, 0x69);  // 0x69 = DSD idle pattern (mean 0)
        histPos_.assign(info_.channels, 0);
        return Result<void>::success();
    }

    const StreamFormat& format() const noexcept override { return fmt_; }
    uint64_t totalFrames() const noexcept override { return total_; }
    const char* codecName() const noexcept override { return info_.layout == Layout::Planar ? "DSD (DSF)" : "DSD (DFF)"; }

    Result<uint32_t> read(int32_t* dst, uint32_t frames) noexcept override {
        const uint16_t ch = info_.channels;
        uint32_t done = 0;
        while (done < frames && outPos_ < total_) {
            for (uint16_t c = 0; c < ch; ++c) {
                for (uint32_t b = 0; b < bytesPerOut_; ++b) {
                    auto byte = byteAt(c, outPos_ * bytesPerOut_ + b);
                    if (!byte) return Result<uint32_t>::fail(ErrorCode::Io, src_->name() + ": read error (storage removed?)");
                    push(c, byte.value());
                }
                dst[static_cast<size_t>(done) * ch + c] = filterOut(c);
            }
            ++outPos_;
            ++done;
        }
        return done;
    }

    Result<void> seek(uint64_t frame) noexcept override {
        if (total_ > 0 && frame >= total_) frame = total_ - 1;
        // Prime the filter history with the bytes just before the target so there's no click.
        const uint64_t target = frame * bytesPerOut_;
        const uint64_t from = target > groups_ ? target - groups_ : 0;
        std::fill(hist_.begin(), hist_.end(), uint8_t{0x69});
        std::fill(histPos_.begin(), histPos_.end(), 0u);
        for (uint16_t c = 0; c < info_.channels; ++c)
            for (uint64_t i = from; i < target; ++i) {
                auto byte = byteAt(c, i);
                if (!byte) return Result<void>::fail(ErrorCode::Io, src_->name() + ": read error");
                push(c, byte.value());
            }
        outPos_ = frame;
        return Result<void>::success();
    }

private:
    Result<void> parseDsf() {
        uint8_t h[28 + 52 + 12] = {};
        auto r = src_->readAt(0, h, sizeof h);
        if (!r || r.value() < sizeof h) return Result<void>::fail(ErrorCode::Corrupt, src_->name() + ": short DSF header");
        if (std::memcmp(h + 28, "fmt ", 4) != 0) return Result<void>::fail(ErrorCode::Corrupt, src_->name() + ": no fmt chunk");
        const uint8_t* f = h + 28;
        const uint64_t fmtSize = le64(f + 4);
        if (le32(f + 16) != 0) return Result<void>::fail(ErrorCode::Unsupported, src_->name() + ": compressed DSF");
        info_.channels = static_cast<uint16_t>(le32(f + 24));
        info_.dsdRate = le32(f + 28);
        const uint32_t bits = le32(f + 32);
        const uint64_t samples = le64(f + 36);
        info_.blockSize = le32(f + 44);
        info_.lsbFirst = bits == 1;
        if (bits != 1 && bits != 8) return Result<void>::fail(ErrorCode::Corrupt, src_->name() + ": bad DSF bit order");
        if (info_.blockSize == 0 || info_.blockSize > (1u << 20)) return Result<void>::fail(ErrorCode::Corrupt, src_->name() + ": bad block size");
        const uint64_t dataChunk = 28 + fmtSize;
        uint8_t d[12] = {};
        auto dr = src_->readAt(dataChunk, d, sizeof d);
        if (!dr || dr.value() < 12 || std::memcmp(d, "data", 4) != 0) return Result<void>::fail(ErrorCode::Corrupt, src_->name() + ": no data chunk");
        info_.layout = Layout::Planar;
        info_.dataOffset = dataChunk + 12;
        const uint64_t dataBytes = le64(d + 4) >= 12 ? le64(d + 4) - 12 : 0;
        const uint64_t avail = std::min<uint64_t>(dataBytes, src_->size() > info_.dataOffset ? src_->size() - info_.dataOffset : 0);
        const uint64_t perChannelStored = info_.channels ? avail / info_.channels : 0;
        info_.bytesPerChannel = std::min<uint64_t>(samples / 8, perChannelStored);  // ignore block padding
        return Result<void>::success();
    }

    Result<void> parseDff() {
        uint8_t h[16] = {};
        auto r = src_->readAt(0, h, sizeof h);
        if (!r || r.value() < 16 || std::memcmp(h + 12, "DSD ", 4) != 0) return Result<void>::fail(ErrorCode::Corrupt, src_->name() + ": not DSDIFF");
        const uint64_t end = std::min<uint64_t>(src_->size(), 12 + be64(h + 4));
        uint64_t off = 16;
        bool haveData = false, compressed = false;
        for (int guard = 0; guard < 256 && off + 12 <= end; ++guard) {
            uint8_t ck[12];
            auto cr = src_->readAt(off, ck, 12);
            if (!cr || cr.value() < 12) break;
            const uint64_t size = be64(ck + 4);
            const uint64_t body = off + 12;
            if (std::memcmp(ck, "PROP", 4) == 0) {
                // PROP: "SND " followed by sub-chunks FS, CHNL, CMPR …
                uint64_t p = body + 4;
                const uint64_t pend = std::min(end, body + size);
                for (int g2 = 0; g2 < 64 && p + 12 <= pend; ++g2) {
                    uint8_t sc[12 + 8] = {};
                    auto sr = src_->readAt(p, sc, sizeof sc);
                    if (!sr || sr.value() < 12) break;
                    const uint64_t ssize = be64(sc + 4);
                    if (std::memcmp(sc, "FS  ", 4) == 0) info_.dsdRate = be32(sc + 12);
                    else if (std::memcmp(sc, "CHNL", 4) == 0) info_.channels = be16(sc + 12);
                    else if (std::memcmp(sc, "CMPR", 4) == 0) compressed = std::memcmp(sc + 12, "DSD ", 4) != 0;
                    p += 12 + ssize + (ssize & 1);
                }
            } else if (std::memcmp(ck, "DSD ", 4) == 0) {
                info_.dataOffset = body;
                const uint64_t avail = std::min<uint64_t>(size, end > body ? end - body : 0);
                info_.bytesPerChannel = info_.channels ? avail / info_.channels : 0;
                haveData = true;
                break;
            } else if (std::memcmp(ck, "DST ", 4) == 0) {
                compressed = true;
            }
            off = body + size + (size & 1);
        }
        if (compressed) return Result<void>::fail(ErrorCode::Unsupported, src_->name() + ": DST-compressed DFF isn't supported");
        if (!haveData) return Result<void>::fail(ErrorCode::Corrupt, src_->name() + ": no DSD data");
        info_.layout = Layout::Interleaved;
        info_.lsbFirst = false;
        info_.blockSize = 4096;
        return Result<void>::success();
    }

    void designFilter() {
        // Linear-phase Kaiser-windowed sinc: −6 dB at 40 kHz, ~80 dB stop band beyond ~60 kHz, so nothing
        // aliases into the audio band after decimation. Length scales with the DSD rate (360 taps at DSD64).
        const uint32_t taps = static_cast<uint32_t>(std::ceil(360.0 * info_.dsdRate / 2822400.0 / 8.0)) * 8;
        groups_ = taps / 8;
        const double fc = 40000.0 / info_.dsdRate;  // cycles per input sample
        const double beta = 7.86;
        const double i0b = besselI0(beta);
        std::vector<double> h(taps);
        double sum = 0;
        for (uint32_t k = 0; k < taps; ++k) {
            const double m = k - (taps - 1) / 2.0;
            const double x = 2.0 * fc * m;
            const double sinc = std::fabs(x) < 1e-12 ? 1.0 : std::sin(kPi * x) / (kPi * x);
            const double r = 2.0 * k / (taps - 1) - 1.0;
            const double w = besselI0(beta * std::sqrt(std::max(0.0, 1.0 - r * r))) / i0b;
            h[k] = 2.0 * fc * sinc * w;
            sum += h[k];
        }
        for (double& v : h) v /= sum;  // unity gain at DC: 100 % modulation ↔ ±1.0 (0 dB SACD ↔ −6 dBFS)
        // Byte lookup tables: lut[g][byte] = Σ over the 8 bits of that byte of ±h.
        lut_.assign(static_cast<size_t>(groups_) * 256, 0.0f);
        for (uint32_t g = 0; g < groups_; ++g)
            for (int v = 0; v < 256; ++v) {
                double acc = 0;
                for (int b = 0; b < 8; ++b) acc += ((v >> (7 - b)) & 1) ? h[g * 8 + b] : -h[g * 8 + b];
                lut_[static_cast<size_t>(g) * 256 + v] = static_cast<float>(acc);
            }
    }

    // Byte i (per channel, MSB-first order) of channel c.
    Result<uint8_t> byteAt(uint16_t c, uint64_t i) noexcept {
        if (i >= info_.bytesPerChannel) return uint8_t{0x69};
        const uint64_t window = i / info_.blockSize;
        if (window != cacheWindow_) {
            const size_t bytes = static_cast<size_t>(info_.blockSize) * info_.channels;
            std::fill(cache_.begin(), cache_.end(), uint8_t{0x69});
            auto r = src_->readAt(info_.dataOffset + window * bytes, cache_.data(), bytes);
            if (!r) return r.error();
            cacheWindow_ = window;
        }
        const size_t off = static_cast<size_t>(i % info_.blockSize);
        const uint8_t b = info_.layout == Layout::Planar ? cache_[static_cast<size_t>(c) * info_.blockSize + off]
                                                         : cache_[off * info_.channels + c];
        return info_.lsbFirst ? kBitReverse[b] : b;
    }

    void push(uint16_t c, uint8_t b) noexcept {
        // Doubled circular buffer: the newest `groups_` bytes are always contiguous at [pos, pos + groups_).
        uint8_t* h = hist_.data() + static_cast<size_t>(c) * groups_ * 2;
        uint32_t& pos = histPos_[c];
        h[pos] = b;
        h[pos + groups_] = b;
        pos = (pos + 1) % groups_;
    }

    int32_t filterOut(uint16_t c) const noexcept {
        const uint8_t* h = hist_.data() + static_cast<size_t>(c) * groups_ * 2 + histPos_[c];
        double acc = 0;
        for (uint32_t g = 0; g < groups_; ++g) acc += lut_[static_cast<size_t>(g) * 256 + h[g]];
        acc = std::clamp(acc, -1.0, 8388607.0 / 8388608.0);
        return static_cast<int32_t>(std::lrint(acc * 8388608.0)) * 256;  // 24 significant bits, left-justified
    }

    std::unique_ptr<ByteSource> src_;
    DsdInfo info_;
    StreamFormat fmt_{};
    uint64_t total_ = 0, outPos_ = 0;
    uint32_t decim_ = 16, bytesPerOut_ = 2, groups_ = 0;
    std::vector<float> lut_;
    std::vector<uint8_t> hist_;
    std::vector<uint32_t> histPos_;
    std::vector<uint8_t> cache_;
    uint64_t cacheWindow_ = UINT64_MAX;
};

}  // namespace

Result<std::unique_ptr<Decoder>> openDsdDecoder(std::unique_ptr<ByteSource> src) noexcept {
    using R = Result<std::unique_ptr<Decoder>>;
    if (!src) return R::fail(ErrorCode::InvalidArgument, "no source");
    try {
        auto d = std::make_unique<DsdDecoder>(std::move(src));
        auto r = d->init();
        if (!r) return r.error();
        return R(std::unique_ptr<Decoder>(std::move(d)));
    } catch (const std::bad_alloc&) {
        return R::fail(ErrorCode::OutOfMemory, "DSD decoder");
    } catch (...) {
        return R::fail(ErrorCode::Internal, "DSD decoder");
    }
}

}  // namespace groove
