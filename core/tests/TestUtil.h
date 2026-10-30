#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "doctest.h"
#include "groove/ByteSource.h"

// So doctest can print these in failure messages.
namespace doctest {
template <> struct StringMaker<groove::ErrorCode> {
    static String convert(groove::ErrorCode e) { return groove::errorName(e); }
};
template <> struct StringMaker<groove::PlaybackState> {
    static String convert(groove::PlaybackState s) { return groove::stateName(s); }
};
}  // namespace doctest

namespace gt {

/// A per-run temporary directory (removed at exit).
const std::string& tempDir();
std::string tempPath(const std::string& name);

/// Deterministic pseudo-random interleaved samples, quantised to `bits` and left-justified (engine format).
std::vector<int32_t> makeSignal(uint64_t frames, uint16_t channels, uint32_t seed, uint16_t bits);
/// Sine, left-justified, quantised to `bits`.
std::vector<int32_t> makeSine(uint64_t frames, uint16_t channels, double freq, double rate, double amp, uint16_t bits);

/// Writes left-justified samples with libsndfile. sfFormat e.g. SF_FORMAT_WAV | SF_FORMAT_PCM_16.
bool writeAudio(const std::string& path, int sfFormat, uint32_t rate, uint16_t channels, const std::vector<int32_t>& samples);

/// 1-bit DSD from a 2nd-order sigma-delta modulator: a sine of `freq` Hz at `amp` (0..1 of full modulation).
std::vector<uint8_t> makeDsf(uint32_t dsdRate, uint16_t channels, double seconds, double freq, double amp);
std::vector<uint8_t> makeDff(uint32_t dsdRate, uint16_t channels, double seconds, double freq, double amp);
bool writeBytes(const std::string& path, const std::vector<uint8_t>& bytes);
std::vector<uint8_t> readBytes(const std::string& path);

/// Magnitude of `freq` in a mono float signal (Goertzel), normalised so a full-scale sine ≈ 1.
double goertzel(const std::vector<double>& x, double freq, double rate);

/// A source that starts failing after `failAfterBytes` (simulates pulling an SD card mid-song).
class FlakySource final : public groove::ByteSource {
public:
    FlakySource(std::unique_ptr<groove::ByteSource> inner, uint64_t failAfterBytes) : inner_(std::move(inner)), limit_(failAfterBytes) {}
    uint64_t size() const noexcept override { return inner_->size(); }
    groove::Result<size_t> readAt(uint64_t off, void* dst, size_t len) noexcept override {
        if (off + len > limit_) return groove::Result<size_t>::fail(groove::ErrorCode::Io, "simulated storage removal");
        return inner_->readAt(off, dst, len);
    }
    const std::string& name() const noexcept override { return inner_->name(); }

private:
    std::unique_ptr<groove::ByteSource> inner_;
    uint64_t limit_;
};

}  // namespace gt
