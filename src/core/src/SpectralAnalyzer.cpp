#include "uvcore/SpectralAnalyzer.h"

#include <fftw3.h>

#include <algorithm>
#include <cmath>
#include <mutex>
#include <vector>

namespace uv {

namespace {

constexpr int kBlockSize = 4096;
constexpr int kHop = kBlockSize / 2; // 50% overlap
// Content must clear the noise floor by this margin to count as real signal.
// A Hann main lobe puts a tone's skirt bins well above this, while leakage
// valleys between tones fall back under it, so the detected edge tracks the
// highest genuine spectral content.
constexpr double kThresholdDb = 9.0;
// Content also has to stay within this many dB of the strongest in-band bin.
// For near-pure synthetic signals the true noise floor is far below any real
// sidelobe, so a floor-relative test alone catches faint skirts hundreds of
// Hz past the last tone; the peak-relative test pins the edge to the main lobe.
constexpr double kContentDropDb = 40.0;
constexpr int kPersist = 3;        // consecutive bins required to confirm content
constexpr int kSmoothRadius = 2;   // moving-average radius for the dB curve
constexpr double kPi = 3.14159265358979323846;

// fftw planning and plan destruction are not thread-safe. The app analyzes
// files concurrently, so every plan lifecycle call is serialized here.
std::mutex& planMutex() {
    static std::mutex m;
    return m;
}

double binFreq(int bin, double sampleRate) {
    return static_cast<double>(bin) * sampleRate / static_cast<double>(kBlockSize);
}

} // namespace

SpectrumInfo analyzeSpectrum(const AudioBuffer& buffer) {
    SpectrumInfo info;

    const double sampleRate = buffer.sampleRate;
    const double nyquist = sampleRate * 0.5;
    info.cutoffHz = nyquist;

    if (buffer.empty() || sampleRate <= 0.0) {
        return info;
    }

    // Downmix to mono by averaging channels.
    const std::size_t frames = buffer.frames();
    const int channels = buffer.channelCount();
    std::vector<double> mono(frames, 0.0);
    for (int ch = 0; ch < channels; ++ch) {
        const auto& src = buffer.channels[ch];
        const std::size_t n = std::min(frames, src.size());
        for (std::size_t i = 0; i < n; ++i) {
            mono[i] += src[i];
        }
    }
    if (channels > 1) {
        const double inv = 1.0 / static_cast<double>(channels);
        for (auto& s : mono) s *= inv;
    }

    const int numBins = kBlockSize / 2 + 1;

    // Precompute the Hann window.
    std::vector<double> window(kBlockSize);
    for (int n = 0; n < kBlockSize; ++n) {
        window[n] = 0.5 * (1.0 - std::cos(2.0 * kPi * n / (kBlockSize - 1)));
    }

    double* in = fftw_alloc_real(kBlockSize);
    fftw_complex* out = fftw_alloc_complex(numBins);

    fftw_plan plan = nullptr;
    {
        std::lock_guard<std::mutex> lock(planMutex());
        // FFTW_ESTIMATE leaves the input/output arrays untouched during
        // planning, so the window data can be filled per block afterward.
        plan = fftw_plan_dft_r2c_1d(kBlockSize, in, out, FFTW_ESTIMATE);
    }

    std::vector<double> magSum(numBins, 0.0);

    // Number of overlapping blocks. Short input still yields one zero-padded block.
    std::size_t numBlocks = 1;
    if (mono.size() > static_cast<std::size_t>(kBlockSize)) {
        numBlocks = (mono.size() - kBlockSize) / kHop + 1;
    }

    for (std::size_t b = 0; b < numBlocks; ++b) {
        const std::size_t start = b * kHop;
        for (int n = 0; n < kBlockSize; ++n) {
            const std::size_t idx = start + static_cast<std::size_t>(n);
            const double sample = (idx < mono.size()) ? mono[idx] : 0.0;
            in[n] = sample * window[n];
        }
        fftw_execute(plan);
        for (int k = 0; k < numBins; ++k) {
            const double re = out[k][0];
            const double im = out[k][1];
            magSum[k] += std::sqrt(re * re + im * im);
        }
    }

    {
        std::lock_guard<std::mutex> lock(planMutex());
        fftw_destroy_plan(plan);
    }
    fftw_free(in);
    fftw_free(out);

    const double invBlocks = 1.0 / static_cast<double>(numBlocks);
    std::vector<double> magDb(numBins);
    for (int k = 0; k < numBins; ++k) {
        const double mag = magSum[k] * invBlocks;
        magDb[k] = 20.0 * std::log10(mag + 1e-12);
    }

    // Smooth the dB curve to bridge single-bin dips before edge detection.
    std::vector<double> smooth(numBins);
    for (int k = 0; k < numBins; ++k) {
        double acc = 0.0;
        int count = 0;
        for (int d = -kSmoothRadius; d <= kSmoothRadius; ++d) {
            const int j = k + d;
            if (j >= 0 && j < numBins) {
                acc += magDb[j];
                ++count;
            }
        }
        smooth[k] = acc / count;
    }

    // Noise floor: median magnitude across the top octave below Nyquist. The
    // median is robust to the handful of tone bins that may sit in that range.
    std::vector<double> topOctave;
    for (int k = 1; k < numBins; ++k) {
        const double f = binFreq(k, sampleRate);
        if (f >= nyquist * 0.5 && f <= nyquist) {
            topOctave.push_back(magDb[k]);
        }
    }
    if (!topOctave.empty()) {
        std::sort(topOctave.begin(), topOctave.end());
        info.noiseFloorDb = topOctave[topOctave.size() / 2];
    }

    double peakDb = info.noiseFloorDb;
    for (int k = 1; k < numBins; ++k) {
        peakDb = std::max(peakDb, smooth[k]);
    }
    const double threshold =
        std::max(info.noiseFloorDb + kThresholdDb, peakDb - kContentDropDb);

    // Scan from Nyquist downward for the highest frequency where content stays
    // persistently above the floor. The start of the first qualifying run (its
    // highest bin) marks where real content ends.
    double cutoffHz = 0.0;
    int runCount = 0;
    int runStartBin = numBins - 1;
    for (int k = numBins - 1; k >= 1; --k) {
        if (smooth[k] > threshold) {
            if (runCount == 0) {
                runStartBin = k;
            }
            ++runCount;
            if (runCount >= kPersist) {
                cutoffHz = binFreq(runStartBin, sampleRate);
                break;
            }
        } else {
            runCount = 0;
        }
    }
    info.cutoffHz = cutoffHz;

    // Spectral slope: least-squares fit of dB vs log2(freq) across the octave
    // below the cutoff.
    if (cutoffHz > 0.0) {
        double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
        int n = 0;
        const double lo = cutoffHz * 0.5;
        for (int k = 1; k < numBins; ++k) {
            const double f = binFreq(k, sampleRate);
            if (f >= lo && f <= cutoffHz) {
                const double x = std::log2(f);
                const double y = magDb[k];
                sx += x; sy += y; sxx += x * x; sxy += x * y;
                ++n;
            }
        }
        if (n >= 2) {
            const double denom = n * sxx - sx * sx;
            if (std::abs(denom) > 1e-12) {
                info.slopeDbPerOct = (n * sxy - sx * sy) / denom;
            }
        }
    }

    info.rolledOff = cutoffHz < 0.90 * nyquist;
    return info;
}

SpectrumCurve computeSpectrumCurve(const AudioBuffer& buffer, int fftSize) {
    SpectrumCurve c;
    c.sampleRate = buffer.sampleRate;
    if (buffer.empty() || buffer.sampleRate <= 0.0 || fftSize < 2) return c;

    const std::size_t frames = buffer.frames();
    const int channels = buffer.channelCount();
    std::vector<double> mono(frames, 0.0);
    for (int ch = 0; ch < channels; ++ch) {
        const auto& src = buffer.channels[ch];
        const std::size_t n = std::min(frames, src.size());
        for (std::size_t i = 0; i < n; ++i) mono[i] += src[i];
    }
    if (channels > 1) {
        const double inv = 1.0 / static_cast<double>(channels);
        for (auto& s : mono) s *= inv;
    }

    const int numBins = fftSize / 2 + 1;
    const std::size_t hop = static_cast<std::size_t>(fftSize / 2);

    std::vector<double> window(fftSize);
    double windowSum = 0.0;
    for (int n = 0; n < fftSize; ++n) {
        window[n] = 0.5 * (1.0 - std::cos(2.0 * kPi * n / (fftSize - 1)));
        windowSum += window[n];
    }

    double* in = fftw_alloc_real(fftSize);
    fftw_complex* out = fftw_alloc_complex(numBins);
    fftw_plan plan = nullptr;
    {
        std::lock_guard<std::mutex> lock(planMutex());
        plan = fftw_plan_dft_r2c_1d(fftSize, in, out, FFTW_ESTIMATE);
    }

    std::size_t numBlocks = 1;
    if (mono.size() > static_cast<std::size_t>(fftSize)) {
        numBlocks = (mono.size() - fftSize) / hop + 1;
    }

    // Averaging power rather than magnitude keeps noisy bins from jittering
    // between redraws and matches how a level meter integrates energy.
    std::vector<double> powSum(numBins, 0.0);
    for (std::size_t b = 0; b < numBlocks; ++b) {
        const std::size_t start = b * hop;
        for (int n = 0; n < fftSize; ++n) {
            const std::size_t idx = start + static_cast<std::size_t>(n);
            in[n] = (idx < mono.size() ? mono[idx] : 0.0) * window[n];
        }
        fftw_execute(plan);
        for (int k = 0; k < numBins; ++k) {
            powSum[k] += out[k][0] * out[k][0] + out[k][1] * out[k][1];
        }
    }

    {
        std::lock_guard<std::mutex> lock(planMutex());
        fftw_destroy_plan(plan);
    }
    fftw_free(in);
    fftw_free(out);

    // 2/sum(window) undoes the one-sided split and the window's coherent gain,
    // so a full-scale sine lands at 0 dBFS regardless of fftSize.
    const double scale = 2.0 / windowSum;
    const double invBlocks = 1.0 / static_cast<double>(numBlocks);
    c.freqHz.reserve(numBins - 1);
    c.magDb.reserve(numBins - 1);
    for (int k = 1; k < numBins; ++k) {
        const double mag = std::sqrt(powSum[k] * invBlocks) * scale;
        const double db = mag > 0.0 ? 20.0 * std::log10(mag) : -200.0;
        c.freqHz.push_back(static_cast<float>(k * buffer.sampleRate / fftSize));
        c.magDb.push_back(static_cast<float>(std::max(db, -200.0)));
    }
    return c;
}

Spectrogram computeSpectrogram(const AudioBuffer& buffer, int maxCols, int rows) {
    Spectrogram s;
    s.sampleRate = buffer.sampleRate;
    if (buffer.empty() || buffer.sampleRate <= 0.0 || maxCols < 1 || rows < 2) return s;

    const double sampleRate = buffer.sampleRate;
    const std::size_t frames = buffer.frames();
    const int channels = buffer.channelCount();
    std::vector<double> mono(frames, 0.0);
    for (int ch = 0; ch < channels; ++ch) {
        const auto& src = buffer.channels[ch];
        const std::size_t n = std::min(frames, src.size());
        for (std::size_t i = 0; i < n; ++i) mono[i] += src[i];
    }
    if (channels > 1) {
        const double inv = 1.0 / static_cast<double>(channels);
        for (auto& v : mono) v *= inv;
    }

    // Scaling the FFT with the rate keeps Hz per bin constant, so a source and
    // its upsampled output show the same detail in the shared band.
    int fftSize = 4096;
    while (static_cast<double>(fftSize) * 48000.0 < 4096.0 * sampleRate) fftSize *= 2;
    const int numBins = fftSize / 2 + 1;
    const std::size_t fft = static_cast<std::size_t>(fftSize);

    std::size_t available = 1;
    if (frames > fft) available = (frames - fft) / (fft / 2) + 1;
    const int cols = static_cast<int>(std::max<std::size_t>(
        1, std::min<std::size_t>(static_cast<std::size_t>(maxCols), available)));
    std::size_t hop = 0;
    if (frames > fft && cols > 1) {
        hop = std::max<std::size_t>(1, (frames - fft) / static_cast<std::size_t>(cols - 1));
    }

    const double minHz = 20.0;
    const double maxHz = sampleRate * 0.5;
    const double binHz = sampleRate / fftSize;

    // Per-row bin span for peak-hold. Rows narrower than a bin get no span and
    // fall back to interpolation at the row's geometric centre.
    std::vector<int> rowLo(rows), rowHi(rows);
    std::vector<double> rowPos(rows);
    const double ratio = maxHz / minHz;
    for (int r = 0; r < rows; ++r) {
        const double fLo = minHz * std::pow(ratio, static_cast<double>(r) / rows);
        const double fHi = minHz * std::pow(ratio, static_cast<double>(r + 1) / rows);
        int lo = static_cast<int>(std::ceil(fLo / binHz));
        int hi = static_cast<int>(std::ceil(fHi / binHz)) - 1;
        if (r == rows - 1) hi = numBins - 1; // include Nyquist bin
        lo = std::max(lo, 1);
        hi = std::min(hi, numBins - 1);
        rowLo[r] = lo;
        rowHi[r] = hi;
        rowPos[r] = std::sqrt(fLo * fHi) / binHz;
    }

    std::vector<double> window(fftSize);
    double windowSum = 0.0;
    for (int n = 0; n < fftSize; ++n) {
        window[n] = 0.5 * (1.0 - std::cos(2.0 * kPi * n / (fftSize - 1)));
        windowSum += window[n];
    }
    const double scale = 2.0 / windowSum;
    const double powScale = scale * scale;

    double* in = fftw_alloc_real(fftSize);
    fftw_complex* out = fftw_alloc_complex(numBins);
    fftw_plan plan = nullptr;
    {
        std::lock_guard<std::mutex> lock(planMutex());
        plan = fftw_plan_dft_r2c_1d(fftSize, in, out, FFTW_ESTIMATE);
    }

    s.db.assign(static_cast<std::size_t>(rows) * cols, -200.0f);
    std::vector<double> power(numBins);
    for (int c = 0; c < cols; ++c) {
        const std::size_t start = static_cast<std::size_t>(c) * hop;
        for (int n = 0; n < fftSize; ++n) {
            const std::size_t idx = start + static_cast<std::size_t>(n);
            in[n] = (idx < frames ? mono[idx] : 0.0) * window[n];
        }
        fftw_execute(plan);
        for (int k = 0; k < numBins; ++k) {
            power[k] = (out[k][0] * out[k][0] + out[k][1] * out[k][1]) * powScale;
        }
        for (int r = 0; r < rows; ++r) {
            double p = 0.0;
            if (rowLo[r] <= rowHi[r]) {
                for (int k = rowLo[r]; k <= rowHi[r]; ++k) p = std::max(p, power[k]);
            } else {
                const double pos = std::clamp(rowPos[r], 0.0, static_cast<double>(numBins - 1));
                const int k0 = static_cast<int>(std::floor(pos));
                const int k1 = std::min(k0 + 1, numBins - 1);
                const double t = pos - k0;
                p = power[k0] * (1.0 - t) + power[k1] * t;
            }
            const double db = p > 0.0 ? 10.0 * std::log10(p) : -200.0;
            s.db[static_cast<std::size_t>(r) * cols + c] =
                static_cast<float>(std::max(db, -200.0));
        }
    }

    {
        std::lock_guard<std::mutex> lock(planMutex());
        fftw_destroy_plan(plan);
    }
    fftw_free(in);
    fftw_free(out);

    s.durationSeconds = static_cast<double>(frames) / sampleRate;
    s.minHz = minHz;
    s.maxHz = maxHz;
    s.cols = cols;
    s.rows = rows;
    return s;
}

} // namespace uv
