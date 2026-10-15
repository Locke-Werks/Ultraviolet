#include "uvcore/Selection.h"

#include "FftwLock.h"

#include <fftw3.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <mutex>
#include <thread>
#include <vector>

namespace uv {

namespace {

constexpr double kPi = 3.14159265358979323846;

// Same ownership rule as in Audition.cpp: plans are created and destroyed under
// the shared planner lock, executed lock-free by the owning thread.
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

double raisedCos(double u) { return 0.5 - 0.5 * std::cos(kPi * std::clamp(u, 0.0, 1.0)); }

// Transition width at an edge: 1/12 octave, but never narrower than 20 Hz so
// low edges still get a mask several bins wide.
double edgeWidth(double hz) { return std::max(20.0, hz * (std::pow(2.0, 1.0 / 12.0) - 1.0)); }

std::vector<double> bandMask(int n, double rate, double f0, double f1) {
    const int bins = n / 2 + 1;
    const double nyq = rate * 0.5;
    const double w0 = edgeWidth(f0), w1 = edgeWidth(f1);
    std::vector<double> m(static_cast<std::size_t>(bins));
    for (int b = 0; b < bins; ++b) {
        const double f = b * rate / n;
        double g = 1.0;
        // Roll-offs sit outside the band so the passband stays flat; an edge at
        // DC or Nyquist is not an edge at all.
        if (f0 > 0.0 && f < f0) g *= raisedCos((f - (f0 - w0)) / w0);
        if (f1 < nyq && f > f1) g *= raisedCos(((f1 + w1) - f) / w1);
        m[static_cast<std::size_t>(b)] = g;
    }
    return m;
}

// Weighted overlap-add with Hann analysis and synthesis at hop n/4; the
// squared windows overlap to a constant 1.5.
std::vector<Sample> filterChannel(const std::vector<Sample>& in, int n, const std::vector<double>& mask) {
    const std::size_t len = in.size();
    const int hop = n / 4;
    const int bins = n / 2 + 1;
    std::vector<double> win(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) win[i] = 0.5 - 0.5 * std::cos(2.0 * kPi * i / n);
    const double norm = 1.0 / (static_cast<double>(n) * 1.5);

    // Frames start n before the signal so every sample is covered by a full
    // set of overlapping windows.
    std::vector<double> acc(len + 2 * static_cast<std::size_t>(n), 0.0);
    FftPair fft(n);
    for (long long start = -n; start < static_cast<long long>(len); start += hop) {
        for (int i = 0; i < n; ++i) {
            const long long idx = start + i;
            const double x = (idx >= 0 && idx < static_cast<long long>(len)) ? in[static_cast<std::size_t>(idx)] : 0.0;
            fft.time[i] = x * win[i];
        }
        fftw_execute(fft.fwd);
        for (int b = 0; b < bins; ++b) {
            fft.freq[b][0] *= mask[static_cast<std::size_t>(b)];
            fft.freq[b][1] *= mask[static_cast<std::size_t>(b)];
        }
        fftw_execute(fft.inv);
        for (int i = 0; i < n; ++i) {
            const long long idx = start + i + n;
            if (idx >= 0 && idx < static_cast<long long>(acc.size()))
                acc[static_cast<std::size_t>(idx)] += fft.time[i] * win[i] * norm;
        }
    }
    return std::vector<Sample>(acc.begin() + n, acc.begin() + n + static_cast<std::ptrdiff_t>(len));
}

} // namespace

AudioBuffer extractSelection(const AudioBuffer& in, const Selection& s) {
    AudioBuffer out;
    out.sampleRate = in.sampleRate;
    out.sourceBitDepth = in.sourceBitDepth;
    const std::size_t len = in.frames();
    if (len == 0 || in.sampleRate <= 0) {
        out.channels.assign(in.channels.size(), {});
        return out;
    }
    const double rate = in.sampleRate;

    std::size_t a = 0, b = len;
    if (s.hasTime) {
        const double lo = std::min(s.t0, s.t1), hi = std::max(s.t0, s.t1);
        a = static_cast<std::size_t>(std::clamp(std::llround(lo * rate), 0LL, static_cast<long long>(len)));
        b = static_cast<std::size_t>(std::clamp(std::llround(hi * rate), 0LL, static_cast<long long>(len)));
    }
    if (b <= a) {
        out.channels.assign(in.channels.size(), {});
        return out;
    }

    // ~12 Hz bins: 4096 at 48 kHz, scaled with the rate so resolution in Hz holds.
    int n = 1024;
    while (n < rate / 12.0) n *= 2;

    bool filter = false;
    std::vector<double> mask;
    if (s.hasFreq) {
        const double f0 = std::max(0.0, std::min(s.f0, s.f1));
        const double f1 = std::max(s.f0, s.f1);
        filter = f0 > 0.0 || f1 < rate * 0.5;
        if (filter) mask = bandMask(n, rate, f0, f1);
    }

    // Filter with n of real context either side of the cut, so the mask's time
    // smearing at the selection edges is fed by actual neighbouring audio.
    const std::size_t pa = filter ? (a > static_cast<std::size_t>(n) ? a - n : 0) : a;
    const std::size_t pb = filter ? std::min(len, b + static_cast<std::size_t>(n)) : b;

    out.channels.resize(in.channels.size());
    std::vector<std::thread> workers;
    for (std::size_t ch = 0; ch < in.channels.size(); ++ch) {
        workers.emplace_back([&, ch] {
            const auto& src = in.channels[ch];
            const std::size_t e = std::min(pb, src.size());
            std::vector<Sample> seg(src.begin() + static_cast<std::ptrdiff_t>(std::min(pa, e)),
                                    src.begin() + static_cast<std::ptrdiff_t>(e));
            seg.resize(pb - pa, 0.0);
            if (filter) seg = filterChannel(seg, n, mask);
            out.channels[ch].assign(seg.begin() + static_cast<std::ptrdiff_t>(a - pa),
                                    seg.begin() + static_cast<std::ptrdiff_t>(b - pa));
        });
    }
    for (auto& t : workers) t.join();

    // A cut lands mid-waveform; short fades keep it from clicking. Only at
    // edges that are actual cuts, not at the file's own start and end.
    if (s.hasTime) {
        const std::size_t outLen = b - a;
        const std::size_t fade = std::min(outLen / 2, static_cast<std::size_t>(0.005 * rate));
        for (auto& c : out.channels) {
            for (std::size_t i = 0; i < fade; ++i) {
                const double g = raisedCos((i + 0.5) / fade);
                if (a > 0) c[i] *= g;
                if (b < len) c[outLen - 1 - i] *= g;
            }
        }
    }
    return out;
}

} // namespace uv
