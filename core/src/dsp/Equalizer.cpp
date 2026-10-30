#include "groove/Equalizer.h"

#include <algorithm>
#include <cmath>
#include <complex>

namespace groove {

namespace {
constexpr double kPi = 3.14159265358979323846;
double clampd(double v, double lo, double hi) { return std::isfinite(v) ? std::min(hi, std::max(lo, v)) : lo; }
}  // namespace

std::array<EqBand, kEqBands> EqSettings::defaultBands() noexcept {
    const double f[kEqBands] = {31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000};
    std::array<EqBand, kEqBands> b{};
    for (size_t i = 0; i < kEqBands; ++i) {
        b[i].freqHz = f[i];
        b[i].gainDb = 0;
        b[i].q = 1.41;
        b[i].type = i == 0 ? FilterType::LowShelf : i == kEqBands - 1 ? FilterType::HighShelf : FilterType::Peaking;
    }
    return b;
}

bool EqSettings::transparent() const noexcept {
    if (!enabled) return true;
    if (std::fabs(preampDb) > 1e-9) return false;
    for (const auto& b : bands)
        if (std::fabs(b.gainDb) > 1e-9) return false;
    return true;
}

Biquad designBiquad(const EqBand& band, double fs) noexcept {
    Biquad c;
    const double gain = clampd(band.gainDb, -24.0, 24.0);
    const double q = clampd(band.q, 0.1, 18.0);
    const double f = clampd(band.freqHz, 10.0, fs * 0.49);
    if (std::fabs(gain) < 1e-9 || fs <= 0) return c;  // identity
    const double A = std::pow(10.0, gain / 40.0);
    const double w0 = 2.0 * kPi * f / fs;
    const double cw = std::cos(w0), sw = std::sin(w0);
    const double alpha = sw / (2.0 * q);
    double b0, b1, b2, a0, a1, a2;
    switch (band.type) {
        case FilterType::Peaking:
            b0 = 1 + alpha * A; b1 = -2 * cw; b2 = 1 - alpha * A;
            a0 = 1 + alpha / A; a1 = -2 * cw; a2 = 1 - alpha / A;
            break;
        case FilterType::LowShelf: {
            const double s = 2 * std::sqrt(A) * alpha;
            b0 = A * ((A + 1) - (A - 1) * cw + s);
            b1 = 2 * A * ((A - 1) - (A + 1) * cw);
            b2 = A * ((A + 1) - (A - 1) * cw - s);
            a0 = (A + 1) + (A - 1) * cw + s;
            a1 = -2 * ((A - 1) + (A + 1) * cw);
            a2 = (A + 1) + (A - 1) * cw - s;
            break;
        }
        case FilterType::HighShelf:
        default: {
            const double s = 2 * std::sqrt(A) * alpha;
            b0 = A * ((A + 1) + (A - 1) * cw + s);
            b1 = -2 * A * ((A - 1) + (A + 1) * cw);
            b2 = A * ((A + 1) + (A - 1) * cw - s);
            a0 = (A + 1) - (A - 1) * cw + s;
            a1 = 2 * ((A - 1) - (A + 1) * cw);
            a2 = (A + 1) - (A - 1) * cw - s;
            break;
        }
    }
    c.b0 = b0 / a0; c.b1 = b1 / a0; c.b2 = b2 / a0; c.a1 = a1 / a0; c.a2 = a2 / a0;
    return c;
}

double cascadeMagnitude(const EqDesign& d, double f, double fs) noexcept {
    const double w = 2.0 * kPi * f / fs;
    const std::complex<double> z1 = std::polar(1.0, -w), z2 = std::polar(1.0, -2 * w);
    double mag = 1.0;
    for (uint32_t i = 0; i < d.stageCount; ++i) {
        const Biquad& c = d.stages[i];
        const std::complex<double> num = c.b0 + c.b1 * z1 + c.b2 * z2;
        const std::complex<double> den = 1.0 + c.a1 * z1 + c.a2 * z2;
        mag *= std::abs(num) / std::abs(den);
    }
    return mag;
}

double cascadePeakDb(const EqDesign& d, double fs) noexcept {
    // 2048 log-spaced points from 10 Hz to just under Nyquist, plus each band centre is implied by the density.
    double peak = 0.0;
    const double lo = std::log(10.0), hi = std::log(fs * 0.4999);
    for (int i = 0; i <= 2048; ++i) {
        const double f = std::exp(lo + (hi - lo) * i / 2048.0);
        peak = std::max(peak, cascadeMagnitude(d, f, fs));
    }
    return 20.0 * std::log10(std::max(peak, 1e-12));
}

double cascadeL1Norm(const EqDesign& d, double fs) noexcept {
    // Impulse response of the cascade (unity gain) summed in absolute value until it has decayed.
    std::array<double, kEqBands * 2> z{};
    double sum = 0.0, tail = 0.0;
    const size_t maxN = static_cast<size_t>(fs * 2.0);  // 2 s is far past any audio-band biquad's decay
    for (size_t n = 0; n < maxN; ++n) {
        double x = n == 0 ? 1.0 : 0.0;
        for (uint32_t i = 0; i < d.stageCount; ++i) {
            const Biquad& c = d.stages[i];
            const double y = c.b0 * x + z[i * 2];
            z[i * 2] = c.b1 * x - c.a1 * y + z[i * 2 + 1];
            z[i * 2 + 1] = c.b2 * x - c.a2 * y;
            x = y;
        }
        sum += std::fabs(x);
        tail = tail * 0.999 + std::fabs(x);
        if (n > 4096 && tail < 1e-12) break;
    }
    return sum;
}

EqDesign designEq(const EqSettings& s, double fs) noexcept {
    EqDesign d;
    d.transparent = s.transparent();
    if (d.transparent || fs <= 0) return d;
    for (const auto& b : s.bands) {
        if (std::fabs(b.gainDb) < 1e-9) continue;
        d.stages[d.stageCount++] = designBiquad(b, fs);
    }
    double headroomDb = 0.0;
    if (d.stageCount > 0) {
        if (s.headroom == HeadroomMode::Guaranteed) {
            const double l1 = cascadeL1Norm(d, fs);
            if (l1 > 1.0) headroomDb = -20.0 * std::log10(l1);
        } else {
            const double peak = cascadePeakDb(d, fs);
            if (peak > 0.0) headroomDb = -peak;
        }
    }
    d.headroomDb = headroomDb;
    const double preamp = std::min(clampd(s.preampDb, -48.0, 12.0), 12.0);
    d.gain = std::pow(10.0, (preamp + headroomDb) / 20.0);
    return d;
}

void EqProcessor::prepare(uint16_t channels) {
    channels_ = channels;
    state_.assign(static_cast<size_t>(channels) * kEqBands, State{});
}

void EqProcessor::reset() noexcept {
    for (auto& s : state_) s = State{};
}

void EqProcessor::process(const EqDesign& d, double* x, uint32_t frames) noexcept {
    const uint16_t ch = channels_;
    if (ch == 0 || state_.size() < static_cast<size_t>(ch) * kEqBands) return;
    for (uint32_t i = 0; i < d.stageCount; ++i) {
        const Biquad c = d.stages[i];
        for (uint16_t k = 0; k < ch; ++k) {
            State& st = state_[static_cast<size_t>(k) * kEqBands + i];
            double z1 = st.z1, z2 = st.z2;
            double* p = x + k;
            for (uint32_t n = 0; n < frames; ++n, p += ch) {
                const double in = *p;
                const double y = c.b0 * in + z1;
                z1 = c.b1 * in - c.a1 * y + z2;
                z2 = c.b2 * in - c.a2 * y;
                *p = y;
            }
            // Flush denormals so a silent tail can't slow the audio thread down.
            st.z1 = std::fabs(z1) < 1e-30 ? 0.0 : z1;
            st.z2 = std::fabs(z2) < 1e-30 ? 0.0 : z2;
        }
    }
    if (d.gain != 1.0) {
        const size_t n = static_cast<size_t>(frames) * ch;
        for (size_t i = 0; i < n; ++i) x[i] *= d.gain;
    }
}

}  // namespace groove
