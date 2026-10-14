#pragma once

#include "uvcore/Types.h"

#include <memory>

namespace uv {

struct HfParams {
    double strength = 0.5;        // 0 = off (exact no-op); 1 = synthesized band starts at exactly the source's level at the cutoff
    double cutoffHz = 0.0;        // top shoulder of the source's low-pass cliff: last frequency on the source's own trend
    double sourceNyquistHz = 0.0; // nothing above this survives resampling, so content above it must be synthesized
    bool ultrasonic = false;      // continue the rolloff past 20 kHz into the ultrasonic
    double rolloff = 0.5;         // 0 gentle .. 1 steep extra decay above the cutoff
};

// High-frequency extension, applied once at the target rate after resampling.
// Implementations continue the source's spectrum upward from the cutoff with no
// seam and a smooth decay. Kept model-agnostic so a learned implementation can
// sit behind the same seam.
class HfSynthesizer {
public:
    virtual ~HfSynthesizer() = default;

    // Modifies the buffer in place. strength <= 0 leaves it bit-exact.
    virtual void extend(AudioBuffer& targetRateBuffer, const HfParams& params) = 0;
};

// Deterministic DSP implementation: STFT spectral band replication shaped by a
// level-tracking decay envelope.
class AlgorithmicHfSynthesizer : public HfSynthesizer {
public:
    void extend(AudioBuffer& targetRateBuffer, const HfParams& params) override;
};

std::unique_ptr<HfSynthesizer> makeAlgorithmicHfSynthesizer();

} // namespace uv
