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

} // namespace

bool runPipeline(const std::string& inputPath, const std::string& outputPath,
                 const PipelineSettings& settings, ProgressSink* progress,
                 std::string* err) {
    report(progress, 0.0);

    auto loaded = loadAudio(inputPath, err);
    if (!loaded) return false;
    AudioBuffer buf = std::move(*loaded);
    if (progress && progress->cancelled()) return false;
    report(progress, 0.1);

    const SpectrumInfo info = analyzeSpectrum(buf);
    report(progress, 0.2);

    auto synth = makeAlgorithmicHfSynthesizer();

    if (wantsSynthesis(settings, info)) {
        HfParams hp;
        hp.strength = settings.hfStrength;
        hp.ultrasonic = settings.ultrasonic;
        hp.detectedCutoffHz = info.cutoffHz;
        synth->restoreInBand(buf, hp); // at source rate
    }
    if (progress && progress->cancelled()) return false;
    report(progress, 0.45);

    if (buf.sampleRate != settings.targetRate) {
        buf = resample(buf, settings.targetRate);
    }
    report(progress, 0.75);

    if (settings.ultrasonic && wantsSynthesis(settings, info)) {
        HfParams hp;
        hp.strength = settings.hfStrength;
        hp.ultrasonic = true;
        hp.detectedCutoffHz = info.cutoffHz;
        synth->exciteUltrasonic(buf, hp); // at target rate
    }
    if (progress && progress->cancelled()) return false;
    report(progress, 0.85);

    if (settings.outputFormat == OutputFormat::Pcm24) {
        ditherTo(buf, 24);
    }
    report(progress, 0.9);

    if (!saveAudio(outputPath, buf, settings.outputFormat, err)) return false;

    report(progress, 1.0);
    return true;
}

} // namespace uv
