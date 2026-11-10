// Filter phase option, exciter and transient shaper.
#include <catch2/catch_test_macros.hpp>

#include "uvcore/Exciter.h"
#include "uvcore/HfSynthesizer.h"
#include "uvcore/Pipeline.h"
#include "uvcore/Resampler.h"
#include "uvcore/TransientShaper.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;

uv::AudioBuffer mono(double sr, std::size_t n) {
    uv::AudioBuffer b;
    b.sampleRate = sr;
    b.sourceBitDepth = 24;
    b.channels.assign(1, std::vector<uv::Sample>(n, 0.0));
    return b;
}

std::size_t argmaxAbs(const std::vector<double>& v) {
    return static_cast<std::size_t>(std::max_element(v.begin(), v.end(), [](double a, double b) {
                                        return std::abs(a) < std::abs(b);
                                    }) - v.begin());
}

double energy(const std::vector<double>& v, std::size_t lo, std::size_t hi) {
    double e = 0.0;
    for (std::size_t i = lo; i < hi && i < v.size(); ++i) e += v[i] * v[i];
    return e;
}

} // namespace

TEST_CASE("minimum-phase upsample has no pre-ringing and keeps the peak aligned", "[phase]") {
    auto in = mono(48000.0, 4800);
    in.channels[0][2400] = 1.0;
    const auto lin = uv::resample(in, 192000.0, uv::FilterPhase::Linear);
    const auto mp = uv::resample(in, 192000.0, uv::FilterPhase::Minimum);
    REQUIRE(mp.frames() == lin.frames());

    const auto pl = argmaxAbs(lin.channels[0]);
    const auto pm = argmaxAbs(mp.channels[0]);
    std::printf("[phase] linear peak %zu, minimum peak %zu\n", pl, pm);
    REQUIRE(std::llabs(static_cast<long long>(pl) - static_cast<long long>(pm)) <= 1);

    // Energy more than 24 samples (125 us) before the peak, relative to total.
    // Closer than that is the attack itself rising, which minimum phase keeps.
    const double total = energy(mp.channels[0], 0, mp.frames());
    const double preM = energy(mp.channels[0], 0, pm - 24);
    const double preL = energy(lin.channels[0], 0, pl - 24);
    const double preMDb = 10.0 * std::log10(preM / total + 1e-30);
    const double preLDb = 10.0 * std::log10(preL / total + 1e-30);
    std::printf("[phase] pre-peak energy: linear %.1f dB, minimum %.1f dB\n", preLDb, preMDb);
    REQUIRE(preMDb < -80.0);
    REQUIRE(preLDb > -40.0);
}

TEST_CASE("exciter is a no-op at zero and adds nothing to a low sine", "[exciter]") {
    auto b = mono(192000.0, 192000);
    for (std::size_t i = 0; i < b.frames(); ++i) b.channels[0][i] = 0.5 * std::sin(2.0 * kPi * 3000.0 * i / 192000.0);
    REQUIRE(uv::exciterSignal(b, {0.0, 16000.0}).empty());

    const auto ex = uv::exciterSignal(b, {1.0, 16000.0});
    REQUIRE(ex.size() == 1);
    const double ratio = std::sqrt(energy(ex[0], 20000, 172000) / energy(b.channels[0], 20000, 172000));
    std::printf("[exciter] 3 kHz sine, added/input rms %.2e\n", ratio);
    REQUIRE(ratio < 1e-3);
}

TEST_CASE("exciter puts its energy on the click", "[exciter]") {
    // A single-sample click at 48 kHz, upsampled: the sinc the user hears as
    // a soft transient. The exciter is told the top of the band is 16 kHz.
    auto src = mono(48000.0, 48000);
    src.channels[0][24000] = 0.8;
    auto up = uv::resample(src, 192000.0);
    const auto ex = uv::exciterSignal(up, {1.0, 16000.0});
    REQUIRE(ex.size() == 1);
    const std::size_t at = argmaxAbs(up.channels[0]);
    const std::size_t w = 96; // 0.5 ms at 192 kHz
    const double total = energy(ex[0], 0, ex[0].size());
    REQUIRE(total > 0.0);
    const double near = energy(ex[0], at - w, at + w);
    std::printf("[exciter] share of exciter energy within 0.5 ms of the click: %.3f\n", near / total);
    REQUIRE(near / total > 0.9);
}

TEST_CASE("transient shaper is a no-op at zero and boosts onsets", "[transient]") {
    // Noise burst with a hard onset at 0.5 s.
    const double sr = 192000.0;
    auto b = mono(sr, 192000);
    unsigned s = 12345;
    for (std::size_t i = 96000; i < b.frames(); ++i) {
        s = s * 1664525u + 1013904223u;
        b.channels[0][i] = 0.3 * ((s >> 8) / 8388608.0 - 1.0);
    }
    std::vector<std::vector<uv::Sample>> added(1, std::vector<uv::Sample>(b.frames()));
    for (std::size_t i = 0; i < b.frames(); ++i) added[0][i] = 0.1 * b.channels[0][i];

    auto same = b;
    uv::applyTransientShaper(same, added, {0.0, 0.0});
    REQUIRE(same.channels[0] == b.channels[0]);

    const auto g = uv::transientGain(b, {1.0, 0.0});
    double onset = 0.0, held = 0.0;
    for (std::size_t i = 96000; i < 96000 + 384; ++i) onset = std::max(onset, g[i]);
    for (std::size_t i = 150000; i < 160000; ++i) held = std::max(held, g[i]);
    std::printf("[transient] gain at onset %.2f, sustained %.2f\n", onset, held);
    REQUIRE(onset > 2.0);
    REQUIRE(held < 1.3);
    // Look-ahead: the gain is already rising before the onset sample.
    REQUIRE(g[96000 - 96] > 1.05);
}

TEST_CASE("pipeline defaults leave new stages out of the signal", "[transient][pipeline]") {
    auto b = mono(48000.0, 24000);
    for (std::size_t i = 0; i < b.frames(); ++i) b.channels[0][i] = 0.3 * std::sin(2.0 * kPi * 1000.0 * i / 48000.0);
    uv::PipelineSettings base;
    base.mode = uv::PipelineSettings::Mode::Rescue;
    auto shaped = base;
    shaped.transientAttack = 0.0; // explicit zeros still mean off
    shaped.exciterAmount = 0.0;
    REQUIRE(uv::processBuffer(b, base).channels == uv::processBuffer(b, shaped).channels);

    // The extension's added band reproduces the in-place change exactly.
    auto up = uv::resample(b, 192000.0);
    auto copy = up;
    uv::HfParams hp;
    hp.strength = 0.5;
    hp.cutoffHz = 16000.0;
    hp.sourceNyquistHz = 24000.0;
    std::vector<std::vector<uv::Sample>> added;
    uv::AlgorithmicHfSynthesizer().extend(up, hp, &added);
    REQUIRE(added.size() == 1);
    double maxErr = 0.0;
    for (std::size_t i = 0; i < up.frames(); ++i)
        maxErr = std::max(maxErr, std::abs(up.channels[0][i] - copy.channels[0][i] - added[0][i]));
    REQUIRE(maxErr < 1e-12);
}
