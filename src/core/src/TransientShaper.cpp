#include "uvcore/TransientShaper.h"

#include <algorithm>
#include <cmath>

namespace uv {

namespace {

double coef(double ms, double sr) {
    return std::exp(-1.0 / (0.001 * ms * sr));
}

// One-pole smoother on signal power with separate rise and fall times.
std::vector<double> envelope(const std::vector<double>& x, double attackMs, double releaseMs, double sr) {
    const double a = coef(attackMs, sr), r = coef(releaseMs, sr);
    std::vector<double> e(x.size());
    double s = 0.0;
    for (std::size_t i = 0; i < x.size(); ++i) {
        const double v = x[i];
        s = v > s ? a * s + (1.0 - a) * v : r * s + (1.0 - r) * v;
        e[i] = s;
    }
    return e;
}

constexpr double kRangeDb = 12.0;   // gain at attack or sustain = +-1
constexpr double kOnsetDb = 6.0;    // fast over slow by this much counts as a full onset
constexpr double kLookaheadMs = 1.0;
constexpr double kSmoothMs = 0.5;

} // namespace

std::vector<double> transientGain(const AudioBuffer& buffer, const TransientParams& params) {
    const std::size_t n = buffer.frames();
    const double sr = buffer.sampleRate;
    if (n == 0 || sr <= 0.0) return {};

    // Linked detector: the loudest channel drives every channel, so the stereo
    // image does not wander on hard-panned hits.
    std::vector<double> pw(n, 0.0);
    for (const auto& ch : buffer.channels)
        for (std::size_t i = 0; i < n; ++i) pw[i] = std::max(pw[i], ch[i] * ch[i]);

    // Power, not peaks: a peak follower holds near the crest of steady noise
    // and reads it as a permanent onset. The fast one averages enough cycles
    // to sit still on a sustained note; the slow one lags a rise by tens of ms.
    const auto fast = envelope(pw, 1.0, 1.0, sr);
    const auto slow = envelope(pw, 30.0, 60.0, sr);

    std::vector<double> gdb(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double diff = 10.0 * std::log10((fast[i] + 1e-18) / (slow[i] + 1e-18));
        const double t = std::clamp(diff / kOnsetDb, 0.0, 1.0);
        gdb[i] = kRangeDb * (params.attack * t + params.sustain * (1.0 - t));
    }

    // Shift earlier and smooth with a centered box, so the gain is up before
    // the onset and the steps do not click.
    const long long la = std::llround(0.001 * kLookaheadMs * sr);
    const long long half = std::max(1LL, std::llround(0.001 * kSmoothMs * sr / 2.0));
    std::vector<double> prefix(n + 1, 0.0);
    for (std::size_t i = 0; i < n; ++i) prefix[i + 1] = prefix[i] + gdb[i];
    std::vector<double> g(n);
    const long long N = static_cast<long long>(n);
    for (long long i = 0; i < N; ++i) {
        const long long c = i + la;
        const long long lo = std::clamp(c - half, 0LL, N);
        const long long hi = std::clamp(c + half + 1, 0LL, N);
        const double avg = hi > lo ? (prefix[hi] - prefix[lo]) / static_cast<double>(hi - lo) : gdb[N - 1];
        g[static_cast<std::size_t>(i)] = std::pow(10.0, avg / 20.0);
    }
    return g;
}

void applyTransientShaper(AudioBuffer& buffer, const std::vector<std::vector<Sample>>& added,
                          const TransientParams& params) {
    if (!transientShaperActive(params) || added.empty() || buffer.empty()) return;
    const auto g = transientGain(buffer, params);
    for (std::size_t ch = 0; ch < buffer.channels.size() && ch < added.size(); ++ch) {
        auto& x = buffer.channels[ch];
        const auto& e = added[ch];
        const std::size_t n = std::min({x.size(), e.size(), g.size()});
        for (std::size_t i = 0; i < n; ++i) x[i] += (g[i] - 1.0) * e[i];
    }
}

} // namespace uv
