#include <sndfile.h>

#include <random>

#include "TestUtil.h"
#include "doctest.h"
#include "groove/WavStream.h"

using namespace groove;

namespace {
std::unique_ptr<WavStream> openStream(const std::string& path) {
    auto src = FileSource::openPath(path);
    REQUIRE(src.ok());
    auto w = WavStream::open(std::move(src).value());
    REQUIRE_MESSAGE(w.ok(), w.error().message);
    return std::move(w).value();
}
std::vector<uint8_t> readAll(WavStream& w) {
    std::vector<uint8_t> v(w.size());
    size_t at = 0;
    while (at < v.size()) {
        auto r = w.read(at, v.data() + at, std::min<size_t>(65536, v.size() - at));
        REQUIRE(r.ok());
        REQUIRE(r.value() > 0);
        at += r.value();
    }
    return v;
}
}  // namespace

TEST_CASE("WavStream: FLAC/AIFF become a WAV that libsndfile reads back sample-identical") {
    struct Case { const char* name; int fmt; uint16_t bits; uint32_t rate; uint16_t ch; uint16_t outBits; };
    for (const Case& c : {Case{"w16.flac", SF_FORMAT_FLAC | SF_FORMAT_PCM_16, 16, 44100, 2, 16},
                          Case{"w24.flac", SF_FORMAT_FLAC | SF_FORMAT_PCM_24, 24, 96000, 2, 24},
                          Case{"w24m.aiff", SF_FORMAT_AIFF | SF_FORMAT_PCM_24, 24, 48000, 1, 24},
                          Case{"w32.wav", SF_FORMAT_WAV | SF_FORMAT_PCM_32, 32, 44100, 2, 32}}) {
        CAPTURE(c.name);
        const auto sig = gt::makeSignal(c.rate / 2 + 123, c.ch, 77, c.bits);
        REQUIRE(gt::writeAudio(gt::tempPath(c.name), c.fmt, c.rate, c.ch, sig));
        auto w = openStream(gt::tempPath(c.name));
        CHECK(w->outBits() == c.outBits);
        const auto bytes = readAll(*w);
        const std::string out = gt::tempPath(std::string(c.name) + ".out.wav");
        REQUIRE(gt::writeBytes(out, bytes));
        SF_INFO info{};
        SNDFILE* f = sf_open(out.c_str(), SFM_READ, &info);
        REQUIRE(f);
        CHECK(info.samplerate == static_cast<int>(c.rate));
        CHECK(info.channels == c.ch);
        CHECK(info.frames == static_cast<sf_count_t>(sig.size() / c.ch));
        std::vector<int32_t> back(sig.size());
        CHECK(sf_readf_int(f, back.data(), info.frames) == info.frames);
        sf_close(f);
        CHECK(back == sig);
    }
}

TEST_CASE("WavStream: random byte ranges equal the same bytes of a full sequential read") {
    const auto sig = gt::makeSignal(44100 * 3, 2, 5, 24);
    REQUIRE(gt::writeAudio(gt::tempPath("rr.flac"), SF_FORMAT_FLAC | SF_FORMAT_PCM_24, 44100, 2, sig));
    auto ref = openStream(gt::tempPath("rr.flac"));
    const auto all = readAll(*ref);
    auto w = openStream(gt::tempPath("rr.flac"));
    std::mt19937 rng(9);
    std::vector<uint8_t> buf(70000);
    for (int i = 0; i < 300; ++i) {
        const uint64_t off = rng() % all.size();
        const size_t len = 1 + rng() % buf.size();
        auto r = w->read(off, buf.data(), len);
        REQUIRE(r.ok());
        REQUIRE(r.value() == std::min<size_t>(len, all.size() - off));
        REQUIRE(std::equal(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(r.value()), all.begin() + static_cast<std::ptrdiff_t>(off)));
    }
    CHECK(w->read(all.size(), buf.data(), 10).value() == 0);
}

TEST_CASE("WavStream: DSD becomes 24-bit 176.4 kHz WAV; truncated input stays the declared size") {
    REQUIRE(gt::writeBytes(gt::tempPath("ws.dsf"), gt::makeDsf(2822400, 2, 0.3, 1000, 0.5)));
    auto w = openStream(gt::tempPath("ws.dsf"));
    CHECK(w->format().sampleRate == 176400);
    CHECK(w->outBits() == 24);
    CHECK(readAll(*w).size() == w->size());

    const auto sig = gt::makeSignal(44100, 2, 6, 16);
    REQUIRE(gt::writeAudio(gt::tempPath("cut.wav"), SF_FORMAT_WAV | SF_FORMAT_PCM_16, 44100, 2, sig));
    auto bytes = gt::readBytes(gt::tempPath("cut.wav"));
    bytes.resize(bytes.size() - 40000);  // header still claims the full length
    auto t = WavStream::open(std::make_unique<MemorySource>(bytes, "cut.wav"));
    REQUIRE(t.ok());
    CHECK(readAll(*t.value()).size() == t.value()->size());
}
