#include <catch2/catch_test_macros.hpp>

#include "uvcore/Selection.h"

#include <cmath>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;

uv::AudioBuffer sine(double freq, double rate, double seconds, double amp = 0.5) {
    uv::AudioBuffer b;
    b.sampleRate = rate;
    b.channels.assign(2, std::vector<uv::Sample>(static_cast<std::size_t>(rate * seconds)));
    for (auto& c : b.channels)
        for (std::size_t i = 0; i < c.size(); ++i)
            c[i] = amp * std::sin(2.0 * kPi * freq * static_cast<double>(i) / rate);
    return b;
}

// RMS over the middle half, away from any edge transient.
double midRms(const std::vector<uv::Sample>& x) {
    const std::size_t a = x.size() / 4, b = x.size() * 3 / 4;
    double acc = 0.0;
    for (std::size_t i = a; i < b; ++i) acc += x[i] * x[i];
    return std::sqrt(acc / static_cast<double>(b - a));
}

double db(double r) { return 20.0 * std::log10(r); }

} // namespace

TEST_CASE("extractSelection passes a tone inside the band", "[selection]") {
    const auto in = sine(3000.0, 48000.0, 2.0);
    uv::Selection s;
    s.hasFreq = true;
    s.f0 = 2000.0;
    s.f1 = 5000.0;
    const auto out = uv::extractSelection(in, s);
    REQUIRE(out.frames() == in.frames());
    for (int ch = 0; ch < 2; ++ch)
        CHECK(std::abs(db(midRms(out.channels[ch]) / midRms(in.channels[ch]))) < 0.5);
}

TEST_CASE("extractSelection attenuates a tone outside the band", "[selection]") {
    const auto in = sine(500.0, 44100.0, 2.0);
    uv::Selection s;
    s.hasFreq = true;
    s.f0 = 2000.0;
    s.f1 = 5000.0;
    const auto out = uv::extractSelection(in, s);
    CHECK(db(midRms(out.channels[0]) / midRms(in.channels[0])) < -60.0);

    const auto hi = sine(12000.0, 96000.0, 2.0);
    const auto outHi = uv::extractSelection(hi, s);
    CHECK(db(midRms(outHi.channels[0]) / midRms(hi.channels[0])) < -60.0);
}

TEST_CASE("extractSelection cuts the time span to the right length", "[selection]") {
    const auto in = sine(1000.0, 48000.0, 3.0);
    uv::Selection s;
    s.hasTime = true;
    s.t0 = 0.5;
    s.t1 = 1.75;
    const auto out = uv::extractSelection(in, s);
    CHECK(out.frames() == 60000);
    CHECK(out.channelCount() == 2);
    // Fade-in starts from silence.
    CHECK(std::abs(out.channels[0][0]) < 0.01);

    // Box: same length, band-limited.
    s.hasFreq = true;
    s.f0 = 4000.0;
    s.f1 = 8000.0;
    const auto box = uv::extractSelection(in, s);
    CHECK(box.frames() == 60000);
    CHECK(db(midRms(box.channels[0]) / midRms(in.channels[0])) < -60.0);
}
