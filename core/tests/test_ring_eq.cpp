#include <atomic>
#include <cmath>
#include <thread>
#include <vector>

#include "doctest.h"
#include "groove/Equalizer.h"
#include "groove/SpscRing.h"

using namespace groove;

TEST_CASE("SpscRing: capacity, wrap-around and partial reads") {
    SpscRing<int32_t> r(10);
    CHECK(r.capacity() == 16);
    std::vector<int32_t> a(12), b(16);
    for (int i = 0; i < 12; ++i) a[i] = i;
    CHECK(r.write(a.data(), 12) == 12);
    CHECK(r.read(b.data(), 8) == 8);
    CHECK(r.write(a.data(), 12) == 12);  // wraps
    CHECK(r.readable() == 16);
    CHECK(r.write(a.data(), 1) == 0);    // full
    CHECK(r.read(b.data(), 16) == 16);
    CHECK(b[0] == 8);
    CHECK(b[3] == 11);
    CHECK(b[4] == 0);
    CHECK(b[15] == 11);
    CHECK(r.totalWritten() == 24);
    CHECK(r.totalRead() == 24);
}

TEST_CASE("SpscRing: concurrent producer/consumer keeps every item in order") {
    SpscRing<uint32_t> r(1024);
    constexpr uint32_t kN = 2'000'000;
    std::atomic<bool> bad{false};
    std::thread consumer([&] {
        uint32_t expect = 0, buf[97];
        while (expect < kN) {
            const size_t n = r.read(buf, 97);
            for (size_t i = 0; i < n; ++i)
                if (buf[i] != expect++) bad = true;
        }
    });
    uint32_t next = 0, buf[61];
    while (next < kN) {
        uint32_t n = 0;
        while (n < 61 && next + n < kN) { buf[n] = next + n; ++n; }
        size_t w = 0;
        while (w < n) w += r.write(buf + w, n - w);
        next += n;
    }
    consumer.join();
    CHECK_FALSE(bad.load());
}

TEST_CASE("TripleBuffer: the reader never sees a torn value") {
    struct Pair { uint64_t a, b; };
    TripleBuffer<Pair> tb;
    std::atomic<bool> stop{false}, torn{false}, backwards{false};
    Pair seen{};
    std::thread reader([&] {
        Pair p{};
        while (!stop) {
            if (tb.fetch(p)) {
                if (p.a != p.b) torn = true;
                if (p.a < seen.a) backwards = true;
                seen = p;
            }
        }
    });
    for (uint64_t i = 1; i < 300000; ++i) tb.publish(Pair{i, i});
    stop = true;
    reader.join();
    CHECK_FALSE(torn.load());
    CHECK_FALSE(backwards.load());
    Pair last = seen;
    tb.fetch(last);  // whatever the reader hadn't picked up yet
    CHECK(last.a == 299999);
}

namespace {
double sineMax(const EqDesign& d, double freq, double fs, uint16_t ch = 1) {
    EqProcessor p;
    p.prepare(ch);
    const uint32_t n = static_cast<uint32_t>(fs);  // 1 s
    std::vector<double> x(static_cast<size_t>(n) * ch);
    for (uint32_t i = 0; i < n; ++i)
        for (uint16_t c = 0; c < ch; ++c) x[i * ch + c] = std::sin(2 * 3.14159265358979 * freq * i / fs);  // full scale
    p.process(d, x.data(), n);
    double m = 0;
    for (size_t i = x.size() / 2; i < x.size(); ++i) m = std::max(m, std::fabs(x[i]));  // steady state
    return m;
}
}  // namespace

TEST_CASE("EQ: flat settings are transparent (engine bypasses them)") {
    EqSettings s;
    s.enabled = true;
    CHECK(s.transparent());
    CHECK(designEq(s, 48000).transparent);
    s.bands[4].gainDb = 0.5;
    CHECK_FALSE(s.transparent());
    s.enabled = false;
    CHECK(s.transparent());
}

TEST_CASE("EQ: peaking band gain matches the request at its centre") {
    EqSettings s;
    s.enabled = true;
    s.bands[5].gainDb = 6.0;  // 1 kHz
    const EqDesign d = designEq(s, 48000);
    CHECK(20 * std::log10(cascadeMagnitude(d, 1000, 48000)) == doctest::Approx(6.0).epsilon(0.001));
    CHECK(20 * std::log10(cascadeMagnitude(d, 100, 48000)) == doctest::Approx(0.0).epsilon(0.05));
}

TEST_CASE("EQ: PeakResponse headroom keeps a full-scale sine below clipping at every boosted band") {
    EqSettings s;
    s.enabled = true;
    for (auto& b : s.bands) b.gainDb = 12.0;  // extreme: everything +12 dB
    s.bands[0].gainDb = 12;
    const EqDesign d = designEq(s, 44100);
    CHECK(d.headroomDb < -11.0);
    for (double f : {31.0, 125.0, 1000.0, 4000.0, 12000.0, 18000.0}) CHECK(sineMax(d, f, 44100) <= 1.0 + 1e-6);
}

TEST_CASE("EQ: Guaranteed headroom cannot clip even for the worst-case input") {
    EqSettings s;
    s.enabled = true;
    s.headroom = HeadroomMode::Guaranteed;
    s.bands[1].gainDb = 12;
    s.bands[5].gainDb = -9;
    s.bands[8].gainDb = 10;
    s.bands[8].q = 6;
    const double fs = 48000;
    const EqDesign d = designEq(s, fs);
    // Worst case for a linear filter: input = sign of its time-reversed impulse response.
    EqDesign unity = d;
    unity.gain = 1.0;
    const size_t L = 48000;
    std::vector<double> h(L, 0.0);
    h[0] = 1.0;
    EqProcessor p;
    p.prepare(1);
    p.process(unity, h.data(), static_cast<uint32_t>(L));
    std::vector<double> x(L);
    for (size_t i = 0; i < L; ++i) x[i] = h[L - 1 - i] >= 0 ? 1.0 : -1.0;
    EqProcessor q;
    q.prepare(1);
    q.process(d, x.data(), static_cast<uint32_t>(L));
    CHECK(std::fabs(x[L - 1]) <= 1.0 + 1e-9);
    double peak = 0;
    for (double v : x) peak = std::max(peak, std::fabs(v));
    CHECK(peak <= 1.0 + 1e-9);
}

TEST_CASE("EQ: processing is stable with silence (no NaN/denormal build-up)") {
    EqSettings s;
    s.enabled = true;
    s.bands[0].gainDb = 12;
    s.bands[9].gainDb = -12;
    const EqDesign d = designEq(s, 192000);
    EqProcessor p;
    p.prepare(2);
    std::vector<double> x(2 * 192000, 0.0);
    x[0] = 1.0;
    p.process(d, x.data(), 192000);
    for (double v : x) REQUIRE(std::isfinite(v));
}
