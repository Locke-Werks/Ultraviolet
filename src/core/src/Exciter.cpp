#include "uvcore/Exciter.h"

#include <algorithm>
#include <cmath>

namespace uv {

namespace {

constexpr double kPi = 3.14159265358979323846;

struct Biquad {
    double b0, b1, b2, a1, a2;
};

// RBJ cookbook sections. Two in cascade with these Qs make a 4th-order
// Butterworth; run forward and backward that is 8th order and zero phase.
constexpr double kButterQ[2] = {0.54119610, 1.30656296};

Biquad lowpass(double fc, double sr, double q) {
    const double w = 2.0 * kPi * fc / sr, c = std::cos(w), a = std::sin(w) / (2.0 * q);
    const double a0 = 1.0 + a;
    return {(1.0 - c) / 2.0 / a0, (1.0 - c) / a0, (1.0 - c) / 2.0 / a0, -2.0 * c / a0, (1.0 - a) / a0};
}

Biquad highpass(double fc, double sr, double q) {
    const double w = 2.0 * kPi * fc / sr, c = std::cos(w), a = std::sin(w) / (2.0 * q);
    const double a0 = 1.0 + a;
    return {(1.0 + c) / 2.0 / a0, -(1.0 + c) / a0, (1.0 + c) / 2.0 / a0, -2.0 * c / a0, (1.0 - a) / a0};
}

void runBiquad(std::vector<double>& x, const Biquad& f) {
    double z1 = 0.0, z2 = 0.0;
    for (double& v : x) {
        const double y = f.b0 * v + z1;
        z1 = f.b1 * v - f.a1 * y + z2;
        z2 = f.b2 * v - f.a2 * y;
        v = y;
    }
}

void filtfilt(std::vector<double>& x, const std::vector<Biquad>& chain) {
    for (const auto& f : chain) runBiquad(x, f);
    std::reverse(x.begin(), x.end());
    for (const auto& f : chain) runBiquad(x, f);
    std::reverse(x.begin(), x.end());
}

std::vector<Biquad> butter(bool high, double fc, double sr) {
    std::vector<Biquad> c;
    for (double q : kButterQ) c.push_back(high ? highpass(fc, sr, q) : lowpass(fc, sr, q));
    return c;
}

} // namespace

std::vector<std::vector<Sample>> exciterSignal(const AudioBuffer& buffer, const ExciterParams& params) {
    const double sr = buffer.sampleRate;
    const double fc = params.cutoffHz;
    if (params.amount <= 0.0 || buffer.empty() || sr <= 0.0 || fc <= 0.0) return {};
    // The harmonics need somewhere to go above the cutoff.
    if (fc >= 0.45 * sr) return {};

    const auto bandHp = butter(true, 0.5 * fc, sr);
    const auto bandLp = butter(false, fc, sr);
    const auto keepHp = butter(true, fc, sr);
    // Keeps the shaper's own aliasing out: anything it makes above this is
    // dropped instead of folding back down.
    const auto topLp = butter(false, std::min(0.45 * sr, 4.0 * fc), sr);

    std::vector<std::vector<Sample>> out(buffer.channels.size());
    for (std::size_t ch = 0; ch < buffer.channels.size(); ++ch) {
        std::vector<double> band(buffer.channels[ch].begin(), buffer.channels[ch].end());
        filtfilt(band, bandHp);
        filtfilt(band, bandLp);

        // Normalize the drive to the band's level so the character does not
        // depend on how loud the track is. Peak over a high percentile would be
        // more robust; RMS is enough because the shaper saturates gently.
        double e = 0.0;
        for (double v : band) e += v * v;
        const double rms = std::sqrt(e / static_cast<double>(band.size()));
        if (rms < 1e-9) {
            out[ch].assign(band.size(), 0.0);
            continue;
        }
        const double drive = 1.0 / (3.0 * rms);

        // Odd harmonics from tanh, even harmonics from the asymmetric term.
        // The linear part of the input is subtracted so only distortion
        // products remain; the high-pass below would remove most of it anyway.
        for (double& v : band) {
            const double u = v * drive;
            const double t = std::tanh(u);
            const double odd = t - u;
            const double even = 0.5 * t * t;
            v = (odd + even) / drive;
        }
        filtfilt(band, keepHp);
        filtfilt(band, topLp);

        const double gain = params.amount;
        for (double& v : band) v *= gain;
        out[ch] = std::move(band);
    }
    return out;
}

} // namespace uv
