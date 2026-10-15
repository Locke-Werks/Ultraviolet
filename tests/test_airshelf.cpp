#include <catch2/catch_test_macros.hpp>

#include "uvcore/AirShelf.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;

double toneGainDb(double f, double topDb) {
    const double sr = 192000.0;
    uv::AudioBuffer b;
    b.sampleRate = sr;
    b.channels.assign(1, std::vector<uv::Sample>(static_cast<std::size_t>(sr)));
    for (std::size_t i = 0; i < b.channels[0].size(); ++i) b.channels[0][i] = 0.25 * std::sin(2.0 * kPi * f * i / sr);
    const auto in = b.channels[0];
    uv::applyAirShelf(b, topDb);
    // Middle half only, away from the edges.
    double ei = 0, eo = 0;
    for (std::size_t i = 48000; i < 144000; ++i) {
        ei += in[i] * in[i];
        eo += b.channels[0][i] * b.channels[0][i];
    }
    return 10.0 * std::log10(eo / ei);
}

} // namespace

TEST_CASE("air shelf is flat low, full gain high, off at zero", "[air]") {
    REQUIRE(uv::airShelfGainDb(1000.0, 6.5) == 0.0);
    REQUIRE(uv::airShelfGainDb(30000.0, 6.5) == 6.5);
    REQUIRE(uv::airShelfGainDb(15000.0, 0.0) == 0.0);
    const double lo = toneGainDb(1000.0, 6.5);
    const double mid = toneGainDb(9000.0, 6.5);
    const double hi = toneGainDb(20000.0, 6.5);
    const double ultra = toneGainDb(30000.0, 6.5);
    std::printf("[air] 1k %+.3f, 9k %+.3f (expect %+.3f), 20k %+.3f, 30k %+.3f dB\n", lo, mid,
                uv::airShelfGainDb(9000.0, 6.5), hi, ultra);
    REQUIRE(std::abs(lo) < 0.01);
    REQUIRE(std::abs(mid - uv::airShelfGainDb(9000.0, 6.5)) < 0.1);
    REQUIRE(std::abs(hi - 6.5) < 0.05);
    REQUIRE(std::abs(ultra - 6.5) < 0.05);
    REQUIRE(std::abs(toneGainDb(15000.0, 0.0)) < 1e-12);
}
