#include "TestUtil.h"

#include <sndfile.h>
#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>

namespace gt {

namespace {
struct TempDir {
    std::string path;
    TempDir() {
        char tmpl[] = "/tmp/groove-test-XXXXXX";
        const char* p = mkdtemp(tmpl);
        path = p ? p : "/tmp";
    }
    ~TempDir() {
        std::error_code ec;
        if (path != "/tmp") std::filesystem::remove_all(path, ec);
    }
};
constexpr double kPi = 3.14159265358979323846;
}  // namespace

const std::string& tempDir() {
    static TempDir d;
    return d.path;
}

std::string tempPath(const std::string& name) { return tempDir() + "/" + name; }

std::vector<int32_t> makeSignal(uint64_t frames, uint16_t channels, uint32_t seed, uint16_t bits) {
    std::mt19937 rng(seed);
    std::vector<int32_t> v(frames * channels);
    const unsigned shift = 32 - bits;
    for (auto& s : v) {
        const int32_t r = static_cast<int32_t>(rng());
        s = static_cast<int32_t>((static_cast<uint32_t>(r) >> shift) << shift);  // keep only the top `bits` bits
    }
    return v;
}

std::vector<int32_t> makeSine(uint64_t frames, uint16_t channels, double freq, double rate, double amp, uint16_t bits) {
    std::vector<int32_t> v(frames * channels);
    const double scale = std::ldexp(1.0, bits - 1) - 1;
    for (uint64_t i = 0; i < frames; ++i) {
        const int64_t q = std::llround(std::sin(2 * kPi * freq * i / rate) * amp * scale);
        for (uint16_t c = 0; c < channels; ++c) v[i * channels + c] = static_cast<int32_t>(q * (int64_t(1) << (32 - bits)));
    }
    return v;
}

bool writeAudio(const std::string& path, int sfFormat, uint32_t rate, uint16_t channels, const std::vector<int32_t>& samples) {
    SF_INFO info{};
    info.samplerate = static_cast<int>(rate);
    info.channels = channels;
    info.format = sfFormat;
    SNDFILE* f = sf_open(path.c_str(), SFM_WRITE, &info);
    if (!f) return false;
    const sf_count_t frames = static_cast<sf_count_t>(samples.size() / channels);
    const sf_count_t w = sf_writef_int(f, samples.data(), frames);
    sf_close(f);
    return w == frames;
}

namespace {
// 2nd-order sigma-delta modulator, one bit per sample, MSB-first bytes per channel.
std::vector<std::vector<uint8_t>> modulate(uint32_t rate, uint16_t ch, double seconds, double freq, double amp) {
    const uint64_t samples = static_cast<uint64_t>(seconds * rate) / 8 * 8;
    std::vector<std::vector<uint8_t>> out(ch, std::vector<uint8_t>(samples / 8, 0));
    for (uint16_t c = 0; c < ch; ++c) {
        double i1 = 0, i2 = 0, y = 0;
        for (uint64_t n = 0; n < samples; ++n) {
            const double x = amp * std::sin(2 * kPi * freq * n / rate + c * 0.5);
            i1 += x - y;
            i2 += i1 - y;
            y = i2 >= 0 ? 1.0 : -1.0;
            if (y > 0) out[c][n / 8] |= static_cast<uint8_t>(0x80 >> (n % 8));
        }
    }
    return out;
}
void put32le(std::vector<uint8_t>& v, uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back(static_cast<uint8_t>(x >> (8 * i))); }
void put64le(std::vector<uint8_t>& v, uint64_t x) { for (int i = 0; i < 8; ++i) v.push_back(static_cast<uint8_t>(x >> (8 * i))); }
void put32be(std::vector<uint8_t>& v, uint32_t x) { for (int i = 3; i >= 0; --i) v.push_back(static_cast<uint8_t>(x >> (8 * i))); }
void put64be(std::vector<uint8_t>& v, uint64_t x) { for (int i = 7; i >= 0; --i) v.push_back(static_cast<uint8_t>(x >> (8 * i))); }
void put4(std::vector<uint8_t>& v, const char* s) { v.insert(v.end(), s, s + 4); }
uint8_t rev(uint8_t b) { uint8_t r = 0; for (int i = 0; i < 8; ++i) if (b & (1 << i)) r |= static_cast<uint8_t>(0x80 >> i); return r; }
}  // namespace

std::vector<uint8_t> makeDsf(uint32_t rate, uint16_t ch, double seconds, double freq, double amp) {
    auto bits = modulate(rate, ch, seconds, freq, amp);
    const uint32_t block = 4096;
    const uint64_t bytesPerCh = bits[0].size();
    const uint64_t blocks = (bytesPerCh + block - 1) / block;
    const uint64_t dataLen = blocks * block * ch;
    std::vector<uint8_t> v;
    put4(v, "DSD "); put64le(v, 28); put64le(v, 28 + 52 + 12 + dataLen); put64le(v, 0);
    put4(v, "fmt "); put64le(v, 52); put32le(v, 1); put32le(v, 0); put32le(v, ch == 1 ? 1 : 2); put32le(v, ch);
    put32le(v, rate); put32le(v, 1); put64le(v, bytesPerCh * 8); put32le(v, block); put32le(v, 0);
    put4(v, "data"); put64le(v, 12 + dataLen);
    for (uint64_t b = 0; b < blocks; ++b)
        for (uint16_t c = 0; c < ch; ++c)
            for (uint32_t i = 0; i < block; ++i) {
                const uint64_t idx = b * block + i;
                v.push_back(idx < bytesPerCh ? rev(bits[c][idx]) : 0);  // DSF: LSB first
            }
    return v;
}

std::vector<uint8_t> makeDff(uint32_t rate, uint16_t ch, double seconds, double freq, double amp) {
    auto bits = modulate(rate, ch, seconds, freq, amp);
    const uint64_t bytesPerCh = bits[0].size();
    std::vector<uint8_t> prop;
    put4(prop, "SND ");
    put4(prop, "FS  "); put64be(prop, 4); put32be(prop, rate);
    put4(prop, "CHNL"); put64be(prop, 2 + 4 * ch); prop.push_back(0); prop.push_back(static_cast<uint8_t>(ch));
    for (uint16_t c = 0; c < ch; ++c) put4(prop, c == 0 ? "SLFT" : "SRGT");
    put4(prop, "CMPR"); put64be(prop, 4 + 1 + 14 + 1); put4(prop, "DSD "); prop.push_back(14);
    const char* nm = "not compressed";
    prop.insert(prop.end(), nm, nm + 14); prop.push_back(0);
    std::vector<uint8_t> body;
    put4(body, "DSD ");
    put4(body, "FVER"); put64be(body, 4); put32be(body, 0x01050000);
    put4(body, "PROP"); put64be(body, prop.size()); body.insert(body.end(), prop.begin(), prop.end());
    if (prop.size() & 1) body.push_back(0);
    put4(body, "DSD "); put64be(body, bytesPerCh * ch);
    for (uint64_t i = 0; i < bytesPerCh; ++i)
        for (uint16_t c = 0; c < ch; ++c) body.push_back(bits[c][i]);
    std::vector<uint8_t> v;
    put4(v, "FRM8"); put64be(v, body.size());
    v.insert(v.end(), body.begin(), body.end());
    return v;
}

bool writeBytes(const std::string& path, const std::vector<uint8_t>& bytes) {
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(f);
}

std::vector<uint8_t> readBytes(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
}

double goertzel(const std::vector<double>& x, double freq, double rate) {
    // Blackman-Harris window so leakage from a strong tone doesn't mask a weak one nearby.
    const double w = 2 * kPi * freq / rate, c = 2 * std::cos(w);
    const size_t n = x.size();
    double s1 = 0, s2 = 0, wsum = 0;
    for (size_t i = 0; i < n; ++i) {
        const double t = 2 * kPi * static_cast<double>(i) / static_cast<double>(n - 1);
        const double win = 0.35875 - 0.48829 * std::cos(t) + 0.14128 * std::cos(2 * t) - 0.01168 * std::cos(3 * t);
        wsum += win;
        const double s0 = x[i] * win + c * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    const double power = s1 * s1 + s2 * s2 - c * s1 * s2;
    return 2.0 * std::sqrt(std::max(0.0, power)) / wsum;
}

}  // namespace gt
