// Resampler tests. Owned by the Resampler task.
// Verify arbitrary-ratio resampling via r8brain: correct output length and
// sample rate, preserved peak/DC, and a preserved dominant frequency.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "uvcore/Resampler.h"

#include <cmath>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;

uv::AudioBuffer makeSine(double sampleRate, std::size_t frames, double freq,
                         double amplitude = 1.0, int channels = 1) {
    uv::AudioBuffer buf;
    buf.sampleRate = sampleRate;
    buf.sourceBitDepth = 24;
    buf.channels.assign(static_cast<std::size_t>(channels),
                        std::vector<uv::Sample>(frames));
    for (int ch = 0; ch < channels; ++ch) {
        for (std::size_t n = 0; n < frames; ++n) {
            buf.channels[ch][n] =
                amplitude * std::sin(2.0 * kPi * freq *
                                     (static_cast<double>(n) / sampleRate));
        }
    }
    return buf;
}

// Goertzel magnitude of one frequency bin over an interior span of the signal.
double goertzelMag(const std::vector<uv::Sample>& x, double sampleRate,
                   double freq) {
    // Skip filter edges; measure the stable interior only.
    const std::size_t start = x.size() / 8;
    const std::size_t end = x.size() - x.size() / 8;
    const double w = 2.0 * kPi * freq / sampleRate;
    const double coeff = 2.0 * std::cos(w);
    double s0 = 0.0, s1 = 0.0, s2 = 0.0;
    for (std::size_t n = start; n < end; ++n) {
        s0 = x[n] + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return std::sqrt(s1 * s1 + s2 * s2 - coeff * s1 * s2);
}

double peak(const std::vector<uv::Sample>& x) {
    double p = 0.0;
    // Trim a few edge samples where the filter transient lives.
    for (std::size_t n = 16; n + 16 < x.size(); ++n) {
        p = std::max(p, std::abs(x[n]));
    }
    return p;
}

} // namespace

TEST_CASE("48k->192k preserves a 1 kHz sine", "[resampler]") {
    const std::size_t inFrames = 48000;
    uv::AudioBuffer in = makeSine(48000.0, inFrames, 1000.0, 1.0);
    uv::AudioBuffer out = uv::resample(in, 192000.0);

    REQUIRE(out.sampleRate == 192000.0);
    REQUIRE(out.channelCount() == 1);

    // Length is ~4x the input, within a few samples.
    const double expected =
        static_cast<double>(inFrames) * 192000.0 / 48000.0;
    REQUIRE(std::abs(static_cast<double>(out.frames()) - expected) <= 4.0);

    // Peak preserved within ~0.5 dB.
    const double inPeak = peak(in.channels[0]);
    const double outPeak = peak(out.channels[0]);
    const double db = 20.0 * std::log10(outPeak / inPeak);
    REQUIRE(std::abs(db) <= 0.5);

    // Dominant frequency is still ~1 kHz: the 1 kHz bin dwarfs a decoy at 5 kHz.
    const double sig = goertzelMag(out.channels[0], 192000.0, 1000.0);
    const double decoy = goertzelMag(out.channels[0], 192000.0, 5000.0);
    REQUIRE(sig > 100.0 * decoy);
}

TEST_CASE("44.1k->192k has correct length and rate", "[resampler]") {
    const std::size_t inFrames = 44100;
    uv::AudioBuffer in = makeSine(44100.0, inFrames, 1000.0, 1.0);
    uv::AudioBuffer out = uv::resample(in, 192000.0);

    REQUIRE(out.sampleRate == 192000.0);

    const double expected =
        static_cast<double>(inFrames) * 192000.0 / 44100.0;
    REQUIRE(std::abs(static_cast<double>(out.frames()) - expected) <= 4.0);

    // Peak still intact through a non-integer ratio.
    const double inPeak = peak(in.channels[0]);
    const double outPeak = peak(out.channels[0]);
    REQUIRE(std::abs(20.0 * std::log10(outPeak / inPeak)) <= 0.5);
}

TEST_CASE("DC level is preserved", "[resampler]") {
    const std::size_t inFrames = 20000;
    uv::AudioBuffer in;
    in.sampleRate = 48000.0;
    in.sourceBitDepth = 24;
    in.channels.assign(1, std::vector<uv::Sample>(inFrames, 0.5));

    uv::AudioBuffer out = uv::resample(in, 96000.0);

    // Interior DC stays ~0.5 with no gross gain error or ripple.
    const std::size_t start = out.frames() / 4;
    const std::size_t end = out.frames() - out.frames() / 4;
    double minV = 1e9, maxV = -1e9;
    for (std::size_t n = start; n < end; ++n) {
        minV = std::min(minV, out.channels[0][n]);
        maxV = std::max(maxV, out.channels[0][n]);
    }
    REQUIRE_THAT(minV, Catch::Matchers::WithinAbs(0.5, 0.01));
    REQUIRE_THAT(maxV, Catch::Matchers::WithinAbs(0.5, 0.01));
}

TEST_CASE("Equal rates return a bit-exact copy", "[resampler]") {
    uv::AudioBuffer in = makeSine(48000.0, 1000, 1000.0, 0.75, 2);
    uv::AudioBuffer out = uv::resample(in, 48000.0);

    REQUIRE(out.sampleRate == 48000.0);
    REQUIRE(out.channelCount() == 2);
    REQUIRE(out.frames() == in.frames());
    for (int ch = 0; ch < 2; ++ch) {
        for (std::size_t n = 0; n < in.frames(); ++n) {
            REQUIRE(out.channels[ch][n] == in.channels[ch][n]);
        }
    }
}
