#include <cmath>
#include <string>

#include "TestUtil.h"
#include "doctest.h"
#include "groove/Metadata.h"

using namespace groove;

namespace {
TrackMetadata parse(const std::string& file, bool art = false) {
    auto src = FileSource::openPath(std::string(GROOVE_FIXTURES) + "/" + file);
    REQUIRE(src.ok());
    ParseOptions o;
    o.loadArtwork = art;
    auto m = readMetadata(*src.value(), o);
    REQUIRE_MESSAGE(m.ok(), m.error().message);
    return std::move(m).value();
}
void checkCommonTags(const TrackMetadata& m) {
    CHECK(m.title == "Groove Test");
    CHECK(m.artist == "Test Artist");
    CHECK(m.album == "Test Album");
    CHECK(m.albumArtist == "Album Artist");
    CHECK(m.genre == "Electronic");
    CHECK(m.year == 2024);
    CHECK(m.trackNo == 3);
    CHECK(m.trackTotal == 12);
}
}  // namespace

TEST_CASE("Metadata: FLAC (Vorbis comments, STREAMINFO, PICTURE block)") {
    const auto m = parse("tagged.flac", true);
    checkCommonTags(m);
    CHECK(m.discNo == 1);
    CHECK(m.container == "FLAC");
    CHECK(m.codec == "FLAC");
    CHECK(m.lossless);
    CHECK(m.sampleRate == 48000);
    CHECK(m.bitDepth == 24);
    CHECK(m.channels == 2);
    CHECK(m.durationSeconds == doctest::Approx(1.0).epsilon(0.001));
    REQUIRE(m.artwork);
    CHECK(m.artwork->mime == "image/png");
    CHECK(m.artwork->pictureType == 3);
    CHECK(m.artwork->data == gt::readBytes(std::string(GROOVE_FIXTURES) + "/cover.png"));
    // Lazy artwork read from the recorded offset gives the same bytes.
    auto src = FileSource::openPath(std::string(GROOVE_FIXTURES) + "/tagged.flac");
    ArtworkRef ref = *m.artwork;
    ref.data.clear();
    auto bytes = readArtwork(*src.value(), ref);
    REQUIRE(bytes.ok());
    CHECK(bytes.value() == gt::readBytes(std::string(GROOVE_FIXTURES) + "/cover.png"));
}

TEST_CASE("Metadata: MP3 with ID3v2.3 (APIC, TXXX) and ID3v2.4, Xing frame count") {
    const auto m = parse("tagged_v23.mp3", true);
    checkCommonTags(m);
    CHECK(m.container == "MP3");
    CHECK(m.codec == "MP3");
    CHECK_FALSE(m.lossless);
    CHECK(m.sampleRate == 48000);
    CHECK(m.channels == 2);
    CHECK(m.bitDepth == 0);  // not applicable: never invented
    CHECK(m.durationSeconds == doctest::Approx(1.0).epsilon(0.06));
    REQUIRE(m.artwork);
    CHECK(m.artwork->mime == "image/jpeg");
    CHECK(m.artwork->data == gt::readBytes(std::string(GROOVE_FIXTURES) + "/cover.jpg"));
    bool mood = false;
    for (auto& kv : m.extra) mood = mood || (kv.first == "MOOD" && kv.second == "Happy");
    CHECK(mood);

    const auto m4 = parse("tagged_v24.mp3");
    checkCommonTags(m4);
}

TEST_CASE("Metadata: MP4 — AAC (esds, covr) and ALAC (magic cookie: 96 kHz / 24-bit)") {
    const auto aac = parse("tagged_aac.m4a", true);
    checkCommonTags(aac);
    CHECK(aac.container == "MP4");
    CHECK(aac.codec == "AAC");
    CHECK_FALSE(aac.lossless);
    CHECK(aac.sampleRate == 48000);
    CHECK(aac.bitrateKbps > 50);
    REQUIRE(aac.artwork);
    CHECK(aac.artwork->data == gt::readBytes(std::string(GROOVE_FIXTURES) + "/cover.jpg"));

    const auto alac = parse("tagged_alac.m4a");
    checkCommonTags(alac);
    CHECK(alac.codec == "ALAC");
    CHECK(alac.lossless);
    CHECK(alac.sampleRate == 96000);
    CHECK(alac.bitDepth == 24);
    CHECK(alac.channels == 2);
    CHECK(alac.durationSeconds == doctest::Approx(1.0).epsilon(0.01));
}

TEST_CASE("Metadata: Ogg Vorbis and Opus (comment packets, granule duration)") {
    const auto v = parse("tagged.ogg");
    checkCommonTags(v);
    CHECK(v.codec == "Vorbis");
    CHECK(v.sampleRate == 48000);
    CHECK(v.durationSeconds == doctest::Approx(1.0).epsilon(0.01));
    const auto o = parse("tagged.opus");
    checkCommonTags(o);
    CHECK(o.codec == "Opus");
    CHECK(o.channels == 2);
    CHECK(o.durationSeconds == doctest::Approx(1.0).epsilon(0.02));
}

TEST_CASE("Metadata: WAV (fmt + LIST INFO) and AIFF (COMM 80-bit rate + ID3 chunk)") {
    const auto w = parse("tagged.wav");
    CHECK(w.title == "Groove Test");
    CHECK(w.artist == "Test Artist");
    CHECK(w.container == "WAV");
    CHECK(w.sampleRate == 44100);
    CHECK(w.bitDepth == 24);
    CHECK(w.durationSeconds == doctest::Approx(1.0).epsilon(0.001));
    const auto a = parse("tagged.aiff");
    CHECK(a.container == "AIFF");
    CHECK(a.sampleRate == 44100);
    CHECK(a.bitDepth == 16);
    CHECK(a.title == "Groove Test");
    CHECK(a.durationSeconds == doctest::Approx(1.0).epsilon(0.001));
}

TEST_CASE("Metadata: hand-built ID3v2.4 with UTF-16, unsynchronised APIC and a v2.4 plain-size frame") {
    auto frame = [](const char* id, std::vector<uint8_t> body, uint16_t flags = 0) {
        std::vector<uint8_t> f(id, id + 4);
        const uint32_t n = static_cast<uint32_t>(body.size());
        f.push_back(static_cast<uint8_t>(n >> 21 & 0x7f));
        f.push_back(static_cast<uint8_t>(n >> 14 & 0x7f));
        f.push_back(static_cast<uint8_t>(n >> 7 & 0x7f));
        f.push_back(static_cast<uint8_t>(n & 0x7f));
        f.push_back(static_cast<uint8_t>(flags >> 8));
        f.push_back(static_cast<uint8_t>(flags));
        f.insert(f.end(), body.begin(), body.end());
        return f;
    };
    // TIT2 in UTF-16 with BOM: "Ünïcode 🎵"
    std::vector<uint8_t> tit{1, 0xFF, 0xFE};
    for (uint16_t u : std::initializer_list<uint16_t>{0x00DC, 'n', 0x00EF, 'c', 'o', 'd', 'e', ' ', 0xD83C, 0xDFB5}) { tit.push_back(static_cast<uint8_t>(u)); tit.push_back(static_cast<uint8_t>(u >> 8)); }
    // APIC with frame-level unsynchronisation: image bytes FF D8 FF E0 are stored as FF 00 D8 FF 00 E0.
    std::vector<uint8_t> apic{0};
    const char* mime = "image/jpeg";
    apic.insert(apic.end(), mime, mime + 10);
    apic.push_back(0);
    apic.push_back(3);  // front cover
    apic.push_back(0);  // empty description
    for (uint8_t b : std::initializer_list<uint8_t>{0xFF, 0x00, 0xD8, 0xFF, 0x00, 0xE0, 0x01, 0x02}) apic.push_back(b);
    std::vector<uint8_t> body;
    auto t = frame("TIT2", tit);
    body.insert(body.end(), t.begin(), t.end());
    auto a = frame("APIC", apic, 0x0002);
    body.insert(body.end(), a.begin(), a.end());
    auto c = frame("TCON", {0, '(', '1', '7', ')'});
    body.insert(body.end(), c.begin(), c.end());
    std::vector<uint8_t> tag{'I', 'D', '3', 4, 0, 0};
    const uint32_t n = static_cast<uint32_t>(body.size());
    for (int s : {21, 14, 7, 0}) tag.push_back(static_cast<uint8_t>(n >> s & 0x7f));
    tag.insert(tag.end(), body.begin(), body.end());
    // followed by an MP3 frame header so it's a valid MP3
    for (uint8_t b : std::initializer_list<uint8_t>{0xFF, 0xFB, 0x90, 0x64}) tag.push_back(b);
    tag.resize(tag.size() + 2000, 0);
    MemorySource src(tag, "id3v24.mp3");
    auto m = readMetadata(src);
    REQUIRE(m.ok());
    CHECK(m.value().title == "\xC3\x9Cn\xC3\xAF" "code \xF0\x9F\x8E\xB5");
    CHECK(m.value().genre == "Rock");
    REQUIRE(m.value().artwork);
    CHECK(m.value().artwork->inlineOnly);  // unsynchronised: not addressable by offset
    CHECK(m.value().artwork->data == std::vector<uint8_t>{0xFF, 0xD8, 0xFF, 0xE0, 0x01, 0x02});
}

TEST_CASE("Metadata: DSF header + DFF PROP") {
    const std::string p = gt::tempPath("meta.dsf");
    REQUIRE(gt::writeBytes(p, gt::makeDsf(5644800, 2, 0.25, 1000, 0.5)));
    auto src = FileSource::openPath(p);
    auto m = readMetadata(*src.value());
    REQUIRE(m.ok());
    CHECK(m.value().codec == "DSD");
    CHECK(m.value().sampleRate == 5644800);  // DSD128
    CHECK(m.value().bitDepth == 1);
    CHECK(m.value().durationSeconds == doctest::Approx(0.25).epsilon(0.01));
    MemorySource dff(gt::makeDff(2822400, 2, 0.25, 1000, 0.5), "a.dff");
    auto d = readMetadata(dff);
    REQUIRE(d.ok());
    CHECK(d.value().container == "DFF");
    CHECK(d.value().channels == 2);
    CHECK(d.value().durationSeconds == doctest::Approx(0.25).epsilon(0.01));
}

TEST_CASE("Metadata: truncated and garbage inputs never crash and never invent values") {
    for (const char* f : {"tagged.flac", "tagged_v23.mp3", "tagged_aac.m4a", "tagged.ogg", "tagged.wav", "tagged.aiff"}) {
        CAPTURE(f);
        const auto full = gt::readBytes(std::string(GROOVE_FIXTURES) + "/" + f);
        for (size_t cut : {size_t(12), size_t(40), size_t(200), full.size() / 3, full.size() - 7}) {
            std::vector<uint8_t> part(full.begin(), full.begin() + std::min(cut, full.size()));
            MemorySource src(part, f);
            auto m = readMetadata(src);
            if (m.ok()) CHECK(m.value().sampleRate < 1000000);
        }
    }
    MemorySource junk(std::vector<uint8_t>(100, 0xAB), "junk");
    CHECK(readMetadata(junk).code() == ErrorCode::Unsupported);
}
