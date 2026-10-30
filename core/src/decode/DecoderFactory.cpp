#include <cstring>
#include <mutex>
#include <vector>

#include "groove/Decoder.h"

namespace groove {
namespace {
std::mutex gPlatformMutex;
std::vector<PlatformDecoderFactory>& platformFactories() {
    static std::vector<PlatformDecoderFactory> v;
    return v;
}
}  // namespace

void registerPlatformDecoder(PlatformDecoderFactory factory) {
    std::lock_guard<std::mutex> lk(gPlatformMutex);
    platformFactories().push_back(std::move(factory));
}

Result<std::unique_ptr<Decoder>> openDecoder(std::unique_ptr<ByteSource> src) noexcept {
    using R = Result<std::unique_ptr<Decoder>>;
    if (!src) return R::fail(ErrorCode::InvalidArgument, "no source");
    uint8_t h[16] = {};
    auto r = src->readAt(0, h, sizeof h);
    if (!r) return r.error();
    if (r.value() < 12) return R::fail(ErrorCode::Corrupt, src->name() + ": file too short");

    // ID3v2 in front of FLAC is legal (and common from some taggers); libFLAC skips it itself.
    bool flac = std::memcmp(h, "fLaC", 4) == 0;
    if (!flac && std::memcmp(h, "ID3", 3) == 0) {
        const uint64_t tag = 10 + ((h[6] & 0x7f) << 21 | (h[7] & 0x7f) << 14 | (h[8] & 0x7f) << 7 | (h[9] & 0x7f)) + ((h[5] & 0x10) ? 10 : 0);
        uint8_t m[4] = {};
        auto mr = src->readAt(tag, m, 4);
        flac = mr && mr.value() == 4 && std::memcmp(m, "fLaC", 4) == 0;
    }
    if (flac) return openFlacDecoder(std::move(src));
    if (std::memcmp(h, "DSD ", 4) == 0 || std::memcmp(h, "FRM8", 4) == 0) return openDsdDecoder(std::move(src));
    if (std::memcmp(h + 4, "ftyp", 4) == 0) {
        // MP4/M4A (AAC, ALAC): platform codecs only (AudioToolbox / MediaCodec).
        std::vector<PlatformDecoderFactory> factories;
        {
            std::lock_guard<std::mutex> lk(gPlatformMutex);
            factories = platformFactories();
        }
        for (auto& f : factories) {
            auto d = f(src);
            if (d) return d;
            if (!src) return d;  // factory consumed the source and failed
        }
        return R::fail(ErrorCode::Unsupported, src->name() + ": MP4/M4A needs the platform decoder");
    }
    return openSndfileDecoder(std::move(src));
}

}  // namespace groove
