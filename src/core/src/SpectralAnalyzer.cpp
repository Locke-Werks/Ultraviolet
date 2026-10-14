#include "uvcore/SpectralAnalyzer.h"

// STUB: implemented by the SpectralAnalyzer task (FFTW).

namespace uv {

SpectrumInfo analyzeSpectrum(const AudioBuffer& buffer) {
    SpectrumInfo info;
    info.cutoffHz = buffer.sampleRate * 0.5;
    return info;
}

} // namespace uv
