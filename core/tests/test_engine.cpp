#include <sndfile.h>

#include <chrono>
#include <cmath>
#include <atomic>
#include <cstring>
#include <mutex>
#include <random>
#include <thread>

#include "TestUtil.h"
#include "doctest.h"
#include "groove/AudioEngine.h"
#include "groove/NullOutput.h"

using namespace groove;
using namespace std::chrono_literals;

namespace {

struct Recorder final : EngineListener {
    std::mutex m;
    std::vector<uint64_t> started;
    std::vector<uint64_t> failed;
    std::vector<PlaybackState> states;
    std::atomic<int> queueEnded{0};
    void onStateChanged(PlaybackState s, uint64_t) noexcept override { std::lock_guard<std::mutex> l(m); states.push_back(s); }
    void onTrackStarted(uint64_t id) noexcept override { std::lock_guard<std::mutex> l(m); started.push_back(id); }
    void onTrackFailed(uint64_t id, ErrorCode, const std::string&) noexcept override { std::lock_guard<std::mutex> l(m); failed.push_back(id); }
    void onQueueEnded() noexcept override { ++queueEnded; }
    std::vector<uint64_t> startedCopy() { std::lock_guard<std::mutex> l(m); return started; }
    std::vector<uint64_t> failedCopy() { std::lock_guard<std::mutex> l(m); return failed; }
};

bool waitFor(const std::function<bool()>& pred, std::chrono::milliseconds limit = 10s) {
    const auto end = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < end) {
        if (pred()) return true;
        std::this_thread::sleep_for(2ms);
    }
    return pred();
}

std::vector<int16_t> asInt16(const std::vector<uint8_t>& bytes) {
    std::vector<int16_t> v(bytes.size() / 2);
    std::memcpy(v.data(), bytes.data(), v.size() * 2);
    return v;
}
std::vector<int16_t> to16(const std::vector<int32_t>& s) {
    std::vector<int16_t> v(s.size());
    for (size_t i = 0; i < s.size(); ++i) v[i] = static_cast<int16_t>(s[i] >> 16);
    return v;
}
/// Index where `needle` starts inside `hay` (exact match), or npos.
size_t findSeq(const std::vector<int16_t>& hay, const std::vector<int16_t>& needle) {
    if (needle.empty() || hay.size() < needle.size()) return std::string::npos;
    auto it = std::search(hay.begin(), hay.end(), needle.begin(), needle.end());
    return it == hay.end() ? std::string::npos : static_cast<size_t>(it - hay.begin());
}

struct Rig {
    NullOutput* out = nullptr;
    std::unique_ptr<AudioEngine> engine;
    std::shared_ptr<Recorder> rec = std::make_shared<Recorder>();
    explicit Rig(NullOutput::Options o = {}, std::shared_ptr<SourceOpener> opener = nullptr) {
        auto output = std::make_unique<NullOutput>(o);
        out = output.get();
        EngineConfig cfg;
        cfg.ringSeconds = 1.0;
        cfg.prefillSeconds = 0.3;
        auto e = AudioEngine::create(cfg, std::move(output), std::move(opener));
        REQUIRE(e.ok());
        engine = std::move(e).value();
        engine->setListener(rec);
    }
    PlaybackState state() const { return engine->snapshot().state; }
};

NullOutput::Options int16Out() {
    NullOutput::Options o;
    o.format = SampleFormat::Int16;
    o.speed = 6.0;
    return o;
}

}  // namespace

TEST_CASE("Engine FSM: transition table") {
    using S = PlaybackState;
    CHECK(AudioEngine::isTransitionAllowed(S::Stopped, S::Loading));
    CHECK_FALSE(AudioEngine::isTransitionAllowed(S::Stopped, S::Playing));
    CHECK_FALSE(AudioEngine::isTransitionAllowed(S::Stopped, S::Paused));
    CHECK(AudioEngine::isTransitionAllowed(S::Loading, S::Playing));
    CHECK(AudioEngine::isTransitionAllowed(S::Loading, S::Error));
    CHECK(AudioEngine::isTransitionAllowed(S::Playing, S::Paused));
    CHECK(AudioEngine::isTransitionAllowed(S::Paused, S::Playing));
    CHECK_FALSE(AudioEngine::isTransitionAllowed(S::Error, S::Playing));
    CHECK(AudioEngine::isTransitionAllowed(S::Error, S::Loading));
    CHECK(AudioEngine::isTransitionAllowed(S::Error, S::Stopped));
}

TEST_CASE("Engine: gapless playback is bit-perfect across tracks (FLAC → WAV, 16-bit)") {
    const auto a = gt::makeSignal(44100, 2, 1, 16);       // 1.0 s
    const auto b = gt::makeSignal(22050 + 37, 2, 2, 16);  // odd length on purpose
    REQUIRE(gt::writeAudio(gt::tempPath("g1.flac"), SF_FORMAT_FLAC | SF_FORMAT_PCM_16, 44100, 2, a));
    REQUIRE(gt::writeAudio(gt::tempPath("g2.wav"), SF_FORMAT_WAV | SF_FORMAT_PCM_16, 44100, 2, b));
    Rig rig(int16Out());
    rig.engine->load({1, gt::tempPath("g1.flac")});
    rig.engine->setNext(TrackSource{2, gt::tempPath("g2.wav")});
    REQUIRE(waitFor([&] { return rig.rec->queueEnded > 0; }));
    CHECK(rig.state() == PlaybackState::Stopped);
    CHECK(rig.rec->startedCopy() == std::vector<uint64_t>{1, 2});
    const auto snap = rig.engine->snapshot();
    CHECK(snap.underruns == 0);

    // The device received A immediately followed by B — not a single sample missing, added or changed.
    std::vector<int16_t> expected = to16(a);
    const auto b16 = to16(b);
    expected.insert(expected.end(), b16.begin(), b16.end());
    const auto got = asInt16(rig.out->captured());
    REQUIRE(got.size() >= expected.size());
    if (!std::equal(expected.begin(), expected.end(), got.begin())) {
        size_t first = 0;
        while (first < expected.size() && expected[first] == got[first]) ++first;
        MESSAGE("first mismatch at sample " << first << " of " << expected.size() << " (A has " << a.size() << "), A found at "
                << findSeq(got, to16(a)) << ", B found at " << findSeq(got, b16) << ", got size " << got.size());
    }
    CHECK(std::equal(expected.begin(), expected.end(), got.begin()));
    for (size_t i = expected.size(); i < got.size(); ++i) REQUIRE(got[i] == 0);  // silence after the end
}

TEST_CASE("Engine: 24-bit sources stay exact on Int24 and Float32 outputs and are reported bit-perfect") {
    const auto a = gt::makeSignal(48000 / 2, 2, 5, 24);
    REQUIRE(gt::writeAudio(gt::tempPath("h24.flac"), SF_FORMAT_FLAC | SF_FORMAT_PCM_24, 96000, 2, a));
    for (SampleFormat f : {SampleFormat::Int24Packed, SampleFormat::Float32, SampleFormat::Int32}) {
        CAPTURE(static_cast<int>(f));
        NullOutput::Options o;
        o.format = f;
        o.speed = 8.0;
        Rig rig(o);
        rig.engine->load({7, gt::tempPath("h24.flac")});
        REQUIRE(waitFor([&] { return rig.state() == PlaybackState::Playing; }));
        CHECK(rig.engine->snapshot().bitPerfect);
        REQUIRE(waitFor([&] { return rig.rec->queueEnded > 0; }));
        const auto bytes = rig.out->captured();
        for (size_t i = 0; i < a.size(); ++i) {
            int32_t v = 0;
            if (f == SampleFormat::Int24Packed) {
                v = static_cast<int32_t>(static_cast<uint32_t>(bytes[i * 3]) << 8 | static_cast<uint32_t>(bytes[i * 3 + 1]) << 16 | static_cast<uint32_t>(bytes[i * 3 + 2]) << 24);
            } else if (f == SampleFormat::Float32) {
                float x;
                std::memcpy(&x, bytes.data() + i * 4, 4);
                v = static_cast<int32_t>(std::llround(static_cast<double>(x) * 2147483648.0));
            } else {
                std::memcpy(&v, bytes.data() + i * 4, 4);
            }
            REQUIRE(v == a[i]);
        }
    }
}

TEST_CASE("Engine: a sample-rate change between tracks reopens the output at the new rate, both tracks exact") {
    const auto a = gt::makeSignal(44100 / 2, 2, 11, 16);
    const auto b = gt::makeSignal(48000 / 2, 2, 12, 16);
    REQUIRE(gt::writeAudio(gt::tempPath("r441.wav"), SF_FORMAT_WAV | SF_FORMAT_PCM_16, 44100, 2, a));
    REQUIRE(gt::writeAudio(gt::tempPath("r48.flac"), SF_FORMAT_FLAC | SF_FORMAT_PCM_16, 48000, 2, b));
    Rig rig(int16Out());
    rig.engine->load({1, gt::tempPath("r441.wav")});
    rig.engine->setNext(TrackSource{2, gt::tempPath("r48.flac")});
    REQUIRE(waitFor([&] { return rig.rec->queueEnded > 0; }));
    const auto opens = rig.out->openHistory();
    REQUIRE(opens.size() >= 2);
    CHECK(opens.front().sampleRate == 44100);
    CHECK(opens.back().sampleRate == 48000);
    CHECK(rig.rec->startedCopy() == std::vector<uint64_t>{1, 2});
    const auto got = asInt16(rig.out->captured());
    const size_t ia = findSeq(got, to16(a)), ib = findSeq(got, to16(b));
    CHECK(ia == 0);
    CHECK(ib != std::string::npos);
    CHECK(ib >= a.size());
}

TEST_CASE("Engine: pause/resume loses or repeats nothing; seek lands on the exact frame") {
    const auto a = gt::makeSignal(44100 * 2, 2, 21, 16);
    REQUIRE(gt::writeAudio(gt::tempPath("p.flac"), SF_FORMAT_FLAC | SF_FORMAT_PCM_16, 44100, 2, a));
    {
        NullOutput::Options o = int16Out();
        o.speed = 2.0;
        Rig rig(o);
        rig.engine->load({1, gt::tempPath("p.flac")});
        REQUIRE(waitFor([&] { return rig.engine->snapshot().positionSeconds > 0.3; }));
        rig.engine->pause();
        REQUIRE(rig.engine->waitIdle(5s));
        CHECK(rig.state() == PlaybackState::Paused);
        const double posPaused = rig.engine->snapshot().positionSeconds;
        std::this_thread::sleep_for(150ms);
        CHECK(rig.engine->snapshot().positionSeconds == doctest::Approx(posPaused));  // frozen while paused
        rig.engine->play();
        REQUIRE(waitFor([&] { return rig.rec->queueEnded > 0; }));
        const auto got = asInt16(rig.out->captured());
        const auto exp = to16(a);
        REQUIRE(got.size() >= exp.size());
        CHECK(std::equal(exp.begin(), exp.end(), got.begin()));  // continuous across the pause
    }
    {
        Rig rig(int16Out());
        rig.engine->load({1, gt::tempPath("p.flac")}, LoadOptions{false, 0});
        REQUIRE(rig.engine->waitIdle(5s));
        CHECK(rig.state() == PlaybackState::Paused);
        rig.engine->seek(0.5);
        rig.engine->play();
        REQUIRE(waitFor([&] { return rig.rec->queueEnded > 0; }));
        const auto got = asInt16(rig.out->captured());
        const auto exp = to16(a);
        const std::vector<int16_t> tail(exp.begin() + 22050 * 2, exp.end());
        REQUIRE(got.size() >= tail.size());
        CHECK(std::equal(tail.begin(), tail.end(), got.begin()));
    }
}

TEST_CASE("Engine: EQ or volume switch off bit-perfect; volume scales the signal; clipping is counted, never wrapped") {
    const auto a = gt::makeSine(44100, 2, 1000, 44100, 1.0, 16);  // full-scale sine
    REQUIRE(gt::writeAudio(gt::tempPath("sine.wav"), SF_FORMAT_WAV | SF_FORMAT_PCM_16, 44100, 2, a));
    NullOutput::Options o;
    o.format = SampleFormat::Float32;
    o.speed = 8;
    Rig rig(o);
    rig.engine->setVolume(0.5);
    rig.engine->load({1, gt::tempPath("sine.wav")});
    REQUIRE(waitFor([&] { return rig.state() == PlaybackState::Playing; }));
    auto snap = rig.engine->snapshot();
    CHECK_FALSE(snap.bitPerfect);
    CHECK(snap.dspActive);
    REQUIRE(waitFor([&] { return rig.rec->queueEnded > 0; }));
    const auto bytes = rig.out->captured();
    float peak = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        float x;
        std::memcpy(&x, bytes.data() + i * 4, 4);
        peak = std::max(peak, std::fabs(x));
    }
    CHECK(peak == doctest::Approx(0.5).epsilon(0.01));

    // +12 dB EQ with headroom: no clipped samples.
    Rig rig2(o);
    EqSettings eq;
    eq.enabled = true;
    eq.bands[5].gainDb = 12;
    rig2.engine->setEq(eq);
    rig2.engine->load({2, gt::tempPath("sine.wav")});
    REQUIRE(waitFor([&] { return rig2.rec->queueEnded > 0; }));
    CHECK(rig2.engine->snapshot().clippedSamples == 0);
    CHECK(rig2.engine->snapshot().eqHeadroomDb < -11.9);
}

TEST_CASE("Engine: missing file → Error + onTrackFailed; storage pulled mid-song → skips to the next track") {
    {
        Rig rig(int16Out());
        rig.engine->load({9, gt::tempPath("nope.flac")});
        REQUIRE(rig.engine->waitIdle(5s));
        CHECK(rig.state() == PlaybackState::Error);
        CHECK(rig.engine->snapshot().lastError == ErrorCode::FileNotFound);
        CHECK(rig.rec->failedCopy() == std::vector<uint64_t>{9});
        // Recovery: a valid load works again from Error.
        const auto a = gt::makeSignal(4410, 2, 3, 16);
        REQUIRE(gt::writeAudio(gt::tempPath("ok.wav"), SF_FORMAT_WAV | SF_FORMAT_PCM_16, 44100, 2, a));
        rig.engine->load({10, gt::tempPath("ok.wav")});
        REQUIRE(waitFor([&] { return rig.rec->queueEnded > 0; }));
    }
    {
        // An opener that hands out a source which dies after ~0.4 s of audio for track 1.
        struct FlakyOpener final : SourceOpener {
            Result<std::unique_ptr<ByteSource>> open(const std::string& uri) noexcept override {
                auto s = FileSource::openPath(uri);
                if (!s) return s.error();
                if (uri.find("dies") != std::string::npos)
                    return std::unique_ptr<ByteSource>(std::make_unique<gt::FlakySource>(std::move(s).value(), 44 + 44100 * 4 * 4 / 10));
                return std::unique_ptr<ByteSource>(std::move(s).value());
            }
        };
        const auto a = gt::makeSignal(44100 * 2, 2, 31, 16);
        const auto b = gt::makeSignal(44100 / 4, 2, 32, 16);
        REQUIRE(gt::writeAudio(gt::tempPath("dies.wav"), SF_FORMAT_WAV | SF_FORMAT_PCM_16, 44100, 2, a));
        REQUIRE(gt::writeAudio(gt::tempPath("next.wav"), SF_FORMAT_WAV | SF_FORMAT_PCM_16, 44100, 2, b));
        Rig rig(int16Out(), std::make_shared<FlakyOpener>());
        rig.engine->load({1, gt::tempPath("dies.wav")});
        rig.engine->setNext(TrackSource{2, gt::tempPath("next.wav")});
        REQUIRE(waitFor([&] { return rig.rec->queueEnded > 0; }));
        CHECK(rig.rec->failedCopy() == std::vector<uint64_t>{1});
        const auto started = rig.rec->startedCopy();
        CHECK(started.front() == 1);
        CHECK(started.back() == 2);  // fallback track played
        CHECK(findSeq(asInt16(rig.out->captured()), to16(b)) != std::string::npos);
    }
}

TEST_CASE("Engine: output device lost → reopened and playback continues; permanent loss → Error(DeviceLost)") {
    const auto a = gt::makeSignal(44100 * 2, 2, 41, 16);
    REQUIRE(gt::writeAudio(gt::tempPath("dev.wav"), SF_FORMAT_WAV | SF_FORMAT_PCM_16, 44100, 2, a));
    {
        Rig rig(int16Out());
        rig.engine->load({1, gt::tempPath("dev.wav")});
        REQUIRE(waitFor([&] { return rig.engine->snapshot().positionSeconds > 0.2; }));
        rig.out->injectDeviceError();
        REQUIRE(waitFor([&] { return rig.out->openHistory().size() >= 2; }));
        REQUIRE(waitFor([&] { return rig.rec->queueEnded > 0; }));
        CHECK(rig.engine->snapshot().lastError == ErrorCode::None);
    }
    {
        Rig rig(int16Out());
        rig.engine->load({1, gt::tempPath("dev.wav")});
        REQUIRE(waitFor([&] { return rig.state() == PlaybackState::Playing; }));
        rig.out->failNextOpens(100);
        rig.out->injectDeviceError();
        REQUIRE(waitFor([&] { return rig.state() == PlaybackState::Error; }, 15s));
        CHECK(rig.engine->snapshot().lastError == ErrorCode::DeviceLost);
    }
}

TEST_CASE("Engine: hammering every command from many threads never crashes, deadlocks or corrupts state") {
    const auto a = gt::makeSignal(44100, 2, 51, 16);
    const auto b = gt::makeSignal(48000, 1, 52, 24);
    REQUIRE(gt::writeAudio(gt::tempPath("s1.flac"), SF_FORMAT_FLAC | SF_FORMAT_PCM_16, 44100, 2, a));
    REQUIRE(gt::writeAudio(gt::tempPath("s2.wav"), SF_FORMAT_WAV | SF_FORMAT_PCM_24, 48000, 1, b));
    NullOutput::Options o = int16Out();
    o.speed = 20;
    o.capture = false;
    Rig rig(o);
    std::atomic<bool> stop{false};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&, t] {
            std::mt19937 rng(static_cast<uint32_t>(t));
            while (!stop) {
                switch (rng() % 11) {
                    case 0: rig.engine->load({rng() % 3 + 1, (rng() & 1) ? gt::tempPath("s1.flac") : gt::tempPath("s2.wav")}, LoadOptions{(rng() & 1) != 0, 0}); break;
                    case 1: rig.engine->setNext(TrackSource{rng() % 3 + 10, gt::tempPath("s2.wav")}); break;
                    case 2: rig.engine->play(); break;
                    case 3: rig.engine->pause(); break;
                    case 4: rig.engine->seek((rng() % 1000) / 1000.0); break;
                    case 5: rig.engine->stop(); break;
                    case 6: { EqSettings e; e.enabled = rng() & 1; e.bands[rng() % 10].gainDb = static_cast<double>(rng() % 24) - 12; rig.engine->setEq(e); break; }
                    case 7: rig.engine->setVolume((rng() % 101) / 100.0); break;
                    case 8: rig.engine->setNext(std::nullopt); break;
                    case 9: rig.engine->load({99, gt::tempPath("missing.flac")}); break;
                    default: (void)rig.engine->snapshot(); break;
                }
                std::this_thread::sleep_for(std::chrono::microseconds(rng() % 3000));
            }
        });
    }
    std::this_thread::sleep_for(3s);
    stop = true;
    for (auto& th : threads) th.join();
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(rig.engine->waitIdle(30s));
    MESSAGE("command backlog drained in " << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count() << " ms");
    const auto s = rig.engine->snapshot().state;
    CHECK((s == PlaybackState::Stopped || s == PlaybackState::Playing || s == PlaybackState::Paused || s == PlaybackState::Error));
    rig.engine.reset();  // destructor joins everything while audio may still be running
}

TEST_CASE("Engine: destroying while playing is clean; DSD plays (converted, never reported bit-perfect)") {
    REQUIRE(gt::writeBytes(gt::tempPath("e.dsf"), gt::makeDsf(2822400, 2, 0.3, 440, 0.4)));
    NullOutput::Options o;
    o.format = SampleFormat::Float32;
    o.speed = 4;
    Rig rig(o);
    rig.engine->load({1, gt::tempPath("e.dsf")});
    REQUIRE(waitFor([&] { return rig.state() == PlaybackState::Playing; }));
    const auto snap = rig.engine->snapshot();
    CHECK(snap.source.fromDsd);
    CHECK(snap.output.sampleRate == 176400);
    CHECK_FALSE(snap.bitPerfect);
    rig.engine.reset();
}

TEST_CASE("Engine: PCM tap delivers recent output into caller-owned memory") {
    const auto a = gt::makeSine(44100, 2, 440, 44100, 0.5, 16);
    REQUIRE(gt::writeAudio(gt::tempPath("tap.wav"), SF_FORMAT_WAV | SF_FORMAT_PCM_16, 44100, 2, a));
    NullOutput::Options o = int16Out();
    o.speed = 1.0;
    Rig rig(o);
    rig.engine->enableTap(true);
    rig.engine->load({1, gt::tempPath("tap.wav")});
    REQUIRE(waitFor([&] { return rig.engine->snapshot().positionSeconds > 0.2; }));
    std::vector<float> buf(4096);
    size_t n = 0;
    REQUIRE(waitFor([&] { n = rig.engine->readTap(buf.data(), buf.size()); return n > 0; }));
    CHECK(n % 2 == 0);
    float peak = 0;
    for (size_t i = 0; i < n; ++i) peak = std::max(peak, std::fabs(buf[i]));
    CHECK(peak > 0.3f);
    CHECK(peak < 0.51f);
}
