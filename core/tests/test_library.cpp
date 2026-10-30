#include <sndfile.h>
#include <sys/stat.h>
#include <utime.h>

#include <atomic>
#include <filesystem>
#include <thread>

#include "TestUtil.h"
#include "doctest.h"
#include "groove/Library.h"

using namespace groove;
namespace fs = std::filesystem;

namespace {
std::string makeLibraryDir() {
    const std::string root = gt::tempPath("library");
    fs::create_directories(root + "/Album A");
    fs::create_directories(root + "/Album B/CD1");
    fs::create_directories(root + "/.hidden");
    for (const char* f : {"tagged.flac", "tagged_v23.mp3", "tagged_aac.m4a", "tagged_alac.m4a", "tagged.ogg", "tagged.wav"})
        fs::copy_file(std::string(GROOVE_FIXTURES) + "/" + f, root + "/Album A/" + f, fs::copy_options::overwrite_existing);
    REQUIRE(gt::writeAudio(root + "/Album B/CD1/hires.flac", SF_FORMAT_FLAC | SF_FORMAT_PCM_24, 192000, 2, gt::makeSignal(19200, 2, 1, 24)));
    REQUIRE(gt::writeBytes(root + "/Album B/dsd.dsf", gt::makeDsf(2822400, 2, 0.2, 1000, 0.5)));
    REQUIRE(gt::writeBytes(root + "/Album B/notes.txt", {'h', 'i'}));
    fs::copy_file(std::string(GROOVE_FIXTURES) + "/tagged.flac", root + "/.hidden/secret.flac", fs::copy_options::overwrite_existing);
    return root;
}
ScanProgress runScan(LibraryScanner& s, const std::string& root) {
    ScanProgress last;
    std::mutex m;
    REQUIRE(s.scanDirectories({root}, [&](const ScanProgress& p) { std::lock_guard<std::mutex> l(m); last = p; }).ok());
    s.wait();
    std::lock_guard<std::mutex> l(m);
    return last;
}
}  // namespace

TEST_CASE("Library: background scan indexes audio, skips hidden folders, and is incremental") {
    const std::string root = makeLibraryDir();
    const std::string dbPath = gt::tempPath("lib1.db");
    LibraryScanner scanner(dbPath, nullptr);
    auto p1 = runScan(scanner, root);
    CHECK(p1.finished);
    CHECK(p1.added == 8);
    CHECK(p1.failed == 0);

    auto db = LibraryDb::open(dbPath);
    REQUIRE(db.ok());
    CHECK(db.value()->count().value() == 8);

    auto p2 = runScan(scanner, root);  // nothing changed
    CHECK(p2.unchanged == 8);
    CHECK(p2.added == 0);

    // Change one file's mtime, delete another: updated 1, removed 1.
    struct utimbuf times{1000000000, 1000000000};
    REQUIRE(::utime((root + "/Album A/tagged.ogg").c_str(), &times) == 0);
    fs::remove(root + "/Album A/tagged.wav");
    auto p3 = runScan(scanner, root);
    CHECK(p3.updated == 1);
    CHECK(p3.removed == 1);
    CHECK(db.value()->count().value() == 7);
}

TEST_CASE("Library: smart playlists filter by metadata with bound parameters only") {
    const std::string root = makeLibraryDir();
    const std::string dbPath = gt::tempPath("lib2.db");
    LibraryScanner scanner(dbPath, nullptr);
    runScan(scanner, root);
    auto dbr = LibraryDb::open(dbPath);
    REQUIRE(dbr.ok());
    LibraryDb& db = *dbr.value();

    SmartPlaylist hires;  // lossless, ≥ 88.2 kHz, ≥ 24-bit
    hires.rules = {{Field::Lossless, Op::IsTrue, {}, {}},
                   {Field::SampleRate, Op::GreaterOrEqual, int64_t{88200}, {}},
                   {Field::BitDepth, Op::GreaterOrEqual, int64_t{24}, {}}};
    hires.sortBy = Field::SampleRate;
    hires.descending = true;
    auto r = db.query(hires);
    REQUIRE(r.ok());
    REQUIRE(r.value().size() == 2);  // 192 kHz FLAC, 96 kHz ALAC
    CHECK(r.value()[0].sampleRate == 192000);
    CHECK(r.value()[1].codec == "ALAC");

    SmartPlaylist byArtist;
    byArtist.rules = {{Field::Artist, Op::Contains, std::string("artist"), {}}, {Field::Codec, Op::NotEquals, std::string("mp3"), {}}};
    auto r2 = db.query(byArtist);
    REQUIRE(r2.ok());
    CHECK(r2.value().size() == 5);  // tagged files except the MP3

    SmartPlaylist anyOf;
    anyOf.matchAll = false;
    anyOf.rules = {{Field::Codec, Op::Equals, std::string("DSD"), {}}, {Field::Year, Op::Between, int64_t{2020}, int64_t{2025}}};
    anyOf.limit = 3;
    auto r3 = db.query(anyOf);
    REQUIRE(r3.ok());
    CHECK(r3.value().size() == 3);

    // SQL injection attempt is just a string that matches nothing; the table survives.
    SmartPlaylist evil;
    evil.rules = {{Field::Title, Op::Equals, std::string("x' OR 1=1; DROP TABLE tracks; --"), {}}};
    auto r4 = db.query(evil);
    REQUIRE(r4.ok());
    CHECK(r4.value().empty());
    CHECK(db.count().value() == 8);

    // LIKE wildcards in user text are literal.
    SmartPlaylist pct;
    pct.rules = {{Field::Title, Op::Contains, std::string("%"), {}}};
    CHECK(db.query(pct).value().empty());

    // Type errors are rejected before any SQL runs.
    SmartPlaylist bad;
    bad.rules = {{Field::SampleRate, Op::Contains, std::string("44"), {}}};
    CHECK(db.query(bad).code() == ErrorCode::InvalidArgument);
    bad.rules = {{Field::Title, Op::Greater, int64_t{3}, {}}};
    CHECK(db.query(bad).code() == ErrorCode::InvalidArgument);
}

TEST_CASE("Library: platform-listed entries (Android SAF) and UI queries during a scan") {
    const std::string root = makeLibraryDir();
    const std::string dbPath = gt::tempPath("lib3.db");
    std::vector<FileEntry> entries;
    for (const auto& e : fs::recursive_directory_iterator(root + "/Album A")) {
        if (!e.is_regular_file()) continue;
        struct stat sb{};
        ::stat(e.path().c_str(), &sb);
        entries.push_back({e.path().string(), "Album A", static_cast<int64_t>(sb.st_size), static_cast<int64_t>(sb.st_mtime)});
    }
    LibraryScanner scanner(dbPath, nullptr);
    auto ui = LibraryDb::open(dbPath);  // UI thread's own connection
    REQUIRE(ui.ok());
    std::atomic<bool> done{false};
    REQUIRE(scanner.scanEntries("content://tree/primary%3AMusic", entries, [&](const ScanProgress& p) { if (p.finished) done = true; }).ok());
    int queries = 0;
    while (!done) {  // WAL: reads proceed while the scanner writes
        REQUIRE(ui.value()->query(SmartPlaylist{}).ok());
        ++queries;
    }
    scanner.wait();
    CHECK(queries > 0);
    CHECK(ui.value()->count().value() == 6);
    // Re-scan with one entry gone: swept.
    entries.pop_back();
    REQUIRE(scanner.scanEntries("content://tree/primary%3AMusic", entries, nullptr).ok());
    scanner.wait();
    CHECK(ui.value()->count().value() == 5);
}
