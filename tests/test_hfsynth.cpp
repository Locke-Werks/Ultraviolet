// HfSynthesizer::extend tests. Material is a deterministic music-like signal
// (pink-ish noise, harmonic tones, bursts and gaps) generated at 48 kHz,
// low-passed with an encoder-like skirt at 16 kHz and resampled to 192 kHz, so
// the synthesizer sees what it sees in the pipeline. Explicit cutoffs are
// passed to extend(); nothing here depends on the spectrum detector.
#include <catch2/catch_test_macros.hpp>

#include "uvcore/HfSynthesizer.h"
#include "uvcore/Resampler.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <random>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;
using cplx = std::complex<double>;

void fft(std::vector<cplx>& a, bool inverse) {
    const std::size_t n = a.size();
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (std::size_t len = 2; len <= n; len <<= 1) {
        const double ang = 2.0 * kPi / static_cast<double>(len) * (inverse ? 1.0 : -1.0);
        const cplx wl(std::cos(ang), std::sin(ang));
        for (std::size_t i = 0; i < n; i += len) {
            cplx w(1.0, 0.0);
            for (std::size_t k = 0; k < len / 2; ++k) {
                const cplx u = a[i + k];
                const cplx v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
    if (inverse) {
        for (cplx& v : a) v /= static_cast<double>(n);
    }
}

constexpr double kFc = 16000.0;          // top shoulder of the encoder skirt
constexpr double kSrcRate = 48000.0;
constexpr double kRate = 192000.0;
constexpr std::size_t kSrcLen = 1u << 17; // about 2.7 s at 48 kHz

// Burst envelope: 100 ms of silence, then 400 ms bursts with a fast attack and
// a decay, separated by 300 ms gaps of true silence.
double envelopeAt(double t) {
    const double t0 = 0.1, period = 0.7, on = 0.4;
    if (t < t0) return 0.0;
    const double ph = std::fmod(t - t0, period);
    if (ph >= on) return 0.0;
    const double attack = std::min(1.0, ph / 0.005);
    const double tail = std::min(1.0, (on - ph) / 0.005);
    return attack * tail * std::exp(-ph / 0.5);
}

bool inGapCentre(double t) {
    const double t0 = 0.1, period = 0.7, on = 0.4;
    if (t < t0) return false;
    const double ph = std::fmod(t - t0, period);
    return ph > on + 0.12 && ph < on + 0.22;
}

bool inBurstCentre(double t) {
    const double t0 = 0.1, period = 0.7;
    if (t < t0) return false;
    const double ph = std::fmod(t - t0, period);
    return ph > 0.05 && ph < 0.3;
}

// Gain of the encoder skirt in dB: flat to kFc, a raised-cosine drop of 70 dB
// over 400 Hz, then on down to -120 dB.
double skirtDb(double f) {
    if (f <= kFc) return 0.0;
    if (f <= kFc + 400.0) return -35.0 * (1.0 - std::cos(kPi * (f - kFc) / 400.0));
    return std::max(-120.0, -70.0 - 0.05 * (f - kFc - 400.0));
}

uv::AudioBuffer makeMusicLike(double peak) {
    std::mt19937 rng(1234);
    std::uniform_real_distribution<double> uni(0.0, 2.0 * kPi);
    const std::size_t n = kSrcLen;

    // Pink-ish noise with a -4.5 dB/oct power slope, built in the frequency
    // domain so the slope is exact.
    std::vector<cplx> spec(n, cplx(0.0, 0.0));
    const double ampExp = -4.5 / (20.0 * std::log10(2.0));
    for (std::size_t k = 1; k < n / 2; ++k) {
        const double f = static_cast<double>(k) * kSrcRate / static_cast<double>(n);
        const double a = std::pow(std::max(f, 30.0) / 1000.0, ampExp);
        spec[k] = std::polar(a, uni(rng));
        spec[n - k] = std::conj(spec[k]);
    }
    fft(spec, true);
    std::vector<double> x(n);
    double rmsNoise = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        x[i] = spec[i].real();
        rmsNoise += x[i] * x[i];
    }
    rmsNoise = std::sqrt(rmsNoise / n);
    for (double& v : x) v /= rmsNoise;

    // Harmonic tones on a 220 Hz fundamental, rolling off with harmonic number.
    for (int h = 1; h * 220.0 < 15000.0; h += (h < 8 ? 1 : 3)) {
        const double f = h * 220.0;
        const double a = 0.6 / std::pow(static_cast<double>(h), 0.8);
        const double ph = uni(rng);
        for (std::size_t i = 0; i < n; ++i) x[i] += a * std::sin(2.0 * kPi * f * i / kSrcRate + ph);
    }

    for (std::size_t i = 0; i < n; ++i) x[i] *= envelopeAt(static_cast<double>(i) / kSrcRate);

    // Encoder skirt. Circular filtering is harmless: the envelope is silent at
    // both ends.
    std::vector<cplx> X(x.begin(), x.end());
    fft(X, false);
    for (std::size_t k = 0; k <= n / 2; ++k) {
        const double f = static_cast<double>(k) * kSrcRate / static_cast<double>(n);
        const double g = std::pow(10.0, skirtDb(f) / 20.0);
        X[k] *= g;
        if (k > 0 && k < n / 2) X[n - k] *= g;
    }
    fft(X, true);
    double mx = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        x[i] = X[i].real();
        mx = std::max(mx, std::abs(x[i]));
    }
    for (double& v : x) v *= peak / mx;

    uv::AudioBuffer b;
    b.sampleRate = kSrcRate;
    b.channels.push_back(x);
    return uv::resample(b, kRate);
}

const uv::AudioBuffer& musicAt192() {
    static const uv::AudioBuffer b = makeMusicLike(0.5);
    return b;
}

// Long-term power spectrum per bin: Welch average with a 4-term
// Blackman-Harris window, whose -92 dB sidelobes keep the measurement itself
// from smearing energy across the cutoff.
struct LongTerm {
    double rate = 0.0;
    int n = 0;
    std::vector<double> pow;

    // 1/6-octave smoothed level in dB at f.
    double db(double f) const {
        const double df = rate / n;
        const int lo = std::max(1, static_cast<int>(std::floor(f * std::pow(2.0, -1.0 / 12.0) / df)));
        const int hi = std::min(n / 2, static_cast<int>(std::ceil(f * std::pow(2.0, 1.0 / 12.0) / df)));
        double acc = 0.0;
        for (int k = lo; k <= hi; ++k) acc += pow[k];
        return 10.0 * std::log10(acc / (hi - lo + 1) + 1e-300);
    }

    double bandEnergy(double loHz, double hiHz) const {
        const double df = rate / n;
        double acc = 0.0;
        for (int k = 0; k <= n / 2; ++k) {
            const double f = k * df;
            if (f >= loHz && f < hiHz) acc += pow[k];
        }
        return acc;
    }
};

std::vector<double> blackmanHarris(int n) {
    std::vector<double> w(n);
    for (int i = 0; i < n; ++i) {
        const double t = 2.0 * kPi * i / n;
        w[i] = 0.35875 - 0.48829 * std::cos(t) + 0.14128 * std::cos(2 * t) - 0.01168 * std::cos(3 * t);
    }
    return w;
}

LongTerm longTerm(const std::vector<double>& x, double rate, int n = 8192) {
    LongTerm lt;
    lt.rate = rate;
    lt.n = n;
    lt.pow.assign(n / 2 + 1, 0.0);
    const std::vector<double> w = blackmanHarris(n);
    std::vector<cplx> buf(n);
    int count = 0;
    for (std::size_t s = 0; s + n <= x.size(); s += n / 2) {
        for (int i = 0; i < n; ++i) buf[i] = cplx(x[s + i] * w[i], 0.0);
        fft(buf, false);
        for (int k = 0; k <= n / 2; ++k) lt.pow[k] += std::norm(buf[k]);
        ++count;
    }
    for (double& p : lt.pow) p /= std::max(count, 1);
    return lt;
}

std::vector<double> diff(const std::vector<double>& a, const std::vector<double>& b) {
    std::vector<double> d(a.size());
    for (std::size_t i = 0; i < a.size(); ++i) d[i] = a[i] - b[i];
    return d;
}

uv::HfParams params(double strength, bool ultrasonic, double rolloff = 0.5) {
    uv::HfParams p;
    p.strength = strength;
    p.cutoffHz = kFc;
    p.sourceNyquistHz = 0.5 * kSrcRate;
    p.ultrasonic = ultrasonic;
    p.rolloff = rolloff;
    return p;
}

uv::AudioBuffer extended(const uv::HfParams& p) {
    uv::AudioBuffer b = musicAt192();
    uv::AlgorithmicHfSynthesizer().extend(b, p);
    return b;
}

// The designed decay above fc on top of the music's own slope, in dB.
double designedExtraDb(double f, double rolloff) {
    const double u = std::log2(f / kFc);
    return u > 0.0 ? (6.0 + 24.0 * rolloff) * u + 3.0 * u * u : 0.0;
}

double blendWidth() { return std::max(200.0, 0.04 * kFc); }

} // namespace

TEST_CASE("extend strength 0 is a bit-exact null", "[hfsynth]") {
    uv::AudioBuffer b = musicAt192();
    uv::AlgorithmicHfSynthesizer().extend(b, params(0.0, true));
    REQUIRE(b.channels[0] == musicAt192().channels[0]);
}

TEST_CASE("extend is a no-op when the cutoff is at or above the fade top", "[hfsynth]") {
    uv::AudioBuffer b = musicAt192();
    uv::HfParams p = params(1.0, false);
    p.cutoffHz = 22000.0;
    p.sourceNyquistHz = 48000.0;
    uv::AlgorithmicHfSynthesizer().extend(b, p);
    REQUIRE(b.channels[0] == musicAt192().channels[0]);
}

TEST_CASE("extend leaves content below the blend region untouched", "[hfsynth]") {
    const uv::AudioBuffer out = extended(params(1.0, true));
    const auto& in = musicAt192().channels[0];
    const LongTerm ltIn = longTerm(in, kRate);
    const LongTerm ltDiff = longTerm(diff(out.channels[0], in), kRate);
    const double edge = kFc - blendWidth();
    const double eIn = ltIn.bandEnergy(1.0, edge);
    const double eDiff = ltDiff.bandEnergy(1.0, edge);
    const double ratioDb = 10.0 * std::log10(eDiff / eIn + 1e-300);
    std::printf("[hfsynth] change below fc-D: %.1f dB relative to input\n", ratioDb);
    REQUIRE(ratioDb < -80.0);
}

TEST_CASE("extend continues the source trend through the cutoff with no seam", "[hfsynth]") {
    const double rolloff = 0.5;
    const uv::AudioBuffer out = extended(params(1.0, true, rolloff));
    const LongTerm lt = longTerm(out.channels[0], kRate);

    // Line fit (dB against log2 f) over [fc/2, 0.9 fc] in 1/24-octave steps.
    double sw = 0, sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (double u = -1.0; u <= std::log2(0.9) + 1e-9; u += 1.0 / 24.0) {
        const double y = lt.db(kFc * std::pow(2.0, u));
        sw += 1; sx += u; sy += y; sxx += u * u; sxy += u * y;
    }
    const double slope = (sw * sxy - sx * sy) / (sw * sxx - sx * sx);
    const double intercept = (sy - slope * sx) / sw;
    auto expected = [&](double f) {
        const double u = std::log2(f / kFc);
        return intercept + slope * u - designedExtraDb(f, rolloff);
    };

    std::printf("[hfsynth] fitted source slope %.2f dB/oct, level at fc %.2f dB\n", slope, intercept);
    std::printf("[hfsynth]    f(Hz)   measured  expected  delta\n");
    double worst = 0.0;
    for (double u = std::log2(0.8); u <= std::log2(1.5) + 1e-9; u += 1.0 / 48.0) {
        const double f = kFc * std::pow(2.0, u);
        const double m = lt.db(f);
        const double e = expected(f);
        std::printf("[hfsynth] %8.0f  %8.2f  %8.2f  %+6.2f\n", f, m, e, m - e);
        worst = std::max(worst, std::abs(m - e));
        if (f >= kFc && f <= 1.25 * kFc) REQUIRE(std::abs(m - e) < 3.0);
    }
    std::printf("[hfsynth] worst deviation in [0.8 fc, 1.5 fc]: %.2f dB\n", worst);
    REQUIRE(worst < 3.0);
}

TEST_CASE("extend decays smoothly with no edges at 20 or 24 kHz", "[hfsynth]") {
    const uv::AudioBuffer out = extended(params(1.0, true));
    const LongTerm lt = longTerm(out.channels[0], kRate);
    const double fadeTop = 48000.0;

    double runningMin = lt.db(kFc);
    for (double u = 0.0; kFc * std::pow(2.0, u) < fadeTop; u += 1.0 / 24.0) {
        const double v = lt.db(kFc * std::pow(2.0, u));
        REQUIRE(v <= runningMin + 1.5);
        runningMin = std::min(runningMin, v);
    }

    const double step24 = lt.db(23500.0) - lt.db(24500.0);
    const double step20 = lt.db(19500.0) - lt.db(20500.0);
    std::printf("[hfsynth] level change across 24 kHz: %.2f dB, across 20 kHz: %.2f dB\n",
                step24, step20);
    REQUIRE(std::abs(step24) < 3.0);
    REQUIRE(std::abs(step20) < 3.0);
}

TEST_CASE("extend fades out completely above the fade top", "[hfsynth]") {
    const auto& in = musicAt192().channels[0];
    {
        const uv::AudioBuffer out = extended(params(1.0, true));
        const LongTerm lt = longTerm(diff(out.channels[0], in), kRate);
        const double ref = longTerm(out.channels[0], kRate).db(kFc);
        double worst = -400.0;
        // First centre whose 1/6-octave window lies wholly above 48 kHz.
        for (double f = 48000.0 * std::pow(2.0, 1.0 / 12.0); f < 95000.0; f *= std::pow(2.0, 1.0 / 24.0)) worst = std::max(worst, lt.db(f));
        std::printf("[hfsynth] ultrasonic: added content above 48 kHz peaks %.1f dB below fc\n", ref - worst);
        REQUIRE(ref - worst > 80.0);
    }
    {
        const uv::AudioBuffer out = extended(params(1.0, false));
        const LongTerm lt = longTerm(out.channels[0], kRate);
        const double ref = lt.db(kFc);
        double worst = -400.0;
        for (double f = 22500.0 * std::pow(2.0, 1.0 / 12.0); f < 95000.0; f *= std::pow(2.0, 1.0 / 24.0)) worst = std::max(worst, lt.db(f));
        std::printf("[hfsynth] ultrasonic off: output above 22.5 kHz peaks %.1f dB below fc\n", ref - worst);
        REQUIRE(ref - worst > 80.0);
    }
}

TEST_CASE("extend follows transients: gaps stay silent", "[hfsynth]") {
    const uv::AudioBuffer out = extended(params(1.0, true));
    const std::vector<double> d = diff(out.channels[0], musicAt192().channels[0]);

    // Short-time energy of the extension above fc.
    const int n = 2048;
    const std::vector<double> w = blackmanHarris(n);
    const double df = kRate / n;
    std::vector<cplx> buf(n);
    double gap = 0.0, burst = 0.0;
    int gapCount = 0, burstCount = 0;
    for (std::size_t s = 0; s + n <= d.size(); s += n / 2) {
        const double t = (s + n / 2) / kRate;
        const bool g = inGapCentre(t), b = inBurstCentre(t);
        if (!g && !b) continue;
        for (int i = 0; i < n; ++i) buf[i] = cplx(d[s + i] * w[i], 0.0);
        fft(buf, false);
        double e = 0.0;
        for (int k = static_cast<int>(kFc / df); k <= n / 2; ++k) e += std::norm(buf[k]);
        if (g) { gap += e; ++gapCount; } else { burst += e; ++burstCount; }
    }
    REQUIRE(gapCount > 0);
    REQUIRE(burstCount > 0);
    const double ratioDb = 10.0 * std::log10((gap / gapCount + 1e-300) / (burst / burstCount));
    std::printf("[hfsynth] extension energy in gaps vs bursts: %.1f dB\n", ratioDb);
    REQUIRE(ratioDb < -50.0);
}

TEST_CASE("extend rolloff control steepens the decay", "[hfsynth]") {
    const double gentle = longTerm(extended(params(1.0, true, 0.0)).channels[0], kRate).db(1.5 * kFc);
    const double steep = longTerm(extended(params(1.0, true, 1.0)).channels[0], kRate).db(1.5 * kFc);
    std::printf("[hfsynth] level at 1.5 fc: rolloff 0 %.1f dB, rolloff 1 %.1f dB\n", gentle, steep);
    REQUIRE(steep < gentle - 6.0);
}

TEST_CASE("extend is deterministic, finite and bounded", "[hfsynth]") {
    const uv::AudioBuffer a = extended(params(0.8, true));
    const uv::AudioBuffer b = extended(params(0.8, true));
    REQUIRE(a.channels[0] == b.channels[0]);

    uv::AudioBuffer full = makeMusicLike(1.0);
    full.channels.push_back(full.channels[0]);
    for (double& v : full.channels[1]) v = -v;
    uv::AlgorithmicHfSynthesizer().extend(full, params(1.0, true, 0.0));
    REQUIRE(full.channels[0].size() == a.channels[0].size());
    for (const auto& ch : full.channels) {
        for (double v : ch) {
            REQUIRE(std::isfinite(v));
            REQUIRE(std::abs(v) < 4.0);
        }
    }
}
