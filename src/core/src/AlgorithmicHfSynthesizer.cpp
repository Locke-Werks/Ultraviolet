#include "uvcore/HfSynthesizer.h"

#include <algorithm>
#include <cmath>
#include <vector>

// Self-contained algorithmic HF synthesizer. All filtering is time-domain
// windowed-sinc FIR, written here so the module carries no FFT dependency and
// stays decoupled from the spectral analyzer. Linear-phase FIRs keep the added
// band phase-coherent with the source and make the band-pass isolation sharp
// enough that content below the cutoff is left alone within filter leakage.

namespace uv {

namespace {

constexpr double kPi = 3.14159265358979323846;

// Normalized sinc: sin(pi x)/(pi x), with the removable singularity at 0.
double sinc(double x) {
    if (std::abs(x) < 1e-12) {
        return 1.0;
    }
    const double px = kPi * x;
    return std::sin(px) / px;
}

// Blackman window over a length-N kernel. Low side-lobes matter more than the
// slightly wider main lobe here: leakage below the cutoff is what the band-pass
// must suppress, and side-lobe level sets that floor.
double blackman(int n, int N) {
    const double t = static_cast<double>(n) / static_cast<double>(N - 1);
    return 0.42 - 0.5 * std::cos(2.0 * kPi * t) + 0.08 * std::cos(4.0 * kPi * t);
}

// Length chosen odd so the FIR has an exact integer group delay ((N-1)/2),
// which a symmetric centered convolution removes. Enough taps that the
// transition band is a few hundred Hz at 48 kHz.
constexpr int kNumTaps = 257;

// Windowed-sinc low-pass kernel. fcNorm is the cutoff in cycles/sample
// (freqHz / sampleRate), so the usable range is (0, 0.5).
std::vector<double> lowpassKernel(double fcNorm) {
    fcNorm = std::clamp(fcNorm, 1e-6, 0.5 - 1e-6);
    std::vector<double> h(kNumTaps);
    const int M = kNumTaps - 1;
    double sum = 0.0;
    for (int n = 0; n < kNumTaps; ++n) {
        const double k = n - M / 2.0;
        h[n] = 2.0 * fcNorm * sinc(2.0 * fcNorm * k) * blackman(n, kNumTaps);
        sum += h[n];
    }
    // Normalize DC gain to unity so the low-pass (and the high/band-pass derived
    // from it by subtraction) have predictable passband gain.
    for (double& v : h) {
        v /= sum;
    }
    return h;
}

// High-pass by spectral inversion of a low-pass: delta minus low-pass.
std::vector<double> highpassKernel(double fcNorm) {
    std::vector<double> h = lowpassKernel(fcNorm);
    for (double& v : h) {
        v = -v;
    }
    h[(kNumTaps - 1) / 2] += 1.0;
    return h;
}

// Band-pass as the difference of two low-passes: LP(fHigh) - LP(fLow).
std::vector<double> bandpassKernel(double fLowNorm, double fHighNorm) {
    std::vector<double> lo = lowpassKernel(fLowNorm);
    std::vector<double> hi = lowpassKernel(fHighNorm);
    for (int n = 0; n < kNumTaps; ++n) {
        hi[n] -= lo[n];
    }
    return hi;
}

// Centered convolution: linear-phase FIR with its (N-1)/2 group delay removed,
// producing an output aligned sample-for-sample with the input. Edges fall off
// where the kernel runs past the signal, which is acceptable for an added band.
std::vector<double> convolveCentered(const std::vector<double>& x,
                                     const std::vector<double>& h) {
    const int N = static_cast<int>(x.size());
    const int K = static_cast<int>(h.size());
    const int half = (K - 1) / 2;
    std::vector<double> y(N, 0.0);
    for (int i = 0; i < N; ++i) {
        double acc = 0.0;
        for (int j = 0; j < K; ++j) {
            const int idx = i + half - j;
            if (idx >= 0 && idx < N) {
                acc += h[j] * x[idx];
            }
        }
        y[i] = acc;
    }
    return y;
}

double rms(const std::vector<double>& v) {
    if (v.empty()) {
        return 0.0;
    }
    double acc = 0.0;
    for (double s : v) {
        acc += s * s;
    }
    return std::sqrt(acc / static_cast<double>(v.size()));
}

} // namespace

void AlgorithmicHfSynthesizer::restoreInBand(AudioBuffer& buffer, const HfParams& params) {
    const double sr = buffer.sampleRate;
    const double nyquist = sr * 0.5;
    const double topHz = std::min(params.restoreTopHz, 0.95 * nyquist);

    // True no-op guards: nothing added, buffer untouched bit-for-bit, so the
    // transparent path nulls against a pure resample.
    if (params.strength <= 0.0 || params.detectedCutoffHz <= 0.0 ||
        params.detectedCutoffHz >= std::min(params.restoreTopHz, nyquist) ||
        sr <= 0.0 || topHz <= params.detectedCutoffHz) {
        return;
    }

    const double cutoff = params.detectedCutoffHz;

    // Source band to excite: an octave below the cutoff. Squaring content near
    // cutoff/2 lands its second harmonic back around the cutoff, which is what
    // continues the spectral trend upward rather than inventing unrelated tones.
    const std::vector<double> hpKernel = highpassKernel((cutoff * 0.5) / sr);
    const std::vector<double> bpKernel = bandpassKernel(cutoff / sr, topHz / sr);

    // Energy reference: a band just below the cutoff. The added band is scaled
    // to this times strength so the restoration matches the source's own level.
    const double measLow = std::max(cutoff * 0.70, cutoff - 2000.0);
    const std::vector<double> measKernel = bandpassKernel(measLow / sr, cutoff / sr);

    for (std::vector<double>& ch : buffer.channels) {
        if (ch.empty()) {
            continue;
        }

        const double refRms = rms(convolveCentered(ch, measKernel));
        if (refRms <= 0.0) {
            continue; // silent below cutoff: nothing to extend
        }

        // High-pass a copy near cutoff/2, then a harmonic-generating
        // nonlinearity. Full-wave rectification minus its mean is a strong,
        // stable harmonic source with no DC offset leaking into the result.
        std::vector<double> excited = convolveCentered(ch, hpKernel);
        double mean = 0.0;
        for (double& s : excited) {
            s = std::abs(s);
            mean += s;
        }
        mean /= static_cast<double>(excited.size());
        for (double& s : excited) {
            s -= mean;
        }

        // Band-pass isolates the newly created energy to [cutoff, topHz] so the
        // add leaves everything below cutoff unchanged within filter leakage.
        std::vector<double> band = convolveCentered(excited, bpKernel);

        const double bandRms = rms(band);
        if (bandRms <= 0.0) {
            continue;
        }
        const double scale = params.strength * refRms / bandRms;

        for (std::size_t i = 0; i < ch.size(); ++i) {
            ch[i] += scale * band[i];
        }
    }
}

void AlgorithmicHfSynthesizer::exciteUltrasonic(AudioBuffer& buffer, const HfParams& params) {
    if (!params.ultrasonic || params.strength <= 0.0) {
        return;
    }
    const double sr = buffer.sampleRate;
    const double nyquist = sr * 0.5;

    const double floorHz = 24000.0;
    const double topHz = 0.9 * nyquist;
    // Only runs where there is actually ultrasonic headroom above 24 kHz.
    if (sr <= 0.0 || topHz <= floorHz) {
        return;
    }

    // Take the top of the existing band, waveshape it to throw harmonics above
    // 24 kHz, band-pass to [24k, 0.9*Nyquist] and add at a low level. Kept
    // subtle: this is air, not a second signal.
    const double srcLow = std::max(floorHz * 0.5, floorHz - 6000.0);
    const std::vector<double> srcKernel = bandpassKernel(srcLow / sr, floorHz / sr);
    const std::vector<double> outKernel = bandpassKernel(floorHz / sr, topHz / sr);

    constexpr double kUltrasonicGain = 0.15;

    for (std::vector<double>& ch : buffer.channels) {
        if (ch.empty()) {
            continue;
        }
        const double refRms = rms(convolveCentered(ch, srcKernel));
        if (refRms <= 0.0) {
            continue;
        }

        std::vector<double> excited = convolveCentered(ch, srcKernel);
        for (double& s : excited) {
            s = s * s; // squaring shifts energy up by an octave
        }

        std::vector<double> band = convolveCentered(excited, outKernel);
        const double bandRms = rms(band);
        if (bandRms <= 0.0) {
            continue;
        }
        const double scale = params.strength * kUltrasonicGain * refRms / bandRms;

        for (std::size_t i = 0; i < ch.size(); ++i) {
            ch[i] += scale * band[i];
        }
    }
}

std::unique_ptr<HfSynthesizer> makeAlgorithmicHfSynthesizer() {
    return std::make_unique<AlgorithmicHfSynthesizer>();
}

} // namespace uv
