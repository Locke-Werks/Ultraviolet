// In-memory processing and display-spectrum tests.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "uvcore/AudioFile.h"
#include "uvcore/Pipeline.h"
#include "uvcore/SpectralAnalyzer.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>

namespace {

constexpr double kPi = 3.14159265358979323846;

struct TempFile {
    std::filesystem::path path;
    explicit TempFile(const std::string& name) {
        path = std::filesystem::temp_directory_path() / name;
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
    ~TempFile() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
    std::string str() const { return path.string(); }
};

uv::AudioBuffer makeSine(double rate, double freq, double amp, std::size_t frames,
                         int channels) {
    uv::AudioBuffer b;
    b.sampleRate = rate;
    b.channels.assign(channels, std::vector<double>(frames));
    for (int ch = 0; ch < channels; ++ch) {
        for (std::size_t i = 0; i < frames; ++i) {
            b.channels[ch][i] = amp * std::sin(2.0 * kPi * freq * i / rate);
        }
    }
    return b;
}

struct CancelAlways : uv::ProgressSink {
    bool cancelled() const override { return true; }
};

struct Recorder : uv::ProgressSink {
    std::vector<double> values;
    void onProgress(double f) override { values.push_back(f); }
};

} // namespace

TEST_CASE("processBuffer 48k to 192k Mastering matches runPipeline output", "[processing]") {
    // Stored as float32 so the comparison is not polluted by input quantization.
    const uv::AudioBuffer src = makeSine(48000.0, 1000.0, 0.5, 48000 / 4, 2);
    TempFile in("uv_processing_in.wav");
    TempFile out("uv_processing_out.wav");
    REQUIRE(uv::saveAudio(in.str(), src, uv::OutputFormat::Float32));
    auto reloaded = uv::loadAudio(in.str());
    REQUIRE(reloaded);

    uv::PipelineSettings s;
    s.targetRate = 192000.0;
    s.outputFormat = uv::OutputFormat::Float32;
    s.mode = uv::PipelineSettings::Mode::Mastering;

    Recorder rec;
    const uv::AudioBuffer mem = uv::processBuffer(*reloaded, s, &rec);
    REQUIRE(mem.sampleRate == 192000.0);
    REQUIRE(mem.channelCount() == 2);
    const double ratio = static_cast<double>(mem.frames()) / src.frames();
    REQUIRE(ratio == Catch::Approx(4.0).margin(0.01));
    for (const auto& ch : mem.channels)
        for (double v : ch) REQUIRE(std::isfinite(v));
    REQUIRE(!rec.values.empty());
    REQUIRE(std::is_sorted(rec.values.begin(), rec.values.end()));
    REQUIRE(rec.values.back() == 1.0);

    std::string err;
    REQUIRE(uv::runPipeline(in.str(), out.str(), s, nullptr, &err));
    auto file = uv::loadAudio(out.str());
    REQUIRE(file);
    REQUIRE(file->frames() == mem.frames());
    REQUIRE(file->channelCount() == mem.channelCount());
    double maxDiff = 0.0;
    for (int ch = 0; ch < mem.channelCount(); ++ch) {
        for (std::size_t i = 0; i < mem.frames(); ++i) {
            const double expected = static_cast<double>(static_cast<float>(mem.channels[ch][i]));
            maxDiff = std::max(maxDiff, std::abs(file->channels[ch][i] - expected));
        }
    }
    REQUIRE(maxDiff <= 1e-7);
}

TEST_CASE("processBuffer honours cancellation", "[processing]") {
    const uv::AudioBuffer src = makeSine(48000.0, 1000.0, 0.5, 4800, 1);
    uv::PipelineSettings s;
    CancelAlways cancel;
    REQUIRE(uv::processBuffer(src, s, &cancel).empty());
}

TEST_CASE("computeSpectrumCurve reads a full-scale sine near 0 dB", "[processing]") {
    const uv::AudioBuffer src = makeSine(48000.0, 1000.0, 1.0, 48000, 1);
    const uv::SpectrumCurve c = uv::computeSpectrumCurve(src);
    REQUIRE(c.sampleRate == 48000.0);
    REQUIRE(!c.freqHz.empty());
    REQUIRE(c.freqHz.size() == c.magDb.size());
    REQUIRE(c.freqHz.front() > 0.0f);
    REQUIRE(std::is_sorted(c.freqHz.begin(), c.freqHz.end()));
    for (std::size_t i = 1; i < c.freqHz.size(); ++i) REQUIRE(c.freqHz[i] > c.freqHz[i - 1]);

    const auto peak = std::max_element(c.magDb.begin(), c.magDb.end());
    const std::size_t k = static_cast<std::size_t>(peak - c.magDb.begin());
    const double binWidth = 48000.0 / 8192.0;
    REQUIRE(std::abs(c.freqHz[k] - 1000.0) <= binWidth);
    REQUIRE(std::abs(*peak) <= 1.5);
}

TEST_CASE("computeSpectrumCurve covers ultrasonic range at 192 kHz", "[processing]") {
    const uv::AudioBuffer src = makeSine(192000.0, 1000.0, 0.5, 192000 / 4, 2);
    const uv::SpectrumCurve c = uv::computeSpectrumCurve(src);
    REQUIRE(!c.freqHz.empty());
    REQUIRE(c.freqHz.back() > 90000.0f);
}

TEST_CASE("computeSpectrumCurve handles empty input", "[processing]") {
    uv::AudioBuffer b;
    b.sampleRate = 44100.0;
    const uv::SpectrumCurve c = uv::computeSpectrumCurve(b);
    REQUIRE(c.freqHz.empty());
    REQUIRE(c.sampleRate == 44100.0);
}
