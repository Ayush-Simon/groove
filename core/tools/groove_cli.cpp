// groove-cli: exercise the core on a desktop without a phone.
//   groove-cli meta <file>                       tags + stream info
//   groove-cli render <out.wav> <file> [file…]   gapless offline render of a queue (bit-exact, via NullOutput)
//   groove-cli scan <library.db> <folder>        index a folder (incremental)
//   groove-cli hires <library.db>                smart playlist: lossless, ≥ 88.2 kHz, ≥ 24-bit
#include <sndfile.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "groove/AudioEngine.h"
#include "groove/Library.h"
#include "groove/Metadata.h"
#include "groove/NullOutput.h"

using namespace groove;

namespace {

int meta(const char* path) {
    auto src = FileSource::openPath(path);
    if (!src) { std::fprintf(stderr, "%s\n", src.error().message.c_str()); return 1; }
    auto m = readMetadata(*src.value());
    if (!m) { std::fprintf(stderr, "%s\n", m.error().message.c_str()); return 1; }
    const TrackMetadata& t = m.value();
    std::printf("title        %s\nartist       %s\nalbum        %s\nalbum artist %s\ngenre        %s\nyear         %d\ntrack        %d/%d\ndisc         %d/%d\n",
                t.title.c_str(), t.artist.c_str(), t.album.c_str(), t.albumArtist.c_str(), t.genre.c_str(), t.year, t.trackNo, t.trackTotal, t.discNo, t.discTotal);
    std::printf("container    %s\ncodec        %s%s\nsample rate  %u Hz\nbit depth    %u\nchannels     %u\nduration     %.3f s\nbitrate      %u kbps\n",
                t.container.c_str(), t.codec.c_str(), t.lossless ? " (lossless)" : "", t.sampleRate, t.bitDepth, t.channels, t.durationSeconds, t.bitrateKbps);
    if (t.artwork) std::printf("artwork      %s, %llu bytes%s\n", t.artwork->mime.c_str(), static_cast<unsigned long long>(t.artwork->size), t.artwork->inlineOnly ? " (inline)" : "");
    for (const auto& kv : t.extra) std::printf("tag          %s = %s\n", kv.first.c_str(), kv.second.c_str());
    return 0;
}

struct Done final : EngineListener {
    std::atomic<bool> ended{false}, failed{false};
    void onQueueEnded() noexcept override { ended = true; }
    void onTrackFailed(uint64_t, ErrorCode, const std::string& m) noexcept override { std::fprintf(stderr, "failed: %s\n", m.c_str()); failed = true; }
};

int render(const char* outPath, int n, char** files) {
    NullOutput::Options o;
    o.format = SampleFormat::Int32;
    o.speed = 30;  // 30× real time: well within decoder speed, so the render never underruns
    o.captureLimitBytes = size_t(4) << 30;
    auto out = std::make_unique<NullOutput>(o);
    NullOutput* null = out.get();
    EngineConfig cfg;
    cfg.ringSeconds = 4;
    auto er = AudioEngine::create(cfg, std::move(out), nullptr);
    if (!er) { std::fprintf(stderr, "%s\n", er.error().message.c_str()); return 1; }
    auto done = std::make_shared<Done>();
    er.value()->setListener(done);
    er.value()->load({1, files[0]});
    // Queue the rest one by one as each starts (what the app does), so every transition is gapless.
    for (int i = 1; i < n; ++i) {
        er.value()->setNext(TrackSource{static_cast<uint64_t>(i + 1), files[i]});
        while (!done->ended && er.value()->snapshot().trackId != static_cast<uint64_t>(i + 1)) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    while (!done->ended && !done->failed) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const auto snap = er.value()->snapshot();
    // Read everything from the output BEFORE destroying the engine: the engine owns it.
    const auto opens = null->openHistory();
    const auto bytes = null->captured();
    er.value().reset();
    null = nullptr;
    if (opens.empty()) return 1;
    SF_INFO info{};
    info.samplerate = static_cast<int>(opens.back().sampleRate);
    info.channels = opens.back().channels;
    info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_32;
    SNDFILE* f = sf_open(outPath, SFM_WRITE, &info);
    if (!f) { std::fprintf(stderr, "can't write %s: %s\n", outPath, sf_strerror(nullptr)); return 1; }
    sf_writef_int(f, reinterpret_cast<const int*>(bytes.data()), static_cast<sf_count_t>(bytes.size() / 4 / info.channels));
    sf_close(f);
    if (opens.size() > 1)
        std::fprintf(stderr, "warning: the queue changes sample rate; the file is written at %u Hz, so render same-rate queues for a valid file\n",
                     opens.back().sampleRate);
    std::printf("rendered %zu frames, %llu underruns, output reopened %zu time(s)\n", bytes.size() / 4 / info.channels,
                static_cast<unsigned long long>(snap.underruns), opens.size() - 1);
    return 0;
}

int scan(const char* db, const char* dir) {
    LibraryScanner s(db, nullptr);
    std::mutex m;
    ScanProgress last;
    if (!s.scanDirectories({dir}, [&](const ScanProgress& p) { std::lock_guard<std::mutex> l(m); last = p; std::printf("\r%llu files…", static_cast<unsigned long long>(p.seen)); std::fflush(stdout); })) return 1;
    s.wait();
    std::printf("\rseen %llu, added %llu, updated %llu, unchanged %llu, removed %llu, failed %llu\n",
                static_cast<unsigned long long>(last.seen), static_cast<unsigned long long>(last.added), static_cast<unsigned long long>(last.updated),
                static_cast<unsigned long long>(last.unchanged), static_cast<unsigned long long>(last.removed), static_cast<unsigned long long>(last.failed));
    return 0;
}

int hires(const char* dbPath) {
    auto db = LibraryDb::open(dbPath);
    if (!db) { std::fprintf(stderr, "%s\n", db.error().message.c_str()); return 1; }
    SmartPlaylist p;
    p.rules = {{Field::Lossless, Op::IsTrue, {}, {}}, {Field::SampleRate, Op::GreaterOrEqual, int64_t{88200}, {}}, {Field::BitDepth, Op::GreaterOrEqual, int64_t{24}, {}}};
    p.sortBy = Field::AlbumArtist;
    auto r = db.value()->query(p);
    if (!r) { std::fprintf(stderr, "%s\n", r.error().message.c_str()); return 1; }
    for (const auto& t : r.value()) std::printf("%6u Hz %2u-bit  %-5s  %s — %s\n", t.sampleRate, t.bitDepth, t.codec.c_str(), t.artist.c_str(), t.title.c_str());
    std::printf("%zu tracks\n", r.value().size());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 3 && !std::strcmp(argv[1], "meta")) return meta(argv[2]);
    if (argc >= 4 && !std::strcmp(argv[1], "render")) return render(argv[2], argc - 3, argv + 3);
    if (argc >= 4 && !std::strcmp(argv[1], "scan")) return scan(argv[2], argv[3]);
    if (argc >= 3 && !std::strcmp(argv[1], "hires")) return hires(argv[2]);
    std::fprintf(stderr, "usage: groove-cli meta <file> | render <out.wav> <files…> | scan <db> <folder> | hires <db>\n");
    return 2;
}
