// libFuzzer target: arbitrary bytes into the decoder factory (FLAC, libsndfile formats, DSF/DFF),
// then decode, seek and decode again.
#include <cstddef>
#include <cstdint>
#include <vector>

#include "groove/Decoder.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    auto d = groove::openDecoder(std::make_unique<groove::MemorySource>(std::vector<uint8_t>(data, data + size), "fuzz"));
    if (!d.ok()) return 0;
    groove::Decoder& dec = *d.value();
    std::vector<int32_t> buf(1024u * dec.format().channels);
    uint64_t frames = 0;
    for (int i = 0; i < 64; ++i) {
        auto r = dec.read(buf.data(), 1024);
        if (!r.ok() || r.value() == 0) break;
        frames += r.value();
    }
    if (dec.totalFrames() > 0) (void)dec.seek((size * 7919u) % dec.totalFrames());
    for (int i = 0; i < 4; ++i) {
        auto r = dec.read(buf.data(), 1024);
        if (!r.ok() || r.value() == 0) break;
    }
    (void)frames;
    return 0;
}
