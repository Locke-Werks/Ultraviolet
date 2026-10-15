#include "uvcore/HfSynthesizer.h"

#include "FftwLock.h"

#include <fftw3.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <mutex>
#include <random>
#include <vector>

// STFT spectral band replication at the target rate.
//
// The extension is computed as a correction spectrum C = w * (S - X), where X
// is the input, S the synthesized continuation and w a blend weight that is
// exactly zero below fc - D. The time-domain output is input + ISTFT(C), so
// everything below the blend region is untouched rather than merely
// reconstructed, and the region above fc (the source's falling skirt and the
// resampler's transition band) is replaced by S outright.

namespace uv {

namespace {

constexpr double kPi = 3.14159265358979323846;

using cplx = std::complex<double>;

// Energy share of the random-phase component in the fine structure. Pure
// replication of a single octave sounds metallic and shows comb-like repeats;
// pure noise loses the source's timing detail. A third splits the difference.
constexpr double kNoiseShare = 0.33;

// Per-sample power gain of overlap-adding frames whose content is
// uncorrelated from frame to frame, relative to a consistent STFT, is
// m4 / (R * m2^2) for a Hann window with R frames overlapping (m2, m4 the
// window's mean square and mean fourth power). Random-phase noise is such
// content; the replicated patches are not, so only the noise is boosted.
double noiseOlaCompensation(int overlap) {
    const double m2 = 3.0 / 8.0;
    const double m4 = 35.0 / 128.0;
    return std::sqrt(static_cast<double>(overlap) * m2 * m2 / m4);
}

struct FftPlans {
    fftw_plan fwd = nullptr;
    fftw_plan inv = nullptr;
    double* time = nullptr;
    fftw_complex* freq = nullptr;
    int n = 0;

    explicit FftPlans(int size) : n(size) {
        time = fftw_alloc_real(static_cast<std::size_t>(n));
        freq = fftw_alloc_complex(static_cast<std::size_t>(n / 2 + 1));
        std::lock_guard<std::mutex> lock(detail::fftwPlanMutex());
        fwd = fftw_plan_dft_r2c_1d(n, time, freq, FFTW_ESTIMATE);
        inv = fftw_plan_dft_c2r_1d(n, freq, time, FFTW_ESTIMATE);
    }
    ~FftPlans() {
        {
            std::lock_guard<std::mutex> lock(detail::fftwPlanMutex());
            if (fwd) fftw_destroy_plan(fwd);
            if (inv) fftw_destroy_plan(inv);
        }
        fftw_free(time);
        fftw_free(freq);
    }
    FftPlans(const FftPlans&) = delete;
    FftPlans& operator=(const FftPlans&) = delete;
};

// Fills plans.freq with the spectrum of frame m (start = m*hop - pad in signal
// coordinates, zeros outside the signal).
void analyzeFrame(const std::vector<double>& x, long long start, const std::vector<double>& win,
                  FftPlans& plans) {
    const int n = plans.n;
    const long long len = static_cast<long long>(x.size());
    for (int i = 0; i < n; ++i) {
        const long long idx = start + i;
        plans.time[i] = (idx >= 0 && idx < len) ? x[static_cast<std::size_t>(idx)] * win[i] : 0.0;
    }
    fftw_execute(plans.fwd);
}

// Bins at about 45 Hz: fine enough that the blend ramp spans many bins, coarse
// enough in time (about 21 ms) to follow transients through the level tracker.
int chooseFftSize(double sampleRate) {
    int n = 256;
    while (n < 65536 && sampleRate / n > 47.0) n *= 2;
    return n;
}

} // namespace

void AlgorithmicHfSynthesizer::extend(AudioBuffer& buffer, const HfParams& params) {
    const double sr = buffer.sampleRate;
    if (params.strength <= 0.0 || sr <= 0.0 || buffer.empty()) return;

    const double targetNyq = 0.5 * sr;
    const double srcNyq = params.sourceNyquistHz > 0.0 ? params.sourceNyquistHz : targetNyq;
    // The resampler's own transition sits just under the source Nyquist, so it
    // must be blended over like any encoder skirt.
    const double fc = params.cutoffHz > 0.0 ? std::min(params.cutoffHz, 0.97 * srcNyq)
                                            : 0.97 * srcNyq;
    const double fadeTop = params.ultrasonic ? std::min(48000.0, 0.90 * targetNyq)
                                             : std::min(22000.0, 0.90 * targetNyq);
    if (fc <= 0.0 || fc >= fadeTop) return;

    const int N = chooseFftSize(sr);
    const int H = N / 4;
    const int bins = N / 2 + 1;
    const double df = sr / N;

    const double D = std::max(200.0, 0.04 * fc);
    const double preserveHz = std::min(params.preserveBelowHz, 0.97 * srcNyq);
    const double Dp = std::max(200.0, 0.04 * preserveHz);
    const int kc = static_cast<int>(std::lround(fc / df));
    const int k0 = std::max(2, static_cast<int>(std::lround((fc - D) / df)));
    const int B = std::max(1, static_cast<int>(std::lround(0.5 * fc / df)));
    const int srcLo = std::max(1, k0 - B);
    const int kTop = std::min(bins - 1, static_cast<int>(std::ceil(fadeTop / df)));
    if (kc <= k0 || kTop <= kc || srcLo >= k0) return;

    // Periodic Hann for analysis and synthesis: hann^2 at hop N/4 sums to a
    // constant 1.5, so ISTFT of an unmodified STFT is exact.
    std::vector<double> win(N);
    for (int i = 0; i < N; ++i) win[i] = 0.5 - 0.5 * std::cos(2.0 * kPi * i / N);
    const double olaNorm = 1.0 / (1.5 * N); // FFTW's unnormalized c2r times the COLA sum

    const long long len = static_cast<long long>(buffer.frames());
    const long long pad = N; // frames start at -N so the first samples see full overlap
    const long long frames = (len + 2 * pad) / H + 1;
    auto frameStart = [&](long long m) { return m * H - pad; };

    FftPlans plans(N);
    const fftw_complex* F = plans.freq;

    // Level bands. L(fc) per frame is the mean bin power in [0.70, 0.97] fc,
    // carried to fc along the long-term slope so it represents the trend's
    // value at the cutoff rather than the middle of the band.
    const int measLo = std::max(1, static_cast<int>(std::lround(0.70 * fc / df)));
    const int measHi = std::max(measLo, static_cast<int>(std::lround(0.97 * fc / df)));
    const int slopeLo = std::max(1, static_cast<int>(std::lround(0.5 * fc / df)));

    // Pass 1: per-frame level near fc for every channel, plus the long-term
    // spectrum below fc from which the music's own slope is measured.
    const int chCount = buffer.channelCount();
    std::vector<std::vector<double>> level(chCount, std::vector<double>(frames, 0.0));
    std::vector<double> longTerm(bins, 0.0);
    for (int ch = 0; ch < chCount; ++ch) {
        const std::vector<double>& x = buffer.channels[ch];
        for (long long m = 0; m < frames; ++m) {
            analyzeFrame(x, frameStart(m), win, plans);
            double acc = 0.0;
            for (int k = measLo; k <= measHi; ++k) {
                const double p = F[k][0] * F[k][0] + F[k][1] * F[k][1];
                acc += p;
            }
            level[ch][m] = acc / (measHi - measLo + 1);
            for (int k = slopeLo; k <= measHi; ++k) {
                longTerm[k] += F[k][0] * F[k][0] + F[k][1] * F[k][1];
            }
        }
    }

    // Least-squares slope of dB against log2 f over [fc/2, 0.97 fc]. Bins are
    // weighted by 1/f so each octave counts equally, the way the ear and a
    // log-frequency display weigh it.
    double slope = -4.5;
    {
        double sw = 0, sx = 0, sy = 0, sxx = 0, sxy = 0;
        for (int k = slopeLo; k <= measHi; ++k) {
            if (longTerm[k] <= 0.0) continue;
            const double xo = std::log2(k * df / fc);
            const double y = 10.0 * std::log10(longTerm[k]);
            const double wgt = 1.0 / k;
            sw += wgt; sx += wgt * xo; sy += wgt * y; sxx += wgt * xo * xo; sxy += wgt * xo * y;
        }
        const double den = sw * sxx - sx * sx;
        if (sw > 0.0 && std::abs(den) > 1e-12) slope = (sw * sxy - sx * sy) / den;
        slope = std::clamp(slope, -12.0, -1.0);
    }

    // Carry the band mean (geometric centre of the band) up to fc.
    const double bandCentre = std::sqrt(measLo * df * measHi * df);
    const double toFc = std::pow(10.0, slope * std::log2(fc / bandCentre) / 10.0);

    // Fast attack, slow release: an onset raises the extension immediately so
    // it follows transients, and the 60 ms release keeps it from fluttering
    // between frames. Smoothing runs on dB, not power: a power-domain release
    // would take most of a second to fall 50 dB into a gap, while in dB the
    // level heads for the floor as fast as the source does and silence stays
    // silent.
    const double release = std::exp(-static_cast<double>(H) / (0.060 * sr));
    constexpr double kFloorDb = -300.0;
    for (auto& lv : level) {
        double s = kFloorDb;
        for (double& v : lv) {
            const double p = v * toFc > 0.0 ? std::max(kFloorDb, 10.0 * std::log10(v * toFc)) : kFloorDb;
            s = p > s ? p : release * s + (1.0 - release) * p;
            v = s <= kFloorDb + 1.0 ? 0.0 : std::pow(10.0, s / 10.0);
        }
    }

    // Static per-bin target shape relative to L(fc), in linear amplitude.
    // Below fc the target is the source's own trend so the crossfade lands on
    // the same level it leaves; above fc an extra decay that steepens with
    // frequency, then a raised-cosine fade so there is no edge at the top.
    const double gainDb = 20.0 * std::log10(std::min(params.strength, 1.0));
    const double extraPerOct = 6.0 + 24.0 * std::clamp(params.rolloff, 0.0, 1.0);
    const double fadeStart = std::max(fadeTop * std::pow(2.0, -1.0 / 3.0), fc);
    std::vector<double> shape(bins, 0.0);
    // Crossfade weights: Y = keep*X + take*S. X and S are mostly uncorrelated,
    // so a linear crossfade would sag up to 3 dB mid-ramp; cos/sin weights of a
    // raised-cosine ramp keep the summed power level through the crossover.
    // keep = 1, take = 0 below fc - D, so the correction is exactly zero there.
    std::vector<double> keep(bins, 1.0);
    std::vector<double> take(bins, 0.0);
    for (int k = k0; k <= kTop; ++k) {
        const double f = k * df;
        const double u = std::log2(f / fc);
        const double extra = u > 0.0 ? extraPerOct * u + 3.0 * u * u : 0.0;
        double g = std::pow(10.0, (gainDb + slope * u - extra) / 20.0);
        if (f >= fadeTop) {
            g = 0.0;
        } else if (f > fadeStart) {
            g *= 0.5 * (1.0 + std::cos(kPi * (f - fadeStart) / (fadeTop - fadeStart)));
        }
        shape[k] = g;
        const double fLo = fc - D;
        const double w = f >= fc ? 1.0 : (f <= fLo ? 0.0 : 0.5 - 0.5 * std::cos(kPi * (f - fLo) / D));
        keep[k] = std::cos(0.5 * kPi * w);
        take[k] = std::sin(0.5 * kPi * w);
        if (preserveHz > fc) {
            // Original stays at full level up to the real cutoff, then hands
            // over with the same power-complementary ramp.
            const double pLo = preserveHz - Dp;
            const double wp = f >= preserveHz ? 1.0 : (f <= pLo ? 0.0 : 0.5 - 0.5 * std::cos(kPi * (f - pLo) / Dp));
            keep[k] = std::cos(0.5 * kPi * wp);
            if (f >= fc) take[k] = 1.0;
        }
    }
    // Above the fade the original is also cleared, which removes only
    // resampler residue: by definition nothing above fc is on the source's trend.
    const int kEnd = bins - 1;
    for (int k = kTop + 1; k <= kEnd; ++k) {
        keep[k] = 0.0;
        take[k] = 1.0;
    }

    // Whitening windows: about 1/3 octave around each source bin, so a patch
    // keeps its partials and timing but not its own tilt, which the target
    // envelope supplies instead.
    const int srcCount = k0 - srcLo;
    std::vector<int> envLo(srcCount), envHi(srcCount);
    for (int j = 0; j < srcCount; ++j) {
        const int k = srcLo + j;
        envLo[j] = std::max(srcLo, static_cast<int>(std::floor(k * std::pow(2.0, -1.0 / 6.0))));
        envHi[j] = std::min(k0 - 1, static_cast<int>(std::ceil(k * std::pow(2.0, 1.0 / 6.0))));
    }

    // Translation for each target bin: patch p maps [k0 + (p-1)B, k0 + pB) down
    // to the clean octave [k0 - B, k0), all of it below the blend region.
    std::vector<int> shift(bins, 0);
    for (int k = k0; k <= kTop; ++k) shift[k] = ((k - k0) / B + 1) * B;

    // Unit phasor table for the noise component: deterministic and cheaper than
    // a sincos per bin per frame.
    constexpr int kPhases = 4096;
    std::vector<cplx> phasor(kPhases);
    for (int i = 0; i < kPhases; ++i) phasor[i] = std::polar(1.0, 2.0 * kPi * i / kPhases);

    const double structGain = std::sqrt(1.0 - kNoiseShare);
    const double noiseGain = std::sqrt(kNoiseShare) * noiseOlaCompensation(N / H);

    std::vector<double> srcPow(srcCount), prefix(srcCount + 1), envInv(srcCount);
    std::vector<cplx> X(bins);
    std::vector<double> corr(static_cast<std::size_t>(len), 0.0);

    for (int ch = 0; ch < chCount; ++ch) {
        std::vector<double>& x = buffer.channels[ch];
        std::fill(corr.begin(), corr.end(), 0.0);
        std::mt19937 rng(0x5EEDu + static_cast<unsigned>(ch) * 7919u);

        for (long long m = 0; m < frames; ++m) {
            const double lv = level[ch][m];
            const long long start = frameStart(m);
            analyzeFrame(x, start, win, plans);
            for (int k = 0; k < bins; ++k) X[k] = cplx(F[k][0], F[k][1]);

            prefix[0] = 0.0;
            for (int j = 0; j < srcCount; ++j) {
                srcPow[j] = std::norm(X[srcLo + j]);
                prefix[j + 1] = prefix[j] + srcPow[j];
            }
            const double framePow = prefix[srcCount] / srcCount;
            const double floorPow = std::max(framePow * 1e-6, 1e-300);
            for (int j = 0; j < srcCount; ++j) {
                const double e = (prefix[envHi[j] - srcLo + 1] - prefix[envLo[j] - srcLo]) /
                                 (envHi[j] - envLo[j] + 1);
                envInv[j] = e > floorPow ? 1.0 / std::sqrt(e) : 0.0;
            }

            const double amp = std::sqrt(lv);
            // The phase advance a translated bin needs per frame is
            // 2 pi d H m / N; with H = N/4 that is a quarter turn times d*m.
            static const cplx quarter[4] = {cplx(1, 0), cplx(0, 1), cplx(-1, 0), cplx(0, -1)};

            fftw_complex* Fo = plans.freq;
            for (int k = 0; k < k0; ++k) {
                Fo[k][0] = 0.0;
                Fo[k][1] = 0.0;
            }
            for (int k = k0; k <= kEnd; ++k) {
                cplx s(0.0, 0.0);
                if (k <= kTop && shape[k] > 0.0 && amp > 0.0) {
                    const int d = shift[k];
                    const int j = k - d - srcLo;
                    const cplx rot = quarter[static_cast<int>((static_cast<long long>(d) * m) & 3)];
                    const cplx structure = X[k - d] * envInv[j] * rot;
                    const cplx noise = phasor[rng() & (kPhases - 1)];
                    s = (structGain * structure + noiseGain * noise) * (amp * shape[k]);
                } else if (k <= kTop) {
                    (void)rng(); // keep the noise sequence aligned to bins regardless of level
                }
                const cplx c = take[k] * s + (keep[k] - 1.0) * X[k];
                Fo[k][0] = c.real();
                Fo[k][1] = c.imag();
            }
            Fo[0][1] = 0.0;
            Fo[bins - 1][1] = 0.0;
            fftw_execute(plans.inv);

            for (int i = 0; i < N; ++i) {
                const long long idx = start + i;
                if (idx >= 0 && idx < len) {
                    corr[static_cast<std::size_t>(idx)] += plans.time[i] * win[i] * olaNorm;
                }
            }
        }

        for (long long i = 0; i < len; ++i) x[static_cast<std::size_t>(i)] += corr[static_cast<std::size_t>(i)];
    }
}

std::unique_ptr<HfSynthesizer> makeAlgorithmicHfSynthesizer() {
    return std::make_unique<AlgorithmicHfSynthesizer>();
}

} // namespace uv
