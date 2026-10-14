#include "uvcore/Audition.h"

#include "FftwLock.h"
#include "uvcore/Resampler.h"

#include <fftw3.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <mutex>
#include <thread>
#include <vector>

namespace uv {

namespace {

constexpr double kTwoPi = 6.283185307179586476925286766559;

double princarg(double phase) {
    return phase - kTwoPi * std::floor((phase + kTwoPi * 0.5) / kTwoPi);
}

// FFTW plans for one size. Each channel thread owns one, so execution needs no
// lock; only creation and destruction go through the shared planner mutex.
struct FftPair {
    int n = 0;
    double* time = nullptr;
    fftw_complex* freq = nullptr;
    fftw_plan fwd = nullptr;
    fftw_plan inv = nullptr;

    explicit FftPair(int size) : n(size) {
        time = fftw_alloc_real(static_cast<std::size_t>(n));
        freq = fftw_alloc_complex(static_cast<std::size_t>(n / 2 + 1));
        std::lock_guard<std::mutex> lock(detail::fftwPlanMutex());
        fwd = fftw_plan_dft_r2c_1d(n, time, freq, FFTW_ESTIMATE);
        inv = fftw_plan_dft_c2r_1d(n, freq, time, FFTW_ESTIMATE);
    }
    ~FftPair() {
        {
            std::lock_guard<std::mutex> lock(detail::fftwPlanMutex());
            if (fwd) fftw_destroy_plan(fwd);
            if (inv) fftw_destroy_plan(inv);
        }
        fftw_free(time);
        fftw_free(freq);
    }
    FftPair(const FftPair&) = delete;
    FftPair& operator=(const FftPair&) = delete;
};

// Phase-vocoder time compression by k with identity phase locking
// (Laroche-Dolson). Frames are centred on m*ha in the input and m*ha/k in the
// output, so every event at time t lands at t/k exactly.
std::vector<Sample> compressTime(const std::vector<Sample>& in, int n, int ha, int k) {
    const std::size_t len = in.size();
    const int hs = ha / k;
    const int bins = n / 2 + 1;
    const std::size_t outLen = (len + static_cast<std::size_t>(k) - 1) / static_cast<std::size_t>(k);
    const std::size_t frames = len / static_cast<std::size_t>(ha) + 2;
    const int half = n / 2;

    std::vector<double> win(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) win[i] = 0.5 - 0.5 * std::cos(kTwoPi * i / n);
    // Hann analysis times Hann synthesis overlapped at hop hs sums to 3n/(8hs).
    const double olaNorm = 1.0 / (static_cast<double>(n) * 3.0 * n / (8.0 * hs));

    // Output gets n of padding on both sides so frame edges never clip.
    std::vector<double> acc(outLen + 2 * static_cast<std::size_t>(n), 0.0);

    FftPair fft(n);
    std::vector<double> mag(bins), phase(bins), prevPhase(bins, 0.0), synPhase(bins, 0.0);
    std::vector<int> peaks;
    peaks.reserve(bins);
    std::vector<double> omegaBin(bins);
    for (int b = 0; b < bins; ++b) omegaBin[b] = kTwoPi * b / n;

    for (std::size_t m = 0; m < frames; ++m) {
        const long long centre = static_cast<long long>(m) * ha;
        for (int i = 0; i < n; ++i) {
            const long long idx = centre - half + i;
            const double x = (idx >= 0 && idx < static_cast<long long>(len)) ? in[static_cast<std::size_t>(idx)] : 0.0;
            fft.time[i] = x * win[i];
        }
        fftw_execute(fft.fwd);
        for (int b = 0; b < bins; ++b) {
            const double re = fft.freq[b][0], im = fft.freq[b][1];
            mag[b] = std::sqrt(re * re + im * im);
            phase[b] = std::atan2(im, re);
        }

        if (m == 0) {
            for (int b = 0; b < bins; ++b) synPhase[b] = phase[b];
        } else {
            peaks.clear();
            for (int b = 0; b < bins; ++b) {
                const double v = mag[b];
                if (v <= 0.0) continue;
                bool pk = true;
                for (int d = -2; d <= 2 && pk; ++d) {
                    if (d == 0) continue;
                    const int j = b + d;
                    if (j >= 0 && j < bins && mag[j] > v) pk = false;
                }
                if (pk) peaks.push_back(b);
            }
            // Advance each peak by its own instantaneous frequency, then carry
            // the bins it owns along with it at their original phase offsets.
            std::vector<double> newPhase(bins);
            for (int b : peaks) {
                const double dev = princarg(phase[b] - prevPhase[b] - omegaBin[b] * ha);
                const double omega = omegaBin[b] + dev / ha;
                newPhase[b] = synPhase[b] + omega * hs;
            }
            if (peaks.empty()) {
                for (int b = 0; b < bins; ++b) {
                    const double dev = princarg(phase[b] - prevPhase[b] - omegaBin[b] * ha);
                    newPhase[b] = synPhase[b] + (omegaBin[b] + dev / ha) * hs;
                }
            } else {
                std::size_t p = 0;
                for (int b = 0; b < bins; ++b) {
                    while (p + 1 < peaks.size() && std::abs(peaks[p + 1] - b) < std::abs(peaks[p] - b)) ++p;
                    const int pb = peaks[p];
                    if (b != pb) newPhase[b] = newPhase[pb] + (phase[b] - phase[pb]);
                }
            }
            synPhase.swap(newPhase);
        }
        prevPhase = phase;

        for (int b = 0; b < bins; ++b) {
            fft.freq[b][0] = mag[b] * std::cos(synPhase[b]);
            fft.freq[b][1] = mag[b] * std::sin(synPhase[b]);
        }
        fftw_execute(fft.inv);
        const long long outCentre = static_cast<long long>(m) * hs;
        for (int i = 0; i < n; ++i) {
            const long long idx = outCentre - half + i + n;
            if (idx >= 0 && idx < static_cast<long long>(acc.size()))
                acc[static_cast<std::size_t>(idx)] += fft.time[i] * win[i] * olaNorm;
        }
    }

    return std::vector<Sample>(acc.begin() + n, acc.begin() + n + static_cast<std::ptrdiff_t>(outLen));
}

} // namespace

AudioBuffer addedComponent(const AudioBuffer& processed, const AudioBuffer& source) {
    AudioBuffer out;
    out.sampleRate = processed.sampleRate;
    out.sourceBitDepth = processed.sourceBitDepth;
    const std::size_t len = processed.frames();
    const int chans = std::min(processed.channelCount(), source.channelCount());
    if (chans <= 0) {
        out.channels = processed.channels;
        return out;
    }

    const AudioBuffer r = (source.sampleRate == processed.sampleRate) ? source
                                                                        : resample(source, processed.sampleRate);
    out.channels.assign(static_cast<std::size_t>(chans), std::vector<Sample>(len, 0.0));
    for (int ch = 0; ch < chans; ++ch) {
        const auto& p = processed.channels[ch];
        const auto& s = r.channels[ch];
        const std::size_t common = std::min({len, p.size(), s.size()});
        auto& o = out.channels[ch];
        for (std::size_t i = 0; i < common; ++i) o[i] = p[i] - s[i];
        // Past the end of the resampled source there is nothing to cancel.
        for (std::size_t i = common; i < std::min(len, p.size()); ++i) o[i] = p[i];
    }
    return out;
}

AudioBuffer pitchShiftDown(const AudioBuffer& buffer, int octaves) {
    if (octaves <= 0 || buffer.empty() || buffer.sampleRate <= 0) return buffer;
    octaves = std::min(octaves, 3);
    const int k = 1 << octaves;
    const std::size_t len = buffer.frames();

    // 4096 at 192 kHz gives ~47 Hz bins; scale so resolution in Hz holds at any rate.
    int n = 1024;
    while (n < 4096.0 * buffer.sampleRate / 192000.0 - 0.5) n *= 2;
    const int ha = n / 8;

    AudioBuffer compressed;
    compressed.sampleRate = buffer.sampleRate / k;
    compressed.sourceBitDepth = buffer.sourceBitDepth;
    compressed.channels.resize(buffer.channels.size());

    std::vector<std::thread> workers;
    for (std::size_t ch = 0; ch < buffer.channels.size(); ++ch) {
        workers.emplace_back([&, ch] { compressed.channels[ch] = compressTime(buffer.channels[ch], n, ha, k); });
    }
    for (auto& t : workers) t.join();

    // Played at sr/k the compressed signal has the original duration with every
    // frequency divided by k; resampling restores the original rate.
    AudioBuffer out = resample(compressed, buffer.sampleRate);
    out.sampleRate = buffer.sampleRate;
    out.sourceBitDepth = buffer.sourceBitDepth;
    for (auto& c : out.channels) c.resize(len, 0.0);
    return out;
}

AudioBuffer timeExpand(const AudioBuffer& buffer, double factor) {
    AudioBuffer out = buffer;
    if (factor > 0) out.sampleRate = buffer.sampleRate / factor;
    return out;
}

double normalizePeak(AudioBuffer& buffer, double targetDbfs) {
    double peak = 0.0;
    for (const auto& c : buffer.channels)
        for (Sample x : c)
            if (std::isfinite(x)) peak = std::max(peak, std::abs(x));
    if (!(peak > 0.0)) return 0.0;
    const double gain = std::pow(10.0, targetDbfs / 20.0) / peak;
    for (auto& c : buffer.channels)
        for (Sample& x : c) x *= gain;
    return 20.0 * std::log10(gain);
}

} // namespace uv
