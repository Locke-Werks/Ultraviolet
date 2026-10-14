#pragma once

#include "uvcore/Types.h"

#include <vector>

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

// Averaged magnitude spectrum for display. Absolute scaling (a full-scale sine
// reads ~0 dB) so curves from different buffers, including different sample
// rates, can be overlaid and compared directly.
struct SpectrumCurve {
    double sampleRate = 0.0;
    std::vector<float> freqHz; // ascending bin centre frequencies, DC excluded
    std::vector<float> magDb;  // same length
};

SpectrumCurve computeSpectrumCurve(const AudioBuffer& buffer, int fftSize = 8192);

// Time-frequency magnitude for display. Rows are log-spaced from minHz to
// maxHz (the buffer's Nyquist) so the high octaves, where this product works,
// get as many rows as the low ones. Columns are bounded so a long file stays
// a few megabytes. Same absolute dB scale as SpectrumCurve.
struct Spectrogram {
    double sampleRate = 0.0;
    double durationSeconds = 0.0;
    double minHz = 20.0;
    double maxHz = 0.0;
    int cols = 0;
    int rows = 0;
    // Row-major, rows * cols. Row 0 is minHz (the bottom of the image),
    // column 0 is the start of the file.
    std::vector<float> db;
};

Spectrogram computeSpectrogram(const AudioBuffer& buffer, int maxCols = 1600, int rows = 480);

} // namespace uv
