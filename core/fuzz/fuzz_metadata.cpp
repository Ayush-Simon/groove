// libFuzzer target: arbitrary bytes into the tag parser (with and without artwork loading).
//   cmake -S core -B build-fuzz -DCMAKE_CXX_COMPILER=clang++ -DGROOVE_FUZZ=ON -DGROOVE_SANITIZE=address,undefined,fuzzer-no-link
//   build-fuzz/fuzz_metadata -max_total_time=600 corpus/ core/tests/fixtures/
#include <cstddef>
#include <cstdint>
#include <vector>

#include "groove/Metadata.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    groove::MemorySource src(std::vector<uint8_t>(data, data + size), "fuzz");
    groove::ParseOptions o;
    o.loadArtwork = size & 1;
    o.maxTagBytes = 1u << 20;
    auto m = groove::readMetadata(src, o);
    if (m.ok() && m.value().artwork) (void)groove::readArtwork(src, *m.value().artwork);
    return 0;
}
