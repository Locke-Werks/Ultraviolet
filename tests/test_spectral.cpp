// SpectralAnalyzer tests. Owned by the SpectralAnalyzer task.
// Music-like material is shaped white noise with a natural downward slope and
// heavy bass, so high frequencies sit 50-70 dB under the spectral peak. That
// is what broke the old peak-relative detector, which called the slope a
// cutoff at 3 kHz.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "uvcore/SpectralAnalyzer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <random>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kSampleRate = 48000.0;
constexpr std::size_t kDurationFrames = 48000; // 1 second
constexpr std::size_t kNoiseFrames = std::size_t{1} << 18; // about 5.5 s at 48 kHz

using cplx = std::complex<double>;

// In-place iterative radix-2 FFT. inverse=true omits the 1/N scale, which the
// caller's peak normalization makes irrelevant.
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
}

struct LowPass {
    double startHz = 0.0; // 0 disables; the skirt begins exactly here
    double widthHz = 400.0;
    double depthDb = 75.0;
};

// Gain in dB of the synthetic "music" spectrum: -6 dB/oct above 200 Hz plus a
// +30 dB bass shelf, which puts 15 kHz roughly 65 dB under the bass peak. Then
// an optional low-pass skirt that is linear in dB.
double musicGainDb(double f, const LowPass& lp) {
    if (f < 20.0) return -200.0;
    double db = -6.0 * std::log2(std::max(f, 200.0) / 1000.0) +
                30.0 / (1.0 + (f / 150.0) * (f / 150.0));
    if (lp.startHz > 0.0 && f > lp.startHz) {
        const double x = (f - lp.startHz) / lp.widthHz;
        db -= x < 1.0 ? lp.depthDb * x : lp.depthDb;
    }
    return db;
}

// Shaped noise by FFT-filtering white Gaussian noise, then a white floor at
// about -130 dBFS so the region above a cliff looks like a real encoder's.
uv::AudioBuffer makeMusic(double sampleRate, const LowPass& lp, unsigned seed = 1234) {
    std::mt19937 rng(seed);
    std::normal_distribution<double> nd(0.0, 1.0);
    const std::size_t n = kNoiseFrames;
    std::vector<cplx> x(n);
    for (auto& v : x) v = cplx(nd(rng), 0.0);
    fft(x, false);
    for (std::size_t k = 0; k <= n / 2; ++k) {
        const double f = static_cast<double>(k) * sampleRate / static_cast<double>(n);
        const double g = std::pow(10.0, musicGainDb(f, lp) / 20.0);
        x[k] *= g;
        if (k != 0 && k != n / 2) x[n - k] *= g;
    }
    fft(x, true);

    uv::AudioBuffer buf;
    buf.sampleRate = sampleRate;
    buf.channels.resize(1);
    auto& ch = buf.channels[0];
    ch.resize(n);
    double peak = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        ch[i] = x[i].real();
        peak = std::max(peak, std::abs(ch[i]));
    }
    const double s = peak > 0.0 ? 0.5 / peak : 1.0;
    std::normal_distribution<double> floorNoise(0.0, 3e-7);
    for (auto& v : ch) v = v * s + floorNoise(rng);
    return buf;
}

// A mono buffer of summed sine tones at the given frequencies.
uv::AudioBuffer makeTones(const std::vector<double>& freqs) {
    uv::AudioBuffer buf;
    buf.sampleRate = kSampleRate;
    buf.channels.resize(1);
    auto& ch = buf.channels[0];
    ch.assign(kDurationFrames, 0.0);
    for (double f : freqs) {
        for (std::size_t i = 0; i < kDurationFrames; ++i) {
            ch[i] += std::sin(2.0 * kPi * f * static_cast<double>(i) / kSampleRate);
        }
    }
    const double scale = freqs.empty() ? 1.0 : 1.0 / static_cast<double>(freqs.size());
    for (auto& s : ch) s *= scale;
    return buf;
}

std::vector<double> tonesEvery500Up(double topHz) {
    std::vector<double> freqs;
    for (double f = 500.0; f <= topHz + 1.0; f += 500.0) {
        freqs.push_back(f);
    }
    return freqs;
}

void report(const char* name, const uv::SpectrumInfo& info, double trueHz) {
    std::printf("[spectral] %-28s cutoff %8.1f Hz (true %8.1f, err %+5.2f%%) rolledOff %d "
                "slope %6.2f dB/oct floor %7.1f dB\n",
                name, info.cutoffHz, trueHz, 100.0 * (info.cutoffHz - trueHz) / trueHz,
                info.rolledOff ? 1 : 0, info.slopeDbPerOct, info.noiseFloorDb);
}

} // namespace

TEST_CASE("Full-band music with a natural slope is not mistaken for a cutoff", "[spectral]") {
    // Regression: a peak-relative threshold reported 3.1 kHz on material like this.
    auto buf = makeMusic(kSampleRate, {});
    const auto info = uv::analyzeSpectrum(buf);
    report("full band 48k", info, kSampleRate * 0.5);

    REQUIRE(info.cutoffHz >= 0.98 * kSampleRate * 0.5);
    REQUIRE(info.rolledOff == false);
    REQUIRE(info.slopeDbPerOct < -3.0);
    REQUIRE(info.slopeDbPerOct > -9.0);
}

TEST_CASE("Encoder low-pass at 16 kHz is located at its shoulder", "[spectral]") {
    auto buf = makeMusic(kSampleRate, {16000.0, 400.0, 75.0});
    const auto info = uv::analyzeSpectrum(buf);
    report("lowpass 16k", info, 16000.0);

    REQUIRE(std::abs(info.cutoffHz - 16000.0) <= 0.025 * 16000.0);
    REQUIRE(info.rolledOff == true);
    REQUIRE(info.slopeDbPerOct < -3.0);
    REQUIRE(info.slopeDbPerOct > -9.0);
}

TEST_CASE("Encoder low-pass at 20 kHz is found near 20 kHz", "[spectral]") {
    auto buf = makeMusic(kSampleRate, {20000.0, 500.0, 70.0});
    const auto info = uv::analyzeSpectrum(buf);
    report("lowpass 20k", info, 20000.0);

    REQUIRE(info.cutoffHz > 15000.0);
    REQUIRE(std::abs(info.cutoffHz - 20000.0) <= 0.025 * 20000.0);
    REQUIRE(info.rolledOff == true);
}

TEST_CASE("Anti-alias cliff on a 44.1 kHz file is detected", "[spectral]") {
    auto buf = makeMusic(44100.0, {20500.0, 800.0, 90.0});
    const auto info = uv::analyzeSpectrum(buf);
    report("anti-alias 20.5k @44.1k", info, 20500.0);

    REQUIRE(std::abs(info.cutoffHz - 20500.0) <= 0.025 * 20500.0);
    REQUIRE(info.rolledOff == true);
}

TEST_CASE("Low-bitrate low-pass at 11 kHz is detected", "[spectral]") {
    auto buf = makeMusic(kSampleRate, {11000.0, 300.0, 70.0});
    const auto info = uv::analyzeSpectrum(buf);
    report("lowpass 11k", info, 11000.0);

    REQUIRE(std::abs(info.cutoffHz - 11000.0) <= 0.025 * 11000.0);
    REQUIRE(info.rolledOff == true);
    REQUIRE(info.noiseFloorDb < -100.0);
}

TEST_CASE("Band-limited tones to 16 kHz are detected as rolled off", "[spectral]") {
    auto buf = makeTones(tonesEvery500Up(16000.0));
    const auto info = uv::analyzeSpectrum(buf);
    report("tones to 16k", info, 16000.0);

    REQUIRE(info.cutoffHz > 15000.0);
    REQUIRE(info.cutoffHz < 17000.0);
    REQUIRE(info.rolledOff == true);
}

TEST_CASE("Tones up to 23 kHz put the cutoff near Nyquist", "[spectral]") {
    // rolledOff is not asserted: 23 kHz is below 0.98 * Nyquist, and any real
    // edge below that counts as a low-pass the product should extend.
    auto buf = makeTones(tonesEvery500Up(23000.0));
    const auto info = uv::analyzeSpectrum(buf);
    report("tones to 23k", info, 23000.0);

    REQUIRE(info.cutoffHz > 22000.0);
}

TEST_CASE("A pure 1 kHz tone does not crash and cuts above 1 kHz", "[spectral]") {
    auto buf = makeTones({1000.0});
    const auto info = uv::analyzeSpectrum(buf);
    report("pure 1k tone", info, 1000.0);

    REQUIRE(info.cutoffHz > 1000.0);
}

TEST_CASE("Silence and very short input are handled", "[spectral]") {
    uv::AudioBuffer silent;
    silent.sampleRate = kSampleRate;
    silent.channels.assign(2, std::vector<double>(kDurationFrames, 0.0));
    const auto a = uv::analyzeSpectrum(silent);
    REQUIRE(a.cutoffHz == kSampleRate * 0.5);
    REQUIRE(a.rolledOff == false);
    REQUIRE(std::isfinite(a.noiseFloorDb));
    REQUIRE(std::isfinite(a.slopeDbPerOct));

    uv::AudioBuffer tiny;
    tiny.sampleRate = kSampleRate;
    tiny.channels.assign(1, std::vector<double>{0.5, -0.25, 0.1});
    const auto b = uv::analyzeSpectrum(tiny);
    REQUIRE(std::isfinite(b.cutoffHz));
    REQUIRE(b.cutoffHz > 0.0);
    REQUIRE(b.cutoffHz <= kSampleRate * 0.5);

    uv::AudioBuffer empty;
    empty.sampleRate = kSampleRate;
    const auto c = uv::analyzeSpectrum(empty);
    REQUIRE(c.rolledOff == false);
}

TEST_CASE("Four minutes of stereo 48 kHz analyzes in under a second", "[spectral]") {
    const std::size_t frames = static_cast<std::size_t>(240.0 * kSampleRate);
    uv::AudioBuffer buf;
    buf.sampleRate = kSampleRate;
    buf.channels.assign(2, std::vector<double>(frames));
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> ud(-0.5, 0.5);
    for (auto& ch : buf.channels) {
        for (auto& v : ch) v = ud(rng);
    }
    const auto t0 = std::chrono::steady_clock::now();
    const auto info = uv::analyzeSpectrum(buf);
    const double secs =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("[spectral] 4 min stereo analyzed in %.3f s\n", secs);

    REQUIRE(info.rolledOff == false);
#ifdef NDEBUG
    REQUIRE(secs < 1.0);
#endif
}
