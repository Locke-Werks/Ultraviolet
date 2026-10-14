// AudioFile tests. Owned by the AudioFile task.
// Target: bit-exact null on decode->encode->decode round-trip (WAV).
#include <catch2/catch_test_macros.hpp>

#include "uvcore/AudioFile.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace {

// A unique temp path per test so parallel or repeated runs do not collide,
// with an RAII guard that removes it even when an assertion aborts the body.
struct TempWav {
    std::filesystem::path path;
    explicit TempWav(const std::string& tag) {
        path = std::filesystem::temp_directory_path() /
               ("uv_audiofile_" + tag + ".wav");
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
    ~TempWav() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
    std::string str() const { return path.string(); }
};

uv::AudioBuffer makeStereo(double sampleRate, std::size_t frames) {
    uv::AudioBuffer b;
    b.sampleRate = sampleRate;
    b.channels.assign(2, std::vector<uv::Sample>(frames));
    for (std::size_t i = 0; i < frames; ++i) {
        // Channel 0: a sine that stays well inside full scale.
        b.channels[0][i] = 0.5 * std::sin(2.0 * 3.14159265358979 * 440.0 *
                                           static_cast<double>(i) / sampleRate);
        // Channel 1: a different sine, phase/freq offset.
        b.channels[1][i] = 0.3 * std::sin(2.0 * 3.14159265358979 * 660.0 *
                                           static_cast<double>(i) / sampleRate);
    }
    // Seed exact dyadic values that float32 represents without rounding.
    if (frames > 4) {
        b.channels[0][0] = 0.5;
        b.channels[0][1] = -0.25;
        b.channels[0][2] = 0.0;
        b.channels[1][0] = -0.5;
        b.channels[1][1] = 0.125;
    }
    return b;
}

} // namespace

TEST_CASE("Float32 round-trip is bit-exact", "[audiofile]") {
    TempWav tmp("float32");
    const double sr = 48000.0;
    const std::size_t frames = 1024;
    uv::AudioBuffer in = makeStereo(sr, frames);

    std::string err;
    REQUIRE(uv::saveAudio(tmp.str(), in, uv::OutputFormat::Float32, &err));

    auto out = uv::loadAudio(tmp.str(), &err);
    REQUIRE(out.has_value());
    REQUIRE(out->sampleRate == sr);
    REQUIRE(out->channelCount() == 2);
    REQUIRE(out->frames() == frames);

    // A Float32 WAV stores single precision, so bit-exact means the double
    // read back equals the input rounded once to float. The seeded dyadic
    // values (0.5, -0.25, 0.125) survive that rounding unchanged.
    for (int c = 0; c < 2; ++c) {
        for (std::size_t i = 0; i < frames; ++i) {
            const double expected =
                static_cast<double>(static_cast<float>(in.channels[c][i]));
            REQUIRE(out->channels[c][i] == expected);
        }
    }
}

TEST_CASE("Pcm24 round-trip is within one LSB", "[audiofile]") {
    TempWav tmp("pcm24");
    const double sr = 44100.0;
    const std::size_t frames = 1024;
    uv::AudioBuffer in = makeStereo(sr, frames);

    std::string err;
    REQUIRE(uv::saveAudio(tmp.str(), in, uv::OutputFormat::Pcm24, &err));

    auto out = uv::loadAudio(tmp.str(), &err);
    REQUIRE(out.has_value());
    REQUIRE(out->sampleRate == sr);
    REQUIRE(out->sourceBitDepth == 24);

    // One 24-bit LSB over the normalized [-1, 1] range.
    const double lsb = 1.0 / static_cast<double>(1 << 23);
    for (int c = 0; c < 2; ++c) {
        for (std::size_t i = 0; i < frames; ++i) {
            REQUIRE(std::abs(out->channels[c][i] - in.channels[c][i]) <= lsb);
        }
    }
}

TEST_CASE("Channel and frame counts are preserved", "[audiofile]") {
    TempWav tmp("counts");
    const std::size_t frames = 777;
    uv::AudioBuffer in = makeStereo(32000.0, frames);

    std::string err;
    REQUIRE(uv::saveAudio(tmp.str(), in, uv::OutputFormat::Float32, &err));

    auto out = uv::loadAudio(tmp.str(), &err);
    REQUIRE(out.has_value());
    REQUIRE(out->channelCount() == in.channelCount());
    REQUIRE(out->frames() == in.frames());
}

TEST_CASE("Float64 at 384 kHz round-trips bit-exact", "[audiofile]") {
    TempWav tmp("float64_384k");
    const double sr = 384000.0;
    uv::AudioBuffer in = makeStereo(sr, 4096);
    // Values float32 cannot hold: only a true 64-bit write returns them intact.
    in.channels[0][3] = 0.1;
    in.channels[1][3] = 1.0 / 3.0;

    std::string err;
    REQUIRE(uv::saveAudio(tmp.str(), in, uv::OutputFormat::Float64, &err));
    auto out = uv::loadAudio(tmp.str(), &err);
    REQUIRE(out.has_value());
    REQUIRE(out->sampleRate == sr);
    REQUIRE(out->sourceBitDepth == 64);
    for (int c = 0; c < 2; ++c) {
        for (std::size_t i = 0; i < in.frames(); ++i) {
            REQUIRE(out->channels[c][i] == in.channels[c][i]);
        }
    }
}

TEST_CASE("Small files are written as plain RIFF WAV", "[audiofile]") {
    // RF64 is only needed past 4 GB; everything smaller must stay a
    // standard WAV that any player opens.
    TempWav tmp("riff_header");
    std::string err;
    REQUIRE(uv::saveAudio(tmp.str(), makeStereo(48000.0, 1000), uv::OutputFormat::Float64, &err));
    std::FILE* f = std::fopen(tmp.str().c_str(), "rb");
    REQUIRE(f != nullptr);
    char magic[4] = {};
    const std::size_t got = std::fread(magic, 1, 4, f);
    std::fclose(f);
    REQUIRE(got == 4);
    REQUIRE(std::string(magic, 4) == "RIFF");
}

TEST_CASE("Nonexistent path fails and sets err", "[audiofile]") {
    const std::string missing =
        (std::filesystem::temp_directory_path() / "uv_no_such_dir_xyz" /
         "missing.wav").string();
    std::string err = "untouched";
    auto out = uv::loadAudio(missing, &err);
    REQUIRE_FALSE(out.has_value());
    REQUIRE(err != "untouched");
    REQUIRE_FALSE(err.empty());
}
