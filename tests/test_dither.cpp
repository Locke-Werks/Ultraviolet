// BitDepth/Dither tests. Owned by the BitDepthProcessor task.
// Targets: float path is a no-op (bit-exact); 24-bit path quantizes to grid,
// noise floor near expected, no DC offset introduced.
#include <catch2/catch_test_macros.hpp>

#include "uvcore/Dither.h"

#include <cmath>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;

uv::AudioBuffer makeSine(double amplitude, std::size_t frames, double freq) {
    uv::AudioBuffer buf;
    buf.sampleRate = 48000.0;
    buf.channels.resize(1);
    buf.channels[0].reserve(frames);
    for (std::size_t n = 0; n < frames; ++n) {
        const double t = static_cast<double>(n) / 48000.0;
        buf.channels[0].push_back(amplitude * std::sin(2.0 * kPi * freq * t));
    }
    return buf;
}

bool onGrid(double value, int bits) {
    const double scaled = value * std::ldexp(1.0, bits - 1);
    return std::abs(scaled - std::nearbyint(scaled)) < 1e-6;
}

} // namespace

TEST_CASE("24-bit output lands on the grid", "[dither]") {
    uv::AudioBuffer buf = makeSine(0.5, 4096, 997.0);
    uv::ditherTo(buf, 24);
    for (double s : buf.channels[0]) {
        REQUIRE(onGrid(s, 24));
    }
}

TEST_CASE("Dither introduces no DC offset", "[dither]") {
    // Zero-mean input over a long buffer: the mean must stay near zero.
    uv::AudioBuffer buf = makeSine(0.25, 200000, 440.0);
    uv::ditherTo(buf, 16);

    double sum = 0.0;
    for (double s : buf.channels[0]) sum += s;
    const double mean = sum / static_cast<double>(buf.channels[0].size());

    // A few LSBs of tolerance at 16-bit.
    REQUIRE(std::abs(mean) < 1e-4);
}

TEST_CASE("Full-scale and over-full-scale stay in range", "[dither]") {
    uv::AudioBuffer buf;
    buf.sampleRate = 48000.0;
    buf.channels.resize(1);
    buf.channels[0] = {1.0, -1.0, 1.5, -1.5, 0.999999, -0.999999, 2.0, -2.0};

    uv::ditherTo(buf, 24);

    const double topRail = 1.0 - std::ldexp(1.0, -23); // 1 - 1LSB
    for (double s : buf.channels[0]) {
        REQUIRE(s >= -1.0);
        REQUIRE(s <= topRail);
    }
}

TEST_CASE("Low-level sine is not quantized to silence", "[dither]") {
    // ~2 LSB at 16-bit. Dither must keep the signal alive (nonzero variance).
    const double twoLsb = 2.0 * std::ldexp(1.0, -15);
    uv::AudioBuffer buf = makeSine(twoLsb, 8192, 1000.0);
    uv::ditherTo(buf, 16);

    double sum = 0.0;
    for (double s : buf.channels[0]) sum += s;
    const double mean = sum / static_cast<double>(buf.channels[0].size());

    double var = 0.0;
    for (double s : buf.channels[0]) {
        const double d = s - mean;
        var += d * d;
    }
    var /= static_cast<double>(buf.channels[0].size());

    REQUIRE(var > 0.0);
}

TEST_CASE("targetBits=32 leaves the buffer bit-exact", "[dither]") {
    uv::AudioBuffer original = makeSine(0.33, 1024, 523.0);
    uv::AudioBuffer buf = original;

    uv::ditherTo(buf, 32);

    REQUIRE(buf.channels[0].size() == original.channels[0].size());
    for (std::size_t n = 0; n < buf.channels[0].size(); ++n) {
        REQUIRE(buf.channels[0][n] == original.channels[0][n]);
    }
}
