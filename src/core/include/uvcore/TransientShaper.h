#pragma once

#include "uvcore/Types.h"

#include <vector>

namespace uv {

struct TransientParams {
    double attack = 0.0;  // -1..+1, gain on the synthesized band at onsets (+1 = +12 dB)
    double sustain = 0.0; // -1..+1, gain on it between onsets (+1 = +12 dB)
};

inline bool transientShaperActive(const TransientParams& p) {
    return p.attack != 0.0 || p.sustain != 0.0;
}

// Per-frame gain from a linked onset detector: fast envelope against slow
// envelope on the full-band signal, smoothed and shifted slightly earlier so
// the gain is already up when the onset arrives. Offline, so the look-ahead
// costs nothing.
std::vector<double> transientGain(const AudioBuffer& buffer, const TransientParams& params);

// Rides only the synthesized band: buffer += (g - 1) * added. Leaves the buffer
// bit-exact when the shaper is inactive or added is empty.
void applyTransientShaper(AudioBuffer& buffer, const std::vector<std::vector<Sample>>& added,
                          const TransientParams& params);

} // namespace uv
