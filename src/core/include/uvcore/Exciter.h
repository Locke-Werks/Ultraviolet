#pragma once

#include "uvcore/Types.h"

#include <vector>

namespace uv {

struct ExciterParams {
    double amount = 0.0;   // 0 = off (exact no-op) .. 1
    double cutoffHz = 0.0; // top of the source band; harmonics are kept above it
};

// Harmonic exciter for the band above the source cutoff. The top octave of the
// source, [cutoff/2, cutoff], goes through a memoryless waveshaper, so every
// harmonic it creates lands on the same sample as the transient that caused
// it. Only what falls above the cutoff is kept. All filtering is zero-phase.
//
// Returns the signal to add, one vector per channel, or an empty vector when
// there is nothing to add. The buffer is not modified.
std::vector<std::vector<Sample>> exciterSignal(const AudioBuffer& buffer, const ExciterParams& params);

} // namespace uv
