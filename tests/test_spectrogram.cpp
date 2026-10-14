// Spectrogram tests. Owned by the spectrogram task.
#include <catch2/catch_test_macros.hpp>

#include "uvcore/SpectralAnalyzer.h"

#include <chrono>
#include <cmath>
#include <cstdio>

namespace {

constexpr double kPi = 3.14159265358979323846;

int rowForHz(const uv::Spectrogram& s, double hz) {
    const double t = std::log(hz / s.minHz) / std::log(s.maxHz / s.minHz);
    return static_cast<int>(t * s.rows);
}

int brightestRow(const uv::Spectrogram& s, int col) {
    int best = 0;
    for (int r = 1; r < s.rows; ++r) {
        if (s.db[r * s.cols + col] > s.db[best * s.cols + col]) best = r;
    }
    return best;
}

} // namespace

TEST_CASE("Spectrogram tracks a tone change over time", "[spectrogram]") {
    const double sr = 48000.0;
    const std::size_t n = static_cast<std::size_t>(sr * 4);
    uv::AudioBuffer buf;
    buf.sampleRate = sr;
    buf.channels.assign(1, std::vector<double>(n));
    for (std::size_t i = 0; i < n; ++i) {
        const double f = i < n / 2 ? 1000.0 : 10000.0;
        buf.channels[0][i] = std::sin(2.0 * kPi * f * i / sr);
    }

    const auto s = uv::computeSpectrogram(buf, 64, 200);
    REQUIRE(s.cols == 64);
    REQUIRE(s.rows == 200);
    REQUIRE(s.db.size() == 64u * 200u);
    REQUIRE(s.maxHz == 24000.0);

    const int early = brightestRow(s, 5);
    const int late = brightestRow(s, 58);
    CHECK(std::abs(early - rowForHz(s, 1000.0)) <= 2);
    CHECK(std::abs(late - rowForHz(s, 10000.0)) <= 2);
    CHECK(std::abs(s.db[early * s.cols + 5]) < 2.0f);
    CHECK(std::abs(s.db[late * s.cols + 58]) < 2.0f);
}

TEST_CASE("Spectrogram shows a hard low-pass edge", "[spectrogram]") {
    const double sr = 48000.0;
    const std::size_t n = static_cast<std::size_t>(sr * 2);
    uv::AudioBuffer buf;
    buf.sampleRate = sr;
    buf.channels.assign(2, std::vector<double>(n, 0.0));
    for (double f = 500.0; f <= 16000.0; f += 500.0) {
        for (std::size_t i = 0; i < n; ++i) {
            const double v = 0.02 * std::sin(2.0 * kPi * f * i / sr + f);
            buf.channels[0][i] += v;
            buf.channels[1][i] += v;
        }
    }

    const auto s = uv::computeSpectrogram(buf, 32, 240);
    REQUIRE(s.cols > 0);
    double below = 0.0, above = 0.0;
    int nb = 0, na = 0;
    const int r1k = rowForHz(s, 1000.0), r15k = rowForHz(s, 15000.0), r17k = rowForHz(s, 17500.0);
    for (int c = 0; c < s.cols; ++c) {
        for (int r = r1k; r < r15k; ++r) { below += s.db[r * s.cols + c]; ++nb; }
        for (int r = r17k; r < s.rows; ++r) { above += s.db[r * s.cols + c]; ++na; }
    }
    below /= nb;
    above /= na;
    CHECK(below - above >= 60.0);
}

TEST_CASE("Spectrogram at 192 kHz reaches 96 kHz", "[spectrogram]") {
    const double sr = 192000.0;
    const std::size_t n = static_cast<std::size_t>(sr * 3);
    uv::AudioBuffer buf;
    buf.sampleRate = sr;
    buf.channels.assign(2, std::vector<double>(n));
    for (std::size_t i = 0; i < n; ++i) {
        buf.channels[0][i] = 0.5 * std::sin(2.0 * kPi * 40000.0 * i / sr);
        buf.channels[1][i] = buf.channels[0][i];
    }
    const auto t0 = std::chrono::steady_clock::now();
    const auto s = uv::computeSpectrogram(buf);
    const double ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();
    std::printf("spectrogram 192k 3s stereo: %.1f ms\n", ms);
    CHECK(s.maxHz == 96000.0);
    CHECK(s.cols > 0);
    CHECK(s.rows == 480);
    CHECK(s.durationSeconds == 3.0);
    CHECK(std::abs(brightestRow(s, s.cols / 2) - rowForHz(s, 40000.0)) <= 2);
}

TEST_CASE("Spectrogram of empty input is empty", "[spectrogram]") {
    uv::AudioBuffer buf;
    buf.sampleRate = 44100.0;
    const auto s = uv::computeSpectrogram(buf);
    CHECK(s.cols == 0);
    CHECK(s.rows == 0);
    CHECK(s.db.empty());
    CHECK(s.sampleRate == 44100.0);
}
