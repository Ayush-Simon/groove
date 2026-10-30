// 10-band parametric equaliser, double precision, RBJ biquads in transposed direct form II.
//
// Clipping prevention — two modes, because "never clips" has two honest meanings:
//  * PeakResponse (default): the preamp is lowered by the highest point of the combined frequency
//    response. A full-scale sine at any frequency can't clip. Rare transient overshoot is still
//    possible; any such sample is clamped and counted (EngineSnapshot::clippedSamples).
//  * Guaranteed: the preamp is lowered by the L1 norm of the cascade's impulse response, which is the
//    exact worst-case gain for any input. Mathematically cannot clip, but boosts come out quieter.
// When every band is 0 dB and the preamp is 0 dB the EQ is "transparent" and the engine bypasses it
// entirely (bit-perfect path).
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

namespace groove {

enum class FilterType : uint8_t { Peaking, LowShelf, HighShelf };
enum class HeadroomMode : uint8_t { PeakResponse, Guaranteed };

struct EqBand {
    FilterType type = FilterType::Peaking;
    double freqHz = 1000.0;
    double gainDb = 0.0;  // clamped to ±24
    double q = 1.0;       // clamped to 0.1 … 18
};

constexpr size_t kEqBands = 10;

struct EqSettings {
    bool enabled = false;
    std::array<EqBand, kEqBands> bands = defaultBands();
    double preampDb = 0.0;  // user preamp, ≤ 0 recommended
    HeadroomMode headroom = HeadroomMode::PeakResponse;

    /// 31 Hz low shelf, 62…8k peaking (Q 1.41), 16 kHz high shelf — the same layout as the web app.
    static std::array<EqBand, kEqBands> defaultBands() noexcept;
    bool transparent() const noexcept;
};

struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;  // normalised (a0 = 1)
};

/// Coefficients for one settings snapshot. Computed off the audio thread.
struct EqDesign {
    std::array<Biquad, kEqBands> stages{};
    uint32_t stageCount = 0;
    double gain = 1.0;            // linear: user preamp × automatic headroom
    double headroomDb = 0.0;      // automatic part, ≤ 0
    bool transparent = true;
};

/// Pure functions, testable without audio.
Biquad designBiquad(const EqBand& band, double sampleRate) noexcept;
EqDesign designEq(const EqSettings& settings, double sampleRate) noexcept;
/// |H(f)| of the cascade, linear.
double cascadeMagnitude(const EqDesign& d, double freqHz, double sampleRate) noexcept;
double cascadePeakDb(const EqDesign& d, double sampleRate) noexcept;
double cascadeL1Norm(const EqDesign& d, double sampleRate) noexcept;

/// Per-channel filter state. Real-time safe: prepare() allocates, process() doesn't.
class EqProcessor {
public:
    void prepare(uint16_t channels);
    void reset() noexcept;
    /// In-place on interleaved doubles. Applies d.gain. Returns nothing; never allocates or throws.
    void process(const EqDesign& d, double* interleaved, uint32_t frames) noexcept;

private:
    struct State { double z1 = 0, z2 = 0; };
    std::vector<State> state_;  // [channel * kEqBands + stage]
    uint16_t channels_ = 0;
};

/// Lock-free triple buffer: one writer thread publishes, one reader thread (real-time) picks up the
/// newest value without ever blocking or seeing a half-written object.
template <typename T>
class TripleBuffer {
public:
    /// Writer: publish a new value.
    void publish(const T& v) noexcept {
        slots_[back_] = v;
        const int prev = middle_.exchange(back_ | kDirty, std::memory_order_acq_rel);
        back_ = prev & kIndex;
    }
    /// Reader: returns true and updates `out` if something new was published since the last call.
    bool fetch(T& out) noexcept {
        if (!(middle_.load(std::memory_order_acquire) & kDirty)) return false;
        const int prev = middle_.exchange(front_, std::memory_order_acq_rel);
        front_ = prev & kIndex;
        out = slots_[front_];
        return true;
    }

private:
    static constexpr int kDirty = 4, kIndex = 3;
    std::array<T, 3> slots_{};
    int back_ = 0;                  // writer-owned
    int front_ = 1;                 // reader-owned
    std::atomic<int> middle_{2};    // shared
};

}  // namespace groove
