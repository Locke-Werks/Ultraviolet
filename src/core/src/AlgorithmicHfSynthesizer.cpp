#include "uvcore/HfSynthesizer.h"

// STUB: implemented by the HfSynthesizer task.

namespace uv {

void AlgorithmicHfSynthesizer::restoreInBand(AudioBuffer& /*buffer*/, const HfParams& /*params*/) {
    // no-op
}

void AlgorithmicHfSynthesizer::exciteUltrasonic(AudioBuffer& /*buffer*/, const HfParams& /*params*/) {
    // no-op
}

std::unique_ptr<HfSynthesizer> makeAlgorithmicHfSynthesizer() {
    return std::make_unique<AlgorithmicHfSynthesizer>();
}

} // namespace uv
