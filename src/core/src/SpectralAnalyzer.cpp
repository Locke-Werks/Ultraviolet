#include "uvcore/SpectralAnalyzer.h"

#include "FftwLock.h"

#include <fftw3.h>

#include <algorithm>
#include <cmath>
#include <mutex>
#include <vector>

namespace uv {

namespace {

constexpr double kPi = 3.14159265358979323846;

// Cutoff detection constants. Real music slopes downward by tens of dB across
// the band, so nothing here is relative to the spectral peak: a cutoff is a
// local cliff, judged against the source's own trend just below it.
constexpr double kSearchMinHz = 4000.0;      // encoder and anti-alias cliffs live above this
constexpr double kCliffDropDb = 25.0;        // minimum depth of a low-pass cliff
constexpr double kCliffSpanOct = 0.125;      // a cliff happens within this span...
constexpr double kCliffSpanMinHz = 1500.0;   // ...or this many Hz, whichever is larger
constexpr double kFloorToleranceDb = 10.0;   // above the cliff the level must stay this close to the floor
constexpr double kShoulderToleranceDb = 3.0; // shoulder = back within this of the trend
constexpr double kSmoothOct = 1.0 / 24.0;    // fractional-octave smoothing width
// Bins are clamped this far below the strongest bin. Pure tones leave leakage
// valleys hundreds of dB deep that would otherwise read as cliffs or dominate
// the floor median; any real floor sits well above this.
constexpr double kDynamicRangeDb = 160.0;
// A cliff whose shoulder sits this far under the strongest smoothed level is
// a step in the noise floor (dither shelf, converter skirt below an earlier
// low-pass), not the edge of program material.
constexpr double kGenuineContentDb = 100.0;

// fftw planning and plan destruction are not thread-safe, and FFTW's planner
// state is global, so this must be the same lock the synthesizer uses or two
// files analyzed and extended concurrently can plan at once.
std::mutex& planMutex() {
    return detail::fftwPlanMutex();
}

struct LineFit {
    double slope = 0.0;
    double intercept = 0.0;
    bool ok = false;
    double at(double x) const { return intercept + slope * x; }
};

// Least squares of dB against log2(f) over bins [lo, hi]. Refit twice without
// points far below the line: sparse tonal material leaves valleys between
// partials that would otherwise drag the trend down and tilt it, and the
// shoulder is defined against the level of the content, not of the gaps.
LineFit fitTrend(const std::vector<double>& db, double binHz, int lo, int hi) {
    constexpr double kRejectBelowDb = 6.0;
    LineFit fit;
    lo = std::max(lo, 1);
    hi = std::min(hi, static_cast<int>(db.size()) - 1);
    for (int pass = 0; pass < 3; ++pass) {
        double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
        int n = 0;
        for (int k = lo; k <= hi; ++k) {
            const double x = std::log2(k * binHz);
            if (fit.ok && db[k] < fit.at(x) - kRejectBelowDb) continue;
            sx += x; sy += db[k]; sxx += x * x; sxy += x * db[k];
            ++n;
        }
        if (n < 2) break;
        const double denom = n * sxx - sx * sx;
        if (std::abs(denom) < 1e-12) break;
        fit.slope = (n * sxy - sx * sy) / denom;
        fit.intercept = (sy - fit.slope * sx) / n;
        fit.ok = true;
    }
    return fit;
}

double medianOf(const std::vector<double>& v, int lo, int hi) {
    if (hi < lo) return -200.0;
    std::vector<double> tmp(v.begin() + lo, v.begin() + hi + 1);
    auto mid = tmp.begin() + tmp.size() / 2;
    std::nth_element(tmp.begin(), mid, tmp.end());
    return *mid;
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

    // Under 6 Hz per bin at any rate, so a few-hundred-Hz encoder skirt spans
    // dozens of bins and the shoulder can be placed within a percent or so.
    int fftSize = 8192;
    while (static_cast<double>(fftSize) * 48000.0 < 8192.0 * sampleRate) fftSize *= 2;
    const int numBins = fftSize / 2 + 1;
    const std::size_t hop = static_cast<std::size_t>(fftSize / 2);
    const double binHz = sampleRate / fftSize;

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
        // FFTW_ESTIMATE leaves the arrays untouched during planning, so the
        // windowed data can be filled per block afterward.
        plan = fftw_plan_dft_r2c_1d(fftSize, in, out, FFTW_ESTIMATE);
    }

    std::size_t numBlocks = 1;
    if (mono.size() > static_cast<std::size_t>(fftSize)) {
        numBlocks = (mono.size() - fftSize) / hop + 1;
    }

    // Power, not magnitude: the long-term average then reflects energy, and
    // a cliff's depth does not depend on how noisy individual blocks are.
    std::vector<double> power(numBins, 0.0);
    for (std::size_t b = 0; b < numBlocks; ++b) {
        const std::size_t start = b * hop;
        for (int n = 0; n < fftSize; ++n) {
            const std::size_t idx = start + static_cast<std::size_t>(n);
            in[n] = (idx < mono.size() ? mono[idx] : 0.0) * window[n];
        }
        fftw_execute(plan);
        for (int k = 0; k < numBins; ++k) {
            power[k] += out[k][0] * out[k][0] + out[k][1] * out[k][1];
        }
    }

    {
        std::lock_guard<std::mutex> lock(planMutex());
        fftw_destroy_plan(plan);
    }
    fftw_free(in);
    fftw_free(out);

    // Same absolute scale as computeSpectrumCurve, so noiseFloorDb reads in
    // dBFS-per-bin and is comparable with what the display shows.
    const double scale = 2.0 / windowSum;
    const double powScale = scale * scale / static_cast<double>(numBlocks);
    double peakPow = 0.0;
    for (int k = 1; k < numBins; ++k) {
        power[k] *= powScale;
        peakPow = std::max(peakPow, power[k]);
    }
    const double minPow = std::max(peakPow * std::pow(10.0, -kDynamicRangeDb / 10.0), 1e-40);
    for (int k = 0; k < numBins; ++k) power[k] = std::max(power[k], minPow);

    // Fractional-octave smoothing in the power domain. Averaging power rather
    // than dB keeps an isolated tone from being diluted into its neighbours'
    // leakage valleys. Summed directly: a prefix sum would cancel away the
    // tiny values above a cliff against the huge ones below it.
    const double halfWidth = std::pow(2.0, kSmoothOct * 0.5);
    std::vector<double> sm(numBins, 10.0 * std::log10(minPow));
    for (int k = 1; k < numBins; ++k) {
        const double f = k * binHz;
        int lo = static_cast<int>(std::ceil(f / halfWidth / binHz));
        int hi = static_cast<int>(std::floor(f * halfWidth / binHz));
        lo = std::clamp(lo, 1, k);
        hi = std::clamp(hi, k, numBins - 1);
        double acc = 0.0;
        for (int j = lo; j <= hi; ++j) acc += power[j];
        sm[k] = 10.0 * std::log10(acc / (hi - lo + 1));
    }

    double peakSm = sm[1];
    for (int k = 1; k < numBins; ++k) peakSm = std::max(peakSm, sm[k]);

    const int lastBin = numBins - 1;
    const int searchLo = std::max(2, static_cast<int>(std::ceil(kSearchMinHz / binHz)));

    // Suffix max lets the "nothing comes back above the floor" test run in O(1).
    std::vector<double> suffixMax(numBins + 1, -1e300);
    for (int k = lastBin; k >= 0; --k) suffixMax[k] = std::max(suffixMax[k + 1], sm[k]);

    auto spanBinsAt = [&](double f) {
        const double spanHz = std::max(f * (std::pow(2.0, kCliffSpanOct) - 1.0), kCliffSpanMinHz);
        return std::max(2, static_cast<int>(std::lround(spanHz / binHz)));
    };

    bool found = false;
    int shoulderBin = lastBin;
    int bottomBin = lastBin;
    LineFit trend;

    // Scan downward so the highest qualifying cliff wins: a source low-passed
    // at 16 kHz and then anti-aliased at 21 kHz has its program edge at the
    // cliff with real content on top, which the genuineness test separates.
    int t = lastBin - 1;
    while (t >= searchLo) {
        const int span = spanBinsAt(t * binHz);
        const int end = std::min(lastBin, t + span);
        int bmin = t + 1;
        for (int j = t + 1; j <= end; ++j) {
            if (sm[j] < sm[bmin]) bmin = j;
        }
        if (bmin > lastBin || sm[t] - sm[bmin] < kCliffDropDb) {
            --t;
            continue;
        }
        const double floorDb = medianOf(sm, bmin, lastBin);
        const double ceiling = std::max(floorDb + kFloorToleranceDb, sm[bmin] + kShoulderToleranceDb);
        if (bmin < lastBin && suffixMax[bmin + 1] > ceiling) {
            --t;
            continue;
        }

        // The steepest point anchors both the trend window (kept clear of the
        // skirt) and the walk back up to the shoulder.
        const int sLo = std::max(searchLo, t - span);
        int steep = t;
        double steepest = 0.0;
        for (int k = std::max(sLo, 2); k <= bmin - 2 && k + 2 <= lastBin; ++k) {
            const double d = sm[k + 2] - sm[k - 2];
            if (d < steepest) {
                steepest = d;
                steep = k;
            }
        }
        const double fSteep = steep * binHz;
        const int fitHi = static_cast<int>(std::floor(fSteep * std::pow(2.0, -kCliffSpanOct) / binHz));
        const int fitLo = static_cast<int>(std::ceil(fSteep * 0.5 / binHz));
        const LineFit fit = fitTrend(sm, binHz, fitLo, fitHi);

        int k = steep;
        if (fit.ok) {
            while (k > fitLo && sm[k] < fit.at(std::log2(k * binHz)) - kShoulderToleranceDb) --k;
        }

        if (sm[k] >= peakSm - kGenuineContentDb && sm[k] - floorDb >= kCliffDropDb) {
            found = true;
            shoulderBin = k;
            bottomBin = bmin;
            trend = fit;
            break;
        }
        t = std::min(t, k) - 1;
    }

    if (found) {
        info.cutoffHz = shoulderBin * binHz;
        info.noiseFloorDb = medianOf(sm, bottomBin, lastBin);
        info.slopeDbPerOct = trend.slope;
    } else {
        info.cutoffHz = nyquist;
        const int topLo = static_cast<int>(std::ceil(nyquist * 0.5 / binHz));
        info.noiseFloorDb = medianOf(sm, std::max(1, topLo), lastBin);
        // Stop short of Nyquist, where the smoothing window is one-sided.
        const int fitHi = static_cast<int>(std::floor(nyquist * 0.97 / binHz));
        const LineFit fit = fitTrend(sm, binHz, topLo, fitHi);
        if (fit.ok) info.slopeDbPerOct = fit.slope;
    }

    info.rolledOff = info.cutoffHz < 0.98 * nyquist;
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
