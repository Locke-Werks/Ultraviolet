#pragma once

#include "uvcore/SpectralAnalyzer.h"
#include "uvcore/Types.h"

#include <memory>

namespace uv {

struct HfParams {
    double strength = 0.5;        // 0 = none, 1 = maximum synthesis
    bool ultrasonic = false;      // generate content above the original Nyquist
    double detectedCutoffHz = 0.0; // where real source content ends (from SpectrumInfo)
    double restoreTopHz = 20000.0; // top of the in-band region to fill
};

// Interface for high-frequency synthesis. Two stages, both no-ops when
// strength <= 0 so the transparent path nulls against a pure resample.
class HfSynthesizer {
public:
    virtual ~HfSynthesizer() = default;

    // In-band restoration: fill [detectedCutoffHz, restoreTopHz] at the SOURCE rate,
    // before resampling. Modifies buffer in place.
    virtual void restoreInBand(AudioBuffer& buffer, const HfParams& params) = 0;

    // Ultrasonic excitation: generate content above the original Nyquist at the
    // TARGET rate, after resampling. Modifies buffer in place.
    virtual void exciteUltrasonic(AudioBuffer& buffer, const HfParams& params) = 0;
};

// Deterministic DSP implementation (harmonic transposition + waveshaping).
class AlgorithmicHfSynthesizer : public HfSynthesizer {
public:
    void restoreInBand(AudioBuffer& buffer, const HfParams& params) override;
    void exciteUltrasonic(AudioBuffer& buffer, const HfParams& params) override;
};

std::unique_ptr<HfSynthesizer> makeAlgorithmicHfSynthesizer();

} // namespace uv
