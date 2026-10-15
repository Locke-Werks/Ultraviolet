// Audition tests. Owned by the audition task.
#include <catch2/catch_test_macros.hpp>

#include "uvcore/Audition.h"
#include "uvcore/Pipeline.h"
#include "uvcore/Resampler.h"
#include "uvcore/SpectralAnalyzer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;

uv::AudioBuffer sine(double freq, double rate, double seconds, double amp = 0.5) {
    uv::AudioBuffer b;
    b.sampleRate = rate;
    b.channels.assign(1, std::vector<uv::Sample>(static_cast<std::size_t>(rate * seconds)));
    for (std::size_t i = 0; i < b.channels[0].size(); ++i)
        b.channels[0][i] = amp * std::sin(2.0 * kPi * freq * static_cast<double>(i) / rate);
    return b;
}

// Hann-windowed Goertzel power at one frequency.
double power(const std::vector<uv::Sample>& x, double rate, double f) {
    const std::size_t n = x.size();
    const double w = 2.0 * kPi * f / rate, c = 2.0 * std::cos(w);
    double s1 = 0.0, s2 = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double win = 0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) / static_cast<double>(n));
        const double s = x[i] * win + c * s1 - s2;
        s2 = s1;
        s1 = s;
    }
    return s1 * s1 + s2 * s2 - c * s1 * s2;
}

double bandEnergy(const std::vector<uv::Sample>& x, double rate, double lo, double hi, double step = 50.0) {
    double e = 0.0;
    for (double f = lo; f <= hi; f += step) e += power(x, rate, f);
    return e;
}

double dominant(const std::vector<uv::Sample>& x, double rate, double lo, double hi, double step) {
    double best = lo, bestP = -1.0;
    for (double f = lo; f <= hi; f += step) {
        const double p = power(x, rate, f);
        if (p > bestP) {
            bestP = p;
            best = f;
        }
    }
    return best;
}

double rms(const std::vector<uv::Sample>& x, std::size_t a, std::size_t b) {
    double s = 0.0;
    for (std::size_t i = a; i < b; ++i) s += x[i] * x[i];
    return std::sqrt(s / static_cast<double>(b - a));
}

std::vector<uv::Sample> slice(const std::vector<uv::Sample>& v, std::size_t a, std::size_t n) {
    return std::vector<uv::Sample>(v.begin() + static_cast<std::ptrdiff_t>(a),
                                   v.begin() + static_cast<std::ptrdiff_t>(a + n));
}

// Sines below 16 kHz plus noise low-passed at 15 kHz, so nothing above 16 kHz.
uv::AudioBuffer bandLimitedSource() {
    const double rate = 48000.0;
    const std::size_t n = 48000;
    uv::AudioBuffer b;
    b.sampleRate = rate;
    b.sourceBitDepth = 24;
    b.channels.assign(2, std::vector<uv::Sample>(n, 0.0));
    std::mt19937 rng(1234);
    std::normal_distribution<double> g(0.0, 1.0);
    std::vector<double> noise(n);
    for (auto& v : noise) v = g(rng);
    const int taps = 255, mid = taps / 2;
    std::vector<double> h(taps);
    const double fc = 15000.0 / rate;
    for (int i = 0; i < taps; ++i) {
        const int t = i - mid;
        const double sinc = t == 0 ? 2.0 * fc : std::sin(2.0 * kPi * fc * t) / (kPi * t);
        h[i] = sinc * (0.42 - 0.5 * std::cos(2.0 * kPi * i / (taps - 1)) + 0.08 * std::cos(4.0 * kPi * i / (taps - 1)));
    }
    for (int ch = 0; ch < 2; ++ch) {
        for (std::size_t i = 0; i < n; ++i) {
            double acc = 0.0;
            for (int j = 0; j < taps; ++j) {
                const long long k = static_cast<long long>(i) - j;
                if (k >= 0) acc += h[j] * noise[static_cast<std::size_t>(k)];
            }
            const double t = static_cast<double>(i) / rate;
            b.channels[ch][i] = 0.05 * acc + 0.2 * std::sin(2.0 * kPi * 440.0 * t) +
                                0.1 * std::sin(2.0 * kPi * 3000.0 * t) + 0.05 * std::sin(2.0 * kPi * 12000.0 * t);
        }
    }
    return b;
}

} // namespace

TEST_CASE("addedComponent is silent in Mastering mode", "[audition]") {
    const uv::AudioBuffer src = bandLimitedSource();
    uv::PipelineSettings s;
    s.targetRate = 192000.0;
    s.outputFormat = uv::OutputFormat::Float32;
    s.mode = uv::PipelineSettings::Mode::Mastering;
    const uv::AudioBuffer proc = uv::processBuffer(src, s);
    const uv::AudioBuffer added = uv::addedComponent(proc, src);
    REQUIRE(added.sampleRate == proc.sampleRate);
    REQUIRE(added.frames() == proc.frames());
    double peak = 0.0;
    for (const auto& c : added.channels)
        for (double x : c) peak = std::max(peak, std::abs(x));
    std::printf("[audition] mastering added peak = %.3g\n", peak);
    REQUIRE(peak < 1e-6);
}

TEST_CASE("addedComponent isolates the Rescue extension", "[audition]") {
    const uv::AudioBuffer src = bandLimitedSource();
    uv::PipelineSettings s;
    s.targetRate = 192000.0;
    s.outputFormat = uv::OutputFormat::Float32;
    s.mode = uv::PipelineSettings::Mode::Rescue;
    s.hfStrength = 1.0;
    s.ultrasonic = true;
    const uv::AudioBuffer proc = uv::processBuffer(src, s);
    const uv::AudioBuffer added = uv::addedComponent(proc, src);
    REQUIRE(added.frames() == proc.frames());
    // The extension blends in over [fc - D, fc] with D = max(200, 0.04 fc),
    // so "below" stops short of that ramp for the detected cutoff.
    const double fc = uv::analyzeSpectrum(src).cutoffHz;
    const double belowTop = std::min(15000.0, fc - std::max(200.0, 0.04 * fc) - 300.0);
    const auto seg = slice(added.channels[0], 72000, 48000);
    const double below = bandEnergy(seg, 192000.0, 200.0, belowTop);
    const double above = bandEnergy(seg, 192000.0, 16500.0, 40000.0);
    const double db = 10.0 * std::log10(above / std::max(below, 1e-300));
    std::printf("[audition] rescue added: fc %.0f Hz, below to %.0f Hz, above/below = %.1f dB\n", fc,
                belowTop, db);
    REQUIRE(belowTop > 12000.0);
    REQUIRE(above > 0.0);
    REQUIRE(db > 60.0);
}

TEST_CASE("a manual cutoff below the real one adds over the original", "[audition][cutoff]") {
    const uv::AudioBuffer src = bandLimitedSource();
    uv::PipelineSettings s;
    s.targetRate = 192000.0;
    s.mode = uv::PipelineSettings::Mode::Auto;
    s.hfStrength = 1.0;
    s.ultrasonic = true;
    s.cutoffOverrideHz = 8000.0;
    const uv::AudioBuffer proc = uv::processBuffer(src, s);
    const uv::AudioBuffer plain = uv::resample(src, 192000.0);
    const auto a = slice(proc.channels[0], 72000, 48000);
    const auto b = slice(plain.channels[0], 72000, 48000);
    // The 12 kHz tone sits between the manual and real cutoffs and must survive.
    const double tone = 10.0 * std::log10(power(a, 192000.0, 12000.0) / power(b, 192000.0, 12000.0));
    const uv::AudioBuffer added = uv::addedComponent(proc, src);
    const auto c = slice(added.channels[0], 72000, 48000);
    const double addedBand = bandEnergy(c, 192000.0, 9000.0, 14000.0);
    const double addedLow = bandEnergy(c, 192000.0, 200.0, 7000.0);
    std::printf("[audition] manual cutoff: 12 kHz tone %+.2f dB, added 9-14k over <7k %.1f dB\n", tone,
                10.0 * std::log10(addedBand / std::max(addedLow, 1e-300)));
    REQUIRE(tone > -1.0);
    REQUIRE(addedBand > 100.0 * addedLow);
}

TEST_CASE("pitchShiftDown moves a sine down by octaves", "[audition]") {
    struct Case {
        double f;
        int oct;
    };
    for (Case c : {Case{32000.0, 2}, Case{40000.0, 1}}) {
        const uv::AudioBuffer in = sine(c.f, 192000.0, 1.0);
        const uv::AudioBuffer out = uv::pitchShiftDown(in, c.oct);
        REQUIRE(out.frames() == in.frames());
        REQUIRE(out.sampleRate == in.sampleRate);
        const double expect = c.f / (1 << c.oct);
        const auto seg = slice(out.channels[0], 48000, 96000);
        const double coarse = dominant(seg, 192000.0, 1000.0, 60000.0, 100.0);
        const double fine = dominant(seg, 192000.0, coarse - 100.0, coarse + 100.0, 2.0);
        const double rIn = rms(in.channels[0], 19200, 172800);
        const double rOut = rms(out.channels[0], 19200, 172800);
        const double dbDiff = 20.0 * std::log10(rOut / rIn);
        std::printf("[audition] %g Hz -%d oct -> %g Hz, level %.2f dB\n", c.f, c.oct, fine, dbDiff);
        REQUIRE(std::abs(fine - expect) < expect * 0.01);
        REQUIRE(std::abs(dbDiff) < 2.0);
    }
}

TEST_CASE("pitchShiftDown preserves onset timing", "[audition]") {
    const double rate = 192000.0;
    uv::AudioBuffer in;
    in.sampleRate = rate;
    in.channels.assign(1, std::vector<uv::Sample>(static_cast<std::size_t>(rate * 1.5), 0.0));
    for (std::size_t i = static_cast<std::size_t>(rate * 0.5); i < static_cast<std::size_t>(rate * 1.0); ++i)
        in.channels[0][i] = 0.5 * std::sin(2.0 * kPi * 30000.0 * static_cast<double>(i) / rate);
    const uv::AudioBuffer out = uv::pitchShiftDown(in, 2);
    const auto& y = out.channels[0];
    // Envelope: RMS over 1 ms windows.
    const std::size_t w = 192;
    std::vector<double> env;
    for (std::size_t i = 0; i + w <= y.size(); i += w) env.push_back(rms(y, i, i + w));
    const double mx = *std::max_element(env.begin(), env.end());
    std::size_t first = 0;
    while (env[first] <= 0.5 * mx) ++first;
    const double onset = static_cast<double>(first * w) / rate;
    std::printf("[audition] burst onset at %.4f s\n", onset);
    REQUIRE(std::abs(onset - 0.5) < 0.015);
}

TEST_CASE("pitchShiftDown is deterministic and handles stereo", "[audition]") {
    uv::AudioBuffer in = sine(25000.0, 192000.0, 0.5);
    in.channels.push_back(in.channels[0]);
    const uv::AudioBuffer a = uv::pitchShiftDown(in, 1);
    const uv::AudioBuffer b = uv::pitchShiftDown(in, 1);
    REQUIRE(a.channels == b.channels);
    REQUIRE(a.channels[0] == a.channels[1]);
    const uv::AudioBuffer same = uv::pitchShiftDown(in, 0);
    REQUIRE(same.channels == in.channels);
}

TEST_CASE("pitchShiftDown runtime on a long buffer", "[audition][.slow]") {
    uv::AudioBuffer in = sine(30000.0, 192000.0, 240.0);
    in.channels.push_back(in.channels[0]);
    const auto t0 = std::chrono::steady_clock::now();
    const uv::AudioBuffer out = uv::pitchShiftDown(in, 2);
    const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("[audition] 240 s stereo 192 kHz pitch shift: %.2f s\n", sec);
    REQUIRE(out.frames() == in.frames());
}

TEST_CASE("timeExpand is exact", "[audition]") {
    const uv::AudioBuffer in = sine(30000.0, 192000.0, 0.1);
    const uv::AudioBuffer out = uv::timeExpand(in, 4.0);
    REQUIRE(out.channels == in.channels);
    REQUIRE(out.sampleRate == 48000.0);
    REQUIRE(uv::timeExpand(in, 0.0).sampleRate == 192000.0);
}

TEST_CASE("normalizePeak", "[audition]") {
    uv::AudioBuffer b = sine(1000.0, 48000.0, 0.1, 0.01);
    double peak0 = 0.0;
    for (double x : b.channels[0]) peak0 = std::max(peak0, std::abs(x));
    const double g = uv::normalizePeak(b, -6.0);
    double peak = 0.0;
    for (double x : b.channels[0]) peak = std::max(peak, std::abs(x));
    REQUIRE(std::abs(peak - std::pow(10.0, -6.0 / 20.0)) < 1e-9);
    REQUIRE(std::abs(g - 20.0 * std::log10(peak / peak0)) < 1e-9);

    uv::AudioBuffer silent;
    silent.sampleRate = 48000.0;
    silent.channels.assign(2, std::vector<uv::Sample>(100, 0.0));
    const auto copy = silent.channels;
    REQUIRE(uv::normalizePeak(silent) == 0.0);
    REQUIRE(silent.channels == copy);
}
