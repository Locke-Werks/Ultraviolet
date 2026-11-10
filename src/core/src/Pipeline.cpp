#include "uvcore/Pipeline.h"

#include "uvcore/AirShelf.h"
#include "uvcore/Dither.h"
#include "uvcore/Exciter.h"
#include "uvcore/HfSynthesizer.h"
#include "uvcore/Resampler.h"
#include "uvcore/SpectralAnalyzer.h"
#include "uvcore/TransientShaper.h"

#include <algorithm>

namespace uv {

namespace {

void report(ProgressSink* p, double f) {
    if (p) p->onProgress(f);
}

bool wantsSynthesis(const PipelineSettings& s, const SpectrumInfo& info) {
    switch (s.mode) {
        case PipelineSettings::Mode::Mastering: return false;
        case PipelineSettings::Mode::Rescue:    return true;
        case PipelineSettings::Mode::Auto:      return info.rolledOff || s.cutoffOverrideHz > 0.0;
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
                          ? resample(input, settings.targetRate, settings.filterPhase)
                          : input;
    if (isCancelled(progress)) return {};
    report(progress, 0.5);

    // Cutoff the extension and exciter both work from: detected, or the
    // user's override.
    double cutoffHz = info.cutoffHz;
    double preserveBelowHz = 0.0;
    if (settings.cutoffOverrideHz > 0.0) {
        cutoffHz = settings.cutoffOverrideHz;
        if (settings.cutoffOverrideHz < info.cutoffHz) preserveBelowHz = info.cutoffHz;
    }
    const double srcNyq = 0.5 * input.sampleRate;

    // Everything added above the source band, kept separately so the
    // transient shaper can ride it without touching the original.
    std::vector<std::vector<Sample>> added;
    const TransientParams tp{settings.transientAttack, settings.transientSustain};
    const bool shaping = transientShaperActive(tp);

    // Extension runs at the target rate so it can continue the spectrum past
    // the source Nyquist in one smooth curve instead of stopping at it.
    if (synthesize) {
        HfParams hp;
        hp.strength = settings.hfStrength;
        hp.cutoffHz = cutoffHz;
        hp.preserveBelowHz = preserveBelowHz;
        hp.sourceNyquistHz = srcNyq;
        hp.ultrasonic = settings.ultrasonic;
        hp.rolloff = settings.rolloff;
        makeAlgorithmicHfSynthesizer()->extend(buf, hp, shaping ? &added : nullptr);
    }
    if (isCancelled(progress)) return {};
    report(progress, 0.65);

    if (settings.exciterAmount > 0.0) {
        ExciterParams ep;
        ep.amount = settings.exciterAmount;
        ep.cutoffHz = cutoffHz > 0.0 ? std::min(cutoffHz, 0.97 * srcNyq) : 0.97 * srcNyq;
        auto ex = exciterSignal(buf, ep);
        if (!ex.empty()) {
            if (added.empty()) added.assign(buf.channels.size(), std::vector<Sample>(buf.frames(), 0.0));
            for (std::size_t ch = 0; ch < buf.channels.size(); ++ch) {
                for (std::size_t i = 0; i < buf.frames(); ++i) {
                    buf.channels[ch][i] += ex[ch][i];
                    added[ch][i] += ex[ch][i];
                }
            }
        }
    }
    if (isCancelled(progress)) return {};
    report(progress, 0.75);

    applyTransientShaper(buf, added, tp);
    if (isCancelled(progress)) return {};
    // After the extension, so the shelf lifts the synthesized band with the
    // rest of the top end, the way a mastering EQ after the upsampler would.
    applyAirShelf(buf, settings.airDb);
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
