#include <catch2/catch_test_macros.hpp>

#include "uvcore/Types.h"

TEST_CASE("AudioBuffer basic accounting", "[smoke]") {
    uv::AudioBuffer buf;
    buf.channels.resize(2);
    buf.channels[0].assign(100, 0.0);
    buf.channels[1].assign(100, 0.0);
    buf.sampleRate = 48000.0;

    REQUIRE(buf.channelCount() == 2);
    REQUIRE(buf.frames() == 100);
    REQUIRE_FALSE(buf.empty());
}
