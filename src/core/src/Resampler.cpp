#include "uvcore/Resampler.h"

// STUB: implemented by the Resampler task (r8brain-free-src).

namespace uv {

AudioBuffer resample(const AudioBuffer& input, double targetRate) {
    AudioBuffer out = input;
    out.sampleRate = targetRate;
    return out;
}

} // namespace uv
