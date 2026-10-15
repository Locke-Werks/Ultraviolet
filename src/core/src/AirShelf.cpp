#include "uvcore/AirShelf.h"

#include "FftwLock.h"

#include <fftw3.h>

#include <algorithm>
#include <cmath>
#include <mutex>
#include <thread>
#include <vector>

namespace uv {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kLoHz = 4000.0;
constexpr double kHiHz = 20000.0;

} // namespace

double airShelfGainDb(double hz, double topDb) {
    if (topDb <= 0.0 || hz <= kLoHz) return 0.0;
    if (hz >= kHiHz) return topDb;
    const double u = std::log2(hz / kLoHz) / std::log2(kHiHz / kLoHz);
    // Smoothstep so neither corner has a kink in the response.
    return topDb * u * u * (3.0 - 2.0 * u);
}

void applyAirShelf(AudioBuffer& buffer, double topDb) {
    if (topDb <= 0.0 || buffer.empty() || buffer.sampleRate <= 0.0) return;
    const double sr = buffer.sampleRate;

    // About 6 Hz bins: the shelf starts at 4 kHz, so this resolves it finely
    // and keeps the implied filter far shorter than one frame.
    int n = 1024;
    while (n < sr / 6.0) n *= 2;
    const int hop = n / 4;
    const int bins = n / 2 + 1;

    std::vector<double> gain(bins);
    for (int k = 0; k < bins; ++k) gain[k] = std::pow(10.0, airShelfGainDb(k * sr / n, topDb) / 20.0);
    std::vector<double> win(n);
    for (int i = 0; i < n; ++i) win[i] = 0.5 - 0.5 * std::cos(2.0 * kPi * i / n);
    // Hann analysis and synthesis at hop n/4 overlap-add to 1.5; FFTW's c2r
    // is unnormalized by n.
    const double norm = 1.0 / (1.5 * n);

    auto run = [&](std::vector<Sample>& x) {
        double* t = fftw_alloc_real(n);
        fftw_complex* f = fftw_alloc_complex(bins);
        fftw_plan fwd, inv;
        {
            std::lock_guard<std::mutex> lock(detail::fftwPlanMutex());
            fwd = fftw_plan_dft_r2c_1d(n, t, f, FFTW_ESTIMATE);
            inv = fftw_plan_dft_c2r_1d(n, f, t, FFTW_ESTIMATE);
        }
        const long long len = static_cast<long long>(x.size());
        std::vector<double> out(x.size() + 2 * static_cast<std::size_t>(n), 0.0);
        for (long long start = -n; start < len; start += hop) {
            for (int i = 0; i < n; ++i) {
                const long long idx = start + i;
                t[i] = (idx >= 0 && idx < len ? x[idx] : 0.0) * win[i];
            }
            fftw_execute(fwd);
            for (int k = 0; k < bins; ++k) {
                f[k][0] *= gain[k];
                f[k][1] *= gain[k];
            }
            fftw_execute(inv);
            for (int i = 0; i < n; ++i) out[static_cast<std::size_t>(start + i + n)] += t[i] * win[i] * norm;
        }
        std::copy(out.begin() + n, out.begin() + n + len, x.begin());
        {
            std::lock_guard<std::mutex> lock(detail::fftwPlanMutex());
            fftw_destroy_plan(fwd);
            fftw_destroy_plan(inv);
        }
        fftw_free(t);
        fftw_free(f);
    };

    std::vector<std::thread> workers;
    for (auto& ch : buffer.channels) workers.emplace_back([&run, &ch] { run(ch); });
    for (auto& w : workers) w.join();
}

} // namespace uv
