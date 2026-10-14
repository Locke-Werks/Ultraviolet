#pragma once

#include "uvcore/Types.h"

namespace uv {

// Summary of a source's high-frequency content, used to drive auto mode
// and to tell the HF synthesizer where the real content ends.
struct SpectrumInfo {
    double cutoffHz = 0.0;        // frequency above which energy collapses to the noise floor
    double slopeDbPerOct = 0.0;   // spectral slope just below the cutoff
    double noiseFloorDb = -120.0; // estimated broadband noise floor
    bool rolledOff = false;       // true if cutoff is well below Nyquist (band-limited source)
};

// Analyze a (source-rate) buffer with a windowed-average magnitude spectrum.
SpectrumInfo analyzeSpectrum(const AudioBuffer& buffer);

} // namespace uv
