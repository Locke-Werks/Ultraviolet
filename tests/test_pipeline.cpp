// End-to-end pipeline integration. Exercises the whole chain on a real file on
// disk: synthesize -> saveAudio -> runPipeline (analyze, synth, resample,
// dither, encode) -> loadAudio -> check. This is the composition test that the
// per-module tests cannot cover.
#include <catch2/catch_test_macros.hpp>

#include "uvcore/AudioFile.h"
#include "uvcore/Pipeline.h"
#include "uvcore/Types.h"

#include <cmath>
#include <filesystem>
#include <system_error>
#include <vector>

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

// Band-limited 48 kHz source: tones up to 15 kHz, so a Rescue pass has room to
// add real content in [15 kHz, 20 kHz] and Auto mode should classify it rolled off.
uv::AudioBuffer makeSource(std::size_t frames) {
    const double tones[] = {220.0, 2000.0, 7000.0, 12000.0, 15000.0};
    uv::AudioBuffer b;
    b.sampleRate = 48000.0;
    b.sourceBitDepth = 16;
    b.channels.assign(2, std::vector<uv::Sample>(frames, 0.0));
    for (std::size_t i = 0; i < frames; ++i) {
        double s = 0.0;
        for (double f : tones) {
            s += std::sin(2.0 * kPi * f * static_cast<double>(i) / 48000.0);
        }
        s *= 0.18;
        b.channels[0][i] = s;
        b.channels[1][i] = s;
    }
    return b;
}

// Progress sink that records the last fraction and that it reached the end.
struct RecordingProgress : uv::ProgressSink {
    double last = -1.0;
    bool monotonic = true;
    void onProgress(double f) override {
        if (f < last) monotonic = false;
        last = f;
    }
};

double bandEnergy(const std::vector<uv::Sample>& x, double sr, double lo, double hi) {
    const std::size_t N = x.size();
    const double df = sr / static_cast<double>(N);
    const int kLo = static_cast<int>(std::ceil(lo / df));
    const int kHi = static_cast<int>(std::floor(hi / df));
    double total = 0.0;
    for (int k = kLo; k <= kHi; ++k) {
        const double w = 2.0 * kPi * static_cast<double>(k) / static_cast<double>(N);
        const double coeff = 2.0 * std::cos(w);
        double s1 = 0.0, s2 = 0.0;
        for (std::size_t n = 0; n < N; ++n) {
            const double s0 = x[n] + coeff * s1 - s2;
            s2 = s1;
            s1 = s0;
        }
        total += s1 * s1 + s2 * s2 - coeff * s1 * s2;
    }
    return total;
}

} // namespace

TEST_CASE("Pipeline upscales 48k to 192k end to end", "[pipeline]") {
    TempFile in("uv_pipe_in.wav");
    TempFile out("uv_pipe_out.wav");

    const std::size_t frames = 48000; // 1 second
    {
        std::string err;
        REQUIRE(uv::saveAudio(in.str(), makeSource(frames), uv::OutputFormat::Pcm24, &err));
    }

    uv::PipelineSettings s;
    s.targetRate = 192000.0;
    s.outputFormat = uv::OutputFormat::Float32;
    s.mode = uv::PipelineSettings::Mode::Mastering;

    RecordingProgress prog;
    std::string err;
    REQUIRE(uv::runPipeline(in.str(), out.str(), s, &prog, &err));
    REQUIRE(prog.monotonic);
    REQUIRE(prog.last == 1.0);

    auto result = uv::loadAudio(out.str(), &err);
    REQUIRE(result.has_value());
    REQUIRE(result->sampleRate == 192000.0);
    REQUIRE(result->channelCount() == 2);

    const double expected = static_cast<double>(frames) * 192000.0 / 48000.0;
    REQUIRE(std::abs(static_cast<double>(result->frames()) - expected) <= 8.0);

    for (const auto& ch : result->channels) {
        for (double v : ch) {
            REQUIRE(std::isfinite(v));
            REQUIRE(std::abs(v) < 4.0);
        }
    }
}

TEST_CASE("Rescue mode adds HF energy the source lacked", "[pipeline]") {
    TempFile in("uv_pipe_resc_in.wav");
    TempFile outMaster("uv_pipe_master.wav");
    TempFile outRescue("uv_pipe_rescue.wav");

    const std::size_t frames = 48000;
    {
        std::string err;
        REQUIRE(uv::saveAudio(in.str(), makeSource(frames), uv::OutputFormat::Pcm24, &err));
    }

    uv::PipelineSettings base;
    base.targetRate = 192000.0;
    base.outputFormat = uv::OutputFormat::Float32;

    std::string err;

    uv::PipelineSettings master = base;
    master.mode = uv::PipelineSettings::Mode::Mastering;
    REQUIRE(uv::runPipeline(in.str(), outMaster.str(), master, nullptr, &err));

    uv::PipelineSettings rescue = base;
    rescue.mode = uv::PipelineSettings::Mode::Rescue;
    rescue.hfStrength = 0.8;
    REQUIRE(uv::runPipeline(in.str(), outRescue.str(), rescue, nullptr, &err));

    auto m = uv::loadAudio(outMaster.str(), &err);
    auto r = uv::loadAudio(outRescue.str(), &err);
    REQUIRE(m.has_value());
    REQUIRE(r.has_value());

    // Above the 15 kHz source edge, Mastering has only resampler residue while
    // Rescue has synthesized content, so Rescue must carry more band energy.
    const double mE = bandEnergy(m->channels[0], 192000.0, 15500.0, 19500.0);
    const double rE = bandEnergy(r->channels[0], 192000.0, 15500.0, 19500.0);
    REQUIRE(rE > mE * 5.0);
}
