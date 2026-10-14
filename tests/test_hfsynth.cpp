// HfSynthesizer tests. Owned by the HfSynthesizer task.
// Targets: strength<=0 is a bit-exact null; restoreInBand adds energy only in
// [cutoff, restoreTop] and leaves below-cutoff content essentially unchanged;
// output stays finite and bounded for full-scale input.
#include <catch2/catch_test_macros.hpp>

#include "uvcore/HfSynthesizer.h"

#include <cmath>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;

uv::AudioBuffer makeBandLimited(double sampleRate, std::size_t frames) {
    // Sines up to ~15 kHz only; nothing above the detected cutoff.
    const double tones[] = {440.0, 3000.0, 8000.0, 12000.0, 15000.0};
    uv::AudioBuffer buf;
    buf.sampleRate = sampleRate;
    buf.channels.resize(1);
    std::vector<uv::Sample>& ch = buf.channels[0];
    ch.resize(frames, 0.0);
    for (std::size_t n = 0; n < frames; ++n) {
        double s = 0.0;
        for (double f : tones) {
            s += std::sin(2.0 * kPi * f * static_cast<double>(n) / sampleRate);
        }
        ch[n] = 0.18 * s; // leave headroom; peak well under full scale
    }
    return buf;
}

// Goertzel-style band energy: sum |DFT bin|^2 over bins spanning [loHz, hiHz].
double bandEnergy(const std::vector<uv::Sample>& x, double sampleRate,
                  double loHz, double hiHz) {
    const std::size_t N = x.size();
    const double df = sampleRate / static_cast<double>(N);
    const int kLo = static_cast<int>(std::ceil(loHz / df));
    const int kHi = static_cast<int>(std::floor(hiHz / df));
    double total = 0.0;
    for (int k = kLo; k <= kHi; ++k) {
        const double w = 2.0 * kPi * static_cast<double>(k) / static_cast<double>(N);
        const double cw = std::cos(w);
        const double coeff = 2.0 * cw;
        double s0 = 0.0, s1 = 0.0, s2 = 0.0;
        for (std::size_t n = 0; n < N; ++n) {
            s0 = x[n] + coeff * s1 - s2;
            s2 = s1;
            s1 = s0;
        }
        const double re = s1 - s2 * cw;
        const double im = s2 * std::sin(w);
        total += re * re + im * im;
    }
    return total;
}

} // namespace

TEST_CASE("restoreInBand strength 0 is a bit-exact null", "[hfsynth]") {
    uv::AudioBuffer buf = makeBandLimited(48000.0, 8192);
    const std::vector<uv::Sample> before = buf.channels[0];

    uv::AlgorithmicHfSynthesizer synth;
    uv::HfParams p;
    p.strength = 0.0;
    p.detectedCutoffHz = 15000.0;
    p.restoreTopHz = 20000.0;
    synth.restoreInBand(buf, p);

    REQUIRE(buf.channels[0].size() == before.size());
    for (std::size_t i = 0; i < before.size(); ++i) {
        REQUIRE(buf.channels[0][i] == before[i]); // bit-exact, no modification
    }
}

TEST_CASE("restoreInBand no-op when cutoff at or above restore top", "[hfsynth]") {
    uv::AudioBuffer buf = makeBandLimited(48000.0, 8192);
    const std::vector<uv::Sample> before = buf.channels[0];

    uv::AlgorithmicHfSynthesizer synth;
    uv::HfParams p;
    p.strength = 0.7;
    p.detectedCutoffHz = 20000.0; // >= min(restoreTop, Nyquist)
    p.restoreTopHz = 20000.0;
    synth.restoreInBand(buf, p);

    for (std::size_t i = 0; i < before.size(); ++i) {
        REQUIRE(buf.channels[0][i] == before[i]);
    }
}

TEST_CASE("restoreInBand adds energy in band and preserves below-cutoff", "[hfsynth]") {
    uv::AudioBuffer buf = makeBandLimited(48000.0, 16384);
    const std::vector<uv::Sample> before = buf.channels[0];

    uv::AlgorithmicHfSynthesizer synth;
    uv::HfParams p;
    p.strength = 0.7;
    p.detectedCutoffHz = 15000.0;
    p.restoreTopHz = 20000.0;
    synth.restoreInBand(buf, p);

    const std::vector<uv::Sample>& after = buf.channels[0];

    // Measure strictly above the 15 kHz source tone so "before" is just filter
    // leakage, not the band-edge tone itself.
    const double inBandBefore = bandEnergy(before, 48000.0, 15500.0, 19500.0);
    const double inBandAfter = bandEnergy(after, 48000.0, 15500.0, 19500.0);
    // Measurable new energy where there was essentially none.
    REQUIRE(inBandAfter > inBandBefore * 10.0);
    REQUIRE(inBandAfter > 1.0);

    // Content below the cutoff (up to 14 kHz) stays within a few percent.
    const double belowBefore = bandEnergy(before, 48000.0, 100.0, 14000.0);
    const double belowAfter = bandEnergy(after, 48000.0, 100.0, 14000.0);
    REQUIRE(belowBefore > 0.0);
    const double ratio = belowAfter / belowBefore;
    REQUIRE(ratio > 0.97);
    REQUIRE(ratio < 1.03);
}

TEST_CASE("restoreInBand stays finite and bounded for full-scale input", "[hfsynth]") {
    uv::AudioBuffer buf;
    buf.sampleRate = 48000.0;
    buf.channels.resize(2);
    const std::size_t frames = 8192;
    for (auto& ch : buf.channels) {
        ch.resize(frames);
    }
    // Full-scale broadband-ish content: two near-cutoff tones at unity each.
    for (std::size_t n = 0; n < frames; ++n) {
        const double t = static_cast<double>(n) / 48000.0;
        const double s = 0.5 * std::sin(2.0 * kPi * 7000.0 * t) +
                         0.5 * std::sin(2.0 * kPi * 14000.0 * t);
        buf.channels[0][n] = s;
        buf.channels[1][n] = -s;
    }

    uv::AlgorithmicHfSynthesizer synth;
    uv::HfParams p;
    p.strength = 1.0;
    p.detectedCutoffHz = 15000.0;
    p.restoreTopHz = 20000.0;
    synth.restoreInBand(buf, p);

    for (const auto& ch : buf.channels) {
        for (double s : ch) {
            REQUIRE(std::isfinite(s));
            REQUIRE(std::abs(s) < 4.0);
        }
    }
}

TEST_CASE("exciteUltrasonic no-op when disabled", "[hfsynth]") {
    uv::AudioBuffer buf = makeBandLimited(96000.0, 8192);
    const std::vector<uv::Sample> before = buf.channels[0];

    uv::AlgorithmicHfSynthesizer synth;
    uv::HfParams p;
    p.ultrasonic = false;
    p.strength = 0.7;
    synth.exciteUltrasonic(buf, p);

    for (std::size_t i = 0; i < before.size(); ++i) {
        REQUIRE(buf.channels[0][i] == before[i]);
    }
}

TEST_CASE("exciteUltrasonic adds content above 24 kHz at target rate", "[hfsynth]") {
    // 96 kHz target: Nyquist 48 kHz, so there is room above 24 kHz.
    uv::AudioBuffer buf;
    buf.sampleRate = 96000.0;
    buf.channels.resize(1);
    const std::size_t frames = 16384;
    buf.channels[0].resize(frames);
    for (std::size_t n = 0; n < frames; ++n) {
        const double t = static_cast<double>(n) / 96000.0;
        buf.channels[0][n] = 0.3 * std::sin(2.0 * kPi * 20000.0 * t);
    }
    const std::vector<uv::Sample> before = buf.channels[0];

    uv::AlgorithmicHfSynthesizer synth;
    uv::HfParams p;
    p.ultrasonic = true;
    p.strength = 0.7;
    synth.exciteUltrasonic(buf, p);

    const double ultraBefore = bandEnergy(before, 96000.0, 24000.0, 43200.0);
    const double ultraAfter = bandEnergy(buf.channels[0], 96000.0, 24000.0, 43200.0);
    REQUIRE(ultraAfter > ultraBefore);

    for (double s : buf.channels[0]) {
        REQUIRE(std::isfinite(s));
        REQUIRE(std::abs(s) < 4.0);
    }
}
