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

} // namespace uv
