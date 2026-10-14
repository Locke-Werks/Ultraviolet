// SpectralAnalyzer tests. Owned by the SpectralAnalyzer task.
// Synthesize band-limited signals at 48 kHz and check that the analyzer finds
// where real content ends and classifies full-band vs rolled-off.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "uvcore/SpectralAnalyzer.h"

#include <cmath>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kSampleRate = 48000.0;
constexpr std::size_t kDurationFrames = 48000; // 1 second

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
    // Normalize so a tone count does not blow past full scale.
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

} // namespace

TEST_CASE("Band-limited content at 16 kHz is detected as rolled off", "[spectral]") {
    auto buf = makeTones(tonesEvery500Up(16000.0));
    uv::SpectrumInfo info = uv::analyzeSpectrum(buf);

    REQUIRE(info.cutoffHz > 15000.0);
    REQUIRE(info.cutoffHz < 17000.0);
    REQUIRE(info.rolledOff == true);
}

TEST_CASE("Content up to near Nyquist is full band", "[spectral]") {
    auto buf = makeTones(tonesEvery500Up(23000.0));
    uv::SpectrumInfo info = uv::analyzeSpectrum(buf);

    REQUIRE(info.cutoffHz > 22000.0);
    REQUIRE(info.rolledOff == false);
}

TEST_CASE("A pure 1 kHz tone does not crash and cuts above 1 kHz", "[spectral]") {
    auto buf = makeTones({1000.0});
    uv::SpectrumInfo info = uv::analyzeSpectrum(buf);

    REQUIRE(info.cutoffHz > 1000.0);
}
