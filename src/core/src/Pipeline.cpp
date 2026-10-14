#include "uvcore/Pipeline.h"

#include "uvcore/Dither.h"
#include "uvcore/HfSynthesizer.h"
#include "uvcore/Resampler.h"
#include "uvcore/SpectralAnalyzer.h"

namespace uv {

namespace {

void report(ProgressSink* p, double f) {
    if (p) p->onProgress(f);
}

bool wantsSynthesis(const PipelineSettings& s, const SpectrumInfo& info) {
    switch (s.mode) {
        case PipelineSettings::Mode::Mastering: return false;
        case PipelineSettings::Mode::Rescue:    return true;
        case PipelineSettings::Mode::Auto:      return info.rolledOff;
    }
    return false;
}

bool isCancelled(const ProgressSink* p) {
    return p && p->cancelled();
}

// Maps processBuffer's 0..1 progress into a sub-range so runPipeline can
// reserve room for load and save while keeping the overall report monotonic.
class ScaledSink : public ProgressSink {
public:
    ScaledSink(ProgressSink* inner, double lo, double hi) : inner_(inner), lo_(lo), hi_(hi) {}
    void onProgress(double f) override {
        if (inner_) inner_->onProgress(lo_ + (hi_ - lo_) * f);
    }
    bool cancelled() const override { return inner_ && inner_->cancelled(); }

private:
    ProgressSink* inner_;
    double lo_;
    double hi_;
};

} // namespace

AudioBuffer processBuffer(const AudioBuffer& input, const PipelineSettings& settings,
                          ProgressSink* progress) {
    report(progress, 0.0);
    if (isCancelled(progress)) return {};

    const SpectrumInfo info = analyzeSpectrum(input);
    const bool synthesize = wantsSynthesis(settings, info);
    if (isCancelled(progress)) return {};
    report(progress, 0.15);

    AudioBuffer buf = input.sampleRate != settings.targetRate
                          ? resample(input, settings.targetRate)
                          : input;
    if (isCancelled(progress)) return {};
    report(progress, 0.5);

    // Extension runs at the target rate so it can continue the spectrum past
    // the source Nyquist in one smooth curve instead of stopping at it.
    if (synthesize) {
        HfParams hp;
        hp.strength = settings.hfStrength;
        hp.cutoffHz = info.cutoffHz;
        hp.sourceNyquistHz = 0.5 * input.sampleRate;
        hp.ultrasonic = settings.ultrasonic;
        hp.rolloff = settings.rolloff;
        makeAlgorithmicHfSynthesizer()->extend(buf, hp);
    }
    if (isCancelled(progress)) return {};
    report(progress, 0.9);

    if (settings.outputFormat == OutputFormat::Pcm24) {
        ditherTo(buf, 24);
    }
    if (isCancelled(progress)) return {};
    report(progress, 1.0);
    return buf;
}

bool runPipeline(const std::string& inputPath, const std::string& outputPath,
                 const PipelineSettings& settings, ProgressSink* progress,
                 std::string* err) {
    report(progress, 0.0);

    auto loaded = loadAudio(inputPath, err);
    if (!loaded) return false;
    report(progress, 0.1);

    ScaledSink scaled(progress, 0.1, 0.9);
    AudioBuffer buf = processBuffer(*loaded, settings, &scaled);
    if (buf.empty()) {
        if (err) *err = "cancelled";
        return false;
    }

    if (!saveAudio(outputPath, buf, settings.outputFormat, err)) return false;

    report(progress, 1.0);
    return true;
}

} // namespace uv
