#include <sndfile.h>

#include <cmath>
#include <vector>

#include "TestUtil.h"
#include "doctest.h"
#include "groove/Decoder.h"

using namespace groove;

namespace {
std::unique_ptr<Decoder> open(const std::string& path) {
    auto src = FileSource::openPath(path);
    REQUIRE(src.ok());
    auto d = openDecoder(std::move(src).value());
    REQUIRE_MESSAGE(d.ok(), d.error().message);
    return std::move(d).value();
}
std::vector<int32_t> decodeAll(Decoder& d) {
    std::vector<int32_t> out, buf(1000 * d.format().channels);
    for (;;) {
        auto r = d.read(buf.data(), 1000);
        REQUIRE(r.ok());
        if (r.value() == 0) break;
        out.insert(out.end(), buf.begin(), buf.begin() + r.value() * d.format().channels);
    }
    return out;
}
}  // namespace

TEST_CASE("Decoders are bit-exact for PCM sources (WAV/AIFF via libsndfile, FLAC via libFLAC)") {
    struct Case { const char* name; int fmt; uint16_t bits; uint32_t rate; uint16_t ch; const char* codec; };
    const Case cases[] = {
        {"a16.wav", SF_FORMAT_WAV | SF_FORMAT_PCM_16, 16, 44100, 2, "PCM"},
        {"a24.wav", SF_FORMAT_WAV | SF_FORMAT_PCM_24, 24, 96000, 2, "PCM"},
        {"a24.aiff", SF_FORMAT_AIFF | SF_FORMAT_PCM_24, 24, 48000, 1, "PCM"},
        {"a16.flac", SF_FORMAT_FLAC | SF_FORMAT_PCM_16, 16, 44100, 2, "FLAC"},
        {"a24.flac", SF_FORMAT_FLAC | SF_FORMAT_PCM_24, 24, 192000, 2, "FLAC"},
    };
    for (const Case& c : cases) {
        CAPTURE(c.name);
        const auto sig = gt::makeSignal(c.rate / 3, c.ch, 1234, c.bits);
        const std::string path = gt::tempPath(c.name);
        REQUIRE(gt::writeAudio(path, c.fmt, c.rate, c.ch, sig));
        auto d = open(path);
        CHECK(std::string(d->codecName()) == c.codec);
        CHECK(d->format().sampleRate == c.rate);
        CHECK(d->format().channels == c.ch);
        CHECK(d->format().bitsPerSample == c.bits);
        CHECK(d->totalFrames() == c.rate / 3);
        const auto out = decodeAll(*d);
        REQUIRE(out.size() == sig.size());
        CHECK(out == sig);  // every bit identical

        // Seek lands exactly on the requested frame.
        const uint64_t at = c.rate / 7;
        REQUIRE(d->seek(at).ok());
        std::vector<int32_t> buf(500 * c.ch);
        auto r = d->read(buf.data(), 500);
        REQUIRE(r.ok());
        REQUIRE(r.value() == 500);
        CHECK(std::equal(buf.begin(), buf.end(), sig.begin() + at * c.ch));
    }
}

TEST_CASE("DSD: DSF and DFF decode to a clean 176.4 kHz PCM sine and seek without errors") {
    for (const char* kind : {"dsf", "dff"}) {
        CAPTURE(kind);
        const uint32_t rate = 2822400;  // DSD64
        const auto bytes = std::string(kind) == "dsf" ? gt::makeDsf(rate, 2, 0.5, 1000, 0.5) : gt::makeDff(rate, 2, 0.5, 1000, 0.5);
        const std::string path = gt::tempPath(std::string("tone.") + kind);
        REQUIRE(gt::writeBytes(path, bytes));
        auto d = open(path);
        CHECK(d->format().sampleRate == 176400);
        CHECK(d->format().fromDsd);
        CHECK(d->format().bitsPerSample == 24);
        CHECK(d->totalFrames() == 88200);
        const auto out = decodeAll(*d);
        REQUIRE(out.size() == 88200u * 2);
        std::vector<double> left;
        for (size_t i = 2 * 8000; i < out.size(); i += 2) left.push_back(out[i] / 2147483648.0);  // skip filter warm-up
        const double fund = gt::goertzel(left, 1000, 176400);
        const double h2 = gt::goertzel(left, 2000, 176400), h3 = gt::goertzel(left, 3000, 176400);
        const double off = gt::goertzel(left, 1370, 176400);
        CHECK(fund == doctest::Approx(0.5).epsilon(0.03));  // amplitude preserved (unity-gain filter)
        CHECK(20 * std::log10(h2 / fund) < -60);
        CHECK(20 * std::log10(h3 / fund) < -60);
        CHECK(20 * std::log10(off / fund) < -80);
        REQUIRE(d->seek(44100).ok());
        std::vector<int32_t> buf(256 * 2);
        auto r = d->read(buf.data(), 256);
        REQUIRE(r.ok());
        CHECK(r.value() == 256);
        // After a seek the filter is primed: the samples match a continuous decode closely.
        for (int i = 0; i < 256; ++i) CHECK(std::abs(buf[i * 2] / 256 - out[(44100 + i) * 2] / 256) < 64);
    }
}

TEST_CASE("Decoders fail cleanly on missing, truncated, unsupported and corrupt files") {
    auto missing = FileSource::openPath(gt::tempPath("does-not-exist.flac"));
    CHECK(missing.code() == ErrorCode::FileNotFound);

    auto garbage = std::make_unique<MemorySource>(std::vector<uint8_t>(4000, 0x5A), "garbage.bin");
    CHECK_FALSE(openDecoder(std::move(garbage)).ok());

    auto tiny = std::make_unique<MemorySource>(std::vector<uint8_t>{'f', 'L', 'a', 'C'}, "tiny.flac");
    CHECK(openDecoder(std::move(tiny)).code() == ErrorCode::Corrupt);

    // A FLAC file cut in half decodes what's there and then reports end/corruption, never crashes.
    const auto sig = gt::makeSignal(44100, 2, 9, 16);
    REQUIRE(gt::writeAudio(gt::tempPath("cut.flac"), SF_FORMAT_FLAC | SF_FORMAT_PCM_16, 44100, 2, sig));
    auto bytes = gt::readBytes(gt::tempPath("cut.flac"));
    bytes.resize(bytes.size() / 2);
    auto d = openDecoder(std::make_unique<MemorySource>(bytes, "cut.flac"));
    REQUIRE(d.ok());
    std::vector<int32_t> buf(4096 * 2);
    for (int i = 0; i < 1000; ++i) {
        auto r = d.value()->read(buf.data(), 4096);
        if (!r.ok() || r.value() == 0) break;
    }

    // Storage removed mid-stream: the decoder returns ErrorCode::Io.
    auto inner = FileSource::openPath(gt::tempPath("cut.flac"));
    REQUIRE(gt::writeAudio(gt::tempPath("full.flac"), SF_FORMAT_FLAC | SF_FORMAT_PCM_16, 44100, 2, sig));
    auto full = FileSource::openPath(gt::tempPath("full.flac"));
    REQUIRE(full.ok());
    auto flaky = std::make_unique<gt::FlakySource>(std::move(full).value(), 20000);
    auto fd = openDecoder(std::move(flaky));
    REQUIRE(fd.ok());
    ErrorCode got = ErrorCode::None;
    for (int i = 0; i < 1000 && got == ErrorCode::None; ++i) {
        auto r = fd.value()->read(buf.data(), 4096);
        if (!r.ok()) got = r.code();
        else if (r.value() == 0) break;
    }
    CHECK(got == ErrorCode::Io);

    auto mp4 = std::make_unique<MemorySource>(gt::readBytes(std::string(GROOVE_FIXTURES) + "/tagged_aac.m4a"), "a.m4a");
    CHECK(openDecoder(std::move(mp4)).code() == ErrorCode::Unsupported);  // needs the platform decoder
}
